// testload.h — the built-in test load of the control test firmware (env control_test): 8 programs, one per
// slot/channel, each exactly 24 steps, loop on. Pure C++ (no Arduino), unit-tested on a PC (test/testload_test.cpp).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "engine.h"

namespace lf {

#define LF_TEST_CHANNELS 8

// Text form of the program for channel index 0..7 (channel 1..8). Channel n starts its visible pulse n x 500 ms late (chase).
size_t testProgramText(int ch, char* buf, size_t cap);
// Parsed program (loop on, 24 steps). False if the text does not parse (cannot happen; tested).
bool   testProgram(int ch, LfProgram& out);
// Level 0..1 the program has t_ms after PLAY; loop programs wrap.
float  testLevelAt(const LfProgram& p, uint32_t t_ms);

}  // namespace lf
