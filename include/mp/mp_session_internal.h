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
#define MP_HOST_PEER_COUNT          (MP_MAX_PLAYERS + 2) // 3 clients + 1 spare for REJECT(FULL) + 2 for a reconnect that takes over a stale peer (PS-054)
#define MP_SHUTDOWN_FLUSH_MS        250
#define MP_CLOSING_PEER_MS          1000            // force-reset a peer we disconnected after this
#define MP_PING_MIN_GAP_MS          250
#define MP_UPDATE_MIN_GAP_MS        1000

// Presence (PS-030, protocol.md 6.8 / 8.4)
#define MP_PRESENCE_MIN_GAP_MS      50      // sender: at most 20 Hz
#define MP_PRESENCE_KEEPALIVE_MS    1000    // sender: resend unchanged presence
#define MP_PRESENCE_HOST_GAP_MS     25      // host: drop a client's PRESENCE closer than this
#define MP_PRESENCE_STATS_MS        10000   // --mp-debug bytes/s report interval
#define MP_PRESENCE_WIRE_OVERHEAD   40      // est. ENet (12) + UDP/IPv4 (28) bytes per packet

// Results of the presence receive paths (mp_presence.c)
enum
{
    MP_PRESENCE_ACCEPTED,
    MP_PRESENCE_STALE,      // duplicate or older seq: silent drop
    MP_PRESENCE_IGNORED,    // slot empty / own slot / epoch mismatch / rate limit: silent drop
    MP_PRESENCE_INVALID,    // failed validation: the caller counts it as malformed
};

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
    u32 lastPresenceRecvMs; // last accepted PRESENCE from this client (25 ms rule)
};

// Per remote slot: newer-than state and the FIFO consumed by MP_PopRemotePresence.
struct MpPresenceSlot
{
    bool8 seqValid;         // FALSE until the first PRESENCE after the slot was (re)assigned
    u16 lastSeq;
    u8 head;
    u8 count;
    struct MpPresence queue[MP_PRESENCE_QUEUE_SIZE];
};

// PRESENCE traffic per transport peer for the --mp-debug report (message bytes, header included).
struct MpPresenceTraffic
{
    u32 bytesOut;
    u32 packetsOut;
    u32 bytesIn;
    u32 packetsIn;
};

struct MpPresenceState
{
    bool8 sentAny;              // at least one PRESENCE sent in this session
    u16 nextSeq;                // starts at 0 when the session starts
    u32 lastSentMs;
    struct MpPresence lastSent; // includes the seq it was sent with
    struct MpPresenceSlot slots[MP_MAX_PLAYERS];
    struct MpPresenceTraffic traffic[MP_HOST_PEER_COUNT];   // client: [0] = the host
    u32 trafficStartMs;
    // Session totals (selftest and --mp-debug)
    u32 sent;
    u32 accepted;
    u32 stale;
    u32 ignored;
    u32 invalid;
};

// Interaction (PS-041, mp_interact.c).
struct MpInteractRequest
{
    bool8 active;
    u8 slot;                // the other party (target for outgoing, requester for incoming)
    u8 epoch;               // its epoch when the request started
    u8 kind;                // MP_INTERACT_*
    u8 requestId;
    u32 startMs;
};

// Host only: requests in flight, indexed by REQUESTER slot (so one per requester).
struct MpInteractRoute
{
    bool8 active;
    u8 fromEpoch;
    u8 to;
    u8 toEpoch;
    u8 kind;
    u8 requestId;
    u8 dataCount;           // PS-042: data messages forwarded for this route (limit per kind)
    u32 startMs;
};

// Requester (PS-042): data that arrived for our outgoing request, held until the RESPONSE.
// Session memory only, never the save (AD-10).
struct MpInteractRx
{
    u8 kind;                // kind of the request it belongs to (MP_INTERACT_NONE: empty)
    u8 slot;                // the target
    u8 requestId;
    bool8 bad;              // something arrived that failed validation / was unexpected
    bool8 complete;         // ACCEPTED with valid data: MP_GetViewed* may return it
    bool8 hasCard;
    struct MpTrainerCard card;
    // PS-043 VIEW_PARTY: the parts received so far (in order), every mon already validated.
    u8 partyCount;
    u8 partsReceived;
    u8 monsReceived;
    u8 mons[PARTY_SIZE][MP_MON_WIRE_SIZE];
};

struct MpInteractState
{
    struct MpInteractRequest outgoing;      // our request (we are the requester)
    struct MpInteractRequest incoming;      // the request to us (we are the target)
    u8 nextRequestId;
    struct MpInteractRoute routes[MP_MAX_PLAYERS];  // host only
    u32 lastRequestMs[MP_MAX_PLAYERS];              // host only: per requester, for the 1 s gap
    struct MpInteractRx rx;                         // PS-042: requester side
};

struct MpClientState
{
    bool8 transportConnected;
    u8 malformed;
    u32 joinMs;
    u32 lastRecvMs;
    u32 lastPingSentMs;
    u32 lastPingRecvMs;
    // PS-054 reconnect
    u8 token[MP_SESSION_TOKEN_SIZE];    // from the last WELCOME (all zero: none)
    bool8 reconnecting;                 // lost the host, retrying with RESUME (state stays IN_SESSION)
    bool8 attemptActive;                // a RESUME attempt is dialling / waiting for WELCOME
    u8 attempts;
    u32 lostMs;                         // when the connection was lost
    u32 attemptStartMs;
    u32 nextAttemptMs;
};

// PS-054: every timer of the dropout / reconnect logic. The game uses the defaults; the
// selftest scales them down so a "40 s" dropout runs in 4 s (gMpNetTiming).
struct MpNetTiming
{
    u32 enetTimeoutMinMs;   // ENet peer timeout minimum (after its retries) ...
    u32 enetTimeoutMaxMs;   // ... and maximum: a dead link is noticed after about this long
    u32 enetPingMs;         // ENet's keep-alive ping
    u32 idleTimeoutMs;      // session: nothing received for this long = connection lost
    u32 graceMs;            // host: a lost client's slot is held this long (MP_RECONNECT_GRACE_MS)
    u32 retryWindowMs;      // client: retries for this long after the loss (MP_RECONNECT_WINDOW_MS)
    u32 retryFirstMs;       // client: first wait before a RESUME attempt, doubled up to retryMaxMs
    u32 retryMaxMs;
    u32 attemptMs;          // client: one attempt (connect + RESUME + WELCOME) gives up after this
};
extern struct MpNetTiming gMpNetTiming;
// Sets gMpNetTiming to the defaults divided by `divisor` (1 = the game's values) and applies the
// ENet part to the transport (peers connecting from now on).
void MpNetTiming_Set(u32 divisor);

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
    u8 slotPeer[MP_MAX_PLAYERS];                    // host only: slot -> peer id (MP_PEER_NONE while lost)
    u8 slotToken[MP_MAX_PLAYERS][MP_SESSION_TOKEN_SIZE]; // host only (PS-054): each client's reconnect token
    u32 slotLostMs[MP_MAX_PLAYERS];                 // host only (PS-054): when players[slot].connectionLost was set
    char hostIp[MP_ADDRESS_MAX_LEN + 1];            // client (PS-054): the address MP_Join dialled

    struct MpClientState client;

    struct MpEvent events[MP_EVENT_QUEUE_SIZE];
    u8 eventHead;
    u8 eventCount;

    struct MpPlayerSummary localSummary;
    bool8 summaryDirty;
    u32 lastUpdateSentMs;

    struct MpPresenceState presence;    // PS-030
    struct MpInteractState interact;    // PS-041

    bool8 flushing;        // MP_Leave / failure: transport kept open to deliver BYE
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

// PS-054: fills `token` with MP_SESSION_TOKEN_SIZE random bytes, never all zero.
void MpSession_NewToken(u8 *token);

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

// mp_presence.c (PS-030)
void MpPresence_ResetLocal(u8 instance);                // forget the stored local presence (MP_Init, selftest)
void MpPresence_ResetSlot(u8 slot);                     // slot (re)assigned or freed: clear FIFO + seq state
// Sends the local presence when it changed (>= 50 ms since the last send) or as the 1 s
// keep-alive. Host: to every player peer; client: to the host. Call while IN_SESSION.
void MpPresence_Update(u32 now);
// Host: a PRESENCE payload from client `peer` (already in the table). Overwrites slot/epoch,
// validates, applies the 25 ms rule and the newer-than check, stores, queues and forwards.
u8 MpPresence_OnHostReceive(u8 peer, const u8 *payload, u32 len, u32 now);
// Client: a PRESENCE payload from the host. Validates, drops stale / foreign-epoch / own-slot
// updates, stores and queues.
u8 MpPresence_OnClientReceive(const u8 *payload, u32 len);
// Host: sends the cached presence of every other player (host included) to a new client.
void MpPresence_SendCachedTo(u8 peer, u8 newSlot);
// --mp-debug: presence bytes/s per peer every MP_PRESENCE_STATS_MS.
void MpPresence_DebugReport(u32 now);

// mp_interact.c (PS-041, protocol.md 6.11)
// The target's answer deadline. MP_INTERACT_TIMEOUT_MS; the selftest shortens it.
extern u32 gMpInteractTimeoutMs;
// Like MpSession_QueueEvent, with the interact fields.
void MpSession_QueueInteractEvent(u8 type, u8 slot, u8 kind, u8 result);
// Host: an INTERACT_* message from client `peer` (in the player table). Returns FALSE if it
// was malformed (the caller counts it).
bool8 MpInteract_OnHostReceive(u8 peer, u8 type, const u8 *payload, u32 len, u32 now);
// Client: an INTERACT_* message from the host. Returns FALSE if it was malformed.
bool8 MpInteract_OnClientReceive(u8 type, const u8 *payload, u32 len);
// Host: `slot` is about to leave the table (before MpSession_ClearSlot): cancel / fail the
// requests it is part of. Client: the same when PLAYER_LEFT for `slot` arrives.
void MpInteract_OnSlotLeaving(u8 slot);
// Timeouts. Call while IN_SESSION, once per MP_Update.
void MpInteract_Update(u32 now);
// PS-042: where the target's session gets the card it sends. MP_BuildLocalTrainerCard
// (the save) by default; the selftest points it at fixtures. Called on the main thread at
// the moment the request arrives.
extern void (*gMpLocalTrainerCardFunc)(struct MpTrainerCard *out);
// PS-043: where the target's session gets the party it sends after ACCEPTED. MP_BuildLocalParty
// (gPlayerParty, read only) by default; the selftest points it at fixtures. Returns the count.
extern u8 (*gMpLocalPartyFunc)(u8 mons[PARTY_SIZE][MP_MON_WIRE_SIZE]);

#define MP_SLOG(fmt, ...) MP_LOG("%s" fmt, MpSession_Tag(), ##__VA_ARGS__)
#define MP_SDLOG(fmt, ...) MP_DLOG("%s" fmt, MpSession_Tag(), ##__VA_ARGS__)

#endif // GUARD_MP_MP_SESSION_INTERNAL_H
