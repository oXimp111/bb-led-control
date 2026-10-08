# LED Frame Control — instructions for Claude

Wireless control of ~24 LED strips (one per picture frame) for a show. Owner: Sean (US, develops).
Installer: his girlfriend (China, Windows, non-technical) — explain steps simply when helping her.

## System
- **Wall nodes**: one ESP32 (30-pin ESP-WROOM-32, CH340 USB) per wall, up to 8 PWM channels each,
  driving opto-isolated MOSFET boards (YYNMOS) at **500 Hz max**. Same firmware on every node; identity = MAC.
- **Control center**: ESP32-S3 touchscreen (Waveshare ESP32-S3-Touch-LCD-5B), not written yet.
- **Radio**: ESP-NOW on Wi-Fi channel `LF_WIFI_CHANNEL` (1). No router.
- **Programs**: up to 10 steps (HOLD / FADE / BLINK, each with a duration), optional loop. Node stores 8
  slots; each channel is assigned a slot. Without loop, the channel holds the last step's end level.

## Layout
```
lib/protocol/protocol.h   ONLY definition of radio packets (node + control center). Bump LF_PROTO_VER on layout change.
lib/engine/               program player + text format, pure C++ (unit-tested on PC)
src/node/                 wall node firmware (node_config.h = pins and timings)
test/engine_test.cpp      PC unit tests for the engine
web/control.html          control UI + built-in simulator (single file). Bottom of file = USB JSON-line protocol
                          the control ESP32 must implement. Wall/frame layout = WALLS config at top.
```

## Rules
- Never change `protocol.h` struct layouts without bumping `LF_PROTO_VER` and rebuilding BOTH sides.
- PWM pins: avoid GPIO 0,2,5,12,15 (strapping), 6–11 (flash), 34–39 (input only).
- Node must restore saved state to the outputs BEFORE the radio starts. Outputs are set to 0 first.
- Radio loss: node keeps doing what it was doing. Never turn lights off because the link dropped.
- Pinned toolchain (`platformio.ini`): uses Arduino core 2.x API (`ledcSetup`/`ledcAttachPin`). Do not upgrade.

## Build / flash (Windows)
1. CH340 driver: WCH CH341SER.EXE v3.9+ (https://www.wch-ic.com/downloads/CH341SER_EXE.html). Board shows as COMx in Device Manager.
2. `pip install platformio` (Python 3 required).
3. Build + flash + monitor: `pio run -e node -t upload` then `pio device monitor` (115200 baud).
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

## Node test checklist (single board + Seloky MOSFET on GPIO16, strip on ch1)
1. Boot log shows MAC, `state restored`, help text. LED fast-blinks (unpaired).
2. `set 1 100` → strip full. `set 1 0 3000` → smooth 3 s fade out. `set 1 10` → steady dim, no flicker.
3. `prog 1 H100:10s B0-100/1s:5s H0:0` then `assign 1 1` then `play 1` → 10 s on, 5 blinks, off.
4. `prog 2 loop F0-100:2s F100-0:2s`, `assign 1 2`, `play 1` → breathes forever. `status` shows PLAYING.
5. Power-cycle while #4 runs → resumes breathing. `stop 1`, wait 2 s, power-cycle → holds stopped level.
6. Power-cycle with strip powered: no flash at boot.
7. Hold BOOT 3 s, release → LED double-blinks, strip pulses, closes after 60 s.
8. Hold BOOT 10 s → factory reset; `progs` all empty.
