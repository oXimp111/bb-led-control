// Host test for the built-in test load (lib/testload).
// g++ -std=c++17 -Ilib/protocol -Ilib/engine -Ilib/testload test/testload_test.cpp lib/testload/testload.cpp lib/engine/engine.cpp -o testload_test && ./testload_test
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "testload.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main() {
  static_assert(sizeof(LfMsgProg) <= 250, "must fit one ESP-NOW v1 packet");
  CHECK(sizeof(LfMsgProg) == 231);
  uint16_t crcs[LF_TEST_CHANNELS];
  for (int ch = 0; ch < LF_TEST_CHANNELS; ch++) {
    LfProgram p, q; char text[LF_PROG_TEXT_MAX], text2[LF_PROG_TEXT_MAX], err[64];
    CHECK(lf::testProgram(ch, p));
    CHECK(p.n_steps == 24);
    CHECK(p.flags & LF_PROG_LOOP);
    CHECK(lf::validateProgram(p));
    // text round trip: format -> parse -> identical bytes, identical text
    lf::formatProgram(p, text, sizeof text);
    CHECK(lf::parseProgram(text, q, err, sizeof err));
    CHECK(memcmp(&p, &q, sizeof p) == 0);
    lf::formatProgram(q, text2, sizeof text2);
    CHECK(!strcmp(text, text2));
    crcs[ch] = lf::programCrc(p);
    CHECK(crcs[ch] != 0);
    // edge values and every step type
    bool hold = 0, fade = 0, blink = 0, l0 = 0, l100 = 0, p1 = 0, p100 = 0, p65535 = 0, hold0 = 0, fadeUp = 0, fadeDown = 0, longStep = 0;
    for (int i = 0; i < p.n_steps; i++) {
      const LfStep& s = p.steps[i];
      hold |= s.type == STEP_HOLD; fade |= s.type == STEP_FADE; blink |= s.type == STEP_BLINK;
      l0 |= s.a == 0 || s.b == 0; l100 |= s.a == 100 || s.b == 100;
      if (s.type == STEP_BLINK) { p1 |= s.period_ms == 1; p100 |= s.period_ms == 100; p65535 |= s.period_ms == 65535; }
      if (s.type == STEP_HOLD && s.duration_ms == 0) hold0 = 1;
      if (s.type == STEP_FADE) { fadeUp |= s.b > s.a; fadeDown |= s.b < s.a; }
      longStep |= s.duration_ms >= 600000;
    }
    CHECK(hold && fade && blink && l0 && l100 && p1 && p100 && p65535 && hold0 && fadeUp && fadeDown && longStep);
    // chase: lit 100 % from ch*500 ms for 1 s, dark just before; the first 31 s are not dark all the time
    uint32_t off = ch * 500;
    CHECK(fabsf(lf::testLevelAt(p, off + 500) - 1.0f) < 1e-6f);
    if (off) CHECK(lf::testLevelAt(p, off - 1) == 0.0f);
    CHECK(fabsf(lf::testLevelAt(p, off + 1500)) < 1e-6f);
    // fits a real MSG_PROG
    LfMsgProg m; memset(&m, 0, sizeof m); m.prog = p;
    CHECK(sizeof m == 231);
    // loop wrap
    uint32_t total = lf::programTotalMs(p);
    CHECK(total > 600000);
    CHECK(lf::testLevelAt(p, total + off + 500) == lf::testLevelAt(p, off + 500));
  }
  for (int a = 0; a < LF_TEST_CHANNELS; a++)
    for (int b = a + 1; b < LF_TEST_CHANNELS; b++) CHECK(crcs[a] != crcs[b]);
  printf(fails ? "testload_test: %d FAILED\n" : "testload_test: all passed\n", fails);
  return fails != 0;
}
