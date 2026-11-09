// Pokemon Spectrum multiplayer selftest: PS-045 direct link battle.
//
// 1. The Cable Club's party rules (MP_BattleCheckParty) and the kind -> service mapping.
// 2. INTERACT with the new kind 5 (BATTLE_DOUBLE, protocol 8): every kind byte 0..255 on all
//    three INTERACT types, round trips, and a random-buffer fuzz.
// 3. The start flow's linkup state machine (MP_BattleFlowStep) as a pure function: the happy
//    path for the master and the slave, every failure (no group, timeouts, the group or the
//    partner or the session gone, a single/double mismatch), and a random-input fuzz of its
//    invariants.
// 4. In process, over the real session and link groups: two clients ask for the group, the
//    flow reaches OPEN_LINK on both; then the partner leaves the group (a simulated group end)
//    or the session, and the flow fails on the other side, which is free to group again.
// The battle itself needs two real windows (card PS-045, manual test).

#include <string.h>
#include "global.h"
#include "link.h"
#include "constants/cable_club.h"
#include "constants/species.h"
#include "mp/mp.h"
#include "mp/mp_battle.h"
#include "mp/mp_internal.h"
#include "mp/mp_link.h"
#include "mp/mp_protocol.h"
#include "mp/mp_selftest.h"
#include "mp/mp_session_internal.h"
#include "mp/mp_transport.h"

static u32 sFail;

#define B_CHECK(cond, what)                                                           \
    do                                                                                \
    {                                                                                 \
        if (!(cond))                                                                  \
        {                                                                             \
            MP_LOG("selftest: FAIL: battle: %s (%s:%d)\n", what, __FILE__, __LINE__); \
            sFail++;                                                                  \
        }                                                                             \
    } while (0)

static void Report(const char *what, u32 failuresBefore)
{
    if (sFail == failuresBefore)
        MP_LOG("selftest: battle: %s OK\n", what);
    else
        MP_LOG("selftest: battle: %s FAILED (%u)\n", what, (unsigned)(sFail - failuresBefore));
}

static u32 sRng = 0x42544C45u; // "BTLE"

static u32 Rand(void)
{
    sRng ^= sRng << 13;
    sRng ^= sRng >> 17;
    sRng ^= sRng << 5;
    return sRng;
}

// ===========================================================================
// 1. Party rules
// ===========================================================================

static u8 Party(bool8 isDouble, u16 s0, u16 h0, u16 s1, u16 h1, u16 s2, u16 h2)
{
    u16 species[PARTY_SIZE] = { s0, s1, s2, SPECIES_NONE, SPECIES_NONE, SPECIES_NONE };
    u16 hp[PARTY_SIZE] = { h0, h1, h2, 0, 0, 0 };

    return MP_BattleCheckParty(species, hp, PARTY_SIZE, isDouble);
}

static void TestPartyRules(void)
{
    u32 failuresBefore = sFail;
    u16 species[PARTY_SIZE] = {0};
    u16 hp[PARTY_SIZE] = {0};
    u32 i, n, mons, usable;
    u8 r;

    B_CHECK(Party(FALSE, SPECIES_NONE, 0, SPECIES_NONE, 0, SPECIES_NONE, 0) == MP_BATTLE_PARTY_NO_MONS, "single: empty party");
    B_CHECK(Party(FALSE, SPECIES_EGG, 10, SPECIES_EGG, 10, SPECIES_NONE, 0) == MP_BATTLE_PARTY_NO_MONS, "single: only eggs");
    B_CHECK(Party(FALSE, SPECIES_TREECKO, 20, SPECIES_NONE, 0, SPECIES_NONE, 0) == MP_BATTLE_PARTY_OK, "single: one mon");
    B_CHECK(Party(FALSE, SPECIES_TREECKO, 0, SPECIES_NONE, 0, SPECIES_NONE, 0) == MP_BATTLE_PARTY_OK, "single: one fainted mon (healed for the battle)");
    B_CHECK(Party(FALSE, SPECIES_EGG, 10, SPECIES_TORCHIC, 1, SPECIES_NONE, 0) == MP_BATTLE_PARTY_OK, "single: egg + mon");
    B_CHECK(Party(TRUE, SPECIES_TREECKO, 20, SPECIES_NONE, 0, SPECIES_NONE, 0) == MP_BATTLE_PARTY_NEED_TWO, "double: one mon");
    B_CHECK(Party(TRUE, SPECIES_TREECKO, 20, SPECIES_EGG, 10, SPECIES_NONE, 0) == MP_BATTLE_PARTY_NEED_TWO, "double: mon + egg");
    B_CHECK(Party(TRUE, SPECIES_TREECKO, 20, SPECIES_TORCHIC, 0, SPECIES_NONE, 0) == MP_BATTLE_PARTY_NEED_TWO, "double: second mon fainted (Cable Club rule)");
    B_CHECK(Party(TRUE, SPECIES_TREECKO, 20, SPECIES_TORCHIC, 1, SPECIES_NONE, 0) == MP_BATTLE_PARTY_OK, "double: two usable");
    B_CHECK(Party(TRUE, SPECIES_EGG, 10, SPECIES_TREECKO, 0, SPECIES_TORCHIC, 3) == MP_BATTLE_PARTY_NEED_TWO, "double: egg, fainted, one usable");
    B_CHECK(Party(TRUE, SPECIES_NONE, 0, SPECIES_NONE, 0, SPECIES_NONE, 0) == MP_BATTLE_PARTY_NO_MONS, "double: empty party");
    B_CHECK(MP_BattleCheckParty(species, hp, 0, FALSE) == MP_BATTLE_PARTY_NO_MONS, "count 0");

    // Random parties against a reference count; a count above PARTY_SIZE is clamped.
    for (n = 0; n < 20000; n++)
    {
        mons = usable = 0;
        for (i = 0; i < PARTY_SIZE; i++)
        {
            switch (Rand() % 4)
            {
            case 0:  species[i] = SPECIES_NONE; break;
            case 1:  species[i] = SPECIES_EGG; break;
            default: species[i] = (u16)(1 + Rand() % (SPECIES_EGG - 1)); break;
            }
            hp[i] = (Rand() & 1) ? (u16)(Rand() % 999) : 0;
            if (species[i] != SPECIES_NONE && species[i] != SPECIES_EGG)
            {
                mons++;
                usable += (hp[i] != 0);
            }
        }
        r = MP_BattleCheckParty(species, hp, PARTY_SIZE + (Rand() % 3), Rand() & 1);
        B_CHECK(r == MP_BATTLE_PARTY_OK || r == MP_BATTLE_PARTY_NO_MONS || r == MP_BATTLE_PARTY_NEED_TWO, "party fuzz: result in range");
        B_CHECK(MP_BattleCheckParty(species, hp, PARTY_SIZE, FALSE) == (mons ? MP_BATTLE_PARTY_OK : MP_BATTLE_PARTY_NO_MONS), "party fuzz: single rule");
        B_CHECK(MP_BattleCheckParty(species, hp, PARTY_SIZE, TRUE)
                == (mons == 0 ? MP_BATTLE_PARTY_NO_MONS : (usable >= 2 ? MP_BATTLE_PARTY_OK : MP_BATTLE_PARTY_NEED_TWO)), "party fuzz: double rule");
    }

    B_CHECK(MP_BattleServiceForKind(MP_INTERACT_BATTLE) == USING_SINGLE_BATTLE, "BATTLE -> single");
    B_CHECK(MP_BattleServiceForKind(MP_INTERACT_BATTLE_DOUBLE) == USING_DOUBLE_BATTLE, "BATTLE_DOUBLE -> double");
    for (i = 0; i < 256; i++)
    {
        bool8 battle = (i == MP_INTERACT_BATTLE || i == MP_INTERACT_BATTLE_DOUBLE);

        B_CHECK(MP_IsBattleKind((u8)i) == battle, "only the two battle kinds are battles");
        if (!battle)
            B_CHECK(MP_BattleServiceForKind((u8)i) == 0, "no service for other kinds");
    }
    Report("party rules and kinds", failuresBefore);
}

// ===========================================================================
// 2. INTERACT with kind BATTLE_DOUBLE
// ===========================================================================

static void TestInteractKind(void)
{
    static const u8 sTypes[3] = { MP_MSG_INTERACT_REQUEST, MP_MSG_INTERACT_RESPONSE, MP_MSG_INTERACT_CANCEL };
    u8 buf[MP_MAX_PACKET_SIZE + 8];
    struct MpMsgInteract in, out;
    u32 failuresBefore = sFail;
    u32 t, k, n, i;
    u16 len;
    u8 r;

    B_CHECK(MP_PROTOCOL_VERSION >= 8, "protocol version 8 or later (PS-045; 9 since PS-051)");
    B_CHECK(MP_INTERACT_BATTLE_DOUBLE == 5 && MP_INTERACT_KIND_COUNT == 6, "kind 5 is BATTLE_DOUBLE");
    for (t = 0; t < 3; t++)
    {
        memset(&in, 0, sizeof(in));
        in.slot = 1;
        in.epoch = 3;
        in.kind = MP_INTERACT_BATTLE_DOUBLE;
        in.requestId = 0x5C;
        in.result = (sTypes[t] == MP_MSG_INTERACT_RESPONSE) ? MP_INTERACT_RESULT_ACCEPTED : 0;
        len = MpProto_WriteInteract(buf, MP_MAX_PACKET_SIZE, sTypes[t], &in);
        B_CHECK(len == MP_HEADER_SIZE + MP_INTERACT_SIZE, "BATTLE_DOUBLE write size");
        B_CHECK(MpProto_ParseInteract(sTypes[t], buf + MP_HEADER_SIZE, MP_INTERACT_SIZE, &out) == MP_PARSE_OK
             && out.kind == MP_INTERACT_BATTLE_DOUBLE && out.slot == 1 && out.epoch == 3 && out.requestId == 0x5C, "BATTLE_DOUBLE round trip");
        // Every kind byte: 1..5 parse, 0 and 6..255 are BAD_FIELD.
        for (k = 0; k < 256; k++)
        {
            buf[MP_HEADER_SIZE + 2] = (u8)k;
            r = MpProto_ParseInteract(sTypes[t], buf + MP_HEADER_SIZE, MP_INTERACT_SIZE, &out);
            if (k >= 1 && k < MP_INTERACT_KIND_COUNT)
                B_CHECK(r == MP_PARSE_OK && out.kind == k, "kinds 1..5 accepted");
            else
                B_CHECK(r == MP_PARSE_ERR_BAD_FIELD, "kind 0 / 6..255 rejected");
        }
    }
    // Random payloads: never crashes, and whatever parses has a kind in range.
    for (n = 0; n < 40000; n++)
    {
        u32 size = Rand() % (MP_INTERACT_SIZE + 4);

        for (i = 0; i < size; i++)
            buf[i] = (u8)Rand();
        if (Rand() & 1)
            buf[2] = (u8)(MP_INTERACT_BATTLE_DOUBLE + (Rand() % 3) - 1);
        r = MpProto_ParseInteract(sTypes[Rand() % 3], buf, size, &out);
        if (r == MP_PARSE_OK)
            B_CHECK(size == MP_INTERACT_SIZE && out.kind >= 1 && out.kind < MP_INTERACT_KIND_COUNT && out.slot < MP_MAX_PLAYERS, "fuzz: parsed fields in range");
    }
    Report("INTERACT kind BATTLE_DOUBLE (parser, every kind byte, fuzz)", failuresBefore);
}

// ===========================================================================
// 3. The linkup state machine (pure)
// ===========================================================================

static void In(struct MpBattleFlowInput *in, u8 linkState, u8 players, bool8 master, u8 exchange)
{
    in->sessionActive = TRUE;
    in->partnerPresent = TRUE;
    in->linkState = linkState;
    in->linkPlayerCount = players;
    in->isLinkMaster = master;
    in->exchangeStatus = exchange;
}

// The happy path, as the link master or the slave.
static void HappyPath(bool8 master)
{
    struct MpBattleFlow f;
    struct MpBattleFlowInput in;
    u32 i, advances = 0;
    u8 act;

    MP_BattleFlowInit(&f);
    B_CHECK(f.state == MP_BATTLE_FLOW_WAIT_GROUP, "init: WAIT_GROUP");
    In(&in, MP_LINK_STATE_REQUESTED, 0, master, EXCHANGE_NOT_STARTED);
    for (i = 0; i < 100; i++)
        B_CHECK(MP_BattleFlowStep(&f, &in) == MP_BATTLE_ACT_NONE, "requesting: wait");
    In(&in, MP_LINK_STATE_GROUPED, 0, master, EXCHANGE_NOT_STARTED);
    B_CHECK(MP_BattleFlowStep(&f, &in) == MP_BATTLE_ACT_OPEN_LINK && f.state == MP_BATTLE_FLOW_WAIT_PLAYERS, "grouped: open the link");
    In(&in, MP_LINK_STATE_GROUPED, 1, master, EXCHANGE_NOT_STARTED);
    for (i = 0; i < 10; i++)
        B_CHECK(MP_BattleFlowStep(&f, &in) == MP_BATTLE_ACT_NONE, "one player on the cable: wait");
    In(&in, MP_LINK_STATE_GROUPED, 2, master, EXCHANGE_NOT_STARTED);
    for (i = 0; i < 200 && f.state == MP_BATTLE_FLOW_WAIT_PLAYERS; i++)
    {
        act = MP_BattleFlowStep(&f, &in);
        if (act == MP_BATTLE_ACT_ADVANCE_LINK)
            advances++;
        else
            B_CHECK(act == MP_BATTLE_ACT_NONE, "two players: no other action");
    }
    B_CHECK(f.state == MP_BATTLE_FLOW_WAIT_EXCHANGE, "two players: WAIT_EXCHANGE");
    B_CHECK(advances == (master ? 1u : 0u), "only the master advances the handshake, once");
    // The slave moves on at once; the master once MP_BATTLE_MASTER_DELAY_FRAMES have passed
    // since the link was opened (10 of them with one player).
    B_CHECK(i == (master ? (u32)(MP_BATTLE_MASTER_DELAY_FRAMES - 10) : 1u), "the master waits its delay first, the slave doesn't");
    for (i = 0; i < 20; i++)
        B_CHECK(MP_BattleFlowStep(&f, &in) == MP_BATTLE_ACT_NONE, "exchange not started: wait");
    In(&in, MP_LINK_STATE_GROUPED, 2, master, EXCHANGE_COMPLETE);
    B_CHECK(MP_BattleFlowStep(&f, &in) == MP_BATTLE_ACT_LINKED && f.state == MP_BATTLE_FLOW_LINKED, "exchange complete: LINKED");
    // Final: nothing else happens, even with bad inputs.
    In(&in, MP_LINK_STATE_NONE, 0, master, EXCHANGE_TIMED_OUT);
    in.sessionActive = FALSE;
    B_CHECK(MP_BattleFlowStep(&f, &in) == MP_BATTLE_ACT_NONE && f.state == MP_BATTLE_FLOW_LINKED, "LINKED is final");
}

// Drives a fresh flow to `state` with good inputs.
static void FlowTo(struct MpBattleFlow *f, u8 state)
{
    struct MpBattleFlowInput in;
    u32 i;

    MP_BattleFlowInit(f);
    if (state == MP_BATTLE_FLOW_WAIT_GROUP)
        return;
    In(&in, MP_LINK_STATE_GROUPED, 2, FALSE, EXCHANGE_NOT_STARTED);
    MP_BattleFlowStep(f, &in);
    for (i = 0; i < 5 && f->state != state; i++)
        MP_BattleFlowStep(f, &in);
}

static bool8 FailsWith(u8 state, u8 linkState, u8 players, u8 exchange, bool8 session, bool8 partner)
{
    struct MpBattleFlow f;
    struct MpBattleFlowInput in;
    u8 act;

    FlowTo(&f, state);
    if (f.state != state)
        return FALSE;
    In(&in, linkState, players, FALSE, exchange);
    in.sessionActive = session;
    in.partnerPresent = partner;
    act = MP_BattleFlowStep(&f, &in);
    if (act != MP_BATTLE_ACT_FAIL || f.state != MP_BATTLE_FLOW_FAILED)
        return FALSE;
    In(&in, MP_LINK_STATE_GROUPED, 2, FALSE, EXCHANGE_COMPLETE);
    return MP_BattleFlowStep(&f, &in) == MP_BATTLE_ACT_NONE && f.state == MP_BATTLE_FLOW_FAILED; // final
}

static bool8 TimesOut(u8 state, u8 linkState, u8 players, u32 limit)
{
    struct MpBattleFlow f;
    struct MpBattleFlowInput in;
    u32 i;

    FlowTo(&f, state);
    In(&in, linkState, players, FALSE, EXCHANGE_NOT_STARTED);
    for (i = 0; i < limit; i++)
    {
        if (MP_BattleFlowStep(&f, &in) != MP_BATTLE_ACT_NONE)
            return FALSE; // too early
    }
    return MP_BattleFlowStep(&f, &in) == MP_BATTLE_ACT_FAIL;
}

static void TestFlow(void)
{
    u32 failuresBefore = sFail;
    struct MpBattleFlow f;
    struct MpBattleFlowInput in;
    u32 n, opens, linked;
    u8 act, prev;
    static const u8 sStates[3] = { MP_BATTLE_FLOW_WAIT_GROUP, MP_BATTLE_FLOW_WAIT_PLAYERS, MP_BATTLE_FLOW_WAIT_EXCHANGE };
    u32 s;

    HappyPath(FALSE);
    HappyPath(TRUE);

    B_CHECK(FailsWith(MP_BATTLE_FLOW_WAIT_GROUP, MP_LINK_STATE_NONE, 0, EXCHANGE_NOT_STARTED, TRUE, TRUE), "group refused / ended before it formed");
    B_CHECK(FailsWith(MP_BATTLE_FLOW_WAIT_PLAYERS, MP_LINK_STATE_NONE, 1, EXCHANGE_NOT_STARTED, TRUE, TRUE), "group ended while waiting for the players");
    B_CHECK(FailsWith(MP_BATTLE_FLOW_WAIT_EXCHANGE, MP_LINK_STATE_NONE, 2, EXCHANGE_NOT_STARTED, TRUE, TRUE), "group ended during the exchange");
    B_CHECK(FailsWith(MP_BATTLE_FLOW_WAIT_EXCHANGE, MP_LINK_STATE_GROUPED, 2, EXCHANGE_DIFF_SELECTIONS, TRUE, TRUE), "single vs double: DIFF_SELECTIONS fails");
    B_CHECK(FailsWith(MP_BATTLE_FLOW_WAIT_EXCHANGE, MP_LINK_STATE_GROUPED, 2, EXCHANGE_TIMED_OUT, TRUE, TRUE), "exchange timed out");
    B_CHECK(FailsWith(MP_BATTLE_FLOW_WAIT_EXCHANGE, MP_LINK_STATE_GROUPED, 2, EXCHANGE_WRONG_NUM_PLAYERS, TRUE, TRUE), "wrong number of players");
    for (s = 0; s < 3; s++)
    {
        B_CHECK(FailsWith(sStates[s], MP_LINK_STATE_GROUPED, 2, EXCHANGE_COMPLETE, FALSE, TRUE), "session ended");
        B_CHECK(FailsWith(sStates[s], MP_LINK_STATE_GROUPED, 2, EXCHANGE_COMPLETE, TRUE, FALSE), "partner left the session");
    }
    B_CHECK(TimesOut(MP_BATTLE_FLOW_WAIT_GROUP, MP_LINK_STATE_REQUESTED, 0, MP_BATTLE_GROUP_TIMEOUT_FRAMES), "the group never forms: timeout");
    B_CHECK(TimesOut(MP_BATTLE_FLOW_WAIT_PLAYERS, MP_LINK_STATE_GROUPED, 1, MP_BATTLE_LINKUP_TIMEOUT_FRAMES), "the partner never opens: timeout");
    B_CHECK(TimesOut(MP_BATTLE_FLOW_WAIT_EXCHANGE, MP_LINK_STATE_GROUPED, 2, MP_BATTLE_LINKUP_TIMEOUT_FRAMES), "the exchange never ends: timeout");
    Report("linkup state machine", failuresBefore);

    // Random inputs. Invariants: OPEN_LINK at most once and before anything else; LINKED only
    // once, only after OPEN_LINK, only with GROUPED + COMPLETE + session + partner; after
    // LINKED / FAILED nothing; FAIL only from a waiting state.
    failuresBefore = sFail;
    for (n = 0; n < 2000; n++)
    {
        u32 step;

        MP_BattleFlowInit(&f);
        opens = linked = 0;
        for (step = 0; step < 400; step++)
        {
            u32 r = Rand();

            in.sessionActive = (r & 0x3F) != 0;
            in.partnerPresent = (r & 0xFC0) != 0;
            in.linkState = (u8)((r >> 12) % 4);
            in.linkPlayerCount = (u8)((r >> 14) % 4);
            in.isLinkMaster = (r >> 16) & 1;
            in.exchangeStatus = (u8)((r >> 17) % (EXCHANGE_STAT_7 + 1));
            if ((r >> 20) & 1)
                in.exchangeStatus = EXCHANGE_NOT_STARTED;
            prev = f.state;
            act = MP_BattleFlowStep(&f, &in);
            switch (act)
            {
            case MP_BATTLE_ACT_OPEN_LINK:
                opens++;
                B_CHECK(prev == MP_BATTLE_FLOW_WAIT_GROUP && in.linkState == MP_LINK_STATE_GROUPED, "fuzz: OPEN_LINK only from WAIT_GROUP when grouped");
                break;
            case MP_BATTLE_ACT_ADVANCE_LINK:
                B_CHECK(prev == MP_BATTLE_FLOW_WAIT_PLAYERS && in.isLinkMaster && in.linkPlayerCount >= 2, "fuzz: ADVANCE only by the master with two players");
                break;
            case MP_BATTLE_ACT_LINKED:
                linked++;
                B_CHECK(prev == MP_BATTLE_FLOW_WAIT_EXCHANGE && opens == 1 && in.linkState == MP_LINK_STATE_GROUPED
                     && in.exchangeStatus == EXCHANGE_COMPLETE && in.sessionActive && in.partnerPresent, "fuzz: LINKED only when everything is right");
                break;
            case MP_BATTLE_ACT_FAIL:
                B_CHECK(prev <= MP_BATTLE_FLOW_WAIT_EXCHANGE && f.state == MP_BATTLE_FLOW_FAILED, "fuzz: FAIL only from a waiting state");
                break;
            default:
                B_CHECK(act == MP_BATTLE_ACT_NONE, "fuzz: action in range");
                break;
            }
            if (prev == MP_BATTLE_FLOW_LINKED || prev == MP_BATTLE_FLOW_FAILED)
                B_CHECK(act == MP_BATTLE_ACT_NONE && f.state == prev, "fuzz: LINKED / FAILED are final");
            B_CHECK(f.state <= MP_BATTLE_FLOW_FAILED, "fuzz: state in range");
        }
        B_CHECK(opens <= 1 && linked <= 1, "fuzz: OPEN_LINK and LINKED at most once");
    }
    Report("linkup state machine fuzz (2000 x 400 random inputs)", failuresBefore);
}

// ===========================================================================
// 4. In process: real link groups, then a group end / a session leave
// ===========================================================================

#define BATTLE_PORT_FIRST   27520
#define BATTLE_PORT_LAST    27529

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

static bool8 CondInSession(void) { return MP_GetState() == MP_STATE_IN_SESSION; }
static bool8 CondUngrouped(void) { return MP_LinkGetState() == MP_LINK_STATE_NONE; }

static bool8 JoinAs(u8 instance, const char *name)
{
    Sel(instance);
    if (!MP_SetNameOverride(name))
        return FALSE;
    return MP_Join("127.0.0.1", sPort) && WaitFor(instance, CondInSession, 5000);
}

// One flow step for `instance` with its real session / link state (partner = `partnerSlot`).
// The cable itself is not opened here (no game), so the player count is given.
static u8 StepReal(u8 instance, struct MpBattleFlow *f, u8 partnerSlot, u8 players)
{
    struct MpBattleFlowInput in;

    Sel(instance);
    in.sessionActive = MP_IsActive();
    in.partnerPresent = MP_GetPlayer(partnerSlot) != NULL;
    in.linkState = MP_LinkGetState();
    in.linkPlayerCount = players;
    in.isLinkMaster = (MP_LinkGetLinkId() == 0);
    in.exchangeStatus = EXCHANGE_NOT_STARTED;
    return MP_BattleFlowStep(f, &in);
}

// Both ask for the group and step their flows until both said OPEN_LINK (or a failure).
static bool8 GroupBoth(struct MpBattleFlow *a, struct MpBattleFlow *b)
{
    u8 mask = (u8)((1 << S_C1) | (1 << S_C2));
    u32 start = MpTransport_GetTimeMs();
    u8 actA = MP_BATTLE_ACT_NONE, actB = MP_BATTLE_ACT_NONE;

    MP_BattleFlowInit(a);
    MP_BattleFlowInit(b);
    Sel(S_C1);
    if (!MP_LinkJoin(mask))
        return FALSE;
    Sel(S_C2);
    if (!MP_LinkJoin(mask))
        return FALSE;
    while ((u32)(MpTransport_GetTimeMs() - start) < 3000 && (a->state == MP_BATTLE_FLOW_WAIT_GROUP || b->state == MP_BATTLE_FLOW_WAIT_GROUP))
    {
        PumpOnce();
        if (a->state == MP_BATTLE_FLOW_WAIT_GROUP)
            actA = StepReal(S_C1, a, S_C2, 0);
        if (b->state == MP_BATTLE_FLOW_WAIT_GROUP)
            actB = StepReal(S_C2, b, S_C1, 0);
        if (actA == MP_BATTLE_ACT_FAIL || actB == MP_BATTLE_ACT_FAIL)
            return FALSE;
    }
    return actA == MP_BATTLE_ACT_OPEN_LINK && actB == MP_BATTLE_ACT_OPEN_LINK
        && a->state == MP_BATTLE_FLOW_WAIT_PLAYERS && b->state == MP_BATTLE_FLOW_WAIT_PLAYERS;
}

// Steps `f` (on `instance`) until it fails; FALSE if it doesn't within `timeoutMs`.
static bool8 FailsSoon(u8 instance, struct MpBattleFlow *f, u8 partnerSlot, u32 timeoutMs)
{
    u32 start = MpTransport_GetTimeMs();

    while ((u32)(MpTransport_GetTimeMs() - start) < timeoutMs)
    {
        PumpOnce();
        if (StepReal(instance, f, partnerSlot, 1) == MP_BATTLE_ACT_FAIL)
            return TRUE;
        if (f->state != MP_BATTLE_FLOW_WAIT_PLAYERS)
            return FALSE;
    }
    return FALSE;
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
    struct MpBattleFlow a, b;
    bool8 hosting = FALSE;
    u8 i;

    for (i = 0; i < S_COUNT; i++)
        MpSession_ResetInstance(i);
    Sel(S_HOST);
    MP_SetNameOverride("Host");
    for (sPort = BATTLE_PORT_FIRST; sPort <= BATTLE_PORT_LAST && !hosting; sPort++)
        hosting = MP_Host(sPort);
    sPort--;
    B_CHECK(hosting, "battle host on a port in 27520..27529");
    if (!hosting)
        return;
    B_CHECK(JoinAs(S_C1, "BtlA"), "client 1 joins");
    B_CHECK(JoinAs(S_C2, "BtlB"), "client 2 joins");
    Report("session for the battle flow tests", failuresBefore);

    // Grouping: both flows reach OPEN_LINK; time controls are locked while grouped (PS-048).
    failuresBefore = sFail;
    B_CHECK(GroupBoth(&a, &b), "both flows: GROUPED -> OPEN_LINK");
    Sel(S_C1); B_CHECK(MP_LinkTimeControlsLocked(), "C1: time controls locked while grouped");
    Sel(S_C2); B_CHECK(MP_LinkTimeControlsLocked(), "C2: time controls locked while grouped");
    // Simulated group end: C2 leaves the group; C1's flow fails, and C1 is free again.
    Sel(S_C2);
    MP_LinkLeave();
    B_CHECK(FailsSoon(S_C1, &a, S_C2, 3000), "C1: the group ends -> the flow fails");
    B_CHECK(WaitFor(S_C1, CondUngrouped, 3000), "C1: back to no group");
    Sel(S_C1); B_CHECK(!MP_LinkTimeControlsLocked(), "C1: time controls unlocked again");
    B_CHECK(WaitFor(S_C2, CondUngrouped, 3000), "C2: back to no group");
    Report("battle flow: grouping, then a group end", failuresBefore);

    // Again (both can group again), then C2 leaves the session.
    failuresBefore = sFail;
    B_CHECK(GroupBoth(&a, &b), "regroup after the failed start");
    Sel(S_C2);
    MP_Leave();
    B_CHECK(FailsSoon(S_C1, &a, S_C2, 5000), "C1: the partner leaves the session -> the flow fails");
    B_CHECK(WaitFor(S_C1, CondUngrouped, 3000), "C1: the group is gone");
    Report("battle flow: the partner leaves the session", failuresBefore);

    CleanUp();
}

u32 MpSelftest_Battle(void)
{
    sFail = 0;
    TestPartyRules();
    TestInteractKind();
    TestFlow();
    TestInProcess();
    return sFail;
}
