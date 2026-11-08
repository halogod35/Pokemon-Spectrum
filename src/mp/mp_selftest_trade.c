// Pokemon Spectrum multiplayer selftest: PS-044 host-arbitrated trade commit.
//
// 1. TRADE_COMMIT parser / writer: round trip, sizes, every field rule, random fuzz.
// 2. The host's decision logic as pure functions: both ready -> COMMIT to both; a member's
//    group ends / it times out / the rounds disagree -> ABORT, never COMMIT to only one side;
//    decisions are final and repeated; plus a random-event fuzz of that invariant.
// 3. In process, over the real session and loopback ENet, with link groups (PS-046):
//    host + 2 clients. Client-client and host-client trades commit on both sides; a member
//    that leaves the link or the session before both are ready leaves NEITHER side committed;
//    a member whose session ends while it waits presumes ABORT (in doubt).
// The trade screens themselves need two real windows (card PS-044, manual test).

#include <string.h>
#include "mp/mp.h"
#include "mp/mp_internal.h"
#include "mp/mp_link.h"
#include "mp/mp_protocol.h"
#include "mp/mp_selftest.h"
#include "mp/mp_session_internal.h"
#include "mp/mp_trade_commit.h"
#include "mp/mp_transport.h"

static u32 sFail;

#define T_CHECK(cond, what)                                                          \
    do                                                                               \
    {                                                                                \
        if (!(cond))                                                                 \
        {                                                                            \
            MP_LOG("selftest: FAIL: trade: %s (%s:%d)\n", what, __FILE__, __LINE__); \
            sFail++;                                                                 \
        }                                                                            \
    } while (0)

static void Report(const char *what, u32 failuresBefore)
{
    if (sFail == failuresBefore)
        MP_LOG("selftest: trade: %s OK\n", what);
    else
        MP_LOG("selftest: trade: %s FAILED (%u)\n", what, (unsigned)(sFail - failuresBefore));
}

static u32 sRng = 0x54524144u; // "TRAD"

static u32 Rand(void)
{
    sRng ^= sRng << 13;
    sRng ^= sRng >> 17;
    sRng ^= sRng << 5;
    return sRng;
}

// ===========================================================================
// 1. Parser
// ===========================================================================

static u8 ParseBytes(u8 b0, u8 b1, u8 b2, u8 b3)
{
    struct MpMsgTradeCommit m;
    u8 p[4];

    p[0] = b0;
    p[1] = b1;
    p[2] = b2;
    p[3] = b3;
    return MpProto_ParseTradeCommit(p, sizeof(p), &m);
}

static void TestParser(void)
{
    u32 failuresBefore = sFail;
    struct MpMsgTradeCommit in, out;
    struct MpMsgHeader hdr;
    u8 buf[MP_MAX_PACKET_SIZE];
    u8 raw[16];
    u16 len;
    u32 i, j, ok = 0;
    u8 op, reason;

    // Round trip of every valid op / reason.
    for (op = MP_TRADE_OP_READY; op <= MP_TRADE_OP_ABORT; op++)
    {
        for (reason = 0; reason <= MP_TRADE_ABORT_LAST; reason++)
        {
            if ((op == MP_TRADE_OP_ABORT) != (reason != 0))
                continue;
            in.op = op;
            in.groupId = 7;
            in.seq = 200;
            in.reason = reason;
            len = MpProto_WriteTradeCommit(buf, sizeof(buf), &in);
            T_CHECK(len == 4 + MP_TRADE_COMMIT_SIZE, "TRADE_COMMIT writes 8 bytes");
            T_CHECK(MpProto_ParseHeader(buf, len, &hdr) && hdr.type == MP_MSG_TRADE_COMMIT
                    && hdr.version == MP_PROTOCOL_VERSION && hdr.length == MP_TRADE_COMMIT_SIZE, "TRADE_COMMIT header");
            T_CHECK(MpProto_ParseTradeCommit(buf + 4, len - 4, &out) == MP_PARSE_OK
                    && out.op == in.op && out.groupId == 7 && out.seq == 200 && out.reason == reason, "TRADE_COMMIT round trip");
        }
    }
    T_CHECK(MP_MSG_TRADE_COMMIT >= MP_MSG_LINK_FIRST && MP_MSG_TRADE_COMMIT <= MP_MSG_LINK_LAST, "TRADE_COMMIT is in the LINK_* range");
    T_CHECK(MP_PROTOCOL_VERSION == 6, "protocol version 6");

    // Writer refuses invalid ops.
    in.op = 0;
    T_CHECK(MpProto_WriteTradeCommit(buf, sizeof(buf), &in) == 0, "writer refuses op 0");
    in.op = 4;
    T_CHECK(MpProto_WriteTradeCommit(buf, sizeof(buf), &in) == 0, "writer refuses op 4");
    in.op = MP_TRADE_OP_READY;
    T_CHECK(MpProto_WriteTradeCommit(buf, 6, &in) == 0, "writer refuses a too small buffer");

    // Sizes.
    memset(raw, 0, sizeof(raw));
    raw[0] = MP_TRADE_OP_READY;
    raw[1] = 1;
    raw[2] = 1;
    for (i = 0; i < MP_TRADE_COMMIT_SIZE; i++)
        T_CHECK(MpProto_ParseTradeCommit(raw, i, &out) == MP_PARSE_ERR_TRUNCATED, "short TRADE_COMMIT is TRUNCATED");
    T_CHECK(MpProto_ParseTradeCommit(raw, MP_TRADE_COMMIT_SIZE, &out) == MP_PARSE_OK, "4-byte READY parses");
    for (i = MP_TRADE_COMMIT_SIZE + 1; i < sizeof(raw); i++)
        T_CHECK(MpProto_ParseTradeCommit(raw, i, &out) == MP_PARSE_ERR_OVERSIZED, "long TRADE_COMMIT is OVERSIZED");
    T_CHECK(MpProto_ParseTradeCommit(NULL, 4, &out) != MP_PARSE_OK, "NULL payload refused");

    // Field rules.
    T_CHECK(ParseBytes(0, 1, 1, 0) == MP_PARSE_ERR_BAD_FIELD, "op 0 refused");
    T_CHECK(ParseBytes(4, 1, 1, 0) == MP_PARSE_ERR_BAD_FIELD, "op 4 refused");
    T_CHECK(ParseBytes(0xFF, 1, 1, 0) == MP_PARSE_ERR_BAD_FIELD, "op 0xFF refused");
    T_CHECK(ParseBytes(MP_TRADE_OP_READY, 0, 1, 0) == MP_PARSE_ERR_BAD_FIELD, "group 0 refused");
    T_CHECK(ParseBytes(MP_TRADE_OP_READY, 1, 0, 0) == MP_PARSE_ERR_BAD_FIELD, "seq 0 refused");
    T_CHECK(ParseBytes(MP_TRADE_OP_READY, 1, 1, 1) == MP_PARSE_ERR_BAD_FIELD, "READY with a reason refused");
    T_CHECK(ParseBytes(MP_TRADE_OP_COMMIT, 1, 1, 2) == MP_PARSE_ERR_BAD_FIELD, "COMMIT with a reason refused");
    T_CHECK(ParseBytes(MP_TRADE_OP_ABORT, 1, 1, 0) == MP_PARSE_ERR_BAD_FIELD, "ABORT without a reason refused");
    T_CHECK(ParseBytes(MP_TRADE_OP_ABORT, 1, 1, MP_TRADE_ABORT_LAST + 1) == MP_PARSE_ERR_BAD_FIELD, "ABORT with an unknown reason refused");
    T_CHECK(ParseBytes(MP_TRADE_OP_ABORT, 1, 1, MP_TRADE_ABORT_IN_DOUBT) == MP_PARSE_ERR_BAD_FIELD, "local-only reason refused on the wire");
    T_CHECK(ParseBytes(MP_TRADE_OP_ABORT, 0xFF, 0xFF, MP_TRADE_ABORT_TIMEOUT) == MP_PARSE_OK, "ABORT 255/255 TIMEOUT parses");

    // Wrong direction (checked before any session state is touched).
    T_CHECK(!MpTradeCommit_OnHostReceive(1, (const u8 *)"\x02\x01\x01\x00", 4), "host refuses COMMIT from a client");
    T_CHECK(!MpTradeCommit_OnHostReceive(1, (const u8 *)"\x03\x01\x01\x01", 4), "host refuses ABORT from a client");
    T_CHECK(!MpTradeCommit_OnHostReceive(MP_MAX_PLAYERS, (const u8 *)"\x01\x01\x01\x00", 4), "host refuses a bad slot");
    T_CHECK(!MpTradeCommit_OnClientReceive((const u8 *)"\x01\x01\x01\x00", 4), "client refuses READY from the host");
    T_CHECK(!MpTradeCommit_OnClientReceive((const u8 *)"\x02\x01", 2), "client refuses a short message");
    Report("TRADE_COMMIT parser: round trip, sizes, fields, direction", failuresBefore);

    // Fuzz: random lengths and bytes. Whatever parses obeys the rules and round-trips.
    failuresBefore = sFail;
    for (i = 0; i < 40000; i++)
    {
        u32 n = Rand() % 9;

        for (j = 0; j < n; j++)
            raw[j] = (u8)((Rand() & 3) == 0 ? (Rand() % 5) : Rand());
        if (MpProto_ParseTradeCommit(raw, n, &out) != MP_PARSE_OK)
        {
            T_CHECK(out.op == 0 && out.groupId == 0 && out.seq == 0 && out.reason == 0, "failed parse leaves the output zeroed");
            continue;
        }
        ok++;
        T_CHECK(n == MP_TRADE_COMMIT_SIZE, "only 4-byte messages parse");
        T_CHECK(out.op >= MP_TRADE_OP_READY && out.op <= MP_TRADE_OP_ABORT && out.groupId != 0 && out.seq != 0, "parsed fields in range");
        T_CHECK((out.op == MP_TRADE_OP_ABORT) ? (out.reason >= MP_TRADE_ABORT_FIRST && out.reason <= MP_TRADE_ABORT_LAST) : (out.reason == 0),
                "parsed reason matches the op");
        len = MpProto_WriteTradeCommit(buf, sizeof(buf), &out);
        T_CHECK(len == 8 && memcmp(buf + 4, raw, 4) == 0, "parsed message writes back the same bytes");
    }
    T_CHECK(ok > 10, "fuzz produced valid messages too"); // ~30 expected: 1/9 lengths are 4, then op and reason must agree
    Report("TRADE_COMMIT parser fuzz (40000 buffers)", failuresBefore);
}

// ===========================================================================
// 2. Pure decision logic
// ===========================================================================

struct SentDecision
{
    u8 slot, op, groupId, seq, reason;
};

static struct SentDecision sSent[64];
static u32 sSentCount;

static void RecordSend(u8 slot, u8 op, u8 groupId, u8 seq, u8 reason)
{
    if (sSentCount < (sizeof(sSent) / sizeof(sSent[0])))
    {
        sSent[sSentCount].slot = slot;
        sSent[sSentCount].op = op;
        sSent[sSentCount].groupId = groupId;
        sSent[sSentCount].seq = seq;
        sSent[sSentCount].reason = reason;
    }
    sSentCount++;
}

static void ClearSent(void)
{
    sSentCount = 0;
}

static bool8 SentIs(u32 i, u8 slot, u8 op, u8 reason)
{
    return i < sSentCount && sSent[i].slot == slot && sSent[i].op == op && sSent[i].reason == reason;
}

static bool8 AnyCommit(void)
{
    u32 i;

    for (i = 0; i < sSentCount && i < (sizeof(sSent) / sizeof(sSent[0])); i++)
    {
        if (sSent[i].op == MP_TRADE_OP_COMMIT)
            return TRUE;
    }
    return FALSE;
}

static void TestPureLogic(void)
{
    struct MpTradeCommitHost h;
    u32 failuresBefore = sFail;

    // Both ready -> COMMIT to both, the waiting partner first.
    memset(&h, 0, sizeof(h));
    ClearSent();
    MpTradeCommit_HostReady(&h, 1, 5, 1, TRUE, 2, 1000, RecordSend);
    T_CHECK(sSentCount == 0, "one READY: no decision yet");
    MpTradeCommit_HostReady(&h, 2, 5, 1, TRUE, 1, 1100, RecordSend);
    T_CHECK(sSentCount == 2 && SentIs(0, 1, MP_TRADE_OP_COMMIT, 0) && SentIs(1, 2, MP_TRADE_OP_COMMIT, 0), "both READY: COMMIT to both, partner first");
    T_CHECK(h.commits == 1 && h.aborts == 0, "one committed round counted");
    // A repeated READY (its COMMIT was lost) gets the same decision again, only to itself.
    ClearSent();
    MpTradeCommit_HostReady(&h, 1, 5, 1, TRUE, 2, 1200, RecordSend);
    T_CHECK(sSentCount == 1 && SentIs(0, 1, MP_TRADE_OP_COMMIT, 0), "repeated READY: COMMIT repeated");
    // The tick never touches decided rounds.
    ClearSent();
    MpTradeCommit_HostTick(&h, 1000 + 10 * MP_TRADE_COMMIT_TIMEOUT_MS, RecordSend);
    MpTradeCommit_HostGroupEnded(&h, 0x06, RecordSend);
    T_CHECK(sSentCount == 0, "timeout / group end after COMMIT: nothing changes");
    Report("commit logic: both ready -> COMMIT both; decisions are final", failuresBefore);

    // Next round in the same group: the group ends with only one READY -> ABORT, and the late
    // READY of the other gets ABORT too. Nobody gets COMMIT.
    failuresBefore = sFail;
    ClearSent();
    MpTradeCommit_HostReady(&h, 1, 5, 2, TRUE, 2, 2000, RecordSend);
    MpTradeCommit_HostGroupEnded(&h, 0x06, RecordSend);
    T_CHECK(sSentCount == 1 && SentIs(0, 1, MP_TRADE_OP_ABORT, MP_TRADE_ABORT_GROUP_ENDED), "group ended: ABORT to the ready member");
    MpTradeCommit_HostReady(&h, 2, 5, 2, FALSE, MP_SLOT_NONE, 2100, RecordSend);
    T_CHECK(sSentCount == 2 && SentIs(1, 2, MP_TRADE_OP_ABORT, MP_TRADE_ABORT_GROUP_ENDED), "late READY after the group ended: ABORT");
    T_CHECK(!AnyCommit(), "no COMMIT when one member dropped before both were ready");
    Report("commit logic: member drops before commit -> neither commits", failuresBefore);

    // Timeout: the partner never gets ready; its READY after the timeout is refused as well.
    failuresBefore = sFail;
    memset(&h, 0, sizeof(h));
    ClearSent();
    MpTradeCommit_HostReady(&h, 0, 9, 1, TRUE, 3, 50000, RecordSend);
    MpTradeCommit_HostTick(&h, 50000 + MP_TRADE_COMMIT_TIMEOUT_MS - 1, RecordSend);
    T_CHECK(sSentCount == 0, "no timeout before MP_TRADE_COMMIT_TIMEOUT_MS");
    MpTradeCommit_HostTick(&h, 50000 + MP_TRADE_COMMIT_TIMEOUT_MS, RecordSend);
    T_CHECK(sSentCount == 1 && SentIs(0, 0, MP_TRADE_OP_ABORT, MP_TRADE_ABORT_TIMEOUT), "timeout: ABORT");
    MpTradeCommit_HostReady(&h, 3, 9, 1, TRUE, 0, 70000, RecordSend);
    T_CHECK(sSentCount == 2 && SentIs(1, 3, MP_TRADE_OP_ABORT, MP_TRADE_ABORT_TIMEOUT), "READY after the timeout: ABORT, not COMMIT");
    T_CHECK(!AnyCommit(), "no COMMIT after a timeout");
    // Timer wrap-around (u32 milliseconds).
    memset(&h, 0, sizeof(h));
    ClearSent();
    MpTradeCommit_HostReady(&h, 1, 2, 1, TRUE, 2, 0xFFFFFF00u, RecordSend);
    MpTradeCommit_HostTick(&h, 0x00000100u, RecordSend);
    T_CHECK(sSentCount == 0, "no false timeout across the u32 wrap");
    Report("commit logic: timeout, late READY, timer wrap", failuresBefore);

    // Disagreement and bad READYs.
    failuresBefore = sFail;
    memset(&h, 0, sizeof(h));
    ClearSent();
    MpTradeCommit_HostReady(&h, 1, 4, 1, TRUE, 2, 10, RecordSend);
    MpTradeCommit_HostReady(&h, 2, 4, 2, TRUE, 1, 20, RecordSend);
    T_CHECK(sSentCount == 2 && SentIs(0, 1, MP_TRADE_OP_ABORT, MP_TRADE_ABORT_MISMATCH) && SentIs(1, 2, MP_TRADE_OP_ABORT, MP_TRADE_ABORT_MISMATCH),
            "members on different rounds: ABORT both");
    ClearSent();
    MpTradeCommit_HostReady(&h, 3, 4, 1, FALSE, MP_SLOT_NONE, 30, RecordSend);
    T_CHECK(sSentCount == 1 && SentIs(0, 3, MP_TRADE_OP_ABORT, MP_TRADE_ABORT_MISMATCH), "READY outside a matching group: ABORT");
    ClearSent();
    MpTradeCommit_HostReady(&h, 2, 4, 3, TRUE, 2, 40, RecordSend);
    T_CHECK(sSentCount == 1 && SentIs(0, 2, MP_TRADE_OP_ABORT, MP_TRADE_ABORT_MISMATCH), "partner == itself: ABORT");
    ClearSent();
    MpTradeCommit_HostReady(&h, MP_MAX_PLAYERS, 4, 1, TRUE, 1, 50, RecordSend);
    MpTradeCommit_HostReady(&h, 1, 0, 1, TRUE, 2, 50, RecordSend);
    MpTradeCommit_HostReady(&h, 1, 4, 0, TRUE, 2, 50, RecordSend);
    T_CHECK(sSentCount == 0, "out-of-range slot / group 0 / seq 0 ignored");
    // A slot that left is forgotten; a new group starts clean.
    MpTradeCommit_HostForgetSlot(&h, 1);
    T_CHECK(h.rec[1].state == MP_TRADE_REC_NONE, "forgotten slot has no record");
    ClearSent();
    MpTradeCommit_HostReady(&h, 1, 5, 1, TRUE, 2, 60, RecordSend);
    MpTradeCommit_HostReady(&h, 2, 5, 1, TRUE, 1, 70, RecordSend);
    T_CHECK(sSentCount == 2 && SentIs(0, 1, MP_TRADE_OP_COMMIT, 0) && SentIs(1, 2, MP_TRADE_OP_COMMIT, 0), "a new group commits normally");
    Report("commit logic: mismatches and bad READYs abort", failuresBefore);
}

// Random events against the host logic, with members that behave like mp_trade_commit.c's: a
// member sends READY for a new round only when it is not waiting, may repeat it while waiting,
// and sometimes skips a round number (to provoke disagreement). Invariants: a member never gets
// two different decisions for one round; a (group, round) is never COMMIT for one member and
// ABORT for the other; once the group has ended and the timeout passed, nobody is left waiting.
#define FUZZ_MAX_GROUPS 64
#define FUZZ_MAX_SEQ    12

struct FuzzMember
{
    u8 groupId;
    u8 seq;
    bool8 waiting;
};

static u8 sFuzzDecision[2][FUZZ_MAX_GROUPS][FUZZ_MAX_SEQ + 1]; // [member][group][seq] = op or 0
static struct FuzzMember sFuzzMembers[2];

static void FuzzSend(u8 slot, u8 op, u8 groupId, u8 seq, u8 reason)
{
    u8 member = (slot == 1) ? 0 : 1;
    struct FuzzMember *m = &sFuzzMembers[member];
    u8 *d;

    if ((slot != 1 && slot != 2) || groupId >= FUZZ_MAX_GROUPS || seq > FUZZ_MAX_SEQ)
    {
        sFail++;
        MP_LOG("selftest: FAIL: trade: fuzz: decision out of range\n");
        return;
    }
    d = &sFuzzDecision[member][groupId][seq];
    if (*d != 0 && *d != op)
    {
        sFail++;
        MP_LOG("selftest: FAIL: trade: fuzz: slot %u got two decisions for group %u round %u\n", (unsigned)slot, (unsigned)groupId, (unsigned)seq);
    }
    *d = op;
    if (m->waiting && m->groupId == groupId && m->seq == seq)
        m->waiting = FALSE;
}

static void TestPureFuzz(void)
{
    struct MpTradeCommitHost h;
    u32 failuresBefore = sFail;
    u32 now = 1;
    u8 groupId;
    bool8 alive;
    u32 i, run;
    u8 g, s;

    for (run = 0; run < 20; run++)
    {
        memset(&h, 0, sizeof(h));
        memset(sFuzzDecision, 0, sizeof(sFuzzDecision));
        memset(sFuzzMembers, 0, sizeof(sFuzzMembers));
        groupId = 1;
        alive = TRUE;
        sFuzzMembers[0].groupId = sFuzzMembers[1].groupId = groupId;
        for (i = 0; i < 3000; i++)
        {
            u32 r = Rand() % 100;
            u8 member = (u8)(Rand() & 1);
            u8 slot = (u8)(member + 1);
            struct FuzzMember *m = &sFuzzMembers[member];

            now += Rand() % 3000;
            if (r < 55)
            {
                if (!m->waiting)
                {
                    if (m->seq + 2 > FUZZ_MAX_SEQ)
                        continue;
                    m->seq += ((Rand() % 10) == 0) ? 2 : 1; // now and then a member skips a round
                    m->waiting = TRUE;
                }
                MpTradeCommit_HostReady(&h, slot, m->groupId, m->seq, alive && m->groupId == groupId, (u8)(3 - slot), now, FuzzSend);
            }
            else if (r < 70)
            {
                MpTradeCommit_HostTick(&h, now, FuzzSend);
            }
            else if (r < 74 && alive)
            {
                MpTradeCommit_HostGroupEnded(&h, 0x06, FuzzSend);
                alive = FALSE;
            }
            else if (r < 80 && !alive && groupId + 1 < FUZZ_MAX_GROUPS)
            {
                groupId++;
                alive = TRUE;
                for (g = 0; g < 2; g++)
                {
                    if (!sFuzzMembers[g].waiting)
                    {
                        sFuzzMembers[g].groupId = groupId;
                        sFuzzMembers[g].seq = 0;
                    }
                }
            }
        }
        // Everything still waiting is decided once the group ends and time passes.
        MpTradeCommit_HostGroupEnded(&h, 0x06, FuzzSend);
        MpTradeCommit_HostTick(&h, now + MP_TRADE_COMMIT_TIMEOUT_MS, FuzzSend);
        T_CHECK(h.rec[1].state != MP_TRADE_REC_READY && h.rec[2].state != MP_TRADE_REC_READY, "fuzz: no READY left undecided");
        T_CHECK(!sFuzzMembers[0].waiting && !sFuzzMembers[1].waiting, "fuzz: no member left waiting");
        for (g = 0; g < FUZZ_MAX_GROUPS; g++)
        {
            for (s = 0; s <= FUZZ_MAX_SEQ; s++)
            {
                u8 a = sFuzzDecision[0][g][s], b = sFuzzDecision[1][g][s];

                if ((a == MP_TRADE_OP_COMMIT && b == MP_TRADE_OP_ABORT) || (a == MP_TRADE_OP_ABORT && b == MP_TRADE_OP_COMMIT))
                {
                    sFail++;
                    MP_LOG("selftest: FAIL: trade: fuzz: group %u round %u committed on one side only\n", (unsigned)g, (unsigned)s);
                }
            }
        }
    }
    Report("commit logic fuzz (20 x 3000 random events): never COMMIT on one side only", failuresBefore);
}

// ===========================================================================
// 3. In process: host + 2 clients over loopback
// ===========================================================================

#define TRADE_PORT_FIRST    27530
#define TRADE_PORT_LAST     27539
#define S_HOST  0
#define S_C1    1
#define S_C2    2
#define S_COUNT 3

static u16 sPort;

static void Sel(u8 instance)
{
    MpSession_SelectInstance(instance);
}

static void PumpOnce(void)
{
    struct MpEvent ev;
    u8 i;

    for (i = 0; i < S_COUNT; i++)
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

typedef bool8 (*InstCond)(void);

static bool8 WaitFor(u8 instance, InstCond cond, u32 timeoutMs)
{
    u32 start = MpTransport_GetTimeMs();
    bool8 ok;

    for (;;)
    {
        Sel(instance);
        ok = cond();
        if (ok || (u32)(MpTransport_GetTimeMs() - start) >= timeoutMs)
            return ok;
        PumpOnce();
    }
}

static bool8 CondInSession(void)    { return MP_GetState() == MP_STATE_IN_SESSION; }
static bool8 CondGrouped(void)      { return MP_LinkGetState() == MP_LINK_STATE_GROUPED; }
static bool8 CondUngrouped(void)    { return MP_LinkGetState() == MP_LINK_STATE_NONE; }
static bool8 CondDecided(void)      { u8 st = MP_TradeCommitPoll(); return st == MP_TRADE_COMMIT_COMMIT || st == MP_TRADE_COMMIT_ABORT; }

static u8 PollOf(u8 instance)
{
    Sel(instance);
    return MP_TradeCommitPoll();
}

static u8 ReasonOf(u8 instance)
{
    Sel(instance);
    return MP_TradeCommitGetAbortReason();
}

static void ResetOf(u8 instance)
{
    Sel(instance);
    MP_TradeCommitReset();
}

static bool8 JoinAs(u8 instance, const char *name)
{
    Sel(instance);
    if (!MP_SetNameOverride(name))
        return FALSE;
    return MP_Join("127.0.0.1", sPort) && WaitFor(instance, CondInSession, 5000);
}

static bool8 GroupUp(u8 a, u8 b)
{
    u8 mask = (u8)((1 << a) | (1 << b));

    Sel(a);
    if (!MP_LinkJoin(mask))
        return FALSE;
    Sel(b);
    if (!MP_LinkJoin(mask))
        return FALSE;
    return WaitFor(a, CondGrouped, 3000) && WaitFor(b, CondGrouped, 3000);
}

static bool8 Ready(u8 instance)
{
    Sel(instance);
    return MP_TradeCommitReady();
}

static void CleanUp(void)
{
    u32 start;
    bool8 open;
    u8 i;

    for (i = 0; i < S_COUNT; i++)
    {
        Sel(i);
        MP_Leave();
    }
    start = MpTransport_GetTimeMs();
    do
    {
        PumpOnce();
        open = FALSE;
        for (i = 0; i < S_COUNT; i++)
        {
            Sel(i);
            open = open || MpTransport_IsOpen();
        }
    } while (open && (u32)(MpTransport_GetTimeMs() - start) < 3000);
    for (i = 0; i < S_COUNT; i++)
    {
        Sel(i);
        MpTransport_Close();
        MpSession_ResetInstance(i);
    }
    Sel(0);
}

static void TestInProcess(void)
{
    u32 failuresBefore = sFail;
    bool8 hosting = FALSE;
    u8 i;

    for (i = 0; i < S_COUNT; i++)
        MpSession_ResetInstance(i);
    Sel(S_HOST);
    MP_SetNameOverride("Host");
    for (sPort = TRADE_PORT_FIRST; sPort <= TRADE_PORT_LAST && !hosting; sPort++)
        hosting = MP_Host(sPort);
    sPort--;
    T_CHECK(hosting, "trade host on a port in 27530..27539");
    if (!hosting)
        return;
    T_CHECK(JoinAs(S_C1, "TrdA"), "client 1 joins");
    T_CHECK(JoinAs(S_C2, "TrdB"), "client 2 joins");
    Sel(S_C1); T_CHECK(MP_GetLocalSlot() == 1, "client 1 is slot 1");
    Sel(S_C2); T_CHECK(MP_GetLocalSlot() == 2, "client 2 is slot 2");
    Report("session for the trade commit tests", failuresBefore);

    // No group: READY is refused locally.
    failuresBefore = sFail;
    T_CHECK(!Ready(S_C1), "READY without a link group is refused");
    T_CHECK(PollOf(S_C1) == MP_TRADE_COMMIT_ABORT && ReasonOf(S_C1) == MP_TRADE_ABORT_NOT_GROUPED, "... as ABORT (not grouped)");
    ResetOf(S_C1);
    T_CHECK(PollOf(S_C1) == MP_TRADE_COMMIT_IDLE, "reset to IDLE");
    Report("READY without a group", failuresBefore);

    // A: clients 1 and 2 trade, the host only arbitrates. Two rounds in one group.
    failuresBefore = sFail;
    T_CHECK(GroupUp(S_C1, S_C2), "C1 + C2 grouped");
    T_CHECK(Ready(S_C1), "C1 READY");
    PumpFor(150);
    T_CHECK(PollOf(S_C1) == MP_TRADE_COMMIT_WAITING, "C1 waits while C2 is not ready");
    T_CHECK(Ready(S_C2), "C2 READY");
    T_CHECK(WaitFor(S_C1, CondDecided, 3000) && WaitFor(S_C2, CondDecided, 3000), "both decided");
    T_CHECK(PollOf(S_C1) == MP_TRADE_COMMIT_COMMIT && PollOf(S_C2) == MP_TRADE_COMMIT_COMMIT, "both COMMIT");
    ResetOf(S_C1);
    ResetOf(S_C2);
    T_CHECK(Ready(S_C2) && Ready(S_C1), "round 2: both READY");
    T_CHECK(WaitFor(S_C1, CondDecided, 3000) && WaitFor(S_C2, CondDecided, 3000), "round 2 decided");
    T_CHECK(PollOf(S_C1) == MP_TRADE_COMMIT_COMMIT && PollOf(S_C2) == MP_TRADE_COMMIT_COMMIT, "round 2: both COMMIT");
    ResetOf(S_C1);
    ResetOf(S_C2);
    Report("client + client trade: both ready -> both COMMIT (2 rounds)", failuresBefore);

    // B: round 3, C2 leaves the link before it is ready: C1 gets ABORT, C2 never commits.
    failuresBefore = sFail;
    T_CHECK(Ready(S_C1), "C1 READY (round 3)");
    PumpFor(100);
    Sel(S_C2);
    MP_LinkLeave();
    T_CHECK(WaitFor(S_C1, CondDecided, 3000), "C1 decided");
    T_CHECK(PollOf(S_C1) == MP_TRADE_COMMIT_ABORT && ReasonOf(S_C1) == MP_TRADE_ABORT_GROUP_ENDED, "C1: ABORT (group ended)");
    T_CHECK(!Ready(S_C2) && PollOf(S_C2) == MP_TRADE_COMMIT_ABORT, "C2: no group any more, ABORT");
    ResetOf(S_C1);
    ResetOf(S_C2);
    T_CHECK(WaitFor(S_C1, CondUngrouped, 3000), "C1's group is gone too");
    Report("member leaves the link before commit -> neither commits", failuresBefore);

    // C: the host trades with client 1.
    failuresBefore = sFail;
    T_CHECK(GroupUp(S_HOST, S_C1), "host + C1 grouped");
    T_CHECK(Ready(S_HOST), "host READY");
    PumpFor(100);
    T_CHECK(PollOf(S_HOST) == MP_TRADE_COMMIT_WAITING, "host waits for C1");
    T_CHECK(Ready(S_C1), "C1 READY");
    T_CHECK(WaitFor(S_C1, CondDecided, 3000) && WaitFor(S_HOST, CondDecided, 3000), "both decided");
    T_CHECK(PollOf(S_HOST) == MP_TRADE_COMMIT_COMMIT && PollOf(S_C1) == MP_TRADE_COMMIT_COMMIT, "host and C1 COMMIT");
    ResetOf(S_HOST);
    ResetOf(S_C1);
    // Client first, then the host: the host's own COMMIT comes after the client's (flushed).
    T_CHECK(Ready(S_C1), "round 2: C1 READY");
    PumpFor(100);
    T_CHECK(Ready(S_HOST), "round 2: host READY");
    T_CHECK(PollOf(S_HOST) == MP_TRADE_COMMIT_COMMIT, "host decided at once (its own READY completed the round)");
    T_CHECK(WaitFor(S_C1, CondDecided, 3000) && PollOf(S_C1) == MP_TRADE_COMMIT_COMMIT, "C1 COMMIT");
    ResetOf(S_HOST);
    ResetOf(S_C1);
    Report("host + client trade: both COMMIT, either order", failuresBefore);

    // D: round 3, the host is ready and C1's session ends: the host gets ABORT, C1 (never
    // ready) has nothing to commit.
    failuresBefore = sFail;
    T_CHECK(Ready(S_HOST), "host READY (round 3)");
    Sel(S_C1);
    MP_Leave();
    T_CHECK(WaitFor(S_HOST, CondDecided, 5000), "host decided");
    T_CHECK(PollOf(S_HOST) == MP_TRADE_COMMIT_ABORT && ReasonOf(S_HOST) == MP_TRADE_ABORT_GROUP_ENDED, "host: ABORT (C1 gone)");
    T_CHECK(PollOf(S_C1) == MP_TRADE_COMMIT_IDLE, "C1: nothing to commit");
    ResetOf(S_HOST);
    Report("client leaves the session before commit -> neither commits", failuresBefore);

    // E: in doubt. C2 groups with the host, sends READY and its session ends before any
    // decision: C2 presumes ABORT; the host, never ready, does not commit either.
    failuresBefore = sFail;
    T_CHECK(GroupUp(S_HOST, S_C2), "host + C2 grouped");
    T_CHECK(Ready(S_C2), "C2 READY");
    Sel(S_C2);
    MP_Leave();
    T_CHECK(PollOf(S_C2) == MP_TRADE_COMMIT_ABORT && ReasonOf(S_C2) == MP_TRADE_ABORT_IN_DOUBT, "C2: ABORT (in doubt, presumed)");
    PumpFor(300);
    T_CHECK(PollOf(S_HOST) == MP_TRADE_COMMIT_IDLE, "host: not ready, nothing to commit");
    Sel(S_HOST);
    T_CHECK(!MP_TradeCommitReady() && MP_TradeCommitPoll() == MP_TRADE_COMMIT_ABORT, "host: READY after C2 left is refused");
    ResetOf(S_HOST);
    ResetOf(S_C2);
    Report("member's session ends while it waits -> presumed ABORT", failuresBefore);

    CleanUp();
}

u32 MpSelftest_Trade(void)
{
    sFail = 0;
    TestParser();
    TestPureLogic();
    TestPureFuzz();
    TestInProcess();
    return sFail;
}
