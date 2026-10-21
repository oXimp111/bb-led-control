// serial_ota.h — firmware update over the normal USB serial port WHILE the firmware runs: no BOOT button, no
// download mode. Used on the bench so boards can be flashed remotely (tools/serial_flash.py). Same code in every build.
//
// The image goes into the other app slot (default partition table: app0/app1, 1.25 MB each); the running firmware is
// untouched until the whole image arrived and its MD5 matched. The new firmware then starts as "pending": the
// bootloader goes back to the previous one at the next reset unless tools/serial_flash.py confirms it, which it only
// does after the new firmware has received a test transfer and reported the expected SHA-256. A firmware that
// crashes, hangs or lacks this code is therefore never kept. A board flashed with BOOT held (esptool) has no
// pending state and is not affected.
//
// Protocol (lines end with \n; board answers start with "@ota "):
//   @ota ping                 -> @ota pong <name> part=<app0|app1> state=<flashed|pending|valid|...> sha=<64 hex>
//   @ota begin <size> <md5>   -> @ota ready <chunk>   then binary chunks (below), or @ota error <why>
//                                (refused while this firmware is unconfirmed: reset first, see serial_ota.cpp)
//   @ota test <size>          -> same transfer, nothing written (proves the receive path works)
//   @ota confirm              -> @ota confirmed       (keep this firmware)
// Chunk = offset (4 bytes) + payload (min(chunk, bytes left from offset)) + CRC-32 of offset+payload (4 bytes; all
// little endian, CRC as zlib.crc32). Board: @ota ok <bytes so far> | @ota resend <bytes so far> (bad CRC, wrong offset
// or a gap > 1 s inside a chunk; input is flushed) | at the end @ota test ok, or @ota done (then it restarts into the
// new firmware) | @ota error <why>. A repeated chunk (its answer got lost) is answered again, not written twice, so
// the PC may simply send a chunk again when no answer came. 10 s without a good chunk aborts the transfer.
// While pending, the board prints "@ota pending <name>" every second.
#pragma once

namespace serialota {
void begin(const char* name);   // right after Serial.begin()
bool line(const char* s);       // a complete input line: true = it was an "@ota" command and is handled
bool active();                  // true while an image is being received: the caller must not read Serial
void poll();                    // every loop()
}
