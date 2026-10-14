#include "engine.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

namespace lf {

static float pct(uint8_t v) { return (v > 100 ? 100 : v) / 100.0f; }

uint32_t programTotalMs(const LfProgram& p) {
  uint64_t t = 0;
  for (uint8_t i = 0; i < p.n_steps && i < LF_MAX_STEPS; i++) t += p.steps[i].duration_ms;
  return t > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)t;  // saturate (only reachable with absurd durations)
}

float stepEndLevel(const LfStep& s) {
  switch (s.type) {
    case STEP_FADE: return pct(s.b);
    default:        return pct(s.a);  // HOLD ends at a; BLINK ends back at a
  }
}

static float stepLevelAt(const LfStep& s, uint32_t t) {
  switch (s.type) {
    case STEP_HOLD: return pct(s.a);
    case STEP_FADE: {
      if (s.duration_ms == 0) return pct(s.b);
      float f = (float)t / (float)s.duration_ms;
      return pct(s.a) + (pct(s.b) - pct(s.a)) * f;
    }
    case STEP_BLINK: {
      if (s.period_ms == 0) return pct(s.a);
      float ph = (float)(t % s.period_ms) / (float)s.period_ms;  // 0..1
      float tri = ph < 0.5f ? 2.0f * ph : 2.0f - 2.0f * ph;     // 0 ->1 ->0
      return pct(s.a) + (pct(s.b) - pct(s.a)) * tri;
    }
  }
  return 0;
}

float programLevelAt(const LfProgram& p, uint32_t t) {
  uint8_t n = p.n_steps > LF_MAX_STEPS ? LF_MAX_STEPS : p.n_steps;
  if (n == 0) return 0;
  for (uint8_t i = 0; i < n; i++) {
    const LfStep& s = p.steps[i];
    if (t < s.duration_ms) return stepLevelAt(s, t);
    t -= s.duration_ms;
  }
  return stepEndLevel(p.steps[n - 1]);  // finished: hold last step's end level
}

void normalizeProgram(LfProgram& p) {
  if (p.n_steps > LF_MAX_STEPS) p.n_steps = LF_MAX_STEPS;
  p.flags &= LF_PROG_LOOP;
  for (uint8_t i = 0; i < LF_MAX_STEPS; i++) {
    LfStep& s = p.steps[i];
    if (i >= p.n_steps) { memset(&s, 0, sizeof(s)); continue; }
    if (s.a > 100) s.a = 100;
    if (s.b > 100) s.b = 100;
    if (s.type == STEP_HOLD) { s.b = 0; s.period_ms = 0; }
    if (s.type == STEP_FADE) s.period_ms = 0;
  }
}

bool validateProgram(const LfProgram& p) {
  if (p.n_steps > LF_MAX_STEPS) return false;
  for (uint8_t i = 0; i < p.n_steps; i++) {
    const LfStep& s = p.steps[i];
    if (s.type > STEP_BLINK || s.a > 100 || s.b > 100) return false;
    if (s.type == STEP_BLINK && s.period_ms == 0) return false;
  }
  return true;
}

uint16_t crc16(const uint8_t* d, size_t len) {  // CRC-16/CCITT-FALSE
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)d[i] << 8;
    for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
  }
  return crc;
}

uint16_t programCrc(const LfProgram& p) {
  if (p.n_steps == 0) return 0;
  uint16_t c = crc16((const uint8_t*)&p, sizeof(p));
  return c == 0 ? 1 : c;  // 0 reserved for "empty"
}

uint32_t levelToDuty(float level, uint32_t max_duty) {
  if (level <= 0) return 0;
  if (level >= 1) return max_duty;
  return (uint32_t)lroundf(powf(level, 2.2f) * (float)max_duty);
}

// ---------------- Channel ----------------
void Channel::setManual(float target, uint32_t fade_ms, uint32_t now) {
  if (target < 0) target = 0;
  if (target > 1) target = 1;
  mode_ = MANUAL;
  prog_ = nullptr;
  from_ = cur_;
  to_ = target;
  fstart_ = now;
  fdur_ = fade_ms;
  if (fade_ms == 0) cur_ = target;
}

void Channel::play(const LfProgram* p, uint32_t now, uint32_t offset_ms) {
  if (!p) return;
  mode_ = PROGRAM;
  prog_ = p;
  start_ = now - offset_ms;  // unsigned: elapsed (now - start_) == offset_ms, also across millis() wrap
}

bool Channel::sync(uint32_t t_ms, uint32_t now) {
  if (mode_ != PROGRAM || !prog_) return false;
  uint32_t total = programTotalMs(*prog_);
  bool loop = (prog_->flags & LF_PROG_LOOP) && total > 0;
  uint32_t cur = now - start_, target = t_ms;
  int64_t drift;
  if (loop) {  // compare positions inside one cycle, shortest way round
    cur %= total;
    target %= total;
    drift = (int64_t)target - (int64_t)cur;
    if (drift > (int64_t)(total / 2)) drift -= total;
    else if (drift < -(int64_t)(total / 2)) drift += total;
  } else {
    drift = (int64_t)target - (int64_t)cur;
  }
  if (drift <= LF_SYNC_TOLERANCE_MS && drift >= -(int64_t)LF_SYNC_TOLERANCE_MS) return false;
  start_ = now - target;
  return true;
}

void Channel::stop(uint32_t now) {
  if (mode_ == PROGRAM) setManual(cur_, 0, now);
}

float Channel::update(uint32_t now) {
  if (mode_ == PROGRAM && prog_) {
    uint32_t total = programTotalMs(*prog_);
    uint32_t t = now - start_;  // unsigned math: safe across millis() wrap
    if ((prog_->flags & LF_PROG_LOOP) && total > 0 && t >= total) {
      start_ += (t / total) * total;  // re-anchor so t stays small
      t = now - start_;
    }
    cur_ = programLevelAt(*prog_, t);
  } else if (fdur_ > 0) {
    uint32_t t = now - fstart_;
    if (t >= fdur_) { cur_ = to_; fdur_ = 0; }
    else cur_ = from_ + (to_ - from_) * ((float)t / (float)fdur_);
  }
  return cur_;
}

// ---------------- text format ----------------
static void setErr(char* err, size_t n, const char* msg, const char* at) {
  if (err && n) snprintf(err, n, "%s at '%.12s'", msg, at ? at : "");
}

static bool parseLevel(const char*& s, uint8_t& out) {
  char* end;
  long v = strtol(s, &end, 10);
  if (end == s || v < 0 || v > 100) return false;
  out = (uint8_t)v;
  s = end;
  return true;
}

static bool parseDur(const char*& s, uint32_t& out) {
  char* end;
  double v = strtod(s, &end);
  if (end == s || !(v >= 0)) return false;  // also rejects NaN
  if (end[0] == 'm' && end[1] == 's') { end += 2; }
  else if (end[0] == 's') { v *= 1000.0; end += 1; }
  if (v > 4294967295.0) return false;  // uint32 ms
  out = (uint32_t)(v + 0.5);
  s = end;
  return true;
}

bool parseProgram(const char* text, LfProgram& out, char* err, size_t errlen) {
  memset(&out, 0, sizeof(out));
  const char* s = text;
  while (*s) {
    while (*s == ' ' || *s == ',' || *s == '\t') s++;
    if (!*s || *s == '\r' || *s == '\n') break;
    const char* tok = s;
    if (strncmp(s, "loop", 4) == 0 && (s[4] == 0 || strchr(" ,\t\r\n", s[4]))) {
      out.flags |= LF_PROG_LOOP; s += 4; continue;
    }
    if (out.n_steps >= LF_MAX_STEPS) { setErr(err, errlen, "max 24 steps", tok); return false; }
    LfStep& st = out.steps[out.n_steps];
    char c = (char)toupper((unsigned char)*s++);
    if (c == 'H') {
      st.type = STEP_HOLD;
      if (!parseLevel(s, st.a)) { setErr(err, errlen, "bad level", tok); return false; }
    } else if (c == 'F' || c == 'B') {
      st.type = c == 'F' ? STEP_FADE : STEP_BLINK;
      if (!parseLevel(s, st.a) || *s++ != '-' || !parseLevel(s, st.b)) {
        setErr(err, errlen, "bad levels (use a-b)", tok); return false;
      }
      if (c == 'B') {
        uint32_t per;
        if (*s++ != '/' || !parseDur(s, per) || per == 0 || per > 65535) {
          setErr(err, errlen, "bad blink period (1..65535 ms)", tok); return false;
        }
        st.period_ms = (uint16_t)per;
      }
    } else {
      setErr(err, errlen, "unknown step (H/F/B)", tok); return false;
    }
    uint32_t dur;  // local: st is packed, a reference to its field is undefined behaviour
    if (*s++ != ':' || !parseDur(s, dur)) {
      setErr(err, errlen, "bad duration", tok); return false;
    }
    st.duration_ms = dur;
    if (*s && *s != ' ' && *s != ',' && *s != '\r' && *s != '\n') {
      setErr(err, errlen, "junk after step", tok); return false;
    }
    out.n_steps++;
  }
  normalizeProgram(out);
  return true;
}

static int fmtDur(char* b, size_t n, uint32_t ms) {
  if (ms != 0 && ms % 1000 == 0) return snprintf(b, n, "%lus", (unsigned long)(ms / 1000));
  return snprintf(b, n, "%lums", (unsigned long)ms);
}

size_t formatProgram(const LfProgram& p, char* buf, size_t len) {
  size_t o = 0;
  auto room = [&]() { return o < len ? len - o : 0; };
  auto adv = [&](int w) { if (w > 0) o += (size_t)w; if (o > len) o = len; };
  if (len) buf[0] = 0;
  if (p.flags & LF_PROG_LOOP) adv(snprintf(buf + o, room(), "loop "));
  for (uint8_t i = 0; i < p.n_steps && i < LF_MAX_STEPS; i++) {
    const LfStep& s = p.steps[i];
    if (s.type == STEP_HOLD) adv(snprintf(buf + o, room(), "H%u:", s.a));
    else if (s.type == STEP_FADE) adv(snprintf(buf + o, room(), "F%u-%u:", s.a, s.b));
    else {
      adv(snprintf(buf + o, room(), "B%u-%u/", s.a, s.b));
      adv(fmtDur(buf + o, room(), s.period_ms));
      adv(snprintf(buf + o, room(), ":"));
    }
    adv(fmtDur(buf + o, room(), s.duration_ms));
    if (i + 1 < p.n_steps) adv(snprintf(buf + o, room(), " "));
  }
  if (p.n_steps == 0) adv(snprintf(buf + o, room(), "(empty)"));
  return o;
}

}  // namespace lf
