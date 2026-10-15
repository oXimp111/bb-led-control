# Relay (range extender) — for the far wall

Status 2026-10-15: bench-tested with three boards (control on the PC, node far away on power only, relay in between). Node without relay: -95 dBm or offline; with relay: online at -70 dBm (page RSSI). Relay printout: `tx_error 0`, node heartbeats counted at the expected 2 per second. Not yet measured: the cabinet position on site, several nodes at once, a long soak run.

## What it is
One extra plain ESP32 (same 30-pin board as the nodes), no LEDs/MOSFETs attached, powered from any USB charger. Firmware: `src/relay/main.cpp`, env `relay`.
It repeats every LED-frame radio packet it hears, unchanged and with the original sender's MAC, so **the node and control firmware do not change**
(no reflash of the 4 nodes or the control board, `LF_PROTO_VER` stays 3). No pairing, no settings: plug it in and it works.

## How it works (short)
- Listens in Wi-Fi promiscuous mode on channel 1, recognises ESP-NOW frames of this project (Espressif OUI + `LF` magic), re-sends them with `esp_wifi_80211_tx`.
- control -> nodes (commands, SYNC, PAIR_ACK) are already broadcast: repeated as is.
- nodes -> control (ACK, HEARTBEAT, PAIR_REQ) are unicast to the control; the copy goes out as broadcast (the control accepts it, and nobody has to MAC-ACK it).
- Same frame heard twice within 30 ms is repeated once. A node that hears both control and relay gets each packet twice; it already ignores the repeat (same seq) and SYNC/heartbeat are harmless twice.
- Pairing a far node works through the relay (PAIR_REQ up, PAIR_ACK down).

## Known side effects
- A node that only reaches the control through the relay may show the **fast "link lost" blink** on its status LED even though it works: the LED counts MAC-level delivery
  of its unicast heartbeat, and the relay cannot ACK for the control. The page showing the node ONLINE is what counts.
- Latency: +1-3 ms per hop. No issue for the 500 ms target.
- Two relays: tested 2026-10-15 with `tools/relay_test.py` (control + `node_fulltest` node + 2 relays, all within 1-2 m, worst case for copies).
  Each command reached the node up to 3 times (direct + each relay), copies at most 10.7 ms apart; 0 of 450 commands failed, 0 applied twice,
  0 state mismatches, 40/40 program CRCs right, relay `tx_error`/`queue_drop` 0. Slowest command 92 ms (47 ms without relays).
  Logs: `test_logs/relaytest_off_20261015_133850/`, `test_logs/relaytest_2relays_20261015_134925/` (relay.log = second relay's printout).
  Range was not part of this test: at range expect more retries (slower GO/uploads), not more duplicates.

## Where to put it
- Roughly halfway between the control center and the far wall, high (shelf / on top of a frame, not on the floor), not inside a metal box, not touching a power supply or LED driver.
- It must hear BOTH sides well. Target RSSI at the relay better than about **-75 dBm** from the control and from the far node (-60 is excellent, -85 is marginal).
- Read it: plug the relay into the laptop, `pio device monitor` (115200). Every 5 s it prints each sender it hears:
  ```
  [relay] 5 s: control->nodes 12  nodes->control 30  duplicate 3  tx_error 0  queue_drop 0
          24:6F:28:xx:xx:xx  CONTROL      rssi -58 dBm  frames 12
          70:4B:CA:xx:xx:xx  node         rssi -71 dBm  frames 10
  ```
  `CONTROL` = the control board, `node` = a wall node. The far node must appear with rssi better than -80, and the CONTROL line must appear.
  (A node that is already out of reach of the relay does not appear at all.)
- Also useful: the page shows RSSI of each node as seen by the control. Compare before and after.
- The LED on the relay blinks briefly once a second (alive) and flashes on every repeated packet.

## If it does not help
- No `CONTROL` line: relay is too far from the control (or the control board is not powered). No far `node` line: too far from that wall.
- `tx_error` large: send me the serial output; `esp_wifi_80211_tx` may be refused by the Wi-Fi core.
- Other cheap range measures, independent of the relay: ESP32 module with external antenna (ESP32-WROOM-32U + 2 dBi antenna), antenna side of the board facing the other end and away from metal/curtain rails;
  Wi-Fi channel 1 vs 6/11 (change `LF_WIFI_CHANNEL` in `protocol.h`, which means reflashing EVERYTHING).
