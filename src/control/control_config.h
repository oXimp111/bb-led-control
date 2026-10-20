// control_config.h — settings for the control center (plain 30-pin ESP32).
#pragma once

#define CTRL_FW_VERSION 4

// Wi-Fi access point the operator's laptop joins. Page: http://192.168.4.1/
// The AP channel IS the ESP-NOW channel, so it must stay LF_WIFI_CHANNEL.
#define AP_SSID          "LED-Control"
#define AP_PASSWORD      "ledframes"     // 8+ characters
#define AP_MAX_CLIENTS   4
#define MDNS_NAME        "ledcontrol"    // http://ledcontrol.local/ where mDNS works

#define WS_PORT          81              // same JSON lines as USB, one JSON object per message

#define STATUS_LED_PIN   2

#define MAX_PEERS        24              // paired nodes remembered (saved in flash)
#define MAX_PENDING      48              // commands in flight (waiting for a node ACK)
#define CMD_RETRY_MS     120             // resend a command if no ACK after this long
#define CMD_MAX_TRIES    6               // total sends before reporting "no_ack"
#define SEND_GAP_MS      3               // minimum spacing between radio sends
#define NODE_OFFLINE_MS  2000            // no heartbeat for this long = node offline (heartbeat is every 500 ms)
#define WS_ALIVE_MS      1000            // {"evt":"alive"} to every page this often: a page that hears nothing for 3 s reconnects
#define WS_STUCK_MS      3000            // a page whose send buffer stays full this long is dead: drop it (lines are skipped meanwhile)
#define WS_SLOW_MS       100           // a WebSocket write slower than this = client stopped taking data: drop it
#define SCAN_MAX_MS      120000          // pairing scan switches itself off after this long
#define PAIR_JOB_MS      4000            // how long "pair" waits for the node's first heartbeat
#define LINE_MAX         2048            // longest JSON line accepted (a 24-step program is ~800)
