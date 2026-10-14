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

## Node test build (`node_test`) — packet accuracy + output check
Same node firmware plus logging, built with `-DLF_TEST_LOG` (all code in `src/node/test_log.h`, formatter in `lib/testlog/lf_csv.h`).
It only observes: same parser, engine and LEDC output. Env `node` compiles to the same machine code as without these hooks.
Build/flash: `pio run -e node_test -t upload`, then `pio device monitor`. Flash `-e node` again to return to normal.
Log: LittleFS `/rxlog.csv` (rotated to `/rxlog.old` above 256 KB), counters in NVS (saved every 10 s and at `report`; survive power loss).
A `report` block prints automatically 5 s after boot. Extra serial commands:
```
report        summary block (paste it to Claude)
log [n|all]   last n CSV lines (default 50) or the whole file
clear         wipe log + counters
outtest       each channel 1..8: 0/25/50/75/100 % for 1 s, prints written vs readback vs expected duty (~40 s)
```
CSV columns (one row per event; empty = not applicable):
`ms,event,mac,type,len,exp_len,hdr_ok,seq,gap,dup,rssi,ack,slot,n_steps,loop,crc,ch,level_pct,duty_w,duty_r,duty_exp,mismatch,note`
- `event`: BOOT, RX (a received packet), OUT (channel check), OUTTEST, PAIR (window open/closed, PAIR_ACK stored the controller MAC).
- RX: `len` received bytes, `exp_len` struct size for that type (node accepts longer, never shorter), `hdr_ok` magic + `LF_PROTO_VER`,
  `gap` seqs missing since the previous packet from the controller, `dup` 1 = same seq as previous (a retry), `rssi` dBm
  (from promiscuous action frames; approximate, empty if unavailable), `ack` status we sent (0 OK, 1 BAD_LEN, 2 BAD_ARG, 3 BAD_VERSION; empty = none),
  PROG only: `slot` (1-based), `n_steps`, `loop`, `crc` = `programCrc()` of the stored slot (hex).
- OUT/OUTTEST: `ch` 1-based, `level_pct` engine level, `duty_w` duty we wrote, `duty_r` duty read back from the LEDC peripheral,
  `duty_exp` duty the engine level should give, `mismatch` 1 if the readback is not what was written (at 100 % the core writes 16383 and the register reads 16384 = constant high; that counts as OK). OUT rows are written after every applied
  PROG/ASSIGN/PLAY/SET/STOP (all channels) and once per second for PLAYING channels. While pairing or identify is active `duty_w` differs from `duty_exp` on purpose.
Counters (`report`): `raw` frames given to our callback · `taken` packets processed · `queue_drops` rx queue (16) was full ·
`bad_len` too short for its type · `bad_hdr` too short or wrong magic · `bad_ver` other protocol version · `ignored` valid but not from our controller or not for us ·
`checked` packets used for seq statistics · `gaps` sum of missing seqs · `dups` repeated seqs · `loss %` = gaps / (checked + gaps) ·
`acks_sent` · `programs_stored` MSG_PROG applied · `write_mismatch` readback differed right after a duty change · `log_mismatch` same, found in a logged check.
Limits: the readback is the LEDC register, not the electrical pin (no scope). The control firmware uses ONE seq counter for all nodes and for SYNC,
so `gaps` equals real loss only with a single node; with several nodes expect gaps even on a perfect link. `outtest` blocks the node for ~40 s (no heartbeats).

## Control test build (`control_test`) — pairs with a node, sends max-size packets, measures
A second plain ESP32 (same 30-pin board) in `src/control_test/main.cpp`; the test load is in `lib/testload/` (pure C++, host-tested).
Uses `protocol.h` and the engine unchanged. Build/flash: `pio run -e control_test -t upload`, `pio device monitor` (115200).
Pair it with a `node_test` board (see "Node test build"). It is NOT the real control center: no web page, one node only.
**Test load:** 8 programs (slot n = channel n), each exactly 24 steps with loop on = `MSG_PROG` of 231 bytes, the largest real packet.
Every step type, levels 0/100, fade up and down, blink periods 1 / 100 / 65535 ms, a 0 ms hold, a 1 ms fade, equal-level fade/blink,
and a 600000 ms step (10 min fade up) at the end. The first ~31 s are easy to watch; channel n starts its pulse n x 500 ms late (chase).
`progs` prints the programs and their `programCrc`. `load` = 8 x PROG, 8 x ASSIGN (slot n -> channel n), PLAY all (17 commands).
Pairing: `pair` (or hold BOOT 3 s) opens a 60 s window, accepts `MSG_PAIR_REQ`, answers `MSG_PAIR_ACK`, stores the node MAC in NVS.
Pairing is confirmed by the node's first heartbeat. BOOT short press (< 3 s) = `load`. LED: double blink = pairing, slow = heartbeats fresh, fast = no node.
Commands:
```
pair | unpair                 open pairing window / forget node MAC (the node stays paired until its BOOT is held 10 s)
mode raw|reliable             reliable (default) = retry until ACK, 5 tries, 120 ms each; raw = send once, no retry (true loss)
load | play | stop            send the test load / PLAY all offset 0 / STOP all
set <ch|all> <0-100>          manual level          progs   print programs + CRC
stress N [interval_ms] [raw|reliable]   repeat `load` N times (default 100, 200 ms), prints a STRESS RESULT block
report | log [n|all] | clear | status | help      `report` prints automatically 5 s after boot
```
Log `/txlog.csv` (LittleFS, rotated to `/txlog.old` above 256 KB), counters in NVS (every 10 s and at `report`).
CSV columns: `ms,event,type,len,seq,mode,attempts,ack,rtt_us,total_ms,slot,ch,expected,reported,note`
- `TX` one row per command: `attempts` transmissions, `ack` status (0 OK, 1 BAD_LEN, 2 BAD_ARG, 3 BAD_VERSION; empty = no ACK), `rtt_us` from the acked
  attempt to its ACK, `total_ms` from the first send. `TX_ERR` esp_now_send failed. `PAIR`, `BOOT`, `STRESS` (summary), `OUTCHK` (summary of a 60 s window).
- `CRC_BAD` heartbeat prog_crc differs from expected (`slot`, `expected`, `reported` in decimal). `OUT_BAD` a level outside tolerance (`ch`, `expected`/`reported` %, `total_ms` since PLAY).
  `HB_GAP` two heartbeats more than 1500 ms apart (`total_ms`).
Checks (PASS/FAIL in `report`):
- **Program CRC:** node heartbeat `prog_crc[slot]` == expected `programCrc` for all 8 slots (also counted on every heartbeat after a load).
- **Output:** for 60 s after PLAY every heartbeat level per channel is compared with the level the control computes from the same program at (now - PLAY time).
  `strict` = within +-10 % at that instant; `tolerant` = within +-10 % of the program at some moment within +-60 ms (timing jitter; blinks of 1/100 ms can never match strictly).
  PASS = at least 50 samples and tolerant >= 95 %. The compared level is what the node's engine reports, not the pin (see node test limits).
- **Heartbeats:** at least 90 % of the ~2/s expected over 10+ s and longest gap <= 1500 ms.
- **Delivery:** reliable mode: no command failed and none was rejected. Raw mode: read `raw loss %` (command OR its ACK lost).
`RESULT:` line = PASS only when all four pass; INCOMPLETE until a load ran and its 60 s window ended. MAC-layer counters (`mac_fail`) count frames the
radio could not deliver even after its own hardware retries. RSSI is approximate (promiscuous action frames).

### Two-board test procedure (node_test + control_test, one strip on node GPIO16)
1. Flash the node: `pio run -e node_test -t upload` (hold BOOT during the upload if it cannot connect). Flash the second board: `pio run -e control_test -t upload`.
   Connect the strip MOSFET to node GPIO16. Open a serial monitor on each board (two terminals, 115200).
2. Control: type `clear`, then `pair`. Node: within 60 s hold BOOT 3 s and release (strips pulse). Control prints `PAIR_REQ ...` then `pairing confirmed`.
   Node `report`: `not paired` must now say `paired to <control MAC>`.
3. Node: `outtest` (about 40 s) must end `0 mismatch(es)`.
4. Control: `load`. Expect `all 17 commands ACKed`. The strip on channel 1 pulses at once, other channels (if wired) chase 0.5 s apart.
5. Wait 65 s, then control `report`. PASS looks like: ACKed 100 %, failed 0, rejected 0, all 8 slots `PASS`, output check `PASS` (tolerant >= 95 %),
   heartbeats `PASS` (about 2/s, longest gap < 1500 ms), `RESULT: ... => PASS`. Node `report`: `programs_stored` 8, `bad_len/bad_hdr/bad_ver` 0, `queue_drops` 0, both mismatch counters 0.
6. Stress: flash the plain node (`pio run -e node -t upload`) first - `node_test` can hit the task watchdog when loads repeat. `mode raw`, `stress 100 200`, then `mode reliable`, `stress 100 200`. PASS: reliable `STRESS: PASS`, no CRC mismatches.
7. Power-cycle the node, wait 10 s, control `report`: CRC must still PASS (programs are in flash); the node resumes the programs from t = 0.
8. Paste both `report` blocks (and `log 100` on any FAIL) to Claude.

### Test results so far (details: docs/TEST_RESULTS.md)
- 2026-10-13, node_test + control_test, boards side by side: single load PASS (17/17 ACKed, node saw all packets with correct lengths, no seq gaps, 8/8 program CRCs,
  output 100 % within tolerance, outtest 40/40). Stress on the plain `node` firmware: 50 loads PASS (850/850 ACKed, 0 retries, no reboots, ACK RTT avg 20 ms, max 45 ms).
  Stress on `node_test` FAILED (task watchdog reboots caused by the test logging) - known limitation, not a node firmware problem.
- Not possible with the current setup (skipped, do not remind): node power-cycle check, strip on GPIO16, wall-to-wall RSSI/loss survey, multi-node.

### Findings for future firmware (from the 2026-10-13 two-board tests)
- **Flashing these boards:** they do not enter download mode by themselves. Hold BOOT BEFORE starting the upload and keep holding until it prints `Writing`;
  release, then tap EN. (Hold, tap EN, release before the command does not work: esptool resets the chip again.)
- **Opening the serial port from a PC resets the board** (DTR). A node then restarts its saved playing programs from t=0; counters/log in flash survive.
- **LEDC readback lags one PWM period** (2 ms at 500 Hz): a duty read right after `ledcWrite` is the previous value. Wait >= 4 ms. Core 2.x also turns duty `max`
  (16383) into 16384 (constant high, no 1-tick low per period), so 100 % reads back 16384; `ledcReadFreq()` returns 0 while duty is 0.
- **Large packet timing (plain node, 231-byte MSG_PROG):** ACK round trip 3-45 ms, a full 17-command load takes about 0.4 s, heartbeats stay at 2/s.
  The 120 ms ACK timeout is adequate. Node reset reason 6 = task watchdog (5 s).
- **Never put file or flash logging on the node's radio path.** A LittleFS append after every packet made the node trip the 5 s task watchdog when loads repeated
  back to back (reboot every 13-29 s; playing channels restart at t=0 until the next SYNC). Keep flash writes deferred and rare; log from RAM and flush when idle.
- **Flash writes per PROG:** `saveProgs()` rewrites the whole 8-slot blob (1.7 KB) for every MSG_PROG, even when the program is identical. Idea (not done): skip
  the write when the slot already holds the same program; control sends a slot only when the heartbeat `prog_crc` differs (see planned latency fixes).
- **Seq numbers:** control firmware uses one counter for all nodes and for SYNC, so a node sees gaps by design with several nodes. Per-node loss statistics need per-node seq.
- **Pairing survives reboots and a control reflash** (verified: control NVS holds the node MAC, node NVS holds the control MAC). After the very first `node_test` flash a node showed
  `not paired`; cause not found - re-pair after reflashing a node if it does not heartbeat.
- **ESP-NOW MAC send success is not delivery to the app:** use the application ACK; the send callback only says the radio got a frame-level ack.
- **Host tests on a PC without g++:** `pip install --target <dir> ziglang`, then `PYTHONPATH=<dir> python -m ziglang c++ -std=c++17 -fno-sanitize=undefined ...` (see test file headers).
  `-fno-sanitize=undefined` is needed because `parseProgram` passes `st.duration_ms` (a field of a packed struct) by reference (`lib/engine/engine.cpp`, parseDur call): works on ESP32,
  but it is undefined behaviour; fix by parsing into a local `uint32_t` and copying.

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
6. **`node_test` watchdog reboots under repeated loads**: fix by logging to RAM and writing the file only when idle, or accept the limitation (stress is run on the plain `node`). Recommend: accept unless a stress test with logging is needed.
7. **Per-node sequence numbers in the real control center** so loss statistics stay valid with several nodes. Low value; the node already de-duplicates by the last seq. Recommend: only if a loss survey is ever possible.
8. **Restore the production control firmware** on the control board before real use: it currently runs `control_test` (`pio run -e control -t upload`, BOOT held). The node board currently runs the plain `node` firmware.
9. Still open from before: after a power cycle a FINISHED non-loop program must hold its end level (DECIDED, not implemented); one-broadcast start for shows and steps (PLANNED).

## Open decisions / roadmap
- DECIDED, not implemented: after a power cycle, a FINISHED non-loop program must hold its end level, not replay.
  Needs a saved "finished" flag per channel.
- DONE (2026-10-13, needs hardware test): cue lists / stepped shows, see "Cue lists". Protocol v3 must be flashed on EVERY node and the control board (they refuse each other's packets otherwise).
- PLANNED latency fixes: frame click should send only PLAY when the node already has that program (compare
  `prog_crc` from heartbeat); start shows and steps with ONE broadcast to all nodes.
- Skipped (not possible with the current setup): multi-node test of control firmware; site survey (packet loss/RSSI at each wall).
