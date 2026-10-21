// PC stand-ins for the parts of Arduino/ESP-IDF that lib/serialota uses. Only for test/serialota_host/board.cpp.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <string>
#include <vector>

uint32_t millis();
void delay(uint32_t ms);

struct HostSerial {
  int available();
  int read();
  void printf(const char* fmt, ...);
  void println(const char* s);
  void flush();
};
extern HostSerial Serial;

struct HostEsp { [[noreturn]] void restart(); };
extern HostEsp ESP;
