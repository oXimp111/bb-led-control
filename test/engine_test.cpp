// PC unit test for the program engine. Run:
//   g++ -std=c++17 -Wall -Ilib/protocol -Ilib/engine test/engine_test.cpp lib/engine/engine.cpp -o engine_test && ./engine_test
#include <cstdio>
#include <cmath>
#include <cstring>
#include "engine.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
static bool near(float a, float b) { return fabsf(a - b) < 0.011f; }

int main() {
  using namespace lf;
  LfProgram p; char err[64];

  // Sean's example: 100% for 10 s, blink 0-100 (1 s period) for 5 s, then off.
  CHECK(parseProgram("H100:10s B0-100/1s:5s H0:0", p, err, sizeof err));
  CHECK(p.n_steps == 3 && !(p.flags & LF_PROG_LOOP));
  CHECK(programTotalMs(p) == 15000);
  CHECK(near(programLevelAt(p, 0), 1.0f));
  CHECK(near(programLevelAt(p, 9999), 1.0f));
  CHECK(near(programLevelAt(p, 10000), 0.0f));   // blink starts at a=0
  CHECK(near(programLevelAt(p, 10500), 1.0f));   // half period = peak
  CHECK(near(programLevelAt(p, 10250), 0.5f));
  CHECK(near(programLevelAt(p, 15000), 0.0f));   // off
  CHECK(near(programLevelAt(p, 999999), 0.0f));  // stays off (no loop)

  char buf[160];
  formatProgram(p, buf, sizeof buf);
  CHECK(strcmp(buf, "H100:10s B0-100/1s:5s H0:0ms") == 0);
  LfProgram p2; CHECK(parseProgram(buf, p2, err, sizeof err));
  CHECK(memcmp(&p, &p2, sizeof p) == 0);          // round trip
  CHECK(programCrc(p) == programCrc(p2) && programCrc(p) != 0);

  // fade + loop
  CHECK(parseProgram("loop F0-100:2s, F100-0:2000ms", p, err, sizeof err));
  CHECK(p.flags & LF_PROG_LOOP);
  CHECK(near(programLevelAt(p, 1000), 0.5f));
  CHECK(near(programLevelAt(p, 3000), 0.5f));
  Channel ch;
  ch.play(&p, 1000);
  CHECK(near(ch.update(1000 + 4000 * 7 + 1000), 0.5f));  // 7 loops later
  // millis() wrap
  ch.play(&p, 0xFFFFFC18u);            // 1000 ms before wrap
  CHECK(near(ch.update(0x00000000u), 0.5f));

  // manual fade + stop
  ch.setManual(1.0f, 0, 0);
  CHECK(!ch.playing() && near(ch.update(5), 1.0f));
  ch.setManual(0.0f, 1000, 100);
  CHECK(near(ch.update(600), 0.5f));
  CHECK(near(ch.update(5000), 0.0f));
  ch.play(&p, 0); ch.update(1000); ch.stop(1000);
  CHECK(!ch.playing() && near(ch.update(9000), 0.5f));  // holds level at stop

  // errors
  CHECK(!parseProgram("H101:1s", p, err, sizeof err));
  CHECK(!parseProgram("B0-100:1s", p, err, sizeof err));      // missing period
  CHECK(!parseProgram("X5:1s", p, err, sizeof err));
  CHECK(!parseProgram("H1:1 H1:1 H1:1 H1:1 H1:1 H1:1 H1:1 H1:1 H1:1 H1:1 H1:1", p, err, sizeof err));
  CHECK(parseProgram("", p, err, sizeof err) && p.n_steps == 0 && programCrc(p) == 0);
  CHECK(near(programLevelAt(p, 100), 0.0f));

  // gamma
  CHECK(levelToDuty(0, 16383) == 0 && levelToDuty(1, 16383) == 16383);
  CHECK(levelToDuty(0.5f, 16383) > 3000 && levelToDuty(0.5f, 16383) < 3700);

  printf(fails ? "%d FAILED\n" : "ALL PASSED\n", fails);
  return fails ? 1 : 0;
}
