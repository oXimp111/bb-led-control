// Simulated ESP32 board for testing lib/serialota + tools/serial_flash.py on a PC (driven by tools/serial_flash_test.py).
// One process run = one boot. stdin/stdout = the USB serial port. The board's flash lives in a folder:
//   <dir>/state     "boot app0|app1" + one line per slot "app0 <flashed|new|pending|valid|aborted|empty>"
//   <dir>/app0.bin, app1.bin   images; markers inside an image choose how that "firmware" behaves:
//       NAME:<name>;  BAUD:<baud>;  BEHAVE:crash|noota|hang;   (no BEHAVE = normal firmware with serialota)
//   <dir>/baud      written at boot: the baud rate the running firmware uses (the test's fake port garbles on mismatch)
// Exit codes: 3 = ESP.restart(), 5 = crash (panic), 9 = no bootable app. The fake port restarts the process for 3 and 5.
// Bootloader rules (ESP-IDF with rollback): slot "new" -> "pending" and boot it; slot still "pending" at boot -> "aborted",
// boot the other slot.
#include "Arduino.h"
#include "Update.h"
#include "esp_ota_ops.h"
#include "serial_ota.h"
#include <chrono>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <fcntl.h>
#include <io.h>

static std::string dir;
static std::string bootSlot = "app0", running;
static std::map<std::string, std::string> slotState = {{"app0", "empty"}, {"app1", "empty"}};
static std::vector<uint8_t> image;
static esp_partition_t runPart;

static std::string other(const std::string& s) { return s == "app0" ? "app1" : "app0"; }
static void saveState() {
  std::ofstream f(dir + "/state");
  f << "boot " << bootSlot << "\napp0 " << slotState["app0"] << "\napp1 " << slotState["app1"] << "\n";
}
static void loadState() {
  std::ifstream f(dir + "/state");
  std::string k, v;
  while (f >> k >> v) { if (k == "boot") bootSlot = v; else slotState[k] = v; }
}
static std::vector<uint8_t> readFile(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static std::string marker(const char* key, const char* dflt) {
  std::string s(image.begin(), image.begin() + std::min<size_t>(image.size(), 4096)), k = key;
  size_t i = s.find(k);
  if (i == std::string::npos) return dflt;
  size_t e = s.find(';', i);
  return s.substr(i + k.size(), e - i - k.size());
}

// ---- Arduino stand-ins
static auto t0 = std::chrono::steady_clock::now();
uint32_t millis() { return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count(); }
void delay(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static std::mutex mu;
static std::deque<uint8_t> rx;
HostSerial Serial;
int HostSerial::available() { std::lock_guard<std::mutex> l(mu); return (int)rx.size(); }
int HostSerial::read() { std::lock_guard<std::mutex> l(mu); if (rx.empty()) return -1; int c = rx.front(); rx.pop_front(); return c; }
void HostSerial::printf(const char* fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stdout, fmt, a); va_end(a); fflush(stdout); }
void HostSerial::println(const char* s) { fputs(s, stdout); fputc('\n', stdout); fflush(stdout); }
void HostSerial::flush() { fflush(stdout); }
HostEsp ESP;
void HostEsp::restart() { saveState(); fflush(stdout); _exit(3); }

// ---- MD5 (RFC 1321) for Update.setMD5
static std::string md5hex(const std::vector<uint8_t>& msg) {
  static const uint32_t K[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
  static const int R[64] = {7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
                            4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
  std::vector<uint8_t> m = msg;
  uint64_t bits = (uint64_t)msg.size() * 8;
  m.push_back(0x80);
  while (m.size() % 64 != 56) m.push_back(0);
  for (int i = 0; i < 8; i++) m.push_back((uint8_t)(bits >> (8 * i)));
  uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
  for (size_t off = 0; off < m.size(); off += 64) {
    uint32_t w[16];
    for (int i = 0; i < 16; i++) w[i] = m[off + 4 * i] | m[off + 4 * i + 1] << 8 | m[off + 4 * i + 2] << 16 | (uint32_t)m[off + 4 * i + 3] << 24;
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 64; i++) {
      uint32_t f; int g;
      if (i < 16) { f = (b & c) | (~b & d); g = i; }
      else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
      else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
      else { f = c ^ (b | ~d); g = (7 * i) % 16; }
      uint32_t t = d; d = c; c = b;
      uint32_t x = a + f + K[i] + w[g];
      b = b + ((x << R[i]) | (x >> (32 - R[i])));
      a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
  }
  char out[33];
  for (int i = 0; i < 16; i++) snprintf(out + 2 * i, 3, "%02x", (h[i / 4] >> (8 * (i % 4))) & 0xff);
  return out;
}

// ---- Update stand-in: the other slot of the simulated flash
HostUpdate Update;
static std::vector<uint8_t> upBuf;
static size_t upSize = 0;
static std::string upMd5, upErr = "no error";
bool HostUpdate::begin(size_t size) {
  if (size > 0x140000) { upErr = "Not Enough Space"; return false; }
  upBuf.clear(); upSize = size; upMd5.clear(); return true;
}
bool HostUpdate::setMD5(const char* md5) { if (strlen(md5) != 32) return false; upMd5 = md5; return true; }
size_t HostUpdate::write(uint8_t* d, size_t n) { upBuf.insert(upBuf.end(), d, d + n); return n; }
void HostUpdate::abort() { upBuf.clear(); upSize = 0; }
const char* HostUpdate::errorString() { return upErr.c_str(); }
bool HostUpdate::end() {
  if (upBuf.size() != upSize) { upErr = "Not Finished"; return false; }
  if (!upMd5.empty() && md5hex(upBuf) != upMd5) { upErr = "MD5 Check Failed"; return false; }
  if (upBuf.empty() || upBuf[0] != 0xE9) { upErr = "Wrong Magic Byte"; return false; }
  std::string slot = other(running);
  std::ofstream(dir + "/" + slot + ".bin", std::ios::binary).write((const char*)upBuf.data(), upBuf.size());
  slotState[slot] = "new";
  bootSlot = slot;
  saveState();
  return true;
}

// ---- esp_ota stand-ins
const esp_partition_t* esp_ota_get_running_partition() { return &runPart; }
esp_err_t esp_ota_get_state_partition(const esp_partition_t* p, esp_ota_img_states_t* s) {
  const std::string& st = slotState[p->label];
  if (st == "flashed" || st == "empty") return ESP_ERR_NOT_FOUND;
  *s = st == "new" ? ESP_OTA_IMG_NEW : st == "pending" ? ESP_OTA_IMG_PENDING_VERIFY : st == "valid" ? ESP_OTA_IMG_VALID
     : st == "aborted" ? ESP_OTA_IMG_ABORTED : ESP_OTA_IMG_INVALID;
  return ESP_OK;
}
esp_err_t esp_ota_mark_app_valid_cancel_rollback() { slotState[running] = "valid"; saveState(); return ESP_OK; }
esp_err_t esp_partition_get_sha256(const esp_partition_t*, uint8_t* sha) {   // image with appended hash: its last 32 bytes
  if (image.size() < 32) return ESP_ERR_NOT_FOUND;
  memcpy(sha, image.data() + image.size() - 32, 32);
  return ESP_OK;
}

int main(int argc, char** argv) {
  if (argc < 2) { fprintf(stderr, "usage: board <dir>\n"); return 2; }
  dir = argv[1];
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
  loadState();
  // bootloader (ESP-IDF with rollback): NEW -> PENDING and boot it; a slot still PENDING -> ABORTED, try the other one
  // (also aborted if it is pending too); "flashed" (no OTA record) and VALID boot as they are
  bool ok = false;
  for (int i = 0; i < 2 && !ok; i++) {
    std::string& st = slotState[bootSlot];
    if (st == "new") { st = "pending"; ok = true; }
    else if (st == "valid" || st == "flashed") ok = true;
    else { if (st == "pending") st = "aborted"; bootSlot = other(bootSlot); }
  }
  if (!ok) { saveState(); printf("[sim] no bootable app\n"); fflush(stdout); return 9; }
  running = bootSlot;
  saveState();
  snprintf(runPart.label, sizeof(runPart.label), "%s", running.c_str());
  image = readFile(dir + "/" + running + ".bin");
  std::string name = marker("NAME:", "node"), behave = marker("BEHAVE:", "ok"), baud = marker("BAUD:", "115200");
  std::ofstream(dir + "/baud") << baud;
  std::thread([] {
    int c;
    while ((c = getchar()) != EOF) { std::lock_guard<std::mutex> l(mu); rx.push_back((uint8_t)c); }
  }).detach();
  printf("[sim] boot %s (%s) behave=%s\n", running.c_str(), name.c_str(), behave.c_str());
  fflush(stdout);
  if (behave == "hang") for (;;) delay(1000);
  if (behave == "crash") { delay(50); printf("Guru Meditation Error: Core 1 panic'ed (simulated)\n"); fflush(stdout); _exit(5); }
  if (behave == "noota") {   // firmware without lib/serialota: the Arduino core confirms a pending image by itself
    if (slotState[running] == "pending") { slotState[running] = "valid"; saveState(); }
    for (;;) { while (Serial.available()) Serial.read(); delay(5); }
  }
  serialota::begin(name.c_str());
  // the node's pollSerial(), reduced: lines go to serialota first
  std::string ln;
  for (;;) {
    serialota::poll();
    while (!serialota::active() && Serial.available()) {
      char c = (char)Serial.read();
      if (c == '\r') continue;
      if (c == '\n') { if (!ln.empty() && !serialota::line(ln.c_str())) Serial.println("ERR unknown command. type 'help'"); ln.clear(); }
      else if (ln.size() < 1023) ln += c;
    }
    delay(1);
  }
}
