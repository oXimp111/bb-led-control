# Two-board radio test results (2026-10-13)

Setup: node = ESP32 30-pin (MAC 70:4B:CA:21:4F:EC), control test board = ESP32 30-pin (MAC 70:4B:CA:25:7C:30),
boards side by side on one PC (RSSI about -35 to -52 dBm), channel 1, no strip attached during the automated runs.
Control firmware: `control_test` (commit 9b1c73a or later). Node firmware as noted per run.

## 1. Single load, node = `node_test` (packet accuracy + execution accuracy)

`load` = 8 x MSG_PROG (24 steps, 231 bytes, loop), 8 x MSG_ASSIGN, 1 x MSG_PLAY. Run after `clear` on both boards, 64 s wait.

| Check | Result |
|---|---|
| Commands ACKed (control) | 17 / 17, 0 retries, 0 rejected, no BAD_LEN / BAD_ARG / BAD_VERSION |
| Node saw (node report) | raw 17, taken 17, queue_drops 0, PROG 8 / ASSIGN 8 / PLAY 1 |
| Packet lengths | PROG 231 = expected 231, ASSIGN 14, PLAY 17; bad_len 0, bad_hdr 0, bad_ver 0, ignored 0 |
| Sequence numbers | consecutive, gaps 0, dups 0 |
| Program CRC, 8 slots | node stored CRC == control expected CRC (FD3B 8EEE 07EF 4B46 05CC A4E7 9C9E 4F00), 0 mismatches in 1,064 heartbeat checks |
| Output vs program (60 s, 968 samples) | strict +-10 %: 95-97 %; tolerant (+-60 ms jitter): 100 %, all 8 channels |
| Node LEDC readback (after the fix in 6b54c7e) | write_mismatch 0, log_mismatch 0 over 752 checks |
| `outtest` (8 ch x 0/25/50/75/100 %) | 40 / 40 ok, 0 mismatches (100 % reads back 16384, the core's constant-high remap, counted as correct) |
| Heartbeats | 2.00 / s; longest gap 1254-1566 ms during the load (limit 1500 ms, once exceeded) |
| Node radio sends (ACKs + heartbeats) | 176 delivered, 0 failed |

Verdict: delivery, packet accuracy, stored programs and execution (engine level and PWM duty) are correct for a single load.
Not verified: the electrical signal at the pin and the strip itself (no scope); wall-to-wall range (boards were side by side).

## 2. Stress, node = `node_test` (FAILED, caused by the test logging)

`stress 100 200` reliable: 22 of 100 loads incomplete, 22 commands failed after 5 tries, ACKed 98.7 %, first-attempt loss 9.1 %, 0 CRC mismatches.
`stress 100 200 raw`: round-trip loss 13.7 % (1700 sent, 1467 answered). Control saw heartbeat gaps up to 40 s.
Node had rebooted about 40 times: serial output shows `Task watchdog got triggered ... loopTask`, `abort()`, reboot (reset reason 6),
13-29 s apart while the load repeated. Everything that arrived was correct (no CRC mismatch).
Cause: the `node_test` instrumentation (a LittleFS write after every packet plus per-channel rows) - see section 3.

## 3. Stress, node = plain `node` firmware (PASSED)

Same control, same command: `stress 50 200 reliable`, node watched on serial.

| Metric | `node_test` node | plain `node` |
|---|---|---|
| Single load time | 1.03-1.16 s | 0.37 s |
| Commands ACKed | 98.7 % | 850 / 850 (100 %) |
| Retries | 247 | 0 |
| ACK round trip (min / avg / max) | 6 / 68 / 118 ms | 3 / 20 / 45 ms |
| Node watchdog reboots | about 40 | 0 |
| Longest heartbeat gap | 40 s | 571 ms |
| Heartbeat rate | 0.55 / s under stress | 2.00 / s |
| CRC mismatches | 0 | 0 |

Verdict: the normal node firmware is not affected. ACKs of the large PROG packet take at most 45 ms there, so the 120 ms ACK timeout of the real
control center is adequate. (An earlier guess that the flash write of the program store causes the 40-110 ms ACK times was wrong: most of it was the test logging.)

## Conclusions and follow-ups
- Use `node_test` for single-load accuracy checks only. Do not run `stress` against `node_test`; use the plain `node` firmware for stress and timing.
- Open: `node_test` can hit the 5 s task watchdog when loads repeat back to back (suspect: LittleFS flush after every packet; not root-caused).
- Open: node never power-cycled during these runs (CLAUDE.md step 7); wall-to-wall RSSI and loss survey; real strip observed only by the operator.
- Plain-node stress was 50 loads, earlier runs 100.

## Log of the day's test sequence (2026-10-13)
1. Board on COM3 identified as the production control (MAC ...7C:31 AP). The node board was a separate ESP32 (MAC ...4F:EC); both CH340, COM3 / COM4.
2. node_test flashed to the node (needed BOOT held during upload). First report: boots 1, log ok, not paired, outtest 8 x 5 levels: only 100 % flagged (core remap 16383 -> 16384), fixed in 080e2a7.
3. control_test flashed to the control board; pairing confirmed by first heartbeat. Load #1: 17/17 ACKed, CRC 8/8, but the output window closed at once (stale `now` bug, fixed in 9b1c73a).
4. Load #2 (fixed control): output PASS 960 samples, tolerant 100 %. Heartbeat gap 1642 ms (load #1) / 1254 ms (#2) / 1566 ms (clean run) around the load.
5. Node-side log read: packets correct (see section 1). Node reported ~16,100 "readback differs" events: instrumentation artifact (readback before the 2 ms PWM latch), fixed in 6b54c7e;
   after reflashing, write_mismatch 0 / log_mismatch 0 and outtest 40/40.
6. `stress 100` on node_test: poor delivery, node rebooting (task watchdog) - section 2. Cause isolated by flashing the plain `node` firmware: `stress 50` PASS - section 3.
