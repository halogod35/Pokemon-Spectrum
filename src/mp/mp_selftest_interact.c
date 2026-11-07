// --mp-selftest, PS-041 interaction group:
//  1. INTERACT_* parser fuzzing (protocol.md 8.3): round trip for all three types, truncated,
//     oversized, every single-bit flip (deterministic, never a crash), 100 000 garbage buffers
//     from a fixed seed, and targeted field cases.
//  2. In-process host + 2 clients (session instances over loopback), with the answer timeout
//     shortened to INTERACT_TEST_TIMEOUT_MS:
//     request -> accept, request -> decline (host as target), busy (target already asked),
//     explicit BUSY answer, timeout (nobody answers), requester cancels, rate limit (second
//     request within 1 s, and a second one while one is pending), target not on the
//     requester's map, requester leaving mid-request, malformed INTERACT from the host side.

#include <string.h>
#include "mp/mp.h"
#include "mp/mp_internal.h"
#include "mp/mp_protocol.h"
#include "mp/mp_selftest.h"
#include "mp/mp_session_internal.h"
#include "mp/mp_transport.h"
#include "constants/maps.h"

static u32 sFail;

#define I_CHECK(cond, what)                                                         \
    do                                                                              \
    {                                                                               \
        if (!(cond))                                                                \
        {                                                                           \
            MP_LOG("selftest: FAIL: %s (%s:%d)\n", what, __FILE__, __LINE__);       \
            sFail++;                                                                \
        }                                                                           \
    } while (0)

static u32 sRng = 0x4D504941u; // "MPIA"

static u32 Rand(void)
{
    sRng ^= sRng << 13;
    sRng ^= sRng >> 17;
    sRng ^= sRng << 5;
    return sRng;
}

static void Report(const char *what, u32 failuresBefore)
{
    if (sFail == failuresBefore)
        MP_LOG("selftest: interact: %s OK\n", what);
}

// ===========================================================================
// 1. Parser
// ===========================================================================

static const u8 sTypes[3] = { MP_MSG_INTERACT_REQUEST, MP_MSG_INTERACT_RESPONSE, MP_MSG_INTERACT_CANCEL };

static void FixtureInteract(u8 type, struct MpMsgInteract *m)
{
    memset(m, 0, sizeof(*m));
    m->slot = 2;
    m->epoch = 7;
    m->kind = MP_INTERACT_BATTLE;
    m->requestId = 0xA5;
    m->result = (type == MP_MSG_INTERACT_RESPONSE) ? MP_INTERACT_RESULT_DECLINED : 0;
}

static bool8 SameInteract(const struct MpMsgInteract *a, const struct MpMsgInteract *b)
{
    return a->slot == b->slot && a->epoch == b->epoch && a->kind == b->kind
        && a->requestId == b->requestId && a->result == b->result;
}

static void TestParser(void)
{
    u8 buf[MP_MAX_PACKET_SIZE + 8];
    u8 copy[MP_MAX_PACKET_SIZE + 8];
    struct MpMsgInteract in, out, out2;
    struct MpMsgHeader hdr;
    u32 failuresBefore = sFail;
    u32 t, i, bit, n, okFlips;
    u16 len;
    u8 r1, r2;

    for (t = 0; t < 3; t++)
    {
        u8 type = sTypes[t];
        const u8 *payload = &buf[MP_HEADER_SIZE];

        FixtureInteract(type, &in);
        len = MpProto_WriteInteract(buf, MP_MAX_PACKET_SIZE, type, &in);
        I_CHECK(len == MP_HEADER_SIZE + MP_INTERACT_SIZE, "INTERACT write size");
        I_CHECK(MpProto_ParseHeader(buf, len, &hdr) && hdr.type == type && hdr.version == MP_PROTOCOL_VERSION
             && hdr.length == MP_INTERACT_SIZE, "INTERACT header");
        // Round trip
        I_CHECK(MpProto_ParseInteract(type, payload, MP_INTERACT_SIZE, &out) == MP_PARSE_OK && SameInteract(&in, &out), "INTERACT round trip");
        // Truncated
        for (n = 0; n < MP_INTERACT_SIZE; n++)
            I_CHECK(MpProto_ParseInteract(type, payload, n, &out) == MP_PARSE_ERR_TRUNCATED, "INTERACT truncated");
        I_CHECK(MpProto_ParseInteract(type, NULL, 0, &out) == MP_PARSE_ERR_TRUNCATED, "INTERACT NULL");
        // Oversized: +1, padded to 512 and 513 bytes
        memset(copy, 0, sizeof(copy));
        memcpy(copy, payload, MP_INTERACT_SIZE);
        I_CHECK(MpProto_ParseInteract(type, copy, MP_INTERACT_SIZE + 1, &out) == MP_PARSE_ERR_OVERSIZED, "INTERACT +1");
        I_CHECK(MpProto_ParseInteract(type, copy, 512, &out) == MP_PARSE_ERR_OVERSIZED, "INTERACT 512");
        I_CHECK(MpProto_ParseInteract(type, copy, 513, &out) == MP_PARSE_ERR_OVERSIZED, "INTERACT 513");
        // Writer refuses a too-small buffer
        I_CHECK(MpProto_WriteInteract(copy, MP_HEADER_SIZE + MP_INTERACT_SIZE - 1, type, &in) == 0, "INTERACT write into small buffer");
        // Bit flips: deterministic, and an OK result is always within the field rules
        okFlips = 0;
        for (i = 0; i < MP_INTERACT_SIZE; i++)
        {
            for (bit = 0; bit < 8; bit++)
            {
                memcpy(copy, payload, MP_INTERACT_SIZE);
                copy[i] ^= (u8)(1 << bit);
                r1 = MpProto_ParseInteract(type, copy, MP_INTERACT_SIZE, &out);
                r2 = MpProto_ParseInteract(type, copy, MP_INTERACT_SIZE, &out2);
                I_CHECK(r1 == r2 && (r1 != MP_PARSE_OK || SameInteract(&out, &out2)), "INTERACT bit flip deterministic");
                if (r1 == MP_PARSE_OK)
                {
                    okFlips++;
                    I_CHECK(out.slot < MP_MAX_PLAYERS && out.epoch != 0
                         && out.kind > MP_INTERACT_NONE && out.kind < MP_INTERACT_KIND_COUNT, "INTERACT flip result in range");
                    if (type == MP_MSG_INTERACT_RESPONSE)
                        I_CHECK(out.result >= MP_INTERACT_RESULT_ACCEPTED && out.result <= MP_INTERACT_RESULT_UNAVAILABLE, "RESPONSE flip result in range");
                    else
                        I_CHECK(out.result == 0, "REQUEST/CANCEL result parses as 0");
                }
                else
                {
                    I_CHECK(out.slot == 0 && out.epoch == 0 && out.kind == 0, "INTERACT error zeroes out");
                }
            }
        }
        I_CHECK(okFlips > 0, "some INTERACT bit flips are still valid (reserved bytes)");
    }

    // Targeted cases
    FixtureInteract(MP_MSG_INTERACT_RESPONSE, &in);
    len = MpProto_WriteInteract(buf, MP_MAX_PACKET_SIZE, MP_MSG_INTERACT_RESPONSE, &in);
    memcpy(copy, &buf[MP_HEADER_SIZE], MP_INTERACT_SIZE);
    copy[0] = MP_MAX_PLAYERS;
    I_CHECK(MpProto_ParseInteract(MP_MSG_INTERACT_RESPONSE, copy, MP_INTERACT_SIZE, &out) == MP_PARSE_ERR_BAD_FIELD, "slot 4 rejected");
    copy[0] = 0xFF;
    I_CHECK(MpProto_ParseInteract(MP_MSG_INTERACT_RESPONSE, copy, MP_INTERACT_SIZE, &out) == MP_PARSE_ERR_BAD_FIELD, "slot 0xFF rejected");
    memcpy(copy, &buf[MP_HEADER_SIZE], MP_INTERACT_SIZE);
    copy[1] = 0;
    I_CHECK(MpProto_ParseInteract(MP_MSG_INTERACT_RESPONSE, copy, MP_INTERACT_SIZE, &out) == MP_PARSE_ERR_BAD_FIELD, "epoch 0 rejected");
    memcpy(copy, &buf[MP_HEADER_SIZE], MP_INTERACT_SIZE);
    copy[2] = MP_INTERACT_NONE;
    I_CHECK(MpProto_ParseInteract(MP_MSG_INTERACT_RESPONSE, copy, MP_INTERACT_SIZE, &out) == MP_PARSE_ERR_BAD_FIELD, "kind 0 rejected");
    copy[2] = MP_INTERACT_KIND_COUNT;
    I_CHECK(MpProto_ParseInteract(MP_MSG_INTERACT_RESPONSE, copy, MP_INTERACT_SIZE, &out) == MP_PARSE_ERR_BAD_FIELD, "kind MP_INTERACT_KIND_COUNT rejected");
    memcpy(copy, &buf[MP_HEADER_SIZE], MP_INTERACT_SIZE);
    copy[4] = MP_INTERACT_RESULT_NONE;
    I_CHECK(MpProto_ParseInteract(MP_MSG_INTERACT_RESPONSE, copy, MP_INTERACT_SIZE, &out) == MP_PARSE_ERR_BAD_FIELD, "RESPONSE result 0 rejected");
    copy[4] = MP_INTERACT_RESULT_CANCELLED;
    I_CHECK(MpProto_ParseInteract(MP_MSG_INTERACT_RESPONSE, copy, MP_INTERACT_SIZE, &out) == MP_PARSE_ERR_BAD_FIELD, "RESPONSE result CANCELLED rejected");
    I_CHECK(MpProto_ParseInteract(MP_MSG_INTERACT_REQUEST, copy, MP_INTERACT_SIZE, &out) == MP_PARSE_OK && out.result == 0, "REQUEST ignores the result byte");
    memcpy(copy, &buf[MP_HEADER_SIZE], MP_INTERACT_SIZE);
    copy[5] = 0xFF; copy[6] = 0xFF; copy[7] = 0xFF;
    I_CHECK(MpProto_ParseInteract(MP_MSG_INTERACT_RESPONSE, copy, MP_INTERACT_SIZE, &out) == MP_PARSE_OK, "reserved bytes ignored");
    I_CHECK(MpProto_ParseInteract(MP_MSG_PRESENCE, copy, MP_INTERACT_SIZE, &out) == MP_PARSE_ERR_BAD_FIELD, "non-INTERACT type rejected");
    I_CHECK(MpProto_WriteInteract(buf, MP_MAX_PACKET_SIZE, MP_MSG_BYE, &in) == 0, "writer refuses a non-INTERACT type");

    // Garbage: random length 0..600, random content, random type of the three
    MP_LOG("selftest: interact: garbage seed 0x%08X\n", (unsigned)sRng);
    for (i = 0; i < 100000; i++)
    {
        u32 glen = Rand() % 601;
        u8 type = sTypes[Rand() % 3];

        for (n = 0; n < glen && n < sizeof(copy); n++)
            copy[n] = (u8)Rand();
        if (glen > sizeof(copy))
            glen = sizeof(copy);
        if (MpProto_ParseInteract(type, copy, glen, &out) == MP_PARSE_OK)
            I_CHECK(glen == MP_INTERACT_SIZE && out.slot < MP_MAX_PLAYERS && out.epoch != 0, "garbage accepted only when well formed");
    }
    Report("INTERACT parser fuzzing", failuresBefore);
}

// ===========================================================================
// 2. Host + 2 clients
// ===========================================================================

#define I_HOST  0
#define I_C1    1
#define I_C2    2
#define INSTANCES 3

#define INTERACT_PORT_FIRST 27560
#define INTERACT_PORT_LAST  27569
#define INTERACT_TEST_TIMEOUT_MS 1500

struct InteractSeen
{
    u8 requests;
    u8 requestSlot;
    u8 requestKind;
    u8 results;
    u8 resultSlot;
    u8 resultKind;
    u8 result;
    u8 cancels;
    u8 cancelSlot;
    u8 cancelResult;
};

static struct InteractSeen sSeen[INSTANCES];
static u16 sPort;
static u8 sWaitInstance;
static u8 sWaitSlot;
static u8 sWaitCount;
static u16 sWaitMap;     // MAP_* the host must have in sWaitSlot's presence

static void Sel(u8 instance)
{
    MpSession_SelectInstance(instance);
}

static void PumpOnce(void)
{
    struct MpEvent ev;
    u8 i;

    for (i = 0; i < INSTANCES; i++)
    {
        Sel(i);
        MP_Update();
        while (MP_PollEvent(&ev))
        {
            struct InteractSeen *s = &sSeen[i];

            switch (ev.type)
            {
            case MP_EVENT_INTERACT_REQUEST:
                s->requests++;
                s->requestSlot = ev.slot;
                s->requestKind = ev.kind;
                break;
            case MP_EVENT_INTERACT_RESULT:
                s->results++;
                s->resultSlot = ev.slot;
                s->resultKind = ev.kind;
                s->result = ev.result;
                break;
            case MP_EVENT_INTERACT_CANCELLED:
                s->cancels++;
                s->cancelSlot = ev.slot;
                s->cancelResult = ev.result;
                break;
            }
        }
    }
}

static void PumpFor(u32 ms)
{
    u32 start = MpTransport_GetTimeMs();

    while ((u32)(MpTransport_GetTimeMs() - start) < ms)
        PumpOnce();
}

static bool8 PumpUntil(bool8 (*cond)(void), u32 timeoutMs)
{
    u32 start = MpTransport_GetTimeMs();

    while (!cond() && (u32)(MpTransport_GetTimeMs() - start) < timeoutMs)
        PumpOnce();
    return cond();
}

static bool8 CondInSession(void)    { Sel(sWaitInstance); return MP_GetState() == MP_STATE_IN_SESSION; }
static bool8 CondRequests(void)     { return sSeen[sWaitInstance].requests >= sWaitCount; }
static bool8 CondResults(void)      { return sSeen[sWaitInstance].results >= sWaitCount; }
static bool8 CondCancels(void)      { return sSeen[sWaitInstance].cancels >= sWaitCount; }

static bool8 CondHostHasPresence(void)
{
    const struct MpPlayer *p;

    Sel(I_HOST);
    p = MP_GetPlayer(sWaitSlot);
    // hasPresence alone is not enough: a client that joined before setting its presence
    // first announces MP_MAP_NONE (PS-030), so wait for the map we set.
    return p != NULL && p->hasPresence && p->presence.mapGroup == MAP_GROUP(sWaitMap) && p->presence.mapNum == MAP_NUM(sWaitMap);
}

static bool8 CondSlotGone(void)
{
    Sel(sWaitInstance);
    return MP_GetPlayer(sWaitSlot) == NULL;
}

static bool8 CondAllClosed(void)
{
    u8 i;

    for (i = 0; i < INSTANCES; i++)
    {
        Sel(i);
        if (MpTransport_IsOpen())
            return FALSE;
    }
    return TRUE;
}

static bool8 Wait(bool8 (*cond)(void), u8 instance, u8 count, u32 timeoutMs)
{
    sWaitInstance = instance;
    sWaitCount = count;
    return PumpUntil(cond, timeoutMs);
}

static void SetMap(u8 instance, u16 mapId)
{
    struct MpPresence p;

    memset(&p, 0, sizeof(p));
    p.mapGroup = (u8)MAP_GROUP(mapId);
    p.mapNum = (u8)MAP_NUM(mapId);
    p.x = 5 + instance;
    p.y = 5;
    p.facing = DIR_SOUTH;
    p.movement = MP_MOVE_NONE;
    p.avatarState = MP_AVATAR_ON_FOOT;
    p.busy = MP_BUSY_NONE;
    p.stepId = (u8)(mapId + instance);
    Sel(instance);
    MP_SetLocalPresence(&p);
}

static bool8 JoinAs(u8 instance, const char *name)
{
    Sel(instance);
    if (!MP_SetNameOverride(name))
        return FALSE;
    return MP_Join("127.0.0.1", sPort);
}

static void CleanUp(void)
{
    u8 i;

    for (i = 0; i < INSTANCES; i++)
    {
        Sel(i);
        MP_Leave();
    }
    PumpUntil(CondAllClosed, 2000);
    for (i = 0; i < INSTANCES; i++)
    {
        Sel(i);
        MpTransport_Close();
        MpSession_ResetInstance(i);
    }
    Sel(0);
}

// Waits out the host's per-requester gap so the next request is not rate limited.
static void WaitGap(void)
{
    PumpFor(MP_INTERACT_MIN_GAP_MS + 100);
}

static void TestScenario(void)
{
    u32 failuresBefore;
    u8 i;
    bool8 hosting = FALSE;
    const u16 sameMap = MAP_LITTLEROOT_TOWN;
    const u16 otherMap = MAP_OLDALE_TOWN;

    memset(sSeen, 0, sizeof(sSeen));
    for (i = 0; i < INSTANCES; i++)
        MpSession_ResetInstance(i);

    // --- session: host + 2 clients, everyone on the same map ---
    failuresBefore = sFail;
    Sel(I_HOST);
    MP_SetNameOverride("Host");
    for (sPort = INTERACT_PORT_FIRST; sPort <= INTERACT_PORT_LAST && !hosting; sPort++)
        hosting = MP_Host(sPort);
    sPort--;
    I_CHECK(hosting, "interact host on a port in 27560..27569");
    if (!hosting)
        return;
    I_CHECK(JoinAs(I_C1, "IntA"), "client 1 MP_Join");
    I_CHECK(Wait(CondInSession, I_C1, 0, 5000), "client 1 in session");
    I_CHECK(JoinAs(I_C2, "IntB"), "client 2 MP_Join");
    I_CHECK(Wait(CondInSession, I_C2, 0, 5000), "client 2 in session");
    for (i = 0; i < INSTANCES; i++)
        SetMap(i, sameMap);
    for (i = 0; i < INSTANCES; i++)
    {
        sWaitSlot = i;
        sWaitMap = sameMap;
        I_CHECK(PumpUntil(CondHostHasPresence, 2000), "host has everyone's presence");
    }
    Sel(I_C1); I_CHECK(MP_GetLocalSlot() == 1, "client 1 is slot 1");
    Sel(I_C2); I_CHECK(MP_GetLocalSlot() == 2, "client 2 is slot 2");
    Report("session for the interaction tests", failuresBefore);

    // --- API refusals ---
    failuresBefore = sFail;
    Sel(I_C1);
    I_CHECK(!MP_SendInteractRequest(1, MP_INTERACT_TRADE), "cannot ask yourself");
    I_CHECK(!MP_SendInteractRequest(3, MP_INTERACT_TRADE), "cannot ask an empty slot");
    I_CHECK(!MP_SendInteractRequest(2, MP_INTERACT_NONE), "kind NONE refused");
    I_CHECK(!MP_SendInteractRequest(MP_MAX_PLAYERS, MP_INTERACT_TRADE), "slot out of range refused");
    Report("API refusals", failuresBefore);

    // --- request -> accept (client to client, relayed) ---
    failuresBefore = sFail;
    Sel(I_C1);
    I_CHECK(MP_SendInteractRequest(2, MP_INTERACT_TRADE), "C1 asks C2 to trade");
    I_CHECK(MP_IsInteractRequestPending(), "C1 has a request pending");
    I_CHECK(!MP_SendInteractRequest(MP_HOST_SLOT, MP_INTERACT_BATTLE), "a second request while one is pending is refused");
    I_CHECK(Wait(CondRequests, I_C2, 1, 2000), "C2 receives the request");
    I_CHECK(sSeen[I_C2].requestSlot == 1 && sSeen[I_C2].requestKind == MP_INTERACT_TRADE, "C2 sees C1 asking to trade");
    Sel(I_C2);
    I_CHECK(MP_HasIncomingInteract(), "C2 has an incoming request");
    MP_RespondInteract(MP_INTERACT_RESULT_ACCEPTED);
    I_CHECK(!MP_HasIncomingInteract(), "answering clears the incoming request");
    I_CHECK(Wait(CondResults, I_C1, 1, 2000), "C1 gets the result");
    I_CHECK(sSeen[I_C1].resultSlot == 2 && sSeen[I_C1].resultKind == MP_INTERACT_TRADE
         && sSeen[I_C1].result == MP_INTERACT_RESULT_ACCEPTED, "C1: C2 accepted the trade");
    Sel(I_C1);
    I_CHECK(!MP_IsInteractRequestPending(), "C1's request is done");
    Report("request -> accept", failuresBefore);

    // --- request -> decline (host is the target) ---
    failuresBefore = sFail;
    Sel(I_C2);
    I_CHECK(MP_SendInteractRequest(MP_HOST_SLOT, MP_INTERACT_BATTLE), "C2 asks the host to battle");
    I_CHECK(Wait(CondRequests, I_HOST, 1, 2000), "the host receives the request");
    I_CHECK(sSeen[I_HOST].requestSlot == 2 && sSeen[I_HOST].requestKind == MP_INTERACT_BATTLE, "host sees C2 asking to battle");
    Sel(I_HOST);
    MP_RespondInteract(MP_INTERACT_RESULT_DECLINED);
    I_CHECK(Wait(CondResults, I_C2, 1, 2000), "C2 gets the result");
    I_CHECK(sSeen[I_C2].resultSlot == MP_HOST_SLOT && sSeen[I_C2].result == MP_INTERACT_RESULT_DECLINED, "C2: the host declined");
    Report("request -> decline", failuresBefore);

    // --- busy: the target is already being asked; then an explicit BUSY answer ---
    failuresBefore = sFail;
    WaitGap();
    Sel(I_C1);
    I_CHECK(MP_SendInteractRequest(2, MP_INTERACT_BATTLE), "C1 asks C2 to battle");
    I_CHECK(Wait(CondRequests, I_C2, 2, 2000), "C2 receives it");
    Sel(I_HOST);
    I_CHECK(MP_SendInteractRequest(2, MP_INTERACT_TRADE), "the host asks C2 too");
    I_CHECK(Wait(CondResults, I_HOST, 1, 2000), "the host gets an answer at once");
    I_CHECK(sSeen[I_HOST].resultSlot == 2 && sSeen[I_HOST].result == MP_INTERACT_RESULT_BUSY, "host: C2 is busy (already asked)");
    I_CHECK(sSeen[I_C2].requests == 2, "C2 never saw the second request");
    Sel(I_C2);
    MP_RespondInteract(MP_INTERACT_RESULT_BUSY);
    I_CHECK(Wait(CondResults, I_C1, 2, 2000), "C1 gets the answer");
    I_CHECK(sSeen[I_C1].result == MP_INTERACT_RESULT_BUSY, "C1: C2 answered busy");
    Report("busy", failuresBefore);

    // --- timeout: nobody answers ---
    failuresBefore = sFail;
    gMpInteractTimeoutMs = INTERACT_TEST_TIMEOUT_MS;
    WaitGap();
    Sel(I_C1);
    I_CHECK(MP_SendInteractRequest(MP_HOST_SLOT, MP_INTERACT_TRADE), "C1 asks the host");
    I_CHECK(Wait(CondRequests, I_HOST, 2, 2000), "the host receives it");
    I_CHECK(Wait(CondResults, I_C1, 3, INTERACT_TEST_TIMEOUT_MS + 3000), "C1 gets an answer after the timeout");
    I_CHECK(sSeen[I_C1].resultSlot == MP_HOST_SLOT && sSeen[I_C1].result == MP_INTERACT_RESULT_TIMEOUT, "C1: timed out");
    I_CHECK(sSeen[I_HOST].cancels == 1 && sSeen[I_HOST].cancelResult == MP_INTERACT_RESULT_TIMEOUT, "host: the prompt is withdrawn (timeout)");
    Sel(I_HOST);
    I_CHECK(!MP_HasIncomingInteract(), "host has no incoming request after the timeout");
    gMpInteractTimeoutMs = MP_INTERACT_TIMEOUT_MS;
    Report("timeout", failuresBefore);

    // --- the requester cancels ---
    failuresBefore = sFail;
    Sel(I_C2);
    I_CHECK(MP_SendInteractRequest(1, MP_INTERACT_TRADE), "C2 asks C1");
    I_CHECK(Wait(CondRequests, I_C1, 1, 2000), "C1 receives it");
    Sel(I_C2);
    MP_CancelInteractRequest();
    I_CHECK(!MP_IsInteractRequestPending(), "C2's request is withdrawn");
    I_CHECK(Wait(CondCancels, I_C1, 1, 2000), "C1 is told");
    I_CHECK(sSeen[I_C1].cancelSlot == 2 && sSeen[I_C1].cancelResult == MP_INTERACT_RESULT_CANCELLED, "C1: C2 cancelled");
    Sel(I_C1);
    MP_RespondInteract(MP_INTERACT_RESULT_ACCEPTED); // too late: ignored
    PumpFor(300);
    I_CHECK(sSeen[I_C2].results == 1, "no result reaches C2 after it cancelled");
    Report("requester cancels", failuresBefore);

    // --- rate limit: a second request within MP_INTERACT_MIN_GAP_MS ---
    failuresBefore = sFail;
    WaitGap();
    Sel(I_HOST);
    I_CHECK(MP_SendInteractRequest(1, MP_INTERACT_BATTLE), "host asks C1");
    I_CHECK(Wait(CondRequests, I_C1, 2, 2000), "C1 receives it");
    Sel(I_C1);
    MP_RespondInteract(MP_INTERACT_RESULT_DECLINED);
    I_CHECK(Wait(CondResults, I_HOST, 2, 2000), "host gets the decline");
    Sel(I_HOST);
    I_CHECK(MP_SendInteractRequest(1, MP_INTERACT_BATTLE), "host asks again at once");
    I_CHECK(Wait(CondResults, I_HOST, 3, 2000), "host gets an answer");
    I_CHECK(sSeen[I_HOST].result == MP_INTERACT_RESULT_UNAVAILABLE, "second request within 1 s: UNAVAILABLE");
    I_CHECK(sSeen[I_C1].requests == 2, "C1 never saw the rate-limited request");
    Report("rate limit", failuresBefore);

    // --- the target is not on the requester's map ---
    failuresBefore = sFail;
    SetMap(I_C2, otherMap);
    sWaitSlot = 2;
    sWaitMap = otherMap;
    I_CHECK(PumpUntil(CondHostHasPresence, 2000), "host sees C2 on the other map");
    WaitGap();
    Sel(I_C1);
    I_CHECK(MP_SendInteractRequest(2, MP_INTERACT_TRADE), "C1 asks C2 on another map");
    I_CHECK(Wait(CondResults, I_C1, 4, 2000), "C1 gets an answer");
    I_CHECK(sSeen[I_C1].result == MP_INTERACT_RESULT_UNAVAILABLE, "not on your map: UNAVAILABLE");
    I_CHECK(sSeen[I_C2].requests == 2, "C2 never saw it");
    SetMap(I_C2, sameMap);
    sWaitSlot = 2;
    sWaitMap = sameMap;
    I_CHECK(PumpUntil(CondHostHasPresence, 2000), "host sees C2 back on the same map");
    Report("target on another map", failuresBefore);

    // --- malformed INTERACT payloads are rejected by the receive paths ---
    failuresBefore = sFail;
    {
        u8 junk[MP_INTERACT_SIZE] = { 9, 0, 7, 0, 0, 0, 0, 0 };

        Sel(I_C1);
        I_CHECK(!MpInteract_OnClientReceive(MP_MSG_INTERACT_REQUEST, junk, sizeof(junk)), "client: bad INTERACT is malformed");
        junk[0] = 1; junk[1] = 1; junk[2] = MP_INTERACT_TRADE; // names ourselves
        I_CHECK(!MpInteract_OnClientReceive(MP_MSG_INTERACT_REQUEST, junk, sizeof(junk)), "client: INTERACT naming ourselves is malformed");
        I_CHECK(!MpInteract_OnClientReceive(MP_MSG_INTERACT_REQUEST, junk, 3), "client: short INTERACT is malformed");
        I_CHECK(!MP_HasIncomingInteract(), "nothing was applied");
    }
    Report("malformed INTERACT", failuresBefore);

    // --- the requester leaves mid-request ---
    failuresBefore = sFail;
    WaitGap();
    Sel(I_C1);
    I_CHECK(MP_SendInteractRequest(2, MP_INTERACT_BATTLE), "C1 asks C2");
    I_CHECK(Wait(CondRequests, I_C2, 3, 2000), "C2 receives it");
    Sel(I_C1);
    MP_Leave();
    I_CHECK(Wait(CondCancels, I_C2, 1, 3000), "C2 is told the request is gone");
    I_CHECK(sSeen[I_C2].cancelSlot == 1, "C2: the request from C1 was withdrawn");
    Sel(I_C2);
    I_CHECK(!MP_HasIncomingInteract(), "C2 has no incoming request");
    sWaitInstance = I_HOST;
    sWaitSlot = 1;
    I_CHECK(PumpUntil(CondSlotGone, 3000), "the host removed C1");
    Report("requester leaves mid-request", failuresBefore);

    CleanUp();
}

u32 MpSelftest_Interact(void)
{
    u32 timeout = gMpInteractTimeoutMs;

    sFail = 0;
    TestParser();
    TestScenario();
    gMpInteractTimeoutMs = timeout;
    return sFail;
}
