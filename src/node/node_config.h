// node_config.h — hardware settings for a wall node (30-pin ESP-WROOM-32 board).
#pragma once

#define FW_VERSION 1

// PWM output pins, channel 1..8 in order. Safe pins only (no strapping/flash/input-only).
// Test kit (Seloky modules): use channel 1 = GPIO16, channel 2 = GPIO17.
static const int PWM_PINS[] = {16, 17, 18, 19, 21, 22, 23, 25};
#define NUM_CH (int)(sizeof(PWM_PINS) / sizeof(PWM_PINS[0]))

#define PWM_FREQ_HZ   500   // YYNMOS boards: <= 500 Hz
#define PWM_BITS      14
#define PWM_MAX_DUTY  ((1u << PWM_BITS) - 1)

#define STATUS_LED_PIN   2  // onboard blue LED on most 30-pin boards (verify on arrival)
#define BUTTON_PIN       0  // BOOT button, active LOW. Never hold during power-on.

#define TICK_MS            10      // output update rate
#define HEARTBEAT_MS       500     // 2 Hz
#define HEARTBEAT_JITTER   50      // +/- ms, avoids collisions between nodes
#define LINK_TIMEOUT_MS    3000    // no delivered heartbeat for this long = link lost (LED)
#define PAIR_HOLD_MS       3000
#define RESET_HOLD_MS      10000
#define PAIR_WINDOW_MS     60000
#define PAIR_REQ_EVERY_MS  500
#define SAVE_DELAY_MS      1000    // write state to flash this long after last change
#define WDT_TIMEOUT_S      5
