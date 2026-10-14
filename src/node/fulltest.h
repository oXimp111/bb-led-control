// fulltest.h — "full capability node test" instrumentation. Compiled ONLY with -DLF_FULLTEST (env node_fulltest);
// included by src/node/main.cpp, so the test runs the production command code, not a copy.
// It only observes: every received packet is printed on serial (raw hex + decoded fields), counters are kept in RAM,
// and `ft ...` serial commands report engine state and measure the real electrical level on each output pad.
// Never writes flash and never blocks the radio path for more than a pad-sampling window (<= 500 ms, only on request).
//
// Machine-readable lines start with '@' (the harness tools/fulltest.py parses them):
//   @RX,node_ms,isr_us,src_mac,len,hdr_ok,ver,type,target,seq,gap,dup,ack,hex
//   @ST,node_ms,paired,pairing,heartbeats   then @CH,... per channel, @PRG,... per non-empty slot, @END
//   @SMP,t0_ms,t1_ms,window_us,ch,duty_meas_pct,rises,freq_hz,duty_written_pct,level_pct  per channel, then @SMPEND
//   @RPT,key,value ...
#pragma once
#include <driver/gpio.h>
#include <soc/gpio_reg.h>
#include <soc/io_mux_reg.h>
#include <soc/gpio_periph.h>
#include <esp_timer.h>

struct FtCounters {
  uint32_t rx_total = 0, rx_dup = 0, rx_gap_sum = 0, rx_badhdr = 0, rx_noack = 0, qdrop = 0, qmax = 0;
  uint32_t acks = 0, acks_bad = 0, prog_write = 0, prog_skip = 0, state_save = 0, hb_sent = 0, loop_max_ms = 0;
  uint32_t rx_type[32] = {0};
};
static FtCounters ftc;

static void ft_count(int what) {
  switch (what) {
    case FTC_PROG_WRITE: ftc.prog_write++; break;
    case FTC_PROG_SKIP:  ftc.prog_skip++; break;
    case FTC_STATE_SAVE: ftc.state_save++; break;
    case FTC_HB_SENT:    ftc.hb_sent++; break;
    case FTC_QDROP:      ftc.qdrop++; break;
  }
}

static void ft_padsEnable() {   // read the pad level back through the input buffer (output signal stays routed from the LEDC)
  for (int i = 0; i < NUM_CH; i++) PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[PWM_PINS[i]]);   // only the pad's input buffer; gpio_set_direction() would re-route the pin away from the LEDC
}

// ---- packet log
static struct { bool on = false; uint16_t seq = 0; uint8_t type = 0; int ack = -1; uint32_t us = 0; } ftCur;
static bool ftHaveSeq = false;
static uint16_t ftPrevSeq = 0;

static void ft_ack(uint16_t seq, uint8_t status) {
  ftc.acks++;
  if (status != ACK_OK) ftc.acks_bad++;
  if (ftCur.on && ftCur.seq == seq) ftCur.ack = status;
}

static void ft_pre(const RxItem& it) {
  ftCur.on = false; ftCur.ack = -1;
  if (it.len < sizeof(LfHeader)) return;
  const LfHeader* h = (const LfHeader*)it.data;
  ftCur.on = true; ftCur.seq = h->seq; ftCur.type = h->type; ftCur.us = it.t_us;
}

static void ft_post(const RxItem& it) {
  static char line[640];
  ftc.rx_total++;
  uint32_t q = uxQueueMessagesWaiting(rxQueue) + 1; if (q > ftc.qmax) ftc.qmax = q;
  char ms[18]; macStr(it.mac, ms);
  int n = 0;
  if (it.len < sizeof(LfHeader)) {
    ftc.rx_badhdr++;
    n = snprintf(line, sizeof line, "@RX,%lu,%lu,%s,%u,0,,,,,,,,", (unsigned long)millis(), (unsigned long)it.t_us, ms, it.len);
  } else {
    const LfHeader* h = (const LfHeader*)it.data;
    bool hdrOk = h->magic == LF_MAGIC;
    if (!hdrOk) ftc.rx_badhdr++;
    char tg[18]; macStr(h->target, tg);
    int gap = 0, dup = 0;
    if (hdrOk && st.paired && memcmp(it.mac, st.ctrl, 6) == 0) {
      if (ftHaveSeq) {
        uint16_t d = (uint16_t)(h->seq - ftPrevSeq);
        if (d == 0) dup = 1; else if (d < 1000) { gap = d - 1; ftc.rx_gap_sum += gap; }
      }
      ftHaveSeq = true; ftPrevSeq = h->seq;
      if (dup) ftc.rx_dup++;
    }
    if (h->type < 32) ftc.rx_type[h->type]++;
    if (ftCur.ack < 0 && h->type != MSG_SYNC) ftc.rx_noack++;
    n = snprintf(line, sizeof line, "@RX,%lu,%lu,%s,%u,%d,%u,%u,%s,%u,%d,%d,", (unsigned long)millis(), (unsigned long)it.t_us, ms, it.len,
                 hdrOk ? 1 : 0, h->ver, h->type, tg, h->seq, gap, dup);
    if (ftCur.ack >= 0) n += snprintf(line + n, sizeof line - n, "%d", ftCur.ack);
    line[n++] = ',';
  }
  for (int i = 0; i < it.len && n + 3 < (int)sizeof line; i++) n += snprintf(line + n, sizeof line - n, "%02X", it.data[i]);
  line[n++] = '\n';
  Serial.write((const uint8_t*)line, n);
  ftCur.on = false;
}

// ---- state dump
static void ft_state() {
  Serial.printf("@ST,%lu,%d,%d,%lu\n", (unsigned long)millis(), st.paired ? 1 : 0, pairing ? 1 : 0, (unsigned long)ftc.hb_sent);
  uint32_t now = millis();
  for (int i = 0; i < NUM_CH; i++) {
    Serial.printf("@CH,%d,%c,%.2f,%d,%lu,%d,%lu\n", i + 1, chans[i].playing() ? 'P' : 'M', chans[i].level() * 100.0f,
                  st.assign[i] < LF_MAX_PROGS ? st.assign[i] + 1 : 0, (unsigned long)lastDuty[i],
                  (int32_t)(identifyUntil[i] - now) > 0 ? 1 : 0, (unsigned long)millis());
  }
  for (int s = 0; s < LF_MAX_PROGS; s++)
    if (progs[s].n_steps) Serial.printf("@PRG,%d,%u,%u\n", s + 1, lf::programCrc(progs[s]), progs[s].n_steps);
  Serial.println("@END");
}

// ---- pad measurement: sample all 8 output pads at once from the GPIO input register
static void ft_sample(uint32_t ms) {
  if (ms < 10) ms = 10;
  if (ms > 500) ms = 500;
  uint32_t high[LF_MAX_CH] = {0}, rises[LF_MAX_CH] = {0}, total = 0;
  uint32_t padMask = 0;
  for (int i = 0; i < NUM_CH; i++) padMask |= 1u << PWM_PINS[i];     // all PWM pins are below GPIO32
  uint32_t t0ms = millis();
  int64_t t0 = esp_timer_get_time(), tEnd = t0 + (int64_t)ms * 1000;
  uint32_t prev = REG_READ(GPIO_IN_REG) & padMask;
  for (;;) {
    for (int k = 0; k < 256; k++) {
      uint32_t v = REG_READ(GPIO_IN_REG) & padMask, r = v & ~prev;
      prev = v; total++;
      for (int i = 0; i < NUM_CH; i++) { high[i] += (v >> PWM_PINS[i]) & 1; rises[i] += (r >> PWM_PINS[i]) & 1; }
    }
    if (esp_timer_get_time() >= tEnd) break;
  }
  int64_t t1 = esp_timer_get_time();
  uint32_t t1ms = millis();
  esp_task_wdt_reset();
  float win_s = (t1 - t0) / 1e6f;
  for (int i = 0; i < NUM_CH; i++) {
    uint32_t d = lastDuty[i];
    float dw = d >= PWM_MAX_DUTY ? 100.0f : d * 100.0f / (PWM_MAX_DUTY + 1);
    Serial.printf("@SMP,%lu,%lu,%lu,%d,%.3f,%lu,%.1f,%.3f,%.2f\n", (unsigned long)t0ms, (unsigned long)t1ms, (unsigned long)(t1 - t0), i + 1,
                  total ? high[i] * 100.0f / total : 0.0f, (unsigned long)rises[i], rises[i] / win_s, dw, chans[i].level() * 100.0f);
  }
  Serial.printf("@SMPEND,%lu\n", (unsigned long)total);
}

static void ft_report() {
  Serial.printf("@RPT,rx_total,%lu\n@RPT,rx_dup,%lu\n@RPT,rx_gap_sum,%lu\n@RPT,rx_badhdr,%lu\n@RPT,rx_noack,%lu\n@RPT,queue_drops,%lu\n@RPT,queue_max,%lu\n",
                (unsigned long)ftc.rx_total, (unsigned long)ftc.rx_dup, (unsigned long)ftc.rx_gap_sum, (unsigned long)ftc.rx_badhdr,
                (unsigned long)ftc.rx_noack, (unsigned long)ftc.qdrop, (unsigned long)ftc.qmax);
  Serial.printf("@RPT,acks,%lu\n@RPT,acks_bad,%lu\n@RPT,prog_flash_writes,%lu\n@RPT,prog_writes_skipped,%lu\n@RPT,state_saves,%lu\n@RPT,heartbeats_sent,%lu\n@RPT,loop_max_ms,%lu\n",
                (unsigned long)ftc.acks, (unsigned long)ftc.acks_bad, (unsigned long)ftc.prog_write, (unsigned long)ftc.prog_skip,
                (unsigned long)ftc.state_save, (unsigned long)ftc.hb_sent, (unsigned long)ftc.loop_max_ms);
  for (int t = 0; t < 32; t++) if (ftc.rx_type[t]) Serial.printf("@RPT,rx_type_%d,%lu\n", t, (unsigned long)ftc.rx_type[t]);
  Serial.println("@RPTEND");
}

static uint32_t ftLastLoop = 0;
static void ft_loop(uint32_t now) {   // longest gap between two loop() passes
  if (ftLastLoop) { uint32_t d = now - ftLastLoop; if (d > ftc.loop_max_ms) ftc.loop_max_ms = d; }
  ftLastLoop = now;
}

// `ft state | ft sample <ms> | ft report | ft clear | ft ping`   returns true if the line was ours
static bool ft_cmd(const char* line) {
  if (strncmp(line, "ft ", 3) != 0 && strcmp(line, "ft") != 0) return false;
  const char* a = line + 2; while (*a == ' ') a++;
  if (!strncmp(a, "state", 5)) ft_state();
  else if (!strncmp(a, "sample", 6)) ft_sample((uint32_t)atoi(a + 6));
  else if (!strncmp(a, "report", 6)) ft_report();
  else if (!strncmp(a, "clear", 5)) { ftc = FtCounters(); ftHaveSeq = false; ftLastLoop = 0; Serial.println("@OK,clear"); }
  else if (!strncmp(a, "ping", 4)) Serial.printf("@PONG,%lu\n", (unsigned long)millis());
  else Serial.println("@ERR,ft state|sample <ms>|report|clear|ping");
  return true;
}
