// serial_ota.cpp — see serial_ota.h. PC test: test/serial_ota_host.cpp + tools/serial_flash_test.py.
#include "serial_ota.h"
#include <Arduino.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

// The Arduino core confirms a new image by itself at boot unless this returns true. Confirming is the flashing tool's job.
extern "C" bool verifyRollbackLater() { return true; }

namespace serialota {

static const size_t CHUNK = 1024;               // the caller's Serial RX buffer must hold a whole chunk + CRC
static const uint32_t GAP_MS = 1000;            // bytes stopped inside a chunk: ask for it again
static const uint32_t IDLE_ABORT_MS = 10000;    // no good chunk for this long: give up, running firmware unchanged
static const uint32_t PENDING_EVERY_MS = 1000;

static const char* gName = "?";
static bool receiving = false, dry = false, pending = false;
static size_t total = 0, done = 0, got = 0, want = 0;
static uint8_t buf[4 + CHUNK + 4];
static uint32_t lastByte = 0, lastGood = 0, lastPending = 0;

static uint32_t crc32(const uint8_t* p, size_t n) {   // zlib CRC-32
  uint32_t c = 0xFFFFFFFF;
  while (n--) {
    c ^= *p++;
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
  }
  return ~c;
}

static const char* stateName(const esp_partition_t* p) {
  esp_ota_img_states_t s;
  if (esp_ota_get_state_partition(p, &s) != ESP_OK) return "flashed";   // written with BOOT held: no OTA record
  switch (s) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    default: return "flashed";   // UNDEFINED: written with BOOT held (Arduino's boot_app0.bin has no OTA state)
  }
}

static void pong() {
  const esp_partition_t* run = esp_ota_get_running_partition();
  uint8_t h[32];
  char hex[65] = "?";
  if (esp_partition_get_sha256(run, h) == ESP_OK)
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", h[i]);
  Serial.printf("@ota pong %s part=%s state=%s sha=%s\n", gName, run->label, stateName(run), hex);
}

static void startChunk() { got = 0; want = 4 + (total - done < CHUNK ? total - done : CHUNK) + 4; }

static void askAgain() {   // drop whatever is in the input (the rest of a damaged chunk) and ask for the chunk at `done`
  while (Serial.available()) Serial.read();
  startChunk();
  Serial.printf("@ota resend %u\n", (unsigned)done);
}

static void stop(const char* why) {
  if (!dry) Update.abort();
  receiving = false;
  Serial.printf("@ota error %s\n", why);
}

void begin(const char* name) {
  gName = name;
  const esp_partition_t* run = esp_ota_get_running_partition();
  esp_ota_img_states_t s;
  pending = esp_ota_get_state_partition(run, &s) == ESP_OK && s == ESP_OTA_IMG_PENDING_VERIFY;
  Serial.printf("@ota boot %s part=%s state=%s\n", name, run->label, stateName(run));
  if (pending) Serial.println("[ota] new firmware NOT confirmed yet: a reset goes back to the previous one (tools/serial_flash.py confirms it)");
}

bool active() { return receiving; }

bool line(const char* s) {
  if (strncmp(s, "@ota", 4) || (s[4] && s[4] != ' ')) return false;
  const char* a = s[4] ? s + 5 : "";
  if (receiving) return true;   // cannot happen (the caller stops reading lines while receiving)
  if (!strcmp(a, "ping")) pong();
  else if (!strcmp(a, "confirm")) {
    if (pending) {
      esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
      if (e == ESP_OK) pending = false;
      else { Serial.printf("@ota error confirm %d\n", (int)e); return true; }
    }
    Serial.println("@ota confirmed");
  } else if (!strncmp(a, "begin ", 6) || !strncmp(a, "test ", 5)) {
    bool test = a[0] == 't';
    unsigned long size = 0;
    char md5[40] = "";
    int n = test ? sscanf(a + 5, "%lu", &size) : sscanf(a + 6, "%lu %39s", &size, md5);
    if (n != (test ? 1 : 2) || size == 0 || (!test && strlen(md5) != 32)) { Serial.println("@ota error bad_args"); return true; }
    if (!test) {
      // Flashing on top of an unconfirmed firmware could leave NO bootable app (the bootloader rejects an unconfirmed
      // fallback too). The tool resets first: the board then runs its last confirmed firmware.
      if (pending) { Serial.println("@ota error pending_reset_first"); return true; }
      if (!Update.begin(size)) { Serial.printf("@ota error begin %s\n", Update.errorString()); return true; }
      if (!Update.setMD5(md5)) { Update.abort(); Serial.println("@ota error bad_md5"); return true; }
    }
    receiving = true; dry = test; total = size; done = 0; lastByte = lastGood = millis();
    startChunk();
    Serial.printf("@ota ready %u\n", (unsigned)CHUNK);
  } else Serial.println("@ota error unknown");
  return true;
}

void poll() {
  uint32_t now = millis();
  if (!receiving) {
    if (pending && now - lastPending >= PENDING_EVERY_MS) { lastPending = now; Serial.printf("@ota pending %s\n", gName); }
    return;
  }
  while (got < want && Serial.available()) { buf[got++] = (uint8_t)Serial.read(); lastByte = now; }
  if (got == want) {
    size_t n = want - 8;
    uint32_t off, c;
    memcpy(&off, buf, 4);
    memcpy(&c, buf + 4 + n, 4);
    if (c != crc32(buf, 4 + n)) return askAgain();
    if (off + CHUNK == done && n == CHUNK) { startChunk(); Serial.printf("@ota ok %u\n", (unsigned)done); return; }   // repeat: answer again
    if (off != done) return askAgain();
    if (!dry && Update.write(buf + 4, n) != n) return stop("write");
    done += n;
    lastGood = now;
    if (done < total) { startChunk(); Serial.printf("@ota ok %u\n", (unsigned)done); return; }
    receiving = false;
    if (dry) { Serial.println("@ota test ok"); return; }
    if (!Update.end()) { Serial.printf("@ota error end %s\n", Update.errorString()); return; }
    Serial.println("@ota done");
    Serial.flush();
    delay(100);
    ESP.restart();
  } else if (now - lastGood > IDLE_ABORT_MS) stop("timeout");
  else if (got && now - lastByte > GAP_MS) askAgain();
}

}  // namespace serialota
