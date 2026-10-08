// PC unit test for the program engine. Run:
//   g++ -std=c++17 -Wall -Ilib/protocol -Ilib/engine test/engine_test.cpp lib/engine/engine.cpp -o engine_test && ./engine_test
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
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
  CHECK(parseProgram("", p, err, sizeof err) && p.n_steps == 0 && programCrc(p) == 0);
  CHECK(near(programLevelAt(p, 100), 0.0f));

  // ---------- 24 steps ----------
  static_assert(LF_MAX_STEPS == 24 && LF_PROTO_VER == 2, "limits");
  static_assert(sizeof(LfProgram) == 218 && sizeof(LfMsgProg) == 231, "wire sizes");
  static_assert(sizeof(LfMsgHeartbeat) <= 250 && sizeof(LfMsgPlay) == 17 && sizeof(LfMsgSync) == 17, "wire sizes");
  {
    std::string t24;
    for (int i = 0; i < 24; i++) t24 += "H" + std::to_string(i * 4) + ":100ms ";
    CHECK(parseProgram(t24.c_str(), p, err, sizeof err) && p.n_steps == 24);
    CHECK(programTotalMs(p) == 2400 && near(programLevelAt(p, 2350), 0.92f) && near(programLevelAt(p, 99999), 0.92f));
    CHECK(validateProgram(p));
    CHECK(!parseProgram((t24 + "H0:0").c_str(), p, err, sizeof err));   // 25th step rejected
    CHECK(strstr(err, "max 24") != nullptr);
    CHECK(parseProgram(("loop " + t24).c_str(), p, err, sizeof err) && p.n_steps == 24 && (p.flags & LF_PROG_LOOP));
    LfProgram bad = p; bad.n_steps = 25; CHECK(!validateProgram(bad));
    bad = p; bad.steps[3].type = STEP_BLINK; bad.steps[3].period_ms = 0; CHECK(!validateProgram(bad));
    bad = p; bad.steps[3].a = 101; CHECK(!validateProgram(bad));
    bad = p; bad.steps[3].type = 3; CHECK(!validateProgram(bad));
    // worst-case text: 24 max-size blink steps must fit LF_PROG_TEXT_MAX
    std::string w = "loop ";
    for (int i = 0; i < 24; i++) w += "B100-100/65535ms:4294967295ms ";
    CHECK(parseProgram(w.c_str(), p, err, sizeof err) && p.n_steps == 24);
    CHECK(p.steps[0].period_ms == 65535 && p.steps[0].duration_ms == 4294967295u);
    char big[LF_PROG_TEXT_MAX];
    size_t n = formatProgram(p, big, sizeof big);
    CHECK(n > 700 && n < sizeof big && strlen(big) == n);
    CHECK(!parseProgram("H0:4294967296", p, err, sizeof err));          // more than uint32 ms
    CHECK(!parseProgram("B0-100/65536ms:1s", p, err, sizeof err));      // period over 65535
    CHECK(!parseProgram("B0-100/0ms:1s", p, err, sizeof err));          // period 0
    CHECK(!parseProgram("H0:nan", p, err, sizeof err));
  }

  // ---------- real compileShow() output (web/control.html, frames W1-1 .. W4-1) ----------
  {
    // "Wall by wall, blink, fade out"
    const char* wall[4] = {
      "F0-100:2000ms H100:10000ms B0-100/800ms:6400ms F100-0:2000ms",
      "H0:3000ms F0-100:2000ms H100:7000ms B0-100/800ms:6400ms F100-0:2000ms",
      "H0:6000ms F0-100:2000ms H100:4000ms B0-100/800ms:6400ms F100-0:2000ms",
      "H0:9000ms F0-100:2000ms H100:1000ms B0-100/800ms:6400ms F100-0:2000ms"};
    for (int w = 0; w < 4; w++) {
      CHECK(parseProgram(wall[w], p, err, sizeof err));
      CHECK(programTotalMs(p) == 20400);
      CHECK(near(programLevelAt(p, 0), 0.0f));
      CHECK(near(programLevelAt(p, 11999), 1.0f));              // every wall fully on just before 12 s
      CHECK(near(programLevelAt(p, 12000), 0.0f));              // blink starts at a=0
      CHECK(near(programLevelAt(p, 12400), 1.0f));              // peak at half of the 800 ms period
      CHECK(near(programLevelAt(p, 18400 + 1000), 0.5f));       // halfway through the final fade-out
      CHECK(near(programLevelAt(p, 20400), 0.0f) && near(programLevelAt(p, 99999), 0.0f));
      char b[200];
      formatProgram(p, b, sizeof b);
      LfProgram q; CHECK(parseProgram(b, q, err, sizeof err) && memcmp(&p, &q, sizeof p) == 0);
    }
    parseProgram(wall[2], p, err, sizeof err);
    CHECK(near(programLevelAt(p, 5999), 0.0f) && near(programLevelAt(p, 7000), 0.5f) && near(programLevelAt(p, 8000), 1.0f));
    // "Alternating breath (loop)" W1-1 and W4-1
    CHECK(parseProgram("loop F0-100:2000ms F100-0:2000ms H0:4000ms", p, err, sizeof err) && (p.flags & LF_PROG_LOOP));
    CHECK(programTotalMs(p) == 8000 && near(programLevelAt(p, 1000), 0.5f) && near(programLevelAt(p, 5000), 0.0f));
    CHECK(parseProgram("loop H0:4000ms F0-100:2000ms F100-0:2000ms", p, err, sizeof err));
    CHECK(near(programLevelAt(p, 5000), 0.5f));
    // "Wave around the room" W4-1
    CHECK(parseProgram("H0:5700ms F0-100:1000ms H100:3000ms F100-0:1000ms", p, err, sizeof err));
    CHECK(programTotalMs(p) == 10700);
    // loop show with the blink the task description quotes
    CHECK(parseProgram("loop F0-100:2000ms H0:0ms B0-100/800ms:6400ms", p, err, sizeof err) && p.n_steps == 3);
  }

  // A 24-step show program (12 clips separated by gaps), as compileShow would produce
  LfProgram show24;
  {
    std::string t;
    for (int i = 0; i < 12; i++) t += "F0-100:500ms H0:500ms ";
    CHECK(parseProgram(t.c_str(), show24, err, sizeof err) && show24.n_steps == 24 && programTotalMs(show24) == 12000);
    CHECK(near(programLevelAt(show24, 250), 0.5f) && near(programLevelAt(show24, 750), 0.0f) && near(programLevelAt(show24, 11250), 0.5f));
  }

  // ---------- play with offset ----------
  {
    LfProgram demo, lp;
    CHECK(parseProgram("H100:10s B0-100/1s:5s H0:0", demo, err, sizeof err));
    CHECK(parseProgram("loop F0-100:2s F100-0:2s", lp, err, sizeof err));   // one cycle = 4000 ms
    Channel c;
    c.play(&demo, 5000, 0);
    CHECK(near(c.update(5000), 1.0f));
    c.play(&demo, 5000, 10500);                                  // 10.5 s already elapsed: blink peak
    CHECK(near(c.update(5000), 1.0f) && c.playing());
    CHECK(near(c.update(5000 + 4500), 0.0f));                    // 15 s: finished, off
    c.play(&demo, 5000, 999999);                                 // far past the end, no loop: end level
    CHECK(near(c.update(5000), 0.0f) && c.playing());
    c.play(&lp, 1000, 1000);                                     // loop, 1 s in
    CHECK(near(c.update(1000), 0.5f));
    c.play(&lp, 1000, 4000 * 3 + 3000);                          // three cycles + 3 s
    CHECK(near(c.update(1000), 0.5f));                           // 1 s into the falling half
    CHECK(near(c.update(2000), 0.0f));                           // wraps to the cycle start
    CHECK(near(c.update(2000 + 4000 * 5 + 500), 0.25f));         // 0.5 s into a later cycle
    c.play(&lp, 1000, 0xFFFFFFF0u);                              // offset near uint32 max: must stay in range
    float v = c.update(1000); CHECK(v >= 0.0f && v <= 1.0f && c.playing());
    // millis() wrap
    c.play(&lp, 0xFFFFFE0Cu, 1000);
    CHECK(near(c.update(0xFFFFFE0Cu), 0.5f));
    CHECK(near(c.update(0xFFFFFE0Cu + 1000), 1.0f));             // crosses the wrap
    CHECK(near(c.update(0xFFFFFE0Cu + 3000), 0.0f));             // 4 s elapsed = cycle start
    c.play(&lp, 0xFFFFFFFFu, 2500);                              // now = last tick before wrap
    CHECK(near(c.update(0x00000000u), programLevelAt(lp, 2501)));
    // playing a 24-step show from the middle
    c.play(&show24, 0, 5250);
    CHECK(near(c.update(0), programLevelAt(show24, 5250)));
  }

  // ---------- sync ----------
  {
    LfProgram lp, np;
    CHECK(parseProgram("loop F0-100:2s F100-0:2s", lp, err, sizeof err));   // one cycle = 4000 ms
    CHECK(parseProgram("F0-100:10s", np, err, sizeof err));
    Channel c;
    c.setManual(0.7f, 0, 0);                         // not playing: ignored
    CHECK(!c.sync(5000, 100) && !c.playing() && near(c.update(100), 0.7f));
    c.play(&np, 1000);                               // within tolerance: nothing moves
    CHECK(!c.sync(1000, 2000));
    CHECK(!c.sync(1020, 2000) && !c.sync(980, 2000)); // |drift| == 20 ms is still fine
    CHECK(near(c.update(2000), 0.1f));
    CHECK(c.sync(1021, 2000)); CHECK(near(c.update(2000), 0.1021f));   // beyond tolerance: re-anchor
    CHECK(c.sync(500, 2000));  CHECK(near(c.update(2000), 0.05f));
    CHECK(c.sync(5000, 2000)); CHECK(near(c.update(2000), 0.5f));
    CHECK(near(c.update(3000), 0.6f));               // keeps running from the new anchor
    CHECK(c.sync(0, 3000));    CHECK(near(c.update(3000), 0.0f));
    // loop: compare positions inside one cycle, shortest way round
    c.play(&lp, 1000);
    CHECK(!c.sync(1000 + 4000 * 9, 2000));           // 9 cycles later, same position
    CHECK(!c.sync(1010 + 4000 * 2, 2000));
    CHECK(c.sync(1000 + 4000 * 9 + 500, 2000));
    CHECK(near(c.update(2000), programLevelAt(lp, 1500)));
    c.play(&lp, 0, 3990);                            // 3990 vs 10: only 20 ms apart across the wrap
    CHECK(!c.sync(10, 0));
    c.play(&lp, 0, 3990);
    CHECK(c.sync(40, 0));                            // 50 ms apart across the wrap: adjust
    CHECK(near(c.update(0), programLevelAt(lp, 40)));
    c.play(&lp, 0, 10);
    CHECK(!c.sync(3995, 0));                         // 15 ms behind across the wrap: fine
    c.play(&lp, 0);                                  // loop that already wrapped internally
    c.update(25000);
    CHECK(!c.sync(25000, 25000));
    CHECK(c.sync(26000, 25000));
    CHECK(near(c.update(25000), programLevelAt(lp, 2000)));
    // millis() wraparound
    c.play(&lp, 0xFFFFFC18u);                        // started 1000 ms before the wrap
    CHECK(!c.sync(1500, 0x000001F4u));               // now = wrap + 500 -> elapsed 1500
    CHECK(c.sync(3000, 0x000001F4u));
    CHECK(near(c.update(0x000001F4u), programLevelAt(lp, 3000)));
    CHECK(near(c.update(0x000001F4u + 1000), programLevelAt(lp, 0)));
    Channel d;                                       // non-loop across the wrap
    d.play(&np, 0xFFFFFFF6u);
    CHECK(!d.sync(100, 90));                         // elapsed 100
    CHECK(d.sync(1000, 90) && near(d.update(90), 0.1f));
    d.play(&np, 0, 50000);                           // finished but still PLAYING: sync pulls it back
    CHECK(d.sync(2000, 0) && near(d.update(0), 0.2f));
    d.stop(0);
    CHECK(!d.playing() && !d.sync(1, 0));            // stopped channels ignore sync
    Channel s; s.play(&show24, 100);                 // 24-step show, mid-show resync
    CHECK(s.sync(5250, 600) && near(s.update(600), programLevelAt(show24, 5250)));
  }

  // gamma
  CHECK(levelToDuty(0, 16383) == 0 && levelToDuty(1, 16383) == 16383);
  CHECK(levelToDuty(0.5f, 16383) > 3000 && levelToDuty(0.5f, 16383) < 3700);

  printf(fails ? "%d FAILED\n" : "ALL PASSED\n", fails);
  return fails ? 1 : 0;
}
