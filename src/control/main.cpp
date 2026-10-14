// Control center firmware (plain ESP32).
//   laptop --Wi-Fi (HTTP page + WebSocket) or USB (JSON lines)--> this board --ESP-NOW--> wall nodes
// The page (web/control.html) is embedded at build time (tools/embed_web.py) and served from flash.
// The JSON protocol is documented at the bottom of web/control.html. Channels and slots are 1-based there.
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "protocol.h"
#include "engine.h"
#include "control_config.h"
#include "web_gz.h"   // generated: WEB_HTML_GZ, WEB_HTML_GZ_LEN, WEB_HTML_RAW_LEN

enum : int { SRC_ALL = -2, SRC_SERIAL = -1 };   // otherwise a WebSocket client number

static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static uint8_t apMac[6];
static WebServer http(80);
static WebSocketsServer wss(WS_PORT);
static Preferences prefs;
static bool radioOk = false;
static bool serialJson = false;   // set once a '{' line arrives on USB: only then are events echoed to USB
static bool scanning = false;
static uint32_t scanStart = 0;
static uint16_t txSeq;

// ------------------------------------------------------------ helpers
static void macStr(const uint8_t* m, char* out) {
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}
static bool macParse(const char* s, uint8_t* out) {
  if (!s || strlen(s) != 17) return false;
  for (int i = 0; i < 6; i++) {
    char a = s[i * 3], b = s[i * 3 + 1];
    if (i < 5 && s[i * 3 + 2] != ':') return false;
    if (!isxdigit((uint8_t)a) || !isxdigit((uint8_t)b)) return false;
    char t[3] = {a, b, 0};
    out[i] = (uint8_t)strtoul(t, nullptr, 16);
  }
  return true;
}

static void sendTo(int src, const String& line) {
  if (src == SRC_SERIAL) Serial.println(line);
  else if (src == SRC_ALL) {
    if (serialJson) Serial.println(line);
    wss.broadcastTXT(line.c_str(), line.length());
  } else wss.sendTXT((uint8_t)src, line.c_str(), line.length());
}
static void sendDoc(int src, const JsonDocument& d) {
  String s;
  serializeJson(d, s);
  sendTo(src, s);
}
static void sendResult(int src, uint32_t id, const char* node, bool ok, const char* err = nullptr) {
  JsonDocument d;
  d["evt"] = "result";
  d["id"] = id;
  if (node) d["node"] = node;
  d["ok"] = ok;
  if (err) d["err"] = err;
  sendDoc(src, d);
}

// ------------------------------------------------------------ peers (paired nodes)
struct Peer {
  bool used;
  bool online;
  uint8_t mac[6];
  uint32_t lastHb;
  int8_t rssi;
  LfMsgHeartbeat hb;
};
static Peer peers[MAX_PEERS];

static Peer* findPeer(const uint8_t* mac) {
  for (auto& p : peers) if (p.used && !memcmp(p.mac, mac, 6)) return &p;
  return nullptr;
}
static void savePeers() {
  uint8_t blob[MAX_PEERS * 6];
  int n = 0;
  for (auto& p : peers) if (p.used) memcpy(blob + 6 * n++, p.mac, 6);
  prefs.putBytes("peers", blob, n * 6);
}
static Peer* addPeer(const uint8_t* mac) {
  if (Peer* p = findPeer(mac)) return p;
  for (auto& p : peers) if (!p.used) {
    memset(&p, 0, sizeof(p));
    p.used = true;
    memcpy(p.mac, mac, 6);
    savePeers();
    return &p;
  }
  return nullptr;
}
static void removePeer(Peer* p) {
  p->used = false;
  savePeers();
}
static void loadPeers() {
  uint8_t blob[MAX_PEERS * 6];
  size_t n = prefs.getBytes("peers", blob, sizeof(blob));
  for (size_t i = 0; i + 6 <= n; i += 6) {
    Peer* p = nullptr;
    for (auto& q : peers) if (!q.used) { p = &q; break; }
    if (!p) break;
    memset(p, 0, sizeof(*p));
    p->used = true;
    memcpy(p->mac, blob + i, 6);
  }
}

static void emitStatus(const Peer& p, int dest) {
  char ms[18];
  macStr(p.mac, ms);
  JsonDocument d;
  d["evt"] = "status";
  d["node"] = ms;
  d["online"] = p.online;
  if (p.online) {
    d["rssi"] = p.rssi;
    JsonArray lv = d["level"].to<JsonArray>();
    JsonArray as = d["assign"].to<JsonArray>();
    JsonArray crc = d["crc"].to<JsonArray>();   // per-slot program CRC16 (0 = empty): lets the UI skip re-uploads
    for (int i = 0; i < LF_MAX_CH; i++) {
      lv.add(p.hb.level[i]);
      if (p.hb.assign[i] < LF_MAX_PROGS) as.add(p.hb.assign[i] + 1); else as.add(nullptr);
    }
    for (int s = 0; s < LF_MAX_PROGS; s++) crc.add(p.hb.prog_crc[s]);
    d["playing"] = p.hb.playing_mask;
    d["n_ch"] = p.hb.n_ch;
    d["fw"] = p.hb.fw_ver;
  }
  sendDoc(dest, d);
}

// ------------------------------------------------------------ radio
struct RxItem { uint8_t mac[6]; uint8_t len; uint8_t data[250]; };
static QueueHandle_t rxQueue;

static void onRecv(const uint8_t* mac, const uint8_t* data, int len) {   // Wi-Fi task: copy only
  if (len <= 0 || len > 250) return;
  RxItem it;
  memcpy(it.mac, mac, 6);
  it.len = (uint8_t)len;
  memcpy(it.data, data, len);
  xQueueSend(rxQueue, &it, 0);
}

// Signal strength per sender, from the raw frame (ESP-NOW travels in management action frames).
struct RssiEnt { uint8_t mac[6]; int8_t rssi; };
static RssiEnt rssiTab[16];
static uint8_t rssiNext = 0;
static void onPromisc(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;
  const wifi_promiscuous_pkt_t* pk = (const wifi_promiscuous_pkt_t*)buf;
  if (pk->rx_ctrl.sig_len < 16) return;
  const uint8_t* src = pk->payload + 10;   // addr2 = transmitter
  for (auto& e : rssiTab) if (!memcmp(e.mac, src, 6)) { e.rssi = pk->rx_ctrl.rssi; return; }
  RssiEnt& e = rssiTab[rssiNext++ & 15];
  memcpy(e.mac, src, 6);
  e.rssi = pk->rx_ctrl.rssi;
}
static int8_t rssiOf(const uint8_t* mac) {
  for (auto& e : rssiTab) if (!memcmp(e.mac, mac, 6)) return e.rssi;
  return 0;
}

static void fillHeader(LfHeader& h, uint8_t type, const uint8_t* target, uint16_t seq) {
  h.magic = LF_MAGIC;
  h.ver = LF_PROTO_VER;
  h.type = type;
  memcpy(h.target, target, 6);
  h.seq = seq;
}
// Everything goes out as broadcast with the node MAC in header.target (no 20-peer limit).
static bool sendRaw(const void* d, size_t n) { return esp_now_send(BCAST, (const uint8_t*)d, n) == ESP_OK; }

// ---- commands waiting for a node ACK. One in flight per node (the node only de-duplicates the last seq).
enum Kind : uint8_t { K_NORMAL, K_UNPAIR };
struct Pending {
  bool used;
  Kind kind;
  int src;
  uint32_t id;
  uint8_t mac[6];
  uint8_t buf[240];
  uint8_t len;
  uint16_t seq;
  uint8_t tries;
  uint32_t nextTry;
  uint32_t order;
  bool isPlay;
  uint32_t t0, baseOffset;   // PLAY: a retry carries offset = baseOffset + time since first send
};
static Pending pend[MAX_PENDING];
static uint32_t orderCtr = 0;

static bool queueCmd(int src, uint32_t id, const uint8_t* mac, const void* pkt, size_t len, Kind kind = K_NORMAL,
                     bool isPlay = false, uint32_t baseOffset = 0) {
  for (auto& p : pend) if (!p.used) {
    p.used = true;
    p.kind = kind;
    p.src = src;
    p.id = id;
    memcpy(p.mac, mac, 6);
    memcpy(p.buf, pkt, len);
    p.len = (uint8_t)len;
    p.seq = ((LfHeader*)p.buf)->seq;
    p.tries = 0;
    p.nextTry = millis();
    p.order = orderCtr++;
    p.isPlay = isPlay;
    p.t0 = millis();
    p.baseOffset = baseOffset;
    return true;
  }
  return false;
}

static void finishPending(Pending& p, bool ok, const char* err) {
  char ms[18];
  macStr(p.mac, ms);
  if (p.kind == K_UNPAIR) {
    Peer* pe = findPeer(p.mac);
    if (pe && (ok || !pe->online)) {   // unreachable node: forget it anyway; it re-appears if it still has us as controller
      removePeer(pe);
      Serial.printf("[peer] %s removed\n", ms);
    }
  }
  sendResult(p.src, p.id, ms, ok, err);
  p.used = false;
}

static void processPending() {
  static uint32_t lastSend = 0;
  uint32_t now = millis();
  for (auto& p : pend)
    if (p.used && p.tries >= CMD_MAX_TRIES && (int32_t)(now - p.nextTry) >= 0) finishPending(p, false, "no_ack");
  if (now - lastSend < SEND_GAP_MS) return;
  Pending* best = nullptr;
  for (auto& p : pend) {
    if (!p.used || p.tries >= CMD_MAX_TRIES || (int32_t)(now - p.nextTry) < 0) continue;
    bool blocked = false;
    for (auto& q : pend) if (q.used && &q != &p && q.order < p.order && !memcmp(q.mac, p.mac, 6)) { blocked = true; break; }
    if (!blocked && (!best || p.order < best->order)) best = &p;
  }
  if (!best) return;
  if (best->isPlay) {   // a retry carries the time that has passed since the first send
    uint32_t off = best->baseOffset + (now - best->t0);
    if (((LfHeader*)best->buf)->type == MSG_GO) ((LfMsgGo*)best->buf)->offset_ms = off;
    else ((LfMsgPlay*)best->buf)->offset_ms = off;
  }
  sendRaw(best->buf, best->len);
  lastSend = now;
  best->tries++;
  best->nextTry = now + CMD_RETRY_MS;
}

// ---- pairing jobs: send PAIR_ACK until the node's first heartbeat shows up
struct PairJob { bool used; int src; uint32_t id; uint8_t mac[6]; uint32_t until, nextSend; };
static PairJob pairJobs[4];

static void processPairJobs() {
  uint32_t now = millis();
  for (auto& j : pairJobs) {
    if (!j.used) continue;
    char ms[18];
    macStr(j.mac, ms);
    if ((int32_t)(now - j.until) >= 0) {
      sendResult(j.src, j.id, ms, false, "no_heartbeat");   // node never confirmed: pairing window closed or out of range
      j.used = false;
    } else if ((int32_t)(now - j.nextSend) >= 0) {
      LfHeader h;
      fillHeader(h, MSG_PAIR_ACK, j.mac, ++txSeq);
      sendRaw(&h, sizeof(h));
      j.nextSend = now + 300;
    }
  }
}

static void warnVersion(const char* what, const uint8_t* mac, uint8_t ver) {
  static uint32_t last = 0;
  if (last && millis() - last < 2000) return;
  last = millis();
  char ms[18];
  macStr(mac, ms);
  Serial.printf("[rx] WARNING: %s from %s has protocol v%u, control speaks v%u. Reflash the older side.\n", what, ms, ver, LF_PROTO_VER);
}

static void handlePacket(const RxItem& it) {
  if (it.len < sizeof(LfHeader)) return;
  const LfHeader* h = (const LfHeader*)it.data;
  if (h->magic != LF_MAGIC) return;
  char ms[18];
  macStr(it.mac, ms);

  if (h->type == MSG_ACK) {
    if (it.len < sizeof(LfMsgAck)) return;
    LfMsgAck a;
    memcpy(&a, it.data, sizeof(a));
    for (auto& p : pend) {
      if (!p.used || p.seq != a.acked_seq || memcmp(p.mac, it.mac, 6)) continue;
      static const char* names[] = {nullptr, "bad_len", "bad_arg", "bad_version"};
      if (a.status == ACK_OK) finishPending(p, true, nullptr);
      else finishPending(p, false, a.status < 4 ? names[a.status] : "bad_ack");
      break;
    }
    return;
  }

  if (h->type == MSG_PAIR_REQ) {
    if (h->ver != LF_PROTO_VER) { if (scanning) warnVersion("pair request", it.mac, h->ver); return; }
    if (!scanning || it.len < sizeof(LfMsgPairReq)) return;
    LfMsgPairReq r;
    memcpy(&r, it.data, sizeof(r));
    JsonDocument d;
    d["evt"] = "found";
    d["node"] = ms;
    d["n_ch"] = r.n_ch;
    d["fw"] = r.fw_ver;
    sendDoc(SRC_ALL, d);
    return;
  }

  if (h->type == MSG_HEARTBEAT) {
    if (h->ver != LF_PROTO_VER) { warnVersion("heartbeat", it.mac, h->ver); return; }
    if (it.len < sizeof(LfMsgHeartbeat)) return;
    // A heartbeat is unicast to us, so this node has our MAC as its controller: adopt it if we lost the list.
    Peer* p = addPeer(it.mac);
    if (!p) return;
    bool was = p->online;
    memcpy(&p->hb, it.data, sizeof(LfMsgHeartbeat));
    p->lastHb = millis();
    p->online = true;
    p->rssi = rssiOf(it.mac);
    if (!was) Serial.printf("[peer] %s online\n", ms);
    emitStatus(*p, SRC_ALL);
    for (auto& j : pairJobs) if (j.used && !memcmp(j.mac, it.mac, 6)) {
      sendResult(j.src, j.id, ms, true);
      j.used = false;
    }
  }
}

static void drainRx() {
  RxItem it;
  for (int n = 0; n < 16 && xQueueReceive(rxQueue, &it, 0) == pdTRUE; n++) handlePacket(it);
}

static void checkOffline() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last < 250) return;
  last = now;
  for (auto& p : peers)
    if (p.used && p.online && now - p.lastHb > NODE_OFFLINE_MS) {
      p.online = false;
      char ms[18];
      macStr(p.mac, ms);
      Serial.printf("[peer] %s offline\n", ms);
      emitStatus(p, SRC_ALL);
    }
}

// ------------------------------------------------------------ UI commands (JSON lines)
// ch: 1..8 or "all". Returns false when invalid.
static bool chMask(JsonVariantConst v, uint8_t& mask) {
  if (v.isNull()) return false;
  if (v.is<const char*>()) {
    if (strcmp(v.as<const char*>(), "all")) return false;
    mask = LF_ALL_CH;
    return true;
  }
  if (!v.is<int>()) return false;
  int c = v.as<int>();
  if (c < 1 || c > LF_MAX_CH) return false;
  mask = 1 << (c - 1);
  return true;
}

static void handleLine(const char* line, int src) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) {
    JsonDocument e;
    e["evt"] = "error";
    e["err"] = "bad_json";
    sendDoc(src, e);
    return;
  }
  uint32_t id = doc["id"] | 0;
  const char* cmd = doc["cmd"] | "";
  const char* nodeS = doc["node"] | "";
  uint8_t mac[6];
  bool hasMac = macParse(nodeS, mac);
  auto fail = [&](const char* err) { sendResult(src, id, hasMac ? nodeS : nullptr, false, err); };
  auto ok = [&]() { sendResult(src, id, hasMac ? nodeS : nullptr, true); };

  if (!strcmp(cmd, "hello")) {
    JsonDocument d;
    char ms[18];
    macStr(apMac, ms);
    d["evt"] = "hello";
    d["id"] = id;
    d["fw"] = CTRL_FW_VERSION;
    d["proto"] = LF_PROTO_VER;
    d["mac"] = ms;
    JsonArray arr = d["peers"].to<JsonArray>();
    for (auto& p : peers) if (p.used) { macStr(p.mac, ms); arr.add(ms); }
    sendDoc(src, d);
    for (auto& p : peers) if (p.used && p.online) emitStatus(p, src);
    return;
  }
  if (!strcmp(cmd, "scan")) {
    scanning = doc["on"] | false;
    scanStart = millis();
    return ok();
  }
  if (!strcmp(cmd, "sync")) {   // best effort: no ACK, no result event
    uint8_t m;
    uint8_t target[6];
    if (!strcmp(nodeS, "all")) memcpy(target, BCAST, 6);
    else if (hasMac) memcpy(target, mac, 6);
    else return;
    if (!chMask(doc["ch"].as<JsonVariantConst>(), m) || !doc["t_ms"].is<uint32_t>()) return;
    LfMsgSync s;
    fillHeader(s.h, MSG_SYNC, target, ++txSeq);
    s.ch_mask = m;
    s.t_ms = doc["t_ms"].as<uint32_t>();
    sendRaw(&s, sizeof(s));
    return;
  }

  if (!hasMac) return fail("bad_node");

  if (!strcmp(cmd, "pair")) {
    for (auto& j : pairJobs) if (j.used && !memcmp(j.mac, mac, 6)) j.used = false;
    for (auto& j : pairJobs) if (!j.used) {
      j.used = true;
      j.src = src;
      j.id = id;
      memcpy(j.mac, mac, 6);
      j.until = millis() + PAIR_JOB_MS;
      j.nextSend = millis();
      addPeer(mac);   // keep it even if the first heartbeat is slow
      return;
    }
    return fail("busy");
  }

  Peer* peer = findPeer(mac);
  if (!peer) return (!strcmp(cmd, "unpair")) ? ok() : fail("not_paired");

  union { LfMsgSet set; LfMsgProg prog; LfMsgAssign assign; LfMsgPlay play; LfMsgGo go; LfMsgChMask chm; LfMsgIdentify ident; LfHeader h; uint8_t raw[240]; } m;
  memset(&m, 0, sizeof(m));
  uint16_t seq = ++txSeq;
  uint8_t mask;
  bool queued;

  if (!strcmp(cmd, "prog")) {
    int slot = doc["slot"] | 0;
    const char* text = doc["text"] | "";
    if (slot < 1 || slot > LF_MAX_PROGS) return fail("bad_arg: slot");
    char err[64] = "";
    LfProgram prog;
    memset(&prog, 0, sizeof(prog));
    if (!lf::parseProgram(text, prog, err, sizeof(err))) {
      String e = String("bad_program: ") + err;
      return fail(e.c_str());
    }
    lf::normalizeProgram(prog);
    fillHeader(m.prog.h, MSG_PROG, mac, seq);
    m.prog.slot = slot - 1;
    m.prog.prog = prog;
    queued = queueCmd(src, id, mac, &m, sizeof(LfMsgProg));
  } else if (!strcmp(cmd, "assign")) {
    if (!chMask(doc["ch"].as<JsonVariantConst>(), mask)) return fail("bad_arg: ch");
    int slot = LF_NO_PROGRAM;
    if (!doc["slot"].isNull()) {
      slot = doc["slot"] | 0;
      if (slot < 1 || slot > LF_MAX_PROGS) return fail("bad_arg: slot");
      slot -= 1;
    }
    fillHeader(m.assign.h, MSG_ASSIGN, mac, seq);
    m.assign.ch_mask = mask;
    m.assign.slot = slot;
    queued = queueCmd(src, id, mac, &m, sizeof(LfMsgAssign));
  } else if (!strcmp(cmd, "play")) {
    if (!chMask(doc["ch"].as<JsonVariantConst>(), mask)) return fail("bad_arg: ch");
    uint32_t base = doc["t_ms"] | 0u;
    fillHeader(m.play.h, MSG_PLAY, mac, seq);
    m.play.ch_mask = mask;
    m.play.offset_ms = base;
    queued = queueCmd(src, id, mac, &m, sizeof(LfMsgPlay), K_NORMAL, true, base);
  } else if (!strcmp(cmd, "go")) {   // cue: {bank:0|1, play:[ch..], stop:[ch..], t_ms}  (play/stop: 1-based channel lists)
    int bank = doc["bank"] | -1;
    if (bank < 0 || bank > 1) return fail("bad_arg: bank");
    uint8_t pm = 0, sm = 0;
    for (JsonVariantConst v : doc["play"].as<JsonArrayConst>()) { int c = v | 0; if (c < 1 || c > LF_MAX_CH) return fail("bad_arg: play"); pm |= 1 << (c - 1); }
    for (JsonVariantConst v : doc["stop"].as<JsonArrayConst>()) { int c = v | 0; if (c < 1 || c > LF_MAX_CH) return fail("bad_arg: stop"); sm |= 1 << (c - 1); }
    if ((pm | sm) == 0 || (pm & sm)) return fail("bad_arg: masks");
    uint32_t base = doc["t_ms"] | 0u;
    fillHeader(m.go.h, MSG_GO, mac, seq);
    m.go.bank = bank;
    m.go.play_mask = pm;
    m.go.stop_mask = sm;
    m.go.offset_ms = base;
    queued = queueCmd(src, id, mac, &m, sizeof(LfMsgGo), K_NORMAL, true, base);
  } else if (!strcmp(cmd, "stop")) {
    if (!chMask(doc["ch"].as<JsonVariantConst>(), mask)) return fail("bad_arg: ch");
    fillHeader(m.chm.h, MSG_STOP, mac, seq);
    m.chm.ch_mask = mask;
    queued = queueCmd(src, id, mac, &m, sizeof(LfMsgChMask));
  } else if (!strcmp(cmd, "set")) {
    if (!chMask(doc["ch"].as<JsonVariantConst>(), mask)) return fail("bad_arg: ch");
    int level = doc["level"] | -1;
    if (level < 0 || level > 100) return fail("bad_arg: level");
    fillHeader(m.set.h, MSG_SET, mac, seq);
    m.set.ch_mask = mask;
    m.set.level = level;
    m.set.fade_ms = doc["fade_ms"] | 0u;
    queued = queueCmd(src, id, mac, &m, sizeof(LfMsgSet));
  } else if (!strcmp(cmd, "identify")) {
    if (doc["ch"].isNull()) mask = LF_ALL_CH;
    else if (!chMask(doc["ch"].as<JsonVariantConst>(), mask)) return fail("bad_arg: ch");
    int secs = doc["seconds"] | 5;
    fillHeader(m.ident.h, MSG_IDENTIFY, mac, seq);
    m.ident.ch_mask = mask;
    m.ident.seconds = constrain(secs, 1, 255);
    queued = queueCmd(src, id, mac, &m, sizeof(LfMsgIdentify));
  } else if (!strcmp(cmd, "unpair")) {
    fillHeader(m.h, MSG_UNPAIR, mac, seq);
    queued = queueCmd(src, id, mac, &m, sizeof(LfHeader), K_UNPAIR);
  } else {
    return fail("unknown_cmd");
  }
  if (!queued) fail("busy");
}

// ------------------------------------------------------------ USB serial
static char lineBuf[LINE_MAX];
static size_t lineLen = 0;
static bool lineOver = false;

static void printStatus() {
  char ms[18];
  macStr(apMac, ms);
  Serial.printf("control fw %d  proto %d  uptime %lus  radio %s\n", CTRL_FW_VERSION, LF_PROTO_VER, millis() / 1000, radioOk ? "ok" : "FAIL");
  Serial.printf("wifi AP \"%s\" ch %d  mac %s  ip %s  stations %d  ws clients %d\n", AP_SSID, LF_WIFI_CHANNEL, ms,
                WiFi.softAPIP().toString().c_str(), WiFi.softAPgetStationNum(), wss.connectedClients());
  Serial.printf("page %u bytes (%u gzip)  scanning %s\n", (unsigned)WEB_HTML_RAW_LEN, (unsigned)WEB_HTML_GZ_LEN, scanning ? "yes" : "no");
  int n = 0;
  for (auto& p : peers) if (p.used) {
    macStr(p.mac, ms);
    n++;
    if (p.online)
      Serial.printf("node %s  ONLINE  rssi %d  fw %u  up %us  playing 0x%02X\n", ms, p.rssi, p.hb.fw_ver, (unsigned)p.hb.uptime_s, p.hb.playing_mask);
    else
      Serial.printf("node %s  offline\n", ms);
  }
  if (!n) Serial.println("no paired nodes");
}

static void handleSerialText(const char* s) {
  if (!strcmp(s, "status")) printStatus();
  else if (!strcmp(s, "scan")) { scanning = true; scanStart = millis(); Serial.println("scan on (120 s)"); }
  else if (!strcmp(s, "reboot")) ESP.restart();
  else if (!strcmp(s, "forget")) {
    for (auto& p : peers) p.used = false;
    savePeers();
    Serial.println("paired-node list cleared (nodes that still have this controller re-appear by themselves)");
  } else if (!strcmp(s, "help") || *s) Serial.println("commands: status | scan | forget | reboot   (or JSON lines from the page)");
}

static void pollSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (lineOver) {
        JsonDocument e;
        e["evt"] = "error";
        e["err"] = "line_too_long";
        sendDoc(SRC_SERIAL, e);
      } else if (lineLen) {
        lineBuf[lineLen] = 0;
        if (lineBuf[0] == '{') { serialJson = true; handleLine(lineBuf, SRC_SERIAL); }
        else handleSerialText(lineBuf);
      }
      lineLen = 0;
      lineOver = false;
    } else if (lineLen < LINE_MAX - 1) lineBuf[lineLen++] = c;
    else lineOver = true;
  }
}

// ------------------------------------------------------------ Wi-Fi: web page + WebSocket
static void onWs(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_CONNECTED) Serial.printf("[ws] client %u connected (%u total)\n", num, wss.connectedClients());
  else if (type == WStype_DISCONNECTED) Serial.printf("[ws] client %u left\n", num);
  else if (type == WStype_TEXT && length < LINE_MAX) {
    static char buf[LINE_MAX];
    memcpy(buf, payload, length);
    buf[length] = 0;
    handleLine(buf, num);
  }
}

static void webInit() {
  http.on("/", HTTP_GET, []() {
    http.sendHeader("Content-Encoding", "gzip");
    http.sendHeader("Cache-Control", "no-cache");
    http.send_P(200, "text/html; charset=utf-8", (PGM_P)WEB_HTML_GZ, WEB_HTML_GZ_LEN);
  });
  http.on("/version", HTTP_GET, []() {
    char b[96];
    snprintf(b, sizeof(b), "{\"fw\":%d,\"proto\":%d,\"page_bytes\":%u}", CTRL_FW_VERSION, LF_PROTO_VER, (unsigned)WEB_HTML_RAW_LEN);
    http.send(200, "application/json", b);
  });
  http.onNotFound([]() {   // phones/laptops probe odd URLs when joining: send them to the page
    http.sendHeader("Location", "http://192.168.4.1/");
    http.send(302, "text/plain", "");
  });
  http.begin();
  wss.begin();
  wss.onEvent(onWs);
  wss.enableHeartbeat(15000, 3000, 2);   // drop dead browser tabs
  if (MDNS.begin(MDNS_NAME)) MDNS.addService("http", "tcp", 80);
}

// ------------------------------------------------------------ setup / loop
static void radioInit() {
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  if (!WiFi.softAP(AP_SSID, AP_PASSWORD, LF_WIFI_CHANNEL, 0, AP_MAX_CLIENTS)) Serial.println("[boot] ERROR: Wi-Fi AP failed");
  WiFi.softAPmacAddress(apMac);
  rxQueue = xQueueCreate(32, sizeof(RxItem));
  if (esp_now_init() != ESP_OK) { Serial.println("[boot] ERROR: ESP-NOW init failed"); return; }
  esp_now_register_recv_cb(onRecv);
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, BCAST, 6);
  p.channel = 0;
  p.ifidx = WIFI_IF_AP;
  p.encrypt = false;
  esp_now_add_peer(&p);
  wifi_promiscuous_filter_t f = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};
  esp_wifi_set_promiscuous_filter(&f);
  esp_wifi_set_promiscuous_rx_cb(onPromisc);
  esp_wifi_set_promiscuous(true);
  radioOk = true;
}

static void ledTick() {
  uint32_t now = millis();
  uint32_t period = scanning ? 150 : 500;
  digitalWrite(STATUS_LED_PIN, (now / period) & 1);
}

void setup() {
  Serial.begin(115200);
  pinMode(STATUS_LED_PIN, OUTPUT);
  delay(200);
  Serial.println();
  Serial.printf("[boot] LED frame control center fw %d, protocol %d\n", CTRL_FW_VERSION, LF_PROTO_VER);
  prefs.begin("ctrl", false);
  loadPeers();
  txSeq = (uint16_t)esp_random();
  radioInit();
  webInit();
  char ms[18];
  macStr(apMac, ms);
  Serial.printf("[boot] Wi-Fi \"%s\" password \"%s\" -> open http://%s/   (controller MAC %s)\n", AP_SSID, AP_PASSWORD,
                WiFi.softAPIP().toString().c_str(), ms);
  Serial.println("[boot] USB: type 'help' for text commands; the page also speaks JSON lines here.");
}

void loop() {
  http.handleClient();
  wss.loop();
  pollSerial();
  drainRx();
  processPending();
  processPairJobs();
  checkOffline();
  if (scanning && millis() - scanStart > SCAN_MAX_MS) scanning = false;
  ledTick();
  delay(1);
}
