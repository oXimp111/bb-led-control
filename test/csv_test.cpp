// Host test for the node test-log CSV formatter (lib/testlog/lf_csv.h).
// g++ -std=c++17 -Ilib/protocol -Ilib/testlog test/csv_test.cpp -o csv_test && ./csv_test
#include <stdio.h>
#include <string.h>
#include "lf_csv.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static int commas(const char* s) { int n = 0; for (; *s; s++) n += *s == ','; return n; }

int main() {
  const int cols = commas(LF_CSV_HEADER);   // 22 commas = 23 columns
  CHECK(cols == 22);

  { // empty row: only ms and event filled, right number of fields
    LfLogRow r; r.ms = 1234; r.event = "BOOT";
    char b[320]; size_t n = lfCsvRow(b, sizeof b, r);
    CHECK(n == strlen(b)); CHECK(b[n - 1] == '\n');
    CHECK(commas(b) == cols);
    CHECK(!strncmp(b, "1234,BOOT,,,,", 13));
  }
  { // full RX row for a 24-step PROG
    uint8_t mac[6] = {0x70, 0x4B, 0xCA, 0x25, 0x7C, 0x31};
    LfLogRow r; r.ms = 99; r.event = "RX"; r.mac = mac; r.type = "PROG";
    r.len = 231; r.exp_len = 231; r.hdr_ok = 1; r.seq = 4242; r.gap = 0; r.dup = 0; r.rssi = -55; r.ack = 0;
    r.slot = 3; r.n_steps = 24; r.loop = 1; r.crc = 0xBEEF;
    char b[320]; lfCsvRow(b, sizeof b, r);
    CHECK(commas(b) == cols);
    CHECK(!strcmp(b, "99,RX,70:4B:CA:25:7C:31,PROG,231,231,1,4242,0,0,-55,0,3,24,1,BEEF,,,,,,,\n"));
  }
  { // output row with mismatch, small crc keeps 4 digits
    LfLogRow r; r.ms = 5; r.event = "OUT"; r.ch = 2; r.level_pct = 50; r.duty_w = 3566; r.duty_r = 3565; r.duty_exp = 3566; r.mismatch = 1; r.crc = 0xA;
    char b[320]; lfCsvRow(b, sizeof b, r);
    CHECK(commas(b) == cols);
    CHECK(strstr(b, ",000A,2,50,3566,3565,3566,1,") != nullptr);
  }
  { // commas / newlines in the note cannot break the row
    LfLogRow r; r.event = "PAIR"; r.note = "a,b\nc";
    char b[320]; lfCsvRow(b, sizeof b, r);
    CHECK(commas(b) == cols);
    CHECK(strstr(b, "a;b;c\n") != nullptr);
  }
  { // too-small buffer: truncated but terminated and never overruns
    LfLogRow r; r.event = "RX"; r.note = "0123456789012345678901234567890123456789";
    char b[24]; memset(b, 'X', sizeof b);
    size_t n = lfCsvRow(b, 16, r);
    CHECK(n <= 15); CHECK(b[n] == 0); CHECK(b[n - 1] == '\n'); CHECK(b[16] == 'X');
  }
  { // names and expected lengths
    char nb[12];
    CHECK(!strcmp(lfMsgName(MSG_SYNC, nb, sizeof nb), "SYNC"));
    CHECK(!strcmp(lfMsgName(77, nb, sizeof nb), "T77"));
    CHECK(lfExpectedLen(MSG_PROG) == 231);
    CHECK(lfExpectedLen(MSG_PLAY) == 17);
    CHECK(lfExpectedLen(MSG_HEARTBEAT) == -1);
  }
  printf(fails ? "csv_test: %d FAILED\n" : "csv_test: all passed\n", fails);
  return fails != 0;
}
