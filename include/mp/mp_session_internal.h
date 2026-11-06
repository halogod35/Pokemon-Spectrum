#ifndef GUARD_MP_MP_SESSION_INTERNAL_H
#define GUARD_MP_MP_SESSION_INTERNAL_H

// Session state shared by mp_session.c (public API, events, timers),
// mp_server.c (host role) and mp_client.c (client role). Internal to src/mp.
//
// All state lives in a struct MpSession. The game uses instance 0 only; the
// --mp-selftest runner drives several instances (a host and clients) in one
// process with MpSession_SelectInstance(), which also selects the matching
// transport instance.

#include "mp/mp.h"
#include "mp/mp_internal.h"
#include "mp/mp_protocol.h"
#include "mp/mp_transport.h"

#define MP_SESSION_INSTANCE_COUNT   MP_TRANSPORT_INSTANCE_COUNT
#define MP_MALFORMED_LIMIT          8
#define MP_HOST_PEER_COUNT          MP_MAX_PLAYERS  // 3 clients + 1 spare for REJECT(FULL)
#define MP_SHUTDOWN_FLUSH_MS        250
#define MP_CLOSING_PEER_MS          1000            // force-reset a peer we disconnected after this
#define MP_PING_MIN_GAP_MS          250
#define MP_UPDATE_MIN_GAP_MS        1000

enum
{
    MP_HOST_PEER_NONE,
    MP_HOST_PEER_PENDING,   // ENet connected, waiting for HELLO
    MP_HOST_PEER_PLAYER,    // in the player table
    MP_HOST_PEER_CLOSING,   // rejected / kicked / left; waiting for the ENet disconnect
};

struct MpHostPeer
{
    u8 state;               // MP_HOST_PEER_*
    u8 slot;                // valid in PLAYER state
    u8 malformed;
    u32 sinceMs;            // connect time (PENDING) or close time (CLOSING)
    u32 lastRecvMs;
    u32 lastPingSentMs;
    u32 lastPingRecvMs;
    u32 lastUpdateRecvMs;
};

struct MpClientState
{
    bool8 transportConnected;
    u8 malformed;
    u32 joinMs;
    u32 lastRecvMs;
    u32 lastPingSentMs;
    u32 lastPingRecvMs;
};

struct MpSession
{
    u8 state;               // MP_STATE_*
    u8 errorReason;         // MP_REASON_*
    bool8 isHost;
    u8 localSlot;
    u16 port;
    u8 instance;            // transport instance used by this session

    // Identity captured at MP_Host / MP_Join (so test overrides stay per instance)
    u16 protocolVersion;
    u32 buildHash;
    u8 buildFlags;

    bool8 slotUsed[MP_MAX_PLAYERS];
    struct MpPlayer players[MP_MAX_PLAYERS];
    u8 nextEpoch;           // host only

    struct MpHostPeer peers[MP_HOST_PEER_COUNT];   // host only, indexed by transport peer id
    u8 slotPeer[MP_MAX_PLAYERS];                    // host only: slot -> peer id

    struct MpClientState client;

    struct MpEvent events[MP_EVENT_QUEUE_SIZE];
    u8 eventHead;
    u8 eventCount;

    struct MpPlayerSummary localSummary;
    bool8 summaryDirty;
    u32 lastUpdateSentMs;

    bool8 flushing;         // MP_Leave / failure: transport kept open to deliver BYE
    u32 flushStartMs;
};

extern struct MpSession *gMpSession;

// mp_session.c
void MpSession_SelectInstance(u8 instance);             // selftest only
void MpSession_ResetInstance(u8 instance);              // selftest only: forget everything (simulated crash)
u8 MpSession_DebugHostPeerCount(void);                  // selftest only: host peers not NONE
const char *MpSession_Tag(void);                        // "" for instance 0, "[i3] " otherwise
void MpSession_QueueEvent(u8 type, u8 slot, u8 reason);
void MpSession_LogTable(void);
void MpSession_SetSlot(u8 slot, u8 epoch, const struct MpPlayerSummary *summary, bool8 isLocal);
void MpSession_ClearSlot(u8 slot);
void MpSession_Fail(u8 event, u8 reason);               // client: ERROR + event + flush the transport
void MpSession_StartFlush(void);
bool8 MpSession_SendTo(u8 peer, const u8 *data, u16 len, u8 channel, bool8 reliable);
void MpSession_TrySendLocalUpdate(u32 now);             // pending PLAYER_UPDATE, 1/s limit

// mp_server.c (host role)
void MpServer_OnTransportEvent(const struct MpTransportEvent *ev);
void MpServer_Update(u32 now);
void MpServer_BroadcastBye(u8 byeReason);
void MpServer_SendLocalUpdate(void);

// mp_client.c (client role)
void MpClient_OnTransportEvent(const struct MpTransportEvent *ev);
void MpClient_Update(u32 now);
void MpClient_SendBye(u8 byeReason);
void MpClient_SendLocalUpdate(void);

#define MP_SLOG(fmt, ...) MP_LOG("%s" fmt, MpSession_Tag(), ##__VA_ARGS__)
#define MP_SDLOG(fmt, ...) MP_DLOG("%s" fmt, MpSession_Tag(), ##__VA_ARGS__)

#endif // GUARD_MP_MP_SESSION_INTERNAL_H
