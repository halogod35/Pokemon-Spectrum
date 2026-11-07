// --mp-selftest, PS-030 presence group:
//  1. PRESENCE parser fuzzing (protocol.md 8.3): round trip, truncated, oversized,
//     every bit flip, 100 000 garbage buffers, targeted field cases; MP_IsValidMap.
//  2. Sequence numbers: RFC 1982 wraparound, a 70 000-step walk across the wrap.
//  3. In-process host + clients (session instances over loopback):
//     - presence set before the session is sent the moment it starts;
//     - client A's updates reach client B through the host, in order, no duplicates;
//     - the receive paths drop duplicate / older / foreign-epoch / own-slot / empty-slot
//       updates, count invalid ones, and accept across the u16 wrap;
//     - the FIFO overflow flag, MP_PopRemotePresence / MP_ClearPresenceOverflow edge cases;
//     - the host overwrites slot/epoch, applies the 25 ms rule, and counts invalid PRESENCE
//       (bad field, wrong channel) as malformed;
//     - a late joiner gets the cached presence of everyone at once;
//     - a simulated walk (a step every frame, worst case) stays under 2 KB/s per player
//       including estimated ENet + UDP/IPv4 overhead, and at most 20 packets/s;
//     - leaving clears the slot's presence everywhere.

#include <string.h>
#include "mp/mp.h"
#include "mp/mp_internal.h"
#include "mp/mp_protocol.h"
#include "mp/mp_selftest.h"
#include "mp/mp_session_internal.h"
#include "mp/mp_summary.h"
#include "mp/mp_transport.h"
#include "constants/maps.h"

static u32 sFail;

#define P_CHECK(cond, what)                                                         \
    do                                                                              \
    {                                                                               \
        if (!(cond))                                                                \
        {                                                                           \
            MP_LOG("selftest: FAIL: %s (%s:%d)\n", what, __FILE__, __LINE__);       \
            sFail++;                                                                \
        }                                                                           \
    } while (0)

static u32 sRng = 0x9E3779B9u;

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
        MP_LOG("selftest: presence: %s OK\n", what);
}

static void FixturePresence(struct MpPresence *p, u16 seq, s16 x, s16 y, u8 stepId)
{
    memset(p, 0, sizeof(*p));
    p->seq = seq;
    p->mapGroup = MAP_GROUP(MAP_LITTLEROOT_TOWN);
    p->mapNum = MAP_NUM(MAP_LITTLEROOT_TOWN);
    p->x = x;
    p->y = y;
    p->facing = DIR_NORTH;
    p->movement = MP_MOVE_WALK;
    p->avatarState = MP_AVATAR_ON_FOOT;
    p->busy = MP_BUSY_NONE;
    p->stepId = stepId;
}

// Writes a PRESENCE payload (no header) into `payload` (MP_PRESENCE_SIZE bytes).
static void BuildPayload(u8 *payload, u8 slot, u8 epoch, const struct MpPresence *p)
{
    struct MpMsgPresence msg;
    u8 buf[MP_HEADER_SIZE + MP_PRESENCE_SIZE];

    memset(&msg, 0, sizeof(msg));
    msg.slot = slot;
    msg.epoch = epoch;
    msg.presence = *p;
    if (MpProto_WritePresence(buf, sizeof(buf), &msg) != sizeof(buf))
        memset(buf, 0, sizeof(buf));
    memcpy(payload, &buf[MP_HEADER_SIZE], MP_PRESENCE_SIZE);
}

static u8 ParseWithByte(const u8 *payload, u32 offset, u8 value)
{
    u8 copy[MP_PRESENCE_SIZE];
    struct MpMsgPresence out;

    memcpy(copy, payload, sizeof(copy));
    copy[offset] = value;
    return MpProto_ParsePresence(copy, sizeof(copy), &out);
}

static u8 ParseWithS16(const u8 *payload, u32 offset, s16 value)
{
    u8 copy[MP_PRESENCE_SIZE];
    struct MpMsgPresence out;

    memcpy(copy, payload, sizeof(copy));
    copy[offset] = (u8)((u16)value & 0xFF);
    copy[offset + 1] = (u8)((u16)value >> 8);
    return MpProto_ParsePresence(copy, sizeof(copy), &out);
}

static bool8 SamePresence(const struct MpPresence *a, const struct MpPresence *b)
{
    return a->seq == b->seq && a->mapGroup == b->mapGroup && a->mapNum == b->mapNum
        && a->x == b->x && a->y == b->y && a->facing == b->facing && a->movement == b->movement
        && a->avatarState == b->avatarState && a->busy == b->busy && a->stepId == b->stepId;
}

// ===========================================================================
// 1. Parser + map table
// ===========================================================================

static void TestParser(void)
{
    u8 buf[MP_MAX_PACKET_SIZE];
    u8 buf2[MP_MAX_PACKET_SIZE];
    u8 big[600];
    u8 payload[MP_PRESENCE_SIZE];
    struct MpMsgPresence msg, out;
    struct MpMsgHeader hdr;
    u32 failuresBefore = sFail;
    u32 n, i, bit, len;
    u8 r1, r2;
    u16 written;

    memset(&msg, 0, sizeof(msg));
    msg.slot = 2;
    msg.epoch = 7;
    FixturePresence(&msg.presence, 0xBEEF, -3, 1000, 42);
    msg.presence.movement = MP_MOVE_WALK_FASTER;
    msg.presence.avatarState = MP_AVATAR_MACH_BIKE;
    msg.presence.busy = MP_BUSY_SCRIPT;
    written = MpProto_WritePresence(buf, sizeof(buf), &msg);
    P_CHECK(written == MP_HEADER_SIZE + MP_PRESENCE_SIZE, "PRESENCE packet is 20 bytes");
    P_CHECK(MpProto_WritePresence(buf2, (u16)(written - 1), &msg) == 0, "PRESENCE writer refuses a short buffer");
    P_CHECK(MpProto_ParseHeader(buf, written, &hdr) && hdr.type == MP_MSG_PRESENCE
         && hdr.version == MP_PROTOCOL_VERSION && hdr.length == MP_PRESENCE_SIZE, "PRESENCE header");
    // Wire layout spot checks (protocol.md 6.8 offsets, little-endian)
    P_CHECK(buf[4] == 2 && buf[5] == 7 && buf[6] == 0xEF && buf[7] == 0xBE, "PRESENCE slot/epoch/seq bytes");
    P_CHECK(buf[10] == 0xFD && buf[11] == 0xFF && buf[12] == 0xE8 && buf[13] == 0x03, "PRESENCE x/y bytes (s16 LE)");
    P_CHECK(buf[14] == DIR_NORTH && buf[15] == MP_MOVE_WALK_FASTER && buf[16] == MP_AVATAR_MACH_BIKE
         && buf[17] == MP_BUSY_SCRIPT && buf[18] == 42 && buf[19] == 0, "PRESENCE enum bytes");
    memcpy(payload, &buf[MP_HEADER_SIZE], MP_PRESENCE_SIZE);

    // 1. round trip
    P_CHECK(MpProto_ParsePresence(payload, MP_PRESENCE_SIZE, &out) == MP_PARSE_OK, "valid PRESENCE parses");
    P_CHECK(out.slot == 2 && out.epoch == 7 && SamePresence(&out.presence, &msg.presence), "PRESENCE fields round trip");
    P_CHECK(MpProto_WritePresence(buf2, sizeof(buf2), &out) == written && memcmp(buf, buf2, written) == 0, "PRESENCE round trip is byte-identical");

    // 2. truncated
    for (len = 0; len < MP_PRESENCE_SIZE; len++)
        P_CHECK(MpProto_ParsePresence(payload, len, &out) == MP_PARSE_ERR_TRUNCATED, "truncated PRESENCE rejected");
    P_CHECK(MpProto_ParsePresence(NULL, 0, &out) != MP_PARSE_OK, "NULL PRESENCE rejected");
    P_CHECK(MpProto_ParsePresence(NULL, MP_PRESENCE_SIZE, &out) != MP_PARSE_OK, "NULL PRESENCE with a length rejected");

    // 3. oversized
    memset(big, 0, sizeof(big));
    memcpy(big, payload, MP_PRESENCE_SIZE);
    P_CHECK(MpProto_ParsePresence(big, MP_PRESENCE_SIZE + 1, &out) == MP_PARSE_ERR_OVERSIZED, "PRESENCE + 1 byte");
    P_CHECK(MpProto_ParsePresence(big, MP_MAX_PACKET_SIZE, &out) == MP_PARSE_ERR_OVERSIZED, "PRESENCE padded to 512");
    P_CHECK(MpProto_ParsePresence(big, MP_MAX_PACKET_SIZE + 1, &out) == MP_PARSE_ERR_OVERSIZED, "PRESENCE padded to 513");

    // 4. every single-bit flip: no crash, deterministic, and an OK result is in range
    for (i = 0; i < MP_PRESENCE_SIZE; i++)
    {
        for (bit = 0; bit < 8; bit++)
        {
            memcpy(big, payload, MP_PRESENCE_SIZE);
            big[i] ^= (u8)(1 << bit);
            r1 = MpProto_ParsePresence(big, MP_PRESENCE_SIZE, &out);
            if (r1 == MP_PARSE_OK)
            {
                P_CHECK(out.slot < MP_MAX_PLAYERS && out.epoch != 0, "bit flip OK: slot/epoch in range");
                P_CHECK(MP_IsValidMap(out.presence.mapGroup, out.presence.mapNum), "bit flip OK: map valid");
                P_CHECK(out.presence.facing >= DIR_SOUTH && out.presence.facing <= DIR_EAST
                     && out.presence.movement < MP_MOVE_COUNT && out.presence.avatarState < MP_AVATAR_STATE_COUNT
                     && out.presence.busy < MP_BUSY_COUNT, "bit flip OK: enums in range");
            }
            r2 = MpProto_ParsePresence(big, MP_PRESENCE_SIZE, &out);
            P_CHECK(r1 == r2, "PRESENCE bit flip result is deterministic");
        }
    }

    // 5. garbage
    for (n = 0; n < 100000; n++)
    {
        len = Rand() % (sizeof(big) + 1);
        for (i = 0; i < len; i++)
            big[i] = (u8)(Rand() >> 24);
        if (MpProto_ParsePresence(big, len, &out) == MP_PARSE_OK)
            P_CHECK(len == MP_PRESENCE_SIZE && (MP_IsValidMap(out.presence.mapGroup, out.presence.mapNum)
                 || out.presence.mapGroup == MP_MAP_NONE), "garbage that parses is valid");
        MpProto_ParseHeader(big, len, &hdr);
    }
    if (sFail == failuresBefore)
        MP_LOG("selftest: parser PRESENCE OK (round trip, truncated, oversized, %u bit flips, 100000 garbage, seed 0x9E3779B9)\n",
               (unsigned)(MP_PRESENCE_SIZE * 8));

    // 6. targeted cases (payload offsets: 0 slot, 1 epoch, 2 seq, 4 group, 5 num, 6 x, 8 y,
    //    10 facing, 11 movement, 12 avatarState, 13 busy, 14 stepId, 15 flags)
    failuresBefore = sFail;
    P_CHECK(ParseWithByte(payload, 0, MP_MAX_PLAYERS) == MP_PARSE_ERR_BAD_FIELD, "PRESENCE slot 4");
    P_CHECK(ParseWithByte(payload, 1, 0) == MP_PARSE_ERR_BAD_FIELD, "PRESENCE epoch 0");
    P_CHECK(ParseWithByte(payload, 4, MP_MAP_NONE) == MP_PARSE_ERR_BAD_FIELD, "PRESENCE mapGroup 0xFF, mapNum valid");
    P_CHECK(ParseWithByte(payload, 5, MP_MAP_NONE) == MP_PARSE_ERR_BAD_FIELD, "PRESENCE mapNum 0xFF, mapGroup valid");
    P_CHECK(ParseWithByte(payload, 4, MAP_GROUPS_COUNT) == MP_PARSE_ERR_BAD_FIELD, "PRESENCE mapGroup = MAP_GROUPS_COUNT");
    P_CHECK(ParseWithByte(payload, 10, 0) == MP_PARSE_ERR_BAD_FIELD, "PRESENCE facing 0");
    P_CHECK(ParseWithByte(payload, 10, 5) == MP_PARSE_ERR_BAD_FIELD, "PRESENCE facing 5");
    P_CHECK(ParseWithByte(payload, 10, DIR_EAST) == MP_PARSE_OK, "PRESENCE facing 4");
    P_CHECK(ParseWithByte(payload, 11, MP_MOVE_COUNT) == MP_PARSE_ERR_BAD_FIELD, "PRESENCE movement 8");
    P_CHECK(ParseWithByte(payload, 11, MP_MOVE_WARP) == MP_PARSE_OK, "PRESENCE movement 7");
    P_CHECK(ParseWithByte(payload, 12, MP_AVATAR_STATE_COUNT) == MP_PARSE_ERR_BAD_FIELD, "PRESENCE avatarState 5");
    P_CHECK(ParseWithByte(payload, 13, MP_BUSY_COUNT) == MP_PARSE_ERR_BAD_FIELD, "PRESENCE busy 6");
    P_CHECK(ParseWithByte(payload, 14, 0xFF) == MP_PARSE_OK, "PRESENCE any stepId");
    P_CHECK(ParseWithByte(payload, 15, 0x80) == MP_PARSE_OK, "PRESENCE reserved flags ignored");
    P_CHECK(ParseWithS16(payload, 6, -16) == MP_PARSE_OK && ParseWithS16(payload, 6, 1023) == MP_PARSE_OK, "PRESENCE x at the bounds");
    P_CHECK(ParseWithS16(payload, 6, -17) == MP_PARSE_ERR_BAD_FIELD && ParseWithS16(payload, 6, 1024) == MP_PARSE_ERR_BAD_FIELD, "PRESENCE x out of range");
    P_CHECK(ParseWithS16(payload, 8, -16) == MP_PARSE_OK && ParseWithS16(payload, 8, 1023) == MP_PARSE_OK, "PRESENCE y at the bounds");
    P_CHECK(ParseWithS16(payload, 8, -17) == MP_PARSE_ERR_BAD_FIELD && ParseWithS16(payload, 8, 0x7FFF) == MP_PARSE_ERR_BAD_FIELD, "PRESENCE y out of range");
    {
        u8 copy[MP_PRESENCE_SIZE];

        // Map numbers at the end of a group (group 0 has MAP_NUM(MAP_UNDERWATER_ROUTE125) + 1 maps)
        memcpy(copy, payload, sizeof(copy));
        copy[4] = MAP_GROUP(MAP_UNDERWATER_ROUTE125);
        copy[5] = MAP_NUM(MAP_UNDERWATER_ROUTE125);
        P_CHECK(MpProto_ParsePresence(copy, sizeof(copy), &out) == MP_PARSE_OK, "PRESENCE last map of group 0");
        copy[5]++;
        P_CHECK(MpProto_ParsePresence(copy, sizeof(copy), &out) == MP_PARSE_ERR_BAD_FIELD, "PRESENCE map past the end of group 0");

        // Not in the field: both 0xFF, busy must be NOT_IN_FIELD, position fields ignored
        memcpy(copy, payload, sizeof(copy));
        copy[4] = MP_MAP_NONE;
        copy[5] = MP_MAP_NONE;
        copy[6] = 0x00; copy[7] = 0x80; // x = -32768 (ignored)
        copy[10] = 0;                    // facing 0 (ignored)
        copy[11] = 0xEE;                 // movement (ignored)
        copy[13] = MP_BUSY_NOT_IN_FIELD;
        P_CHECK(MpProto_ParsePresence(copy, sizeof(copy), &out) == MP_PARSE_OK, "PRESENCE MP_MAP_NONE + NOT_IN_FIELD");
        P_CHECK(out.presence.x == 0 && out.presence.y == 0 && out.presence.facing == DIR_SOUTH
             && out.presence.movement == MP_MOVE_NONE, "PRESENCE MP_MAP_NONE normalises the ignored fields");
        copy[13] = MP_BUSY_NONE;
        P_CHECK(MpProto_ParsePresence(copy, sizeof(copy), &out) == MP_PARSE_ERR_BAD_FIELD, "PRESENCE MP_MAP_NONE needs NOT_IN_FIELD");
        copy[13] = MP_BUSY_NOT_IN_FIELD;
        copy[12] = MP_AVATAR_STATE_COUNT;
        P_CHECK(MpProto_ParsePresence(copy, sizeof(copy), &out) == MP_PARSE_ERR_BAD_FIELD, "PRESENCE MP_MAP_NONE still checks avatarState");
    }

    // MP_IsValidMap
    P_CHECK(MP_IsValidMap(0, 0), "MP_IsValidMap(0, 0)");
    P_CHECK(MP_IsValidMap(MAP_GROUP(MAP_LITTLEROOT_TOWN), MAP_NUM(MAP_LITTLEROOT_TOWN)), "MP_IsValidMap(Littleroot)");
    P_CHECK(MP_IsValidMap(MAP_GROUP(MAP_UNION_ROOM), MAP_NUM(MAP_UNION_ROOM)), "MP_IsValidMap(last map of IndoorDynamic)");
    P_CHECK(!MP_IsValidMap(MAP_GROUP(MAP_UNION_ROOM), MAP_NUM(MAP_UNION_ROOM) + 1), "MP_IsValidMap(past IndoorDynamic)");
    P_CHECK(MP_IsValidMap(MAP_GROUPS_COUNT - 1, 0), "MP_IsValidMap(last group, 0)");
    P_CHECK(!MP_IsValidMap(MAP_GROUPS_COUNT, 0), "MP_IsValidMap(MAP_GROUPS_COUNT, 0)");
    P_CHECK(!MP_IsValidMap(MP_MAP_NONE, MP_MAP_NONE), "MP_IsValidMap(0xFF, 0xFF)");
    P_CHECK(!MP_IsValidMap(0, 0xFF), "MP_IsValidMap(0, 0xFF)");
    P_CHECK(!MP_IsValidMap(0x7F, 0x7F), "MP_IsValidMap(MAP_DYNAMIC)");
    Report("PRESENCE targeted cases + MP_IsValidMap", failuresBefore);
}

// ===========================================================================
// 2. Sequence numbers
// ===========================================================================

static void TestSeq(void)
{
    u32 failuresBefore = sFail;
    u32 i;
    u16 last, s;
    bool8 allNewer = TRUE;

    P_CHECK(MpSeqIsNewer(0, 65535), "seq 0 is newer than 65535 (wrap)");
    P_CHECK(MpSeqIsNewer(1, 65535), "seq 1 is newer than 65535");
    P_CHECK(!MpSeqIsNewer(65535, 0), "seq 65535 is older than 0");
    P_CHECK(MpSeqIsNewer(65535, 65534), "seq 65535 > 65534");
    P_CHECK(!MpSeqIsNewer(65535, 65535), "seq duplicate at the top");
    P_CHECK(MpSeqIsNewer(0x7FFF, 0xFFFF) == FALSE, "seq +0x8000 across the wrap is not newer");
    P_CHECK(MpSeqIsNewer(0x7FFE, 0xFFFF), "seq +0x7FFF across the wrap is newer");
    P_CHECK(!MpSeqIsNewer(0x8000, 0) && !MpSeqIsNewer(0, 0x8000), "seq d = 0x8000 is not newer either way");
    P_CHECK(MpSeqIsNewer(10, 0xFFF0) && !MpSeqIsNewer(0xFFF0, 10), "seq 10 vs 0xFFF0");

    // 70 000 consecutive sends starting near the top: every one is newer than the last,
    // and never newer than the one after it.
    last = 65000;
    for (i = 0; i < 70000; i++)
    {
        s = (u16)(last + 1);
        if (!MpSeqIsNewer(s, last) || MpSeqIsNewer(last, s))
            allNewer = FALSE;
        last = s;
    }
    P_CHECK(allNewer, "seq walk of 70000 across the wrap");
    Report("seq wraparound (RFC 1982)", failuresBefore);
}

// ===========================================================================
// 3. Session scenario
// ===========================================================================

#define I_HOST  0
#define I_A     1
#define I_B     2
#define I_C     3
#define INSTANCES 4

#define PRESENCE_PORT_FIRST 27570
#define PRESENCE_PORT_LAST  27579

static u16 sPort;
static u8 sWaitInstance;
static u8 sWaitSlot;

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
            ;
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

static bool8 CondInSession(void)
{
    Sel(sWaitInstance);
    return MP_GetState() == MP_STATE_IN_SESSION;
}

static bool8 CondHasPresence(void)
{
    const struct MpPlayer *p;

    Sel(sWaitInstance);
    p = MP_GetPlayer(sWaitSlot);
    return p != NULL && p->hasPresence;
}

static bool8 CondSlotEmpty(void)
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

static bool8 WaitInSession(u8 instance, u32 timeoutMs)
{
    sWaitInstance = instance;
    return PumpUntil(CondInSession, timeoutMs);
}

static bool8 WaitPresence(u8 instance, u8 slot, u32 timeoutMs)
{
    sWaitInstance = instance;
    sWaitSlot = slot;
    return PumpUntil(CondHasPresence, timeoutMs);
}

static bool8 JoinAs(u8 instance, const char *name)
{
    Sel(instance);
    if (!MP_SetNameOverride(name))
        return FALSE;
    return MP_Join("127.0.0.1", sPort);
}

static void SetLocal(u8 instance, const struct MpPresence *p)
{
    Sel(instance);
    MP_SetLocalPresence(p);
}

static const struct MpPresence *PresenceOf(u8 instance, u8 slot)
{
    const struct MpPlayer *p;

    Sel(instance);
    p = MP_GetPlayer(slot);
    return (p != NULL && p->hasPresence) ? &p->presence : NULL;
}

// Pops everything queued for `slot` on `instance`; returns the count.
static u32 Drain(u8 instance, u8 slot)
{
    struct MpPresence p;
    u32 n = 0;

    Sel(instance);
    while (MP_PopRemotePresence(slot, &p) && n < 1000)
        n++;
    return n;
}

static u8 InjectClient(u8 slot, u8 epoch, const struct MpPresence *p)
{
    u8 payload[MP_PRESENCE_SIZE];

    BuildPayload(payload, slot, epoch, p);
    return MpPresence_OnClientReceive(payload, sizeof(payload));
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

static void TestSession(void)
{
    struct MpPresence hostP, aP, steps[4], p, cur;
    const struct MpPresence *got;
    u8 slotA, slotB, slotC, epochA, peerA;
    u32 failuresBefore;
    u32 i, n, start, elapsed, prevSeqValid;
    u16 prevSeq, lastSeq;
    u8 prevStep, lastStepSeen;
    bool8 ordered, hosting = FALSE;

    for (i = 0; i < INSTANCES; i++)
        MpSession_ResetInstance((u8)i);

    // --- presence set before the session starts is sent at once ---
    failuresBefore = sFail;
    FixturePresence(&hostP, 0, 5, 6, 1);
    FixturePresence(&aP, 0, 10, 10, 1);
    aP.mapNum = MAP_NUM(MAP_OLDALE_TOWN);
    aP.mapGroup = MAP_GROUP(MAP_OLDALE_TOWN);
    SetLocal(I_HOST, &hostP);
    SetLocal(I_A, &aP);
    // B never sets one: it must announce MP_MAP_NONE / NOT_IN_FIELD.

    Sel(I_HOST);
    MP_SetNameOverride("Host");
    for (sPort = PRESENCE_PORT_FIRST; sPort <= PRESENCE_PORT_LAST && !hosting; sPort++)
        hosting = MP_Host(sPort);
    sPort--;
    P_CHECK(hosting, "presence host on a port in 27570..27579");
    if (!hosting)
        return;
    P_CHECK(JoinAs(I_A, "PresA"), "client A MP_Join");
    P_CHECK(WaitInSession(I_A, 5000), "client A in session");
    P_CHECK(JoinAs(I_B, "PresB"), "client B MP_Join");
    P_CHECK(WaitInSession(I_B, 5000), "client B in session");
    Sel(I_A); slotA = MP_GetLocalSlot();
    Sel(I_B); slotB = MP_GetLocalSlot();
    P_CHECK(slotA == 1 && slotB == 2, "A is slot 1, B is slot 2");
    if (slotA >= MP_MAX_PLAYERS || slotB >= MP_MAX_PLAYERS)
    {
        CleanUp();
        return;
    }
    P_CHECK(WaitPresence(I_B, slotA, 1500), "B has A's presence");
    P_CHECK(WaitPresence(I_B, MP_HOST_SLOT, 1500), "B has the host's presence");
    P_CHECK(WaitPresence(I_A, MP_HOST_SLOT, 1500), "A has the host's presence");
    P_CHECK(WaitPresence(I_HOST, slotA, 1500), "host has A's presence");
    P_CHECK(WaitPresence(I_HOST, slotB, 1500), "host has B's presence");
    P_CHECK(WaitPresence(I_A, slotB, 1500), "A has B's presence");
    got = PresenceOf(I_B, slotA);
    P_CHECK(got != NULL && got->mapGroup == aP.mapGroup && got->mapNum == aP.mapNum && got->x == 10 && got->y == 10
         && got->facing == DIR_NORTH && got->movement == MP_MOVE_WALK, "B sees A's fields");
    got = PresenceOf(I_A, MP_HOST_SLOT);
    P_CHECK(got != NULL && got->x == 5 && got->y == 6, "A sees the host's fields");
    got = PresenceOf(I_HOST, slotB);
    P_CHECK(got != NULL && got->mapGroup == MP_MAP_NONE && got->busy == MP_BUSY_NOT_IN_FIELD, "B announced NOT_IN_FIELD");
    Sel(I_A);
    P_CHECK(MP_GetPlayer(slotA) != NULL && MP_GetPlayer(slotA)->hasPresence && MP_GetPlayer(slotA)->presence.x == 10, "A's own entry shows its local presence");
    Report("presence set before the session is sent when it starts", failuresBefore);

    // --- A's updates reach B through the host, in order ---
    failuresBefore = sFail;
    Drain(I_B, slotA);
    for (i = 0; i < 4; i++)
    {
        FixturePresence(&steps[i], 0, (s16)(11 + i), 10, (u8)(10 + i));
        steps[i].mapGroup = aP.mapGroup;
        steps[i].mapNum = aP.mapNum;
        steps[i].facing = DIR_EAST;
        steps[i].movement = MP_MOVE_WALK_FAST;
        SetLocal(I_A, &steps[i]);
        PumpFor(80); // > 50 ms: one send per step
    }
    PumpFor(250);
    Sel(I_B);
    memset(&cur, 0, sizeof(cur));
    n = 0;
    ordered = TRUE;
    prevSeqValid = FALSE;
    prevSeq = 0;
    prevStep = 0;
    lastStepSeen = 0;
    while (MP_PopRemotePresence(slotA, &p) && n < 100)
    {
        if (prevSeqValid && (!MpSeqIsNewer(p.seq, prevSeq) || (u8)(p.stepId - prevStep) >= 0x80))
            ordered = FALSE;
        prevSeqValid = TRUE;
        prevSeq = p.seq;
        prevStep = p.stepId;
        lastStepSeen = p.stepId;
        cur = p;
        n++;
    }
    P_CHECK(n >= 1, "B received A's steps");
    P_CHECK(ordered, "B's queue for A is in order (seq strictly newer, stepId never goes back)");
    P_CHECK(n >= 1 && lastStepSeen == 13 && cur.x == 14 && cur.facing == DIR_EAST && cur.movement == MP_MOVE_WALK_FAST, "B's last queued update is A's last step");
    got = PresenceOf(I_B, slotA);
    P_CHECK(got != NULL && got->stepId == 13 && got->x == 14, "B's latest presence of A is the last step");
    MP_LOG("selftest: presence: B queued %u updates for A's 4 steps (unreliable channel; loss allowed)\n", (unsigned)n);
    Report("A's steps reach B via the host in order", failuresBefore);

    // --- receive path: stale / duplicate / foreign epoch / own slot / empty slot / invalid ---
    failuresBefore = sFail;
    Sel(I_B);
    epochA = MP_GetPlayer(slotA)->epoch;
    lastSeq = gMpSession->presence.slots[slotA].lastSeq;
    P_CHECK(lastSeq < 0x1000, "A has sent fewer than 4096 PRESENCE so far");
    Drain(I_B, slotA);
    p = steps[3];
    p.seq = lastSeq;
    P_CHECK(InjectClient(slotA, epochA, &p) == MP_PRESENCE_STALE, "duplicate seq is stale");
    p.seq = (u16)(lastSeq - 1);
    P_CHECK(InjectClient(slotA, epochA, &p) == MP_PRESENCE_STALE, "older seq is stale");
    p.seq = (u16)(lastSeq + 0x8000);
    P_CHECK(InjectClient(slotA, epochA, &p) == MP_PRESENCE_STALE, "seq + 0x8000 is not newer");
    p.seq = (u16)(lastSeq + 1);
    p.x = 77;
    P_CHECK(InjectClient(slotA, (u8)((epochA == 0xFF) ? 1 : epochA + 1), &p) == MP_PRESENCE_IGNORED, "foreign epoch ignored");
    P_CHECK(InjectClient(slotB, MP_GetPlayer(slotB)->epoch, &p) == MP_PRESENCE_IGNORED, "own slot ignored");
    P_CHECK(InjectClient(3, 1, &p) == MP_PRESENCE_IGNORED, "empty slot ignored");
    P_CHECK(Drain(I_B, slotA) == 0, "nothing queued by dropped updates");
    Sel(I_B);
    p.facing = 9;
    P_CHECK(InjectClient(slotA, epochA, &p) == MP_PRESENCE_INVALID, "bad facing is invalid");
    p.facing = DIR_EAST;
    P_CHECK(MpPresence_OnClientReceive(NULL, MP_PRESENCE_SIZE) == MP_PRESENCE_INVALID, "NULL payload is invalid");
    P_CHECK(InjectClient(slotA, epochA, &p) == MP_PRESENCE_ACCEPTED, "newer seq accepted");
    got = PresenceOf(I_B, slotA);
    P_CHECK(got != NULL && got->x == 77 && got->seq == (u16)(lastSeq + 1), "accepted update is the latest presence");
    P_CHECK(Drain(I_B, slotA) == 1, "exactly the accepted update is queued");

    // Across the u16 wrap inside the session
    Sel(I_B);
    p.seq = (u16)(lastSeq + 0x7000);
    P_CHECK(InjectClient(slotA, epochA, &p) == MP_PRESENCE_ACCEPTED, "wrap walk 1");
    p.seq = (u16)(p.seq + 0x7000);
    P_CHECK(InjectClient(slotA, epochA, &p) == MP_PRESENCE_ACCEPTED, "wrap walk 2");
    p.seq = 0xFFFF; // lastSeq is small (< 0x1000), so 0xFFFF is less than 0x8000 ahead of lastSeq + 0xE000
    P_CHECK(InjectClient(slotA, epochA, &p) == MP_PRESENCE_ACCEPTED, "wrap walk to 65535");
    p.seq = 0xFFFF;
    P_CHECK(InjectClient(slotA, epochA, &p) == MP_PRESENCE_STALE, "65535 duplicate is stale");
    p.seq = 0;
    P_CHECK(InjectClient(slotA, epochA, &p) == MP_PRESENCE_ACCEPTED, "0 after 65535 is accepted");
    p.seq = 0xFFFF;
    P_CHECK(InjectClient(slotA, epochA, &p) == MP_PRESENCE_STALE, "65535 after 0 is stale");
    Drain(I_B, slotA);
    // A's real seq is small and therefore newer than 0 again: live traffic recovers.
    PumpFor(1200);
    got = PresenceOf(I_B, slotA);
    P_CHECK(got != NULL && got->x == 14 && got->stepId == 13, "live presence from A resumes after the wrap test");
    Report("receive path drops duplicate / older / foreign / own / empty, accepts across the wrap", failuresBefore);

    // --- FIFO overflow and the consumer API ---
    failuresBefore = sFail;
    Sel(I_B);
    Drain(I_B, slotA);
    Sel(I_B);
    lastSeq = gMpSession->presence.slots[slotA].lastSeq;
    for (i = 0; i < MP_PRESENCE_QUEUE_SIZE + 2; i++)
    {
        p = steps[0];
        p.seq = (u16)(lastSeq + 1 + i);
        p.stepId = (u8)(100 + i);
        InjectClient(slotA, epochA, &p);
    }
    P_CHECK(MP_GetPlayer(slotA)->presenceOverflow != 0, "overflow flag set after 10 updates");
    P_CHECK(MP_PopRemotePresence(slotA, &p) && p.stepId == 102, "overflow dropped the 2 oldest");
    P_CHECK(Drain(I_B, slotA) == MP_PRESENCE_QUEUE_SIZE - 1, "queue holds 8");
    Sel(I_B);
    MP_ClearPresenceOverflow(slotA);
    P_CHECK(MP_GetPlayer(slotA)->presenceOverflow == 0, "MP_ClearPresenceOverflow");
    P_CHECK(!MP_PopRemotePresence(slotB, &p), "pop of own slot is FALSE");
    P_CHECK(!MP_PopRemotePresence(3, &p), "pop of an empty slot is FALSE");
    P_CHECK(!MP_PopRemotePresence(200, &p), "pop of slot 200 is FALSE");
    P_CHECK(!MP_PopRemotePresence(slotA, NULL), "pop into NULL is FALSE");
    MP_ClearPresenceOverflow(200); // must not crash
    // Local sanitising
    p = steps[0];
    p.mapGroup = 200;
    p.facing = 9;
    MP_SetLocalPresence(&p);
    got = PresenceOf(I_B, slotB);
    P_CHECK(got != NULL && got->mapGroup == MP_MAP_NONE && got->mapNum == MP_MAP_NONE && got->busy == MP_BUSY_NOT_IN_FIELD, "invalid local map is sent as MP_MAP_NONE");
    p = steps[0];
    p.x = 5000;
    p.y = -5000;
    p.facing = 0;
    p.movement = 99;
    MP_SetLocalPresence(&p);
    got = PresenceOf(I_B, slotB);
    P_CHECK(got != NULL && got->x == MP_PRESENCE_COORD_MAX && got->y == MP_PRESENCE_COORD_MIN
         && got->facing == DIR_SOUTH && got->movement == MP_MOVE_NONE, "local coords clamped, facing and movement fixed");
    PumpFor(300);
    P_CHECK(WaitPresence(I_A, slotB, 500), "A has B's presence");
    got = PresenceOf(I_A, slotB);
    P_CHECK(got != NULL && got->x == MP_PRESENCE_COORD_MAX, "the sanitised presence passes the receiver's checks");
    Drain(I_B, slotA);
    Report("FIFO overflow, consumer API, local sanitising", failuresBefore);

    // --- host path: slot/epoch overwrite, 25 ms rule, invalid PRESENCE is malformed ---
    failuresBefore = sFail;
    Sel(I_HOST);
    peerA = gMpSession->slotPeer[slotA];
    P_CHECK(peerA < MP_HOST_PEER_COUNT, "host knows A's peer");
    if (peerA < MP_HOST_PEER_COUNT)
    {
        u8 payload[MP_PRESENCE_SIZE];
        u8 packet[MP_HEADER_SIZE + MP_PRESENCE_SIZE];
        u32 now = MpTransport_GetTimeMs();
        u8 malformedBefore;

        lastSeq = gMpSession->presence.slots[slotA].lastSeq;
        p = steps[3];
        p.seq = lastSeq;
        BuildPayload(payload, slotA, epochA, &p);
        P_CHECK(MpPresence_OnHostReceive(peerA, payload, sizeof(payload), now + 1000) == MP_PRESENCE_STALE, "host: duplicate seq is stale");
        p.seq = (u16)(lastSeq + 1);
        BuildPayload(payload, 3, 99, &p); // lies about slot and epoch
        P_CHECK(MpPresence_OnHostReceive(peerA, payload, sizeof(payload), now + 1000) == MP_PRESENCE_ACCEPTED, "host: slot/epoch overwritten, accepted");
        P_CHECK(MP_GetPlayer(slotA)->presence.seq == (u16)(lastSeq + 1) && MP_GetPlayer(3) == NULL, "host stored it for A's real slot");
        p.seq = (u16)(lastSeq + 2);
        BuildPayload(payload, slotA, epochA, &p);
        P_CHECK(MpPresence_OnHostReceive(peerA, payload, sizeof(payload), now + 1010) == MP_PRESENCE_IGNORED, "host: < 25 ms after the last one is dropped");
        P_CHECK(MpPresence_OnHostReceive(peerA, payload, sizeof(payload) - 1, now + 2000) == MP_PRESENCE_INVALID, "host: 15-byte PRESENCE is invalid");
        P_CHECK(MpPresence_OnHostReceive(MP_HOST_PEER_COUNT, payload, sizeof(payload), now + 2000) == MP_PRESENCE_IGNORED, "host: bad peer id");

        // Over the wire: an invalid PRESENCE and one on the wrong channel count as malformed.
        malformedBefore = gMpSession->peers[peerA].malformed;
        Sel(I_A);
        p.facing = 0;
        BuildPayload(payload, slotA, epochA, &p);
        packet[0] = MP_MSG_PRESENCE;
        packet[1] = MP_PROTOCOL_VERSION;
        packet[2] = MP_PRESENCE_SIZE;
        packet[3] = 0;
        memcpy(&packet[MP_HEADER_SIZE], payload, sizeof(payload));
        MpTransport_Send(0, MP_CHANNEL_PRESENCE, packet, sizeof(packet), TRUE);
        p.facing = DIR_EAST;
        p.seq = (u16)(lastSeq + 100);
        BuildPayload(payload, slotA, epochA, &p);
        memcpy(&packet[MP_HEADER_SIZE], payload, sizeof(payload));
        MpTransport_Send(0, MP_CHANNEL_CONTROL, packet, sizeof(packet), TRUE);
        PumpFor(300);
        Sel(I_HOST);
        P_CHECK(gMpSession->peers[peerA].malformed == malformedBefore + 2, "host: invalid PRESENCE and wrong channel count as malformed");
        P_CHECK(MP_GetPlayer(slotA) != NULL && MP_GetPlayer(slotA)->presence.seq != (u16)(lastSeq + 100), "host: the wrong-channel PRESENCE was not applied");
    }
    PumpFor(1100); // let keep-alives settle every table again
    Report("host overwrites slot/epoch, 25 ms rule, malformed PRESENCE", failuresBefore);

    // --- a late joiner gets the cached presence of everyone at once ---
    failuresBefore = sFail;
    P_CHECK(JoinAs(I_C, "PresC"), "client C MP_Join");
    P_CHECK(WaitInSession(I_C, 5000), "client C in session");
    Sel(I_C);
    slotC = MP_GetLocalSlot();
    P_CHECK(slotC == 3, "C is slot 3");
    start = MpTransport_GetTimeMs();
    P_CHECK(WaitPresence(I_C, MP_HOST_SLOT, 300) && WaitPresence(I_C, slotA, 300) && WaitPresence(I_C, slotB, 300), "C has everyone's presence right after joining");
    elapsed = MpTransport_GetTimeMs() - start;
    got = PresenceOf(I_C, slotA);
    P_CHECK(got != NULL && got->x == 14 && got->stepId == 13, "C sees A's current position");
    MP_LOG("selftest: presence: late joiner had all presence %u ms after WELCOME\n", (unsigned)elapsed);
    P_CHECK(WaitPresence(I_A, slotC, 1500) && WaitPresence(I_B, slotC, 1500), "A and B get C's presence");
    Report("late joiner gets the cached presence", failuresBefore);

    // --- bandwidth: a step every frame (worst case) for 3 s ---
    failuresBefore = sFail;
    {
        u32 lastStepMs, bytesOut, packetsOut, bytesInB, packetsInB, wireOut, pps;
        u8 stepId = 0;
        s16 x = 20;

        Sel(I_A);
        memset(gMpSession->presence.traffic, 0, sizeof(gMpSession->presence.traffic));
        gMpSession->presence.trafficStartMs = MpTransport_GetTimeMs();
        Sel(I_B);
        memset(gMpSession->presence.traffic, 0, sizeof(gMpSession->presence.traffic));
        gMpSession->presence.trafficStartMs = MpTransport_GetTimeMs();
        start = MpTransport_GetTimeMs();
        lastStepMs = start;
        while ((elapsed = MpTransport_GetTimeMs() - start) < 3000)
        {
            if ((u32)(MpTransport_GetTimeMs() - lastStepMs) >= 16)
            {
                lastStepMs = MpTransport_GetTimeMs();
                FixturePresence(&p, 0, x, 10, ++stepId);
                p.mapGroup = aP.mapGroup;
                p.mapNum = aP.mapNum;
                p.facing = (stepId & 0x20) ? DIR_WEST : DIR_EAST;
                x = (s16)(x + ((stepId & 0x20) ? -1 : 1));
                p.movement = MP_MOVE_WALK_FASTER;
                p.avatarState = MP_AVATAR_MACH_BIKE;
                SetLocal(I_A, &p);
            }
            PumpOnce();
        }
        Sel(I_A);
        bytesOut = gMpSession->presence.traffic[0].bytesOut;
        packetsOut = gMpSession->presence.traffic[0].packetsOut;
        Sel(I_B);
        bytesInB = gMpSession->presence.traffic[0].bytesIn;
        packetsInB = gMpSession->presence.traffic[0].packetsIn;
        PumpFor(1200); // a keep-alive repairs a last update lost or rate-limited on the way
        wireOut = (bytesOut + packetsOut * MP_PRESENCE_WIRE_OVERHEAD) * 1000 / elapsed;
        pps = packetsOut * 1000 / elapsed;
        MP_LOG("selftest: presence: simulated walk %u ms, %u steps: A sent %u PRESENCE (%u pkt/s), %u B/s message, ~%u B/s on the wire; B received %u (%u B/s, all players)\n",
               (unsigned)elapsed, (unsigned)stepId, (unsigned)packetsOut, (unsigned)pps,
               (unsigned)(bytesOut * 1000 / elapsed), (unsigned)wireOut, (unsigned)packetsInB,
               (unsigned)(bytesInB * 1000 / elapsed));
        P_CHECK(packetsOut > 0, "A sent presence while walking");
        P_CHECK(pps <= 21, "at most 20 PRESENCE per second");
        P_CHECK(wireOut < 2048, "walking costs < 2 KB/s per player (incl. ENet + UDP/IPv4 overhead)");
        got = PresenceOf(I_B, slotA);
        P_CHECK(got != NULL && got->stepId == stepId, "B ends on A's last step after the walk");
    }
    Report("simulated walk stays under 2 KB/s and 20 Hz", failuresBefore);

    // --- leaving clears the slot's presence everywhere ---
    failuresBefore = sFail;
    Sel(I_B);
    MP_Leave();
    sWaitInstance = I_A;
    sWaitSlot = slotB;
    P_CHECK(PumpUntil(CondSlotEmpty, 3000), "A sees B leave");
    Sel(I_A);
    P_CHECK(!MP_PopRemotePresence(slotB, &p) && gMpSession->presence.slots[slotB].count == 0
         && !gMpSession->presence.slots[slotB].seqValid, "B's presence queue cleared on A");
    Sel(I_HOST);
    P_CHECK(MP_GetPlayer(slotB) == NULL && gMpSession->presence.slots[slotB].count == 0, "B's presence cleared on the host");
    Report("leaving clears the slot's presence", failuresBefore);

    CleanUp();
}

u32 MpSelftest_Presence(void)
{
    sFail = 0;
    TestParser();
    TestSeq();
    TestSession();
    return sFail;
}
