// Wall node firmware. Same binary on every node; identity = MAC address.
// Receives commands from the control center over ESP-NOW, plays programs, saves
// everything to flash, and accepts the same commands over USB serial for testing.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_task_wdt.h>
#include <Preferences.h>
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

static void saveState() {
  if (!nvsOk) return;
  for (int i = 0; i < NUM_CH; i++) {
    st.manual[i] = (uint8_t)lroundf(chans[i].manualTarget() * 100);
    if (chans[i].playing()) st.playing_mask |= (1 << i); else st.playing_mask &= ~(1 << i);
  }
  prefs.putBytes("state", &st, sizeof(st));
  dirty = false;
  Serial.println("[save] state saved");
}

static void saveProgs() {
  if (nvsOk) prefs.putBytes("progs", progs, sizeof(progs));
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
  if (prefs.getBytesLength("progs") == sizeof(progs)) prefs.getBytes("progs", progs, sizeof(progs));
  for (int i = 0; i < LF_MAX_PROGS; i++)
    if (!lf::validateProgram(progs[i])) memset(&progs[i], 0, sizeof(LfProgram));
}

static bool chanHasProgram(int i) {
  uint8_t s = st.assign[i];
  return s < LF_MAX_PROGS && progs[s].n_steps > 0;
}

// ------------------------------------------------------------ actions (serial + radio share these)
static uint8_t doSet(uint8_t mask, uint8_t level, uint32_t fade) {
  if (level > 100) return ACK_BAD_ARG;
  uint32_t now = millis();
  for (int i = 0; i < NUM_CH; i++) if (mask & (1 << i)) chans[i].setManual(level / 100.0f, fade, now);
  markDirty();
  return ACK_OK;
}

static uint8_t doPlay(uint8_t mask) {
  uint32_t now = millis();
  for (int i = 0; i < NUM_CH; i++)
    if ((mask & (1 << i)) && chanHasProgram(i)) chans[i].play(&progs[st.assign[i]], now);
  markDirty();
  return ACK_OK;
}

static uint8_t doStop(uint8_t mask) {
  uint32_t now = millis();
  for (int i = 0; i < NUM_CH; i++) if (mask & (1 << i)) chans[i].stop(now);
  markDirty();
  return ACK_OK;
}

static uint8_t doProg(uint8_t slot, const LfProgram& p) {
  if (slot >= LF_MAX_PROGS || !lf::validateProgram(p)) return ACK_BAD_ARG;
  progs[slot] = p;
  lf::normalizeProgram(progs[slot]);
  saveProgs();
  // channels currently playing this slot restart from t=0 with the new program
  uint8_t restart = 0;
  for (int i = 0; i < NUM_CH; i++) if (chans[i].playing() && st.assign[i] == slot) restart |= 1 << i;
  if (restart) doPlay(restart);
  char buf[200]; lf::formatProgram(progs[slot], buf, sizeof buf);
  Serial.printf("[prog] slot %u = %s\n", slot + 1, buf);
  return ACK_OK;
}

static uint8_t doAssign(uint8_t mask, uint8_t slot) {
  if (slot >= LF_MAX_PROGS && slot != LF_NO_PROGRAM) return ACK_BAD_ARG;
  for (int i = 0; i < NUM_CH; i++) if (mask & (1 << i)) st.assign[i] = slot;
  markDirty();
  return ACK_OK;
}

static uint8_t doIdentify(uint8_t mask, uint8_t seconds) {
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
struct RxItem { uint8_t mac[6]; uint8_t len; uint8_t data[250]; };
static QueueHandle_t rxQueue;

static void onRecv(const uint8_t* mac, const uint8_t* data, int len) {  // WiFi task: copy only
  if (len <= 0 || len > 250) return;
  RxItem it;
  memcpy(it.mac, mac, 6);
  it.len = (uint8_t)len;
  memcpy(it.data, data, len);
  xQueueSend(rxQueue, &it, 0);
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
  esp_now_send(st.ctrl, (uint8_t*)&hb, sizeof(hb));
}

static void sendPairReq() {
  LfMsgPairReq r;
  fillHeader(r.h, MSG_PAIR_REQ, BCAST);
  r.fw_ver = FW_VERSION;
  r.n_ch = NUM_CH;
  esp_now_send(BCAST, (uint8_t*)&r, sizeof(r));
}

static void handlePacket(const RxItem& it) {
  if (it.len < sizeof(LfHeader)) return;
  const LfHeader* h = (const LfHeader*)it.data;
  if (h->magic != LF_MAGIC || h->ver != LF_PROTO_VER) return;
  bool toMe = memcmp(h->target, myMac, 6) == 0;
  bool toAll = memcmp(h->target, BCAST, 6) == 0;
  char ms[18]; macStr(it.mac, ms);

  if (h->type == MSG_PAIR_ACK) {
    if (!pairing || !toMe) return;
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
    case MSG_STOP:
      if (it.len >= sizeof(LfMsgChMask)) {
        const LfMsgChMask* m = (const LfMsgChMask*)it.data;
        status = h->type == MSG_PLAY ? doPlay(m->ch_mask) : doStop(m->ch_mask);
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
  char buf[200];
  for (int s = 0; s < LF_MAX_PROGS; s++) {
    lf::formatProgram(progs[s], buf, sizeof buf);
    Serial.printf("slot %d crc %04X: %s\n", s + 1, lf::programCrc(progs[s]), buf);
  }
}

static const char* HELP =
  "commands (channels 1..8 or 'all', slots 1..8):\n"
  "  status | progs | help\n"
  "  set <ch> <0-100> [fade_ms]\n"
  "  prog <slot> [loop] <steps>   e.g. prog 1 H100:10s B0-100/1s:5s H0:0\n"
  "       H<lvl>:<dur>  F<a>-<b>:<dur>  B<a>-<b>/<period>:<dur>   dur: 500ms | 2s\n"
  "  assign <ch> <slot|none>\n"
  "  play <ch> | stop <ch> | identify <ch> [seconds]\n"
  "  pair | unpair | reset (factory) | reboot\n";

static void handleLine(char* line) {
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

  if (!strcmp(cmd, "prog")) {
    int slot = a1 ? atoi(a1) : 0;
    if (slot < 1 || slot > LF_MAX_PROGS) { Serial.println("ERR slot 1..8"); return; }
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
    uint8_t slot = !strncmp(rest, "none", 4) ? LF_NO_PROGRAM : (uint8_t)(atoi(rest) - 1);
    r = doAssign(mask, slot);
  } else if (!strcmp(cmd, "play")) r = doPlay(mask);
  else if (!strcmp(cmd, "stop")) r = doStop(mask);
  else if (!strcmp(cmd, "identify")) r = doIdentify(mask, rest ? atoi(rest) : 5);
  else { Serial.println("ERR unknown command. type 'help'"); return; }
  Serial.println(r == ACK_OK ? "OK" : "ERR bad argument");
}

static void pollSerial() {
  static char buf[256];
  static size_t n = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') { buf[n] = 0; if (n) handleLine(buf); n = 0; }
    else if (n < sizeof(buf) - 1) buf[n++] = c;
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
  pinMode(STATUS_LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Serial.begin(115200);
  delay(50);
  Serial.printf("\n[boot] LED frame node fw %d, reset reason %d\n", FW_VERSION, (int)esp_reset_reason());

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
  while (xQueueReceive(rxQueue, &it, 0) == pdTRUE) handlePacket(it);
  pollSerial();
  pollButton(now);

  if (now - lastTick >= TICK_MS) { lastTick = now; updateOutputs(now); updateLed(now); }

  if (pairing) {
    if (now - pairStart >= PAIR_WINDOW_MS) { pairing = false; Serial.println("[pair] window closed, no controller"); }
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
