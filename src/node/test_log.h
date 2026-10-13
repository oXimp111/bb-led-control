// test_log.h — node TEST instrumentation. Included by main.cpp only when built with -DLF_TEST_LOG (env node_test).
// Pure observation: it never changes what the node does with a packet. Logs to LittleFS /rxlog.csv,
// keeps counters in NVS, adds serial commands report | log | clear | outtest.
// Included AFTER the node state (st, progs, chans, lastDuty, myMac) so it can read them.
#pragma once
#include <LittleFS.h>
#include <esp_wifi.h>
#include "lf_csv.h"

static_assert(sizeof(LfMsgProg) <= 250, "MSG_PROG with 24 steps must fit one ESP-NOW v1 packet (250 bytes)");

static void updateOutputs(uint32_t now);   // main.cpp
static void handleLine(char* line);        // main.cpp
static void macStr(const uint8_t* m, char* out);   // main.cpp

#define TL_FILE        "/rxlog.csv"
#define TL_FILE_OLD    "/rxlog.old"
#define TL_MAX_BYTES   (256 * 1024)
#define TL_N_TYPES     20

// ------------------------------------------------------------ counters (persisted in NVS)
struct TlCounters {
  uint32_t layout;
  uint32_t boots;
  uint32_t raw_rx;          // every ESP-NOW frame handed to our callback
  uint32_t queue_drops;     // frames lost because the 16-slot rx queue was full
  uint32_t rx_total;        // packets taken from the queue
  uint32_t rx_type[TL_N_TYPES];
  uint32_t bad_len;         // known type, version ok, shorter than the struct
  uint32_t bad_hdr;         // shorter than a header, or wrong magic
  uint32_t bad_ver;         // right magic, other LF_PROTO_VER
  uint32_t ignored;         // valid header but not from our controller / not addressed to us
  uint32_t seq_rx;          // packets from our controller that took part in the seq check
  uint32_t gaps;            // sum of missing seq numbers
  uint32_t dups;            // same seq as the previous packet (retries)
  uint32_t acks_sent;
  uint32_t progs_stored;    // MSG_PROG accepted and applied
  uint32_t out_checks;      // channel rows logged with a readback
  uint32_t wr_mismatch;     // LEDC readback != value just written (checked on every duty change)
  uint32_t log_mismatch;    // same, found by a logged readback
};
static TlCounters tlC;
static Preferences tlPrefs;
static bool        tlNvs = false, tlFs = false, tlCDirty = false;
static char        tlBuf[4096];
static size_t      tlBufN = 0;
static uint32_t    tlLastPersist = 0, tlLastFlush = 0, tlLastOut = 0;
static bool        tlBootReported = false;
static uint32_t    tlWrMismatchRows = 0;

// ------------------------------------------------------------ log file
static void tlWriteHeader(File& f) { f.print(LF_CSV_HEADER); }

static void tlFlush() {
  if (!tlFs || !tlBufN) { tlBufN = 0; return; }
  File f = LittleFS.open(TL_FILE, FILE_APPEND);
  if (f) { f.write((const uint8_t*)tlBuf, tlBufN); f.close(); }
  tlBufN = 0;
  File g = LittleFS.open(TL_FILE, FILE_READ);
  size_t sz = g ? g.size() : 0;
  if (g) g.close();
  if (sz > TL_MAX_BYTES) {
    LittleFS.remove(TL_FILE_OLD);
    LittleFS.rename(TL_FILE, TL_FILE_OLD);
    File n = LittleFS.open(TL_FILE, FILE_WRITE);
    if (n) { tlWriteHeader(n); n.close(); }
  }
  tlLastFlush = millis();
}

static void tlRow(const LfLogRow& r) {
  char line[320];
  size_t n = lfCsvRow(line, sizeof line, r);
  if (tlBufN + n > sizeof tlBuf) tlFlush();
  memcpy(tlBuf + tlBufN, line, n);
  tlBufN += n;
}

static void tlPersist() {
  if (tlNvs) tlPrefs.putBytes("c", &tlC, sizeof tlC);
  tlLastPersist = millis();
  tlCDirty = false;
}

// ------------------------------------------------------------ RSSI (promiscuous management frames; ESP-NOW = action frames)
struct TlRssi { uint8_t mac[6]; int8_t rssi; bool used; };
static TlRssi tlRssi[4];
static uint8_t tlRssiNext = 0;

static void tlPromisc(void* buf, wifi_promiscuous_pkt_type_t type) {  // WiFi task
  if (type != WIFI_PKT_MGMT) return;
  const wifi_promiscuous_pkt_t* p = (const wifi_promiscuous_pkt_t*)buf;
  if (p->rx_ctrl.sig_len < 16 || p->payload[0] != 0xD0) return;   // action frame only
  const uint8_t* src = p->payload + 10;                            // address 2 = transmitter
  for (auto& e : tlRssi) if (e.used && !memcmp(e.mac, src, 6)) { e.rssi = p->rx_ctrl.rssi; return; }
  TlRssi& e = tlRssi[tlRssiNext++ & 3];
  memcpy(e.mac, src, 6); e.rssi = p->rx_ctrl.rssi; e.used = true;
}

static int32_t tlRssiOf(const uint8_t* mac) {
  for (auto& e : tlRssi) if (e.used && !memcmp(e.mac, mac, 6)) return e.rssi;
  return LF_NA;
}

// ------------------------------------------------------------ output check
// The Arduino core 2.x turns duty == max (16383) into 16384 = constant high, so that is the correct readback for 100 %.
static uint32_t tlExpectRead(uint32_t written) { return written == PWM_MAX_DUTY ? written + 1 : written; }

static void tlOutRow(const char* event, int i, int shownLevel, const char* note) {
  LfLogRow r;
  r.ms = millis(); r.event = event;
  r.ch = i + 1;
  r.level_pct = shownLevel;
  r.duty_w = (int32_t)lastDuty[i];
  r.duty_r = (int32_t)ledcRead(i);
  r.duty_exp = (int32_t)lf::levelToDuty(chans[i].level(), PWM_MAX_DUTY);
  r.mismatch = (uint32_t)r.duty_r != tlExpectRead((uint32_t)r.duty_w);
  r.note = note;
  tlC.out_checks++;
  if (r.mismatch) tlC.log_mismatch++;
  tlCDirty = true;
  tlRow(r);
}

// All channels (or only PLAYING ones) with the engine level, duty written and duty read back from the LEDC peripheral.
static void tlLogOutputs(const char* note, bool playingOnly) {
  for (int i = 0; i < NUM_CH; i++) {
    if (playingOnly && !chans[i].playing()) continue;
    tlOutRow("OUT", i, (int)lroundf(chans[i].level() * 100), note);
  }
}

static void tlAfterWrite(int i, uint32_t d) {   // called right after every ledcWrite in updateOutputs
  uint32_t back = ledcRead(i);
  if (back == tlExpectRead(d)) return;
  tlC.wr_mismatch++; tlCDirty = true;
  if (tlWrMismatchRows++ < 100) {
    LfLogRow r;
    r.ms = millis(); r.event = "OUT"; r.ch = i + 1;
    r.level_pct = (int)lroundf(chans[i].level() * 100);
    r.duty_w = (int32_t)d; r.duty_r = (int32_t)back;
    r.duty_exp = (int32_t)lf::levelToDuty(chans[i].level(), PWM_MAX_DUTY);
    r.mismatch = 1; r.note = "readback differs right after write";
    tlRow(r);
  }
}

// ------------------------------------------------------------ packet log
static struct {
  int32_t ack;               // status we sent for the packet being handled, LF_NA = none
  bool    haveSeq;
  uint16_t lastSeq;
  uint8_t  seqMac[6];
} tlRx = {LF_NA, false, 0, {0}};

static void tlOnAck(uint16_t, uint8_t status) { tlRx.ack = status; tlC.acks_sent++; tlCDirty = true; }

static void tlRxBegin() { tlRx.ack = LF_NA; }

// Called after handlePacket() ran. pairedBefore = st.paired before it ran.
static void tlRxEnd(const uint8_t* mac, const uint8_t* data, int len, bool pairedBefore) {
  LfLogRow r;
  char nb[12], note[48] = "";
  r.ms = millis(); r.event = "RX"; r.mac = mac; r.len = len;
  r.rssi = tlRssiOf(mac);
  r.ack = tlRx.ack;
  tlC.rx_total++; tlCDirty = true;

  const LfHeader* h = (const LfHeader*)data;
  bool magicOk = len >= (int)sizeof(LfHeader) && h->magic == LF_MAGIC;
  if (!magicOk) {
    tlC.bad_hdr++;
    r.type = "?"; r.hdr_ok = 0; r.note = len < (int)sizeof(LfHeader) ? "short header" : "bad magic";
    tlRow(r); tlFlush();
    return;
  }
  bool verOk = h->ver == LF_PROTO_VER;
  r.type = lfMsgName(h->type, nb, sizeof nb);
  r.hdr_ok = verOk;
  r.seq = h->seq;
  r.exp_len = lfExpectedLen(h->type);
  if (h->type < TL_N_TYPES) tlC.rx_type[h->type]++;
  if (!verOk) { tlC.bad_ver++; snprintf(note, sizeof note, "protocol v%u", h->ver); }
  else if (r.exp_len >= 0 && len < r.exp_len) { tlC.bad_len++; strcpy(note, "short packet"); }

  // seq statistics: every packet from our controller, including SYNC (the controller's counter is shared by all commands)
  bool nowCtrl = st.paired && !memcmp(mac, st.ctrl, 6);
  if (h->type == MSG_PAIR_ACK && !pairedBefore && st.paired && nowCtrl) {
    tlRx.haveSeq = false;   // new pairing: restart statistics
  }
  if (nowCtrl) {
    if (!tlRx.haveSeq || memcmp(tlRx.seqMac, mac, 6)) {
      tlRx.haveSeq = true; tlRx.lastSeq = h->seq; memcpy(tlRx.seqMac, mac, 6);
      r.gap = 0; r.dup = 0;
    } else {
      uint16_t d = (uint16_t)(h->seq - tlRx.lastSeq);
      tlC.seq_rx++;
      if (d == 0) { r.dup = 1; r.gap = 0; tlC.dups++; }
      else if (d < 0x8000) { r.dup = 0; r.gap = d - 1; tlC.gaps += d - 1; tlRx.lastSeq = h->seq; }
      else { r.dup = 0; r.gap = 0; strcpy(note, "out of order"); }
    }
  } else if (h->type != MSG_PAIR_ACK) {
    tlC.ignored++; if (!note[0]) strcpy(note, "ignored (not from my controller)");
  }
  bool dup = r.dup == 1;

  if (h->type == MSG_PROG && len >= (int)sizeof(LfMsgProg)) {
    LfMsgProg m; memcpy(&m, data, sizeof m);
    r.slot = m.slot + 1; r.n_steps = m.prog.n_steps; r.loop = (m.prog.flags & LF_PROG_LOOP) ? 1 : 0;
    if (m.slot < LF_MAX_PROGS) r.crc = lf::programCrc(progs[m.slot]);
    if (tlRx.ack == ACK_OK && !dup) tlC.progs_stored++;
  } else if (h->type == MSG_ASSIGN && len >= (int)sizeof(LfMsgAssign)) {
    r.slot = ((const LfMsgAssign*)data)->slot == LF_NO_PROGRAM ? 0 : ((const LfMsgAssign*)data)->slot + 1;
  }
  if (h->type == MSG_PAIR_ACK) {
    strcpy(note, !pairedBefore && st.paired ? "accepted, controller stored" : "not applied");
  }
  r.note = note;
  tlRow(r);

  if (h->type == MSG_PAIR_ACK && !pairedBefore && st.paired) {
    LfLogRow p; p.ms = millis(); p.event = "PAIR"; p.mac = st.ctrl; p.note = "PAIR_ACK received; controller MAC stored";
    tlRow(p);
  }

  bool applied = tlRx.ack == ACK_OK && !dup;
  if (applied && (h->type == MSG_PROG || h->type == MSG_ASSIGN || h->type == MSG_PLAY ||
                  h->type == MSG_SET || h->type == MSG_STOP)) {
    updateOutputs(millis());   // same path as the 10 ms tick, so the row shows the result of this packet
    snprintf(note, sizeof note, "after %s", r.type);
    tlLogOutputs(note, false);
  }
  tlFlush();
}

static void tlOnPairOpen()   { LfLogRow p; p.ms = millis(); p.event = "PAIR"; p.note = "window open 60 s"; tlRow(p); tlFlush(); }
static void tlOnPairClosed() { LfLogRow p; p.ms = millis(); p.event = "PAIR"; p.note = "window closed, no controller"; tlRow(p); tlFlush(); }

static void tlRawRx()   { tlC.raw_rx++; }      // WiFi task
static void tlDropRx()  { tlC.queue_drops++; } // WiFi task

// ------------------------------------------------------------ report / log / clear / outtest
static size_t tlFileSize(const char* path) {
  if (!LittleFS.exists(path)) return 0;
  File f = LittleFS.open(path, FILE_READ);
  size_t n = f ? f.size() : 0;
  if (f) f.close();
  return n;
}

static void tlReport() {
  tlFlush();
  tlPersist();
  char m[18]; macStr(myMac, m);
  uint32_t good = tlC.seq_rx + tlC.gaps;
  Serial.println("===== NODE TEST REPORT =====");
  Serial.printf("node %s  fw %d  proto %d  uptime %lus  boots %lu\n", m, FW_VERSION, LF_PROTO_VER, millis() / 1000, (unsigned long)tlC.boots);
  Serial.printf("heap free %u  min free %u\n", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
  if (st.paired) { macStr(st.ctrl, m); Serial.printf("paired to %s\n", m); } else Serial.println("not paired");
  Serial.printf("log: fs %s  %s %u B  %s %u B\n", tlFs ? "ok" : "FAIL", TL_FILE, (unsigned)tlFileSize(TL_FILE), TL_FILE_OLD, (unsigned)tlFileSize(TL_FILE_OLD));
  Serial.printf("rx: raw %lu  taken %lu  queue_drops %lu\n", (unsigned long)tlC.raw_rx, (unsigned long)tlC.rx_total, (unsigned long)tlC.queue_drops);
  Serial.print("rx per type:");
  for (int t = 0; t < TL_N_TYPES; t++) if (tlC.rx_type[t]) { char nb[12]; Serial.printf(" %s=%lu", lfMsgName(t, nb, sizeof nb), (unsigned long)tlC.rx_type[t]); }
  Serial.println();
  Serial.printf("bad_len %lu  bad_hdr %lu  bad_ver %lu  ignored %lu\n", (unsigned long)tlC.bad_len, (unsigned long)tlC.bad_hdr, (unsigned long)tlC.bad_ver, (unsigned long)tlC.ignored);
  Serial.printf("seq: checked %lu  gaps(missing) %lu  dups %lu  loss %.2f%%\n", (unsigned long)tlC.seq_rx, (unsigned long)tlC.gaps, (unsigned long)tlC.dups,
                good ? 100.0 * tlC.gaps / good : 0.0);
  Serial.printf("acks_sent %lu  programs_stored %lu\n", (unsigned long)tlC.acks_sent, (unsigned long)tlC.progs_stored);
  Serial.printf("output: checks %lu  write_mismatch %lu  log_mismatch %lu\n", (unsigned long)tlC.out_checks, (unsigned long)tlC.wr_mismatch, (unsigned long)tlC.log_mismatch);
  for (int s = 0; s < LF_MAX_PROGS; s++)
    Serial.printf("slot %d crc %04X steps %u%s\n", s + 1, lf::programCrc(progs[s]), progs[s].n_steps, (progs[s].flags & LF_PROG_LOOP) ? " loop" : "");
  for (int i = 0; i < NUM_CH; i++) {
    uint8_t s = st.assign[i];
    Serial.printf("ch%d gpio%d level %3d%% %s slot %d duty_w %u duty_r %u freq %u\n", i + 1, PWM_PINS[i], (int)lroundf(chans[i].level() * 100),
                  chans[i].playing() ? "PLAYING" : "manual ", s < LF_MAX_PROGS ? s + 1 : 0, (unsigned)lastDuty[i], (unsigned)ledcRead(i), (unsigned)ledcReadFreq(i));
  }
  Serial.println("===== END REPORT =====");
}

static void tlPrintLog(const char* arg) {
  tlFlush();
  File f = LittleFS.open(TL_FILE, FILE_READ);
  if (!f) { Serial.println("ERR no log file"); return; }
  bool all = arg && !strcmp(arg, "all");
  long want = all ? 0 : (arg && atol(arg) > 0 ? atol(arg) : 50);
  long total = 0;
  uint8_t b[256];
  int n;
  while ((n = f.read(b, sizeof b)) > 0) { for (int i = 0; i < n; i++) if (b[i] == '\n') total++; esp_task_wdt_reset(); }
  long skip = all ? 1 : (total > want + 1 ? total - want : 1);   // row 0 is the header and is always shown
  f.seek(0);
  long line = 0;
  while ((n = f.read(b, sizeof b)) > 0) {
    for (int i = 0; i < n; i++) {
      if (line >= skip || line == 0) Serial.write(b[i]);
      if (b[i] == '\n') line++;
    }
    esp_task_wdt_reset();
  }
  f.close();
}

static void tlClear() {
  tlBufN = 0;
  LittleFS.remove(TL_FILE);
  LittleFS.remove(TL_FILE_OLD);
  File f = LittleFS.open(TL_FILE, FILE_WRITE);
  if (f) { tlWriteHeader(f); f.close(); }
  uint32_t boots = tlC.boots;
  memset(&tlC, 0, sizeof tlC);
  tlC.layout = 1; tlC.boots = boots;
  tlWrMismatchRows = 0;
  tlRx.haveSeq = false;
  tlPersist();
  Serial.println("OK log and counters cleared");
}

// channel 1..8 x 0/25/50/75/100 %, 1 s each, through the normal engine + LEDC path
static void tlOutTest() {
  static const int steps[] = {0, 25, 50, 75, 100};
  lf::Channel saved[NUM_CH];
  for (int i = 0; i < NUM_CH; i++) saved[i] = chans[i];
  int bad = 0;
  Serial.println("outtest: 8 channels x 5 levels, about 40 s. Other commands wait.");
  for (int i = 0; i < NUM_CH; i++) {
    for (int lv : steps) {
      chans[i].setManual(lv / 100.0f, 0, millis());
      uint32_t t0 = millis();
      while (millis() - t0 < 1000) { updateOutputs(millis()); esp_task_wdt_reset(); delay(5); }
      char note[24]; snprintf(note, sizeof note, "f=%uHz", (unsigned)ledcReadFreq(i));
      tlOutRow("OUTTEST", i, lv, note);
      uint32_t w = lastDuty[i], rd = ledcRead(i), ex = lf::levelToDuty(lv / 100.0f, PWM_MAX_DUTY);
      bool ok = rd == tlExpectRead(w) && w == ex;
      if (!ok) bad++;
      Serial.printf("ch%d %3d%%  written %5u  readback %5u  expected %5u  %s\n", i + 1, lv, (unsigned)w, (unsigned)rd, (unsigned)ex, ok ? "ok" : "MISMATCH");
    }
    chans[i] = saved[i];
    updateOutputs(millis());
  }
  tlFlush();
  Serial.printf("outtest done: %d mismatch(es)\n", bad);
}

static const char* TL_HELP =
  "test commands:\n"
  "  report            summary block (paste it to Claude)\n"
  "  log [n|all]       last n CSV lines (default 50) or the whole file\n"
  "  clear             wipe log and counters\n"
  "  outtest           step every channel 0/25/50/75/100 %, compare written vs readback duty\n";

// serial entry point: test commands here, everything else goes to the normal handler
static void tlSerial(char* line) {
  char first[16] = "", arg[16] = "";
  sscanf(line, "%15s %15s", first, arg);
  if (!strcmp(first, "report")) { tlReport(); return; }
  if (!strcmp(first, "log")) { tlPrintLog(arg[0] ? arg : nullptr); return; }
  if (!strcmp(first, "clear")) { tlClear(); return; }
  if (!strcmp(first, "outtest")) { tlOutTest(); return; }
  handleLine(line);
  if (!strcmp(first, "help")) Serial.print(TL_HELP);
  else if (!strcmp(first, "set") || !strcmp(first, "prog") || !strcmp(first, "assign") ||
           !strcmp(first, "play") || !strcmp(first, "stop")) {
    updateOutputs(millis());
    tlLogOutputs("after serial cmd", false);
    tlFlush();
  }
}

// ------------------------------------------------------------ lifecycle hooks
static void tlInit() {   // after state restore, before radio
  memset(&tlC, 0, sizeof tlC);
  tlC.layout = 1;
  tlNvs = tlPrefs.begin("lftest", false);
  if (tlNvs) {
    TlCounters tmp;
    if (tlPrefs.getBytesLength("c") == sizeof tmp) {
      tlPrefs.getBytes("c", &tmp, sizeof tmp);
      if (tmp.layout == 1) tlC = tmp;
    }
  }
  tlFs = LittleFS.begin(true);
  if (tlFs && !LittleFS.exists(TL_FILE)) {
    File f = LittleFS.open(TL_FILE, FILE_WRITE);
    if (f) { tlWriteHeader(f); f.close(); }
  }
  tlC.boots++;
  LfLogRow r; r.ms = millis(); r.event = "BOOT";
  char note[64]; snprintf(note, sizeof note, "reset reason %d; fs %s; paired %d", (int)esp_reset_reason(), tlFs ? "ok" : "FAIL", st.paired);
  r.note = note;
  tlRow(r); tlFlush(); tlPersist();
  Serial.printf("[test] node_test build: log %s, counters %s. Type 'help'.\n", tlFs ? "ok" : "FAILED", tlNvs ? "ok" : "FAILED");
}

static void tlRadioReady() {   // after esp_now_init: RSSI of ESP-NOW action frames
  wifi_promiscuous_filter_t f = {};
  f.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
  esp_wifi_set_promiscuous_filter(&f);
  esp_wifi_set_promiscuous_rx_cb(tlPromisc);
  esp_wifi_set_promiscuous(true);
}

static void tlTick(uint32_t now) {   // every loop()
  if (!tlBootReported && now >= 5000) { tlBootReported = true; tlReport(); }
  if (now - tlLastOut >= 1000) {
    tlLastOut = now;
    bool any = false;
    for (int i = 0; i < NUM_CH; i++) any |= chans[i].playing();
    if (any) tlLogOutputs("playing", true);
  }
  if (tlBufN && now - tlLastFlush >= 2000) tlFlush();
  if (tlCDirty && now - tlLastPersist >= 10000) tlPersist();
}
