// protocol.h — the ONE shared definition of every radio packet.
// Used by node firmware AND control center firmware. Change it in one place only,
// and bump LF_PROTO_VER whenever a struct layout changes.
#pragma once
#include <stdint.h>
#include <stddef.h>

#define LF_MAGIC        0x464C   // bytes 'L','F' little-endian
#define LF_PROTO_VER    3
#define LF_MAX_CH       8        // PWM channels per node
#define LF_MAX_STEPS    24       // steps per program (a compiled show needs up to 24 per frame)
#define LF_MAX_PROGS    16       // program slots per node: 2 banks of LF_MAX_CH (bank b, channel c (0-based) -> slot b*LF_MAX_CH + c)
#define LF_WIFI_CHANNEL 1        // all devices must use the same 2.4 GHz channel
#define LF_NO_PROGRAM   0xFF     // "no program assigned"
#define LF_ALL_CH       0xFF     // channel mask: every channel
#define LF_SYNC_TOLERANCE_MS 20  // MSG_SYNC only moves a channel when it is off by more than this

enum LfMsgType : uint8_t {
  MSG_PAIR_REQ  = 1,   // node -> broadcast   (only during pairing window)
  MSG_PAIR_ACK  = 2,   // ctrl -> node        (node stores sender MAC as its controller)
  MSG_HEARTBEAT = 3,   // node -> ctrl        (best effort, never retried)
  MSG_ACK       = 4,   // node -> ctrl        (reply to every command below)
  MSG_SET       = 10,  // manual level (stops program on those channels)
  MSG_PROG      = 11,  // write a program into a slot
  MSG_ASSIGN    = 12,  // assign slot to channels
  MSG_PLAY      = 13,  // (re)start assigned program at offset_ms (0 = from the start)
  MSG_STOP      = 14,  // stop program, hold current level
  MSG_IDENTIFY  = 15,  // pulse channels so a person can find the frame
  MSG_UNPAIR    = 16,  // node forgets its controller
  MSG_SYNC      = 17,  // re-anchor playing channels to show time t_ms (best effort: NO ACK, may be broadcast)
  MSG_GO        = 18,  // cue: switch channels to a program bank and play, stop others (ACKed; same seq on repeats = applied once)
};

enum LfStepType : uint8_t {
  STEP_HOLD  = 0,  // level a for duration
  STEP_FADE  = 1,  // a -> b over duration
  STEP_BLINK = 2,  // a -> b -> a triangle, period_ms per cycle, for duration
  STEP_KEEP  = 3,  // keep the level the channel has at this moment (end of previous step, or level when the program started) for duration
};

enum LfAckStatus : uint8_t {
  ACK_OK = 0,
  ACK_BAD_LEN = 1,
  ACK_BAD_ARG = 2,
  ACK_BAD_VERSION = 3,   // packet had a different LF_PROTO_VER; not applied. Header layout never changes, so this is always readable.
};

#pragma pack(push, 1)

// Every packet starts with this. target = node MAC, or FF:FF:FF:FF:FF:FF = all paired nodes.
// Controller must start seq at a random value after boot (node drops repeats of last seq).
struct LfHeader {
  uint16_t magic;
  uint8_t  ver;
  uint8_t  type;
  uint8_t  target[6];
  uint16_t seq;
};

struct LfStep {          // 9 bytes
  uint8_t  type;         // LfStepType
  uint8_t  a;            // level 0..100 %
  uint8_t  b;            // level 0..100 % (FADE/BLINK)
  uint16_t period_ms;    // BLINK only
  uint32_t duration_ms;
};

#define LF_PROG_LOOP 0x01
struct LfProgram {       // 2 + 24*9 = 218 bytes
  uint8_t n_steps;       // 0..LF_MAX_STEPS
  uint8_t flags;         // LF_PROG_LOOP
  LfStep  steps[LF_MAX_STEPS];  // unused steps must be zero
};

struct LfMsgSet      { LfHeader h; uint8_t ch_mask; uint8_t level; uint32_t fade_ms; };
struct LfMsgProg     { LfHeader h; uint8_t slot; LfProgram prog; };
struct LfMsgAssign   { LfHeader h; uint8_t ch_mask; uint8_t slot; };   // slot LF_NO_PROGRAM = clear
struct LfMsgChMask   { LfHeader h; uint8_t ch_mask; };                 // STOP
struct LfMsgPlay     { LfHeader h; uint8_t ch_mask; uint32_t offset_ms; }; // start as if offset_ms already elapsed
struct LfMsgSync     { LfHeader h; uint8_t ch_mask; uint32_t t_ms; };      // playing channels: elapsed := t_ms
// Cue. For every channel in play_mask: assign slot bank*LF_MAX_CH + ch, play it from offset_ms.
// Channels in stop_mask: stop, hold current level. Channels in neither mask are untouched. Masks must not overlap.
struct LfMsgGo       { LfHeader h; uint8_t bank; uint8_t play_mask; uint8_t stop_mask; uint32_t offset_ms; };
struct LfMsgIdentify { LfHeader h; uint8_t ch_mask; uint8_t seconds; };
struct LfMsgAck      { LfHeader h; uint16_t acked_seq; uint8_t status; };
struct LfMsgPairReq  { LfHeader h; uint16_t fw_ver; uint8_t n_ch; };

struct LfMsgHeartbeat {
  LfHeader h;
  uint16_t fw_ver;
  uint32_t uptime_s;
  uint8_t  n_ch;
  uint8_t  playing_mask;           // bit n = channel n is running a program
  uint8_t  level[LF_MAX_CH];       // current output 0..100 %
  uint8_t  assign[LF_MAX_CH];      // slot per channel, LF_NO_PROGRAM = none
  uint16_t prog_crc[LF_MAX_PROGS]; // CRC16 of each stored slot (0 = empty) -> screen checks sync
};

#pragma pack(pop)

static_assert(sizeof(LfHeader) == 12, "header size");
static_assert(sizeof(LfStep) == 9, "step size");
static_assert(sizeof(LfProgram) == 2 + LF_MAX_STEPS * 9, "program size");
static_assert(sizeof(LfProgram) == 218, "program size");
static_assert(sizeof(LfMsgPlay) == 17, "play size");
static_assert(sizeof(LfMsgSync) == 17, "sync size");
static_assert(sizeof(LfMsgGo) == 19, "go size");
static_assert(LF_MAX_PROGS == 2 * LF_MAX_CH, "two banks");
static_assert(sizeof(LfMsgProg) <= 250, "must fit one ESP-NOW v1 packet");
static_assert(sizeof(LfMsgHeartbeat) <= 250, "must fit one ESP-NOW v1 packet");
