// Control TEST firmware (env control_test). A plain ESP32 that pairs with ONE node, sends it the largest real
// packets (8 x MSG_PROG with 24 steps = 231 bytes, ASSIGN, PLAY) from a built-in test load, and measures
// delivery, program CRCs, node output levels and heartbeats. Uses protocol.h and the engine as they are.
// Logs to LittleFS /txlog.csv, counters in NVS, serial commands (115200). See CLAUDE.md "Control test build".
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_task_wdt.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <limits.h>
#include "protocol.h"
#include "engine.h"
#include "testload.h"

#define FW_VERSION       1
#define STATUS_LED_PIN   2
#define BUTTON_PIN       0     // BOOT, active LOW
#define PAIR_HOLD_MS     3000
#define PAIR_WINDOW_MS   60000
#define ACK_TIMEOUT_MS   120
#define MAX_TRIES        5
#define OUTCHK_MS        60000
#define OUT_TOL_PCT      10
#define OUT_WIN_MS       60    // timing jitter allowed: reported level may match the program at any time within +-60 ms
#define HB_EXPECT_MS     500
#define HB_MAX_GAP_OK_MS 1500
#define LOG_FILE         "/txlog.csv"
#define LOG_OLD          "/txlog.old"
#define LOG_MAX_BYTES    (256 * 1024)
#define NA               INT_MIN

static_assert(sizeof(LfMsgProg) <= 250, "MSG_PROG with 24 steps must fit one ESP-NOW v1 packet (250 bytes)");

static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static uint8_t  myMac[6], nodeMac[6];
static bool     haveNode = false, radioOk = false, nvsOk = false, fsOk = false;
static uint16_t txSeq;
static bool     reliable = true;            // false = raw: send once, no retry
static LfProgram progs[LF_TEST_CHANNELS];
static uint16_t  progCrc[LF_TEST_CHANNELS];
static Preferences prefs;

// ------------------------------------------------------------ counters (persisted in NVS)
struct TypeStat { uint32_t cmds, attempts, acked, failed, first_try, rejected; };   // per command type
struct Rtt { uint32_t n, min, max; uint64_t sum; };
struct Counters {
  uint32_t layout, boots;
  TypeStat t[8];                  // index = type - MSG_SET (SET PROG ASSIGN PLAY STOP IDENTIFY UNPAIR SYNC)
  uint32_t raw_sent, raw_acked;   // commands sent in raw mode / answered
  uint32_t mac_ok, mac_fail;      // ESP-NOW send callback: MAC-layer delivery of our unicast frames
  uint32_t ack_ok, ack_bad_len, ack_bad_arg, ack_bad_ver, ack_stale;
  uint32_t hb_rx, hb_crc_checked, hb_crc_bad, hb_assign_bad;
  uint32_t rx_bad;                // too short / wrong magic / wrong version / unknown type
  uint32_t out_samples, out_strict_ok, out_tol_ok;   // cumulative output check
  Rtt rtt;
};
static Counters C;
static bool cDirty = false;

static TypeStat& ts(uint8_t type) { return C.t[(type >= MSG_SET && type <= MSG_SYNC) ? type - MSG_SET : 0]; }
static void rttAdd(Rtt& r, uint32_t us) { if (!r.n || us < r.min) r.min = us; if (us > r.max) r.max = us; r.sum += us; r.n++; }

// stress run accumulators (session only)
static struct { bool on; uint32_t cmds, acked, rejected, failed, attempts, first; Rtt rtt; uint32_t crcBad0, crcChk0; } S;

// ------------------------------------------------------------ helpers
static void macStr(const uint8_t* m, char* out) { snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]); }

// ------------------------------------------------------------ log file (/txlog.csv)
#define CSV_HEADER "ms,event,type,len,seq,mode,attempts,ack,rtt_us,total_ms,slot,ch,expected,reported,note\n"
struct Row {
  const char* event = ""; const char* type = ""; const char* mode = ""; const char* note = "";
  int len = NA, seq = NA, attempts = NA, ack = NA, rtt_us = NA, total_ms = NA, slot = NA, ch = NA, expected = NA, reported = NA;
};
static char   wbuf[4096];
static size_t wbufN = 0;
static uint32_t lastFlush = 0;

static void logFlush() {
  if (!fsOk || !wbufN) { wbufN = 0; return; }
  File f = LittleFS.open(LOG_FILE, FILE_APPEND);
  if (f) { f.write((const uint8_t*)wbuf, wbufN); f.close(); }
  wbufN = 0;
  File g = LittleFS.open(LOG_FILE, FILE_READ);
  size_t sz = g ? g.size() : 0;
  if (g) g.close();
  if (sz > LOG_MAX_BYTES) {
    LittleFS.remove(LOG_OLD);
    LittleFS.rename(LOG_FILE, LOG_OLD);
    File n = LittleFS.open(LOG_FILE, FILE_WRITE);
    if (n) { n.print(CSV_HEADER); n.close(); }
  }
  lastFlush = millis();
}

static void logRow(const Row& r) {
  char line[256], note[96];
  size_t k = 0;
  for (const char* s = r.note; *s && k < sizeof note - 1; s++) note[k++] = (*s == ',' || *s == '\n' || *s == '\r') ? ';' : *s;
  note[k] = 0;
  char f[10][12];
  const int v[10] = {r.len, r.seq, r.attempts, r.ack, r.rtt_us, r.total_ms, r.slot, r.ch, r.expected, r.reported};
  for (int i = 0; i < 10; i++) { if (v[i] == NA) f[i][0] = 0; else snprintf(f[i], sizeof f[i], "%d", v[i]); }
  int n = snprintf(line, sizeof line, "%lu,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n", (unsigned long)millis(), r.event, r.type,
                   f[0], f[1], r.mode, f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], note);
  if (n <= 0) return;
  if (n >= (int)sizeof line) n = sizeof line - 1;
  if (wbufN + n > sizeof wbuf) logFlush();
  memcpy(wbuf + wbufN, line, n);
  wbufN += n;
}
static void logNote(const char* event, const char* note) { Row r; r.event = event; r.note = note; logRow(r); logFlush(); }

static void persist() { if (nvsOk) prefs.putBytes("c", &C, sizeof C); cDirty = false; }

// ------------------------------------------------------------ rx queue (ESP-NOW callbacks run in the WiFi task)
struct RxItem { uint8_t mac[6]; uint8_t len; uint32_t us; uint8_t data[250]; };
static QueueHandle_t rxQueue;
static volatile uint32_t rxDrops = 0;

static void onRecv(const uint8_t* mac, const uint8_t* data, int len) {
  if (len <= 0 || len > 250) return;
  RxItem it;
  memcpy(it.mac, mac, 6);
  it.len = (uint8_t)len;
  it.us = micros();
  memcpy(it.data, data, len);
  if (xQueueSend(rxQueue, &it, 0) != pdTRUE) rxDrops++;
}
static void onSent(const uint8_t* mac, esp_now_send_status_t s) {
  if (!haveNode || memcmp(mac, nodeMac, 6)) return;
  if (s == ESP_NOW_SEND_SUCCESS) C.mac_ok++; else C.mac_fail++;
}

// RSSI of the node's frames (promiscuous action frames; approximate)
static volatile int rssiLast = 0; static volatile int32_t rssiSum = 0; static volatile uint32_t rssiN = 0; static volatile int rssiMin = 0;
static void onPromisc(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT || !haveNode) return;
  const wifi_promiscuous_pkt_t* p = (const wifi_promiscuous_pkt_t*)buf;
  if (p->rx_ctrl.sig_len < 16 || p->payload[0] != 0xD0 || memcmp(p->payload + 10, nodeMac, 6)) return;
  int r = p->rx_ctrl.rssi;
  rssiLast = r; rssiSum += r;
  if (!rssiN || r < rssiMin) rssiMin = r;
  rssiN++;
}

static void fillHeader(LfHeader& h, uint8_t type, const uint8_t* target, uint16_t seq) {
  h.magic = LF_MAGIC; h.ver = LF_PROTO_VER; h.type = type;
  memcpy(h.target, target, 6);
  h.seq = seq;
}

static void addPeer(const uint8_t* mac) {
  if (esp_now_is_peer_exist(mac)) return;
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, mac, 6);
  p.channel = 0; p.ifidx = WIFI_IF_STA; p.encrypt = false;
  esp_now_add_peer(&p);
}

// ------------------------------------------------------------ checks: heartbeat, program CRC, output
static LfMsgHeartbeat lastHb;
static uint32_t lastHbMs = 0;
static bool     haveHb = false;
static uint32_t hbFirstMs = 0, hbCount = 0, hbMaxGap = 0;   // session stats (heartbeat rate and longest gap)

static bool     loaded = false;     // the test load was sent at least once: CRC / assignment checks make sense
static bool     chkArmed = false;
static uint32_t playT0 = 0;
static struct { uint32_t samples, strict, tol; uint32_t chN[8], chTol[8]; bool done; uint32_t badRows; } O;   // last 60 s window

static void outChkFinish() {
  if (!chkArmed) return;
  chkArmed = false; O.done = true;
  char note[96];
  snprintf(note, sizeof note, "output check: %lu samples, strict %lu, tolerant %lu (%.1f%%)", (unsigned long)O.samples, (unsigned long)O.strict,
           (unsigned long)O.tol, O.samples ? 100.0 * O.tol / O.samples : 0.0);
  logNote("OUTCHK", note);
  Serial.printf("[check] %s\n", note);
}
static void outChkArm(uint32_t t0) {
  memset(&O, 0, sizeof O);
  playT0 = t0; chkArmed = true;
}

static void onHeartbeat(const RxItem& it) {
  uint32_t now = millis();
  LfMsgHeartbeat hb; memcpy(&hb, it.data, sizeof hb);
  C.hb_rx++; cDirty = true;
  if (haveHb) {
    uint32_t gap = now - lastHbMs;
    if (gap > hbMaxGap) hbMaxGap = gap;
    if (gap > HB_MAX_GAP_OK_MS) { Row r; r.event = "HB_GAP"; r.total_ms = gap; r.note = "long gap between heartbeats"; logRow(r); }
  } else hbFirstMs = now;
  hbCount++;
  lastHb = hb; lastHbMs = now; haveHb = true;

  // program CRC and assignment (only meaningful once we sent the test load)
  if (loaded) {
    for (int s = 0; s < LF_TEST_CHANNELS; s++) {
      C.hb_crc_checked++;
      if (hb.prog_crc[s] != progCrc[s]) {
        C.hb_crc_bad++;
        Row r; r.event = "CRC_BAD"; r.slot = s + 1; r.expected = progCrc[s]; r.reported = hb.prog_crc[s]; r.note = "heartbeat CRC differs (decimal)"; logRow(r);
      }
    }
  }

  // output check against the program the control would play
  if (chkArmed) {
    uint32_t t = now - playT0;
    if (t > OUTCHK_MS) { outChkFinish(); return; }
    if (t < 50) return;   // heartbeat may have been built before the node applied PLAY
    for (int ch = 0; ch < LF_TEST_CHANNELS; ch++) {
      int rep = hb.level[ch];
      bool playing = (hb.playing_mask >> ch) & 1;
      int expected = (int)lroundf(lf::testLevelAt(progs[ch], t) * 100);
      bool strict = playing && abs(rep - expected) <= OUT_TOL_PCT, tol = strict;
      if (playing && !strict)   // tolerant: reported level matches the program at some moment within +-OUT_WIN_MS
        for (int dt = -OUT_WIN_MS; dt <= OUT_WIN_MS && !tol; dt += 5) {
          int32_t tt = (int32_t)t + dt;
          if (tt < 0) continue;
          if (abs(rep - (int)lroundf(lf::testLevelAt(progs[ch], (uint32_t)tt) * 100)) <= OUT_TOL_PCT) tol = true;
        }
      O.samples++; O.chN[ch]++; C.out_samples++;
      if (strict) { O.strict++; C.out_strict_ok++; }
      if (tol) { O.tol++; O.chTol[ch]++; C.out_tol_ok++; }
      else if (O.badRows++ < 200) {
        Row r; r.event = "OUT_BAD"; r.ch = ch + 1; r.expected = expected; r.reported = rep; r.total_ms = (int)t;
        r.note = playing ? "level outside tolerance" : "channel not playing"; logRow(r);
      }
    }
  }
  if (loaded) {
    bool bad = false;
    for (int c = 0; c < LF_TEST_CHANNELS; c++) bad |= hb.assign[c] != c;
    if (bad) C.hb_assign_bad++;
  }
}

// ------------------------------------------------------------ pairing
static bool pairing = false;
static uint32_t pairStart = 0;
static bool pairConfirmPending = false;

static void startPairing() {
  pairing = true; pairStart = millis();
  Serial.println("[pair] window open (60 s). Hold BOOT 3 s on the node now.");
  logNote("PAIR", "window open 60 s");
}

static void onPairReq(const RxItem& it) {
  if (!pairing || it.len < sizeof(LfMsgPairReq)) return;
  const LfMsgPairReq* q = (const LfMsgPairReq*)it.data;
  char ms[18]; macStr(it.mac, ms);
  char note[96];
  snprintf(note, sizeof note, "PAIR_REQ from %s fw %u n_ch %u", ms, q->fw_ver, q->n_ch);
  logNote("PAIR", note);
  Serial.printf("[pair] %s\n", note);
  if (!haveNode || memcmp(nodeMac, it.mac, 6)) {
    memcpy(nodeMac, it.mac, 6); haveNode = true;
    if (nvsOk) prefs.putBytes("node", nodeMac, 6);
  }
  addPeer(nodeMac);
  LfHeader h; fillHeader(h, MSG_PAIR_ACK, nodeMac, ++txSeq);
  esp_now_send(nodeMac, (uint8_t*)&h, sizeof h);
  pairConfirmPending = true;
  snprintf(note, sizeof note, "PAIR_ACK sent to %s, node MAC stored", ms);
  logNote("PAIR", note);
}

// ------------------------------------------------------------ rx processing
static volatile bool ackWait = false;
static volatile uint16_t ackWaitSeq = 0;
static volatile bool ackGot = false;
static volatile uint8_t ackStatus = 0;
static volatile uint32_t ackUs = 0;

static void processRx(const RxItem& it) {
  if (it.len < sizeof(LfHeader)) { C.rx_bad++; return; }
  const LfHeader* h = (const LfHeader*)it.data;
  if (h->magic != LF_MAGIC) { C.rx_bad++; return; }
  if (h->type == MSG_PAIR_REQ) { if (h->ver == LF_PROTO_VER) onPairReq(it); else C.rx_bad++; return; }
  if (!haveNode || memcmp(it.mac, nodeMac, 6)) return;   // only my node
  if (h->ver != LF_PROTO_VER) { C.rx_bad++; return; }
  if (h->type == MSG_HEARTBEAT && it.len >= sizeof(LfMsgHeartbeat)) {
    if (pairConfirmPending) {
      pairConfirmPending = false; pairing = false;
      logNote("PAIR", "first heartbeat received: pairing confirmed");
      Serial.println("[pair] confirmed: node is sending heartbeats");
    }
    onHeartbeat(it);
  } else if (h->type == MSG_ACK && it.len >= sizeof(LfMsgAck)) {
    const LfMsgAck* a = (const LfMsgAck*)it.data;
    if (ackWait && !ackGot && a->acked_seq == ackWaitSeq) { ackStatus = a->status; ackUs = it.us; ackGot = true; }
    else C.ack_stale++;
    cDirty = true;
  } else C.rx_bad++;
}

static void pump() {   // call while waiting: handles everything the radio delivered
  esp_task_wdt_reset();
  RxItem it;
  while (xQueueReceive(rxQueue, &it, 0) == pdTRUE) processRx(it);
}
static void waitMs(uint32_t ms) { uint32_t t = millis(); while (millis() - t < ms) { pump(); delay(1); } }

// ------------------------------------------------------------ sending
struct SendResult { bool acked; int status; int attempts; uint32_t rtt_us; uint32_t total_ms; };

// buf holds a complete packet with header filled (seq already assigned). PLAY packets get offset_ms = now - playT0 on every retry.
static SendResult sendCmd(uint8_t* buf, size_t len) {
  LfHeader* h = (LfHeader*)buf;
  SendResult res = {false, -1, 0, 0, 0};
  TypeStat& st = ts(h->type);
  uint32_t starts[MAX_TRIES] = {0};
  uint32_t first = millis();
  int maxTry = reliable ? MAX_TRIES : 1;
  st.cmds++; cDirty = true;
  if (!reliable) C.raw_sent++;
  if (S.on) S.cmds++;
  if (h->type == MSG_PLAY) playT0 = first;
  if (h->type == MSG_PLAY || h->type == MSG_SET || h->type == MSG_STOP || h->type == MSG_PROG || h->type == MSG_ASSIGN) chkArmed = false;   // the old window no longer describes the node
  ackWaitSeq = h->seq; ackGot = false; ackWait = true;
  for (int n = 0; n < maxTry && !ackGot; n++) {
    if (h->type == MSG_PLAY) ((LfMsgPlay*)buf)->offset_ms = millis() - playT0;
    starts[n] = micros();
    esp_err_t e = esp_now_send(nodeMac, buf, len);
    res.attempts++; st.attempts++;
    if (S.on) S.attempts++;
    if (e != ESP_OK) { Row r; r.event = "TX_ERR"; r.seq = h->seq; r.note = "esp_now_send error"; logRow(r); }
    uint32_t t = millis();
    while (!ackGot && millis() - t < ACK_TIMEOUT_MS) { pump(); delay(1); }
  }
  pump();
  ackWait = false;
  res.total_ms = millis() - first;
  if (ackGot) {
    res.acked = true; res.status = ackStatus;
    int k = res.attempts - 1;
    while (k > 0 && (int32_t)(ackUs - starts[k]) < 0) k--;   // ACK of an earlier attempt: measure from that attempt
    res.rtt_us = ackUs - starts[k];
    rttAdd(C.rtt, res.rtt_us);
    if (S.on) rttAdd(S.rtt, res.rtt_us);
    st.acked++;
    if (res.attempts == 1) { st.first_try++; if (S.on) S.first++; }
    if (!reliable) C.raw_acked++;
    if (S.on) S.acked++;
    switch (res.status) {
      case ACK_OK: C.ack_ok++; break;
      case ACK_BAD_LEN: C.ack_bad_len++; break;
      case ACK_BAD_ARG: C.ack_bad_arg++; break;
      case ACK_BAD_VERSION: C.ack_bad_ver++; break;
    }
    if (res.status != ACK_OK) { st.rejected++; if (S.on) S.rejected++; }
  } else { st.failed++; if (S.on) S.failed++; }

  char nb[12];
  static const char* const NAMES[] = {"SET", "PROG", "ASSIGN", "PLAY", "STOP", "IDENTIFY", "UNPAIR", "SYNC"};
  const char* tn = (h->type >= MSG_SET && h->type <= MSG_SYNC) ? NAMES[h->type - MSG_SET] : (snprintf(nb, sizeof nb, "T%u", h->type), nb);
  Row r; r.event = "TX"; r.type = tn; r.len = (int)len; r.seq = h->seq; r.mode = reliable ? "reliable" : "raw";
  r.attempts = res.attempts; r.ack = res.acked ? res.status : NA; r.rtt_us = res.acked ? (int)res.rtt_us : NA; r.total_ms = (int)res.total_ms;
  if (h->type == MSG_PROG) r.slot = ((LfMsgProg*)buf)->slot + 1;
  if (!res.acked) r.note = "NO ACK";
  else if (res.status == ACK_BAD_VERSION) r.note = "node runs another protocol version: reflash it";
  else if (res.status != ACK_OK) r.note = "node rejected the command";
  logRow(r);
  if (res.acked && res.status == ACK_BAD_VERSION) Serial.println("[tx] node answered BAD_VERSION: it runs another protocol version, reflash it");
  return res;
}

static bool needNode() {
  if (haveNode) return true;
  Serial.println("ERR not paired. Type 'pair' (or hold BOOT 3 s) and hold BOOT 3 s on the node.");
  return false;
}

static SendResult sendProg(int s) {
  LfMsgProg m; memset(&m, 0, sizeof m);
  fillHeader(m.h, MSG_PROG, nodeMac, ++txSeq);
  m.slot = s; m.prog = progs[s];
  return sendCmd((uint8_t*)&m, sizeof m);
}
static SendResult sendAssign(int ch, uint8_t slot) {
  LfMsgAssign m; fillHeader(m.h, MSG_ASSIGN, nodeMac, ++txSeq);
  m.ch_mask = 1 << ch; m.slot = slot;
  return sendCmd((uint8_t*)&m, sizeof m);
}
static SendResult sendPlay(uint8_t mask) {
  LfMsgPlay m; fillHeader(m.h, MSG_PLAY, nodeMac, ++txSeq);
  m.ch_mask = mask; m.offset_ms = 0;
  return sendCmd((uint8_t*)&m, sizeof m);
}

// 8 x PROG (231 bytes), 8 x ASSIGN slot n -> channel n, PLAY all. Returns true if every command was ACKed with OK.
static bool loadAll(bool quiet = false) {
  if (!needNode()) return false;
  bool ok = true;
  loaded = true;
  uint32_t t = millis();
  for (int s = 0; s < LF_TEST_CHANNELS; s++) { SendResult r = sendProg(s); ok &= r.acked && r.status == ACK_OK; }
  for (int c = 0; c < LF_TEST_CHANNELS; c++) { SendResult r = sendAssign(c, c); ok &= r.acked && r.status == ACK_OK; }
  SendResult p = sendPlay(LF_ALL_CH);
  ok &= p.acked && p.status == ACK_OK;
  if (p.acked && p.status == ACK_OK) outChkArm(playT0);
  if (!quiet) Serial.printf("[load] %s in %lu ms (%s)\n", ok ? "all 17 commands ACKed" : "INCOMPLETE", (unsigned long)(millis() - t), reliable ? "reliable" : "raw");
  return ok;
}

static bool parseMask(const char* s, uint8_t& mask) {
  if (!s) return false;
  if (!strcmp(s, "all")) { mask = LF_ALL_CH; return true; }
  int c = atoi(s);
  if (c < 1 || c > LF_TEST_CHANNELS) return false;
  mask = 1 << (c - 1);
  return true;
}

// ------------------------------------------------------------ stress
static void pct(char* b, size_t n, uint32_t a, uint32_t t) { snprintf(b, n, "%.2f%%", t ? 100.0 * a / t : 0.0); }

static void doStress(int n, uint32_t interval, bool rel) {
  if (!needNode()) return;
  bool saved = reliable; reliable = rel;
  memset(&S, 0, sizeof S); S.on = true; S.crcBad0 = C.hb_crc_bad; S.crcChk0 = C.hb_crc_checked;
  Serial.printf("[stress] %d x load every %lu ms, %s\n", n, (unsigned long)interval, rel ? "reliable" : "raw");
  uint32_t t0 = millis(), incomplete = 0;
  for (int i = 0; i < n; i++) {
    uint32_t t = millis();
    if (!loadAll(true)) incomplete++;
    while (millis() - t < interval) { pump(); delay(1); }
    if ((i + 1) % 10 == 0) Serial.printf("[stress] %d/%d\n", i + 1, n);
  }
  waitMs(1500);   // let a heartbeat arrive after the last load
  S.on = false; reliable = saved;
  char a[16], b[16];
  pct(a, sizeof a, S.acked, S.cmds);
  uint32_t crcBad = C.hb_crc_bad - S.crcBad0, crcChk = C.hb_crc_checked - S.crcChk0;
  Serial.println("===== STRESS RESULT =====");
  Serial.printf("loads %d  incomplete %lu  time %lu ms\n", n, (unsigned long)incomplete, (unsigned long)(millis() - t0));
  Serial.printf("commands %lu  acked %lu (%s)  failed %lu  rejected %lu\n", (unsigned long)S.cmds, (unsigned long)S.acked, a, (unsigned long)S.failed, (unsigned long)S.rejected);
  Serial.printf("attempts %lu  retries %lu  first-attempt loss %.2f%% (command or ACK lost)\n", (unsigned long)S.attempts, (unsigned long)(S.attempts - S.cmds),
                S.cmds ? 100.0 * (S.cmds - S.first) / S.cmds : 0.0);
  if (S.rtt.n) Serial.printf("RTT us: min %lu  avg %lu  max %lu  (n=%lu)\n", (unsigned long)S.rtt.min, (unsigned long)(S.rtt.sum / S.rtt.n), (unsigned long)S.rtt.max, (unsigned long)S.rtt.n);
  pct(b, sizeof b, crcChk - crcBad, crcChk);
  Serial.printf("heartbeat CRC checks %lu  mismatches %lu (%s ok)\n", (unsigned long)crcChk, (unsigned long)crcBad, b);
  bool pass = rel ? (S.failed == 0 && S.rejected == 0 && incomplete == 0 && crcBad == 0) : (crcBad == 0);
  Serial.printf("STRESS: %s%s\n", pass ? "PASS" : "FAIL", rel ? "" : " (raw mode: judge by loss %, PASS only means no CRC mismatch)");
  Serial.println("===== END STRESS =====");
  char note[96];
  snprintf(note, sizeof note, "%d loads %s: acked %s failed %lu crc_bad %lu %s", n, rel ? "reliable" : "raw", a, (unsigned long)S.failed, (unsigned long)crcBad, pass ? "PASS" : "FAIL");
  logNote("STRESS", note);
  persist();
}

// ------------------------------------------------------------ report / log / clear
static size_t fileSize(const char* p) { if (!LittleFS.exists(p)) return 0; File f = LittleFS.open(p, FILE_READ); size_t n = f ? f.size() : 0; if (f) f.close(); return n; }

static void report() {
  logFlush(); persist();
  char m[18], a[16];
  uint32_t now = millis();
  Serial.println("===== CONTROL TEST REPORT =====");
  macStr(myMac, m);
  Serial.printf("control %s  fw %d  proto %d  uptime %lus  boots %lu  mode %s\n", m, FW_VERSION, LF_PROTO_VER, (unsigned long)(now / 1000), (unsigned long)C.boots, reliable ? "reliable" : "raw");
  if (haveNode) { macStr(nodeMac, m); Serial.printf("node %s  last heartbeat %s\n", m, haveHb ? String((now - lastHbMs) / 1000.0, 1).c_str() : "never"); }
  else Serial.println("node: not paired");
  Serial.printf("heap free %u  log %s %u B  %s %u B  fs %s\n", (unsigned)ESP.getFreeHeap(), LOG_FILE, (unsigned)fileSize(LOG_FILE), LOG_OLD, (unsigned)fileSize(LOG_OLD), fsOk ? "ok" : "FAIL");

  static const char* const NAMES[] = {"SET", "PROG", "ASSIGN", "PLAY", "STOP", "IDENTIFY", "UNPAIR", "SYNC"};
  uint32_t cmds = 0, acked = 0, failed = 0, rejected = 0, attempts = 0, first = 0;
  Serial.println("sent per type:        cmds attempts acked failed rejected");
  for (int i = 0; i < 8; i++) {
    const TypeStat& t = C.t[i];
    if (!t.cmds) continue;
    Serial.printf("  %-9s %9lu %8lu %5lu %6lu %8lu\n", NAMES[i], (unsigned long)t.cmds, (unsigned long)t.attempts, (unsigned long)t.acked, (unsigned long)t.failed, (unsigned long)t.rejected);
    cmds += t.cmds; acked += t.acked; failed += t.failed; rejected += t.rejected; attempts += t.attempts; first += t.first_try;
  }
  pct(a, sizeof a, acked, cmds);
  Serial.printf("commands %lu  ACKed %s  failed(no ACK after tries) %lu  rejected(ACK status!=0) %lu\n", (unsigned long)cmds, a, (unsigned long)failed, (unsigned long)rejected);
  Serial.printf("retries %lu  first-attempt success %.2f%%\n", (unsigned long)(attempts - cmds), cmds ? 100.0 * first / cmds : 0.0);
  Serial.printf("raw mode: sent %lu  answered %lu  raw loss %.2f%% (round trip: command or ACK lost)\n", (unsigned long)C.raw_sent, (unsigned long)C.raw_acked,
                C.raw_sent ? 100.0 * (C.raw_sent - C.raw_acked) / C.raw_sent : 0.0);
  Serial.printf("ACK status: ok %lu  bad_len %lu  bad_arg %lu  bad_version %lu  stale/duplicate %lu\n", (unsigned long)C.ack_ok, (unsigned long)C.ack_bad_len,
                (unsigned long)C.ack_bad_arg, (unsigned long)C.ack_bad_ver, (unsigned long)C.ack_stale);
  Serial.printf("MAC layer (send callback): ok %lu  fail %lu   rx queue drops %lu  rx bad %lu\n", (unsigned long)C.mac_ok, (unsigned long)C.mac_fail, (unsigned long)rxDrops, (unsigned long)C.rx_bad);
  if (C.rtt.n) Serial.printf("RTT us: min %lu  avg %lu  max %lu  (n=%lu)\n", (unsigned long)C.rtt.min, (unsigned long)(C.rtt.sum / C.rtt.n), (unsigned long)C.rtt.max, (unsigned long)C.rtt.n);
  if (rssiN) Serial.printf("RSSI of node frames: last %d  avg %d  min %d dBm (n=%lu)\n", rssiLast, (int)(rssiSum / (int32_t)rssiN), rssiMin, (unsigned long)rssiN);

  // program check: expected CRC vs the node's latest heartbeat
  int crcPass = 0;
  Serial.println("program CRC (expected / node):");
  for (int s = 0; s < LF_TEST_CHANNELS; s++) {
    bool ok = haveHb && lastHb.prog_crc[s] == progCrc[s];
    crcPass += ok;
    Serial.printf("  slot %d  %04X / %s  %s\n", s + 1, progCrc[s], haveHb ? String(lastHb.prog_crc[s], HEX).c_str() : "----", ok ? "PASS" : "FAIL");
  }
  Serial.printf("heartbeat CRC checks %lu  mismatches %lu  wrong channel assignment %lu\n", (unsigned long)C.hb_crc_checked, (unsigned long)C.hb_crc_bad, (unsigned long)C.hb_assign_bad);

  // output check
  bool outPass = false;
  if (O.samples) {
    outPass = O.samples >= 50 && O.tol * 100 >= O.samples * 95;
    Serial.printf("output check (%s window): samples %lu  strict(+-%d%%) %.1f%%  tolerant(+-%dms) %.1f%%  %s\n", O.done ? "last 60 s" : "running", (unsigned long)O.samples,
                  OUT_TOL_PCT, 100.0 * O.strict / O.samples, OUT_WIN_MS, 100.0 * O.tol / O.samples, !O.done ? "RUNNING" : outPass ? "PASS" : "FAIL");
    Serial.print("  per channel tolerant OK %:");
    for (int c = 0; c < LF_TEST_CHANNELS; c++) Serial.printf(" ch%d=%.0f", c + 1, O.chN[c] ? 100.0 * O.chTol[c] / O.chN[c] : 0.0);
    Serial.println();
  } else Serial.println("output check: not run yet (send 'load' or 'play')");
  Serial.printf("output check cumulative: samples %lu  tolerant ok %.1f%%\n", (unsigned long)C.out_samples, C.out_samples ? 100.0 * C.out_tol_ok / C.out_samples : 0.0);

  // heartbeat check
  bool hbPass = false;
  if (haveHb && lastHbMs - hbFirstMs >= 10000) {
    double secs = (lastHbMs - hbFirstMs) / 1000.0;
    uint32_t expected = (lastHbMs - hbFirstMs) / HB_EXPECT_MS + 1;
    hbPass = hbCount * 100 >= expected * 90 && hbMaxGap <= HB_MAX_GAP_OK_MS;
    Serial.printf("heartbeats: %lu received in %.0f s (%.2f/s, expected ~%lu)  longest gap %lu ms  %s\n", (unsigned long)hbCount, secs, hbCount / secs,
                  (unsigned long)expected, (unsigned long)hbMaxGap, hbPass ? "PASS" : "FAIL");
  } else Serial.printf("heartbeats: %lu received this session  longest gap %lu ms  (need 10 s of data)\n", (unsigned long)hbCount, (unsigned long)hbMaxGap);

  bool delivery = cmds > 0 && failed == 0 && rejected == 0;
  const char* verdict = !haveNode ? "INCOMPLETE (not paired)"
                      : (cmds == 0 || !O.done || !haveHb) ? "INCOMPLETE (run 'load' and wait 60 s)"
                      : (delivery && crcPass == LF_TEST_CHANNELS && outPass && hbPass) ? "PASS" : "FAIL";
  Serial.printf("RESULT: delivery %s | program CRC %d/8 | output %s | heartbeats %s => %s\n", reliable && cmds ? (delivery ? "PASS" : "FAIL") : (cmds ? (delivery ? "PASS" : "see raw loss") : "n/a"),
                crcPass, O.samples ? (outPass ? "PASS" : (O.done ? "FAIL" : "running")) : "n/a", hbPass ? "PASS" : "n/a-or-FAIL", verdict);
  Serial.println("===== END REPORT =====");
}

static void printLog(const char* arg) {
  logFlush();
  File f = LittleFS.open(LOG_FILE, FILE_READ);
  if (!f) { Serial.println("ERR no log file"); return; }
  bool all = arg && !strcmp(arg, "all");
  long want = (arg && atol(arg) > 0) ? atol(arg) : 50, total = 0;
  uint8_t b[256]; int n;
  while ((n = f.read(b, sizeof b)) > 0) { for (int i = 0; i < n; i++) total += b[i] == '\n'; esp_task_wdt_reset(); }
  long skip = all ? 1 : (total > want + 1 ? total - want : 1);   // row 0 is the header and is always shown
  f.seek(0);
  long line = 0;
  while ((n = f.read(b, sizeof b)) > 0) {
    for (int i = 0; i < n; i++) { if (line >= skip || line == 0) Serial.write(b[i]); if (b[i] == '\n') line++; }
    esp_task_wdt_reset();
  }
  f.close();
}

static void clearAll() {
  wbufN = 0;
  LittleFS.remove(LOG_FILE); LittleFS.remove(LOG_OLD);
  File f = LittleFS.open(LOG_FILE, FILE_WRITE);
  if (f) { f.print(CSV_HEADER); f.close(); }
  uint32_t boots = C.boots;
  memset(&C, 0, sizeof C); C.layout = 1; C.boots = boots;
  memset(&O, 0, sizeof O); chkArmed = false;
  haveHb = false; hbCount = 0; hbMaxGap = 0; rssiN = 0; rssiSum = 0; rxDrops = 0;
  persist();
  Serial.println("OK log and counters cleared (node pairing kept)");
}

// ------------------------------------------------------------ serial + button
static const char* HELP =
  "commands (channels 1..8 or 'all'):\n"
  "  pair | unpair          open 60 s pairing window / forget the node\n"
  "  mode raw|reliable      raw = send once, no retry (measures loss); reliable = up to 5 tries, 120 ms each\n"
  "  load                   send 8 programs (24 steps) + 8 assigns + play all; arms the 60 s output check\n"
  "  play | stop            play all (offset 0) / stop all (hold level)\n"
  "  set <ch|all> <0-100>   manual level\n"
  "  stress N [interval_ms] [raw|reliable]   repeat load (default 100, 200 ms)\n"
  "  progs                  print the 8 built-in programs and their CRC\n"
  "  report | log [n|all] | clear | status | help\n";

static void printProgs() {
  char text[LF_PROG_TEXT_MAX];
  for (int s = 0; s < LF_TEST_CHANNELS; s++) {
    lf::formatProgram(progs[s], text, sizeof text);
    Serial.printf("slot %d  steps %u  crc %04X  total %lu ms\n  %s\n", s + 1, progs[s].n_steps, progCrc[s], (unsigned long)lf::programTotalMs(progs[s]), text);
  }
}

static void handleLine(char* line) {
  char* cmd = strtok(line, " ");
  if (!cmd) return;
  char* a1 = strtok(nullptr, " ");
  char* a2 = strtok(nullptr, " ");
  char* a3 = strtok(nullptr, " ");
  if (!strcmp(cmd, "help")) Serial.print(HELP);
  else if (!strcmp(cmd, "pair")) startPairing();
  else if (!strcmp(cmd, "unpair")) {
    haveNode = false; memset(nodeMac, 0, 6); if (nvsOk) prefs.remove("node");
    Serial.println("OK node forgotten (the node itself stays paired until reset: hold its BOOT 10 s)");
    logNote("PAIR", "node forgotten");
  }
  else if (!strcmp(cmd, "mode")) {
    if (a1 && !strcmp(a1, "raw")) reliable = false; else if (a1 && !strcmp(a1, "reliable")) reliable = true; else { Serial.println("ERR mode raw|reliable"); return; }
    Serial.printf("OK mode %s\n", reliable ? "reliable" : "raw");
  }
  else if (!strcmp(cmd, "load")) loadAll();
  else if (!strcmp(cmd, "play")) {
    if (!needNode()) return;
    SendResult r = sendPlay(LF_ALL_CH);
    if (r.acked && r.status == ACK_OK) outChkArm(playT0);
    Serial.println(r.acked ? (r.status == ACK_OK ? "OK" : "ERR node rejected") : "ERR no ACK");
  }
  else if (!strcmp(cmd, "stop")) {
    if (!needNode()) return;
    LfMsgChMask m; fillHeader(m.h, MSG_STOP, nodeMac, ++txSeq); m.ch_mask = LF_ALL_CH;
    SendResult r = sendCmd((uint8_t*)&m, sizeof m);
    chkArmed = false;
    Serial.println(r.acked ? "OK" : "ERR no ACK");
  }
  else if (!strcmp(cmd, "set")) {
    uint8_t mask;
    if (!needNode()) return;
    if (!parseMask(a1, mask) || !a2 || atoi(a2) < 0 || atoi(a2) > 100) { Serial.println("ERR set <ch|all> <0-100>"); return; }
    LfMsgSet m; fillHeader(m.h, MSG_SET, nodeMac, ++txSeq); m.ch_mask = mask; m.level = atoi(a2); m.fade_ms = 0;
    SendResult r = sendCmd((uint8_t*)&m, sizeof m);
    chkArmed = false;
    Serial.println(r.acked ? "OK" : "ERR no ACK");
  }
  else if (!strcmp(cmd, "stress")) {
    int n = a1 ? atoi(a1) : 100; uint32_t iv = a2 && isdigit((uint8_t)a2[0]) ? strtoul(a2, nullptr, 10) : 200;
    bool rel = reliable;
    for (char* w : {a2, a3}) { if (w && !strcmp(w, "raw")) rel = false; if (w && !strcmp(w, "reliable")) rel = true; }
    if (n < 1 || n > 100000) { Serial.println("ERR stress N [interval_ms] [raw|reliable]"); return; }
    doStress(n, iv, rel);
  }
  else if (!strcmp(cmd, "progs")) printProgs();
  else if (!strcmp(cmd, "report")) report();
  else if (!strcmp(cmd, "log")) printLog(a1);
  else if (!strcmp(cmd, "clear")) clearAll();
  else if (!strcmp(cmd, "status")) {
    char m[18]; macStr(myMac, m);
    Serial.printf("control %s  radio %s  node %s  mode %s  pairing %s\n", m, radioOk ? "ok" : "FAIL", haveNode ? "paired" : "none", reliable ? "reliable" : "raw", pairing ? "OPEN" : "closed");
    if (haveNode) { macStr(nodeMac, m); Serial.printf("node %s  heartbeat %s\n", m, haveHb && millis() - lastHbMs < 3000 ? "fresh" : "none/old"); }
  }
  else Serial.println("ERR unknown command. type 'help'");
}

static void pollSerial() {
  static char buf[160]; static size_t n = 0; static bool tooLong = false;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') { if (tooLong) Serial.println("ERR line too long"); else if (n) { buf[n] = 0; handleLine(buf); } n = 0; tooLong = false; }
    else if (n < sizeof buf - 1) buf[n++] = c; else tooLong = true;
  }
}

static bool loadRequested = false;
static void pollButton(uint32_t now) {
  static uint32_t downAt = 0; static bool down = false;
  bool pressed = digitalRead(BUTTON_PIN) == LOW;
  if (pressed && !down) { down = true; downAt = now; }
  if (!pressed && down) {
    down = false;
    uint32_t held = now - downAt;
    if (held >= PAIR_HOLD_MS) startPairing();
    else if (held >= 40) loadRequested = true;   // short press = load
  }
}

static void updateLed(uint32_t now) {
  bool on;
  if (!radioOk || !nvsOk || !fsOk) on = true;
  else if (pairing) { uint32_t t = now % 1000; on = t < 100 || (t >= 200 && t < 300); }       // double blink
  else if (haveHb && now - lastHbMs < 3000) on = (now % 2000) < 1000;                         // slow: node heartbeats fresh
  else on = (now % 200) < 100;                                                                // fast: no node / no heartbeats
  digitalWrite(STATUS_LED_PIN, on);
}

// ------------------------------------------------------------ main
void setup() {
  pinMode(STATUS_LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Serial.begin(115200);
  delay(50);
  Serial.printf("\n[boot] LED frame CONTROL TEST fw %d, protocol %d, reset reason %d\n", FW_VERSION, LF_PROTO_VER, (int)esp_reset_reason());
  txSeq = (uint16_t)esp_random();

  memset(&C, 0, sizeof C); C.layout = 1;
  nvsOk = prefs.begin("lfctest", false);
  if (nvsOk) {
    Counters tmp;
    if (prefs.getBytesLength("c") == sizeof tmp) { prefs.getBytes("c", &tmp, sizeof tmp); if (tmp.layout == 1) C = tmp; }
    if (prefs.getBytesLength("node") == 6) { prefs.getBytes("node", nodeMac, 6); haveNode = true; }
  }
  C.boots++;
  fsOk = LittleFS.begin(true);
  if (fsOk && !LittleFS.exists(LOG_FILE)) { File f = LittleFS.open(LOG_FILE, FILE_WRITE); if (f) { f.print(CSV_HEADER); f.close(); } }

  for (int s = 0; s < LF_TEST_CHANNELS; s++) {
    if (!lf::testProgram(s, progs[s])) Serial.printf("[boot] ERROR: built-in program %d does not parse\n", s + 1);
    progCrc[s] = lf::programCrc(progs[s]);
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(LF_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  WiFi.macAddress(myMac);
  rxQueue = xQueueCreate(32, sizeof(RxItem));
  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(onRecv);
    esp_now_register_send_cb(onSent);
    addPeer(BCAST);
    if (haveNode) addPeer(nodeMac);
    radioOk = true;
    wifi_promiscuous_filter_t f = {}; f.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&f);
    esp_wifi_set_promiscuous_rx_cb(onPromisc);
    esp_wifi_set_promiscuous(true);
  } else Serial.println("[boot] ERROR: ESP-NOW init failed");

  char m[18]; macStr(myMac, m);
  Serial.printf("[boot] control MAC %s  node %s  channel %d  seq start %u\n", m, haveNode ? "paired" : "none", LF_WIFI_CHANNEL, txSeq);
  char nm[18]; if (haveNode) { macStr(nodeMac, nm); Serial.printf("[boot] node MAC %s\n", nm); }
  printProgs();
  char note[64]; snprintf(note, sizeof note, "reset %d fs %s node %s", (int)esp_reset_reason(), fsOk ? "ok" : "FAIL", haveNode ? "paired" : "none");
  logNote("BOOT", note);
  persist();

  esp_task_wdt_init(15, true);
  esp_task_wdt_add(NULL);
  Serial.print(HELP);
}

void loop() {
  static uint32_t nextPersist = 10000;
  static bool reported = false;
  uint32_t now = millis();
  esp_task_wdt_reset();
  pump();
  pollSerial();
  pollButton(now);
  if (loadRequested) { loadRequested = false; loadAll(); }
  updateLed(now);

  if (pairing && now - pairStart >= PAIR_WINDOW_MS) { pairing = false; pairConfirmPending = false; logNote("PAIR", "window closed"); Serial.println("[pair] window closed"); }
  if (chkArmed && now - playT0 > OUTCHK_MS + 1000) outChkFinish();   // no heartbeat came to close the window
  if (!reported && now >= 5000) { reported = true; report(); }
  if (wbufN && now - lastFlush >= 2000) logFlush();
  if (cDirty && now >= nextPersist) { persist(); nextPersist = now + 10000; }
  delay(1);
}
