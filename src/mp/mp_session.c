// Multiplayer session: public MP_* API, state machine, player table, event
// queue, timers and the dev-harness auto-start (design.md sections 3, 5, 6).
// Host-role logic is in mp_server.c, client-role logic in mp_client.c.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "mp/mp.h"
#include "mp/mp_internal.h"
#include "mp/mp_protocol.h"
#include "mp/mp_session_internal.h"
#include "mp/mp_summary.h"
#include "mp/mp_transport.h"
#include "mp/mp_link.h"
#include "mp/mp_netstats.h"

bool8 gMpDebugLogging = FALSE;

static struct MpSession sSessions[MP_SESSION_INSTANCE_COUNT];
struct MpSession *gMpSession = &sSessions[0];

static bool8 sMpInitialized = FALSE;

// Dev-harness auto-start (instance 0 only)
static u8 sAutoStartMode = MP_AUTOSTART_NONE;
static char sAutoStartIp[MP_ADDRESS_MAX_LEN + 1];
static u16 sAutoStartPort = 0;
static bool8 sAutoStartArmed = FALSE;   // set by MP_OnContinueSavedGame

// PS-054: dropout / reconnect timers (mp_session_internal.h). MpNetTiming_Set(1) at MP_Init.
struct MpNetTiming gMpNetTiming =
{
    .enetTimeoutMinMs = MP_TRANSPORT_TIMEOUT_MIN_MS,
    .enetTimeoutMaxMs = MP_TRANSPORT_TIMEOUT_MAX_MS,
    .enetPingMs = MP_TRANSPORT_PING_MS,
    .idleTimeoutMs = MP_IDLE_TIMEOUT_MS,
    .graceMs = MP_RECONNECT_GRACE_MS,
    .retryWindowMs = MP_RECONNECT_WINDOW_MS,
    .retryFirstMs = 250,
    .retryMaxMs = 2000,
    .attemptMs = 3000,
};

// PS-054 dev tools: F4 dropout length (0 = until F4 again).
static u32 sSimDropoutMs = 0;

void MpNetTiming_Set(u32 divisor)
{
    struct MpNetTiming *t = &gMpNetTiming;

    if (divisor == 0)
        divisor = 1;
    t->enetTimeoutMinMs = MP_TRANSPORT_TIMEOUT_MIN_MS / divisor;
    t->enetTimeoutMaxMs = MP_TRANSPORT_TIMEOUT_MAX_MS / divisor;
    t->enetPingMs = MP_TRANSPORT_PING_MS / divisor;
    t->idleTimeoutMs = MP_IDLE_TIMEOUT_MS / divisor;
    if (t->idleTimeoutMs < 2500)
        t->idleTimeoutMs = 2500; // above the 1 s PING / keep-alive, which are not scaled
    t->graceMs = MP_RECONNECT_GRACE_MS / divisor;
    t->retryWindowMs = MP_RECONNECT_WINDOW_MS / divisor;
    t->retryFirstMs = 250 / divisor;
    t->retryMaxMs = 2000 / divisor;
    t->attemptMs = 3000 / divisor;
    MpTransport_SetPeerTimeouts(t->enetTimeoutMinMs, t->enetTimeoutMaxMs, t->enetPingMs);
}

// splitmix64 (public domain, Vigna)
static u64 MpSplitMix64(u64 *state)
{
    u64 z = (*state += 0x9E3779B97F4A7C15ull);

    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

void MpSession_NewToken(u8 *token)
{
    static u64 sState = 0;
    static u32 sCount = 0;
    u64 x;
    u32 i;

    if (sState == 0)
    {
        // Seed once: wall clock, CPU clock, the ENet clock, and addresses (ASLR). Where the OS has
        // /dev/urandom, mix that in too. Not cryptographic: friends over direct IP (AD-9), and a
        // token is only useful for the ~35 s a slot is held, from the client's own address range.
        sState = (u64)time(NULL) ^ ((u64)clock() << 32) ^ ((u64)MpTransport_GetTimeMs() << 16)
               ^ (u64)(uintptr_t)&x ^ ((u64)(uintptr_t)sSessions << 7);
#ifndef _WIN32
        {
            FILE *f = fopen("/dev/urandom", "rb");
            u64 r = 0;

            if (f != NULL)
            {
                if (fread(&r, 1, sizeof(r), f) == sizeof(r))
                    sState ^= r;
                fclose(f);
            }
        }
#endif
        if (sState == 0)
            sState = 0x5053504D5053504Dull;
    }
    sState ^= ((u64)MpTransport_GetTimeMs() << 20) ^ (u64)(++sCount);
    do
    {
        x = MpSplitMix64(&sState);
        for (i = 0; i < MP_SESSION_TOKEN_SIZE; i++)
            token[i] = (u8)(x >> (8 * (i % 8)));
    } while (MpProto_TokenIsZero(token));
}


// ---------------------------------------------------------------------------
// Instances (selftest)
// ---------------------------------------------------------------------------

void MpSession_SelectInstance(u8 instance)
{
    if (instance >= MP_SESSION_INSTANCE_COUNT)
        return;
    gMpSession = &sSessions[instance];
    gMpSession->instance = instance;
    MpTransport_SelectInstance(instance);
}

void MpSession_ResetInstance(u8 instance)
{
    if (instance >= MP_SESSION_INSTANCE_COUNT)
        return;
    memset(&sSessions[instance], 0, sizeof(sSessions[instance]));
    sSessions[instance].instance = instance;
    sSessions[instance].localSlot = MP_SLOT_NONE;
    MpPresence_ResetLocal(instance);
    MpLink_ResetInstance(instance); // PS-046
}

u8 MpSession_DebugHostPeerCount(void)
{
    u32 i;
    u8 count = 0;

    for (i = 0; i < MP_HOST_PEER_COUNT; i++)
    {
        if (gMpSession->peers[i].state != MP_HOST_PEER_NONE)
            count++;
    }
    return count;
}

const char *MpSession_Tag(void)
{
    static const char *const sTags[MP_SESSION_INSTANCE_COUNT] =
    {
        "", "[i1] ", "[i2] ", "[i3] ", "[i4] ", "[i5] ", "[i6] ", "[i7] ",
    };

    return sTags[gMpSession->instance < MP_SESSION_INSTANCE_COUNT ? gMpSession->instance : 0];
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

void MpSession_QueueEvent(u8 type, u8 slot, u8 reason)
{
    struct MpSession *s = gMpSession;
    struct MpEvent *ev;
    u32 index;

    if (s->eventCount >= MP_EVENT_QUEUE_SIZE)
    {
        MP_SLOG("WARNING: event queue full, dropping the oldest event (type %u)\n", (unsigned)s->events[s->eventHead].type);
        s->eventHead = (u8)((s->eventHead + 1) % MP_EVENT_QUEUE_SIZE);
        s->eventCount--;
    }
    index = (s->eventHead + s->eventCount) % MP_EVENT_QUEUE_SIZE;
    ev = &s->events[index];
    memset(ev, 0, sizeof(*ev));
    memset(ev->name, MP_EOS, sizeof(ev->name));
    ev->type = type;
    ev->slot = slot;
    ev->reason = reason;
    if (slot < MP_MAX_PLAYERS && s->slotUsed[slot])
        memcpy(ev->name, s->players[slot].summary.name, sizeof(ev->name));
    s->eventCount++;
}

void MpSession_QueueInteractEvent(u8 type, u8 slot, u8 kind, u8 result)
{
    struct MpSession *s = gMpSession;
    struct MpEvent *ev;

    MpSession_QueueEvent(type, slot, MP_REASON_NONE);
    if (s->eventCount == 0)
        return;
    ev = &s->events[(s->eventHead + s->eventCount - 1) % MP_EVENT_QUEUE_SIZE];
    ev->kind = kind;
    ev->result = result;
}

bool8 MP_PollEvent(struct MpEvent *out)
{
    struct MpSession *s = gMpSession;

    if (out == NULL)
        return FALSE;
    if (s->eventCount == 0)
    {
        memset(out, 0, sizeof(*out));
        out->type = MP_EVENT_NONE;
        out->slot = MP_SLOT_NONE;
        memset(out->name, MP_EOS, sizeof(out->name));
        return FALSE;
    }
    *out = s->events[s->eventHead];
    s->eventHead = (u8)((s->eventHead + 1) % MP_EVENT_QUEUE_SIZE);
    s->eventCount--;
    return TRUE;
}

// ---------------------------------------------------------------------------
// Player table
// ---------------------------------------------------------------------------

void MpSession_SetSlot(u8 slot, u8 epoch, const struct MpPlayerSummary *summary, bool8 isLocal)
{
    struct MpPlayer *p;

    if (slot >= MP_MAX_PLAYERS)
        return;
    p = &gMpSession->players[slot];
    memset(p, 0, sizeof(*p));
    p->slot = slot;
    p->epoch = epoch;
    p->isHost = (slot == MP_HOST_SLOT);
    p->isLocal = isLocal;
    p->summary = *summary;
    p->presence.mapGroup = MP_MAP_NONE;
    p->presence.mapNum = MP_MAP_NONE;
    p->presence.facing = DIR_SOUTH;
    p->presence.busy = MP_BUSY_NOT_IN_FIELD;
    gMpSession->slotUsed[slot] = TRUE;
    MpPresence_ResetSlot(slot);
}

void MpSession_ClearSlot(u8 slot)
{
    if (slot >= MP_MAX_PLAYERS)
        return;
    memset(&gMpSession->players[slot], 0, sizeof(gMpSession->players[slot]));
    gMpSession->slotUsed[slot] = FALSE;
    gMpSession->slotPeer[slot] = MP_PEER_NONE;
    memset(gMpSession->slotToken[slot], 0, sizeof(gMpSession->slotToken[slot])); // PS-054
    gMpSession->slotLostMs[slot] = 0;
    MpPresence_ResetSlot(slot);
}

static void MpClearTable(void)
{
    u32 i;

    for (i = 0; i < MP_MAX_PLAYERS; i++)
        MpSession_ClearSlot((u8)i);
}

void MpSession_LogTable(void)
{
    struct MpSession *s = gMpSession;
    char name[MP_PLAYER_NAME_SIZE + 1];
    u32 i;

    MP_SLOG("player table (%u):\n", (unsigned)MP_GetPlayerCount());
    for (i = 0; i < MP_MAX_PLAYERS; i++)
    {
        if (!s->slotUsed[i])
            continue;
        MP_NameToAscii(s->players[i].summary.name, name, sizeof(name));
        MP_SLOG("  slot %u epoch %u %-7s gender %u badges 0x%02X tid 0x%08X%s%s\n",
                (unsigned)i, (unsigned)s->players[i].epoch, name,
                (unsigned)s->players[i].summary.gender, (unsigned)s->players[i].summary.badges,
                (unsigned)s->players[i].summary.trainerId,
                s->players[i].isHost ? " [host]" : "", s->players[i].isLocal ? " [you]" : "");
    }
}

// ---------------------------------------------------------------------------
// Transport helpers
// ---------------------------------------------------------------------------

bool8 MpSession_SendTo(u8 peer, const u8 *data, u16 len, u8 channel, bool8 reliable)
{
    if (len == 0)
        return FALSE;
    return MpTransport_Send(peer, channel, data, len, reliable);
}

void MpSession_StartFlush(void)
{
    gMpSession->flushing = TRUE;
    gMpSession->flushStartMs = MpTransport_GetTimeMs();
}

static void MpCloseTransportNow(void)
{
    MpTransport_Close();
    gMpSession->flushing = FALSE;
    memset(gMpSession->peers, 0, sizeof(gMpSession->peers));
    memset(&gMpSession->client, 0, sizeof(gMpSession->client));
}

void MpSession_Fail(u8 event, u8 reason)
{
    struct MpSession *s = gMpSession;

    MP_SLOG("session %s: %s\n", (event == MP_EVENT_CONNECT_FAILED) ? "connect failed" : "lost", MP_GetReasonName(reason));
    s->state = MP_STATE_ERROR;
    s->errorReason = reason;
    s->localSlot = MP_SLOT_NONE;
    s->client.reconnecting = FALSE; // PS-054: gave up (or never was)
    s->client.attemptActive = FALSE;
    memset(s->client.token, 0, sizeof(s->client.token));
    MpLink_OnSessionEnd(); // PS-046: before the table is cleared
    MpClearTable();
    MpSession_QueueEvent(event, MP_SLOT_NONE, reason);
    if (MpTransport_IsOpen())
    {
        MpTransport_Disconnect(0);
        MpSession_StartFlush();
    }
}

// Transport callback while flushing (after leave / failure): only wait for disconnects.
static void MpOnFlushEvent(const struct MpTransportEvent *ev)
{
    struct MpSession *s = gMpSession;

    if (ev->type != MP_TRANSPORT_EV_DISCONNECT)
        return;
    if (!s->isHost)
    {
        s->client.transportConnected = FALSE;
    }
    else if (ev->peer < MP_HOST_PEER_COUNT)
    {
        s->peers[ev->peer].state = MP_HOST_PEER_NONE;
    }
}

static bool8 MpFlushDone(void)
{
    struct MpSession *s = gMpSession;
    u32 i;

    if (!s->isHost)
        return !s->client.transportConnected;
    for (i = 0; i < MP_HOST_PEER_COUNT; i++)
    {
        if (s->peers[i].state != MP_HOST_PEER_NONE)
            return FALSE;
    }
    return TRUE;
}

static void MpOnTransportEvent(const struct MpTransportEvent *ev)
{
    if (gMpSession->flushing)
        MpOnFlushEvent(ev);
    else if (gMpSession->isHost)
        MpServer_OnTransportEvent(ev);
    else
        MpClient_OnTransportEvent(ev);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void MP_Init(void)
{
    u32 i;

    if (sMpInitialized)
        return;
    sMpInitialized = TRUE;
    MpLog_Open(); // PS-051: every [MP] line also goes to mp_log.txt; prints its path
    for (i = 0; i < MP_SESSION_INSTANCE_COUNT; i++)
        MpSession_ResetInstance((u8)i);
    gMpSession = &sSessions[0];
    MpNetTiming_Set(1); // PS-054
    // No ENet call and no socket here: the transport initialises lazily on the
    // first Listen/Connect, so single-player never touches the network.
    MpBuild_Init();
}

void MP_SetDebugLogging(bool8 enabled)
{
    gMpDebugLogging = enabled ? TRUE : FALSE;
    if (gMpDebugLogging)
        MP_LOG("debug logging on (build %s, protocol %u)\n", MP_GetBuildId(), (unsigned)MpBuild_GetProtocolVersion());
}

static void MpRunAutoStart(void)
{
    u8 mode = sAutoStartMode;

    sAutoStartMode = MP_AUTOSTART_NONE;
    sAutoStartArmed = FALSE;
    if (mode == MP_AUTOSTART_HOST)
    {
        MP_LOG("auto-start: hosting on port %u (--host)\n", (unsigned)(sAutoStartPort ? sAutoStartPort : MP_DEFAULT_PORT));
        if (!MP_Host(sAutoStartPort))
            MP_LOG("auto-start failed (%s); playing single-player\n", MP_GetReasonName(MP_GetErrorReason()));
    }
    else if (mode == MP_AUTOSTART_JOIN)
    {
        MP_LOG("auto-start: joining %s:%u (--join)\n", sAutoStartIp, (unsigned)(sAutoStartPort ? sAutoStartPort : MP_DEFAULT_PORT));
        if (!MP_Join(sAutoStartIp, sAutoStartPort))
            MP_LOG("auto-start failed (%s); playing single-player\n", MP_GetReasonName(MP_GetErrorReason()));
    }
}

void MP_Update(void)
{
    struct MpSession *s = gMpSession;
    u32 now;

    if (sAutoStartArmed && s == &sSessions[0])
        MpRunAutoStart();

    if (!MpTransport_IsOpen())
        return;

    MpTransport_Poll(MpOnTransportEvent);
    now = MpTransport_GetTimeMs();
    MpNetStats_Update(now); // PS-053: overlay rates, --mp-net-stats summary (no-op if the poll closed it)

    if (s->flushing)
    {
        if (MpFlushDone() || (u32)(now - s->flushStartMs) >= MP_LEAVE_FLUSH_MS)
        {
            MP_SDLOG("transport closed\n");
            MpCloseTransportNow();
        }
        else
        {
            MpTransport_Flush();
        }
        return;
    }

    if (s->isHost)
        MpServer_Update(now);
    else
        MpClient_Update(now);
    if (s->state == MP_STATE_IN_SESSION && MpTransport_IsOpen() && !s->client.reconnecting) // PS-054: not while reconnecting
    {
        MpPresence_Update(now);
        MpInteract_Update(now); // PS-041 request timeouts
        MpPresence_DebugReport(now);
    }

    if (MpTransport_IsOpen())
        MpTransport_Flush();
}

void MP_Shutdown(void)
{
    u32 i, start;
    bool8 anyOpen;

    for (i = 0; i < MP_SESSION_INSTANCE_COUNT; i++)
    {
        MpSession_SelectInstance((u8)i);
        if (gMpSession->state != MP_STATE_OFFLINE || MpTransport_IsOpen())
            MP_Leave();
    }

    // Give the BYEs up to 250 ms to go out (only at exit; AD-4 is about frames).
    start = MpTransport_GetTimeMs();
    do
    {
        anyOpen = FALSE;
        for (i = 0; i < MP_SESSION_INSTANCE_COUNT; i++)
        {
            MpSession_SelectInstance((u8)i);
            if (MpTransport_IsOpen())
            {
                MP_Update();
                anyOpen = anyOpen || MpTransport_IsOpen();
            }
        }
    } while (anyOpen && (u32)(MpTransport_GetTimeMs() - start) < MP_SHUTDOWN_FLUSH_MS);

    MpSession_SelectInstance(0);
    MpTransport_Shutdown();
    MP_LOG("shut down\n");
    MpLog_Close(); // PS-051
    sMpInitialized = FALSE;
}

// ---------------------------------------------------------------------------
// Session control
// ---------------------------------------------------------------------------

// Common start of MP_Host / MP_Join: forget the previous session.
static void MpBeginSession(void)
{
    struct MpSession *s = gMpSession;
    u8 instance = s->instance;

    if (MpTransport_IsOpen())
        MpTransport_Close();
    memset(s, 0, sizeof(*s));
    s->instance = instance;
    s->localSlot = MP_SLOT_NONE;
    memset(s->slotPeer, MP_PEER_NONE, sizeof(s->slotPeer));
    s->protocolVersion = MpBuild_GetProtocolVersion();
    s->buildHash = MP_GetBuildHash();
    s->buildFlags = MpBuild_IsDirty() ? MP_BUILD_FLAG_DIRTY : 0;
    MP_BuildPlayerSummary(&s->localSummary);
    MpLink_OnSessionStart(); // PS-046
}

static void MpSetError(u8 reason)
{
    gMpSession->state = MP_STATE_ERROR;
    gMpSession->errorReason = reason;
    MP_SLOG("error: %s\n", MP_GetReasonName(reason));
}

bool8 MP_Host(u16 port)
{
    struct MpSession *s;

    MpBeginSession();
    s = gMpSession;
    if (port == 0)
        port = MP_DEFAULT_PORT;
    s->port = port;
    s->isHost = TRUE;

    if (!MpTransport_Init())
    {
        MpSetError(MP_REASON_TRANSPORT_FAILED);
        return FALSE;
    }
    if (!MpTransport_Listen(port, MP_HOST_PEER_COUNT))
    {
        MpSetError(MP_REASON_BIND_FAILED);
        return FALSE;
    }

    s->nextEpoch = 1;
    s->state = MP_STATE_IN_SESSION;
    s->localSlot = MP_HOST_SLOT;
    MpSession_SetSlot(MP_HOST_SLOT, s->nextEpoch++, &s->localSummary, TRUE);
    MP_SLOG("hosting on UDP port %u (build %s, protocol %u)\n", (unsigned)port, MP_GetBuildId(), (unsigned)s->protocolVersion);
    MpSession_LogTable();
    MpSession_QueueEvent(MP_EVENT_SESSION_STARTED, MP_HOST_SLOT, MP_REASON_NONE);
    return TRUE;
}

bool8 MP_Join(const char *ip, u16 port)
{
    struct MpSession *s;

    MpBeginSession();
    s = gMpSession;
    if (port == 0)
        port = MP_DEFAULT_PORT;
    s->port = port;
    s->isHost = FALSE;

    if (!MpTransport_IsValidIPv4(ip))
    {
        MpSetError(MP_REASON_BAD_ADDRESS);
        return FALSE;
    }
    if (!MpTransport_Init() || !MpTransport_Connect(ip, port))
    {
        MpSetError(MP_REASON_TRANSPORT_FAILED);
        return FALSE;
    }
    strncpy(s->hostIp, ip, sizeof(s->hostIp) - 1); // PS-054: for RESUME (validated above, <= 15 chars)
    s->hostIp[sizeof(s->hostIp) - 1] = '\0';
    s->state = MP_STATE_CONNECTING;
    s->client.joinMs = MpTransport_GetTimeMs();
    s->client.lastRecvMs = s->client.joinMs;
    MP_SLOG("connecting to %s:%u\n", ip, (unsigned)port);
    return TRUE;
}

void MP_Leave(void)
{
    struct MpSession *s = gMpSession;

    if (s->state == MP_STATE_IN_SESSION || s->state == MP_STATE_CONNECTING)
    {
        MP_SLOG("leaving the session\n");
        if (s->isHost)
            MpServer_BroadcastBye(MP_BYE_HOST_SHUTDOWN);
        else if (s->state == MP_STATE_IN_SESSION)
            MpClient_SendBye(MP_BYE_LEAVING);
        else
            MpTransport_Disconnect(0);
        if (MpTransport_IsOpen())
        {
            MpTransport_Flush();
            MpSession_StartFlush();
        }
        MpLink_OnSessionEnd(); // PS-046
    }
    s->state = MP_STATE_OFFLINE;
    s->errorReason = MP_REASON_NONE;
    s->localSlot = MP_SLOT_NONE;
    MpClearTable();
}

void MP_RefreshLocalSummary(void)
{
    struct MpSession *s = gMpSession;
    struct MpPlayerSummary summary;

    if (s->state != MP_STATE_IN_SESSION || s->localSlot >= MP_MAX_PLAYERS)
        return;
    MP_BuildPlayerSummary(&summary);
    if (memcmp(&summary, &s->localSummary, sizeof(summary)) == 0)
        return;
    s->localSummary = summary;
    s->players[s->localSlot].summary = summary;
    s->summaryDirty = TRUE;
}

// Called from the role updates: sends a pending PLAYER_UPDATE when the 1/s limit allows.
void MpSession_TrySendLocalUpdate(u32 now)
{
    struct MpSession *s = gMpSession;

    if (!s->summaryDirty || (s->lastUpdateSentMs != 0 && (u32)(now - s->lastUpdateSentMs) < MP_UPDATE_MIN_GAP_MS))
        return;
    s->summaryDirty = FALSE;
    s->lastUpdateSentMs = now;
    if (s->isHost)
        MpServer_SendLocalUpdate();
    else
        MpClient_SendLocalUpdate();
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

u8 MP_GetState(void)
{
    return gMpSession->state;
}

u8 MP_GetErrorReason(void)
{
    return (gMpSession->state == MP_STATE_ERROR) ? gMpSession->errorReason : MP_REASON_NONE;
}

bool8 MP_IsActive(void)
{
    return gMpSession->state == MP_STATE_IN_SESSION;
}

bool8 MP_IsReconnecting(void)
{
    return gMpSession->state == MP_STATE_IN_SESSION && !gMpSession->isHost && gMpSession->client.reconnecting;
}

// PS-054 dev tools (the game's instance; sdl2.c)
void MP_SetNetSimLoss(u8 percent)
{
    MpTransport_SetSimLoss((u16)((percent > 100 ? 100 : percent) * 10));
    if (percent != 0)
        MP_LOG("net sim: dropping %u%% of all datagrams at random (--mp-net-loss)\n", (unsigned)(percent > 100 ? 100 : percent));
}

void MP_SetNetSimDropoutLength(u32 ms)
{
    sSimDropoutMs = ms;
}

void MP_ToggleNetSimDropout(void)
{
    MpTransport_SetSimBlackout(!MpTransport_GetSimBlackout(NULL), sSimDropoutMs);
}

bool8 MP_IsHost(void)
{
    return gMpSession->state == MP_STATE_IN_SESSION && gMpSession->isHost;
}

u8 MP_GetLocalSlot(void)
{
    return (gMpSession->state == MP_STATE_IN_SESSION) ? gMpSession->localSlot : MP_SLOT_NONE;
}

u8 MP_GetPlayerCount(void)
{
    u32 i;
    u8 count = 0;

    if (gMpSession->state != MP_STATE_IN_SESSION)
        return 0;
    for (i = 0; i < MP_MAX_PLAYERS; i++)
    {
        if (gMpSession->slotUsed[i])
            count++;
    }
    return count;
}

const struct MpPlayer *MP_GetPlayer(u8 slot)
{
    if (gMpSession->state != MP_STATE_IN_SESSION || slot >= MP_MAX_PLAYERS || !gMpSession->slotUsed[slot])
        return NULL;
    return &gMpSession->players[slot];
}

u16 MP_GetPort(void)
{
    return (gMpSession->state == MP_STATE_OFFLINE) ? 0 : gMpSession->port;
}

const char *MP_GetReasonName(u8 reason)
{
    switch (reason)
    {
    case MP_REASON_NONE:             return "NONE";
    case MP_REASON_VERSION:          return "VERSION";
    case MP_REASON_BUILD:            return "BUILD";
    case MP_REASON_FULL:             return "FULL";
    case MP_REASON_BAD_DATA:         return "BAD_DATA";
    case MP_REASON_RESUME_REFUSED:   return "RESUME_REFUSED";
    case MP_REASON_RESUME_MISMATCH:  return "RESUME_MISMATCH";
    case MP_REASON_TIMEOUT:          return "TIMEOUT";
    case MP_REASON_CONNECTION_LOST:  return "CONNECTION_LOST";
    case MP_REASON_HOST_CLOSED:      return "HOST_CLOSED";
    case MP_REASON_KICKED:           return "KICKED";
    case MP_REASON_PROTOCOL_ERROR:   return "PROTOCOL_ERROR";
    case MP_REASON_BIND_FAILED:      return "BIND_FAILED";
    case MP_REASON_BAD_ADDRESS:      return "BAD_ADDRESS";
    case MP_REASON_TRANSPORT_FAILED: return "TRANSPORT_FAILED";
    case MP_REASON_LEFT:             return "LEFT";
    default:                         return "UNKNOWN";
    }
}

// ---------------------------------------------------------------------------
// Dev harness
// ---------------------------------------------------------------------------

void MP_SetAutoStart(u8 mode, const char *ip, u16 port)
{
    u32 i;

    sAutoStartMode = (mode == MP_AUTOSTART_HOST || mode == MP_AUTOSTART_JOIN) ? mode : MP_AUTOSTART_NONE;
    sAutoStartPort = port;
    sAutoStartArmed = FALSE;
    memset(sAutoStartIp, 0, sizeof(sAutoStartIp));
    for (i = 0; ip != NULL && ip[i] != '\0' && i < MP_ADDRESS_MAX_LEN; i++)
        sAutoStartIp[i] = ip[i];
    if (ip != NULL && strlen(ip) > MP_ADDRESS_MAX_LEN)
        sAutoStartIp[0] = '\0'; // too long to be an IPv4 address: MP_Join reports BAD_ADDRESS
}

void MP_OnContinueSavedGame(void)
{
    if (sAutoStartMode != MP_AUTOSTART_NONE)
        sAutoStartArmed = TRUE;
}

void MP_OnNewGame(void)
{
    if (sAutoStartMode != MP_AUTOSTART_NONE)
    {
        MP_LOG("--host/--join needs an existing save (Continue); playing single-player\n");
        sAutoStartMode = MP_AUTOSTART_NONE;
        sAutoStartArmed = FALSE;
    }
}

