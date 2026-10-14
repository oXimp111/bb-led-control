// mosfet_test: bench check of one MOSFET board + LED strip.
// GPIO16 drives the MOSFET board's signal input (500 Hz PWM, same as the real nodes).
// BOOT button (GPIO0): press = start breathing (0-100-0 %, 3 s per cycle), press again = stop and hold the
// current brightness, press again = continue from where it stopped.
// Flash: pio run -e mosfet_test -t upload   (hold BOOT until it prints "Writing", then release and tap EN)
// Uses the same Arduino core 2.x LEDC API as the node firmware.
#include <Arduino.h>
#include <math.h>

static const int      LED_PIN    = 16;
static const int      BUTTON_PIN = 0;     // BOOT, active LOW
static const int      PWM_CH     = 0;
static const uint32_t PWM_FREQ   = 500;   // YYNMOS boards: <= 500 Hz
static const int      PWM_BITS   = 14;
static const uint32_t PWM_MAX    = (1u << PWM_BITS) - 1;
static const uint32_t CYCLE_MS   = 3000;
static const uint32_t DEBOUNCE   = 40;

static bool     running   = false;
static uint32_t t0        = 0;   // millis() at the start of the current cycle position
static uint32_t phaseMs   = 0;   // position in the cycle when stopped
static float    level     = 0;   // current brightness 0..1

// Brightness 0..1 -> duty. Gamma 2.0 so the breathing looks even to the eye.
static void writeLevel(float l) {
  if (l < 0) l = 0;
  if (l > 1) l = 1;
  level = l;
  ledcWrite(PWM_CH, (uint32_t)(l * l * PWM_MAX + 0.5f));
}

// 0 % at phase 0, 100 % at half cycle, back to 0 % at the end (raised cosine).
static float breathe(uint32_t ph) {
  return 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)ph / (float)CYCLE_MS));
}

void setup() {
  Serial.begin(115200);
  ledcSetup(PWM_CH, PWM_FREQ, PWM_BITS);
  ledcAttachPin(LED_PIN, PWM_CH);
  writeLevel(0);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Serial.println("[mosfet_test] GPIO16 PWM 500 Hz. Press BOOT to start/stop the breathing animation.");
}

void loop() {
  static bool     lastRaw = true, stable = true;
  static uint32_t lastChange = 0;
  uint32_t now = millis();

  bool raw = digitalRead(BUTTON_PIN);
  if (raw != lastRaw) { lastRaw = raw; lastChange = now; }
  if (now - lastChange >= DEBOUNCE && raw != stable) {
    stable = raw;
    if (!stable) {                                  // pressed (falling edge)
      running = !running;
      if (running) { t0 = now - phaseMs; Serial.println("[mosfet_test] running"); }
      else         { phaseMs = (now - t0) % CYCLE_MS;
                     Serial.printf("[mosfet_test] stopped at %.0f %%\n", level * 100.0f); }
    }
  }

  if (running) writeLevel(breathe((now - t0) % CYCLE_MS));
  delay(5);
}
