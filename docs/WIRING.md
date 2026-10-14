# Wiring sheet: which ESP32 pin drives which frame

For the installer (and her Claude). The mapping is the same on every wall node and is fixed in firmware:
`PWM_PINS` in `src/node/node_config.h`. Change it there only, rebuild and reflash every node.

## Pin per channel (frame)
Channel n = frame n, counted left to right while facing the wall.

| Channel / frame | ESP32 pin | Usual silkscreen label on the 30-pin board* |
|---|---|---|
| 1 | GPIO16 | RX2 |
| 2 | GPIO17 | TX2 |
| 3 | GPIO18 | D18 |
| 4 | GPIO19 | D19 |
| 5 | GPIO21 | D21 |
| 6 | GPIO22 | D22 |
| 7 | GPIO23 | D23 |
| 8 | GPIO25 | D25 |

\*Labels differ between board makers. If a label does not match, trust the GPIO number printed on the board or its pinout picture.
Never use GPIO 0, 2, 5, 12, 15 (boot pins), 6-11 (flash) or 34-39 (input only) for strips.

## How many pins each node uses
| Wall | Node | Frames | Pins used |
|---|---|---|---|
| W1 yellow curtain | node 1 | 8 | channels 1-8 (all) |
| W2 white wall | node 2 | 4 | channels 1-4 |
| W3 red curtain | node 3 | 7 | channels 1-7 |
| W4 white wall (door side) | node 4 | 5 | channels 1-5 |

Unused pins stay unconnected. The same firmware runs on all four; the page decides which wall a node belongs to (Find nodes, then pick the wall).

## Connections (each strip)
- ESP32 pin for the frame -> signal/PWM input of one YYNMOS MOSFET board.
- ESP32 GND -> the board's input ground (the boards are opto-isolated: signal side and strip-power side are separate).
- The strip and its power supply go on the board's output side. Check the board's own labels for input/output terminals; do not guess polarity.
- PWM is 500 Hz (the maximum the boards accept).

## Check every frame after wiring (do this before hanging anything)
1. In `control.html` (Edit mode) click a frame. The popup shows the node, the channel and the board pin. Press **Identify this frame**: only that strip pulses for 5 s.
   It must be the strip on the correct frame. If not, swap the wires on the board or fix the frame/channel numbering.
2. No page? Use the node's USB serial monitor (`pio device monitor`, 115200): `set 3 100` lights channel 3, `set 3 0` turns it off, `set all 0` everything off.
3. After the first boot with strips connected nothing should flash. If a strip flashes at power-up, tell Sean.

## If a strip does nothing
- Wrong pin or loose GND wire (most common). Try `set <ch> 100` on the node serial monitor.
- Node not on the page: it must show online in the Nodes list.
- Frame number is counted from the wrong side of the wall. Frame 1 is the leftmost when you face the wall.
