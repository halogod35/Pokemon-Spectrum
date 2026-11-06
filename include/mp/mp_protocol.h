#ifndef GUARD_MP_MP_PROTOCOL_H
#define GUARD_MP_MP_PROTOCOL_H

// Wire protocol constants (docs/multiplayer/protocol.md). Internal to src/mp.
// PS-011 creates this header with the constants; PS-012 adds the message
// structs and the pure parsers/writers (protocol.md section 8.3).

#include "gba/types.h"

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

#endif // GUARD_MP_MP_PROTOCOL_H
