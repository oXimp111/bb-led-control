// lf_csv.h — CSV line formatter for the node test log (node_test build). Pure C++, header only,
// unit-tested on a PC (test/csv_test.cpp). One row = one event. Fields that do not apply stay empty.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "protocol.h"

#define LF_NA INT32_MIN   // "no value": the CSV field is left empty

#define LF_CSV_HEADER \
  "ms,event,mac,type,len,exp_len,hdr_ok,seq,gap,dup,rssi,ack,slot,n_steps,loop,crc," \
  "ch,level_pct,duty_w,duty_r,duty_exp,mismatch,note\n"

struct LfLogRow {
  uint32_t       ms;
  const char*    event;     // BOOT | RX | OUT | OUTTEST | PAIR
  const uint8_t* mac;       // sender MAC (6 bytes) or nullptr
  const char*    type;      // message name, see lfMsgName()
  int32_t len, exp_len, hdr_ok, seq, gap, dup, rssi, ack, slot, n_steps, loop, crc;
  int32_t ch, level_pct, duty_w, duty_r, duty_exp, mismatch;
  const char*    note;      // free text; ',' and newlines are replaced by ';'
  LfLogRow() : ms(0), event(""), mac(nullptr), type(""), len(LF_NA), exp_len(LF_NA), hdr_ok(LF_NA),
               seq(LF_NA), gap(LF_NA), dup(LF_NA), rssi(LF_NA), ack(LF_NA), slot(LF_NA), n_steps(LF_NA),
               loop(LF_NA), crc(LF_NA), ch(LF_NA), level_pct(LF_NA), duty_w(LF_NA), duty_r(LF_NA),
               duty_exp(LF_NA), mismatch(LF_NA), note("") {}
};

// Name of a message type for the log; unknown types come out as "T<number>" in buf.
inline const char* lfMsgName(int t, char* buf, size_t cap) {
  switch (t) {
    case MSG_PAIR_REQ:  return "PAIR_REQ";
    case MSG_PAIR_ACK:  return "PAIR_ACK";
    case MSG_HEARTBEAT: return "HEARTBEAT";
    case MSG_ACK:       return "ACK";
    case MSG_SET:       return "SET";
    case MSG_PROG:      return "PROG";
    case MSG_ASSIGN:    return "ASSIGN";
    case MSG_PLAY:      return "PLAY";
    case MSG_STOP:      return "STOP";
    case MSG_IDENTIFY:  return "IDENTIFY";
    case MSG_UNPAIR:    return "UNPAIR";
    case MSG_SYNC:      return "SYNC";
    case MSG_GO:        return "GO";
  }
  snprintf(buf, cap, "T%d", t);
  return buf;
}

// Packet length a node expects for a message type (it accepts longer, never shorter). -1 = not a node-bound type.
inline int lfExpectedLen(int t) {
  switch (t) {
    case MSG_PAIR_ACK:  return (int)sizeof(LfHeader);
    case MSG_SET:       return (int)sizeof(LfMsgSet);
    case MSG_PROG:      return (int)sizeof(LfMsgProg);
    case MSG_ASSIGN:    return (int)sizeof(LfMsgAssign);
    case MSG_PLAY:      return (int)sizeof(LfMsgPlay);
    case MSG_STOP:      return (int)sizeof(LfMsgChMask);
    case MSG_IDENTIFY:  return (int)sizeof(LfMsgIdentify);
    case MSG_UNPAIR:    return (int)sizeof(LfHeader);
    case MSG_SYNC:      return (int)sizeof(LfMsgSync);
    case MSG_GO:        return (int)sizeof(LfMsgGo);
  }
  return -1;
}

// Formats one row incl. trailing '\n' and NUL. Returns the length without the NUL. Never overruns cap (cap >= 4).
inline size_t lfCsvRow(char* out, size_t cap, const LfLogRow& r) {
  char* p = out;
  char* const end = out + cap - 2;   // room for '\n' and NUL
  auto put = [&](char c) { if (p < end) *p++ = c; };
  auto str = [&](const char* s, bool clean) {
    for (; s && *s; s++) put(clean && (*s == ',' || *s == '\n' || *s == '\r') ? ';' : *s);
  };
  auto num = [&](int32_t v) {
    if (v == LF_NA) return;
    char t[16]; snprintf(t, sizeof t, "%ld", (long)v); str(t, false);
  };

  { char t[16]; snprintf(t, sizeof t, "%lu", (unsigned long)r.ms); str(t, false); }
  put(','); str(r.event, true);
  put(',');
  if (r.mac) {
    char t[18]; snprintf(t, sizeof t, "%02X:%02X:%02X:%02X:%02X:%02X", r.mac[0], r.mac[1], r.mac[2], r.mac[3], r.mac[4], r.mac[5]);
    str(t, false);
  }
  put(','); str(r.type, true);
  const int32_t plain[] = {r.len, r.exp_len, r.hdr_ok, r.seq, r.gap, r.dup, r.rssi, r.ack, r.slot, r.n_steps, r.loop};
  for (int32_t v : plain) { put(','); num(v); }
  put(',');
  if (r.crc != LF_NA) { char t[8]; snprintf(t, sizeof t, "%04X", (unsigned)(r.crc & 0xFFFF)); str(t, false); }
  const int32_t tail[] = {r.ch, r.level_pct, r.duty_w, r.duty_r, r.duty_exp, r.mismatch};
  for (int32_t v : tail) { put(','); num(v); }
  put(','); str(r.note, true);
  *p++ = '\n';
  *p = 0;
  return (size_t)(p - out);
}
