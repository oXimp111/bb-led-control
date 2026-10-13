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
- **Programs**: up to 24 steps (HOLD / FADE / BLINK, each with a duration), optional loop. Node stores 8
  slots; each channel is assigned a slot. Without loop, the channel holds the last step's end level.
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

## Rules
- Never change `protocol.h` struct layouts without bumping `LF_PROTO_VER` and rebuilding BOTH sides.
  Current: `LF_PROTO_VER` 2, `LF_MAX_STEPS` 24 (`LfProgram` 218 bytes, `LfMsgProg` 231 bytes, ESP-NOW v1 limit 250).
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
pair | unpair | reset | reboot
```
Extra commands for testing the show features without a control center:
```
play <ch|all> [offset_ms]     start as if offset_ms had already elapsed (loop programs wrap)
sync <ch|all> <t_ms>          PLAYING channels: jump so elapsed == t_ms, only if off by > 20 ms
```
Serial lines up to 1023 characters are accepted (a 24-step program is ~700). Longer lines are rejected with
`ERR line too long` and nothing is applied.
Program text: `H<lvl>:<dur>` hold · `F<a>-<b>:<dur>` fade · `B<a>-<b>/<period>:<dur>` blink (triangle a→b→a).
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
- Every command is validated before anything changes: packet length >= struct size, slot < 8, `ch_mask` only
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
  `duty_exp` duty the engine level should give, `mismatch` 1 if `duty_r != duty_w`. OUT rows are written after every applied
  PROG/ASSIGN/PLAY/SET/STOP (all channels) and once per second for PLAYING channels. While pairing or identify is active `duty_w` differs from `duty_exp` on purpose.
Counters (`report`): `raw` frames given to our callback · `taken` packets processed · `queue_drops` rx queue (16) was full ·
`bad_len` too short for its type · `bad_hdr` too short or wrong magic · `bad_ver` other protocol version · `ignored` valid but not from our controller or not for us ·
`checked` packets used for seq statistics · `gaps` sum of missing seqs · `dups` repeated seqs · `loss %` = gaps / (checked + gaps) ·
`acks_sent` · `programs_stored` MSG_PROG applied · `write_mismatch` readback differed right after a duty change · `log_mismatch` same, found in a logged check.
Limits: the readback is the LEDC register, not the electrical pin (no scope). The control firmware uses ONE seq counter for all nodes and for SYNC,
so `gaps` equals real loss only with a single node; with several nodes expect gaps even on a perfect link. `outtest` blocks the node for ~40 s (no heartbeats).

## Node test checklist (single board + Seloky MOSFET on GPIO16, strip on ch1)
1. Boot log shows MAC, `state restored`, help text. LED fast-blinks (unpaired).
2. `set 1 100` → strip full. `set 1 0 3000` → smooth 3 s fade out. `set 1 10` → steady dim, no flicker.
3. `prog 1 H100:10s B0-100/1s:5s H0:0` then `assign 1 1` then `play 1` → 10 s on, 5 blinks, off.
4. `prog 2 loop F0-100:2s F100-0:2s`, `assign 1 2`, `play 1` → breathes forever. `status` shows PLAYING.
5. Power-cycle while #4 runs → resumes breathing. `stop 1`, wait 2 s, power-cycle → holds stopped level.
6. Power-cycle with strip powered: no flash at boot.
7. Hold BOOT 3 s, release → LED double-blinks, strip pulses, closes after 60 s.
8. Hold BOOT 10 s → factory reset; `progs` all empty.

## Open decisions / roadmap
- DECIDED, not implemented: after a power cycle, a FINISHED non-loop program must hold its end level, not replay.
  Needs a saved "finished" flag per channel.
- PLANNED: third program type "stepped show" — a show cut into steps; in Run mode a button plays step 1 and stops,
  each press plays the next step. Design: upload once, each press = one small broadcast "go to step N".
- PLANNED latency fixes: frame click should send only PLAY when the node already has that program (compare
  `prog_crc` from heartbeat); start shows and steps with ONE broadcast to all nodes.
- TODO: multi-node test of control firmware; site survey (packet loss/RSSI at each wall).
