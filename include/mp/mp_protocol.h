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
// PS-041 player interaction (protocol.md 6.11), channel 0, reliable
#define MP_MSG_INTERACT_REQUEST     0x40    // requester -> host -> target
#define MP_MSG_INTERACT_RESPONSE    0x41    // target -> host -> requester (or host -> requester on its own)
#define MP_MSG_INTERACT_CANCEL      0x42    // requester -> host -> target (or host -> target on its own)
// Virtual link cable (PS-046, protocol.md 6.12). All on channel 0 (reliable, ordered).
#define MP_MSG_LINK_JOIN        0x60    // C -> H
#define MP_MSG_LINK_LEAVE       0x61    // C -> H
#define MP_MSG_LINK_GROUP       0x62    // H -> C
#define MP_MSG_LINK_END         0x63    // H -> C
#define MP_MSG_LINK_OPEN        0x64    // C -> H
#define MP_MSG_LINK_CLOSE       0x65    // C -> H
#define MP_MSG_LINK_START       0x66    // C -> H (link id 0 only)
#define MP_MSG_LINK_STATUS      0x67    // H -> C
#define MP_MSG_LINK_BUS_START   0x68    // H -> C
#define MP_MSG_LINK_BUS_END     0x69    // H -> C
#define MP_MSG_LINK_CMD         0x6A    // C -> H
#define MP_MSG_LINK_ROW         0x6B    // H -> C
#define MP_MSG_LINK_FIRST       0x60    // reserved LINK_* range (protocol.md 4)
#define MP_MSG_LINK_LAST        0x7F
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
#define MP_LINK_CTL_SIZE        4       // every LINK_* control message
#define MP_LINK_CMD_WORDS       8       // == CMD_LENGTH in include/link.h
#define MP_LINK_MAX_MEMBERS     4       // == MAX_LINK_PLAYERS in include/link.h
#define MP_LINK_CMD_SIZE        (4 + 2 * MP_LINK_CMD_WORDS)                 // 20
#define MP_LINK_ROW_SIZE(n)     (8 + 2 * MP_LINK_CMD_WORDS * (n))           // 8 + 16n
#define MP_INTERACT_SIZE        8       // all three INTERACT_* messages

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

// INTERACT_REQUEST / _RESPONSE / _CANCEL (PS-041). `slot` is always the OTHER party of the
// request from the receiver's point of view (see protocol.md 6.11): C -> H the target (REQUEST,
// CANCEL) or the requester (RESPONSE); H -> C the requester (REQUEST, CANCEL) or the target
// (RESPONSE). The host overwrites/derives every slot and epoch; it never trusts the client's.
struct MpMsgInteract
{
    u8 slot;
    u8 epoch;           // epoch of `slot`
    u8 kind;            // MP_INTERACT_TRADE / BATTLE
    u8 requestId;       // chosen by the requester, echoed back
    u8 result;          // RESPONSE only: MP_INTERACT_RESULT_ACCEPTED..UNAVAILABLE; 0 otherwise
};

// LINK_* control messages (protocol.md 6.12). One 4-byte layout: groupId, arg1, arg2,
// reserved. What arg1/arg2 mean depends on the type:
//   JOIN      arg1 = member slot mask (2..4 bits of 0x0F)        groupId ignored
//   LEAVE     -                                                  groupId ignored
//   GROUP     arg1 = member slot mask, arg2 = recipient link id  groupId != 0
//   END       arg1 = reason (MP_LINK_END_*, != 0)                groupId 0 = request refused
//   OPEN / CLOSE / START   arg1 = the member's open sequence     groupId != 0
//   STATUS    arg1 = open mask by link id (< 0x10)               groupId != 0
//   BUS_START arg1 = bus epoch (!= 0), arg2 = recipient's open sequence
//   BUS_END   arg1 = bus epoch (!= 0)
// Ignored fields parse as 0.
struct MpMsgLinkCtl
{
    u8 groupId;
    u8 arg1;
    u8 arg2;
};

struct MpMsgLinkCmd
{
    u8 groupId;
    u8 busEpoch;
    u16 cmd[MP_LINK_CMD_WORDS];     // never all zero
};

struct MpMsgLinkRow
{
    u8 groupId;
    u8 busEpoch;
    u8 count;                       // link members, 2..4
    u16 rowSeq;
    u16 cmds[MP_LINK_MAX_MEMBERS][MP_LINK_CMD_WORDS]; // [link id]; rows >= count are 0
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
// INTERACT_* (PS-041): `type` is the header type (one of the three, else BAD_FIELD). Exactly
// 8 bytes; slot < 4; epoch != 0; kind TRADE or BATTLE; RESPONSE result ACCEPTED..UNAVAILABLE.
// The result byte of REQUEST / CANCEL and the reserved bytes 5..7 are ignored (parse as 0).
u8 MpProto_ParseInteract(u8 type, const u8 *payload, u32 len, struct MpMsgInteract *out);

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
// type = MP_MSG_INTERACT_REQUEST / _RESPONSE / _CANCEL (anything else: returns 0). Writes the
// result byte only for RESPONSE (0 otherwise) and the reserved bytes as 0.
u16 MpProto_WriteInteract(u8 *buf, u16 cap, u8 type, const struct MpMsgInteract *msg);

// LINK_* (PS-046). MpProto_IsLinkCtlType: one of the 4-byte control types.
bool8 MpProto_IsLinkCtlType(u8 type);
u8 MpProto_ParseLinkCtl(u8 type, const u8 *payload, u32 len, struct MpMsgLinkCtl *out);
u16 MpProto_WriteLinkCtl(u8 *buf, u16 cap, u8 type, const struct MpMsgLinkCtl *msg);
u8 MpProto_ParseLinkCmd(const u8 *payload, u32 len, struct MpMsgLinkCmd *out);
u16 MpProto_WriteLinkCmd(u8 *buf, u16 cap, const struct MpMsgLinkCmd *msg);
u8 MpProto_ParseLinkRow(const u8 *payload, u32 len, struct MpMsgLinkRow *out);
u16 MpProto_WriteLinkRow(u8 *buf, u16 cap, const struct MpMsgLinkRow *msg);
// Number of set bits in the low 4 bits (link masks).
u8 MpProto_LinkMaskCount(u8 mask);

// RFC 1982 serial-number "newer than" for u16 sequence numbers (protocol.md 6.8).
bool8 MpSeqIsNewer(u16 s, u16 last);

#endif // GUARD_MP_MP_PROTOCOL_H
