# LED Frame Control — instructions for Claude

Wireless control of ~24 LED strips (one per picture frame) for a show. Owner: Sean (US, develops).
Installer: his girlfriend (China, Windows, non-technical) — explain steps simply when helping her.

## Project scope
- Purpose: lighting for picture frames in Sean's girlfriend's show in China. ~24 LED strips, one per frame.
- Room: 4 walls, clockwise: W1 yellow curtain 8 frames, W2 white wall 4, W3 red curtain 7, W4 white wall (door side) 5.
  One node per wall. Channel n = frame n, counted left→right facing the wall. Control center sits in a room corner (TBD).
- Users: operator uses `web/control.html` on a laptop. Edit mode = assign frame programs. Run mode = click a frame to
  play its program (overrides the show for that frame only; it then holds its end level), or play a whole-room show.
- Program types: single-frame programs (`S.programs`) and whole-room shows (`S.shows`), never mixed in the UI.
- Priorities: lowest cost, reliability (lights never go dark because the radio drops), operator latency < 500 ms.
- Installer is non-technical; Sean guides by video call; her Claude builds/flashes from this repo.
- The simulator in `control.html` is temporary and is removed once the real control center works.

## System
- **Wall nodes**: one ESP32 (30-pin ESP-WROOM-32, CH340 USB) per wall, up to 8 PWM channels each,
  driving opto-isolated MOSFET boards (YYNMOS) at **500 Hz max**. Same firmware on every node; identity = MAC.
- **Control center**: a PLAIN ESP32 (same 30-pin board as the nodes), no screen. Connects to the laptop by USB and
  bridges the JSON-line protocol in `web/control.html` to ESP-NOW. Later it may host the page over its own Wi-Fi AP.
  Firmware: `src/control/` (env `control`). It is a Wi-Fi AP (`LED-Control` / `ledframes`, channel 1) serving
  `web/control.html` at http://192.168.4.1/ and bridging the same JSON lines (WebSocket port 81, or USB 115200)
  to ESP-NOW. Tested on hardware (pairing, Run-mode play, link loss, reboot) with one node; multi-node and RSSI survey still open.
- **Radio**: ESP-NOW on Wi-Fi channel `LF_WIFI_CHANNEL` (1). No router.
- **Programs**: up to 24 steps (HOLD / FADE / BLINK / KEEP, each with a duration), optional loop. Node stores 16
  slots (two banks of 8: bank b, channel c -> slot b*8+c); each channel is assigned a slot. Without loop, the channel holds the last step's end level.
  Channels are fully independent: a whole-room show is one program per frame (slot n = channel n),
  started together with `MSG_PLAY` and kept aligned with `MSG_SYNC` (see "Control center must send").

## Layout
```
lib/protocol/protocol.h   ONLY definition of radio packets (node + control center). Bump LF_PROTO_VER on layout change.
src/control/              control center firmware (AP + web server + WebSocket/USB JSON <-> ESP-NOW bridge)
tools/embed_web.py        build step: gzips web/control.html (+ tools/ws_shim.js) into the control firmware
lib/engine/               program player + text format, pure C++ (unit-tested on PC)
src/node/                 wall node firmware (node_config.h = pins and timings)
test/engine_test.cpp      PC unit tests for the engine
web/control.html          control UI + built-in simulator (single file). Bottom of file = USB JSON-line protocol
                          the control ESP32 must implement. Wall/frame layout = WALLS config at top.
                          Programs tab = whole-room show editor (shows saved in S.shows, one track per frame,
                          compileShow() is the single compiler). Run `selfTest()` in the browser console.
```
UI feature/shortcut reference for the manual: docs/UI_FEATURES.md (keep updated when the UI changes).
Wiring (which pin = which frame, per-frame Identify check): docs/WIRING.md. `CH_PINS` in `web/control.html` must match `PWM_PINS` in `src/node/node_config.h`.

## Rules
- Never change `protocol.h` struct layouts without bumping `LF_PROTO_VER` and rebuilding BOTH sides.
  Current: `LF_PROTO_VER` 3 (v3 = 16 slots, `STEP_KEEP`, `MSG_GO`), `LF_MAX_STEPS` 24 (`LfProgram` 218 bytes, `LfMsgProg` 231 bytes, ESP-NOW v1 limit 250).
  A packet with another version is NOT applied; the node ACKs `ACK_BAD_VERSION` (3) and prints a warning.
  Flash saved by older firmware with the 10-step layout is discarded at boot (`[boot] old program format discarded`).
- PWM pins: avoid GPIO 0,2,5,12,15 (strapping), 6–11 (flash), 34–39 (input only).
- Node must restore saved state to the outputs BEFORE the radio starts. Outputs are set to 0 first.
- Radio loss: node keeps doing what it was doing. Never turn lights off because the link dropped.
- Pinned toolchain (`platformio.ini`): uses Arduino core 2.x API (`ledcSetup`/`ledcAttachPin`). Do not upgrade.

## Build / flash (Windows)
1. CH340 driver: WCH CH341SER.EXE v3.9+ (https://www.wch-ic.com/downloads/CH341SER_EXE.html). Board shows as COMx in Device Manager.
2. `pip install platformio` (Python 3 required).
3. Build + flash + monitor: `pio run -e node -t upload` then `pio device monitor` (115200 baud).
   Control center: `pio run -e control -t upload` (the page is embedded; to ship a new UI edit `web/control.html`, rebuild, flash).
   If upload fails to connect: hold BOOT, tap EN, release BOOT, retry.
4. Engine tests (needs g++): `g++ -std=c++17 -Ilib/protocol -Ilib/engine test/engine_test.cpp lib/engine/engine.cpp -o engine_test && ./engine_test`

## Node serial commands (115200 baud, newline) — channels and slots are 1-based
```
status | progs | help
set <ch|all> <0-100> [fade_ms]          manual level (stops program on those channels)
prog <slot> [loop] <steps>              write program
assign <ch|all> <slot|none>
play <ch|all> | stop <ch|all> | identify <ch|all> [seconds]
go <bank 0|1> <play mask> <stop mask> [offset_ms]   cue test: masks are numbers (bit0 = ch1); slot = bank*8+ch
pair | unpair | reset | reboot
```
Extra commands for testing the show features without a control center:
```
play <ch|all> [offset_ms]     start as if offset_ms had already elapsed (loop programs wrap)
sync <ch|all> <t_ms>          PLAYING channels: jump so elapsed == t_ms, only if off by > 20 ms
```
Serial lines up to 1023 characters are accepted (a 24-step program is ~700). Longer lines are rejected with
`ERR line too long` and nothing is applied.
Program text: `H<lvl>:<dur>` hold · `F<a>-<b>:<dur>` fade · `B<a>-<b>/<period>:<dur>` blink (triangle a→b→a) · `K:<dur>` keep the current level
(level when the program started, or the previous step's end; used for gaps in cue-list sections).
Durations `500ms`, `1.5s`, or bare ms. Example: `prog 1 H100:10s B0-100/1s:5s H0:0`
(100 % for 10 s, blink 0↔100 % with 1 s period for 5 s, then off.)

## Node behavior
- BOOT button: hold ≥3 s and release → 60 s pairing window (strips pulse at 30 %, LED double-blinks).
  Hold 10 s → factory reset. LED is solid while held past 3 s.
- Status LED (GPIO2): slow blink = connected · fast = unpaired or link lost · double = pairing · solid = error.
- Heartbeat to controller every 500 ms ±50 ms jitter (random start offset).
- Commands are ACKed (`MSG_ACK` with seq + status). A repeated seq is re-ACKed but not re-applied.
- Commands from any MAC other than the paired controller are ignored. Controller may send commands as
  broadcast with the node MAC in `header.target` (avoids the ESP-NOW 20-peer limit) or target FF:FF.. for all nodes.
- State is saved to flash 1 s after the last change; programs immediately. Playing channels auto-resume at boot.
- Task watchdog (5 s) reboots on hang.
- Every command is validated before anything changes: packet length >= struct size, slot < 16, `ch_mask` only
  existing channels (and not 0; `0xFF` = all), level <= 100, blink period > 0, <= 24 steps. Bad input -> `ACK_BAD_ARG`
  (or `ACK_BAD_LEN`), state untouched.

## Control center must send (radio side of a show)
A show is compiled by `compileShow()` in `web/control.html` into one program text per frame. The control
ESP32 turns each text into an `LfProgram` (same text format, `lf::parseProgram`) and then:
1. **Upload** each frame's program with `MSG_PROG` to its node (slot n for channel n), `MSG_ASSIGN` slot n -> channel n,
   and wait for the ACKs. Check `prog_crc` in the heartbeat if unsure. All 24 frames stay independent; no node
   needs to know about the others.
2. **Start** with `MSG_PLAY { ch_mask, offset_ms }`. `offset_ms = 0` starts from the beginning; a non-zero offset
   starts every masked channel as if that much time had already passed (loop programs wrap, finished non-loop
   programs hold their end level). The control center records `t0 = its own millis()` at the moment it sends
   PLAY and uses `offset_ms = 0` for a normal start. PLAY is ACKed and retried like any command, so a retry that
   arrives late starts that node late: send `offset_ms = now - t0` on every retry.
3. **Resync every 2 s while the show runs:** `MSG_SYNC { ch_mask, t_ms = millis() - t0 }`, normally as ONE broadcast
   to `FF:FF:FF:FF:FF:FF` with `ch_mask = 0xFF`. Rules:
   - Best effort: nodes do **not** ACK it and it is never retried (the next one comes 2 s later). It does not use
     the command sequence/dedupe, so it cannot disturb retries of other commands.
   - Only PLAYING channels react. A channel whose drift is <= 20 ms is left alone (no visible jumps); a larger
     drift re-anchors that channel so its elapsed time equals `t_ms`.
   - **Excluded frames:** a frame the operator has overridden (manual `set`, its own program, stopped) must be
     left out of `ch_mask` (per node), otherwise a SYNC would drag a re-started frame back into the show. If
     the overridden frame's node also carries show frames, send that node a SYNC with a narrower mask (unicast
     or broadcast with the node MAC in `header.target`) instead of the all-nodes one.
   - For a loop show, `t_ms` may keep growing past the cycle length; the node reduces it modulo the cycle.
4. **Stop** with `MSG_STOP` (channels hold their level); to end a show with lights off send `MSG_SET` level 0.
5. After a node reboot its playing channels restart from t=0; the next SYNC (<= 2 s) pulls them back into the show.
6. Fill `header.ver = LF_PROTO_VER`. If a node answers `ACK_BAD_VERSION`, that node runs other firmware: tell the user
   to reflash it instead of retrying.

## Cue lists (stepped shows): one timeline, cut into sections, one GO per section
Purpose: a 15 min show whose parts are triggered by the operator on the actors' cues instead of by a clock.
- **Editor** (`web/control.html`, Programs → Entire set): tick "Cue list", click the ruler, "+ Cue at playhead". `show.stepped`, `show.cues = [{t,name}]`
  (start of section 2, 3, ...; section 1 starts at 0, name `show.first`). `compileSections()` slices the compiled steps at the cuts, each section starts at t=0.
  Gaps are `K` (hold the level) instead of off. A fade cut at a boundary is split exactly; a blink cut mid-period becomes fades + the rest of the blink.
  A frame with no clip in a section has no program there. The 24-step limit applies per section.
- **Radio**: section k is uploaded into bank k%2 (`MSG_PROG` slot = bank*8+ch). While section k plays the page uploads k+1 into the other bank, so a cue is ONE small
  `MSG_GO { bank, play_mask, stop_mask, offset_ms }` per node (control JSON `{"cmd":"go","node","bank","play":[..],"stop":[..],"t_ms"}`).
  Node `doGo`: all-or-nothing (bad bank/masks or an empty program in play_mask -> `ACK_BAD_ARG`, nothing changes). play_mask channels: assign slot bank*8+ch and play;
  stop_mask channels: stop and hold the level; channels in neither mask (frames the operator took over) are untouched.
- **Rules (tested in `cueSelfTest()` on the simulator)**: GO while a section plays -> its frames freeze where they are, the next section starts (frames with nothing in the
  new section are stopped and hold their level, the others start with a leading `K` step from the level they had). A finished section just holds its last level until the next GO.
  Press during the 300 ms debounce or while a GO is being sent is ignored. GO after the last section does nothing. Space/Enter = GO.
  A `go` that fails is retried by the control firmware (6 sends) and once more by the page with `t_ms` = time elapsed.
- Previous / jump / first GO after a reload may need a re-upload ("preparing", about 0.3-0.5 s). Replay of the current section is instant (its bank is still loaded).
- The page keeps only the section index in localStorage (`lf-cue`); after a reload it shows "was playing before the page was reloaded" and the next GO continues with the next section.
- Not possible: playing a whole cue-list show in one go on the nodes (it would exceed 24 steps); the editor preview plays it continuously.

## Full capability node test (`node_fulltest`) + `tools/fulltest.py`  — the hardware test
Two boards on one PC: the control board runs the REAL `control` firmware, the node board runs `node_fulltest` = the production `src/node/main.cpp` compiled with
`-DLF_FULLTEST` (hooks in `src/node/fulltest.h`; the plain `node` image is byte-identical with or without the hooks, checked by comparing the .text/.rodata/.data hashes).
It never writes flash on the radio path and prints at 460800 baud. Serial lines starting with `@` are machine-readable:
`@RX` every received packet (node ms, ISR us, source MAC, length, header ok, ver, type, target, seq, gap, dup, ACK status, raw hex),
`ft state` -> `@ST/@CH/@PRG/@END` (engine level, mode, slot, written duty per channel, program CRCs),
`ft sample <ms>` -> `@SMP` per channel: duty measured ON THE PAD (all 8 pads read at once from the GPIO input register), edges, frequency, written duty,
`ft report` -> counters (rx by type, dup, gap sum, queue drops/max, ACKs, flash writes vs skipped, heartbeats, longest loop gap), `ft clear`, `ft ping`.
The pad is the ESP32 pin, not the MOSFET board output (no scope). Only the input buffer is enabled (`PIN_INPUT_ENABLE`); `gpio_set_direction` would re-route the pin away from the LEDC.
`tools/fulltest.py` acts as the page: it sends the same JSON lines to the control board that web/control.html sends (shows and cue sections compiled by the real page code via
`tools/export_fixtures.py` -> `tools/fixtures/compiled.json`) and checks everything against an independent Python model (`tools/ftlib.py`): packet bytes (header, length, payload),
program CRCs, engine levels over time, pad duty/frequency/cross-talk. Groups: T0 preflight, T1 pairing + heartbeats, T2 pin map, T3 single actions, T4 shows (+SYNC),
T5 cue list (GO mid-section freeze, hold after finish, bad GO, replay), T6 persistence over a node reboot, T7 control reset while the node plays, T8 stress + GO latency.
Run: `pio run -e control -t upload` (control board), `pio run -e node_fulltest -t upload` (node board), then `python tools/fulltest.py` (options `--only pins,single,shows,cues,persist,ctlreset,stress`, `--no-pair`).
`tools/ft_flash.py <env> <COMx> <label>` flashes while `tools/ft_console.py` (open it in its own window) tells the person what to press. NO keyboard is needed: only hold BOOT while it retries.
Logs: `test_logs/fulltest_<time>/` (node.log, control.log, checks.txt). Flash the plain `node` firmware back before installing on a wall if the test build is not wanted.
Known quirks seen by the test: the control firmware takes a sequence number before it validates a command, so commands it refuses itself leave a "gap" in the node's seq statistics (harmless);
PLAY/GO first transmission carries a few ms of offset_ms (late-retry compensation).
Results: docs/TEST_RESULTS.md.

## Node test checklist (single board + Seloky MOSFET on GPIO16, strip on ch1)
1. Boot log shows MAC, `state restored`, help text. LED fast-blinks (unpaired).
2. `set 1 100` → strip full. `set 1 0 3000` → smooth 3 s fade out. `set 1 10` → steady dim, no flicker.
3. `prog 1 H100:10s B0-100/1s:5s H0:0` then `assign 1 1` then `play 1` → 10 s on, 5 blinks, off.
4. `prog 2 loop F0-100:2s F100-0:2s`, `assign 1 2`, `play 1` → breathes forever. `status` shows PLAYING.
5. Power-cycle while #4 runs → resumes breathing. `stop 1`, wait 2 s, power-cycle → holds stopped level.
6. Power-cycle with strip powered: no flash at boot.
7. Hold BOOT 3 s, release → LED double-blinks, strip pulses, closes after 60 s.
8. Hold BOOT 10 s → factory reset; `progs` all empty.

## Decisions waiting for Sean (REMIND HIM at the start of the next session, with the recommendation)
Source: 2026-10-13 two-board tests, details in docs/TEST_RESULTS.md. Each is a choice about improving the firmware; none is started.
1. DONE (commit 7fd436b) **Skip identical program writes on the node** (`saveProgs()` in `src/node/main.cpp` rewrites the 1.7 KB store on every MSG_PROG). Gain: less flash wear, faster repeat uploads. Risk: very low. Recommend: yes.
2. **Control center sends only changed slots** (compare heartbeat `prog_crc`; same as the planned latency fix). Gain: a repeat load drops from 17 packets to about 1. Recommend: yes, with the real control center work.
3. **Acknowledge first, save to flash after** on the node (like the state, which is saved 1 s later). Gain: heartbeats and the next command are not delayed by a flash write. Cost: a power cut between ACK and save loses that program
   (the control can detect it via `prog_crc`). With decision 1 the benefit is small. Recommend: skip unless uploads must be faster.
4. **ACK timeout in the real control center**: measured PROG ACK max 45 ms, current 120 ms is adequate. Keep, or raise to about 150 ms for margin when the node also runs sync. Recommend: keep.
5. DONE (commit 7fd436b) **Fix the packed-struct reference in `parseProgram`** (`lib/engine/engine.cpp`, `parseDur(s, st.duration_ms)`): undefined behaviour that works on ESP32. Tiny change (local variable), engine tests must still pass. Recommend: yes.
6. DONE: `node_test` and `control_test` were removed; the full capability node test logs to RAM/serial only and survived the stress run (614/614 checks).
7. **Per-node sequence numbers in the real control center** so loss statistics stay valid with several nodes. Low value; the node already de-duplicates by the last seq. Recommend: only if a loss survey is ever possible.
8. DONE (2026-10-13): control board runs the production `control` firmware; node board runs `node_fulltest`. Flash plain `node` (`pio run -e node -t upload`, BOOT held) before putting it on a wall if the test build is not wanted.
9. Still open from before: after a power cycle a FINISHED non-loop program must hold its end level (DECIDED, not implemented); one-broadcast start for shows and steps (PLANNED).

## Open decisions / roadmap
- DECIDED, not implemented: after a power cycle, a FINISHED non-loop program must hold its end level, not replay.
  Needs a saved "finished" flag per channel.
- DONE (2026-10-13, needs hardware test): cue lists / stepped shows, see "Cue lists". Protocol v3 must be flashed on EVERY node and the control board (they refuse each other's packets otherwise).
- PLANNED latency fixes: frame click should send only PLAY when the node already has that program (compare
  `prog_crc` from heartbeat); start shows and steps with ONE broadcast to all nodes.
- Skipped (not possible with the current setup): multi-node test of control firmware; site survey (packet loss/RSSI at each wall).
