// relay/main.cpp — ESP-NOW range extender (repeater). Plain 30-pin ESP32, no outputs, no pairing, no settings.
//
// It listens in promiscuous mode on LF_WIFI_CHANNEL and re-transmits every LED-frame ESP-NOW frame it hears,
// byte for byte, INCLUDING the original source MAC. Nodes and the control center therefore see the packet as if it
// came straight from the other side: neither firmware changes, protocol v3 is untouched.
//   control -> nodes  (commands, SYNC, PAIR_ACK): already broadcast, repeated as is.
//   nodes -> control  (ACK, HEARTBEAT, PAIR_REQ): sent as unicast to the control MAC, which a far control cannot MAC-ACK
//                      for a frame it never heard, so the repeated copy goes out as broadcast instead (the control accepts it).
// A node in range of both sides gets every frame twice; the node already de-duplicates by seq (SYNC/heartbeat are idempotent).
// Serial 115200 prints, every 5 s, who the relay hears and how loudly (RSSI): use it to choose where to put the relay.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include "protocol.h"
#include "serial_ota.h"

#define STATUS_LED_PIN 2
#define MAX_FRAME      300
#define DEDUPE_MS      30      // an identical frame (same source + payload) inside this window is not repeated again

struct Frame { uint16_t len; int8_t rssi; uint8_t data[MAX_FRAME]; };
static QueueHandle_t q;
static uint8_t myMac[6];
static const uint8_t OUI[3] = {0x18, 0xFE, 0x34};   // Espressif OUI used by ESP-NOW action frames

// ---- WiFi task: filter and copy only. Layout of an ESP-NOW frame: 24 byte 802.11 header, category 127, OUI, 4 random bytes,
// vendor element (221, len, OUI, type 4, version 1), then our payload (LfHeader first).
static volatile uint32_t nQueueDrop = 0;
static void onPromisc(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;
  const wifi_promiscuous_pkt_t* pk = (const wifi_promiscuous_pkt_t*)buf;
  const uint8_t* f = pk->payload;
  uint32_t sig = pk->rx_ctrl.sig_len;
  if (sig < 44 || sig > MAX_FRAME + 4) return;
  if (f[0] != 0xD0 || f[24] != 127 || memcmp(f + 25, OUI, 3) || f[32] != 221 || memcmp(f + 34, OUI, 3) || f[37] != 4) return;
  if (f[39] != (LF_MAGIC & 0xFF) || f[40] != (LF_MAGIC >> 8)) return;   // not one of ours
  uint32_t len = 34 + f[33];                                            // end of the vendor element = end of the payload
  if (len > sig || len > MAX_FRAME || len < 39 + sizeof(LfHeader)) return;
  Frame fr;
  fr.len = (uint16_t)len;
  fr.rssi = pk->rx_ctrl.rssi;
  memcpy(fr.data, f, len);
  if (xQueueSend(q, &fr, 0) != pdTRUE) nQueueDrop++;
}

// ---- per-source statistics (for placing the relay)
struct Src { uint8_t mac[6]; bool down; int8_t rssi; uint16_t n; };
static Src srcs[12];
static uint8_t srcNext = 0;
static uint32_t cDown = 0, cUp = 0, cDup = 0, cErr = 0;

static void note(const uint8_t* mac, bool down, int8_t rssi) {
  for (auto& s : srcs) if (!memcmp(s.mac, mac, 6)) { s.rssi = rssi; s.n++; return; }
  Src& s = srcs[srcNext++ % 12];
  memcpy(s.mac, mac, 6); s.down = down; s.rssi = rssi; s.n = 1;
}

struct Seen { uint32_t h; uint32_t t; };
static Seen seen[8];
static uint8_t seenNext = 0;
static bool isDuplicate(const Frame& fr, uint32_t now) {
  uint32_t h = 2166136261u;
  for (int i = 10; i < 16; i++) h = (h ^ fr.data[i]) * 16777619u;           // addr2 = original sender
  for (int i = 39; i < fr.len; i++) h = (h ^ fr.data[i]) * 16777619u;       // payload
  for (auto& s : seen) if (s.h == h && s.t && now - s.t < DEDUPE_MS) return true;
  seen[seenNext++ & 7] = {h, now ? now : 1};
  return false;
}

static uint32_t ledOffAt = 0;

static void forward(Frame& fr) {
  uint32_t now = millis();
  const uint8_t* src = fr.data + 10;
  if (!memcmp(src, myMac, 6)) return;
  uint8_t t = fr.data[42];   // LfHeader.type
  bool down = (t == MSG_PAIR_ACK) || (t >= MSG_SET && t <= MSG_GO);
  bool up = (t == MSG_PAIR_REQ) || (t == MSG_HEARTBEAT) || (t == MSG_ACK);
  if (!down && !up) return;
  note(src, down, fr.rssi);
  if (isDuplicate(fr, now)) { cDup++; return; }
  memset(fr.data + 4, 0xFF, 6);    // addr1 (destination): broadcast, so nobody has to MAC-ACK a frame that was not for the relay
  memset(fr.data + 16, 0xFF, 6);   // addr3
  fr.data[1] &= ~0x08;             // clear the "retry" flag
  if (esp_wifi_80211_tx(WIFI_IF_STA, fr.data, fr.len, true) == ESP_OK) (down ? cDown : cUp)++;
  else cErr++;
  digitalWrite(STATUS_LED_PIN, HIGH);
  ledOffAt = now + 30;
}

static void report() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last < 5000) return;
  last = now;
  Serial.printf("[relay] 5 s: control->nodes %u  nodes->control %u  duplicate %u  tx_error %u  queue_drop %u\n",
                cDown, cUp, cDup, cErr, (unsigned)nQueueDrop);
  for (auto& s : srcs) {
    if (!s.n) continue;
    Serial.printf("        %02X:%02X:%02X:%02X:%02X:%02X  %-12s rssi %d dBm  frames %u\n", s.mac[0], s.mac[1], s.mac[2], s.mac[3], s.mac[4], s.mac[5],
                  s.down ? "CONTROL" : "node", s.rssi, s.n);
    s.n = 0;
  }
  cDown = cUp = cDup = cErr = 0;
}

static void pollSerial() {   // only "@ota" lines (tools/serial_flash.py); the relay has no other commands
  static char buf[128];
  static size_t n = 0;
  while (!serialota::active() && Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') { buf[n] = 0; if (n) serialota::line(buf); n = 0; }
    else if (n < sizeof(buf) - 1) buf[n++] = c;
  }
}

void setup() {
  Serial.setRxBufferSize(2048);   // before begin(): a firmware chunk (tools/serial_flash.py) must fit
  Serial.begin(115200);
  pinMode(STATUS_LED_PIN, OUTPUT);
  q = xQueueCreate(16, sizeof(Frame));
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.setSleep(false);
  esp_wifi_set_max_tx_power(78);   // 19.5 dBm, the maximum
  WiFi.macAddress(myMac);
  wifi_promiscuous_filter_t flt = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};
  esp_wifi_set_promiscuous_filter(&flt);
  esp_wifi_set_promiscuous_rx_cb(onPromisc);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(LF_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  Serial.printf("\n[boot] LED frame RELAY fw 1  protocol v%u  channel %d  MAC %02X:%02X:%02X:%02X:%02X:%02X\n", LF_PROTO_VER, LF_WIFI_CHANNEL,
                myMac[0], myMac[1], myMac[2], myMac[3], myMac[4], myMac[5]);
  serialota::begin("relay");
}

void loop() {
  Frame fr;
  if (xQueueReceive(q, &fr, 10 / portTICK_PERIOD_MS) == pdTRUE) forward(fr);
  serialota::poll();
  pollSerial();
  uint32_t now = millis();
  if (ledOffAt && (int32_t)(now - ledOffAt) >= 0) { digitalWrite(STATUS_LED_PIN, LOW); ledOffAt = 0; }
  if (!ledOffAt && (now % 1000) < 50) digitalWrite(STATUS_LED_PIN, HIGH); else if (!ledOffAt) digitalWrite(STATUS_LED_PIN, LOW);
  report();
}
