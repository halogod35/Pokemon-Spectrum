#ifndef GUARD_MP_MP_PROTOCOL_H
#define GUARD_MP_MP_PROTOCOL_H

// Wire protocol (docs/multiplayer/protocol.md). Internal to src/mp.
// PS-011 created the constants; PS-012 added the message structs and the pure
// parsers/writers (protocol.md section 8.3).
//
// Parsers: pure functions. No global state, no I/O, no allocation. They check
// everything that can be checked from the bytes alone. Session-state checks
// (slot occupied, epoch, rate limits, own slot, ...) are the session's job.
// On error *out is zeroed and must not be used.
//
// Writers: write a whole packet (header + payload). Return the number of bytes
// written, or 0 (and nothing usable) if `cap` is too small.

#include "gba/types.h"
#include "mp/mp.h"

#define MP_HEADER_SIZE          4       // u8 type, u8 version, u16 length (LE)
#define MP_MAX_PACKET_SIZE      512     // header + payload; also ENet maximumPacketSize
#define MP_MAX_PAYLOAD_SIZE     (MP_MAX_PACKET_SIZE - MP_HEADER_SIZE)
#define MP_CONNECT_MAGIC        0x504D5350u // ENet connect data, bytes "PSMP" in LE order

// Message types (protocol.md section 4)
#define MP_MSG_HELLO            0x01
#define MP_MSG_WELCOME          0x02
#define MP_MSG_REJECT           0x03
#define MP_MSG_PLAYER_JOINED    0x04
#define MP_MSG_PLAYER_LEFT      0x05
#define MP_MSG_PLAYER_UPDATE    0x06
#define MP_MSG_BYE              0x07
#define MP_MSG_PING             0x08
#define MP_MSG_PONG             0x09
#define MP_MSG_PRESENCE         0x10
#define MP_MSG_DEBUG_ECHO       0xF0    // --mp-selftest only

#define MP_DEBUG_ECHO_MAX_PAYLOAD 64

// Payload sizes
#define MP_HELLO_SIZE           32
#define MP_HELLO_PREFIX_SIZE    12      // frozen prefix: magic, protocolVersion, reserved, buildHash
#define MP_PLAYER_ENTRY_SIZE    20
#define MP_WELCOME_SIZE(n)      (4 + MP_PLAYER_ENTRY_SIZE * (n))
#define MP_REJECT_SIZE          8
#define MP_PLAYER_LEFT_SIZE     4
#define MP_BYE_SIZE             4
#define MP_PING_SIZE            4
#define MP_PRESENCE_SIZE        16

// PRESENCE coordinate range (protocol.md 6.8): map-local tiles, a little outside the
// map is allowed while crossing a seam.
#define MP_PRESENCE_COORD_MIN   (-16)
#define MP_PRESENCE_COORD_MAX   1023

#define MP_ENTRY_FLAG_HOST      (1 << 0)
#define MP_BUILD_FLAG_DIRTY     (1 << 0)

// BYE reasons (protocol.md 6.7)
#define MP_BYE_LEAVING          0
#define MP_BYE_HOST_SHUTDOWN    1
#define MP_BYE_PROTOCOL_ERROR   2

// Parse results
enum
{
    MP_PARSE_OK,
    MP_PARSE_ERR_TRUNCATED,     // fewer bytes than the message needs
    MP_PARSE_ERR_OVERSIZED,     // more bytes than the message allows
    MP_PARSE_ERR_BAD_MAGIC,
    MP_PARSE_ERR_BAD_FIELD,     // an enum / index / count out of range, or inconsistent
    MP_PARSE_ERR_BAD_SUMMARY,   // a PlayerSummary fails protocol.md 5.1
    MP_PARSE_ERR_BAD_HEADER,
};

struct MpMsgHeader
{
    u8 type;
    u8 version;
    u16 length;
};

struct MpMsgHello
{
    u16 protocolVersion;
    u32 buildHash;
    u8 buildFlags;
    struct MpPlayerSummary summary;
};

struct MpMsgPlayerEntry
{
    u8 slot;
    u8 epoch;
    u8 entryFlags;
    struct MpPlayerSummary summary;
};

struct MpMsgWelcome
{
    u8 yourSlot;
    u8 yourEpoch;
    u8 playerCount;
    struct MpMsgPlayerEntry players[MP_MAX_PLAYERS];
};

struct MpMsgReject
{
    u8 reason;
    u16 hostProtocolVersion;
    u32 hostBuildHash;
};

struct MpMsgPlayerLeft
{
    u8 slot;
    u8 epoch;
    u8 reason;
};

struct MpMsgBye
{
    u8 reason;
};

struct MpMsgPing
{
    u32 timeMs;
};

struct MpMsgPresence
{
    u8 slot;
    u8 epoch;
    struct MpPresence presence; // presence.seq is the wire seq
};

// Header: 4 <= len <= MP_MAX_PACKET_SIZE and length == len - 4. Type/version/state
// are not checked here.
bool8 MpProto_ParseHeader(const u8 *buf, u32 len, struct MpMsgHeader *out);

// HELLO frozen prefix only (protocol.md 6.1 steps 2-4): len >= 12 and magic.
u8 MpProto_ParseHelloPrefix(const u8 *payload, u32 len, u16 *protocolVersion, u32 *buildHash);
// Full HELLO: exactly 32 bytes, magic, valid summary.
u8 MpProto_ParseHello(const u8 *payload, u32 len, struct MpMsgHello *out);
// WELCOME: internal consistency of protocol.md 6.2 (all but session state).
u8 MpProto_ParseWelcome(const u8 *payload, u32 len, struct MpMsgWelcome *out);
// REJECT: frozen layout, len >= 8 (longer is allowed for future versions).
u8 MpProto_ParseReject(const u8 *payload, u32 len, struct MpMsgReject *out);
// PLAYER_JOINED and PLAYER_UPDATE: one PlayerEntry, exactly 20 bytes (protocol.md 5.2).
u8 MpProto_ParsePlayerEntry(const u8 *payload, u32 len, struct MpMsgPlayerEntry *out);
u8 MpProto_ParsePlayerLeft(const u8 *payload, u32 len, struct MpMsgPlayerLeft *out);
u8 MpProto_ParseBye(const u8 *payload, u32 len, struct MpMsgBye *out);
// PING and PONG
u8 MpProto_ParsePing(const u8 *payload, u32 len, struct MpMsgPing *out);
// PRESENCE (PS-030): exactly 16 bytes, slot < 4, epoch != 0, the map rule (MP_IsValidMap),
// coordinate range, facing and enum ranges of protocol.md 6.8. When the map is MP_MAP_NONE
// the ignored fields (x, y, facing, movement) are normalised to 0 / DIR_SOUTH / NONE, and
// the reserved presenceFlags byte always parses as 0.
u8 MpProto_ParsePresence(const u8 *payload, u32 len, struct MpMsgPresence *out);

// `headerVersion` is the version byte written in the HELLO header (low 8 bits of the
// advertised protocol version, so --mp-fake-protocol reaches the header too).
u16 MpProto_WriteHello(u8 *buf, u16 cap, const struct MpMsgHello *msg);
u16 MpProto_WriteWelcome(u8 *buf, u16 cap, const struct MpMsgWelcome *msg);
u16 MpProto_WriteReject(u8 *buf, u16 cap, const struct MpMsgReject *msg);
// type = MP_MSG_PLAYER_JOINED or MP_MSG_PLAYER_UPDATE
u16 MpProto_WritePlayerEntry(u8 *buf, u16 cap, u8 type, const struct MpMsgPlayerEntry *msg);
u16 MpProto_WritePlayerLeft(u8 *buf, u16 cap, const struct MpMsgPlayerLeft *msg);
u16 MpProto_WriteBye(u8 *buf, u16 cap, const struct MpMsgBye *msg);
// type = MP_MSG_PING or MP_MSG_PONG
u16 MpProto_WritePing(u8 *buf, u16 cap, u8 type, const struct MpMsgPing *msg);
// Writes the fields as given (reserved presenceFlags as 0). The sender sanitises first
// (MP_SetLocalPresence), so what it writes always passes MpProto_ParsePresence.
u16 MpProto_WritePresence(u8 *buf, u16 cap, const struct MpMsgPresence *msg);

// RFC 1982 serial-number "newer than" for u16 sequence numbers (protocol.md 6.8).
bool8 MpSeqIsNewer(u16 s, u16 last);

#endif // GUARD_MP_MP_PROTOCOL_H
