# Remote flashing of the bench boards (no BOOT button)

Sean's bench boards (COM4 = control board, COM3 = node board) cannot enter download mode by themselves: on Windows the CH340 driver
sets DTR and RTS one after the other, so the chip leaves reset a few ms before GPIO0 goes low and boots normally (`boot:0x13`).
Tested 2026-10-21: five esptool reset timings, both lines in one Windows call, and "DTR low + software `reboot`" (a software restart
does not re-read GPIO0) all fail. Without someone at the bench the only route is the firmware updating itself.

## How it works
- `lib/serialota` is compiled into `node`, `node_fulltest`, `control`, `control_wstest` and `relay`. While the firmware runs, it accepts
  `@ota` lines on the normal USB serial port (115200, or 460800 for `node_fulltest`).
- The new image goes into the other app slot (default partition table: app0/app1, 1.25 MB each) in 1 KB chunks. Each chunk carries its
  offset and a CRC-32; a damaged or missing chunk is asked for again, a repeated one is answered again but not written twice.
  The running firmware is untouched until the whole image arrived and its MD5 matched.
- The board then restarts into the new image, which starts **unconfirmed** (ESP-IDF app rollback, enabled in the Arduino core's bootloader;
  `verifyRollbackLater()` returns true so the core does not confirm by itself).
- `tools/serial_flash.py` keeps it only if it booted, passed a test transfer, and reports exactly the SHA-256 of the file that was sent.
  Otherwise it resets the board over RTS (EN) and the bootloader goes back to the previous firmware by itself. A crashing image goes back without help.

## Use
```
python tools/serial_flash.py node COM3                 build "node" and flash it (about 2 min)
python tools/serial_flash.py control_wstest COM4 --no-build
python tools/serial_flash.py --status COM3             what runs: env name, slot, state, SHA-256 (and which current build it matches)
python tools/serial_flash.py --reset COM3              reset over USB
```
Exit code 0 = new firmware running and confirmed, 1 = not kept, board runs its previous firmware, 2 = needs attention (message says what).

## Rules (each one keeps a board from being stranded)
- Never send an image without lib/serialota (for example `mosfet_test`): the Arduino core would confirm it and only BOOT could fix the board.
  The tool refuses images that do not contain the updater.
- Never flash on top of an unconfirmed firmware: if that flash also failed, the bootloader would reject both slots. The board refuses;
  the tool resets first so the board runs its last confirmed firmware.
- The tool refuses a different kind of firmware than the board runs (control onto a node board) unless `--any-role`.
- A board flashed with esptool has no OTA state: rollback never applies to it. The installer's boards carry the same code; it does nothing
  unless an `@ota` line arrives over USB.

## Tests
- PC, no hardware: `python tools/serial_flash_test.py` (needs `pip install ziglang`). The real `serial_ota.cpp` runs against a simulated
  board (`test/serialota_host/`: two slots, IDF rollback rules, restarts, crashes, hangs, wrong baud) and the real tool drives it.
  Covers crash / hang / wrong image / no updater / too big / corrupted, lost and repeated chunks / tool dying mid-transfer / interrupted
  confirm. 25/25 on 2026-10-21.
- Real chip, 2026-10-21: both bench boards installed once with BOOT. Remote flash of `node` on COM3: 780 KB in 127 s, app0 -> app1, confirmed,
  SHA matches. Forced rollback (`--bin .pio/build/node_fulltest/firmware.bin` sent as env `node`): not confirmed, tool reset the board, it came
  back to `node` (app1, valid) by itself.
