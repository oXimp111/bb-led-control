#pragma once
#include "Arduino.h"

struct HostUpdate {   // Arduino UpdateClass subset: writes into the other app slot of the simulated board
  bool begin(size_t size);
  bool setMD5(const char* md5);
  size_t write(uint8_t* data, size_t len);
  bool end();
  void abort();
  const char* errorString();
};
extern HostUpdate Update;
