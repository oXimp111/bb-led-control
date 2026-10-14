// Wall node firmware. Same binary on every node; identity = MAC address.
// Receives commands from the control center over ESP-NOW, plays programs, saves
// everything to flash, and accepts the same commands over USB serial for testing.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_task_wdt.h>
#include <Preferences.h>
#include <errno.h>
#include "node_config.h"
#include "protocol.h"
#include "engine.h"

// ------------------------------------------------------------ state
struct Persist {               // saved to flash as one blob
  uint8_t layout = 1;
  uint8_t paired = 0;
  uint8_t ctrl[6] = {0};
  uint8_t assign[LF_MAX_CH];
  uint8_t manual[LF_MAX_CH];   // manual level %, used when not playing
  uint8_t playing_mask = 0;    // channels that auto-play at boot
};

static Persist      st;
static LfProgram    progs[LF_MAX_PROGS];
static lf::Channel  chans[NUM_CH];
static Preferences  prefs;
static uint8_t      myMac[6];
static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static bool     radioOk = false, nvsOk = false;
static bool     pairing = false;
static uint32_t pairStart = 0, lastPairReq = 0;
static uint32_t identifyUntil[NUM_CH] = {0};
static uint32_t lastDuty[NUM_CH];
static bool     dirty = false;
static uint32_t dirtyAt = 0;
static volatile uint32_t lastDelivered = 0;  // last heartbeat MAC-acked by controller
static uint16_t txSeq = 0;
static bool     haveLastSeq = false;
static uint16_t lastSeq = 0;
static uint8_t  lastStatus = 0;

// ------------------------------------------------------------ helpers
static void macStr(const uint8_t* m, char* out) {
  sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void markDirty() { dirty = true; dirtyAt = millis(); }

#ifdef LF_FULLTEST   // test instrumentation (src/node/fulltest.h); not part of the production build
enum { FTC_PROG_WRITE, FTC_PROG_SKIP, FTC_STATE_SAVE, FTC_HB_SENT, FTC_QDROP };
static void ft_count(int what);
#define FT(x) x
#else
#define FT(x)
#endif

static void saveState() {
  if (!nvsOk) return;
  for (int i = 0; i < NUM_CH; i++) {
    st.manual[i] = (uint8_t)lroundf(chans[i].manualTarget() * 100);
    if (chans[i].playing()) st.playing_mask |= (1 << i); else st.playing_mask &= ~(1 << i);
  }
  prefs.putBytes("state", &st, sizeof(st));
  FT(ft_count(FTC_STATE_SAVE));
  dirty = false;
  Serial.println("[save] state saved");
}

static void saveProgs() {
  if (nvsOk) prefs.putBytes("progs", progs, sizeof(progs));
  FT(ft_count(FTC_PROG_WRITE));
}

static void loadAll() {
  memset(st.assign, LF_NO_PROGRAM, sizeof(st.assign));
  memset(st.manual, 0, sizeof(st.manual));
  memset(progs, 0, sizeof(progs));
  nvsOk = prefs.begin("lf", false);
  if (!nvsOk) { Serial.println("[boot] ERROR: flash storage (NVS) failed"); return; }
  Persist tmp;
  if (prefs.getBytesLength("state") == sizeof(Persist)) {
    prefs.getBytes("state", &tmp, sizeof(tmp));
    if (tmp.layout == 1) st = tmp;
  }
  size_t progBytes = prefs.getBytesLength("progs");
  if (progBytes == sizeof(progs)) prefs.getBytes("progs", progs, sizeof(progs));
  else if (progBytes != 0) {  // saved by firmware with a different program size (e.g. old 10-step layout)
    prefs.remove("progs");
    Serial.println("[boot] old program format discarded");
  }
  for (int i = 0; i < LF_MAX_PROGS; i++)
    if (!lf::validateProgram(progs[i])) memset(&progs[i], 0, sizeof(LfProgram));
}

static bool chanHasProgram(int i) {
  uint8_t s = st.assign[i];
  return s < LF_MAX_PROGS && progs[s].n_steps > 0;
}

// ------------------------------------------------------------ actions (serial + radio share these)
static bool normMask(uint8_t& mask);

static uint8_t doSet(uint8_t mask, uint8_t level, uint32_t fade) {
  if (level > 100 || !normMask(mask)) return ACK_BAD_ARG;
  uint32_t now = millis();
  for (int i = 0; i < NUM_CH; i++) if (mask & (1 << i)) chans[i].setManual(level / 100.0f, fade, now);
  markDirty();
  return ACK_OK;
}

// Channel mask check shared by every command: only existing channels, at least one.
// LF_ALL_CH (0xFF) means "every channel" whatever NUM_CH is.
static bool normMask(uint8_t& mask) {
  const uint8_t valid = (uint8_t)((1u << NUM_CH) - 1);
  if (mask == LF_ALL_CH) mask = valid;
  return mask != 0 && (mask & ~valid) == 0;
}

static uint8_t doPlay(uint8_t mask, uint32_t offset_ms = 0) {
  if (!normMask(mask)) return ACK_BAD_ARG;
  uint32_t now = millis();
  for (int i = 0; i < NUM_CH; i++)
    if ((mask & (1 << i)) && chanHasProgram(i)) chans[i].play(&progs[st.assign[i]], now, offset_ms);
  markDirty();
  return ACK_OK;
}

// Re-anchor playing channels to show time t_ms. Nothing worth saving, no ACK.
static uint8_t doSync(uint8_t mask, uint32_t t_ms, int* moved = nullptr) {
  if (!normMask(mask)) return ACK_BAD_ARG;
  uint32_t now = millis();
  int n = 0;
  for (int i = 0; i < NUM_CH; i++)
    if ((mask & (1 << i)) && chans[i].sync(t_ms, now)) n++;
  if (moved) *moved = n;
  return ACK_OK;
}

static uint8_t doStop(uint8_t mask) {
  if (!normMask(mask)) return ACK_BAD_ARG;
  uint32_t now = millis();
  for (int i = 0; i < NUM_CH; i++) if (mask & (1 << i)) chans[i].stop(now);
  markDirty();
  return ACK_OK;
}

static uint8_t doProg(uint8_t slot, const LfProgram& p) {
  if (slot >= LF_MAX_PROGS || !lf::validateProgram(p)) return ACK_BAD_ARG;
  LfProgram np = p;
  lf::normalizeProgram(np);
  if (memcmp(&np, &progs[slot], sizeof(np)) != 0) {  // identical re-upload: no flash write
    progs[slot] = np;
    saveProgs();
  } else FT(ft_count(FTC_PROG_SKIP));
  // channels currently playing this slot restart from t=0 with the new program
  uint8_t restart = 0;
  for (int i = 0; i < NUM_CH; i++) if (chans[i].playing() && st.assign[i] == slot) restart |= 1 << i;
  if (restart) doPlay(restart);
  char buf[LF_PROG_TEXT_MAX]; lf::formatProgram(progs[slot], buf, sizeof buf);
  Serial.printf("[prog] slot %u = %s\n", slot + 1, buf);
  return ACK_OK;
}

// Cue: play_mask channels switch to bank `bank` and play from offset_ms; stop_mask channels stop and hold their level.
// All-or-nothing: a bad argument or a missing program changes nothing (the controller then uploads and retries).
static uint8_t doGo(uint8_t bank, uint8_t play_mask, uint8_t stop_mask, uint32_t offset_ms) {
  const uint8_t valid = (uint8_t)((1u << NUM_CH) - 1);
  if (bank > 1 || (play_mask | stop_mask) == 0 || ((play_mask | stop_mask) & ~valid) || (play_mask & stop_mask)) return ACK_BAD_ARG;
  for (int i = 0; i < NUM_CH; i++)
    if ((play_mask & (1 << i)) && progs[bank * LF_MAX_CH + i].n_steps == 0) return ACK_BAD_ARG;
  uint32_t now = millis();
  for (int i = 0; i < NUM_CH; i++) {
    if (play_mask & (1 << i)) { st.assign[i] = (uint8_t)(bank * LF_MAX_CH + i); chans[i].play(&progs[st.assign[i]], now, offset_ms); }
    else if (stop_mask & (1 << i)) chans[i].stop(now);
  }
  markDirty();
  return ACK_OK;
}

static uint8_t doAssign(uint8_t mask, uint8_t slot) {
  if ((slot >= LF_MAX_PROGS && slot != LF_NO_PROGRAM) || !normMask(mask)) return ACK_BAD_ARG;
  for (int i = 0; i < NUM_CH; i++) if (mask & (1 << i)) st.assign[i] = slot;
  markDirty();
  return ACK_OK;
}

static uint8_t doIdentify(uint8_t mask, uint8_t seconds) {
  if (!normMask(mask)) return ACK_BAD_ARG;
  uint32_t until = millis() + (uint32_t)seconds * 1000;
  for (int i = 0; i < NUM_CH; i++) if (mask & (1 << i)) identifyUntil[i] = until;
  return ACK_OK;
}

static void addPeer(const uint8_t* mac) {
  if (esp_now_is_peer_exist(mac)) return;
  esp_now_peer_info_t p = {};
  memcpy(p.peer_addr, mac, 6);
  p.channel = 0;  // = current channel
  p.ifidx = WIFI_IF_STA;
  p.encrypt = false;
  esp_now_add_peer(&p);
}

static void unpair() {
  if (st.paired && esp_now_is_peer_exist(st.ctrl)) esp_now_del_peer(st.ctrl);
  st.paired = 0;
  memset(st.ctrl, 0, 6);
  markDirty();
  Serial.println("[pair] unpaired");
}

static void factoryReset() {
  Serial.println("[reset] FACTORY RESET - erasing flash settings");
  if (nvsOk) prefs.clear();
  delay(200);
  ESP.restart();
}

static void startPairing() {
  pairing = true;
  pairStart = millis();
  lastPairReq = 0;
  Serial.println("[pair] pairing window open (60 s)");
}

// ------------------------------------------------------------ radio
struct RxItem { uint8_t mac[6]; uint8_t len; uint8_t data[250];
#ifdef LF_FULLTEST
  uint32_t t_us;   // arrival time (WiFi task)
#endif
};
static QueueHandle_t rxQueue;
#ifdef LF_FULLTEST
#include "fulltest.h"
#endif

static void onRecv(const uint8_t* mac, const uint8_t* data, int len) {  // WiFi task: copy only
  if (len <= 0 || len > 250) return;
  RxItem it;
  memcpy(it.mac, mac, 6);
  it.len = (uint8_t)len;
  memcpy(it.data, data, len);
  FT(it.t_us = (uint32_t)esp_timer_get_time());
  if (xQueueSend(rxQueue, &it, 0) != pdTRUE) FT(ft_count(FTC_QDROP));
}

static void onSent(const uint8_t* mac, esp_now_send_status_t s) {
  if (s == ESP_NOW_SEND_SUCCESS && st.paired && memcmp(mac, st.ctrl, 6) == 0) lastDelivered = millis();
}

static void fillHeader(LfHeader& h, uint8_t type, const uint8_t* target) {
  h.magic = LF_MAGIC;
  h.ver = LF_PROTO_VER;
  h.type = type;
  memcpy(h.target, target, 6);
  h.seq = txSeq++;
}

static void sendAck(uint16_t seq, uint8_t status) {
  LfMsgAck a;
  fillHeader(a.h, MSG_ACK, st.ctrl);
  a.acked_seq = seq;
  a.status = status;
  FT(ft_ack(seq, status));
  esp_now_send(st.ctrl, (uint8_t*)&a, sizeof(a));
}

static void sendHeartbeat() {
  LfMsgHeartbeat hb = {};
  fillHeader(hb.h, MSG_HEARTBEAT, st.ctrl);
  hb.fw_ver = FW_VERSION;
  hb.uptime_s = millis() / 1000;
  hb.n_ch = NUM_CH;
  for (int i = 0; i < LF_MAX_CH; i++) {
    hb.assign[i] = st.assign[i];
    if (i < NUM_CH) {
      hb.level[i] = (uint8_t)lroundf(chans[i].level() * 100);
      if (chans[i].playing()) hb.playing_mask |= 1 << i;
    }
  }
  for (int s = 0; s < LF_MAX_PROGS; s++) hb.prog_crc[s] = lf::programCrc(progs[s]);
  FT(ft_count(FTC_HB_SENT));
  esp_now_send(st.ctrl, (uint8_t*)&hb, sizeof(hb));
}

static void sendPairReq() {
  LfMsgPairReq r;
  fillHeader(r.h, MSG_PAIR_REQ, BCAST);
  r.fw_ver = FW_VERSION;
  r.n_ch = NUM_CH;
  esp_now_send(BCAST, (uint8_t*)&r, sizeof(r));
}

static void warnBadVersion(const LfHeader* h, const char* from) {
  static uint32_t lastWarn = 0;
  uint32_t now = millis();
  if (lastWarn && now - lastWarn < 1000) return;  // SYNC may arrive every 2 s: don't flood the log
  lastWarn = now;
  Serial.printf("[rx] WARNING: packet from %s has protocol v%u, this node speaks v%u. Not applied. Update the firmware on the older side.\n",
                from, h->ver, LF_PROTO_VER);
}

static void handlePacket(const RxItem& it) {
  if (it.len < sizeof(LfHeader)) return;
  const LfHeader* h = (const LfHeader*)it.data;
  if (h->magic != LF_MAGIC) return;
  bool toMe = memcmp(h->target, myMac, 6) == 0;
  bool toAll = memcmp(h->target, BCAST, 6) == 0;
  char ms[18]; macStr(it.mac, ms);

  if (h->type == MSG_PAIR_ACK) {
    if (!pairing || !toMe) return;
    if (h->ver != LF_PROTO_VER) { warnBadVersion(h, ms); return; }
    memcpy(st.ctrl, it.mac, 6);
    st.paired = 1;
    addPeer(st.ctrl);
    pairing = false;
    haveLastSeq = false;
    saveState();
    Serial.printf("[pair] paired to controller %s\n", ms);
    return;
  }

  if (!st.paired || memcmp(it.mac, st.ctrl, 6) != 0) return;  // only my controller
  if (!toMe && !toAll) return;

  if (h->ver != LF_PROTO_VER) {  // header layout is stable, so we can still tell the controller why
    warnBadVersion(h, ms);
    if (h->type != MSG_SYNC) sendAck(h->seq, ACK_BAD_VERSION);
    return;
  }

  if (h->type == MSG_SYNC) {  // best effort: no ACK, not part of the retry/dedupe sequence
    if (it.len >= sizeof(LfMsgSync)) {
      LfMsgSync m; memcpy(&m, it.data, sizeof(m));
      doSync(m.ch_mask, m.t_ms);
    }
    return;
  }

  if (haveLastSeq && h->seq == lastSeq) {  // retry of a command already applied
    sendAck(h->seq, lastStatus);
    return;
  }

  uint8_t status = ACK_BAD_LEN;
  switch (h->type) {
    case MSG_SET:
      if (it.len >= sizeof(LfMsgSet)) {
        const LfMsgSet* m = (const LfMsgSet*)it.data;
        status = doSet(m->ch_mask, m->level, m->fade_ms);
      } break;
    case MSG_PROG:
      if (it.len >= sizeof(LfMsgProg)) {
        LfMsgProg m; memcpy(&m, it.data, sizeof(m));
        status = doProg(m.slot, m.prog);
      } break;
    case MSG_ASSIGN:
      if (it.len >= sizeof(LfMsgAssign)) {
        const LfMsgAssign* m = (const LfMsgAssign*)it.data;
        status = doAssign(m->ch_mask, m->slot);
      } break;
    case MSG_PLAY:
      if (it.len >= sizeof(LfMsgPlay)) {
        LfMsgPlay m; memcpy(&m, it.data, sizeof(m));
        status = doPlay(m.ch_mask, m.offset_ms);
      } break;
    case MSG_GO:
      if (it.len >= sizeof(LfMsgGo)) {
        LfMsgGo m; memcpy(&m, it.data, sizeof(m));
        status = doGo(m.bank, m.play_mask, m.stop_mask, m.offset_ms);
      } break;
    case MSG_STOP:
      if (it.len >= sizeof(LfMsgChMask)) {
        const LfMsgChMask* m = (const LfMsgChMask*)it.data;
        status = doStop(m->ch_mask);
      } break;
    case MSG_IDENTIFY:
      if (it.len >= sizeof(LfMsgIdentify)) {
        const LfMsgIdentify* m = (const LfMsgIdentify*)it.data;
        status = doIdentify(m->ch_mask, m->seconds);
      } break;
    case MSG_UNPAIR:
      sendAck(h->seq, ACK_OK);
      unpair();
      return;
    default:
      status = ACK_BAD_ARG;
  }
  haveLastSeq = true;
  lastSeq = h->seq;
  lastStatus = status;
  sendAck(h->seq, status);
  Serial.printf("[rx] type %u seq %u -> status %u\n", h->type, h->seq, status);
}

static void radioInit() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(LF_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  WiFi.macAddress(myMac);
  rxQueue = xQueueCreate(16, sizeof(RxItem));
  if (esp_now_init() != ESP_OK) { Serial.println("[boot] ERROR: ESP-NOW init failed"); return; }
  esp_now_register_recv_cb(onRecv);
  esp_now_register_send_cb(onSent);
  addPeer(BCAST);
  if (st.paired) addPeer(st.ctrl);
  radioOk = true;
}

// ------------------------------------------------------------ serial CLI (1-based channels/slots)
static bool parseMask(const char* s, uint8_t& mask) {
  if (!strcmp(s, "all")) { mask = (uint8_t)((1u << NUM_CH) - 1); return true; }
  int c = atoi(s);
  if (c < 1 || c > NUM_CH) return false;
  mask = 1 << (c - 1);
  return true;
}

static bool parseU32(const char* s, uint32_t& out) {  // plain decimal 0..4294967295, nothing else
  while (*s == ' ') s++;
  if (*s < '0' || *s > '9') return false;
  char* end;
  errno = 0;
  unsigned long long v = strtoull(s, &end, 10);
  while (*end == ' ') end++;
  if (errno || *end || v > 0xFFFFFFFFull) return false;
  out = (uint32_t)v;
  return true;
}

static void printStatus() {
  char m[18];
  macStr(myMac, m);
  Serial.printf("node %s  fw %d  uptime %lus  radio %s  nvs %s\n", m, FW_VERSION,
                millis() / 1000, radioOk ? "ok" : "FAIL", nvsOk ? "ok" : "FAIL");
  if (st.paired) {
    macStr(st.ctrl, m);
    uint32_t ago = millis() - lastDelivered;
    Serial.printf("paired to %s  link %s\n", m, (lastDelivered && ago < LINK_TIMEOUT_MS) ? "OK" : "LOST");
  } else Serial.printf("not paired%s\n", pairing ? " (pairing window open)" : "");
  for (int i = 0; i < NUM_CH; i++) {
    uint8_t s = st.assign[i];
    Serial.printf("ch%d gpio%d level %3d%%  %s  program %s\n", i + 1, PWM_PINS[i],
                  (int)lroundf(chans[i].level() * 100), chans[i].playing() ? "PLAYING" : "manual ",
                  s < LF_MAX_PROGS ? String(s + 1).c_str() : "-");
  }
}

static void printProgs() {
  char buf[LF_PROG_TEXT_MAX];
  for (int s = 0; s < LF_MAX_PROGS; s++) {
    lf::formatProgram(progs[s], buf, sizeof buf);
    Serial.printf("slot %d crc %04X: %s\n", s + 1, lf::programCrc(progs[s]), buf);
  }
}

static const char* HELP =
  "commands (channels 1..8 or 'all', slots 1..16):\n"
  "  status | progs | help\n"
  "  set <ch> <0-100> [fade_ms]\n"
  "  prog <slot> [loop] <steps>   e.g. prog 1 H100:10s B0-100/1s:5s H0:0\n"
  "       H<lvl>:<dur>  F<a>-<b>:<dur>  B<a>-<b>/<period>:<dur>  K:<dur> (keep level)   dur: 500ms | 2s\n"
  "  assign <ch> <slot|none>\n"
  "  play <ch> [offset_ms]        start as if offset_ms already elapsed\n"
  "  sync <ch> <t_ms>             playing channels: jump to t_ms if off by > 20 ms\n"
  "  go <bank 0|1> <play mask> <stop mask> [offset_ms]   cue; masks are numbers, bit0 = ch1 (slot = bank*8+ch)\n"
  "  stop <ch> | identify <ch> [seconds]\n"
  "  pair | unpair | reset (factory) | reboot\n";

static void handleLine(char* line) {
#ifdef LF_FULLTEST
  if (ft_cmd(line)) return;
#endif
  char* cmd = strtok(line, " ");
  if (!cmd) return;
  char* a1 = strtok(nullptr, " ");
  char* rest = strtok(nullptr, "");  // remainder of line
  uint8_t mask, r = ACK_OK;

  if (!strcmp(cmd, "help")) { Serial.print(HELP); return; }
  if (!strcmp(cmd, "status")) { printStatus(); return; }
  if (!strcmp(cmd, "progs")) { printProgs(); return; }
  if (!strcmp(cmd, "pair")) { startPairing(); return; }
  if (!strcmp(cmd, "unpair")) { unpair(); return; }
  if (!strcmp(cmd, "reset")) { factoryReset(); return; }
  if (!strcmp(cmd, "reboot")) { ESP.restart(); return; }

  if (!strcmp(cmd, "go")) {   // go <bank> <play mask> <stop mask> [offset_ms]
    uint32_t b, pm, sm, off = 0; char* t = strtok(rest, " ");
    if (!a1 || !parseU32(a1, b) || !t || !parseU32(t, pm) || !(t = strtok(nullptr, " ")) || !parseU32(t, sm)
        || ((t = strtok(nullptr, " ")) && !parseU32(t, off)) || pm > 255 || sm > 255) { Serial.println("ERR go <bank 0|1> <play mask> <stop mask> [offset_ms]"); return; }
    r = doGo((uint8_t)b, (uint8_t)pm, (uint8_t)sm, off);
    Serial.println(r == ACK_OK ? "OK" : "ERR bad argument");
    return;
  }
  if (!strcmp(cmd, "prog")) {
    int slot = a1 ? atoi(a1) : 0;
    if (slot < 1 || slot > LF_MAX_PROGS) { Serial.println("ERR slot 1..16"); return; }
    LfProgram p; char err[64];
    if (!lf::parseProgram(rest ? rest : "", p, err, sizeof err)) { Serial.printf("ERR %s\n", err); return; }
    r = doProg(slot - 1, p);
  } else if (!a1 || !parseMask(a1, mask)) {
    Serial.println("ERR bad command or channel. type 'help'"); return;
  } else if (!strcmp(cmd, "set")) {
    char* lv = rest ? strtok(rest, " ") : nullptr;
    char* fd = strtok(nullptr, " ");
    if (!lv) { Serial.println("ERR set <ch> <0-100> [fade_ms]"); return; }
    r = doSet(mask, (uint8_t)constrain(atoi(lv), 0, 255), fd ? strtoul(fd, nullptr, 10) : 0);
  } else if (!strcmp(cmd, "assign")) {
    if (!rest) { Serial.println("ERR assign <ch> <slot|none>"); return; }
    bool none = !strncmp(rest, "none", 4);
    int sn = atoi(rest);
    if (!none && (sn < 1 || sn > LF_MAX_PROGS)) { Serial.println("ERR slot 1..16 or none"); return; }
    r = doAssign(mask, none ? LF_NO_PROGRAM : (uint8_t)(sn - 1));
  } else if (!strcmp(cmd, "play")) {
    uint32_t off = 0;
    if (rest && !parseU32(rest, off)) { Serial.println("ERR play <ch> [offset_ms]"); return; }
    r = doPlay(mask, off);
  } else if (!strcmp(cmd, "sync")) {
    uint32_t t;
    if (!rest || !parseU32(rest, t)) { Serial.println("ERR sync <ch> <t_ms>"); return; }
    int moved = 0;
    r = doSync(mask, t, &moved);
    if (r == ACK_OK) { Serial.printf("OK (%d channel(s) moved)\n", moved); return; }
  }
  else if (!strcmp(cmd, "stop")) r = doStop(mask);
  else if (!strcmp(cmd, "identify")) r = doIdentify(mask, rest ? atoi(rest) : 5);
  else { Serial.println("ERR unknown command. type 'help'"); return; }
  Serial.println(r == ACK_OK ? "OK" : "ERR bad argument");
}

#define SERIAL_LINE_MAX 1024  // a 24-step program is ~700 characters

static void pollSerial() {
  static char buf[SERIAL_LINE_MAX];
  static size_t n = 0;
  static bool tooLong = false;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (tooLong) Serial.println("ERR line too long");
      else if (n) { buf[n] = 0; handleLine(buf); }
      n = 0;
      tooLong = false;
    }
    else if (n < sizeof(buf) - 1) buf[n++] = c;
    else tooLong = true;  // drop the rest of this line, report at the newline
  }
}

// ------------------------------------------------------------ button + status LED
static void pollButton(uint32_t now) {
  static uint32_t downAt = 0;
  static bool down = false;
  bool pressed = digitalRead(BUTTON_PIN) == LOW;
  if (pressed && !down) { down = true; downAt = now; }
  if (pressed && down && now - downAt >= RESET_HOLD_MS) factoryReset();
  if (!pressed && down) {
    down = false;
    uint32_t held = now - downAt;
    if (held >= PAIR_HOLD_MS) startPairing();
  }
}

static bool buttonHeldLong(uint32_t now) {
  // solid LED while held past 3 s so the person knows pairing will start on release
  static uint32_t downAt = 0;
  bool pressed = digitalRead(BUTTON_PIN) == LOW;
  if (!pressed) { downAt = 0; return false; }
  if (!downAt) downAt = now;
  return now - downAt >= PAIR_HOLD_MS;
}

static void updateLed(uint32_t now) {
  bool on;
  if (!radioOk || !nvsOk || buttonHeldLong(now)) on = true;                 // solid: error / hold feedback
  else if (pairing) { uint32_t t = now % 1000; on = t < 100 || (t >= 200 && t < 300); }  // double blink
  else if (st.paired && lastDelivered && now - lastDelivered < LINK_TIMEOUT_MS)
    on = (now % 2000) < 1000;                                               // slow: connected
  else on = (now % 200) < 100;                                              // fast: searching / unpaired
  digitalWrite(STATUS_LED_PIN, on);
}

// ------------------------------------------------------------ output
static float pulse(uint32_t now) {  // 0..1 triangle, 1 s period
  float ph = (now % 1000) / 1000.0f;
  return ph < 0.5f ? ph * 2 : 2 - ph * 2;
}

static void updateOutputs(uint32_t now) {
  for (int i = 0; i < NUM_CH; i++) {
    float lv = chans[i].update(now);
    if (pairing) lv = 0.3f * pulse(now);                         // strips pulse during pairing
    else if ((int32_t)(identifyUntil[i] - now) > 0) lv = pulse(now);
    uint32_t d = lf::levelToDuty(lv, PWM_MAX_DUTY);
    if (d != lastDuty[i]) { ledcWrite(i, d); lastDuty[i] = d; }
  }
}

// ------------------------------------------------------------ main
void setup() {
  // 1) outputs OFF immediately (no flash of light at boot)
  for (int i = 0; i < NUM_CH; i++) {
    ledcSetup(i, PWM_FREQ_HZ, PWM_BITS);
    ledcAttachPin(PWM_PINS[i], i);
    ledcWrite(i, 0);
    lastDuty[i] = 0;
  }
  FT(ft_padsEnable());
  pinMode(STATUS_LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Serial.setRxBufferSize(2048);  // before begin(): a pasted 24-step line must not overflow while flash is written
#ifdef LF_FULLTEST
  Serial.begin(460800);
#else
  Serial.begin(115200);
#endif
  delay(50);
  Serial.printf("\n[boot] LED frame node fw %d, reset reason %d\n", FW_VERSION, (int)esp_reset_reason());
  FT(Serial.println("[boot] FULL CAPABILITY NODE TEST build (LF_FULLTEST): packets and pad levels are logged on serial, 460800 baud"));

  // 2) restore saved state BEFORE radio
  loadAll();
  uint32_t now = millis();
  for (int i = 0; i < NUM_CH; i++) {
    if ((st.playing_mask & (1 << i)) && chanHasProgram(i)) chans[i].play(&progs[st.assign[i]], now);
    else chans[i].setManual(st.manual[i] / 100.0f, 0, now);
  }
  updateOutputs(now);
  Serial.println("[boot] state restored");

  // 3) radio
  radioInit();
  char m[18]; macStr(myMac, m);
  Serial.printf("[boot] MAC %s  paired %s\n", m, st.paired ? "yes" : "no");

  // 4) watchdog: reboot if loop() hangs
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);
  Serial.print(HELP);
}

void loop() {
  static uint32_t lastTick = 0, nextHb = 0;
  uint32_t now = millis();
  esp_task_wdt_reset();

  RxItem it;
  FT(ft_loop(now));
  while (xQueueReceive(rxQueue, &it, 0) == pdTRUE) {
    FT(ft_pre(it));
    handlePacket(it);
    FT(ft_post(it));
  }
  pollSerial();
  pollButton(now);

  if (now - lastTick >= TICK_MS) { lastTick = now; updateOutputs(now); updateLed(now); }

  if (pairing) {
    if ((int32_t)(now - pairStart) >= (int32_t)PAIR_WINDOW_MS)   // signed: pairStart can be a few ms newer than this loop's `now`
       { pairing = false; Serial.println("[pair] window closed, no controller"); }
    else if (now - lastPairReq >= PAIR_REQ_EVERY_MS) { lastPairReq = now; sendPairReq(); }
  }

  if (st.paired && radioOk && (int32_t)(now - nextHb) >= 0) {
    if (nextHb == 0) nextHb = now + (esp_random() % HEARTBEAT_MS);  // random start offset
    else {
      sendHeartbeat();
      nextHb = now + HEARTBEAT_MS - HEARTBEAT_JITTER + (esp_random() % (2 * HEARTBEAT_JITTER));
    }
  }

  if (dirty && now - dirtyAt >= SAVE_DELAY_MS) saveState();
  delay(1);
}
