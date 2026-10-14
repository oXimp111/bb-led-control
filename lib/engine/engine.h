// engine.h — program player + text format. Pure C++ (no Arduino), so it is
// unit-tested on a PC (test/engine_test.cpp) and shared by node + control center.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "protocol.h"

namespace lf {

// ---------- program math ----------
uint32_t programTotalMs(const LfProgram& p);
float    stepEndLevel(const LfStep& s);                    // 0..1
float    programLevelAt(const LfProgram& p, uint32_t t_ms, float entry = 0); // 0..1, t past end = end level. entry = level a leading KEEP step holds
void     normalizeProgram(LfProgram& p);                    // zero unused steps, clamp
bool     validateProgram(const LfProgram& p);
uint16_t crc16(const uint8_t* data, size_t len);
uint16_t programCrc(const LfProgram& p);                    // 0 for empty program
uint32_t levelToDuty(float level, uint32_t max_duty);      // gamma 2.2

// ---------- one PWM channel ----------
class Channel {
 public:
  void  setManual(float target, uint32_t fade_ms, uint32_t now); // stops program
  void  play(const LfProgram* p, uint32_t now, uint32_t offset_ms = 0); // start as if offset_ms already elapsed (loop wraps)
  bool  sync(uint32_t t_ms, uint32_t now);                       // PLAYING only: elapsed := t_ms if off by > LF_SYNC_TOLERANCE_MS; true if moved
  void  stop(uint32_t now);                                      // hold current level
  float update(uint32_t now);                                    // call every tick
  bool  playing() const { return mode_ == PROGRAM; }
  float level() const { return cur_; }
  float manualTarget() const { return to_; }

 private:
  enum Mode { MANUAL, PROGRAM } mode_ = MANUAL;
  const LfProgram* prog_ = nullptr;
  uint32_t start_ = 0;
  float from_ = 0, to_ = 0, cur_ = 0, entry_ = 0;
  uint32_t fstart_ = 0, fdur_ = 0;
};

// longest possible formatProgram() output: "loop " + 24 * "B100-100/65535ms:4294967295ms "
#define LF_PROG_TEXT_MAX 768

// ---------- text format (serial commands, CLAUDE.md) ----------
// "[loop] H100:10s B0-100/1s:5s F100-0:3s H0:0"
//   H<lvl>:<dur>              hold
//   F<a>-<b>:<dur>            fade a->b
//   B<a>-<b>/<period>:<dur>   blink a<->b
//   K:<dur>                   keep the current level (level when the program started, or the previous step's end)
//   durations: 500ms | 1.5s | 2000 (bare = ms)
bool   parseProgram(const char* text, LfProgram& out, char* err, size_t errlen);
size_t formatProgram(const LfProgram& p, char* buf, size_t buflen);

}  // namespace lf
