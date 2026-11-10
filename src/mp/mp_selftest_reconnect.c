// --mp-selftest, PS-054 group: surviving dropouts.
//
// A host, a client that loses its signal (C1) and a client that watches (C2), in one process.
// The dropout is the transport's simulated blackout (MpTransport_SetSimBlackout): nothing is sent
// or received by C1, as when a phone loses its signal.
//
// Real timing (the game's timers):
//   - 1 s dropout: ENet rides it out; nobody notices (no reconnect, no notice).
//   - 10 % random packet loss for 2 s: nothing breaks.
//   - 5 s dropout: the session survives with the same slot and epoch (reconnect or ride-out).
//   - RESUME with a wrong token, the token that was already used (a replay), the right token but
//     a different save (a restarted client), or for a free slot: REJECT, and C1 is not disturbed.
// Scaled timing (every PS-054 timer divided by 10, so "20 s" takes 2 s):
//   - "20 s" dropout: C1 reconnects silently with the same slot and epoch; host and C2 see the
//     slot held (PLAYER_STATUS) and back; nobody sees PLAYER_LEFT; requests to C1 are refused.
//   - a trade in its commit when C1 drops: (a) the host committed it: C2 COMMIT, C1 presumes
//     ABORT, and after the reconnect the host tells C1's journal COMMITTED; (b) the host had not:
//     both ABORT and the host tells C1's journal ABORTED. The link group ends for both and they
//     can link again after the reconnect.
//   - "40 s" dropout: C1 gives up (DISCONNECTED(CONNECTION_LOST)) and the host frees the slot
//     (PLAYER_LEFT(CONNECTION_LOST) on the host and C2), as before PS-054.

#include <string.h>
#include "mp/mp.h"
#include "mp/mp_internal.h"
#include "mp/mp_protocol.h"
#include "mp/mp_selftest.h"
#include "mp/mp_session_internal.h"
#include "mp/mp_summary.h"
#include "mp/mp_transport.h"

static u32 sFail;

#define R_CHECK(cond, what)                                                         \
    do                                                                              \
    {                                                                               \
        if (!(cond))                                                                \
        {                                                                           \
            MP_LOG("selftest: FAIL: reconnect: %s (%s:%d)\n", what, __FILE__, __LINE__); \
            sFail++;                                                                \
        }                                                                           \
    } while (0)

#define R_HOST      0
#define R_C1        1   // drops
#define R_C2        2   // watches
#define R_RAW       3   // transport only: hand-made RESUME
#define R_SESSIONS  3   // 0..2 run the session code
#define RECONNECT_PORT_FIRST 27600
#define RECONNECT_PORT_LAST  27609

struct ReconnectEvents
{
    u8 reconnecting;
    u8 reconnected;
    u8 playerReconnecting;
    u8 playerReconnected;
    u8 joined;
    u8 left;
    u8 disconnected;
    u8 lastLeftReason;
    u8 lastDisconnectReason;
};

static struct ReconnectEvents sEv[R_SESSIONS];
static u16 sPort;

static struct
{
    bool8 connected;
    u8 rejectReason;
    bool8 welcomed;
} sRaw;

static void Sel(u8 instance)
{
    MpSession_SelectInstance(instance);
}

static void Report(const char *what, u32 failuresBefore)
{
    if (sFail == failuresBefore)
        MP_LOG("selftest: reconnect: %s OK\n", what);
}

static void Drain(u8 instance)
{
    struct MpEvent ev;
    struct ReconnectEvents *c = &sEv[instance];

    while (MP_PollEvent(&ev))
    {
        switch (ev.type)
        {
        case MP_EVENT_RECONNECTING:         c->reconnecting++; break;
        case MP_EVENT_RECONNECTED:          c->reconnected++; break;
        case MP_EVENT_PLAYER_RECONNECTING:  c->playerReconnecting++; break;
        case MP_EVENT_PLAYER_RECONNECTED:   c->playerReconnected++; break;
        case MP_EVENT_PLAYER_JOINED:        c->joined++; break;
        case MP_EVENT_PLAYER_LEFT:          c->left++; c->lastLeftReason = ev.reason; break;
        case MP_EVENT_DISCONNECTED:         c->disconnected++; c->lastDisconnectReason = ev.reason; break;
        }
    }
}

static void OnRawEvent(const struct MpTransportEvent *ev)
{
    struct MpMsgHeader hdr;
    struct MpMsgReject reject;

    switch (ev->type)
    {
    case MP_TRANSPORT_EV_CONNECT:
        sRaw.connected = TRUE;
        break;
    case MP_TRANSPORT_EV_RECEIVE:
        if (!MpProto_ParseHeader(ev->data, ev->length, &hdr))
            break;
        if (hdr.type == MP_MSG_REJECT && MpProto_ParseReject(ev->data + MP_HEADER_SIZE, hdr.length, &reject) == MP_PARSE_OK)
            sRaw.rejectReason = reject.reason;
        else if (hdr.type == MP_MSG_WELCOME)
            sRaw.welcomed = TRUE;
        break;
    }
}

static void PumpOnce(void)
{
    u8 i;

    for (i = 0; i < R_SESSIONS; i++)
    {
        Sel(i);
        MP_Update();
        Drain(i);
    }
    Sel(R_RAW);
    if (MpTransport_IsOpen())
    {
        MpTransport_Poll(OnRawEvent);
        MpTransport_Flush();
    }
}

static void PumpFor(u32 ms)
{
    u32 start = MpTransport_GetTimeMs();

    while ((u32)(MpTransport_GetTimeMs() - start) < ms)
        PumpOnce();
}

typedef bool8 (*RCond)(void);

static bool8 PumpUntil(RCond cond, u32 timeoutMs)
{
    u32 start = MpTransport_GetTimeMs();

    while (!cond() && (u32)(MpTransport_GetTimeMs() - start) < timeoutMs)
        PumpOnce();
    return cond();
}

static u8 StateOf(u8 instance)
{
    Sel(instance);
    return MP_GetState();
}

static bool8 IsReconnecting(u8 instance)
{
    Sel(instance);
    return MP_IsReconnecting();
}

static bool8 HostSeesLost(u8 slot)
{
    Sel(R_HOST);
    return gMpSession->slotUsed[slot] && gMpSession->players[slot].connectionLost;
}

static bool8 HostHasPeerFor(u8 slot)
{
    Sel(R_HOST);
    return gMpSession->slotUsed[slot] && gMpSession->slotPeer[slot] != MP_PEER_NONE;
}

static bool8 CondC1InSession(void)      { return StateOf(R_C1) == MP_STATE_IN_SESSION; }
static bool8 CondC2InSession(void)      { return StateOf(R_C2) == MP_STATE_IN_SESSION; }
static bool8 CondC1Reconnecting(void)   { return IsReconnecting(R_C1); }
static bool8 CondHostSeesC1Lost(void)   { return HostSeesLost(1); }
static bool8 CondRawConnected(void)     { return sRaw.connected; }
static bool8 CondRawAnswered(void)      { return sRaw.rejectReason != 0 || sRaw.welcomed; }
static bool8 CondHostTwoPeers(void)     { Sel(R_HOST); return MpSession_DebugHostPeerCount() == 2; }
static bool8 CondC1Ungrouped(void)      { Sel(R_C1); return MP_LinkGetState() == MP_LINK_STATE_NONE; }
static bool8 CondC2Ungrouped(void)      { Sel(R_C2); return MP_LinkGetState() == MP_LINK_STATE_NONE; }
static bool8 CondBothGrouped(void)
{
    Sel(R_C1);
    if (MP_LinkGetState() != MP_LINK_STATE_GROUPED)
        return FALSE;
    Sel(R_C2);
    return MP_LinkGetState() == MP_LINK_STATE_GROUPED;
}

static bool8 CondC1Decided(void)
{
    u8 st;

    Sel(R_C1);
    st = MP_TradeCommitPoll();
    return st == MP_TRADE_COMMIT_COMMIT || st == MP_TRADE_COMMIT_ABORT;
}

static bool8 CondC2Decided(void)
{
    u8 st;

    Sel(R_C2);
    st = MP_TradeCommitPoll();
    return st == MP_TRADE_COMMIT_COMMIT || st == MP_TRADE_COMMIT_ABORT;
}

// Back to normal after a dropout: C1 in session and not reconnecting, the host has a peer for
// slot 1 and does not hold it as lost, C2 does not see it as lost.
static bool8 CondSettled(void)
{
    const struct MpPlayer *p;

    if (StateOf(R_C1) != MP_STATE_IN_SESSION || IsReconnecting(R_C1) || HostSeesLost(1) || !HostHasPeerFor(1))
        return FALSE;
    Sel(R_C2);
    p = MP_GetPlayer(1);
    return p != NULL && !p->connectionLost;
}

static bool8 CondHostFreedC1(void)
{
    Sel(R_HOST);
    return !gMpSession->slotUsed[1];
}

static bool8 CondC1Error(void)
{
    return StateOf(R_C1) == MP_STATE_ERROR;
}

static bool8 JoinAs(u8 instance, const char *name)
{
    Sel(instance);
    if (!MP_SetNameOverride(name))
        return FALSE;
    if (!MP_Join("127.0.0.1", sPort))
        return FALSE;
    return PumpUntil(instance == R_C1 ? CondC1InSession : CondC2InSession, 5000);
}

// Host + C1 (slot 1) + C2 (slot 2) with the current gMpNetTiming.
static bool8 StartSession(void)
{
    bool8 hosting = FALSE;
    u8 i;

    for (i = 0; i <= R_RAW; i++)
    {
        Sel(i);
        MpTransport_SetSimBlackout(FALSE, 0);
        MpTransport_SetSimLoss(0);
        MpSession_ResetInstance(i);
    }
    memset(sEv, 0, sizeof(sEv));
    Sel(R_HOST);
    MP_SetNameOverride("Host");
    for (sPort = RECONNECT_PORT_FIRST; sPort <= RECONNECT_PORT_LAST && !hosting; sPort++)
        hosting = MP_Host(sPort);
    sPort--;
    R_CHECK(hosting, "host on a port in 27600..27609");
    if (!hosting)
        return FALSE;
    R_CHECK(JoinAs(R_C1, "Drop"), "C1 joins");
    R_CHECK(JoinAs(R_C2, "Obs"), "C2 joins");
    PumpFor(300);
    Sel(R_C1);
    R_CHECK(MP_GetLocalSlot() == 1 && !MpProto_TokenIsZero(gMpSession->client.token), "C1 is slot 1 and has a token");
    Sel(R_C2);
    R_CHECK(MP_GetLocalSlot() == 2 && MP_GetPlayerCount() == 3, "C2 is slot 2, 3 players");
    memset(sEv, 0, sizeof(sEv));
    if (MP_GetLocalSlot() != 2 || MP_GetPlayerCount() != 3)
        return FALSE;
    Sel(R_C1);
    return MP_GetLocalSlot() == 1;
}

static void StopSession(void)
{
    u32 start;
    bool8 open;
    u8 i;

    for (i = 0; i < R_SESSIONS; i++)
    {
        Sel(i);
        MpTransport_SetSimBlackout(FALSE, 0);
        MP_Leave();
    }
    start = MpTransport_GetTimeMs();
    do
    {
        PumpOnce();
        open = FALSE;
        for (i = 0; i < R_SESSIONS; i++)
        {
            Sel(i);
            open = open || MpTransport_IsOpen();
        }
    } while (open && (u32)(MpTransport_GetTimeMs() - start) < 3000);
    for (i = 0; i <= R_RAW; i++)
    {
        Sel(i);
        MpTransport_Close();
        MpTransport_SetSimBlackout(FALSE, 0);
        MpTransport_SetSimLoss(0);
        MpSession_ResetInstance(i);
    }
    Sel(0);
}

static u8 EpochOf(u8 instance, u8 slot)
{
    const struct MpPlayer *p;

    Sel(instance);
    p = MP_GetPlayer(slot);
    return (p != NULL) ? p->epoch : 0;
}

// Same 3-player table on host, C1 and C2 (slots and epochs).
static void CheckTables(const char *what)
{
    u8 slot, i;
    static const u8 sOthers[] = { R_C1, R_C2 };

    Sel(R_HOST);
    R_CHECK(MP_GetPlayerCount() == 3, what);
    for (i = 0; i < 2; i++)
    {
        Sel(sOthers[i]);
        R_CHECK(MP_GetState() == MP_STATE_IN_SESSION && MP_GetPlayerCount() == 3, what);
        for (slot = 0; slot < 3; slot++)
            R_CHECK(EpochOf(sOthers[i], slot) == EpochOf(R_HOST, slot) && EpochOf(R_HOST, slot) != 0, what);
    }
}

// ---------------------------------------------------------------------------
// Raw RESUME
// ---------------------------------------------------------------------------

// Sends a RESUME for (slot, epoch, token, summary) from a fresh raw connection. Returns the
// REJECT reason (0 if welcomed or no answer); sRaw.welcomed tells the rest.
static u8 RawResume(u8 slot, u8 epoch, const u8 *token, const struct MpPlayerSummary *summary)
{
    struct MpMsgResume resume;
    u8 buf[MP_MAX_PACKET_SIZE];
    u16 len;

    memset(&sRaw, 0, sizeof(sRaw));
    Sel(R_RAW);
    if (!MpTransport_Connect("127.0.0.1", sPort) || !PumpUntil(CondRawConnected, 3000))
    {
        R_CHECK(FALSE, "raw connection for RESUME");
        Sel(R_RAW);
        MpTransport_Close();
        return 0;
    }
    memset(&resume, 0, sizeof(resume));
    resume.protocolVersion = MP_PROTOCOL_VERSION;
    resume.buildHash = MP_GetBuildHash();
    resume.slot = slot;
    resume.epoch = epoch;
    resume.summary = *summary;
    memcpy(resume.token, token, MP_SESSION_TOKEN_SIZE);
    len = MpProto_WriteResume(buf, sizeof(buf), &resume);
    R_CHECK(len == MP_HEADER_SIZE + MP_RESUME_SIZE, "raw RESUME written");
    Sel(R_RAW);
    MpTransport_Send(0, MP_CHANNEL_CONTROL, buf, len, TRUE);
    PumpUntil(CondRawAnswered, 3000);
    Sel(R_RAW);
    MpTransport_Close();
    // The host frees the refused peer (closing, then reset after MP_CLOSING_PEER_MS), so the next
    // raw connection finds a free one.
    R_CHECK(PumpUntil(CondHostTwoPeers, 3000), "the host frees the raw peer");
    return sRaw.rejectReason;
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

static void TestParsers(void)
{
    u32 failuresBefore = sFail;
    struct MpMsgResume resume, rout;
    struct MpMsgPlayerStatus status, sout;
    u8 buf[MP_MAX_PACKET_SIZE];
    u8 copy[MP_MAX_PACKET_SIZE];
    u32 i, bit, n, len;
    u16 written;
    u32 rng = 0x54045404u;

    memset(&resume, 0, sizeof(resume));
    resume.protocolVersion = MP_PROTOCOL_VERSION;
    resume.buildHash = 0xA1B2C3D4u;
    resume.slot = 2;
    resume.epoch = 7;
    memset(resume.summary.name, MP_EOS, sizeof(resume.summary.name));
    resume.summary.name[0] = 0xBB;
    resume.summary.trainerId = 0x12345678u;
    for (i = 0; i < MP_SESSION_TOKEN_SIZE; i++)
        resume.token[i] = (u8)(0x11 * (i + 1));
    written = MpProto_WriteResume(buf, sizeof(buf), &resume);
    R_CHECK(written == MP_HEADER_SIZE + MP_RESUME_SIZE, "RESUME size");
    R_CHECK(MpProto_ParseResume(&buf[MP_HEADER_SIZE], MP_RESUME_SIZE, &rout) == MP_PARSE_OK
         && rout.slot == 2 && rout.epoch == 7 && memcmp(rout.token, resume.token, MP_SESSION_TOKEN_SIZE) == 0
         && rout.summary.trainerId == 0x12345678u, "RESUME round trip");
    for (len = 0; len < MP_RESUME_SIZE; len++)
        R_CHECK(MpProto_ParseResume(&buf[MP_HEADER_SIZE], len, &rout) != MP_PARSE_OK, "RESUME truncated");
    R_CHECK(MpProto_ParseResume(&buf[MP_HEADER_SIZE], MP_RESUME_SIZE + 1, &rout) != MP_PARSE_OK, "RESUME oversized");
    memcpy(copy, &buf[MP_HEADER_SIZE], MP_RESUME_SIZE);
    copy[13] = 0;
    R_CHECK(MpProto_ParseResume(copy, MP_RESUME_SIZE, &rout) == MP_PARSE_ERR_BAD_FIELD, "RESUME slot 0 (the host)");
    copy[13] = MP_MAX_PLAYERS;
    R_CHECK(MpProto_ParseResume(copy, MP_RESUME_SIZE, &rout) == MP_PARSE_ERR_BAD_FIELD, "RESUME slot 4");
    memcpy(copy, &buf[MP_HEADER_SIZE], MP_RESUME_SIZE);
    copy[14] = 0;
    R_CHECK(MpProto_ParseResume(copy, MP_RESUME_SIZE, &rout) == MP_PARSE_ERR_BAD_FIELD, "RESUME epoch 0");
    memcpy(copy, &buf[MP_HEADER_SIZE], MP_RESUME_SIZE);
    memset(&copy[32], 0, MP_SESSION_TOKEN_SIZE);
    R_CHECK(MpProto_ParseResume(copy, MP_RESUME_SIZE, &rout) == MP_PARSE_ERR_BAD_FIELD, "RESUME zero token");
    memcpy(copy, &buf[MP_HEADER_SIZE], MP_RESUME_SIZE);
    copy[16] = MP_EOS;
    R_CHECK(MpProto_ParseResume(copy, MP_RESUME_SIZE, &rout) == MP_PARSE_ERR_BAD_SUMMARY, "RESUME empty name");
    memcpy(copy, &buf[MP_HEADER_SIZE], MP_RESUME_SIZE);
    copy[0] = 0;
    R_CHECK(MpProto_ParseResume(copy, MP_RESUME_SIZE, &rout) == MP_PARSE_ERR_BAD_MAGIC, "RESUME bad magic");
    for (i = 0; i < MP_RESUME_SIZE; i++)
    {
        for (bit = 0; bit < 8; bit++)
        {
            u8 r1, r2;

            memcpy(copy, &buf[MP_HEADER_SIZE], MP_RESUME_SIZE);
            copy[i] ^= (u8)(1 << bit);
            r1 = MpProto_ParseResume(copy, MP_RESUME_SIZE, &rout);
            r2 = MpProto_ParseResume(copy, MP_RESUME_SIZE, &rout);
            R_CHECK(r1 == r2, "RESUME bit flip deterministic");
            if (r1 == MP_PARSE_OK)
                R_CHECK(rout.slot >= 1 && rout.slot < MP_MAX_PLAYERS && rout.epoch != 0 && !MpProto_TokenIsZero(rout.token), "RESUME accepted fields in range");
        }
    }

    status.slot = 3;
    status.epoch = 9;
    status.status = MP_PLAYER_STATUS_LOST;
    written = MpProto_WritePlayerStatus(buf, sizeof(buf), &status);
    R_CHECK(written == MP_HEADER_SIZE + MP_PLAYER_STATUS_SIZE, "PLAYER_STATUS size");
    R_CHECK(MpProto_ParsePlayerStatus(&buf[MP_HEADER_SIZE], MP_PLAYER_STATUS_SIZE, &sout) == MP_PARSE_OK
         && sout.slot == 3 && sout.epoch == 9 && sout.status == MP_PLAYER_STATUS_LOST, "PLAYER_STATUS round trip");
    for (len = 0; len < MP_PLAYER_STATUS_SIZE; len++)
        R_CHECK(MpProto_ParsePlayerStatus(&buf[MP_HEADER_SIZE], len, &sout) != MP_PARSE_OK, "PLAYER_STATUS truncated");
    R_CHECK(MpProto_ParsePlayerStatus(&buf[MP_HEADER_SIZE], MP_PLAYER_STATUS_SIZE + 1, &sout) != MP_PARSE_OK, "PLAYER_STATUS oversized");
    memcpy(copy, &buf[MP_HEADER_SIZE], MP_PLAYER_STATUS_SIZE);
    copy[0] = 0;
    R_CHECK(MpProto_ParsePlayerStatus(copy, MP_PLAYER_STATUS_SIZE, &sout) == MP_PARSE_ERR_BAD_FIELD, "PLAYER_STATUS slot 0");
    copy[0] = 1;
    copy[1] = 0;
    R_CHECK(MpProto_ParsePlayerStatus(copy, MP_PLAYER_STATUS_SIZE, &sout) == MP_PARSE_ERR_BAD_FIELD, "PLAYER_STATUS epoch 0");
    copy[1] = 1;
    copy[2] = 2;
    R_CHECK(MpProto_ParsePlayerStatus(copy, MP_PLAYER_STATUS_SIZE, &sout) == MP_PARSE_ERR_BAD_FIELD, "PLAYER_STATUS status 2");
    R_CHECK(MpProto_ParsePlayerStatus(NULL, 0, &sout) != MP_PARSE_OK, "PLAYER_STATUS NULL");

    // Garbage: no crash, nothing out of range accepted.
    for (n = 0; n < 50000; n++)
    {
        len = (rng % 48);
        for (i = 0; i < len; i++)
        {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            copy[i] = (u8)(rng >> 24);
        }
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        if (MpProto_ParseResume(copy, len, &rout) == MP_PARSE_OK)
            R_CHECK(rout.slot >= 1 && rout.slot < MP_MAX_PLAYERS && !MpProto_TokenIsZero(rout.token), "garbage RESUME in range");
        if (MpProto_ParsePlayerStatus(copy, len, &sout) == MP_PARSE_OK)
            R_CHECK(sout.slot >= 1 && sout.slot < MP_MAX_PLAYERS && sout.status <= MP_PLAYER_STATUS_LOST, "garbage PLAYER_STATUS in range");
    }

    // Tokens: never zero, and not the same twice.
    {
        u8 a[MP_SESSION_TOKEN_SIZE], b[MP_SESSION_TOKEN_SIZE];

        MpSession_NewToken(a);
        MpSession_NewToken(b);
        R_CHECK(!MpProto_TokenIsZero(a) && !MpProto_TokenIsZero(b) && memcmp(a, b, sizeof(a)) != 0, "two fresh tokens differ and are not zero");
    }
    Report("RESUME / PLAYER_STATUS parsers (round trip, truncated, fields, bit flips, 50000 garbage), tokens", failuresBefore);
}

// ---------------------------------------------------------------------------
// Real timing
// ---------------------------------------------------------------------------

static void TestRealTiming(void)
{
    u32 failuresBefore;
    u8 oldToken[MP_SESSION_TOKEN_SIZE];
    u8 curToken[MP_SESSION_TOKEN_SIZE];
    struct MpPlayerSummary c1Summary, other;
    u8 epoch1;
    u32 start, elapsed, dropped;
    bool8 reconnected;

    MpNetTiming_Set(1);
    if (!StartSession())
        return;
    epoch1 = EpochOf(R_HOST, 1);

    // 1 s: ENet rides it out.
    failuresBefore = sFail;
    Sel(R_C1);
    MpTransport_SetSimBlackout(TRUE, 1000);
    PumpFor(1000 + 1500);
    R_CHECK(StateOf(R_C1) == MP_STATE_IN_SESSION && !IsReconnecting(R_C1), "1 s: C1 still in session");
    R_CHECK(sEv[R_C1].reconnecting == 0 && sEv[R_HOST].playerReconnecting == 0 && sEv[R_C2].playerReconnecting == 0,
            "1 s: no reconnect anywhere");
    R_CHECK(sEv[R_HOST].left == 0 && sEv[R_C2].left == 0, "1 s: no PLAYER_LEFT");
    R_CHECK(HostHasPeerFor(1) && EpochOf(R_C2, 1) == epoch1, "1 s: same slot and epoch");
    CheckTables("1 s: same tables");
    Report("1 s dropout is ridden out silently", failuresBefore);

    // 10 % random loss on C1 for 2 s.
    failuresBefore = sFail;
    Sel(R_C1);
    dropped = MpTransport_GetSimDropped();
    MpTransport_SetSimLoss(100);
    PumpFor(2000);
    Sel(R_C1);
    R_CHECK(MpTransport_GetSimDropped() > dropped, "10 % loss: datagrams were dropped");
    MpTransport_SetSimLoss(0);
    PumpFor(500);
    R_CHECK(StateOf(R_C1) == MP_STATE_IN_SESSION && sEv[R_C1].reconnecting == 0 && sEv[R_HOST].left == 0, "10 % loss: session undisturbed");
    CheckTables("10 % loss: same tables");
    Report("10 % packet loss for 2 s changes nothing", failuresBefore);

    // 5 s: the session survives with the same slot and epoch.
    failuresBefore = sFail;
    memset(sEv, 0, sizeof(sEv));
    Sel(R_C1);
    memcpy(oldToken, gMpSession->client.token, sizeof(oldToken));
    c1Summary = gMpSession->localSummary;
    MpTransport_SetSimBlackout(TRUE, 5000);
    start = MpTransport_GetTimeMs();
    PumpFor(2000);
    R_CHECK(StateOf(R_C1) == MP_STATE_IN_SESSION && MP_GetPlayerCount() == 3, "5 s: C1 keeps its session and table during the dropout");
    PumpFor(3000);
    R_CHECK(PumpUntil(CondSettled, 12000), "5 s: back to normal after the signal returns");
    elapsed = MpTransport_GetTimeMs() - start;
    reconnected = sEv[R_C1].reconnected != 0;
    MP_LOG("selftest: reconnect: 5 s dropout: settled after %u ms (%s; C1 reconnecting %u, reconnected %u; host saw lost %u, back %u)\n",
           (unsigned)elapsed, reconnected ? "reconnected" : "ridden out", (unsigned)sEv[R_C1].reconnecting, (unsigned)sEv[R_C1].reconnected,
           (unsigned)sEv[R_HOST].playerReconnecting, (unsigned)sEv[R_HOST].playerReconnected);
    R_CHECK(sEv[R_C1].reconnecting == sEv[R_C1].reconnected, "5 s: every RECONNECTING ended in RECONNECTED");
    R_CHECK(sEv[R_HOST].left == 0 && sEv[R_C2].left == 0 && sEv[R_C1].left == 0, "5 s: no PLAYER_LEFT anywhere");
    R_CHECK(sEv[R_C1].disconnected == 0, "5 s: C1 not disconnected");
    R_CHECK(sEv[R_C2].playerReconnecting == sEv[R_C2].playerReconnected, "5 s: C2 saw the slot come back");
    R_CHECK(EpochOf(R_C1, 1) == epoch1 && EpochOf(R_HOST, 1) == epoch1, "5 s: same epoch");
    CheckTables("5 s: same tables");
    Sel(R_C1);
    memcpy(curToken, gMpSession->client.token, sizeof(curToken));
    if (reconnected)
        R_CHECK(memcmp(oldToken, curToken, sizeof(oldToken)) != 0, "5 s: the RESUME got a new token");
    Report("5 s dropout: same slot and epoch, nobody saw PLAYER_LEFT", failuresBefore);

    // Tokens (AD-9). C1 is connected throughout and must not be disturbed.
    failuresBefore = sFail;
    memset(sEv, 0, sizeof(sEv));
    {
        u8 wrong[MP_SESSION_TOKEN_SIZE];

        memcpy(wrong, curToken, sizeof(wrong));
        wrong[3] ^= 0x5A;
        R_CHECK(RawResume(1, epoch1, wrong, &c1Summary) == MP_REASON_RESUME_REFUSED && !sRaw.welcomed, "wrong token: REJECT(RESUME_REFUSED)");
        R_CHECK(RawResume(1, epoch1, oldToken, &c1Summary) == MP_REASON_RESUME_REFUSED && !sRaw.welcomed,
                "replayed token (already used, or never handed out): REJECT(RESUME_REFUSED)");
        R_CHECK(RawResume(1, (u8)(epoch1 + 1), curToken, &c1Summary) == MP_REASON_RESUME_REFUSED, "right token, wrong epoch: REJECT(RESUME_REFUSED)");
        R_CHECK(RawResume(3, epoch1, curToken, &c1Summary) == MP_REASON_RESUME_REFUSED, "a free slot: REJECT(RESUME_REFUSED)");
        other = c1Summary;
        other.trainerId ^= 0x00010001u;
        R_CHECK(RawResume(1, epoch1, curToken, &other) == MP_REASON_RESUME_MISMATCH && !sRaw.welcomed,
                "restart with the same token but another save (trainer id): REJECT(RESUME_MISMATCH)");
        other = c1Summary;
        other.name[0] = 0xCE; // "S"
        R_CHECK(RawResume(1, epoch1, curToken, &other) == MP_REASON_RESUME_MISMATCH, "same token, another name: REJECT(RESUME_MISMATCH)");
    }
    PumpFor(300);
    R_CHECK(StateOf(R_C1) == MP_STATE_IN_SESSION && !IsReconnecting(R_C1) && HostHasPeerFor(1), "C1 undisturbed by the refused RESUMEs");
    R_CHECK(sEv[R_HOST].left == 0 && sEv[R_C2].left == 0 && sEv[R_C2].joined == 0, "no table change from the refused RESUMEs");
    Sel(R_HOST);
    R_CHECK(memcmp(gMpSession->slotToken[1], curToken, sizeof(curToken)) == 0, "a refused RESUME does not change the token");
    CheckTables("tokens: same tables");
    Report("wrong / replayed / other-save tokens are refused (AD-9)", failuresBefore);

    StopSession();
}

// ---------------------------------------------------------------------------
// Scaled timing (every PS-054 timer / 10)
// ---------------------------------------------------------------------------

static u32 C1Tid(void)
{
    Sel(R_C1);
    return gMpSession->localSummary.trainerId;
}

static u32 C2Tid(void)
{
    Sel(R_C2);
    return gMpSession->localSummary.trainerId;
}

// C1 asks about trade `tradeId` (as its journal would after the reconnect); returns the host's
// answer (MP_TJ_STATUS_*), or NONE if none came.
static u8 AskHostAbout(u32 tradeId)
{
    struct MpMsgTradeRecover m, in;
    u32 start;
    u8 answer = MP_TJ_STATUS_NONE;

    Sel(R_C1);
    while (MP_TradeRecoverPoll(&in))
        ;
    memset(&m, 0, sizeof(m));
    m.slot = 2;
    m.status = MP_TJ_STATUS_READY;
    m.flags = MP_TRADE_RECOVER_FLAG_REPLY;
    m.tradeId = tradeId;
    m.senderTid = C1Tid();
    m.receiverTid = C2Tid();
    m.senderGivenPersonality = 0x11111111u;
    m.senderGivenOtId = 0x22222222u;
    m.senderRecvPersonality = 0x33333333u;
    m.senderRecvOtId = 0x44444444u;
    Sel(R_C1);
    MP_TradeRecoverSend(&m);
    start = MpTransport_GetTimeMs();
    while (answer == MP_TJ_STATUS_NONE && (u32)(MpTransport_GetTimeMs() - start) < 2000)
    {
        PumpOnce();
        Sel(R_C1);
        while (MP_TradeRecoverPoll(&in))
        {
            if (in.slot == MP_SLOT_NONE && in.tradeId == tradeId)
                answer = in.status;
        }
    }
    Sel(R_C2);
    while (MP_TradeRecoverPoll(&in)) // C2's journal would answer; nothing to do here
        ;
    return answer;
}

static bool8 GroupC1C2(void)
{
    Sel(R_C1);
    if (!MP_LinkJoin(0x06))
        return FALSE;
    Sel(R_C2);
    if (!MP_LinkJoin(0x06))
        return FALSE;
    return PumpUntil(CondBothGrouped, 2000);
}

static void TestScaledTiming(void)
{
    u32 failuresBefore;
    u8 epoch1;
    u32 start;
    u8 st1, st2;

    MpNetTiming_Set(10);
    if (!StartSession())
    {
        MpNetTiming_Set(1);
        return;
    }
    epoch1 = EpochOf(R_HOST, 1);

    // "20 s" (2 s at /10)
    failuresBefore = sFail;
    Sel(R_C1);
    MpTransport_SetSimBlackout(TRUE, 2000);
    start = MpTransport_GetTimeMs();
    R_CHECK(PumpUntil(CondC1Reconnecting, 1500), "\"20 s\": C1 notices and starts reconnecting");
    R_CHECK(PumpUntil(CondHostSeesC1Lost, 1500), "\"20 s\": the host holds slot 1");
    R_CHECK(StateOf(R_C1) == MP_STATE_IN_SESSION && MP_IsActive() && MP_GetPlayerCount() == 3, "\"20 s\": C1 still IN_SESSION with its table");
    Sel(R_C2);
    PumpFor(100);
    Sel(R_C2);
    R_CHECK(MP_GetPlayer(1) != NULL && MP_GetPlayer(1)->connectionLost, "\"20 s\": C2 sees slot 1 held (PLAYER_STATUS)");
    R_CHECK(!MP_SendInteractRequest(1, MP_INTERACT_TRADE), "\"20 s\": no trade request to a reconnecting player");
    Sel(R_C1);
    R_CHECK(!MP_LinkJoin(0x06) && !MP_SendInteractRequest(2, MP_INTERACT_TRADE), "\"20 s\": C1 cannot link or ask while reconnecting");
    R_CHECK(PumpUntil(CondSettled, 4000), "\"20 s\": C1 back after the signal returns");
    MP_LOG("selftest: reconnect: \"20 s\" dropout (2000 ms at /10): back after %u ms\n", (unsigned)(MpTransport_GetTimeMs() - start));
    R_CHECK(sEv[R_C1].reconnecting == 1 && sEv[R_C1].reconnected == 1 && sEv[R_C1].disconnected == 0, "\"20 s\": C1 RECONNECTING then RECONNECTED");
    R_CHECK(sEv[R_HOST].playerReconnecting == 1 && sEv[R_HOST].playerReconnected == 1, "\"20 s\": host PLAYER_RECONNECTING / RECONNECTED");
    R_CHECK(sEv[R_C2].playerReconnecting == 1 && sEv[R_C2].playerReconnected == 1, "\"20 s\": C2 PLAYER_RECONNECTING / RECONNECTED");
    R_CHECK(sEv[R_HOST].left == 0 && sEv[R_C2].left == 0 && sEv[R_C1].left == 0 && sEv[R_C2].joined == 0, "\"20 s\": no PLAYER_LEFT / JOINED");
    R_CHECK(EpochOf(R_C1, 1) == epoch1 && EpochOf(R_C2, 1) == epoch1, "\"20 s\": same slot and epoch");
    CheckTables("\"20 s\": same tables");
    Report("\"20 s\" dropout: silent reconnect, slot and epoch kept", failuresBefore);

    // Trade in its commit, (a): the host commits before it notices the drop.
    failuresBefore = sFail;
    memset(sEv, 0, sizeof(sEv));
    R_CHECK(GroupC1C2(), "trade (a): C1 + C2 grouped");
    Sel(R_C1);
    R_CHECK(MP_TradeCommitReady(0x54A00001u), "trade (a): C1 READY");
    PumpFor(60);
    Sel(R_C1);
    MpTransport_SetSimBlackout(TRUE, 1500);
    Sel(R_C2);
    R_CHECK(MP_TradeCommitReady(0x54A00001u), "trade (a): C2 READY");
    R_CHECK(PumpUntil(CondC2Decided, 1000), "trade (a): C2 decided");
    Sel(R_C2);
    st2 = MP_TradeCommitPoll();
    R_CHECK(st2 == MP_TRADE_COMMIT_COMMIT, "trade (a): C2 COMMIT (both were READY)");
    R_CHECK(PumpUntil(CondC1Decided, 1500), "trade (a): C1 decided when its link dropped");
    Sel(R_C1);
    st1 = MP_TradeCommitPoll();
    R_CHECK(st1 == MP_TRADE_COMMIT_ABORT && MP_TradeCommitGetAbortReason() == MP_TRADE_ABORT_IN_DOUBT, "trade (a): C1 presumes ABORT (in doubt)");
    R_CHECK(PumpUntil(CondC1Ungrouped, 1000) && PumpUntil(CondC2Ungrouped, 1500), "trade (a): the link group ended for both (PS-048 path)");
    R_CHECK(PumpUntil(CondSettled, 4000), "trade (a): C1 reconnected");
    R_CHECK(AskHostAbout(0x54A00001u) == MP_TJ_STATUS_COMMITTED, "trade (a): after the reconnect the host tells C1's journal COMMITTED (roll forward)");
    Sel(R_C1);
    MP_TradeCommitReset();
    Sel(R_C2);
    MP_TradeCommitReset();
    R_CHECK(GroupC1C2(), "trade (a): C1 + C2 can link again after the reconnect");
    Sel(R_C1);
    MP_LinkLeave();
    PumpUntil(CondC2Ungrouped, 1000);
    R_CHECK(sEv[R_C2].left == 0 && sEv[R_HOST].left == 0, "trade (a): nobody left");
    Report("dropout mid-trade after the host committed: journal rolls forward after the reconnect", failuresBefore);

    // Trade in its commit, (b): the host notices the drop before the partner is ready.
    failuresBefore = sFail;
    memset(sEv, 0, sizeof(sEv));
    PumpFor(100);
    R_CHECK(GroupC1C2(), "trade (b): C1 + C2 grouped");
    Sel(R_C1);
    R_CHECK(MP_TradeCommitReady(0x54B00002u), "trade (b): C1 READY");
    PumpFor(60);
    Sel(R_C1);
    MpTransport_SetSimBlackout(TRUE, 1500);
    R_CHECK(PumpUntil(CondHostSeesC1Lost, 1500), "trade (b): the host notices the drop");
    R_CHECK(PumpUntil(CondC2Ungrouped, 1000), "trade (b): C2's link group ended");
    Sel(R_C2);
    MP_TradeCommitReady(0x54B00002u); // too late: no group any more (refused locally, or ABORT from the host)
    PumpUntil(CondC2Decided, 1000);
    Sel(R_C2);
    R_CHECK(MP_TradeCommitPoll() == MP_TRADE_COMMIT_ABORT, "trade (b): C2 ABORT");
    R_CHECK(PumpUntil(CondC1Decided, 1500), "trade (b): C1 decided");
    Sel(R_C1);
    R_CHECK(MP_TradeCommitPoll() == MP_TRADE_COMMIT_ABORT, "trade (b): C1 ABORT");
    R_CHECK(PumpUntil(CondSettled, 4000), "trade (b): C1 reconnected");
    R_CHECK(AskHostAbout(0x54B00002u) == MP_TJ_STATUS_ABORTED, "trade (b): after the reconnect the host tells C1's journal ABORTED (delete)");
    Sel(R_C1);
    MP_TradeCommitReset();
    Sel(R_C2);
    MP_TradeCommitReset();
    Report("dropout mid-trade before the commit: neither commits, journal deleted after the reconnect", failuresBefore);

    // "40 s" (4 s at /10): C1 gives up, the host frees the slot.
    failuresBefore = sFail;
    memset(sEv, 0, sizeof(sEv));
    PumpFor(100);
    Sel(R_C1);
    MpTransport_SetSimBlackout(TRUE, 4000);
    start = MpTransport_GetTimeMs();
    R_CHECK(PumpUntil(CondC1Reconnecting, 1500), "\"40 s\": C1 starts reconnecting");
    R_CHECK(PumpUntil(CondC1Error, 4500), "\"40 s\": C1 gives up");
    Sel(R_C1);
    R_CHECK(MP_GetErrorReason() == MP_REASON_CONNECTION_LOST && sEv[R_C1].disconnected == 1
         && sEv[R_C1].lastDisconnectReason == MP_REASON_CONNECTION_LOST, "\"40 s\": C1 DISCONNECTED(CONNECTION_LOST)");
    R_CHECK(PumpUntil(CondHostFreedC1, 5000), "\"40 s\": the host frees slot 1 after the grace");
    MP_LOG("selftest: reconnect: \"40 s\" dropout (4000 ms at /10): slot freed after %u ms\n", (unsigned)(MpTransport_GetTimeMs() - start));
    PumpFor(200);
    R_CHECK(sEv[R_HOST].playerReconnecting == 1 && sEv[R_HOST].left == 1 && sEv[R_HOST].lastLeftReason == MP_REASON_CONNECTION_LOST,
            "\"40 s\": host PLAYER_RECONNECTING, then PLAYER_LEFT(CONNECTION_LOST)");
    R_CHECK(sEv[R_C2].playerReconnecting == 1 && sEv[R_C2].left == 1 && sEv[R_C2].lastLeftReason == MP_REASON_CONNECTION_LOST,
            "\"40 s\": C2 PLAYER_LEFT(CONNECTION_LOST)");
    Sel(R_C2);
    R_CHECK(MP_GetPlayerCount() == 2 && MP_GetPlayer(1) == NULL, "\"40 s\": C2's table without slot 1");
    Sel(R_C1);
    MP_Leave();
    Report("\"40 s\" dropout: C1 disconnected, slot freed after the grace (as before PS-054)", failuresBefore);

    StopSession();
    MpNetTiming_Set(1);
}

u32 MpSelftest_Reconnect(void)
{
    sFail = 0;
    TestParsers();
    TestRealTiming();
    TestScaledTiming();
    MpNetTiming_Set(1);
    Sel(0);
    return sFail;
}
