#include "testload.h"
#include <stdio.h>

namespace lf {

// 24 steps. Every step type, levels 0 and 100, fades up/down, blink periods 1 / 100 / 65535 ms, a 0 ms hold,
// equal-level fade/blink, a 1 ms fade and a 600000 ms step. The first ~31 s (+ chase offset) are easy to watch.
static const char* const TEMPLATE =
  "loop H0:%lums "                       //  1 chase offset (0 ms on channel 1)
  "H100:1s H0:1s "                       //  2-3 pulse
  "F0-100:2s F100-0:2s "                 //  4-5 fade up, fade down
  "B0-100/1s:4s "                        //  6 blink, 1 s period
  "H100:0ms H0:500ms "                   //  7-8 zero-length hold
  "B0-100/100ms:2s H0:500ms "            //  9-10 fast blink
  "B100-0/65535ms:3s "                   // 11 blink with the longest period (slow dim)
  "H100:1s "                             // 12
  "B0-100/1ms:1s "                       // 13 blink with the shortest period (faster than the PWM)
  "F100-100:500ms F0-0:500ms "           // 14-15 fades with equal ends
  "F0-100:3s H100:3s F100-0:3s H0:2s "   // 16-19
  "B50-50/1s:1s "                        // 20 blink with equal levels
  "F100-0:1ms "                          // 21 1 ms fade
  "H100:1s H0:1s "                       // 22-23
  "F0-100:600000ms";                     // 24 long step: 10 minute fade up

size_t testProgramText(int ch, char* buf, size_t cap) {
  if (ch < 0) ch = 0;
  int n = snprintf(buf, cap, TEMPLATE, (unsigned long)ch * 500ul);
  return n < 0 ? 0 : (size_t)n;
}

bool testProgram(int ch, LfProgram& out) {
  char text[LF_PROG_TEXT_MAX], err[64];
  testProgramText(ch, text, sizeof text);
  return parseProgram(text, out, err, sizeof err);
}

float testLevelAt(const LfProgram& p, uint32_t t_ms) {
  uint32_t total = programTotalMs(p);
  if ((p.flags & LF_PROG_LOOP) && total > 0) t_ms %= total;
  return programLevelAt(p, t_ms);
}

}  // namespace lf
