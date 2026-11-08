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
#include "mp/mp_trade_journal.h"
#include "mp/mp_transport.h"
#include "platform.h"

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
// 1. Parsers (TRADE_COMMIT, TRADE_RECOVER)
// ===========================================================================

#define TID(g, s) ((u32)(g) * 256u + (u32)(s) + 1u)     // a per-round trade id for the pure tests

static u8 ParseCommit(u8 op, u8 g, u8 s, u8 reason, u32 tradeId)
{
    struct MpMsgTradeCommit m;
    u8 p[MP_TRADE_COMMIT_SIZE];

    p[0] = op;
    p[1] = g;
    p[2] = s;
    p[3] = reason;
    p[4] = (u8)tradeId;
    p[5] = (u8)(tradeId >> 8);
    p[6] = (u8)(tradeId >> 16);
    p[7] = (u8)(tradeId >> 24);
    return MpProto_ParseTradeCommit(p, sizeof(p), &m);
}

static void FixtureRecover(struct MpMsgTradeRecover *m)
{
    memset(m, 0, sizeof(*m));
    m->slot = 2;
    m->status = MP_TJ_STATUS_READY;
    m->flags = MP_TRADE_RECOVER_FLAG_REPLY;
    m->tradeId = 0xA1B2C3D4u;
    m->senderTid = 0x00053039u;
    m->receiverTid = 0x0001A11Cu;
    m->senderGivenPersonality = 0x11111111u;
    m->senderGivenOtId = 0x00053039u;
    m->senderRecvPersonality = 0x22222222u;
    m->senderRecvOtId = 0x0001A11Cu;
}

static bool8 RecoverEquals(const struct MpMsgTradeRecover *a, const struct MpMsgTradeRecover *b)
{
    return a->slot == b->slot && a->status == b->status && a->flags == b->flags && a->tradeId == b->tradeId
        && a->senderTid == b->senderTid && a->receiverTid == b->receiverTid
        && a->senderGivenPersonality == b->senderGivenPersonality && a->senderGivenOtId == b->senderGivenOtId
        && a->senderRecvPersonality == b->senderRecvPersonality && a->senderRecvOtId == b->senderRecvOtId;
}

static void TestParser(void)
{
    u32 failuresBefore = sFail;
    struct MpMsgTradeCommit in, out;
    struct MpMsgTradeRecover rin, rout;
    struct MpMsgHeader hdr;
    u8 buf[MP_MAX_PACKET_SIZE];
    u8 raw[40];
    u16 len;
    u32 i, j, ok = 0;
    u8 op, reason;

    // TRADE_COMMIT round trip of every valid op / reason.
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
            in.tradeId = 0xDEADBEEFu;
            len = MpProto_WriteTradeCommit(buf, sizeof(buf), &in);
            T_CHECK(len == 4 + MP_TRADE_COMMIT_SIZE, "TRADE_COMMIT writes 12 bytes");
            T_CHECK(MpProto_ParseHeader(buf, len, &hdr) && hdr.type == MP_MSG_TRADE_COMMIT
                    && hdr.version == MP_PROTOCOL_VERSION && hdr.length == MP_TRADE_COMMIT_SIZE, "TRADE_COMMIT header");
            T_CHECK(MpProto_ParseTradeCommit(buf + 4, len - 4, &out) == MP_PARSE_OK && out.op == in.op && out.groupId == 7
                    && out.seq == 200 && out.reason == reason && out.tradeId == 0xDEADBEEFu, "TRADE_COMMIT round trip");
        }
    }
    T_CHECK(MP_MSG_TRADE_COMMIT >= MP_MSG_LINK_FIRST && MP_MSG_TRADE_RECOVER <= MP_MSG_LINK_LAST, "TRADE_* are in the LINK_* range");
    T_CHECK(MP_PROTOCOL_VERSION >= 7, "protocol version 7 or later (PS-050; 8 since PS-045)");
    in.op = 0;
    T_CHECK(MpProto_WriteTradeCommit(buf, sizeof(buf), &in) == 0, "writer refuses op 0");
    in.op = MP_TRADE_OP_READY;
    T_CHECK(MpProto_WriteTradeCommit(buf, 6, &in) == 0, "writer refuses a too small buffer");
    memset(raw, 0, sizeof(raw));
    raw[0] = MP_TRADE_OP_READY;
    raw[1] = 1;
    raw[2] = 1;
    raw[4] = 1;
    for (i = 0; i < MP_TRADE_COMMIT_SIZE; i++)
        T_CHECK(MpProto_ParseTradeCommit(raw, i, &out) == MP_PARSE_ERR_TRUNCATED, "short TRADE_COMMIT is TRUNCATED");
    T_CHECK(MpProto_ParseTradeCommit(raw, MP_TRADE_COMMIT_SIZE, &out) == MP_PARSE_OK, "8-byte READY parses");
    T_CHECK(MpProto_ParseTradeCommit(raw, MP_TRADE_COMMIT_SIZE + 1, &out) == MP_PARSE_ERR_OVERSIZED, "long TRADE_COMMIT is OVERSIZED");
    T_CHECK(ParseCommit(0, 1, 1, 0, 1) == MP_PARSE_ERR_BAD_FIELD, "op 0 refused");
    T_CHECK(ParseCommit(4, 1, 1, 0, 1) == MP_PARSE_ERR_BAD_FIELD, "op 4 refused");
    T_CHECK(ParseCommit(MP_TRADE_OP_READY, 0, 1, 0, 1) == MP_PARSE_ERR_BAD_FIELD, "group 0 refused");
    T_CHECK(ParseCommit(MP_TRADE_OP_READY, 1, 0, 0, 1) == MP_PARSE_ERR_BAD_FIELD, "seq 0 refused");
    T_CHECK(ParseCommit(MP_TRADE_OP_READY, 1, 1, 0, 0) == MP_PARSE_ERR_BAD_FIELD, "READY without a trade id refused");
    T_CHECK(ParseCommit(MP_TRADE_OP_COMMIT, 1, 1, 0, 0) == MP_PARSE_OK, "COMMIT may carry trade id 0");
    T_CHECK(ParseCommit(MP_TRADE_OP_READY, 1, 1, 1, 1) == MP_PARSE_ERR_BAD_FIELD, "READY with a reason refused");
    T_CHECK(ParseCommit(MP_TRADE_OP_ABORT, 1, 1, 0, 1) == MP_PARSE_ERR_BAD_FIELD, "ABORT without a reason refused");
    T_CHECK(ParseCommit(MP_TRADE_OP_ABORT, 1, 1, MP_TRADE_ABORT_IN_DOUBT, 1) == MP_PARSE_ERR_BAD_FIELD, "local-only reason refused");
    T_CHECK(!MpTradeCommit_OnHostReceive(1, (const u8 *)"\x02\x01\x01\x00\x01\x00\x00\x00", 8), "host refuses COMMIT from a client");
    T_CHECK(!MpTradeCommit_OnClientReceive((const u8 *)"\x01\x01\x01\x00\x01\x00\x00\x00", 8), "client refuses READY from the host");

    // TRADE_RECOVER.
    FixtureRecover(&rin);
    len = MpProto_WriteTradeRecover(buf, sizeof(buf), &rin);
    T_CHECK(len == 4 + MP_TRADE_RECOVER_SIZE, "TRADE_RECOVER writes 36 bytes");
    T_CHECK(MpProto_ParseHeader(buf, len, &hdr) && hdr.type == MP_MSG_TRADE_RECOVER && hdr.length == MP_TRADE_RECOVER_SIZE, "TRADE_RECOVER header");
    T_CHECK(MpProto_ParseTradeRecover(buf + 4, len - 4, &rout) == MP_PARSE_OK && RecoverEquals(&rin, &rout), "TRADE_RECOVER round trip");
    memcpy(raw, buf + 4, MP_TRADE_RECOVER_SIZE);
    for (i = 0; i < MP_TRADE_RECOVER_SIZE; i++)
        T_CHECK(MpProto_ParseTradeRecover(raw, i, &rout) == MP_PARSE_ERR_TRUNCATED, "short TRADE_RECOVER is TRUNCATED");
    T_CHECK(MpProto_ParseTradeRecover(raw, MP_TRADE_RECOVER_SIZE + 1, &rout) == MP_PARSE_ERR_OVERSIZED, "long TRADE_RECOVER is OVERSIZED");
    raw[0] = 4;
    T_CHECK(MpProto_ParseTradeRecover(raw, MP_TRADE_RECOVER_SIZE, &rout) == MP_PARSE_ERR_BAD_FIELD, "slot 4 refused");
    raw[0] = MP_SLOT_NONE;
    T_CHECK(MpProto_ParseTradeRecover(raw, MP_TRADE_RECOVER_SIZE, &rout) == MP_PARSE_OK, "slot NONE accepted");
    raw[1] = 0;
    T_CHECK(MpProto_ParseTradeRecover(raw, MP_TRADE_RECOVER_SIZE, &rout) == MP_PARSE_ERR_BAD_FIELD, "status 0 refused");
    raw[1] = MP_TJ_STATUS_COUNT;
    T_CHECK(MpProto_ParseTradeRecover(raw, MP_TRADE_RECOVER_SIZE, &rout) == MP_PARSE_ERR_BAD_FIELD, "status past the end refused");
    raw[1] = MP_TJ_STATUS_READY;
    raw[2] = 2;
    T_CHECK(MpProto_ParseTradeRecover(raw, MP_TRADE_RECOVER_SIZE, &rout) == MP_PARSE_ERR_BAD_FIELD, "unknown flag refused");
    raw[2] = 0;
    raw[3] = 1;
    T_CHECK(MpProto_ParseTradeRecover(raw, MP_TRADE_RECOVER_SIZE, &rout) == MP_PARSE_ERR_BAD_FIELD, "reserved byte refused");
    raw[3] = 0;
    memset(raw + 4, 0, 4);
    T_CHECK(MpProto_ParseTradeRecover(raw, MP_TRADE_RECOVER_SIZE, &rout) == MP_PARSE_ERR_BAD_FIELD, "trade id 0 refused");
    T_CHECK(!MpTradeCommit_OnClientReceiveRecover(raw, 10), "client refuses a short TRADE_RECOVER");
    Report("TRADE_COMMIT / TRADE_RECOVER parsers: round trip, sizes, fields, direction", failuresBefore);

    // Fuzz: random lengths and bytes. Whatever parses obeys the rules and round-trips.
    failuresBefore = sFail;
    for (i = 0; i < 40000; i++)
    {
        u32 n = (i & 1) ? MP_TRADE_COMMIT_SIZE + (Rand() % 3) - 1 : MP_TRADE_RECOVER_SIZE + (Rand() % 3) - 1;

        for (j = 0; j < n; j++)
            raw[j] = (u8)((Rand() & 3) == 0 ? (Rand() % 8) : Rand());
        if (i & 1)
        {
            if (MpProto_ParseTradeCommit(raw, n, &out) != MP_PARSE_OK)
                continue;
            ok++;
            T_CHECK(n == MP_TRADE_COMMIT_SIZE && out.op >= MP_TRADE_OP_READY && out.op <= MP_TRADE_OP_ABORT && out.groupId != 0 && out.seq != 0,
                    "parsed TRADE_COMMIT in range");
            len = MpProto_WriteTradeCommit(buf, sizeof(buf), &out);
            T_CHECK(len == 4 + MP_TRADE_COMMIT_SIZE && memcmp(buf + 4, raw, n) == 0, "parsed TRADE_COMMIT writes back the same bytes");
        }
        else
        {
            if (MpProto_ParseTradeRecover(raw, n, &rout) != MP_PARSE_OK)
            {
                T_CHECK(rout.tradeId == 0 && rout.status == 0, "failed parse leaves the output zeroed");
                continue;
            }
            ok++;
            T_CHECK(n == MP_TRADE_RECOVER_SIZE && rout.status != 0 && rout.status < MP_TJ_STATUS_COUNT && rout.tradeId != 0, "parsed TRADE_RECOVER in range");
            len = MpProto_WriteTradeRecover(buf, sizeof(buf), &rout);
            T_CHECK(len == 4 + MP_TRADE_RECOVER_SIZE && memcmp(buf + 4, raw, n) == 0, "parsed TRADE_RECOVER writes back the same bytes");
        }
    }
    T_CHECK(ok > 10, "fuzz produced valid messages too");
    Report("TRADE_COMMIT / TRADE_RECOVER parser fuzz (40000 buffers)", failuresBefore);
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
    MpTradeCommit_HostReady(&h, 1, 5, 1, TID(5, 1), TRUE, 2, 1000, RecordSend);
    T_CHECK(sSentCount == 0, "one READY: no decision yet");
    MpTradeCommit_HostReady(&h, 2, 5, 1, TID(5, 1), TRUE, 1, 1100, RecordSend);
    T_CHECK(sSentCount == 2 && SentIs(0, 1, MP_TRADE_OP_COMMIT, 0) && SentIs(1, 2, MP_TRADE_OP_COMMIT, 0), "both READY: COMMIT to both, partner first");
    T_CHECK(h.commits == 1 && h.aborts == 0, "one committed round counted");
    // A repeated READY (its COMMIT was lost) gets the same decision again, only to itself.
    ClearSent();
    MpTradeCommit_HostReady(&h, 1, 5, 1, TID(5, 1), TRUE, 2, 1200, RecordSend);
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
    MpTradeCommit_HostReady(&h, 1, 5, 2, TID(5, 2), TRUE, 2, 2000, RecordSend);
    MpTradeCommit_HostGroupEnded(&h, 0x06, RecordSend);
    T_CHECK(sSentCount == 1 && SentIs(0, 1, MP_TRADE_OP_ABORT, MP_TRADE_ABORT_GROUP_ENDED), "group ended: ABORT to the ready member");
    MpTradeCommit_HostReady(&h, 2, 5, 2, TID(5, 2), FALSE, MP_SLOT_NONE, 2100, RecordSend);
    T_CHECK(sSentCount == 2 && SentIs(1, 2, MP_TRADE_OP_ABORT, MP_TRADE_ABORT_GROUP_ENDED), "late READY after the group ended: ABORT");
    T_CHECK(!AnyCommit(), "no COMMIT when one member dropped before both were ready");
    Report("commit logic: member drops before commit -> neither commits", failuresBefore);

    // Timeout: the partner never gets ready; its READY after the timeout is refused as well.
    failuresBefore = sFail;
    memset(&h, 0, sizeof(h));
    ClearSent();
    MpTradeCommit_HostReady(&h, 0, 9, 1, TID(9, 1), TRUE, 3, 50000, RecordSend);
    MpTradeCommit_HostTick(&h, 50000 + MP_TRADE_COMMIT_TIMEOUT_MS - 1, RecordSend);
    T_CHECK(sSentCount == 0, "no timeout before MP_TRADE_COMMIT_TIMEOUT_MS");
    MpTradeCommit_HostTick(&h, 50000 + MP_TRADE_COMMIT_TIMEOUT_MS, RecordSend);
    T_CHECK(sSentCount == 1 && SentIs(0, 0, MP_TRADE_OP_ABORT, MP_TRADE_ABORT_TIMEOUT), "timeout: ABORT");
    MpTradeCommit_HostReady(&h, 3, 9, 1, TID(9, 1), TRUE, 0, 70000, RecordSend);
    T_CHECK(sSentCount == 2 && SentIs(1, 3, MP_TRADE_OP_ABORT, MP_TRADE_ABORT_TIMEOUT), "READY after the timeout: ABORT, not COMMIT");
    T_CHECK(!AnyCommit(), "no COMMIT after a timeout");
    // Timer wrap-around (u32 milliseconds).
    memset(&h, 0, sizeof(h));
    ClearSent();
    MpTradeCommit_HostReady(&h, 1, 2, 1, TID(2, 1), TRUE, 2, 0xFFFFFF00u, RecordSend);
    MpTradeCommit_HostTick(&h, 0x00000100u, RecordSend);
    T_CHECK(sSentCount == 0, "no false timeout across the u32 wrap");
    Report("commit logic: timeout, late READY, timer wrap", failuresBefore);

    // Disagreement and bad READYs.
    failuresBefore = sFail;
    memset(&h, 0, sizeof(h));
    ClearSent();
    MpTradeCommit_HostReady(&h, 1, 4, 1, TID(4, 1), TRUE, 2, 10, RecordSend);
    MpTradeCommit_HostReady(&h, 2, 4, 2, TID(4, 2), TRUE, 1, 20, RecordSend);
    T_CHECK(sSentCount == 2 && SentIs(0, 1, MP_TRADE_OP_ABORT, MP_TRADE_ABORT_MISMATCH) && SentIs(1, 2, MP_TRADE_OP_ABORT, MP_TRADE_ABORT_MISMATCH),
            "members on different rounds: ABORT both");
    ClearSent();
    MpTradeCommit_HostReady(&h, 3, 4, 1, TID(4, 1), FALSE, MP_SLOT_NONE, 30, RecordSend);
    T_CHECK(sSentCount == 1 && SentIs(0, 3, MP_TRADE_OP_ABORT, MP_TRADE_ABORT_MISMATCH), "READY outside a matching group: ABORT");
    ClearSent();
    MpTradeCommit_HostReady(&h, 2, 4, 3, TID(4, 3), TRUE, 2, 40, RecordSend);
    T_CHECK(sSentCount == 1 && SentIs(0, 2, MP_TRADE_OP_ABORT, MP_TRADE_ABORT_MISMATCH), "partner == itself: ABORT");
    ClearSent();
    MpTradeCommit_HostReady(&h, MP_MAX_PLAYERS, 4, 1, TID(4, 1), TRUE, 1, 50, RecordSend);
    MpTradeCommit_HostReady(&h, 1, 0, 1, TID(0, 1), TRUE, 2, 50, RecordSend);
    MpTradeCommit_HostReady(&h, 1, 4, 0, TID(4, 0), TRUE, 2, 50, RecordSend);
    T_CHECK(sSentCount == 0, "out-of-range slot / group 0 / seq 0 ignored");
    // A slot that left is forgotten; a new group starts clean.
    MpTradeCommit_HostForgetSlot(&h, 1);
    T_CHECK(h.rec[1].state == MP_TRADE_REC_NONE, "forgotten slot has no record");
    ClearSent();
    MpTradeCommit_HostReady(&h, 1, 5, 1, TID(5, 1), TRUE, 2, 60, RecordSend);
    MpTradeCommit_HostReady(&h, 2, 5, 1, TID(5, 1), TRUE, 1, 70, RecordSend);
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
                MpTradeCommit_HostReady(&h, slot, m->groupId, m->seq, TID(m->groupId, m->seq), alive && m->groupId == groupId, (u8)(3 - slot), now, FuzzSend);
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
    return MP_TradeCommitReady(0x5A5A0001u);
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
    T_CHECK(!MP_TradeCommitReady(0x5A5A0001u) && MP_TradeCommitPoll() == MP_TRADE_COMMIT_ABORT, "host: READY after C2 left is refused");
    ResetOf(S_HOST);
    ResetOf(S_C2);
    Report("member's session ends while it waits -> presumed ABORT", failuresBefore);

    CleanUp();
}

// ===========================================================================
// 4. Trade journal (PS-050): record, file, rules, host decision ring, crash at every step
// ===========================================================================

static void FixtureJournal(struct MpTradeJournal *j)
{
    u32 i;

    memset(j, 0, sizeof(*j));
    j->state = MP_TJ_STATE_READY;
    j->partyIndex = 2;
    j->myTid = 0x00053039u;
    j->partnerTid = 0x0001A11Cu;
    j->partnerName[0] = 0xBB; // "A"
    for (i = 1; i < MP_PLAYER_NAME_SIZE; i++)
        j->partnerName[i] = 0xFF;
    j->given.personality = 0x11111111u;
    j->given.otId = 0x00053039u;
    j->given.species = 67;     // MACHOKE
    j->received.personality = 0x22222222u;
    j->received.otId = 0x0001A11Cu;
    j->received.species = 64;  // KADABRA
    j->receivedFinalSpecies = 65;
    for (i = 0; i < MP_MON_WIRE_SIZE; i++)
        j->receivedMon[i] = (u8)(i * 7 + 3);
    memcpy(j->receivedMon, &j->received.personality, 4); // little-endian PC builds
    memcpy(j->receivedMon + 4, &j->received.otId, 4);
    j->tradeId = MpTradeJournal_ComputeId(j->myTid, &j->given, j->partnerTid, &j->received);
}

static void TestJournalRecord(void)
{
    u32 failuresBefore = sFail;
    struct MpTradeJournal j, k, partner;
    struct MpMsgTradeRecover m;
    u8 buf[MP_TJ_FILE_SIZE + 8];
    u8 copy[MP_TJ_FILE_SIZE];
    const char *path = "pokemon-spectrum-selftest.trade";
    u32 i, bad = 0;

    FixtureJournal(&j);
    MpTradeJournal_Serialize(&j, buf);
    T_CHECK(MpTradeJournal_Parse(buf, MP_TJ_FILE_SIZE, &k) == MP_TJ_PARSE_OK && k.tradeId == j.tradeId
            && k.state == j.state && memcmp(k.receivedMon, j.receivedMon, MP_MON_WIRE_SIZE) == 0 && k.receivedFinalSpecies == 65, "journal round trip");
    T_CHECK(j.tradeId != 0, "trade id is never 0");
    // Both sides compute the same id: the partner's given is our received.
    partner = j;
    partner.myTid = j.partnerTid;
    partner.partnerTid = j.myTid;
    partner.given = j.received;
    partner.received = j.given;
    T_CHECK(MpTradeJournal_ComputeId(partner.myTid, &partner.given, partner.partnerTid, &partner.received) == j.tradeId, "both sides get the same trade id");
    T_CHECK(MpTradeJournal_ComputeId(j.myTid, &j.received, j.partnerTid, &j.given) != j.tradeId, "the reverse trade has another id");
    // Sizes, magic, checksum, every field rule.
    T_CHECK(MpTradeJournal_Parse(buf, MP_TJ_FILE_SIZE - 1, &k) == MP_TJ_PARSE_BAD_SIZE, "short journal refused");
    T_CHECK(MpTradeJournal_Parse(buf, MP_TJ_FILE_SIZE + 1, &k) == MP_TJ_PARSE_BAD_SIZE, "long journal refused");
    T_CHECK(MpTradeJournal_Parse(NULL, MP_TJ_FILE_SIZE, &k) == MP_TJ_PARSE_BAD_SIZE, "NULL refused");
    memcpy(copy, buf, MP_TJ_FILE_SIZE);
    copy[0] ^= 1;
    T_CHECK(MpTradeJournal_Parse(copy, MP_TJ_FILE_SIZE, &k) == MP_TJ_PARSE_BAD_MAGIC, "bad magic refused");
    for (i = 6; i < MP_TJ_FILE_SIZE; i++)
    {
        memcpy(copy, buf, MP_TJ_FILE_SIZE);
        copy[i] ^= (u8)(1 << (i % 8));
        if (MpTradeJournal_Parse(copy, MP_TJ_FILE_SIZE, &k) == MP_TJ_PARSE_OK)
            bad++;
    }
    T_CHECK(bad == 0, "every single bit flip is refused");
    k = j; k.state = 3;        MpTradeJournal_Serialize(&k, copy);
    T_CHECK(MpTradeJournal_Parse(copy, MP_TJ_FILE_SIZE, &k) == MP_TJ_PARSE_BAD_FIELD, "bad state refused (valid checksum)");
    k = j; k.partyIndex = 6;   MpTradeJournal_Serialize(&k, copy);
    T_CHECK(MpTradeJournal_Parse(copy, MP_TJ_FILE_SIZE, &k) == MP_TJ_PARSE_BAD_FIELD, "party index 6 refused");
    k = j; k.tradeId ^= 1;     MpTradeJournal_Serialize(&k, copy);
    T_CHECK(MpTradeJournal_Parse(copy, MP_TJ_FILE_SIZE, &k) == MP_TJ_PARSE_BAD_FIELD, "trade id not matching the fields refused");
    k = j; k.receivedMon[0] ^= 1; MpTradeJournal_Serialize(&k, copy);
    T_CHECK(MpTradeJournal_Parse(copy, MP_TJ_FILE_SIZE, &k) == MP_TJ_PARSE_BAD_FIELD, "stored mon that is not the received one refused");
    k = j; k.received.species = 0xFFFF; k.tradeId = j.tradeId; MpTradeJournal_Serialize(&k, copy);
    T_CHECK(MpTradeJournal_Parse(copy, MP_TJ_FILE_SIZE, &k) == MP_TJ_PARSE_BAD_FIELD, "species out of range refused");
    k = j; memset(k.partnerName, 0xBB, sizeof(k.partnerName)); MpTradeJournal_Serialize(&k, copy);
    T_CHECK(MpTradeJournal_Parse(copy, MP_TJ_FILE_SIZE, &k) == MP_TJ_PARSE_BAD_FIELD, "unterminated name refused");
    for (i = 0, bad = 0; i < 20000; i++)
    {
        u32 n;

        memcpy(copy, buf, MP_TJ_FILE_SIZE);
        for (n = Rand() % 6 + 1; n != 0; n--)
            copy[Rand() % MP_TJ_FILE_SIZE] = (u8)Rand();
        if (MpTradeJournal_Parse(copy, MP_TJ_FILE_SIZE, &k) == MP_TJ_PARSE_OK && memcmp(copy, buf, MP_TJ_FILE_SIZE) != 0)
            bad++;
    }
    T_CHECK(bad == 0, "fuzz: no corrupted journal is accepted");
    Report("journal record: round trip, id, sizes, checksum, fields, 20000-corruption fuzz", failuresBefore);

    // Binding of recovery messages.
    failuresBefore = sFail;
    MpTradeJournal_FillMsg(&partner, MP_TJ_STATUS_COMMITTED, 1, 0, &m);
    T_CHECK(MpTradeJournal_IdOfMsg(&m) == j.tradeId && MpTradeJournal_MatchesMsg(&j, &m), "the partner's message matches our journal");
    m.senderTid ^= 1;
    T_CHECK(!MpTradeJournal_MatchesMsg(&j, &m), "another sender trainer id does not match");
    MpTradeJournal_FillMsg(&partner, MP_TJ_STATUS_COMMITTED, 1, 0, &m);
    m.senderGivenPersonality ^= 1;
    T_CHECK(!MpTradeJournal_MatchesMsg(&j, &m) && MpTradeJournal_IdOfMsg(&m) != m.tradeId, "another mon does not match, and the id no longer fits");
    MpTradeJournal_FillMsg(&j, MP_TJ_STATUS_READY, 1, 0, &m);
    T_CHECK(!MpTradeJournal_MatchesMsg(&j, &m), "our own message does not match our journal");
    Report("journal: recovery messages bound to trade id, both trainer ids and both mons", failuresBefore);

    // File round trip (crash-safe write, read, overwrite, delete) in the working directory.
    failuresBefore = sFail;
    Platform_RemoveFile(path);
    T_CHECK(Platform_ReadSmallFile(path, copy, MP_TJ_FILE_SIZE) == -1, "missing journal reads as -1");
    T_CHECK(Platform_WriteFileAtomic(path, buf, MP_TJ_FILE_SIZE), "journal written");
    T_CHECK(Platform_ReadSmallFile(path, copy, MP_TJ_FILE_SIZE) == MP_TJ_FILE_SIZE && memcmp(copy, buf, MP_TJ_FILE_SIZE) == 0, "journal reads back");
    k = j;
    k.state = MP_TJ_STATE_COMMIT;
    MpTradeJournal_Serialize(&k, buf);
    T_CHECK(Platform_WriteFileAtomic(path, buf, MP_TJ_FILE_SIZE), "journal replaced (COMMIT)");
    T_CHECK(Platform_ReadSmallFile(path, copy, MP_TJ_FILE_SIZE) == MP_TJ_FILE_SIZE && MpTradeJournal_Parse(copy, MP_TJ_FILE_SIZE, &k) == MP_TJ_PARSE_OK
            && k.state == MP_TJ_STATE_COMMIT, "replaced journal reads back as COMMIT");
    memset(copy, 0x5A, sizeof(copy));
    T_CHECK(Platform_WriteFileAtomic(path, copy, sizeof(copy)) && Platform_ReadSmallFile(path, buf, MP_TJ_FILE_SIZE - 10) == MP_TJ_FILE_SIZE - 9, "an oversized file reads as cap + 1");
    T_CHECK(Platform_RemoveFile(path) && Platform_ReadSmallFile(path, copy, MP_TJ_FILE_SIZE) == -1, "journal deleted");
    T_CHECK(Platform_RemoveFile(path), "deleting a missing journal is fine");
    T_CHECK(!Platform_WriteFileAtomic("pokemon-spectrum-no-such-dir/x.trade", buf, 4), "write into a missing directory fails cleanly");
    Report("journal file: crash-safe write, read, replace, delete", failuresBefore);
}

static void TestJournalRules(void)
{
    u32 failuresBefore = sFail;
    struct MpTradeCommitHost h;

    // Local status.
    T_CHECK(MpTradeJournal_LocalStatus(MP_TJ_STATE_READY, FALSE, FALSE, FALSE) == MP_TJ_STATUS_READY, "READY journal -> READY");
    T_CHECK(MpTradeJournal_LocalStatus(MP_TJ_STATE_READY, FALSE, TRUE, FALSE) == MP_TJ_STATUS_BUSY, "READY and live -> BUSY");
    T_CHECK(MpTradeJournal_LocalStatus(MP_TJ_STATE_COMMIT, FALSE, TRUE, FALSE) == MP_TJ_STATUS_COMMITTED, "COMMIT journal -> COMMITTED");
    T_CHECK(MpTradeJournal_LocalStatus(MP_TJ_STATE_NONE, FALSE, FALSE, TRUE) == MP_TJ_STATUS_NONE_HAS, "no journal + their mon -> NONE_HAS");
    T_CHECK(MpTradeJournal_LocalStatus(MP_TJ_STATE_NONE, FALSE, FALSE, FALSE) == MP_TJ_STATUS_NONE_HASNOT, "no journal, no mon -> NONE_HASNOT");
    T_CHECK(MpTradeJournal_LocalStatus(MP_TJ_STATE_COMMIT, TRUE, FALSE, TRUE) == MP_TJ_STATUS_DAMAGED, "damaged -> DAMAGED");
    // Resolve: READY.
    T_CHECK(MpTradeJournal_Resolve(MP_TJ_STATE_READY, MP_TJ_STATUS_COMMITTED) == MP_TJ_ACTION_COMMIT, "READY + COMMITTED -> roll forward");
    T_CHECK(MpTradeJournal_Resolve(MP_TJ_STATE_READY, MP_TJ_STATUS_NONE_HAS) == MP_TJ_ACTION_COMMIT, "READY + NONE_HAS -> roll forward");
    T_CHECK(MpTradeJournal_Resolve(MP_TJ_STATE_READY, MP_TJ_STATUS_READY) == MP_TJ_ACTION_ABORT, "READY + READY -> neither");
    T_CHECK(MpTradeJournal_Resolve(MP_TJ_STATE_READY, MP_TJ_STATUS_NONE_HASNOT) == MP_TJ_ACTION_ABORT, "READY + NONE_HASNOT -> neither");
    T_CHECK(MpTradeJournal_Resolve(MP_TJ_STATE_READY, MP_TJ_STATUS_ABORTED) == MP_TJ_ACTION_ABORT, "READY + host ABORTED -> neither");
    T_CHECK(MpTradeJournal_Resolve(MP_TJ_STATE_READY, MP_TJ_STATUS_BUSY) == MP_TJ_ACTION_KEEP, "READY + BUSY -> wait");
    T_CHECK(MpTradeJournal_Resolve(MP_TJ_STATE_READY, MP_TJ_STATUS_DAMAGED) == MP_TJ_ACTION_KEEP, "READY + DAMAGED -> keep (never guess)");
    // Resolve: COMMIT.
    T_CHECK(MpTradeJournal_Resolve(MP_TJ_STATE_COMMIT, MP_TJ_STATUS_COMMITTED) == MP_TJ_ACTION_DONE, "COMMIT + COMMITTED -> done");
    T_CHECK(MpTradeJournal_Resolve(MP_TJ_STATE_COMMIT, MP_TJ_STATUS_NONE_HAS) == MP_TJ_ACTION_DONE, "COMMIT + NONE_HAS -> done");
    T_CHECK(MpTradeJournal_Resolve(MP_TJ_STATE_COMMIT, MP_TJ_STATUS_READY) == MP_TJ_ACTION_KEEP, "COMMIT + READY -> keep answering");
    T_CHECK(MpTradeJournal_Resolve(MP_TJ_STATE_COMMIT, MP_TJ_STATUS_ABORTED) == MP_TJ_ACTION_KEEP, "COMMIT + ABORTED (impossible) -> keep");
    Report("journal rules: local status and resolve table", failuresBefore);

    // Host decision ring and trade-id agreement.
    failuresBefore = sFail;
    memset(&h, 0, sizeof(h));
    ClearSent();
    MpTradeCommit_HostReady(&h, 1, 3, 1, 0xAAAA0001u, TRUE, 2, 10, RecordSend);
    MpTradeCommit_HostReady(&h, 2, 3, 1, 0xAAAA0001u, TRUE, 1, 20, RecordSend);
    T_CHECK(MpTradeCommit_HostLookup(&h, 0xAAAA0001u) == MP_TJ_STATUS_COMMITTED, "ring remembers COMMIT");
    MpTradeCommit_HostReady(&h, 1, 3, 2, 0xBBBB0002u, TRUE, 2, 30, RecordSend);
    MpTradeCommit_HostGroupEnded(&h, 0x06, RecordSend);
    T_CHECK(MpTradeCommit_HostLookup(&h, 0xBBBB0002u) == MP_TJ_STATUS_ABORTED, "ring remembers ABORT");
    T_CHECK(MpTradeCommit_HostLookup(&h, 0xCCCC0003u) == MP_TJ_STATUS_NONE && MpTradeCommit_HostLookup(&h, 0) == MP_TJ_STATUS_NONE, "unknown id -> NONE");
    ClearSent();
    MpTradeCommit_HostReady(&h, 3, 9, 1, 0xDDDD0004u, FALSE, MP_SLOT_NONE, 40, RecordSend);
    T_CHECK(MpTradeCommit_HostLookup(&h, 0xDDDD0004u) == MP_TJ_STATUS_NONE, "a READY outside a group is not remembered");
    memset(&h, 0, sizeof(h));
    ClearSent();
    MpTradeCommit_HostReady(&h, 1, 4, 1, 0x01010101u, TRUE, 2, 10, RecordSend);
    MpTradeCommit_HostReady(&h, 2, 4, 1, 0x02020202u, TRUE, 1, 20, RecordSend);
    T_CHECK(sSentCount == 2 && SentIs(0, 1, MP_TRADE_OP_ABORT, MP_TRADE_ABORT_MISMATCH) && SentIs(1, 2, MP_TRADE_OP_ABORT, MP_TRADE_ABORT_MISMATCH),
            "same round, different trade ids -> ABORT both");
    Report("host decision ring; differing trade ids abort", failuresBefore);
}

// --- Crash at every step --------------------------------------------------------------
//
// Two traders A and B and the host H (a third player, or A itself). The live trade runs in
// steps; each run stops at step k with one party crashing (A, B or H; for H the decisions it
// already sent are delivered or lost). The survivors finish what they can on their own, the
// crashed side restarts from its disk state, and recovery runs with the real rules
// (MpTradeJournal_LocalStatus / _Resolve) and the host's decision ring when the host survived.
// Then: both saves have the trade, or neither, and no journal is left.

enum { SIM_A, SIM_B };
enum { SIM_STEPS = 9 };

struct SimSide
{
    bool8 post;         // the save on disk has the trade
    u8 journal;         // MP_TJ_STATE_*
    u8 inbox;           // live decision delivered but not yet acted on (MP_TRADE_OP_*), 0 none
    bool8 alive;
};

struct Sim
{
    struct SimSide s[2];
    struct MpTradeCommitHost h;
    u8 slot[2];
};

static struct Sim sSim;

static void SimSend(u8 slot, u8 op, u8 groupId, u8 seq, u8 reason)
{
    u8 i = (slot == sSim.slot[SIM_A]) ? SIM_A : SIM_B;

    if (sSim.s[i].alive)
        sSim.s[i].inbox = op;
}

static void SimStep(u32 step)
{
    struct SimSide *a = &sSim.s[SIM_A], *b = &sSim.s[SIM_B];

    switch (step)
    {
    case 0: a->journal = MP_TJ_STATE_READY; break;
    case 1: MpTradeCommit_HostReady(&sSim.h, sSim.slot[SIM_A], 1, 1, 0x7777u, TRUE, sSim.slot[SIM_B], 0, SimSend); break;
    case 2: b->journal = MP_TJ_STATE_READY; break;
    case 3: MpTradeCommit_HostReady(&sSim.h, sSim.slot[SIM_B], 1, 1, 0x7777u, TRUE, sSim.slot[SIM_A], 0, SimSend); break;
    case 4: if (a->inbox == MP_TRADE_OP_COMMIT) { a->journal = MP_TJ_STATE_COMMIT; a->inbox = 0; } break;
    case 5: if (a->journal == MP_TJ_STATE_COMMIT) a->post = TRUE; break;
    case 6: if (b->inbox == MP_TRADE_OP_COMMIT) { b->journal = MP_TJ_STATE_COMMIT; b->inbox = 0; } break;
    case 7: if (b->journal == MP_TJ_STATE_COMMIT) b->post = TRUE; break;
    case 8: // both back in the field: they tell each other COMMITTED -> DONE
        if (a->journal == MP_TJ_STATE_COMMIT && b->journal == MP_TJ_STATE_COMMIT && a->post && b->post)
            a->journal = b->journal = MP_TJ_STATE_NONE;
        break;
    }
}

static void SimAct(struct SimSide *x, u8 action)
{
    switch (action)
    {
    case MP_TJ_ACTION_COMMIT:
        x->journal = MP_TJ_STATE_COMMIT;
        x->post = TRUE;             // rolled forward in the field, then saved
        break;
    case MP_TJ_ACTION_ABORT:
        if (x->post)
        {
            sFail++;
            MP_LOG("selftest: FAIL: trade: sim: a signed save was told to abort\n");
        }
        x->journal = MP_TJ_STATE_NONE;
        break;
    case MP_TJ_ACTION_DONE:
        if (x->post)
            x->journal = MP_TJ_STATE_NONE;
        break;
    }
}

static u8 SimStatus(const struct SimSide *x)
{
    return MpTradeJournal_LocalStatus(x->journal, FALSE, FALSE, x->post);
}

// One recovery message from side i (and the answer), as mp_trade.c and the host do it.
static void SimRecoverFrom(u8 i, bool8 hostRing, bool8 partnerOnline)
{
    struct SimSide *me = &sSim.s[i], *other = &sSim.s[i ^ 1];
    u8 decision = hostRing ? MpTradeCommit_HostLookup(&sSim.h, 0x7777u) : MP_TJ_STATUS_NONE;
    u8 status;

    if (me->journal == MP_TJ_STATE_NONE)
        return;
    status = SimStatus(me);
    if (decision != MP_TJ_STATUS_NONE)
    {
        SimAct(me, MpTradeJournal_Resolve(me->journal, decision));
        if (partnerOnline && other->journal != MP_TJ_STATE_NONE)
            SimAct(other, MpTradeJournal_Resolve(other->journal, decision));
    }
    if (!partnerOnline)
        return;
    if (other->journal != MP_TJ_STATE_NONE)
        SimAct(other, MpTradeJournal_Resolve(other->journal, status));
    if (me->journal != MP_TJ_STATE_NONE)
        SimAct(me, MpTradeJournal_Resolve(me->journal, SimStatus(other)));
}

// Returns FALSE (and logs) if the run ends inconsistent.
static bool8 SimRun(bool8 hostIsA, u32 crashStep, u8 crashed, bool8 inFlightLost, bool8 partnerReturns)
{
    bool8 hostAlive = TRUE;
    bool8 signedLive;
    u32 step, round;
    u8 i;

    memset(&sSim, 0, sizeof(sSim));
    sSim.slot[SIM_A] = hostIsA ? 0 : 1;
    sSim.slot[SIM_B] = hostIsA ? 1 : 2;
    sSim.s[SIM_A].alive = sSim.s[SIM_B].alive = TRUE;
    for (step = 0; step < crashStep && step < SIM_STEPS; step++)
        SimStep(step);

    // The crash. crashed: 0 = A, 1 = B, 2 = the host (A itself when hostIsA).
    if (crashStep < SIM_STEPS)
    {
        bool8 hostDies = (crashed == 2) || (hostIsA && crashed == SIM_A);

        if (crashed == SIM_A || crashed == SIM_B || (hostIsA && crashed == 2))
        {
            u8 dead = (crashed == 2) ? SIM_A : crashed;

            sSim.s[dead].alive = FALSE;
            sSim.s[dead].inbox = 0;
        }
        if (hostDies)
        {
            hostAlive = FALSE;
            for (i = 0; i < 2; i++)
            {
                if (inFlightLost)
                    sSim.s[i].inbox = 0;
            }
        }
        else
        {
            // The dead trader's slot leaves: its group ends.
            MpTradeCommit_HostGroupEnded(&sSim.h, (u8)((1 << sSim.slot[SIM_A]) | (1 << sSim.slot[SIM_B])), SimSend);
        }
        // Survivors finish on their own (trade.c state 43 and on).
        for (i = 0; i < 2; i++)
        {
            struct SimSide *x = &sSim.s[i];

            if (!x->alive)
                continue;
            if (x->inbox == MP_TRADE_OP_COMMIT && x->journal == MP_TJ_STATE_READY)
                x->journal = MP_TJ_STATE_COMMIT;
            else if (x->inbox == MP_TRADE_OP_ABORT && x->journal == MP_TJ_STATE_READY)
                x->journal = MP_TJ_STATE_NONE;  // final ABORT: journal deleted
            x->inbox = 0;
            if (x->journal == MP_TJ_STATE_COMMIT)
                x->post = TRUE;                 // signs
            // else READY with no decision: IN_DOUBT, kept
        }
    }
    signedLive = sSim.s[SIM_A].post || sSim.s[SIM_B].post;

    // Restart and load: a COMMIT journal whose save lacks the trade rolls forward.
    for (i = 0; i < 2; i++)
    {
        sSim.s[i].alive = TRUE;
        if (sSim.s[i].journal == MP_TJ_STATE_COMMIT)
            sSim.s[i].post = TRUE;
    }
    if (!hostAlive || hostIsA)
        memset(&sSim.h, 0, sizeof(sSim.h)); // a new host session: no decision ring (host A restarted)
    if (!partnerReturns)
    {
        // Only the crashed side is back: it may learn from the host, never guess.
        u8 me = (crashed == SIM_B) ? SIM_B : SIM_A;
        u8 before = sSim.s[me].journal;

        for (round = 0; round < 5; round++)
            SimRecoverFrom(me, hostAlive && !hostIsA, FALSE);
        if (before != MP_TJ_STATE_NONE && sSim.s[me].journal == MP_TJ_STATE_NONE && !(hostAlive && !hostIsA))
        {
            MP_LOG("selftest: FAIL: trade: sim: journal dropped without the partner or the host\n");
            return FALSE;
        }
        return TRUE;
    }
    for (round = 0; round < 6; round++)
    {
        SimRecoverFrom(SIM_A, hostAlive && !hostIsA, TRUE);
        SimRecoverFrom(SIM_B, hostAlive && !hostIsA, TRUE);
    }
    if (sSim.s[SIM_A].post != sSim.s[SIM_B].post || sSim.s[SIM_A].journal != MP_TJ_STATE_NONE || sSim.s[SIM_B].journal != MP_TJ_STATE_NONE
     || (signedLive && !sSim.s[SIM_A].post))
    {
        MP_LOG("selftest: FAIL: trade: sim: host%s crash %s before step %u%s: A %s/%u, B %s/%u\n", hostIsA ? "=A" : "",
               (crashed == 2) ? "host" : (crashed == SIM_A ? "A" : "B"), (unsigned)crashStep, inFlightLost ? " (in-flight lost)" : "",
               sSim.s[SIM_A].post ? "post" : "pre", (unsigned)sSim.s[SIM_A].journal,
               sSim.s[SIM_B].post ? "post" : "pre", (unsigned)sSim.s[SIM_B].journal);
        return FALSE;
    }
    return TRUE;
}

static void TestJournalCrashes(void)
{
    u32 failuresBefore = sFail;
    u32 runs = 0, step;
    u8 hostIsA, crashed, lost;

    for (hostIsA = 0; hostIsA < 2; hostIsA++)
        for (step = 0; step <= SIM_STEPS; step++)
            for (crashed = 0; crashed < 3; crashed++)
                for (lost = 0; lost < 2; lost++)
                {
                    runs++;
                    if (!SimRun(hostIsA, step, crashed, lost, TRUE))
                        sFail++;
                }
    T_CHECK(runs == 2 * (SIM_STEPS + 1) * 3 * 2, "every combination ran");
    Report("journal: crash at every step (host and client, 120 runs) -> both saves agree, no journal left", failuresBefore);

    failuresBefore = sFail;
    for (hostIsA = 0; hostIsA < 2; hostIsA++)
        for (step = 0; step < SIM_STEPS; step++)
            for (crashed = 0; crashed < 3; crashed++)
                if (!SimRun(hostIsA, step, crashed, FALSE, FALSE))
                    sFail++;
    Report("journal: partner never returns -> the journal is kept unless the host decided", failuresBefore);
}

u32 MpSelftest_Trade(void)
{
    sFail = 0;
    TestParser();
    TestPureLogic();
    TestPureFuzz();
    TestInProcess();
    // PS-050 trade journal
    TestJournalRecord();
    TestJournalRules();
    TestJournalCrashes();
    return sFail;
}
