// --mp-selftest, PS-012 groups:
//  1. Parser fuzzing (protocol.md 8.3) for every message parser.
//  2. An in-process session scenario: a host and loopback clients in one process
//     (session/transport instances), covering the PS-012 acceptance criteria that
//     can be automated: same 4-player table everywhere, REJECT FULL / BUILD / VERSION /
//     BAD_DATA, garbage before HELLO, kick after 8 malformed messages, BYE -> PLAYER_LEFT,
//     PLAYER_UPDATE relay, silent client death (timeout), 100 KB random UDP flood,
//     host BYE -> DISCONNECTED, silent host death -> DISCONNECTED.

#include <string.h>
#include "mp/mp.h"
#include "mp/mp_internal.h"
#include "mp/mp_protocol.h"
#include "mp/mp_selftest.h"
#include "mp/mp_session_internal.h"
#include "mp/mp_summary.h"
#include "mp/mp_transport.h"

static u32 sFail;

#define T_CHECK(cond, what)                                                         \
    do                                                                              \
    {                                                                               \
        if (!(cond))                                                                \
        {                                                                           \
            MP_LOG("selftest: FAIL: %s (%s:%d)\n", what, __FILE__, __LINE__);       \
            sFail++;                                                                \
        }                                                                           \
    } while (0)

static u32 sRng = 0xC0FFEE11u;

static u32 Rand(void)
{
    sRng ^= sRng << 13;
    sRng ^= sRng >> 17;
    sRng ^= sRng << 5;
    return sRng;
}

// ===========================================================================
// 1. Parsers
// ===========================================================================

enum
{
    P_HELLO,
    P_WELCOME,
    P_REJECT,
    P_ENTRY,
    P_LEFT,
    P_BYE,
    P_PING,
    P_COUNT,
};

static const char *const sParserNames[P_COUNT] =
{
    "HELLO", "WELCOME", "REJECT", "PLAYER_ENTRY", "PLAYER_LEFT", "BYE", "PING",
};

union AnyMsg
{
    struct MpMsgHello hello;
    struct MpMsgWelcome welcome;
    struct MpMsgReject reject;
    struct MpMsgPlayerEntry entry;
    struct MpMsgPlayerLeft left;
    struct MpMsgBye bye;
    struct MpMsgPing ping;
};

static u8 RunParser(u8 which, const u8 *p, u32 len, union AnyMsg *out)
{
    switch (which)
    {
    case P_HELLO:   return MpProto_ParseHello(p, len, &out->hello);
    case P_WELCOME: return MpProto_ParseWelcome(p, len, &out->welcome);
    case P_REJECT:  return MpProto_ParseReject(p, len, &out->reject);
    case P_ENTRY:   return MpProto_ParsePlayerEntry(p, len, &out->entry);
    case P_LEFT:    return MpProto_ParsePlayerLeft(p, len, &out->left);
    case P_BYE:     return MpProto_ParseBye(p, len, &out->bye);
    default:        return MpProto_ParsePing(p, len, &out->ping);
    }
}

static u16 WriteMsg(u8 which, u8 *buf, u16 cap, const union AnyMsg *msg)
{
    switch (which)
    {
    case P_HELLO:   return MpProto_WriteHello(buf, cap, &msg->hello);
    case P_WELCOME: return MpProto_WriteWelcome(buf, cap, &msg->welcome);
    case P_REJECT:  return MpProto_WriteReject(buf, cap, &msg->reject);
    case P_ENTRY:   return MpProto_WritePlayerEntry(buf, cap, MP_MSG_PLAYER_JOINED, &msg->entry);
    case P_LEFT:    return MpProto_WritePlayerLeft(buf, cap, &msg->left);
    case P_BYE:     return MpProto_WriteBye(buf, cap, &msg->bye);
    default:        return MpProto_WritePing(buf, cap, MP_MSG_PING, &msg->ping);
    }
}

static void FixtureSummary(struct MpPlayerSummary *s, u32 trainerId)
{
    memset(s, 0, sizeof(*s));
    memset(s->name, MP_EOS, sizeof(s->name));
    s->name[0] = 0xBB; // A
    s->name[1] = 0xCD; // S
    s->name[2] = 0xC2; // H
    s->gender = 0;
    s->avatarStyle = MP_AVATAR_STYLE_PLAYER;
    s->badges = 0x07;
    s->trainerId = trainerId;
}

static void FixtureMsg(u8 which, union AnyMsg *msg)
{
    memset(msg, 0, sizeof(*msg));
    switch (which)
    {
    case P_HELLO:
        msg->hello.protocolVersion = MP_PROTOCOL_VERSION;
        msg->hello.buildHash = 0x12345678u;
        FixtureSummary(&msg->hello.summary, 0x00053039u);
        break;
    case P_WELCOME:
        msg->welcome.yourSlot = 2;
        msg->welcome.yourEpoch = 5;
        msg->welcome.playerCount = 3;
        msg->welcome.flags = MP_WELCOME_FLAG_RESUMED; // PS-054
        memcpy(msg->welcome.token, "\x01\x23\x45\x67\x89\xAB\xCD\xEF", MP_SESSION_TOKEN_SIZE);
        msg->welcome.players[1].entryFlags = MP_ENTRY_FLAG_LOST; // PS-054: slot 1 held for a reconnect
        msg->welcome.players[0].slot = 0;
        msg->welcome.players[0].epoch = 1;
        msg->welcome.players[0].entryFlags = MP_ENTRY_FLAG_HOST;
        FixtureSummary(&msg->welcome.players[0].summary, 100);
        msg->welcome.players[1].slot = 1;
        msg->welcome.players[1].epoch = 3;
        FixtureSummary(&msg->welcome.players[1].summary, 101);
        msg->welcome.players[2].slot = 2;
        msg->welcome.players[2].epoch = 5;
        FixtureSummary(&msg->welcome.players[2].summary, 102);
        break;
    case P_REJECT:
        msg->reject.reason = MP_REASON_FULL;
        msg->reject.hostProtocolVersion = MP_PROTOCOL_VERSION;
        msg->reject.hostBuildHash = 0xCAFEBABEu;
        break;
    case P_ENTRY:
        msg->entry.slot = 1;
        msg->entry.epoch = 2;
        FixtureSummary(&msg->entry.summary, 7);
        break;
    case P_LEFT:
        msg->left.slot = 2;
        msg->left.epoch = 9;
        msg->left.reason = MP_REASON_LEFT;
        break;
    case P_BYE:
        msg->bye.reason = MP_BYE_HOST_SHUTDOWN;
        break;
    default:
        msg->ping.timeMs = 0x01020304u;
        break;
    }
}

// Parses `payload` modified at one byte; returns the parse result.
static u8 ParseWithByte(u8 which, const u8 *payload, u32 len, u32 offset, u8 value)
{
    u8 copy[MP_MAX_PACKET_SIZE];
    union AnyMsg out;

    memcpy(copy, payload, len);
    copy[offset] = value;
    return RunParser(which, copy, len, &out);
}

static void TestTargeted(const u8 *helloPayload, const u8 *welcomePayload, u32 welcomeLen, const u8 *entryPayload, const u8 *leftPayload)
{
    u8 buf[MP_MAX_PACKET_SIZE + 2];
    struct MpMsgHeader hdr;
    u16 version;
    u32 hash;
    u32 i;
    u8 copy[MP_HELLO_SIZE];

    // HELLO summary / magic
    memcpy(copy, helloPayload, MP_HELLO_SIZE);
    for (i = 16; i < 24; i++)
        copy[i] = 0xBB;
    {
        union AnyMsg out;
        T_CHECK(MpProto_ParseHello(copy, MP_HELLO_SIZE, &out.hello) == MP_PARSE_ERR_BAD_SUMMARY, "HELLO name without EOS");
    }
    T_CHECK(ParseWithByte(P_HELLO, helloPayload, MP_HELLO_SIZE, 16, MP_EOS) == MP_PARSE_ERR_BAD_SUMMARY, "HELLO name EOS at byte 0");
    T_CHECK(ParseWithByte(P_HELLO, helloPayload, MP_HELLO_SIZE, 17, 0xFC) == MP_PARSE_ERR_BAD_SUMMARY, "HELLO name with 0xFC");
    T_CHECK(ParseWithByte(P_HELLO, helloPayload, MP_HELLO_SIZE, 17, 0xFD) == MP_PARSE_ERR_BAD_SUMMARY, "HELLO name with 0xFD");
    T_CHECK(ParseWithByte(P_HELLO, helloPayload, MP_HELLO_SIZE, 24, 2) == MP_PARSE_ERR_BAD_SUMMARY, "HELLO gender 2");
    T_CHECK(ParseWithByte(P_HELLO, helloPayload, MP_HELLO_SIZE, 25, MP_AVATAR_STYLE_COUNT) == MP_PARSE_ERR_BAD_SUMMARY, "HELLO avatar style out of range");
    T_CHECK(ParseWithByte(P_HELLO, helloPayload, MP_HELLO_SIZE, 0, 0x00) == MP_PARSE_ERR_BAD_MAGIC, "HELLO bad magic");
    T_CHECK(MpProto_ParseHelloPrefix(helloPayload, MP_HELLO_PREFIX_SIZE, &version, &hash) == MP_PARSE_OK
         && version == MP_PROTOCOL_VERSION && hash == 0x12345678u, "HELLO frozen prefix");
    T_CHECK(MpProto_ParseHelloPrefix(helloPayload, MP_HELLO_PREFIX_SIZE - 1, &version, &hash) != MP_PARSE_OK, "HELLO prefix truncated");

    // WELCOME consistency (fixture: yourSlot 2, epoch 5, entries slot 0/1/2)
    T_CHECK(ParseWithByte(P_WELCOME, welcomePayload, welcomeLen, 2, 0) != MP_PARSE_OK, "WELCOME N = 0");
    T_CHECK(ParseWithByte(P_WELCOME, welcomePayload, welcomeLen, 2, 5) != MP_PARSE_OK, "WELCOME N = 5");
    T_CHECK(ParseWithByte(P_WELCOME, welcomePayload, welcomeLen, MP_WELCOME_HEAD_SIZE + 20 * 1, 2) == MP_PARSE_ERR_BAD_FIELD, "WELCOME duplicate slots");
    T_CHECK(ParseWithByte(P_WELCOME, welcomePayload, welcomeLen, 0, 3) == MP_PARSE_ERR_BAD_FIELD, "WELCOME yourSlot missing");
    T_CHECK(ParseWithByte(P_WELCOME, welcomePayload, welcomeLen, 0, 0) == MP_PARSE_ERR_BAD_FIELD, "WELCOME yourSlot 0");
    T_CHECK(ParseWithByte(P_WELCOME, welcomePayload, welcomeLen, 1, 6) == MP_PARSE_ERR_BAD_FIELD, "WELCOME yourEpoch mismatch");
    memcpy(buf, welcomePayload, welcomeLen);
    buf[MP_WELCOME_HEAD_SIZE] = 3;     // entry 0: slot 0 -> 3
    buf[MP_WELCOME_HEAD_SIZE + 2] = 0; // and drop its host flag: now there is no slot 0
    {
        union AnyMsg out;
        T_CHECK(MpProto_ParseWelcome(buf, welcomeLen, &out.welcome) == MP_PARSE_ERR_BAD_FIELD, "WELCOME without slot 0");
    }
    T_CHECK(ParseWithByte(P_WELCOME, welcomePayload, welcomeLen, MP_WELCOME_HEAD_SIZE + 20 * 1 + 4, MP_EOS) == MP_PARSE_ERR_BAD_SUMMARY, "WELCOME entry with empty name");
    // PS-054: token, flags, LOST entry flag
    memcpy(buf, welcomePayload, welcomeLen);
    memset(&buf[4], 0, MP_SESSION_TOKEN_SIZE);
    {
        union AnyMsg out;
        T_CHECK(MpProto_ParseWelcome(buf, welcomeLen, &out.welcome) == MP_PARSE_ERR_BAD_FIELD, "WELCOME with a zero token");
        T_CHECK(MpProto_ParseWelcome(welcomePayload, welcomeLen, &out.welcome) == MP_PARSE_OK
             && (out.welcome.flags & MP_WELCOME_FLAG_RESUMED) && (out.welcome.players[1].entryFlags & MP_ENTRY_FLAG_LOST)
             && !(out.welcome.players[2].entryFlags & MP_ENTRY_FLAG_LOST), "WELCOME RESUMED flag and LOST entry flag");
    }
    T_CHECK(ParseWithByte(P_WELCOME, welcomePayload, welcomeLen, 3, 0xFE) == MP_PARSE_OK, "WELCOME reserved flag bits ignored");
    T_CHECK(ParseWithByte(P_WELCOME, welcomePayload, welcomeLen, MP_WELCOME_HEAD_SIZE + 2, MP_ENTRY_FLAG_HOST | MP_ENTRY_FLAG_LOST) == MP_PARSE_ERR_BAD_FIELD, "WELCOME host entry flagged LOST");

    // PlayerEntry / PLAYER_LEFT fields
    T_CHECK(ParseWithByte(P_ENTRY, entryPayload, MP_PLAYER_ENTRY_SIZE, 2, MP_ENTRY_FLAG_HOST) == MP_PARSE_ERR_BAD_FIELD, "ENTRY host flag on slot 1");
    T_CHECK(ParseWithByte(P_ENTRY, entryPayload, MP_PLAYER_ENTRY_SIZE, 0, MP_MAX_PLAYERS) == MP_PARSE_ERR_BAD_FIELD, "ENTRY slot 4");
    T_CHECK(ParseWithByte(P_ENTRY, entryPayload, MP_PLAYER_ENTRY_SIZE, 1, 0) == MP_PARSE_ERR_BAD_FIELD, "ENTRY epoch 0");
    T_CHECK(ParseWithByte(P_ENTRY, entryPayload, MP_PLAYER_ENTRY_SIZE, 2, 0x80) == MP_PARSE_OK, "ENTRY reserved flag bits ignored");
    T_CHECK(ParseWithByte(P_LEFT, leftPayload, MP_PLAYER_LEFT_SIZE, 0, MP_MAX_PLAYERS) == MP_PARSE_ERR_BAD_FIELD, "LEFT slot 4");
    T_CHECK(ParseWithByte(P_LEFT, leftPayload, MP_PLAYER_LEFT_SIZE, 1, 0) == MP_PARSE_ERR_BAD_FIELD, "LEFT epoch 0");

    // Header
    memset(buf, 0, sizeof(buf));
    buf[0] = MP_MSG_BYE;
    buf[1] = MP_PROTOCOL_VERSION;
    buf[2] = 4;
    T_CHECK(MpProto_ParseHeader(buf, 8, &hdr) && hdr.length == 4, "header OK");
    T_CHECK(!MpProto_ParseHeader(buf, 3, &hdr), "header len 3");
    T_CHECK(!MpProto_ParseHeader(buf, 9, &hdr), "header length mismatch (long)");
    T_CHECK(!MpProto_ParseHeader(buf, 7, &hdr), "header length mismatch (short)");
    buf[2] = (u8)((MP_MAX_PACKET_SIZE + 1 - 4) & 0xFF);
    buf[3] = (u8)((MP_MAX_PACKET_SIZE + 1 - 4) >> 8);
    T_CHECK(!MpProto_ParseHeader(buf, MP_MAX_PACKET_SIZE + 1, &hdr), "header packet > 512");
    T_CHECK(!MpProto_ParseHeader(NULL, 8, &hdr), "header NULL");

    // RFC 1982 sequence numbers
    T_CHECK(MpSeqIsNewer(1, 0), "seq 1 > 0");
    T_CHECK(MpSeqIsNewer(0, 65535), "seq wrap 0 > 65535");
    T_CHECK(!MpSeqIsNewer(0, 0), "seq duplicate");
    T_CHECK(!MpSeqIsNewer(0, 1), "seq older");
    T_CHECK(MpSeqIsNewer(0x7FFF, 0), "seq +32767");
    T_CHECK(!MpSeqIsNewer(0x8000, 0), "seq +32768 is not newer");
}

u32 MpSelftest_Parsers(void)
{
    u8 buf[MP_MAX_PACKET_SIZE];
    u8 buf2[MP_MAX_PACKET_SIZE];
    u8 big[600];
    u8 helloPayload[MP_HELLO_SIZE];
    u8 welcomePayload[MP_WELCOME_SIZE(MP_MAX_PLAYERS)];
    u8 entryPayload[MP_PLAYER_ENTRY_SIZE];
    u8 leftPayload[MP_PLAYER_LEFT_SIZE];
    u32 welcomeLen = 0;
    union AnyMsg msg, out;
    struct MpMsgHeader hdr;
    u32 failuresBefore;
    u32 n, i, bit, len, plen;
    u8 which, r1, r2;
    u16 written;

    sFail = 0;
    for (which = 0; which < P_COUNT; which++)
    {
        failuresBefore = sFail;
        FixtureMsg(which, &msg);
        written = WriteMsg(which, buf, sizeof(buf), &msg);
        T_CHECK(written >= MP_HEADER_SIZE, "writer produced a packet");
        if (written < MP_HEADER_SIZE)
            continue;
        T_CHECK(MpProto_ParseHeader(buf, written, &hdr) && hdr.length == written - MP_HEADER_SIZE, "header of a written packet");
        plen = written - MP_HEADER_SIZE;
        T_CHECK(WriteMsg(which, buf2, (u16)(written - 1), &msg) == 0, "writer refuses a short buffer");

        // Keep valid payloads for the targeted cases
        if (which == P_HELLO)
            memcpy(helloPayload, &buf[MP_HEADER_SIZE], MP_HELLO_SIZE);
        else if (which == P_WELCOME)
        {
            welcomeLen = plen;
            memcpy(welcomePayload, &buf[MP_HEADER_SIZE], plen);
        }
        else if (which == P_ENTRY)
            memcpy(entryPayload, &buf[MP_HEADER_SIZE], MP_PLAYER_ENTRY_SIZE);
        else if (which == P_LEFT)
            memcpy(leftPayload, &buf[MP_HEADER_SIZE], MP_PLAYER_LEFT_SIZE);

        // 1. round trip
        T_CHECK(RunParser(which, &buf[MP_HEADER_SIZE], plen, &out) == MP_PARSE_OK, "valid message parses");
        T_CHECK(WriteMsg(which, buf2, sizeof(buf2), &out) == written && memcmp(buf, buf2, written) == 0, "round trip is byte-identical");

        // 2. truncated
        for (len = 0; len < plen; len++)
            T_CHECK(RunParser(which, &buf[MP_HEADER_SIZE], len, &out) != MP_PARSE_OK, "truncated message rejected");
        T_CHECK(RunParser(which, NULL, 0, &out) != MP_PARSE_OK, "NULL payload rejected");

        // 3. oversized (REJECT is frozen and allows longer payloads)
        memset(big, 0, sizeof(big));
        memcpy(big, &buf[MP_HEADER_SIZE], plen);
        r1 = RunParser(which, big, plen + 1, &out);
        T_CHECK((which == P_REJECT) ? (r1 == MP_PARSE_OK) : (r1 != MP_PARSE_OK), "payload + 1 byte");
        r1 = RunParser(which, big, MP_MAX_PACKET_SIZE, &out);
        T_CHECK((which == P_REJECT) ? (r1 == MP_PARSE_OK) : (r1 != MP_PARSE_OK), "payload padded to 512");
        r1 = RunParser(which, big, MP_MAX_PACKET_SIZE + 1, &out);
        T_CHECK((which == P_REJECT) ? (r1 == MP_PARSE_OK) : (r1 != MP_PARSE_OK), "payload padded to 513");

        // 4. every single-bit flip: no crash, deterministic
        for (i = 0; i < plen; i++)
        {
            for (bit = 0; bit < 8; bit++)
            {
                memcpy(big, &buf[MP_HEADER_SIZE], plen);
                big[i] ^= (u8)(1 << bit);
                r1 = RunParser(which, big, plen, &out);
                r2 = RunParser(which, big, plen, &out);
                T_CHECK(r1 == r2, "bit flip result is deterministic");
            }
        }

        // 5. garbage
        for (n = 0; n < 100000; n++)
        {
            len = Rand() % (sizeof(big) + 1);
            for (i = 0; i < len; i++)
                big[i] = (u8)(Rand() >> 24);
            RunParser(which, big, len, &out);
            MpProto_ParseHeader(big, len, &hdr);
        }

        if (sFail == failuresBefore)
            MP_LOG("selftest: parser %s OK (round trip, truncated, oversized, %u bit flips, 100000 garbage)\n",
                   sParserNames[which], (unsigned)(plen * 8));
    }

    failuresBefore = sFail;
    if (welcomeLen != 0)
        TestTargeted(helloPayload, welcomePayload, welcomeLen, entryPayload, leftPayload);
    if (sFail == failuresBefore)
        MP_LOG("selftest: parser targeted cases OK (seed 0xC0FFEE11)\n");
    return sFail;
}

// ===========================================================================
// 2. Session scenario
// ===========================================================================

#define I_HOST      0
#define I_C1        1
#define I_C2        2
#define I_C3        3
#define I_C5        4
#define I_FAKEBUILD 5
#define I_FAKEPROTO 6
#define I_RAW       7   // transport only: hand-made packets
#define SESSION_INSTANCES 7 // 0..6 run the real session code

#define SESSION_PORT_FIRST  27580
#define SESSION_PORT_LAST   27589

struct EventCounts
{
    u8 started;
    u8 failed;
    u8 joined;
    u8 left;
    u8 updated;
    u8 disconnected;
    u8 reconnecting;        // PS-054: MP_EVENT_RECONNECTING
    u8 playerReconnecting;  // PS-054: MP_EVENT_PLAYER_RECONNECTING
    u8 lastFailReason;
    u8 lastLeftReason;
    u8 lastDisconnectReason;
};

static struct EventCounts sEvents[SESSION_INSTANCES];

static struct
{
    bool8 connected;
    bool8 disconnected;
    u8 rejectReason;    // 0 = none
    bool8 welcomed;
    u8 welcomeSlot;
    u8 welcomeCount;
    u8 byeReason;       // 0xFF = none
} sRaw;

static u16 sPort;

static void Sel(u8 instance)
{
    MpSession_SelectInstance(instance);
}

static void OnRawEvent(const struct MpTransportEvent *ev)
{
    struct MpMsgHeader hdr;
    union AnyMsg msg;

    switch (ev->type)
    {
    case MP_TRANSPORT_EV_CONNECT:
        sRaw.connected = TRUE;
        break;
    case MP_TRANSPORT_EV_DISCONNECT:
        sRaw.disconnected = TRUE;
        break;
    case MP_TRANSPORT_EV_RECEIVE:
        if (!MpProto_ParseHeader(ev->data, ev->length, &hdr))
            break;
        if (hdr.type == MP_MSG_REJECT && MpProto_ParseReject(ev->data + 4, hdr.length, &msg.reject) == MP_PARSE_OK)
            sRaw.rejectReason = msg.reject.reason;
        else if (hdr.type == MP_MSG_WELCOME && MpProto_ParseWelcome(ev->data + 4, hdr.length, &msg.welcome) == MP_PARSE_OK)
        {
            sRaw.welcomed = TRUE;
            sRaw.welcomeSlot = msg.welcome.yourSlot;
            sRaw.welcomeCount = msg.welcome.playerCount;
        }
        else if (hdr.type == MP_MSG_BYE && MpProto_ParseBye(ev->data + 4, hdr.length, &msg.bye) == MP_PARSE_OK)
            sRaw.byeReason = msg.bye.reason;
        break;
    }
}

static void DrainEvents(u8 instance)
{
    struct MpEvent ev;
    struct EventCounts *c = &sEvents[instance];

    while (MP_PollEvent(&ev))
    {
        switch (ev.type)
        {
        case MP_EVENT_SESSION_STARTED: c->started++; break;
        case MP_EVENT_CONNECT_FAILED:  c->failed++; c->lastFailReason = ev.reason; break;
        case MP_EVENT_PLAYER_JOINED:   c->joined++; break;
        case MP_EVENT_PLAYER_LEFT:     c->left++; c->lastLeftReason = ev.reason; break;
        case MP_EVENT_PLAYER_UPDATED:  c->updated++; break;
        case MP_EVENT_DISCONNECTED:    c->disconnected++; c->lastDisconnectReason = ev.reason; break;
        case MP_EVENT_RECONNECTING:    c->reconnecting++; break;
        case MP_EVENT_PLAYER_RECONNECTING: c->playerReconnecting++; break;
        }
    }
}

static void PumpOnce(void)
{
    u8 i;

    for (i = 0; i < SESSION_INSTANCES; i++)
    {
        Sel(i);
        MP_Update();
        DrainEvents(i);
    }
    Sel(I_RAW);
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

// Pumps until cond() is TRUE or timeoutMs passes. Returns cond().
static bool8 PumpUntil(bool8 (*cond)(void), u32 timeoutMs)
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

static u8 CountOf(u8 instance)
{
    Sel(instance);
    return MP_GetPlayerCount();
}

static u8 HostPeers(void)
{
    Sel(I_HOST);
    return MpSession_DebugHostPeerCount();
}

static u8 sWaitInstance;
static u8 sWaitCount;

static bool8 CondNotConnecting(void) { return StateOf(sWaitInstance) != MP_STATE_CONNECTING; }
static bool8 CondInSession(void)     { return StateOf(sWaitInstance) == MP_STATE_IN_SESSION; }
static bool8 CondNotInSession(void)  { return StateOf(sWaitInstance) != MP_STATE_IN_SESSION; }
static bool8 CondHostPeers(void)     { return HostPeers() == sWaitCount; }
static bool8 CondCount(void)         { return CountOf(sWaitInstance) == sWaitCount; }
static bool8 CondRawConnected(void)  { return sRaw.connected; }
static bool8 CondRawRejected(void)   { return sRaw.rejectReason != 0; }
static bool8 CondRawWelcomed(void)   { return sRaw.welcomed; }
static bool8 CondRawBye(void)        { return sRaw.byeReason != 0xFF; }

static bool8 CondTransportClosed(void)
{
    Sel(sWaitInstance);
    return !MpTransport_IsOpen();
}

static bool8 CondAllClosed(void)
{
    u8 i;

    for (i = 0; i <= I_RAW; i++)
    {
        Sel(i);
        if (MpTransport_IsOpen())
            return FALSE;
    }
    return TRUE;
}

static bool8 WaitFor(bool8 (*cond)(void), u8 instance, u8 count, u32 timeoutMs)
{
    sWaitInstance = instance;
    sWaitCount = count;
    return PumpUntil(cond, timeoutMs);
}

static bool8 JoinAs(u8 instance, const char *name)
{
    Sel(instance);
    if (!MP_SetNameOverride(name))
        return FALSE;
    return MP_Join("127.0.0.1", sPort);
}

// Checks that every instance in `list` has the same table as the host.
static void CheckSameTable(const u8 *list, u8 listCount, u8 expectedCount)
{
    struct MpPlayer host[MP_MAX_PLAYERS];
    bool8 hostUsed[MP_MAX_PLAYERS];
    const struct MpPlayer *p;
    u8 i, slot;

    Sel(I_HOST);
    T_CHECK(MP_GetPlayerCount() == expectedCount, "host player count");
    for (slot = 0; slot < MP_MAX_PLAYERS; slot++)
    {
        p = MP_GetPlayer(slot);
        hostUsed[slot] = (p != NULL);
        if (p != NULL)
            host[slot] = *p;
    }
    for (i = 0; i < listCount; i++)
    {
        Sel(list[i]);
        T_CHECK(MP_GetPlayerCount() == expectedCount, "client player count matches the host");
        for (slot = 0; slot < MP_MAX_PLAYERS; slot++)
        {
            p = MP_GetPlayer(slot);
            T_CHECK((p != NULL) == hostUsed[slot], "same slots occupied as the host");
            if (p == NULL || !hostUsed[slot])
                continue;
            T_CHECK(p->epoch == host[slot].epoch, "same epoch as the host");
            T_CHECK(memcmp(p->summary.name, host[slot].summary.name, MP_PLAYER_NAME_SIZE) == 0, "same name as the host");
            T_CHECK(p->summary.trainerId == host[slot].summary.trainerId, "same trainer id as the host");
            T_CHECK(p->isHost == (slot == 0), "host flag on slot 0 only");
            T_CHECK(p->isLocal == (slot == MP_GetLocalSlot()), "isLocal only on own slot");
        }
    }
}

static void ExpectReject(u8 instance, u8 reason, const char *what)
{
    T_CHECK(WaitFor(CondNotConnecting, instance, 0, 6000), what);
    Sel(instance);
    T_CHECK(MP_GetState() == MP_STATE_ERROR && MP_GetErrorReason() == reason, what);
    DrainEvents(instance);
    T_CHECK(sEvents[instance].failed == 1 && sEvents[instance].lastFailReason == reason, what);
    MP_Leave();
    T_CHECK(MP_GetState() == MP_STATE_OFFLINE, "MP_Leave acknowledges ERROR");
    // The host must free the spare peer again.
    T_CHECK(WaitFor(CondHostPeers, I_HOST, 3, 4000), "host frees the rejected peer");
}

static void RawSend(u8 type, u8 channel, const u8 *payload, u16 len, bool8 badLength)
{
    u8 buf[MP_MAX_PACKET_SIZE];

    buf[0] = type;
    buf[1] = MP_PROTOCOL_VERSION;
    buf[2] = (u8)((len + (badLength ? 1 : 0)) & 0xFF);
    buf[3] = (u8)((len + (badLength ? 1 : 0)) >> 8);
    if (len != 0)
        memcpy(&buf[4], payload, len);
    Sel(I_RAW);
    MpTransport_Send(0, channel, buf, (u16)(4 + len), channel == MP_CHANNEL_CONTROL);
}

static void RawConnect(void)
{
    memset(&sRaw, 0, sizeof(sRaw));
    sRaw.byeReason = 0xFF;
    Sel(I_RAW);
    T_CHECK(MpTransport_Connect("127.0.0.1", sPort), "raw client connect");
    T_CHECK(PumpUntil(CondRawConnected, 5000), "raw client connected");
}

static void RawClose(void)
{
    Sel(I_RAW);
    MpTransport_Close();
}

static u16 RawHello(u8 *payload, const char *name)
{
    struct MpMsgHello hello;
    u8 buf[MP_MAX_PACKET_SIZE];
    u16 len;

    memset(&hello, 0, sizeof(hello));
    hello.protocolVersion = MP_PROTOCOL_VERSION;
    hello.buildHash = MP_GetBuildHash();
    Sel(I_RAW);
    MP_SetNameOverride(name);
    MP_BuildPlayerSummary(&hello.summary);
    len = MpProto_WriteHello(buf, sizeof(buf), &hello);
    if (len < MP_HEADER_SIZE)
        return 0;
    memcpy(payload, &buf[MP_HEADER_SIZE], len - MP_HEADER_SIZE);
    return (u16)(len - MP_HEADER_SIZE);
}

static void Report(const char *what, u32 failuresBefore)
{
    if (sFail == failuresBefore)
        MP_LOG("selftest: session: %s OK\n", what);
}

u32 MpSelftest_Session(void)
{
    static const u8 sClients[] = { I_C1, I_C2, I_C3 };
    static const u8 sTwoClients[] = { I_C1, I_C2 };
    static const u8 sOneClient[] = { I_C1 };
    u8 payload[MP_MAX_PACKET_SIZE];
    u8 junk[8];
    u16 helloLen;
    u32 failuresBefore;
    u32 start, elapsed, sent;
    u8 i;
    bool8 hosting = FALSE;

    sFail = 0;
    memset(sEvents, 0, sizeof(sEvents));
    for (i = 0; i <= I_RAW; i++)
        MpSession_ResetInstance(i);

    // --- host + 3 clients, same table everywhere ---
    failuresBefore = sFail;
    Sel(I_HOST);
    MP_SetNameOverride("Host");
    for (sPort = SESSION_PORT_FIRST; sPort <= SESSION_PORT_LAST && !hosting; sPort++)
        hosting = MP_Host(sPort);
    sPort--;
    T_CHECK(hosting, "host on a port in 27580..27589");
    if (!hosting)
        return sFail;
    T_CHECK(MP_IsHost() && MP_GetLocalSlot() == 0 && MP_GetPlayerCount() == 1, "host is slot 0 alone");

    T_CHECK(JoinAs(I_C1, "Cli1"), "client 1 MP_Join");
    T_CHECK(WaitFor(CondInSession, I_C1, 0, 5000), "client 1 in session");
    T_CHECK(JoinAs(I_C2, "Cli2"), "client 2 MP_Join");
    T_CHECK(WaitFor(CondInSession, I_C2, 0, 5000), "client 2 in session");
    T_CHECK(JoinAs(I_C3, "Cli3"), "client 3 MP_Join");
    T_CHECK(WaitFor(CondInSession, I_C3, 0, 5000), "client 3 in session");
    PumpFor(200);
    CheckSameTable(sClients, 3, 4);
    Sel(I_C1); T_CHECK(MP_GetLocalSlot() == 1, "client 1 got slot 1");
    Sel(I_C3); T_CHECK(MP_GetLocalSlot() == 3, "client 3 got slot 3");
    T_CHECK(sEvents[I_HOST].joined == 3 && sEvents[I_HOST].started == 1, "host events: started + 3 joined");
    T_CHECK(sEvents[I_C1].started == 1 && sEvents[I_C1].joined == 2, "client 1 events: started + 2 joined");
    T_CHECK(sEvents[I_C3].started == 1 && sEvents[I_C3].joined == 0, "client 3 events: started, no joined for existing players");
    // Pings flow for a while without disturbing anything
    PumpFor(2500);
    CheckSameTable(sClients, 3, 4);
    Report("host + 3 clients share the same 4-player table", failuresBefore);

    // --- rejects ---
    failuresBefore = sFail;
    T_CHECK(JoinAs(I_C5, "Cli5"), "5th client MP_Join");
    ExpectReject(I_C5, MP_REASON_FULL, "5th client rejected with FULL");
    Report("5th client gets REJECT(FULL)", failuresBefore);

    failuresBefore = sFail;
    Sel(I_FAKEBUILD);
    MP_SetFakeBuildId("selftest");
    T_CHECK(JoinAs(I_FAKEBUILD, "Fake"), "fake-build client MP_Join");
    MpBuild_ClearOverrides();
    ExpectReject(I_FAKEBUILD, MP_REASON_BUILD, "fake-build client rejected with BUILD");
    Report("--mp-fake-build client gets REJECT(BUILD)", failuresBefore);

    failuresBefore = sFail;
    Sel(I_FAKEPROTO);
    MP_SetFakeProtocolVersion(99);
    T_CHECK(JoinAs(I_FAKEPROTO, "Fake"), "fake-protocol client MP_Join");
    MpBuild_ClearOverrides();
    ExpectReject(I_FAKEPROTO, MP_REASON_VERSION, "fake-protocol client rejected with VERSION");
    Report("--mp-fake-protocol client gets REJECT(VERSION)", failuresBefore);

    // --- garbage before HELLO: silent reset ---
    failuresBefore = sFail;
    RawConnect();
    // ENet raises CONNECT on the client before the host sees the ack: wait for the host.
    T_CHECK(WaitFor(CondHostPeers, I_HOST, 4, 3000), "host sees the raw peer as pending");
    memset(junk, 0x5A, sizeof(junk));
    RawSend(0x55, MP_CHANNEL_CONTROL, junk, sizeof(junk), FALSE);
    T_CHECK(WaitFor(CondHostPeers, I_HOST, 3, 3000), "host resets a peer that sends garbage before HELLO");
    RawClose();
    Report("garbage before HELLO resets the peer", failuresBefore);

    // --- malformed HELLO: REJECT(BAD_DATA) ---
    failuresBefore = sFail;
    RawConnect();
    helloLen = RawHello(payload, "Raw");
    T_CHECK(helloLen == MP_HELLO_SIZE, "raw HELLO built");
    for (i = 16; i < 24; i++)
        payload[i] = 0xBB; // name without EOS
    RawSend(MP_MSG_HELLO, MP_CHANNEL_CONTROL, payload, helloLen, FALSE);
    T_CHECK(PumpUntil(CondRawRejected, 3000) && sRaw.rejectReason == MP_REASON_BAD_DATA, "malformed HELLO gets REJECT(BAD_DATA)");
    RawClose();
    T_CHECK(WaitFor(CondHostPeers, I_HOST, 3, 4000), "host frees the BAD_DATA peer");
    Report("malformed HELLO gets REJECT(BAD_DATA)", failuresBefore);

    // --- client BYE -> PLAYER_LEFT(LEFT) everywhere ---
    failuresBefore = sFail;
    memset(sEvents, 0, sizeof(sEvents));
    Sel(I_C3);
    MP_Leave();
    T_CHECK(MP_GetState() == MP_STATE_OFFLINE, "client 3 offline after MP_Leave");
    T_CHECK(WaitFor(CondCount, I_C1, 3, 3000) && WaitFor(CondCount, I_C2, 3, 3000), "clients see 3 players");
    CheckSameTable(sTwoClients, 2, 3);
    T_CHECK(sEvents[I_HOST].left == 1 && sEvents[I_HOST].lastLeftReason == MP_REASON_LEFT, "host: PLAYER_LEFT(LEFT)");
    T_CHECK(sEvents[I_C1].left == 1 && sEvents[I_C1].lastLeftReason == MP_REASON_LEFT, "client 1: PLAYER_LEFT(LEFT)");
    T_CHECK(sEvents[I_C2].left == 1 && sEvents[I_C2].lastLeftReason == MP_REASON_LEFT, "client 2: PLAYER_LEFT(LEFT)");
    T_CHECK(WaitFor(CondTransportClosed, I_C3, 0, 2000), "client 3 transport closed after the leave flush");
    Report("client BYE gives PLAYER_LEFT(LEFT) to everyone", failuresBefore);

    // --- kick after 8 malformed messages ---
    failuresBefore = sFail;
    memset(sEvents, 0, sizeof(sEvents));
    RawConnect();
    helloLen = RawHello(payload, "Raw");
    RawSend(MP_MSG_HELLO, MP_CHANNEL_CONTROL, payload, helloLen, FALSE);
    T_CHECK(PumpUntil(CondRawWelcomed, 3000), "raw client welcomed");
    T_CHECK(sRaw.welcomeSlot == 3 && sRaw.welcomeCount == 4, "raw client got the freed slot 3");
    T_CHECK(WaitFor(CondCount, I_C1, 4, 2000), "client 1 sees the raw player join");
    for (i = 0; i < MP_MALFORMED_LIMIT - 1; i++)
    {
        switch (i % 4)
        {
        case 0: RawSend(0x55, MP_CHANNEL_CONTROL, junk, 4, FALSE); break;                 // unknown type
        case 1: RawSend(MP_MSG_PLAYER_LEFT, MP_CHANNEL_CONTROL, junk, 4, FALSE); break;   // wrong direction
        case 2: RawSend(MP_MSG_BYE, MP_CHANNEL_CONTROL, junk, 3, TRUE); break;            // length field mismatch
        case 3: RawSend(MP_MSG_PING, MP_CHANNEL_CONTROL, junk, 4, FALSE); break;          // wrong channel
        }
    }
    PumpFor(400);
    T_CHECK(CountOf(I_HOST) == 4 && sRaw.byeReason == 0xFF, "7 malformed messages: not kicked yet");
    RawSend(0x55, MP_CHANNEL_CONTROL, junk, 4, FALSE);
    T_CHECK(PumpUntil(CondRawBye, 3000) && sRaw.byeReason == MP_BYE_PROTOCOL_ERROR, "8th malformed message: BYE(PROTOCOL_ERROR)");
    T_CHECK(WaitFor(CondCount, I_C1, 3, 3000), "client 1 sees the kicked player leave");
    T_CHECK(CountOf(I_HOST) == 3, "host removed the kicked player");
    T_CHECK(sEvents[I_HOST].lastLeftReason == MP_REASON_KICKED && sEvents[I_C1].lastLeftReason == MP_REASON_KICKED, "PLAYER_LEFT(KICKED)");
    RawClose();
    T_CHECK(WaitFor(CondHostPeers, I_HOST, 2, 4000), "host frees the kicked peer");
    Report("kick after 8 malformed messages", failuresBefore);

    // --- PLAYER_UPDATE relay ---
    failuresBefore = sFail;
    memset(sEvents, 0, sizeof(sEvents));
    Sel(I_C1);
    MP_SetNameOverride("Neo");
    MP_RefreshLocalSummary();
    PumpFor(500);
    CheckSameTable(sTwoClients, 2, 3);
    Sel(I_C1);
    {
        const struct MpPlayer *self = MP_GetPlayer(1);
        u8 neo[MP_PLAYER_NAME_SIZE];

        memset(neo, MP_EOS, sizeof(neo));
        neo[0] = 0xC8; // N
        neo[1] = 0xD9; // e
        neo[2] = 0xE3; // o
        T_CHECK(self != NULL && memcmp(self->summary.name, neo, sizeof(neo)) == 0, "client 1 renamed to Neo");
    }
    T_CHECK(sEvents[I_HOST].updated == 1 && sEvents[I_C2].updated == 1 && sEvents[I_C1].updated == 0, "PLAYER_UPDATED on host and client 2 only");
    Report("PLAYER_UPDATE is relayed", failuresBefore);

    // --- silent client death (kill -9): slot held for a reconnect, then PLAYER_LEFT ---
    // PS-054: the host holds a lost client's slot for MP_RECONNECT_GRACE_MS (35 s). This case
    // runs with the grace scaled down 10x (3.5 s); detection itself is the game's (ENet 4-7 s).
    failuresBefore = sFail;
    memset(sEvents, 0, sizeof(sEvents));
    MpNetTiming_Set(10);
    Sel(I_C2);
    MpTransport_Close();          // nothing is sent, like a killed process
    MpSession_ResetInstance(I_C2);
    start = MpTransport_GetTimeMs();
    T_CHECK(WaitFor(CondCount, I_HOST, 2, 16000), "host drops the dead client");
    elapsed = MpTransport_GetTimeMs() - start;
    T_CHECK(elapsed <= 15000, "dead client dropped within 15 s (with the grace scaled to 3.5 s)");
    T_CHECK(sEvents[I_HOST].playerReconnecting == 1, "host held the slot before dropping it");
    T_CHECK(WaitFor(CondCount, I_C1, 2, 2000), "client 1 sees the dead client leave");
    T_CHECK(sEvents[I_C1].playerReconnecting == 1, "client 1 saw the slot held first (PLAYER_STATUS)");
    T_CHECK(sEvents[I_C1].lastLeftReason == MP_REASON_CONNECTION_LOST, "client 1: PLAYER_LEFT(CONNECTION_LOST)");
    MpNetTiming_Set(1);
    MP_LOG("selftest: session: dead client dropped after %u ms (grace 3.5 s)\n", (unsigned)elapsed);
    Report("silent client death: slot held, then PLAYER_LEFT(CONNECTION_LOST)", failuresBefore);

    // --- 100 KB random UDP at the host port (AD-9) ---
    failuresBefore = sFail;
    sent = MpTransport_DebugSendRandomUdp("127.0.0.1", sPort, 100 * 1024, 0x5EED1234u);
    T_CHECK(sent > 0, "random UDP sent");
    PumpFor(1500);
    T_CHECK(StateOf(I_HOST) == MP_STATE_IN_SESSION && CountOf(I_HOST) == 2, "host still in session after the flood");
    T_CHECK(StateOf(I_C1) == MP_STATE_IN_SESSION && CountOf(I_C1) == 2, "client still in session after the flood");
    MP_LOG("selftest: session: sent %u random datagrams (100 KB, seed 0x5EED1234)\n", (unsigned)sent);
    Report("100 KB of random UDP does not disturb the session", failuresBefore);

    // --- host BYE: DISCONNECTED(HOST_CLOSED) ---
    failuresBefore = sFail;
    memset(sEvents, 0, sizeof(sEvents));
    Sel(I_HOST);
    MP_Leave();
    T_CHECK(WaitFor(CondNotInSession, I_C1, 0, 3000), "client 1 leaves the session");
    Sel(I_C1);
    T_CHECK(MP_GetState() == MP_STATE_ERROR && MP_GetErrorReason() == MP_REASON_HOST_CLOSED, "client 1 ERROR(HOST_CLOSED)");
    DrainEvents(I_C1);
    T_CHECK(sEvents[I_C1].disconnected == 1 && sEvents[I_C1].lastDisconnectReason == MP_REASON_HOST_CLOSED, "client 1: DISCONNECTED(HOST_CLOSED)");
    T_CHECK(MP_GetPlayer(0) == NULL && MP_GetPlayerCount() == 0, "client 1 table cleared, offline play continues");
    MP_Leave();
    T_CHECK(PumpUntil(CondAllClosed, 3000), "all transports closed");
    Report("host BYE gives DISCONNECTED(HOST_CLOSED)", failuresBefore);

    // --- silent host death: DISCONNECTED(CONNECTION_LOST) within 15 s ---
    failuresBefore = sFail;
    memset(sEvents, 0, sizeof(sEvents));
    Sel(I_HOST);
    MP_SetNameOverride("Host");
    T_CHECK(MP_Host(sPort), "host again on the same port");
    T_CHECK(JoinAs(I_C1, "Cli1"), "client 1 joins again");
    T_CHECK(WaitFor(CondInSession, I_C1, 0, 5000), "client 1 in session again");
    CheckSameTable(sOneClient, 1, 2);
    // PS-054: the client first tries to reconnect for MP_RECONNECT_WINDOW_MS (scaled here to 3 s).
    MpNetTiming_Set(10);
    Sel(I_HOST);
    MpTransport_Close();
    MpSession_ResetInstance(I_HOST);
    start = MpTransport_GetTimeMs();
    T_CHECK(WaitFor(CondNotInSession, I_C1, 0, 16000), "client 1 notices the dead host");
    elapsed = MpTransport_GetTimeMs() - start;
    T_CHECK(elapsed <= 15000, "dead host noticed within 15 s (with the reconnect window scaled to 3 s)");
    Sel(I_C1);
    T_CHECK(MP_GetState() == MP_STATE_ERROR && MP_GetErrorReason() == MP_REASON_CONNECTION_LOST, "client 1 ERROR(CONNECTION_LOST)");
    DrainEvents(I_C1);
    T_CHECK(sEvents[I_C1].reconnecting == 1, "client 1 tried to reconnect first");
    T_CHECK(sEvents[I_C1].disconnected == 1, "client 1: DISCONNECTED");
    MP_Leave();
    MpNetTiming_Set(1);
    MP_LOG("selftest: session: dead host given up after %u ms (window 3 s)\n", (unsigned)elapsed);
    Report("silent host death: reconnect tries, then DISCONNECTED(CONNECTION_LOST)", failuresBefore);

    // --- BUG-001: PING/PONG before WELCOME is ignored, not counted as malformed ---
    // The raw instance plays a host that sends channel-1 PINGs before its WELCOME.
    failuresBefore = sFail;
    PumpUntil(CondAllClosed, 3000);
    memset(&sRaw, 0, sizeof(sRaw));
    sRaw.byeReason = 0xFF;
    Sel(I_RAW);
    T_CHECK(MpTransport_Listen(sPort, 2), "raw host listens");
    T_CHECK(JoinAs(I_C2, "Cli2"), "client joins the raw host");
    T_CHECK(PumpUntil(CondRawConnected, 5000), "raw host sees the client");
    PumpFor(200); // let the HELLO arrive (the raw host ignores it)
    {
        struct MpMsgPing ping;
        struct MpMsgWelcome welcome;
        u8 pkt[MP_MAX_PACKET_SIZE];
        u16 len;

        ping.timeMs = 1234;
        len = MpProto_WritePing(pkt, sizeof(pkt), MP_MSG_PING, &ping);
        for (i = 0; i < MP_MALFORMED_LIMIT + 1; i++)
            RawSend(MP_MSG_PING, MP_CHANNEL_PRESENCE, &pkt[MP_HEADER_SIZE], (u16)(len - MP_HEADER_SIZE), FALSE);
        len = MpProto_WritePing(pkt, sizeof(pkt), MP_MSG_PONG, &ping);
        RawSend(MP_MSG_PONG, MP_CHANNEL_PRESENCE, &pkt[MP_HEADER_SIZE], (u16)(len - MP_HEADER_SIZE), FALSE);
        PumpFor(300);
        T_CHECK(StateOf(I_C2) == MP_STATE_CONNECTING, "client still connecting after early PINGs");
        Sel(I_C2);
        T_CHECK(gMpSession->client.malformed == 0, "early PING/PONG not counted as malformed");

        memset(&welcome, 0, sizeof(welcome));
        welcome.yourSlot = 1;
        welcome.yourEpoch = 2;
        memset(welcome.token, 0x77, sizeof(welcome.token)); // PS-054: any non-zero token
        welcome.playerCount = 2;
        welcome.players[0].slot = 0;
        welcome.players[0].epoch = 1;
        welcome.players[0].entryFlags = MP_ENTRY_FLAG_HOST;
        MP_BuildPlayerSummary(&welcome.players[0].summary);
        welcome.players[1].slot = 1;
        welcome.players[1].epoch = 2;
        MP_BuildPlayerSummary(&welcome.players[1].summary);
        len = MpProto_WriteWelcome(pkt, sizeof(pkt), &welcome);
        T_CHECK(len > MP_HEADER_SIZE, "raw WELCOME built");
        RawSend(MP_MSG_WELCOME, MP_CHANNEL_CONTROL, &pkt[MP_HEADER_SIZE], (u16)(len - MP_HEADER_SIZE), FALSE);
    }
    T_CHECK(WaitFor(CondInSession, I_C2, 0, 3000), "client connects after the late WELCOME");
    Sel(I_C2);
    T_CHECK(MP_GetLocalSlot() == 1 && gMpSession->client.malformed == 0, "connected as slot 1 with malformed count 0");
    MP_Leave();
    RawClose();
    Report("PING/PONG before WELCOME is ignored (BUG-001)", failuresBefore);

    // Clean up every instance
    for (i = 0; i < SESSION_INSTANCES; i++)
    {
        Sel(i);
        MP_Leave();
    }
    PumpUntil(CondAllClosed, 2000);
    for (i = 0; i <= I_RAW; i++)
    {
        Sel(i);
        MpTransport_Close();
        MpSession_ResetInstance(i);
    }
    Sel(0);
    return sFail;
}
