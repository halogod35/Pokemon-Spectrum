// --mp-selftest, PS-046 group: the virtual link cable.
//
//  1. LINK_* parser fuzzing (protocol.md 8.3 rules): round trip, truncated, oversized, bit
//     flips, garbage, targeted cases.
//  2. In-process scenarios with the REAL link stack (src/link.c: OpenLink, the player-data
//     exchange, SendBlock, SetCloseLinkCallback, the link-error path) running as two "GBAs"
//     in one process. MP_LinkSelftestState() (link.c) saves and restores link.c's whole state
//     per game instance; each game instance has its own fixture save blocks and session
//     instance, and talks to the others through the real session + loopback ENet.
//     A: the host only sequences (it is not a link member), two clients link.
//     B: the host is a link member.
//     Covered: grouping, link ids / GetMultiplayerId, the player-data exchange (gLinkPlayers on
//     both sides), SendBlock both ways, a stalled member catching up (the 50-row report cap),
//     close + re-open on the same group (new bus), disconnect -> CB2_LinkError on the
//     survivor, a member leaving the link while open, and the single-player (no group) cable.
//  3. PS-051: the agreed exit of a direct trade.
//  4. PS-052: the fast cable. Whole blocks of every size (1..0x4000; up to MP_LINK_BLOCK_MAX
//     as one LINK_BLOCK, above that as rows) interleaved with plain command rows from both
//     members, under lag and jitter and with a slow consumer: everything arrives intact and in
//     order on both sides. Then the measurements: a battle-like block exchange timed on the
//     GBA row path (the baseline), with row pacing, and with whole blocks.

#include "global.h"
#include "battle.h" // gBattleTypeFlags (PS-048 recovery test)
#include "decompress.h" // gDecompressionBuffer (PS-052: blocks above 0x100 bytes land there)
#include "link.h"
#include "main.h"
#include "string_util.h"
#include "task.h"
#include "constants/characters.h"
#include "mp/mp.h"
#include "mp/mp_internal.h"
#include "mp/mp_link.h"
#include "mp/mp_protocol.h"
#include "mp/mp_selftest.h"
#include "mp/mp_session_internal.h"
#include "mp/mp_transport.h"

static u32 sFail;

#define L_CHECK(cond, what)                                                         \
    do                                                                              \
    {                                                                               \
        if (!(cond))                                                                \
        {                                                                           \
            MP_LOG("selftest: FAIL: link: %s (%s:%d)\n", what, __FILE__, __LINE__); \
            sFail++;                                                                \
        }                                                                           \
    } while (0)

static void Report(const char *what, u32 failuresBefore)
{
    if (sFail == failuresBefore)
        MP_LOG("selftest: link: %s OK\n", what);
    else
        MP_LOG("selftest: link: %s FAILED (%u)\n", what, (unsigned)(sFail - failuresBefore));
}

static u32 sRng = 0x4C494E4Bu; // "LINK"

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

static const u8 sCtlTypes[] =
{
    MP_MSG_LINK_JOIN, MP_MSG_LINK_LEAVE, MP_MSG_LINK_GROUP, MP_MSG_LINK_END, MP_MSG_LINK_OPEN,
    MP_MSG_LINK_CLOSE, MP_MSG_LINK_START, MP_MSG_LINK_STATUS, MP_MSG_LINK_BUS_START, MP_MSG_LINK_BUS_END,
    MP_MSG_LINK_DONE, // PS-051
};

static void FixtureCtl(u8 type, struct MpMsgLinkCtl *c)
{
    memset(c, 0, sizeof(*c));
    switch (type)
    {
    case MP_MSG_LINK_JOIN:      c->arg1 = 0x06; break;
    case MP_MSG_LINK_LEAVE:     break;
    case MP_MSG_LINK_GROUP:     c->groupId = 3; c->arg1 = 0x0B; c->arg2 = 2; break;
    case MP_MSG_LINK_END:       c->groupId = 3; c->arg1 = MP_LINK_END_LOST; break;
    case MP_MSG_LINK_STATUS:    c->groupId = 3; c->arg1 = 0x05; break;
    case MP_MSG_LINK_BUS_START: c->groupId = 3; c->arg1 = 9; c->arg2 = 4; break;
    case MP_MSG_LINK_BUS_END:   c->groupId = 3; c->arg1 = 9; break;
    default:                    c->groupId = 3; c->arg1 = 7; break; // OPEN / CLOSE / START
    }
}

static u8 ParseCtlBytes(u8 type, u8 b0, u8 b1, u8 b2, u8 b3)
{
    struct MpMsgLinkCtl out;
    u8 p[4];

    p[0] = b0; p[1] = b1; p[2] = b2; p[3] = b3;
    return MpProto_ParseLinkCtl(type, p, sizeof(p), &out);
}

static void FixtureRow(struct MpMsgLinkRow *row, u8 count)
{
    u32 i, j;

    memset(row, 0, sizeof(*row));
    row->groupId = 2;
    row->busEpoch = 5;
    row->count = count;
    row->rowSeq = 0xBEEF;
    for (i = 0; i < count; i++)
    {
        for (j = 0; j < MP_LINK_CMD_WORDS; j++)
            row->cmds[i][j] = (u16)(0x1000 * (i + 1) + j);
    }
}

static bool8 RowEquals(const struct MpMsgLinkRow *a, const struct MpMsgLinkRow *b)
{
    // Field by field: the struct has padding after `count`.
    return a->groupId == b->groupId && a->busEpoch == b->busEpoch && a->count == b->count
        && a->rowSeq == b->rowSeq && memcmp(a->cmds, b->cmds, sizeof(a->cmds)) == 0;
}

static void TestParsers(void)
{
    u8 buf[MP_MAX_PACKET_SIZE + 8];
    u8 copy[MP_MAX_PACKET_SIZE + 8];
    struct MpMsgLinkCtl ctl, ctlOut;
    struct MpMsgLinkCmd cmd, cmdOut;
    struct MpMsgLinkRow row, rowOut;
    struct MpMsgHeader hdr;
    u32 failuresBefore = sFail;
    u32 t, len, cut, bit, n;
    u16 written;
    u8 r1, r2;

    // --- control messages ---
    for (t = 0; t < ARRAY_COUNT(sCtlTypes); t++)
    {
        u8 type = sCtlTypes[t];

        FixtureCtl(type, &ctl);
        written = MpProto_WriteLinkCtl(buf, sizeof(buf), type, &ctl);
        L_CHECK(written == MP_HEADER_SIZE + MP_LINK_CTL_SIZE, "ctl writer size");
        L_CHECK(MpProto_ParseHeader(buf, written, &hdr) && hdr.type == type && hdr.version == MP_PROTOCOL_VERSION, "ctl header");
        L_CHECK(MpProto_ParseLinkCtl(type, buf + 4, hdr.length, &ctlOut) == MP_PARSE_OK
             && memcmp(&ctl, &ctlOut, sizeof(ctl)) == 0, "ctl round trip");
        for (cut = 0; cut < MP_LINK_CTL_SIZE; cut++)
            L_CHECK(MpProto_ParseLinkCtl(type, buf + 4, cut, &ctlOut) == MP_PARSE_ERR_TRUNCATED, "ctl truncated");
        memcpy(copy, buf + 4, MP_LINK_CTL_SIZE);
        copy[MP_LINK_CTL_SIZE] = 0;
        L_CHECK(MpProto_ParseLinkCtl(type, copy, MP_LINK_CTL_SIZE + 1, &ctlOut) == MP_PARSE_ERR_OVERSIZED, "ctl oversized");
        for (bit = 0; bit < MP_LINK_CTL_SIZE * 8; bit++)
        {
            memcpy(copy, buf + 4, MP_LINK_CTL_SIZE);
            copy[bit / 8] ^= (u8)(1 << (bit % 8));
            r1 = MpProto_ParseLinkCtl(type, copy, MP_LINK_CTL_SIZE, &ctlOut);
            r2 = MpProto_ParseLinkCtl(type, copy, MP_LINK_CTL_SIZE, &ctlOut);
            L_CHECK(r1 == r2, "ctl bit flip deterministic");
        }
        L_CHECK(MpProto_IsLinkCtlType(type), "ctl type recognised");
    }
    L_CHECK(MpProto_WriteLinkCtl(buf, sizeof(buf), MP_MSG_LINK_ROW, &ctl) == 0, "ctl writer refuses ROW");
    L_CHECK(MpProto_WriteLinkCtl(buf, 7, MP_MSG_LINK_JOIN, &ctl) == 0, "ctl writer small buffer");
    // targeted
    L_CHECK(ParseCtlBytes(MP_MSG_LINK_JOIN, 0, 0x01, 0, 0) == MP_PARSE_ERR_BAD_FIELD, "JOIN with one member");
    L_CHECK(ParseCtlBytes(MP_MSG_LINK_JOIN, 0, 0x00, 0, 0) == MP_PARSE_ERR_BAD_FIELD, "JOIN with no member");
    L_CHECK(ParseCtlBytes(MP_MSG_LINK_JOIN, 0, 0x13, 0, 0) == MP_PARSE_ERR_BAD_FIELD, "JOIN with slot 4");
    L_CHECK(ParseCtlBytes(MP_MSG_LINK_JOIN, 9, 0x0F, 9, 9) == MP_PARSE_OK, "JOIN with 4 members, reserved ignored");
    L_CHECK(ParseCtlBytes(MP_MSG_LINK_GROUP, 0, 0x03, 0, 0) == MP_PARSE_ERR_BAD_FIELD, "GROUP with group id 0");
    L_CHECK(ParseCtlBytes(MP_MSG_LINK_GROUP, 1, 0x03, 2, 0) == MP_PARSE_ERR_BAD_FIELD, "GROUP with link id == count");
    L_CHECK(ParseCtlBytes(MP_MSG_LINK_GROUP, 1, 0x03, 1, 0) == MP_PARSE_OK, "GROUP link id 1 of 2");
    L_CHECK(ParseCtlBytes(MP_MSG_LINK_END, 0, 0, 0, 0) == MP_PARSE_ERR_BAD_FIELD, "END with reason 0");
    L_CHECK(ParseCtlBytes(MP_MSG_LINK_END, 0, MP_LINK_END_REFUSED, 0, 0) == MP_PARSE_OK, "END for a refused request (group 0)");
    L_CHECK(ParseCtlBytes(MP_MSG_LINK_OPEN, 0, 1, 0, 0) == MP_PARSE_ERR_BAD_FIELD, "OPEN with group id 0");
    L_CHECK(ParseCtlBytes(MP_MSG_LINK_STATUS, 1, 0x10, 0, 0) == MP_PARSE_ERR_BAD_FIELD, "STATUS with link id 4 open");
    L_CHECK(ParseCtlBytes(MP_MSG_LINK_BUS_START, 1, 0, 1, 0) == MP_PARSE_ERR_BAD_FIELD, "BUS_START with epoch 0");
    L_CHECK(ParseCtlBytes(MP_MSG_LINK_BUS_END, 1, 0, 0, 0) == MP_PARSE_ERR_BAD_FIELD, "BUS_END with epoch 0");
    L_CHECK(ParseCtlBytes(MP_MSG_LINK_DONE, 0, 0, 0, 0) == MP_PARSE_ERR_BAD_FIELD, "DONE with group id 0 (PS-051)");
    L_CHECK(ParseCtlBytes(MP_MSG_LINK_DONE, 2, 0x10, 0, 0) == MP_PARSE_ERR_BAD_FIELD, "DONE with link id 4 done (PS-051)");
    L_CHECK(ParseCtlBytes(MP_MSG_LINK_DONE, 2, 0x03, 7, 9) == MP_PARSE_OK, "DONE mask 0x3, arg2 / reserved ignored (PS-051)");
    L_CHECK(ParseCtlBytes(MP_MSG_LINK_END, 2, MP_LINK_END_DONE, 0, 0) == MP_PARSE_OK, "END with reason DONE (PS-051)");
    L_CHECK(ParseCtlBytes(MP_MSG_LINK_CMD, 1, 1, 1, 1) == MP_PARSE_ERR_BAD_FIELD, "ctl parser refuses CMD");
    L_CHECK(ParseCtlBytes(0x7F, 1, 1, 1, 1) == MP_PARSE_ERR_BAD_FIELD, "ctl parser refuses an unassigned LINK_* type");

    // --- LINK_CMD ---
    memset(&cmd, 0, sizeof(cmd));
    cmd.groupId = 4;
    cmd.busEpoch = 200;
    for (n = 0; n < MP_LINK_CMD_WORDS; n++)
        cmd.cmd[n] = (u16)(0xBBBB - n);
    written = MpProto_WriteLinkCmd(buf, sizeof(buf), &cmd);
    L_CHECK(written == MP_HEADER_SIZE + MP_LINK_CMD_SIZE, "CMD writer size");
    L_CHECK(MpProto_ParseLinkCmd(buf + 4, MP_LINK_CMD_SIZE, &cmdOut) == MP_PARSE_OK && memcmp(&cmd, &cmdOut, sizeof(cmd)) == 0, "CMD round trip");
    for (cut = 0; cut < MP_LINK_CMD_SIZE; cut++)
        L_CHECK(MpProto_ParseLinkCmd(buf + 4, cut, &cmdOut) == MP_PARSE_ERR_TRUNCATED, "CMD truncated");
    L_CHECK(MpProto_ParseLinkCmd(buf + 4, MP_LINK_CMD_SIZE + 1, &cmdOut) == MP_PARSE_ERR_OVERSIZED, "CMD oversized");
    memset(copy, 0, sizeof(copy));
    copy[0] = 1;
    copy[1] = 1;
    L_CHECK(MpProto_ParseLinkCmd(copy, MP_LINK_CMD_SIZE, &cmdOut) == MP_PARSE_ERR_BAD_FIELD, "CMD with all-zero words");
    memcpy(copy, buf + 4, MP_LINK_CMD_SIZE);
    copy[0] = 0;
    L_CHECK(MpProto_ParseLinkCmd(copy, MP_LINK_CMD_SIZE, &cmdOut) == MP_PARSE_ERR_BAD_FIELD, "CMD with group id 0");
    for (bit = 0; bit < MP_LINK_CMD_SIZE * 8; bit++)
    {
        memcpy(copy, buf + 4, MP_LINK_CMD_SIZE);
        copy[bit / 8] ^= (u8)(1 << (bit % 8));
        r1 = MpProto_ParseLinkCmd(copy, MP_LINK_CMD_SIZE, &cmdOut);
        r2 = MpProto_ParseLinkCmd(copy, MP_LINK_CMD_SIZE, &cmdOut);
        L_CHECK(r1 == r2, "CMD bit flip deterministic");
    }

    // --- LINK_ROW, 2..4 members ---
    for (n = 2; n <= MP_LINK_MAX_MEMBERS; n++)
    {
        FixtureRow(&row, (u8)n);
        written = MpProto_WriteLinkRow(buf, sizeof(buf), &row);
        L_CHECK(written == MP_HEADER_SIZE + MP_LINK_ROW_SIZE(n), "ROW writer size");
        L_CHECK(MpProto_ParseLinkRow(buf + 4, MP_LINK_ROW_SIZE(n), &rowOut) == MP_PARSE_OK
             && RowEquals(&row, &rowOut), "ROW round trip");
        for (cut = 0; cut < MP_LINK_ROW_SIZE(n); cut++)
            L_CHECK(MpProto_ParseLinkRow(buf + 4, cut, &rowOut) != MP_PARSE_OK, "ROW truncated");
        L_CHECK(MpProto_ParseLinkRow(buf + 4, MP_LINK_ROW_SIZE(n) + 1, &rowOut) != MP_PARSE_OK, "ROW oversized");
        for (bit = 0; bit < (u32)MP_LINK_ROW_SIZE(n) * 8; bit++)
        {
            memcpy(copy, buf + 4, MP_LINK_ROW_SIZE(n));
            copy[bit / 8] ^= (u8)(1 << (bit % 8));
            r1 = MpProto_ParseLinkRow(copy, MP_LINK_ROW_SIZE(n), &rowOut);
            r2 = MpProto_ParseLinkRow(copy, MP_LINK_ROW_SIZE(n), &rowOut);
            L_CHECK(r1 == r2, "ROW bit flip deterministic");
            if (r1 == MP_PARSE_OK)
                L_CHECK(rowOut.count == n, "ROW bit flip keeps count consistent with length");
        }
    }
    FixtureRow(&row, 2);
    MpProto_WriteLinkRow(buf, sizeof(buf), &row);
    memcpy(copy, buf + 4, MP_LINK_ROW_SIZE(2));
    copy[2] = 1;
    L_CHECK(MpProto_ParseLinkRow(copy, MP_LINK_ROW_SIZE(2), &rowOut) == MP_PARSE_ERR_BAD_FIELD, "ROW with 1 member");
    copy[2] = 5;
    L_CHECK(MpProto_ParseLinkRow(copy, MP_LINK_ROW_SIZE(2), &rowOut) == MP_PARSE_ERR_BAD_FIELD, "ROW with 5 members");
    row.count = 5;
    L_CHECK(MpProto_WriteLinkRow(buf, sizeof(buf), &row) == 0, "ROW writer refuses 5 members");

    // --- garbage: every LINK parser over random buffers ---
    for (n = 0; n < 30000; n++)
    {
        len = Rand() % (MP_MAX_PACKET_SIZE + 1);
        for (cut = 0; cut < len; cut++)
            copy[cut] = (u8)Rand();
        if (n % 3 == 0 && len >= 3)
            copy[2] = (u8)(2 + Rand() % 3); // plausible ROW counts reach the body checks
        MpProto_ParseLinkCtl(sCtlTypes[n % ARRAY_COUNT(sCtlTypes)], copy, len, &ctlOut);
        MpProto_ParseLinkCmd(copy, len, &cmdOut);
        if (MpProto_ParseLinkRow(copy, len, &rowOut) == MP_PARSE_OK)
            L_CHECK(rowOut.count >= 2 && rowOut.count <= MP_LINK_MAX_MEMBERS && len == (u32)MP_LINK_ROW_SIZE(rowOut.count), "garbage ROW accepted only when consistent");
    }
    Report("LINK_* parsers (round trip, truncated, oversized, bit flips, 30000 garbage, targeted)", failuresBefore);
}

// ===========================================================================
// 2. Two game instances on the real link stack
// ===========================================================================

#define LINK_PORT_FIRST     27550
#define LINK_PORT_LAST      27559
#define LINK_STATE_CAP      16384
#define LINK_BLOCK_SIZE     200
#define LINK_FRAME_LIMIT    2000000     // safety cap; waits are bounded by LINK_TIME_LIMIT_MS
#define LINK_TIME_LIMIT_MS  8000

#define S_HOST  0
#define S_C1    1
#define S_C2    2
#define S_COUNT 3

#define G_A     0
#define G_B     1
#define G_COUNT 2

struct GameInst
{
    u8 session;
    u8 advanceIn;           // emulates Task_TriggerHandshake: set gShouldAdvanceLinkState after N ticks
    u32 trainerId;
    MainCallback callback2;
    u8 mainState;
    struct SaveBlock1 save1;
    struct SaveBlock2 save2;
    u8 link[LINK_STATE_CAP];
};

static struct GameInst sGames[G_COUNT];
static u8 sNeutralLink[LINK_STATE_CAP];
static struct SaveBlock1 *sSavedPtr1;
static struct SaveBlock2 *sSavedPtr2;
static MainCallback sSavedCallback2;
static u8 sSavedMainState;
static u16 sPort;
static s8 sEntered = -1;

static void Sel(u8 session)
{
    MpSession_SelectInstance(session);
}

// Swap game instance `g` in: its link.c state, save blocks and main callback.
static void Enter(u8 g)
{
    struct GameInst *gi = &sGames[g];

    sEntered = (s8)g;
    Sel(gi->session);
    MP_LinkSelftestState(gi->link, sizeof(gi->link), TRUE);
    gSaveBlock1Ptr = &gi->save1;
    gSaveBlock2Ptr = &gi->save2;
    gMain.callback2 = gi->callback2;
    gMain.state = gi->mainState;
}

static void Leave(void)
{
    struct GameInst *gi;

    if (sEntered < 0)
        return;
    gi = &sGames[sEntered];
    MP_LinkSelftestState(gi->link, sizeof(gi->link), FALSE);
    gi->callback2 = gMain.callback2;
    gi->mainState = gMain.state;
    gSaveBlock1Ptr = sSavedPtr1;
    gSaveBlock2Ptr = sSavedPtr2;
    gMain.callback2 = sSavedCallback2;
    gMain.state = sSavedMainState;
    sEntered = -1;
}

static void TickGame(u8 g)
{
    Enter(g);
    if (sGames[g].advanceIn != 0 && --sGames[g].advanceIn == 0)
        gShouldAdvanceLinkState = 1;
    HandleLinkConnection();
    Leave();
}

static void DrainEvents(void)
{
    struct MpEvent ev;

    while (MP_PollEvent(&ev))
        ;
}

static void PumpNet(void)
{
    u8 i;

    for (i = 0; i < S_COUNT; i++)
    {
        Sel(i);
        MP_Update();
        DrainEvents();
    }
}

// One frame: the network, then each game in `mask` (bit = game) ticks once.
static void Frame(u8 mask)
{
    u8 g;

    PumpNet();
    for (g = 0; g < G_COUNT; g++)
    {
        if (mask & (1 << g))
            TickGame(g);
    }
}

typedef bool8 (*GameCond)(u8 g);

// Runs frames until cond holds for every game in `mask`. Returns TRUE if it did.
static bool8 RunUntil(GameCond cond, u8 mask, u8 tickMask)
{
    u32 start = MpTransport_GetTimeMs();
    u32 frames;
    u8 g;

    for (frames = 0; frames < LINK_FRAME_LIMIT && (u32)(MpTransport_GetTimeMs() - start) < LINK_TIME_LIMIT_MS; frames++)
    {
        bool8 all = TRUE;

        for (g = 0; g < G_COUNT; g++)
        {
            if (mask & (1 << g))
            {
                bool8 ok;

                Enter(g);
                ok = cond(g);
                Leave();
                if (!ok)
                    all = FALSE;
            }
        }
        if (all)
            return TRUE;
        Frame(tickMask);
    }
    return FALSE;
}

static void RunFrames(u32 count, u8 tickMask)
{
    while (count-- != 0)
        Frame(tickMask);
}

static bool8 CondGrouped(u8 g)        { return MP_LinkGetState() == MP_LINK_STATE_GROUPED; }
static bool8 CondTwoOnCable(u8 g)     { return GetLinkPlayerCount_2() >= 2; }
static bool8 CondExchanged(u8 g)      { return gReceivedRemoteLinkPlayers == TRUE; }
static bool8 CondTaskFinished(u8 g)   { return IsLinkTaskFinished(); }
static bool8 CondBothBlocks(u8 g)     { return (GetBlockReceivedStatus() & 3) == 3; }
static bool8 CondClosed(u8 g)         { return !gReceivedRemoteLinkPlayers; }
static bool8 CondLinkError(u8 g)      { return gMain.callback2 == CB2_LinkError; }
static bool8 CondSessionIn(u8 g)      { return MP_GetState() == MP_STATE_IN_SESSION; }

static bool8 CondBacklog(u8 g)
{
    struct MpLinkCableInfo info;

    MpLink_CableGetInfo(&info);
    return info.queued >= 60; // 4 blocks of 16 commands from A
}

static bool8 CondQueueEmpty(u8 g)
{
    struct MpLinkCableInfo info;

    MpLink_CableGetInfo(&info);
    return info.queued == 0;
}

static void SetName(u8 *dest, const char *ascii)
{
    u32 i;

    memset(dest, EOS, PLAYER_NAME_LENGTH + 1);
    for (i = 0; ascii[i] != '\0' && i < PLAYER_NAME_LENGTH; i++)
        dest[i] = (u8)(CHAR_A + (ascii[i] - 'A'));
}

static void InitGame(u8 g, u8 session, const char *name, u32 trainerId)
{
    struct GameInst *gi = &sGames[g];

    memset(gi, 0, sizeof(*gi));
    gi->session = session;
    gi->trainerId = trainerId;
    SetName(gi->save2.playerName, name);
    gi->save2.playerGender = (g == G_A) ? MALE : FEMALE;
    gi->save2.playerTrainerId[0] = (u8)trainerId;
    gi->save2.playerTrainerId[1] = (u8)(trainerId >> 8);
    gi->save2.playerTrainerId[2] = (u8)(trainerId >> 16);
    gi->save2.playerTrainerId[3] = (u8)(trainerId >> 24);
    memcpy(gi->link, sNeutralLink, sizeof(gi->link)); // a freshly booted link.c
    gi->callback2 = NULL;
}

static void FillPattern(u8 *dst, u32 trainerId, u8 round)
{
    u32 i;

    for (i = 0; i < LINK_BLOCK_SIZE; i++)
        dst[i] = (u8)(trainerId * 13u + round * 29u + i * 3u + (i >> 4));
}

static bool8 PatternMatches(const u16 *recv, u32 trainerId, u8 round)
{
    u8 expect[LINK_BLOCK_SIZE];

    FillPattern(expect, trainerId, round);
    return memcmp(recv, expect, LINK_BLOCK_SIZE) == 0;
}

static void OpenBoth(void)
{
    u8 g;

    for (g = 0; g < G_COUNT; g++)
    {
        Enter(g);
        gLinkType = LINKTYPE_TRADE_SETUP;
        OpenLink();
        ResetTasks(); // Task_TriggerHandshake is emulated by advanceIn
        Leave();
        sGames[g].advanceIn = 5;
    }
}

// OpenLink on both, the master advances, the player-data exchange completes. Checks gLinkPlayers.
static void DoExchange(const char *what)
{
    u8 g;

    OpenBoth();
    L_CHECK(RunUntil(CondTwoOnCable, 0x3, 0x3), "both see 2 players on the cable (handshake)");
    RunFrames(30, 0x3); // the game's masters wait 30 frames before advancing
    Enter(G_A);
    L_CHECK(IsLinkMaster(), "link id 0 is the master");
    CheckShouldAdvanceLinkState();
    Leave();
    Enter(G_B);
    L_CHECK(!IsLinkMaster(), "link id 1 is not the master");
    Leave();
    L_CHECK(RunUntil(CondExchanged, 0x3, 0x3), what);
    for (g = 0; g < G_COUNT; g++)
    {
        Enter(g);
        L_CHECK(GetMultiplayerId() == g, "GetMultiplayerId follows slot order");
        L_CHECK(GetLinkPlayerCount() == 2, "2 link players");
        L_CHECK(gLinkPlayers[0].trainerId == sGames[G_A].trainerId, "gLinkPlayers[0] is A");
        L_CHECK(gLinkPlayers[1].trainerId == sGames[G_B].trainerId, "gLinkPlayers[1] is B");
        L_CHECK(StringCompare(gLinkPlayers[0].name, sGames[G_A].save2.playerName) == 0, "gLinkPlayers[0] name");
        L_CHECK(StringCompare(gLinkPlayers[1].name, sGames[G_B].save2.playerName) == 0, "gLinkPlayers[1] name");
        L_CHECK(gLinkPlayers[1].gender == FEMALE && gLinkPlayers[0].gender == MALE, "gLinkPlayers gender");
        L_CHECK(gLinkPlayers[g].linkType == LINKTYPE_TRADE_SETUP, "gLinkPlayers link type");
        L_CHECK((gLinkStatus & LINK_STAT_CONN_ESTABLISHED) && !(gLinkStatus & LINK_STAT_ERRORS), "connection established, no errors");
        Leave();
    }
}

// Both send a 200-byte block, both receive both. `round` changes the pattern.
static void DoBlockRound(u8 round)
{
    u8 g;

    L_CHECK(RunUntil(CondTaskFinished, 0x3, 0x3), "link task idle before SendBlock");
    for (g = 0; g < G_COUNT; g++)
    {
        Enter(g);
        FillPattern(gBlockSendBuffer, sGames[g].trainerId, round);
        L_CHECK(SendBlock(0, gBlockSendBuffer, LINK_BLOCK_SIZE), "SendBlock accepted");
        Leave();
    }
    L_CHECK(RunUntil(CondBothBlocks, 0x3, 0x3), "both blocks received on both sides");
    for (g = 0; g < G_COUNT; g++)
    {
        Enter(g);
        L_CHECK(PatternMatches(gBlockRecvBuffer[0], sGames[G_A].trainerId, round), "block from A intact");
        L_CHECK(PatternMatches(gBlockRecvBuffer[1], sGames[G_B].trainerId, round), "block from B intact");
        ResetBlockReceivedFlags();
        Leave();
    }
}

static void DoClose(void)
{
    u8 g;

    L_CHECK(RunUntil(CondTaskFinished, 0x3, 0x3), "link task idle before closing");
    for (g = 0; g < G_COUNT; g++)
    {
        Enter(g);
        SetCloseLinkCallback();
        Leave();
    }
    L_CHECK(RunUntil(CondClosed, 0x3, 0x3), "both closed the link (SetCloseLinkCallback)");
    RunFrames(3, 0x3);
    for (g = 0; g < G_COUNT; g++)
    {
        Enter(g);
        L_CHECK(gLink.state <= LINK_STATE_START1 && !(gLinkStatus & LINK_STAT_CONN_ESTABLISHED), "cable closed after CloseLink");
        Leave();
    }
}

// A streams 4 blocks while B is frozen (no game ticks); B then catches up. The row path (PS-052:
// whole blocks off), so the rows pile up past the 50-row report cap.
static void DoStall(void)
{
    struct MpLinkCableInfo info;
    struct MpLinkCableStats stats;
    u8 round;

    MP_LinkSetFastBlocks(FALSE);
    for (round = 10; round < 14; round++)
    {
        L_CHECK(RunUntil(CondTaskFinished, 0x1, 0x1), "A idle before the next block");
        Enter(G_A);
        FillPattern(gBlockSendBuffer, sGames[G_A].trainerId, round);
        SendBlock(0, gBlockSendBuffer, LINK_BLOCK_SIZE);
        Leave();
        RunFrames(20, 0x1); // only A ticks
    }
    L_CHECK(RunUntil(CondBacklog, 0x2, 0x1), "rows pile up for the stalled member");
    Enter(G_B);
    MpLink_CableGetInfo(&info);
    L_CHECK(info.queued > QUEUE_CAPACITY, "stalled member has more than 50 rows waiting");
    Leave();
    TickGame(G_B);
    Enter(G_B);
    L_CHECK(gLink.recvQueue.count <= QUEUE_CAPACITY && gLastRecvQueueCount == gLink.recvQueue.count, "reported queue capped at 50");
    L_CHECK(!(gLinkStatus & LINK_STAT_ERRORS), "no link error while catching up");
    Leave();
    L_CHECK(RunUntil(CondQueueEmpty, 0x2, 0x3), "stalled member drains its backlog");
    RunFrames(3, 0x3);
    Enter(G_B);
    L_CHECK(PatternMatches(gBlockRecvBuffer[0], sGames[G_A].trainerId, 13), "after catching up, the last block from A is intact");
    L_CHECK(!(gLinkStatus & LINK_STAT_ERRORS) && gMain.callback2 != CB2_LinkError, "catch-up without a link error");
    MpLink_GetCableStats(&stats);
    L_CHECK(stats.catchUpRows > 0, "PS-052: the stalled member played backlogged block rows several per tick");
    ResetBlockReceivedFlags();
    Leave();
    Enter(G_A);
    ResetBlockReceivedFlags();
    Leave();
    MP_LinkSetFastBlocks(TRUE);
}

// PS-052: the same with whole blocks. B is frozen while A sends 4 blocks, then B ticks again. B's
// game never takes the blocks (its received flags stay set), so each one waits as long as the GBA
// cable would have needed for it, then overwrites the previous one, like the cable; nothing is lost
// and nothing errors.
static void DoStallFast(void)
{
    struct MpLinkCableInfo info;
    struct MpLinkCableStats stats;
    u8 round;

    for (round = 20; round < 24; round++)
    {
        L_CHECK(RunUntil(CondTaskFinished, 0x1, 0x1), "PS-052: A idle before the next whole block");
        Enter(G_A);
        FillPattern(gBlockSendBuffer, sGames[G_A].trainerId, round);
        L_CHECK(SendBlock(0, gBlockSendBuffer, LINK_BLOCK_SIZE), "PS-052: SendBlock accepted");
        L_CHECK(IsLinkTaskFinished(), "PS-052: a whole block leaves at once (the link task is finished)");
        Leave();
        RunFrames(3, 0x1); // only A ticks
    }
    RunFrames(10, 0x0);
    Enter(G_B);
    MpLink_CableGetInfo(&info);
    L_CHECK(info.queued >= 4, "PS-052: the 4 whole blocks wait for the stalled member");
    Leave();
    L_CHECK(RunUntil(CondQueueEmpty, 0x2, 0x3), "PS-052: the stalled member takes its whole blocks");
    RunFrames(3, 0x3);
    Enter(G_B);
    L_CHECK(PatternMatches(gBlockRecvBuffer[0], sGames[G_A].trainerId, 23), "PS-052: the last whole block from A is intact");
    L_CHECK(!(gLinkStatus & LINK_STAT_ERRORS) && gMain.callback2 != CB2_LinkError, "PS-052: no link error");
    MpLink_GetCableStats(&stats);
    L_CHECK(stats.holdTicks > 0, "PS-052: a block waited while the game had not taken the previous one");
    ResetBlockReceivedFlags();
    Leave();
    Enter(G_A);
    ResetBlockReceivedFlags();
    Leave();
}

static bool8 HostOn(u8 session, const char *name)
{
    bool8 ok = FALSE;

    Sel(session);
    MP_SetNameOverride(name);
    for (sPort = LINK_PORT_FIRST; sPort <= LINK_PORT_LAST && !ok; sPort++)
        ok = MP_Host(sPort);
    sPort--;
    return ok;
}

static bool8 JoinOn(u8 session, const char *name)
{
    Sel(session);
    MP_SetNameOverride(name);
    return MP_Join("127.0.0.1", sPort);
}

static bool8 CondSessionOf(u8 session)
{
    Sel(session);
    return MP_GetState() == MP_STATE_IN_SESSION;
}

static bool8 WaitSession(u8 session)
{
    u32 start = MpTransport_GetTimeMs();

    while (!CondSessionOf(session) && (u32)(MpTransport_GetTimeMs() - start) < 5000)
        PumpNet();
    return CondSessionOf(session);
}

static void LeaveAll(void)
{
    u32 start;
    u8 i;
    bool8 open;

    for (i = 0; i < S_COUNT; i++)
    {
        Sel(i);
        MP_Leave();
    }
    start = MpTransport_GetTimeMs();
    do
    {
        PumpNet();
        open = FALSE;
        for (i = 0; i < S_COUNT; i++)
        {
            Sel(i);
            open = open || MpTransport_IsOpen();
        }
    } while (open && (u32)(MpTransport_GetTimeMs() - start) < 3000);
    for (i = 0; i < S_COUNT; i++)
        MpSession_ResetInstance(i);
}

// Scenario A: the host only sequences; clients 1 and 2 link.
static void ScenarioHostNotMember(void)
{
    struct MpLinkStats stats;
    u32 failuresBefore = sFail;
    u8 g;

    InitGame(G_A, S_C1, "ALICE", 0x0001A11Cu);
    InitGame(G_B, S_C2, "BOB", 0x000B0B0Bu);
    L_CHECK(HostOn(S_HOST, "Host"), "host on a port in 27550..27559");
    L_CHECK(JoinOn(S_C1, "Alice") && WaitSession(S_C1), "client 1 joins");
    L_CHECK(JoinOn(S_C2, "Bob") && WaitSession(S_C2), "client 2 joins");
    PumpNet();

    Enter(G_A);
    L_CHECK(!MP_LinkJoin(0x0A), "MP_LinkJoin refuses a mask with an empty slot");
    L_CHECK(!MP_LinkJoin(0x05), "MP_LinkJoin refuses a mask without our slot");
    L_CHECK(MP_LinkJoin(0x06), "client 1 asks for slots 1+2");
    Leave();
    RunFrames(20, 0x3);
    Enter(G_A);
    L_CHECK(MP_LinkGetState() == MP_LINK_STATE_REQUESTED, "one request alone does not form a group");
    Leave();
    Enter(G_B);
    L_CHECK(MP_LinkJoin(0x06), "client 2 asks for slots 1+2");
    Leave();
    L_CHECK(RunUntil(CondGrouped, 0x3, 0x3), "both grouped");
    Enter(G_A); L_CHECK(MP_LinkGetLinkId() == 0, "slot 1 is link id 0"); Leave();
    Enter(G_B); L_CHECK(MP_LinkGetLinkId() == 1, "slot 2 is link id 1"); Leave();
    Report("A: link group of two clients (host not a member)", failuresBefore);

    failuresBefore = sFail;
    DoExchange("player-data exchange completes on both (host relays)");
    Report("A: OpenLink + player-data exchange, gLinkPlayers correct on both", failuresBefore);

    failuresBefore = sFail;
    DoBlockRound(1);
    DoBlockRound(2);
    Report("A: SendBlock exchanges both ways (2 rounds)", failuresBefore);

    failuresBefore = sFail;
    DoStall();
    Report("A: a stalled member catches up (stall, not desync; 50-row cap)", failuresBefore);

    failuresBefore = sFail;
    DoStallFast();
    Report("A: PS-052 a stalled member catches up on whole blocks", failuresBefore);

    failuresBefore = sFail;
    DoClose();
    DoExchange("player-data exchange after re-open (new bus on the same group)");
    DoBlockRound(3);
    Report("A: close + re-open on the same group", failuresBefore);

    // PS-047: a member sends a LINK_CMD no game sends (INIT_BLOCK of 0xFFFF bytes). The host
    // bus refuses it and ends the group: both cables report the error.
    failuresBefore = sFail;
    {
        u16 bad[MP_LINK_CMD_WORDS];

        memset(bad, 0, sizeof(bad));
        bad[0] = LINKCMD_INIT_BLOCK;
        bad[1] = 0xFFFF;
        Enter(G_A);
        MpLink_CableSend(bad);
        Leave();
    }
    L_CHECK(RunUntil(CondLinkError, 0x3, 0x3), "a refused LINK_CMD: both members reach CB2_LinkError");
    Sel(S_HOST);
    MpLink_GetStats(&stats);
    L_CHECK(stats.cmdsRefused == 1, "the host counted one refused command");
    Report("A: host bus pre-filter ends the group on a refused LINK_CMD (PS-047)", failuresBefore);

    // Close both cables and leave the group first, so no fuzzed message can ever reach link.c
    // (the fuzz below targets the session layer; link.c itself is fuzzed by the hardening group). A member that is not open never accepts rows.
    for (g = 0; g < G_COUNT; g++)
    {
        Enter(g);
        CloseLink();
        MP_LinkLeave();
        Leave();
    }
    RunFrames(10, 0x3);

    // Wrong-direction / garbage LINK messages at the host and a client: dropped, no crash.
    failuresBefore = sFail;
    {
        u8 junk[MP_LINK_ROW_SIZE(2)];
        u32 i;

        Sel(S_HOST);
        memset(junk, 0x11, sizeof(junk));
        L_CHECK(!MpLink_OnHostReceive(1, MP_CHANNEL_CONTROL, MP_MSG_LINK_ROW, junk, sizeof(junk)), "host refuses a ROW from a client");
        L_CHECK(!MpLink_OnHostReceive(1, MP_CHANNEL_CONTROL, MP_MSG_LINK_GROUP, junk, 4), "host refuses a GROUP from a client");
        L_CHECK(!MpLink_OnHostReceive(1, MP_CHANNEL_PRESENCE, MP_MSG_LINK_OPEN, junk, 4), "host refuses LINK_* on channel 1");
        L_CHECK(!MpLink_OnHostReceive(1, MP_CHANNEL_CONTROL, 0x7E, junk, 4), "host refuses an unassigned LINK_* type");
        for (i = 0; i < 2000; i++)
        {
            u32 len = Rand() % sizeof(junk);
            u32 k;

            for (k = 0; k < len; k++)
                junk[k] = (u8)Rand();
            MpLink_OnHostReceive((u8)(1 + Rand() % 2), MP_CHANNEL_CONTROL, (u8)(MP_MSG_LINK_FIRST + Rand() % 32), junk, len);
            Sel(S_C1);
            MpLink_OnClientReceive(MP_CHANNEL_CONTROL, (u8)(MP_MSG_LINK_FIRST + Rand() % 32), junk, len);
            Sel(S_HOST);
        }
    }
    // The fuzzing above may have ended the group (random LEAVE/OPEN/CLOSE): that is allowed,
    // only crashes and corruption are failures. Rebuild a clean link for the disconnect test.
    Report("A: wrong-direction and random LINK_* messages are dropped safely", failuresBefore);

    failuresBefore = sFail;
    for (g = 0; g < G_COUNT; g++)
    {
        Enter(g);
        MP_LinkLeave(); // whatever the fuzzing left behind
        Leave();
    }
    RunFrames(10, 0x3);
    for (g = 0; g < G_COUNT; g++)
    {
        memcpy(sGames[g].link, sNeutralLink, sizeof(sGames[g].link));
        sGames[g].callback2 = NULL;
        sGames[g].advanceIn = 0;
    }
    Enter(G_A); MP_LinkJoin(0x06); Leave();
    Enter(G_B); MP_LinkJoin(0x06); Leave();
    L_CHECK(RunUntil(CondGrouped, 0x3, 0x3), "regrouped after the fuzzing");
    DoExchange("player-data exchange before the disconnect test");
    // Client 2 leaves the session while the cable is open.
    Sel(S_C2);
    MP_Leave();
    L_CHECK(RunUntil(CondLinkError, 0x1, 0x3), "survivor reaches CB2_LinkError");
    Enter(G_A);
    L_CHECK(gLinkErrorOccurred && !gReceivedRemoteLinkPlayers, "survivor: gLinkErrorOccurred, link closed");
    L_CHECK(MP_LinkGetState() == MP_LINK_STATE_NONE && MP_LinkGetEndReason() == MP_LINK_END_LOST, "survivor: group ended with LOST");
    Leave();
    Enter(G_B);
    L_CHECK(gMain.callback2 == CB2_LinkError && MP_LinkGetEndReason() == MP_LINK_END_SESSION, "leaver: its own cable reports the error too");
    Leave();
    Sel(S_HOST);
    MpLink_GetStats(&stats);
    L_CHECK(stats.rowsEmitted > 0 && stats.groupsFormed >= 2, "host sequenced rows for a group it is not in");
    Report("A: a member leaving the session -> the game's link-error path", failuresBefore);

    LeaveAll();
}

// Scenario B: the host is a link member (direct calls, no network on its side).
static void ScenarioHostMember(void)
{
    u32 failuresBefore = sFail;

    InitGame(G_A, S_HOST, "HOST", 0x00C0FFEEu);
    InitGame(G_B, S_C1, "CARL", 0x00012345u);
    L_CHECK(HostOn(S_HOST, "Host"), "host on a port in 27550..27559");
    L_CHECK(JoinOn(S_C1, "Carl") && WaitSession(S_C1), "client joins");
    PumpNet();
    Enter(G_A); L_CHECK(MP_LinkJoin(0x03), "host asks for slots 0+1"); Leave();
    Enter(G_B); L_CHECK(MP_LinkJoin(0x03), "client asks for slots 0+1"); Leave();
    L_CHECK(RunUntil(CondGrouped, 0x3, 0x3), "both grouped");
    DoExchange("player-data exchange with the host as link master");
    DoBlockRound(1);
    Enter(G_A); L_CHECK(MP_LinkTimeControlsLocked(), "PS-048: pause / speed-up locked while grouped (host)"); Leave();
    Enter(G_B); L_CHECK(MP_LinkTimeControlsLocked(), "PS-048: pause / speed-up locked while grouped (client)"); Leave();
    Report("B: host as link member: exchange + SendBlock", failuresBefore);

    failuresBefore = sFail;
    Enter(G_B);
    MP_LinkLeave(); // leaves the link (not the session) with the cable open
    Leave();
    L_CHECK(RunUntil(CondLinkError, 0x1, 0x3), "host member reaches CB2_LinkError");
    Enter(G_A);
    L_CHECK(MP_LinkGetEndReason() == MP_LINK_END_LEFT, "host member: group ended with LEFT");
    L_CHECK(MP_GetState() == MP_STATE_IN_SESSION, "the session itself is unaffected");
    Leave();
    Report("B: a member leaving the link while open -> link-error path", failuresBefore);

    // PS-048: the error screen's A button cleans up, then ReloadSave(). The clean-up must close
    // the cable, leave the link group and keep the session. (ReloadSave itself needs the real
    // game and a save file: manual test, see the PS-048 card.)
    failuresBefore = sFail;
    Enter(G_A);
    gBattleTypeFlags = BATTLE_TYPE_LINK | BATTLE_TYPE_LINK_IN_BATTLE;
    MP_LinkPrepareErrorRecovery();
    L_CHECK(!gReceivedRemoteLinkPlayers && !gLinkErrorOccurred && gBattleTypeFlags == 0, "recovery: link and battle flags cleared");
    L_CHECK(MP_LinkGetState() == MP_LINK_STATE_NONE && !MP_LinkTimeControlsLocked(), "recovery: out of the link group, pause / speed-up allowed again");
    L_CHECK(MP_GetState() == MP_STATE_IN_SESSION, "recovery: still in the MP session");
    Leave();
    // After recovery the same players can link again.
    Enter(G_A); L_CHECK(MP_LinkJoin(0x03), "recovery: host asks for a new group"); Leave();
    Enter(G_B); MP_LinkLeave(); L_CHECK(MP_LinkJoin(0x03), "recovery: client asks for a new group"); Leave();
    L_CHECK(RunUntil(CondGrouped, 0x3, 0x3), "recovery: regrouped after the link error");
    Report("B: link-error recovery clean-up keeps the session (PS-048)", failuresBefore);

    // BUG-004: both cancel in the trade menu (SetCloseLinkCallback on both). A finishes its
    // close first and leaves the link group at once, while B still has the rows with A's
    // READY_CLOSE_LINK queued. B must still finish its close cleanly: no link error, no hang.
    failuresBefore = sFail;
    Enter(G_A); gMain.callback2 = NULL; Leave();            // forget the error screens of the test above
    Enter(G_B); gMain.callback2 = NULL; Leave();
    DoExchange("BUG-004: player-data exchange");
    L_CHECK(RunUntil(CondTaskFinished, 0x3, 0x3), "BUG-004: link task idle");
    Enter(G_A); SetCloseLinkCallback(); Leave();
    Enter(G_B); SetCloseLinkCallback(); Leave();
    RunFrames(3, 0x3);                                      // both send READY_CLOSE_LINK
    L_CHECK(RunUntil(CondClosed, 0x1, 0x1), "BUG-004: A closes (B not ticking)");
    Enter(G_A); MP_LinkLeave(); Leave();                    // A's exit leaves the group right away
    RunFrames(20, 0x0);                                     // LINK_END reaches B, rows still queued
    L_CHECK(RunUntil(CondClosed, 0x2, 0x2), "BUG-004: B finishes its close");
    Enter(G_B);
    L_CHECK(gMain.callback2 != CB2_LinkError && !gLinkErrorOccurred, "BUG-004: B closed cleanly, no link error");
    Leave();
    Report("B: BUG-004 a partner leaves right after its close: the other still closes cleanly", failuresBefore);

    LeaveAll();
}

// ===========================================================================
// 3. PS-051: the agreed exit of a direct trade (and battle) on the real link stack
// ===========================================================================
//
// What trade.c does for a direct trade when both players cancel in the trade menu: the leader
// (link id 0) decides "both cancelled", queues BOTH_CANCEL (a 20-byte block, sent 5 frames
// later) and agrees (MP_TradeOnBothCancel -> MP_LinkActivityAgreed); the follower agrees when
// the block arrives. Each then fades and closes its own cable (MP_LinkActivityTryFinish), with
// no close handshake. Variations: the follower's game stalled 1..60 frames (its rows pile up
// while the leader closes and the bus ends under it), the leader slower than the follower,
// after a completed trade (block exchange, the vanilla close, re-open on the same group), and
// lag / jitter (the follower's network pump withheld at random, so control messages and rows
// arrive late and in bursts; channel 0 is reliable and ordered, so they cannot reorder).
// Required: no link error (not even a suppressed one) and no trace dump on either side, the
// follower gets BOTH_CANCEL intact, both cables close, the group ends with DONE on both.

struct ExitCase
{
    const char *name;
    u8 followerStall;   // follower game ticks start this many frames after the leader's decision
    u8 leaderFade;      // frames from the decision until the leader tries to close
    u8 followerFade;    // frames from BOTH_CANCEL until the follower tries to close
    bool8 afterTrade;   // a completed trade (block round + vanilla close + re-open) first
    u8 jitterPct;       // % of frames the follower's network pump is withheld
};

static const struct ExitCase sExitCases[] =
{
    { "follower slower by 1 frame",                 1, 16, 16, FALSE, 0 },
    { "follower slower by 5 frames",                5, 16, 16, FALSE, 0 },
    { "follower slower by 20 frames",              20,  2, 16, FALSE, 0 },
    { "follower slower by 60 frames, leader quick",60,  0, 16, FALSE, 0 },
    { "leader slower (follower closes first)",      0, 60,  2, FALSE, 0 },
    { "after a completed trade, follower slower",  30, 16, 16, TRUE,  0 },
    { "after a completed trade, leader slower",     0, 40,  0, TRUE,  0 },
    { "lag and jitter",                            10, 16, 16, FALSE, 30 },
    { "after a trade, slow follower, heavy jitter",45,  0,  5, TRUE,  50 },
    { "leader slower, jitter",                      0, 50,  1, FALSE, 40 },
};

struct ExitSim
{
    bool8 agreed;
    bool8 closed;
    bool8 gotCancel;
    u16 fadeLeft;
};

static struct ExitSim sExit[G_COUNT];

// One frame: the network (the follower's pump maybe withheld), then the games in tickMask.
static void ExitFrame(u8 jitterPct, u8 followerSession, u8 tickMask)
{
    u8 i, g;

    for (i = 0; i < S_COUNT; i++)
    {
        if (jitterPct != 0 && i == followerSession && (Rand() % 100) < jitterPct)
            continue;
        Sel(i);
        MP_Update();
        DrainEvents();
    }
    for (g = 0; g < G_COUNT; g++)
    {
        if (tickMask & (1 << g))
            TickGame(g);
    }
}

// The trade menu's side of one game, after its tick: agree on BOTH_CANCEL, fade, close.
static void ExitStep(u8 g)
{
    struct ExitSim *x = &sExit[g];

    Enter(g);
    if (g == G_B && !x->agreed && (GetBlockReceivedStatus() & 1))
    {
        x->gotCancel = (gBlockRecvBuffer[0][0] == LINKCMD_BOTH_CANCEL_TRADE && gBlockRecvBuffer[0][1] == 0);
        ResetBlockReceivedFlags();
        MP_LinkActivityAgreed("selftest follower: BOTH_CANCEL received");
        x->agreed = TRUE;
    }
    if (g == G_A && (GetBlockReceivedStatus() & 1))
        ResetBlockReceivedFlags(); // our own block's echo
    if (x->agreed && !x->closed)
    {
        if (x->fadeLeft > 0)
            x->fadeLeft--;
        else if (MP_LinkActivityTryFinish())
            x->closed = TRUE;
    }
    Leave();
}

// Diagnostics: says at which stage a game first reached CB2_LinkError (once per game per case).
static u8 sDiagSeen;

static void ExitDiag(const char *caseName, const char *stage, u32 frame)
{
    u8 g;

    for (g = 0; g < G_COUNT; g++)
    {
        Enter(g);
        if (gMain.callback2 == CB2_LinkError && !(sDiagSeen & (1 << g)))
        {
            sDiagSeen |= (u8)(1 << g);
            MP_LOG("selftest: link: PS-051 diag: case '%s': %s reached CB2_LinkError %s (frame %u; status 0x%08X, err %u, hw %u, link state %u, remote players %u, suppress %u, mp state %u end %u, finishing %u, rejects %u, dumps %u)\n",
                   caseName, g == G_A ? "the leader" : "the follower", stage, (unsigned)frame, (unsigned)gLinkStatus,
                   (unsigned)gLinkErrorOccurred, (unsigned)gLink.hardwareError, (unsigned)gLink.state, (unsigned)gReceivedRemoteLinkPlayers,
                   (unsigned)gSuppressLinkErrorMessage, (unsigned)MP_LinkGetState(), (unsigned)MP_LinkGetEndReason(),
                   (unsigned)MP_LinkIsFinishing(), (unsigned)MP_LinkGetRejectCount(), (unsigned)MP_LinkTraceGetDumpCount());
        }
        Leave();
    }
}

static void RunExitCase(const struct ExitCase *c, u8 followerSession, u8 mask)
{
    u32 failuresBefore = sFail;
    u32 dumpsBefore;
    u32 start, frame = 0;
    u16 linkData[10];
    u8 sendIn = 5; // QUEUE_DELAY_DATA
    u8 g;

    sDiagSeen = 0;
    for (g = 0; g < G_COUNT; g++)
    {
        Enter(g);
        gMain.callback2 = NULL;
        SetSuppressLinkErrorMessage(FALSE);  // the field return does this after every exit
        gLinkErrorOccurred = FALSE;
        L_CHECK(MP_LinkJoin(mask), "PS-051: join for the case");
        Leave();
    }
    L_CHECK(RunUntil(CondGrouped, 0x3, 0x3), "PS-051: grouped");
    ExitDiag(c->name, "while grouping (before the exchange)", 0);
    DoExchange("PS-051: player-data exchange");
    ExitDiag(c->name, "during the player-data exchange", 0);
    if (c->afterTrade)
    {
        DoBlockRound(7);    // the trade's data
        DoClose();          // CB2_SaveAndEndTrade's close (vanilla handshake, both in step)
        DoExchange("PS-051: the trade menu re-opens the link after the trade");
        ExitDiag(c->name, "during the completed trade", 0);
    }
    L_CHECK(RunUntil(CondTaskFinished, 0x3, 0x3), "PS-051: link idle in the trade menu");
    ExitDiag(c->name, "before the exit", 0);

    MpLink_TraceSelftestEndIncident();
    dumpsBefore = MP_LinkTraceGetDumpCount();
    memset(sExit, 0, sizeof(sExit));
    sExit[G_A].agreed = TRUE;
    sExit[G_A].fadeLeft = c->leaderFade;
    sExit[G_B].fadeLeft = c->followerFade;
    Enter(G_A);
    MP_LinkActivityAgreed("selftest leader: both cancelled"); // Leader_HandleCommunication
    Leave();
    memset(linkData, 0, sizeof(linkData));
    linkData[0] = LINKCMD_BOTH_CANCEL_TRADE;

    start = MpTransport_GetTimeMs();
    while ((u32)(MpTransport_GetTimeMs() - start) < LINK_TIME_LIMIT_MS)
    {
        bool8 done = TRUE;

        ExitFrame(c->jitterPct, followerSession, (frame >= c->followerStall) ? 0x3 : 0x1);
        if (sendIn != 0 && --sendIn == 0)
        {
            Enter(G_A);
            L_CHECK(SendBlock(0, linkData, sizeof(linkData)), "PS-051: the leader's BOTH_CANCEL block starts");
            Leave();
        }
        // The leader's fade / exit only starts once the trade menu's queue sent the block
        // (CB_InitExitCanceledTrade checks GetNumQueuedActions() == 0).
        if (sendIn == 0)
            ExitStep(G_A);
        if (frame >= c->followerStall)
            ExitStep(G_B);
        frame++;
        ExitDiag(c->name, sExit[G_A].closed ? "during the exit (leader closed)" : "during the exit (leader open)", frame);
        if (sDiagSeen != 0)
            break; // fail fast; the checks below say why
        for (g = 0; g < G_COUNT; g++)
        {
            Enter(g);
            if (!sExit[g].closed || MP_LinkGetState() != MP_LINK_STATE_NONE)
                done = FALSE;
            Leave();
        }
        if (done)
            break;
    }
    // A few more frames: nothing may go wrong after the exit either.
    RunFrames(10, 0x3);
    ExitDiag(c->name, "after the exit", frame);

    L_CHECK(sExit[G_B].gotCancel, "PS-051: the follower received BOTH_CANCEL intact");
    for (g = 0; g < G_COUNT; g++)
    {
        Enter(g);
        L_CHECK(sExit[g].closed && !gReceivedRemoteLinkPlayers, g == G_A ? "PS-051: the leader closed its cable" : "PS-051: the follower closed its cable");
        L_CHECK(gMain.callback2 != CB2_LinkError, g == G_A ? "PS-051: no CB2_LinkError on the leader" : "PS-051: no CB2_LinkError on the follower");
        L_CHECK(!gLinkErrorOccurred, g == G_A ? "PS-051: no link error at all on the leader (not even suppressed)" : "PS-051: no link error at all on the follower (not even suppressed)");
        L_CHECK(MP_LinkGetState() == MP_LINK_STATE_NONE && MP_LinkGetEndReason() == MP_LINK_END_DONE,
                g == G_A ? "PS-051: the leader's group ended with DONE" : "PS-051: the follower's group ended with DONE");
        L_CHECK(MP_GetState() == MP_STATE_IN_SESSION, "PS-051: the session is unaffected");
        if (sFail != failuresBefore && g == G_B)
            MP_LOG("selftest: link: PS-051 case '%s': leader closed %u, follower closed %u, follower link state %u end %u, frames %u\n",
                   c->name, (unsigned)sExit[G_A].closed, (unsigned)sExit[G_B].closed, (unsigned)MP_LinkGetState(),
                   (unsigned)MP_LinkGetEndReason(), (unsigned)frame);
        Leave();
    }
    L_CHECK(MP_LinkTraceGetDumpCount() == dumpsBefore, "PS-051: no link trace dump (no incident) during the agreed exit");
    {
        char what[128];

        snprintf(what, sizeof(what), "PS-051 agreed trade exit: %s", c->name);
        Report(what, failuresBefore);
    }
    // Independent cases: whatever happened, nothing of this case's link is left for the next one.
    for (g = 0; g < G_COUNT; g++)
    {
        Enter(g);
        CloseLink();
        MP_LinkLeave();
        gMain.callback2 = NULL;
        gLinkErrorOccurred = FALSE;
        SetSuppressLinkErrorMessage(FALSE);
        Leave();
    }
    RunFrames(5, 0x3);
    if (sFail != failuresBefore)
    {
        // A failed case may leave link.c anywhere: the next case starts from a freshly booted
        // link.c. (A passing case keeps its state, so a leftover from a clean exit still shows.)
        for (g = 0; g < G_COUNT; g++)
        {
            memcpy(sGames[g].link, sNeutralLink, sizeof(sGames[g].link));
            sGames[g].callback2 = NULL;
            sGames[g].advanceIn = 0;
        }
    }
    MpLink_TraceSelftestEndIncident();
}

static void ScenarioAgreedExit(bool8 hostIsMember)
{
    u32 failuresBefore = sFail;
    u32 dumpsBefore, i;
    u8 mask, followerSession;
    u8 g;

    if (hostIsMember)
    {
        InitGame(G_A, S_HOST, "HOST", 0x00C0FFEEu);
        InitGame(G_B, S_C1, "CARL", 0x00012345u);
        mask = 0x03;
        followerSession = S_C1;
    }
    else
    {
        InitGame(G_A, S_C1, "ALICE", 0x0001A11Cu);
        InitGame(G_B, S_C2, "BOB", 0x000B0B0Bu);
        mask = 0x06;
        followerSession = S_C2;
    }
    L_CHECK(HostOn(S_HOST, "Host"), "PS-051: host on a port in 27550..27559");
    L_CHECK(JoinOn(S_C1, "Carl") && WaitSession(S_C1), "PS-051: client 1 joins");
    if (!hostIsMember)
        L_CHECK(JoinOn(S_C2, "Bob") && WaitSession(S_C2), "PS-051: client 2 joins");
    PumpNet();
    Report(hostIsMember ? "PS-051 set-up (host is the leader)" : "PS-051 set-up (host only sequences)", failuresBefore);

    if (hostIsMember)
    {
        for (i = 0; i < ARRAY_COUNT(sExitCases); i++)
            RunExitCase(&sExitCases[i], followerSession, mask);
    }
    else
    {
        RunExitCase(&sExitCases[3], followerSession, mask);
        RunExitCase(&sExitCases[4], followerSession, mask);
        RunExitCase(&sExitCases[8], followerSession, mask);
    }

    // Part A: the flight recorder dumps once for a forced error (MP_LinkRejectData on the
    // leader, then the hardware error, the link error and the follower's bus end: one incident).
    failuresBefore = sFail;
    for (g = 0; g < G_COUNT; g++)
    {
        Enter(g);
        gMain.callback2 = NULL;
        gLinkErrorOccurred = FALSE;
        L_CHECK(MP_LinkJoin(mask), "PS-051 trace: join");
        Leave();
    }
    L_CHECK(RunUntil(CondGrouped, 0x3, 0x3), "PS-051 trace: grouped");
    DoExchange("PS-051 trace: player-data exchange");
    MpLink_TraceSelftestEndIncident();
    dumpsBefore = MP_LinkTraceGetDumpCount();
    Enter(G_A);
    MP_LinkRejectData("selftest: a forced error (PS-051 flight recorder)");
    Leave();
    L_CHECK(MP_LinkTraceGetDumpCount() == dumpsBefore + 1, "PS-051 trace: a forced error dumps the trace at once");
    L_CHECK(RunUntil(CondLinkError, 0x3, 0x3), "PS-051 trace: both reach the link-error path (no agreement)");
    RunFrames(10, 0x3);
    L_CHECK(MP_LinkTraceGetDumpCount() == dumpsBefore + 1, "PS-051 trace: one dump per incident (the follow-up errors are the same incident)");
    for (g = 0; g < G_COUNT; g++)
    {
        Enter(g);
        CloseLink();
        MP_LinkLeave();
        gMain.callback2 = NULL;
        gLinkErrorOccurred = FALSE;
        SetSuppressLinkErrorMessage(FALSE);
        Leave();
    }
    RunFrames(10, 0x3);
    MpLink_TraceSelftestEndIncident();
    Report("PS-051 flight recorder: one dump on a forced error", failuresBefore);

    LeaveAll();
}

// ===========================================================================
// 4. PS-052: the fast cable on the real link stack
// ===========================================================================

enum
{
    NET_CLEAN,
    NET_JITTER,     // the laggy session's network pump is withheld 40 % of the frames
    NET_LAG,        // the laggy session's network pump runs every 6th frame (about 100 ms at 60 fps)
};

static const char *const sNetNames[] = { "clean", "jitter", "lag" };

// One frame: the network (one session's pump maybe withheld), then both games tick.
static void NetFrame(u8 net, u8 laggySession, u32 frame)
{
    u8 i, g;

    for (i = 0; i < S_COUNT; i++)
    {
        if (i == laggySession && ((net == NET_JITTER && (Rand() % 100) < 40) || (net == NET_LAG && (frame % 6) != 0)))
            continue;
        Sel(i);
        MP_Update();
        DrainEvents();
    }
    for (g = 0; g < G_COUNT; g++)
        TickGame(g);
}

static bool8 SetUpPair(bool8 hostIsMember, u8 *mask, u8 *laggySession)
{
    bool8 ok = TRUE;

    if (hostIsMember)
    {
        InitGame(G_A, S_HOST, "HOST", 0x00C0FFEEu);
        InitGame(G_B, S_C1, "CARL", 0x00012345u);
        *mask = 0x03;
        *laggySession = S_C1;
    }
    else
    {
        InitGame(G_A, S_C1, "ALICE", 0x0001A11Cu);
        InitGame(G_B, S_C2, "BOB", 0x000B0B0Bu);
        *mask = 0x06;
        *laggySession = S_C2;
    }
    ok = ok && HostOn(S_HOST, "Host");
    ok = ok && JoinOn(S_C1, "Carl") && WaitSession(S_C1);
    if (!hostIsMember)
        ok = ok && JoinOn(S_C2, "Bob") && WaitSession(S_C2);
    PumpNet();
    return ok;
}

// Join the group, open the cable and run the player-data exchange; the link task is then idle.
static bool8 LinkUp(u8 mask, const char *what)
{
    u32 failuresBefore = sFail;
    u8 g;

    for (g = 0; g < G_COUNT; g++)
    {
        Enter(g);
        gMain.callback2 = NULL;
        gLinkErrorOccurred = FALSE;
        SetSuppressLinkErrorMessage(FALSE);
        L_CHECK(MP_LinkJoin(mask), "PS-052: join");
        Leave();
    }
    L_CHECK(RunUntil(CondGrouped, 0x3, 0x3), "PS-052: grouped");
    DoExchange(what);
    L_CHECK(RunUntil(CondTaskFinished, 0x3, 0x3), "PS-052: link task idle after the exchange");
    for (g = 0; g < G_COUNT; g++)
    {
        Enter(g);
        ResetBlockReceivedFlags();
        Leave();
    }
    return sFail == failuresBefore;
}

// Close both cables and leave the group, whatever happened; a failed case restarts link.c.
static void LinkDown(u32 failuresBefore)
{
    u8 g;

    for (g = 0; g < G_COUNT; g++)
    {
        Enter(g);
        CloseLink();
        MP_LinkLeave();
        gMain.callback2 = NULL;
        gLinkErrorOccurred = FALSE;
        SetSuppressLinkErrorMessage(FALSE);
        Leave();
    }
    RunFrames(5, 0x3);
    if (sFail != failuresBefore)
    {
        for (g = 0; g < G_COUNT; g++)
        {
            memcpy(sGames[g].link, sNeutralLink, sizeof(sGames[g].link));
            sGames[g].callback2 = NULL;
            sGames[g].advanceIn = 0;
        }
    }
    MpLink_TraceSelftestEndIncident();
}

// --- 4a. Blocks of every size interleaved with rows: intact and in order ---------------------

#define STREAM_MARKER           0x3A00  // + sender link id: not a LINKCMD, link.c ignores it
#define STREAM_MAX_EVENTS       48
#define STREAM_TIME_LIMIT_MS    20000

// A sends every size, the big ones included (they arrive in gDecompressionBuffer, which the two
// game instances of this process share; only A sends them, so both sides see A's data there).
static const u16 sStreamSizesA[] =
{
    1, 2, 13, 14, 15, 27, 28, 29, 100, 200, 254, 255, BLOCK_BUFFER_SIZE, BLOCK_BUFFER_SIZE + 1, 300, 499,
    MP_LINK_BLOCK_MAX, MP_LINK_BLOCK_MAX + 1, 0x800, MP_LINK_MAX_BLOCK_SIZE,
};
static const u16 sStreamSizesB[] = { 1, 14, 15, 28, 64, 128, 255, BLOCK_BUFFER_SIZE, 7, 3 };

struct StreamSide
{
    u16 events[STREAM_MAX_EVENTS];  // block size, 0 = a marker command row
    u8 count;
    u8 sent;
    u8 recvIdx[G_COUNT];            // [receiving game] the next event expected from this sender
};

static struct StreamSide sStream[G_COUNT];
static u32 sStreamBad;
static u8 sBigSrc[MP_LINK_MAX_BLOCK_SIZE + 16]; // SendBlock above 0x100 bytes sends from the caller's buffer (LinkCB_BlockSend reads up to 13 bytes past it)
static u8 sSavedDecompression[MP_LINK_MAX_BLOCK_SIZE];

static u8 StreamByte(u8 sender, u8 seq, u32 i)
{
    return (u8)(sender * 0x5Bu + seq * 0x1Du + i * 7u + (i >> 7));
}

static u16 StreamWord(u8 sender, u8 seq, u32 j)
{
    return (u16)(0x1000u * sender + seq * 0x31u + j);
}

static void StreamBad(const char *what, u8 receiver, u8 sender, u32 idx)
{
    if (sStreamBad < 10)
        MP_LOG("selftest: FAIL: link: PS-052 stream: %s (game %u from link id %u, event %u)\n", what, (unsigned)receiver, (unsigned)sender, (unsigned)idx);
    sStreamBad++;
}

static void StreamObserver(bool8 isBlock, u8 who, const u16 (*row)[MP_LINK_CMD_WORDS], u16 size)
{
    u8 r, s;
    u32 i, idx;

    if (sEntered < 0 || sEntered >= G_COUNT)
        return;
    r = (u8)sEntered;
    if (isBlock)
    {
        const u8 *data;

        if (who >= G_COUNT)
        {
            StreamBad("a block from a link id not in the group", r, who, 0);
            return;
        }
        idx = sStream[who].recvIdx[r];
        if (idx >= sStream[who].count || sStream[who].events[idx] != size)
        {
            StreamBad("a block out of order or of the wrong size", r, who, idx);
            return;
        }
        data = (size > BLOCK_BUFFER_SIZE) ? (const u8 *)gDecompressionBuffer : (const u8 *)gBlockRecvBuffer[who];
        for (i = 0; i < size; i++)
        {
            if (data[i] != StreamByte(who, (u8)idx, i))
            {
                StreamBad("a block's data is not intact", r, who, idx);
                break;
            }
        }
        sStream[who].recvIdx[r]++;
        return;
    }
    for (s = 0; s < G_COUNT; s++)
    {
        u16 w0 = row[s][0];

        if (w0 == 0 || w0 == LINKCMD_INIT_BLOCK || w0 == LINKCMD_CONT_BLOCK)
            continue;
        idx = sStream[s].recvIdx[r];
        if (w0 != STREAM_MARKER + s)
        {
            StreamBad("an unexpected command in a row", r, s, idx);
            continue;
        }
        if (idx >= sStream[s].count || sStream[s].events[idx] != 0 || row[s][1] != idx)
        {
            StreamBad("a command row out of order", r, s, idx);
            continue;
        }
        for (i = 2; i < MP_LINK_CMD_WORDS; i++)
        {
            if (row[s][i] != StreamWord(s, (u8)idx, i))
            {
                StreamBad("a command row's words are not intact", r, s, idx);
                break;
            }
        }
        sStream[s].recvIdx[r]++;
    }
}

static void StreamScript(u8 g, const u16 *sizes, u32 count)
{
    struct StreamSide *x = &sStream[g];
    u32 i;

    memset(x, 0, sizeof(*x));
    for (i = 0; i < count && x->count + 2 < STREAM_MAX_EVENTS; i++)
    {
        x->events[x->count++] = 0;          // a command row
        x->events[x->count++] = sizes[i];   // then a block
    }
    x->events[x->count++] = 0;
}

// TRUE once every game has received the sender's events before `upTo`.
static bool8 StreamAllReceived(u8 sender, u32 upTo)
{
    u8 r;

    for (r = 0; r < G_COUNT; r++)
    {
        if (sStream[sender].recvIdx[r] < upTo)
            return FALSE;
    }
    return TRUE;
}

// The game side of one sender, after its tick: the next event once the link task is idle.
static void StreamSend(u8 g)
{
    struct StreamSide *x = &sStream[g];
    u16 size;
    u32 i;

    // A test artefact, not a game rule: the two game instances share gDecompressionBuffer, so a
    // block above 0x100 bytes waits until both have taken the previous one (else one side's next
    // block would overwrite the other side's buffer before it is checked).
    if (x->sent < x->count && x->events[x->sent] > BLOCK_BUFFER_SIZE && !StreamAllReceived(g, x->sent))
        return;
    Enter(g);
    if (x->sent < x->count && IsLinkTaskFinished())
    {
        size = x->events[x->sent];
        if (size == 0)
        {
            gSendCmd[0] = STREAM_MARKER + g;
            gSendCmd[1] = x->sent;
            for (i = 2; i < CMD_LENGTH; i++)
                gSendCmd[i] = StreamWord(g, x->sent, i);
            x->sent++;
        }
        else
        {
            u8 *src = (size > BLOCK_BUFFER_SIZE) ? sBigSrc : gBlockSendBuffer;

            for (i = 0; i < size; i++)
                src[i] = StreamByte(g, x->sent, i);
            if (SendBlock(0, src, size))
                x->sent++;
            else
                StreamBad("SendBlock refused while the link task was idle", g, g, x->sent);
        }
    }
    Leave();
}

static bool8 StreamDone(void)
{
    u8 s, r;

    for (s = 0; s < G_COUNT; s++)
    {
        for (r = 0; r < G_COUNT; r++)
        {
            if (sStream[s].recvIdx[r] < sStream[s].count)
                return FALSE;
        }
    }
    return TRUE;
}

static void RunStreamCase(bool8 hostIsMember, bool8 fast, u8 net, bool8 slowConsumer)
{
    struct MpLinkCableStats before, after;
    u32 failuresBefore = sFail;
    u32 rejectsBefore = MP_LinkGetRejectCount();
    u32 start, frame = 0;
    u32 fastA = 0, rowsA = 0, i;
    u8 mask, laggy, g;
    char what[160];

    snprintf(what, sizeof(what), "PS-052 blocks 1..0x4000 + rows, in order and intact: %s, %s, %s%s",
             hostIsMember ? "host is a member" : "host only sequences", fast ? "whole blocks" : "rows only (fallback)",
             sNetNames[net], slowConsumer ? ", slow consumer" : "");
    L_CHECK(SetUpPair(hostIsMember, &mask, &laggy), "PS-052: session set-up");
    MP_LinkSetFastBlocks(fast);
    LinkUp(mask, "PS-052: player-data exchange before the stream");
    StreamScript(G_A, sStreamSizesA, ARRAY_COUNT(sStreamSizesA));
    StreamScript(G_B, sStreamSizesB, ARRAY_COUNT(sStreamSizesB));
    for (i = 0; i < ARRAY_COUNT(sStreamSizesA); i++)
    {
        if (fast && sStreamSizesA[i] <= MP_LINK_BLOCK_MAX)
            fastA++;
        else
            rowsA++;
    }
    sStreamBad = 0;
    memcpy(sSavedDecompression, gDecompressionBuffer, sizeof(sSavedDecompression));
    Enter(G_A);
    MpLink_GetCableStats(&before);
    Leave();
    MpLink_SetSelftestObserver(StreamObserver);

    start = MpTransport_GetTimeMs();
    while (!StreamDone() && (u32)(MpTransport_GetTimeMs() - start) < STREAM_TIME_LIMIT_MS && sStreamBad == 0)
    {
        bool8 linkError = FALSE;

        NetFrame(net, laggy, frame);
        for (g = 0; g < G_COUNT; g++)
        {
            Enter(g);
            if (gMain.callback2 == CB2_LinkError || (gLinkStatus & LINK_STAT_ERRORS))
                linkError = TRUE;
            // The consumer: like the battle's VBlank copy every frame, or a slow menu every 7th.
            if (!slowConsumer || (frame % 7) == 0)
                ResetBlockReceivedFlags();
            Leave();
        }
        if (linkError)
            break;
        StreamSend(G_A);
        StreamSend(G_B);
        frame++;
    }
    MpLink_SetSelftestObserver(NULL);

    L_CHECK(sStreamBad == 0, "PS-052: every block and command row arrived intact and in order");
    L_CHECK(StreamDone(), "PS-052: both sides received every event of both senders (their own included)");
    for (g = 0; g < G_COUNT; g++)
    {
        Enter(g);
        L_CHECK(gMain.callback2 != CB2_LinkError && !gLinkErrorOccurred && !(gLinkStatus & LINK_STAT_ERRORS), "PS-052: no link error during the stream");
        Leave();
    }
    L_CHECK(MP_LinkGetRejectCount() == rejectsBefore, "PS-052: nothing was rejected");
    Enter(G_A);
    MpLink_GetCableStats(&after);
    Leave();
    L_CHECK(after.blocksSentFast - before.blocksSentFast == fastA, "PS-052: A sent the blocks up to MP_LINK_BLOCK_MAX whole (when on)");
    L_CHECK(after.blocksSentRows - before.blocksSentRows == rowsA, "PS-052: A sent the bigger blocks (or all, when off) as rows");
    if (sFail != failuresBefore)
        MP_LOG("selftest: link: PS-052 stream: frames %u; A received A %u/%u B %u/%u; B received A %u/%u B %u/%u\n", (unsigned)frame,
               (unsigned)sStream[G_A].recvIdx[G_A], (unsigned)sStream[G_A].count, (unsigned)sStream[G_B].recvIdx[G_A], (unsigned)sStream[G_B].count,
               (unsigned)sStream[G_A].recvIdx[G_B], (unsigned)sStream[G_A].count, (unsigned)sStream[G_B].recvIdx[G_B], (unsigned)sStream[G_B].count);
    else
        MP_LOG("selftest: link: PS-052 stream (%s): %u frames; A: rows %u (catch-up %u), held %u ticks\n", what, (unsigned)frame,
               (unsigned)(after.rowsPlayed - before.rowsPlayed), (unsigned)(after.catchUpRows - before.catchUpRows),
               (unsigned)(after.holdTicks - before.holdTicks));
    memcpy(gDecompressionBuffer, sSavedDecompression, sizeof(sSavedDecompression));
    LinkDown(failuresBefore);
    MP_LinkSetFastBlocks(TRUE);
    LeaveAll();
    Report(what, failuresBefore);
}

// LINK_BLOCK parser cases (protocol.md 8.3).
static void TestBlockParser(void)
{
    static struct MpMsgLinkBlock sIn, sOut;
    static u8 sBuf[MP_MAX_PACKET_SIZE + 8];
    u32 failuresBefore = sFail;
    struct MpMsgHeader hdr;
    u16 len, size;
    u32 i, k;

    memset(&sIn, 0, sizeof(sIn));
    sIn.groupId = 3;
    sIn.busEpoch = 9;
    sIn.linkId = 1;
    for (size = 1; size <= MP_LINK_BLOCK_MAX; size = (size < 20) ? size + 1 : size + 37)
    {
        sIn.size = size;
        for (i = 0; i < size; i++)
            sIn.data[i] = (u8)(i * 11 + size);
        len = MpProto_WriteLinkBlock(sBuf, MP_MAX_PACKET_SIZE, &sIn);
        L_CHECK(len == MP_HEADER_SIZE + MP_LINK_BLOCK_SIZE(size) && len <= MP_MAX_PACKET_SIZE, "LINK_BLOCK: written length");
        L_CHECK(MpProto_ParseHeader(sBuf, len, &hdr) && hdr.type == MP_MSG_LINK_BLOCK, "LINK_BLOCK: header");
        L_CHECK(MpProto_ParseLinkBlock(sBuf + MP_HEADER_SIZE, len - MP_HEADER_SIZE, &sOut) == MP_PARSE_OK
             && sOut.size == size && sOut.groupId == 3 && sOut.busEpoch == 9 && sOut.linkId == 1
             && memcmp(sOut.data, sIn.data, size) == 0, "LINK_BLOCK: round trip");
        L_CHECK(MpProto_ParseLinkBlock(sBuf + MP_HEADER_SIZE, len - MP_HEADER_SIZE - 1, &sOut) == MP_PARSE_ERR_TRUNCATED, "LINK_BLOCK: one byte short");
        sBuf[len] = 0;
        L_CHECK(MpProto_ParseLinkBlock(sBuf + MP_HEADER_SIZE, len - MP_HEADER_SIZE + 1, &sOut) == MP_PARSE_ERR_OVERSIZED, "LINK_BLOCK: one byte long");
    }
    sIn.size = MP_LINK_BLOCK_MAX;
    len = MpProto_WriteLinkBlock(sBuf, MP_MAX_PACKET_SIZE, &sIn);
    L_CHECK(len == MP_MAX_PACKET_SIZE, "LINK_BLOCK: the biggest whole block fills a packet exactly");
    sIn.size = 0;
    L_CHECK(MpProto_WriteLinkBlock(sBuf, MP_MAX_PACKET_SIZE, &sIn) == 0, "LINK_BLOCK: size 0 is not written");
    sIn.size = MP_LINK_BLOCK_MAX + 1;
    L_CHECK(MpProto_WriteLinkBlock(sBuf, sizeof(sBuf), &sIn) == 0, "LINK_BLOCK: a block above MP_LINK_BLOCK_MAX is not written");
    // Targeted bad fields on a valid 10-byte block.
    sIn.size = 10;
    len = MpProto_WriteLinkBlock(sBuf, MP_MAX_PACKET_SIZE, &sIn);
    {
        u8 *p = sBuf + MP_HEADER_SIZE;
        u32 n = len - MP_HEADER_SIZE;

        p[0] = 0;  L_CHECK(MpProto_ParseLinkBlock(p, n, &sOut) == MP_PARSE_ERR_BAD_FIELD, "LINK_BLOCK: groupId 0"); p[0] = 3;
        p[1] = 0;  L_CHECK(MpProto_ParseLinkBlock(p, n, &sOut) == MP_PARSE_ERR_BAD_FIELD, "LINK_BLOCK: busEpoch 0"); p[1] = 9;
        p[2] = 4;  L_CHECK(MpProto_ParseLinkBlock(p, n, &sOut) == MP_PARSE_ERR_BAD_FIELD, "LINK_BLOCK: link id 4"); p[2] = 1;
        p[4] = 0; p[5] = 0; L_CHECK(MpProto_ParseLinkBlock(p, n, &sOut) == MP_PARSE_ERR_BAD_FIELD, "LINK_BLOCK: size 0");
        p[4] = (u8)(MP_LINK_BLOCK_MAX + 1); p[5] = (u8)((MP_LINK_BLOCK_MAX + 1) >> 8);
        L_CHECK(MpProto_ParseLinkBlock(p, n, &sOut) == MP_PARSE_ERR_BAD_FIELD, "LINK_BLOCK: size above MP_LINK_BLOCK_MAX");
        p[4] = 0xFF; p[5] = 0xFF; L_CHECK(MpProto_ParseLinkBlock(p, n, &sOut) == MP_PARSE_ERR_BAD_FIELD, "LINK_BLOCK: size 0xFFFF");
        p[4] = 11; p[5] = 0; L_CHECK(MpProto_ParseLinkBlock(p, n, &sOut) == MP_PARSE_ERR_TRUNCATED, "LINK_BLOCK: size larger than the payload");
        p[4] = 9;  L_CHECK(MpProto_ParseLinkBlock(p, n, &sOut) == MP_PARSE_ERR_OVERSIZED, "LINK_BLOCK: size smaller than the payload");
        p[4] = 10; L_CHECK(MpProto_ParseLinkBlock(p, n, &sOut) == MP_PARSE_OK, "LINK_BLOCK: restored");
        for (i = 0; i < MP_LINK_BLOCK_HEADER; i++)
            L_CHECK(MpProto_ParseLinkBlock(p, i, &sOut) == MP_PARSE_ERR_TRUNCATED, "LINK_BLOCK: shorter than the header");
        L_CHECK(MpProto_ParseLinkBlock(NULL, 0, &sOut) == MP_PARSE_ERR_TRUNCATED, "LINK_BLOCK: NULL");
    }
    // Garbage: whatever parses must be self-consistent.
    for (k = 0; k < 30000; k++)
    {
        u32 n = Rand() % (MP_MAX_PAYLOAD_SIZE + 4);

        for (i = 0; i < n; i++)
            sBuf[i] = (u8)Rand();
        if ((k & 1) && n >= MP_LINK_BLOCK_HEADER)
        {
            sBuf[4] = (u8)(n - MP_LINK_BLOCK_HEADER); // often a matching size
            sBuf[5] = (u8)((n - MP_LINK_BLOCK_HEADER) >> 8);
        }
        if (MpProto_ParseLinkBlock(sBuf, n, &sOut) == MP_PARSE_OK)
            L_CHECK(sOut.size >= 1 && sOut.size <= MP_LINK_BLOCK_MAX && n == (u32)MP_LINK_BLOCK_SIZE(sOut.size)
                 && sOut.groupId != 0 && sOut.busEpoch != 0 && sOut.linkId < MP_LINK_MAX_MEMBERS, "LINK_BLOCK: garbage that parses is consistent");
    }
    Report("PS-052 LINK_BLOCK parser (round trip, sizes, bad fields, garbage)", failuresBefore);
}

// A whole block for a link id or of a size the game must not take never reaches link.c's buffers:
// the member drops it (wrong link id), the host never relays it (size), link.c rejects a size above
// gDecompressionBuffer.
static void TestDeliverBounds(void)
{
    static u8 sData[MP_LINK_BLOCK_MAX];
    u32 failuresBefore = sFail;
    u32 rejects = MP_LinkGetRejectCount();
    u16 canary;

    memset(sData, 0x5A, sizeof(sData));
    InitGame(G_A, S_HOST, "SOLO", 1);
    Enter(G_A);
    gLink.state = LINK_STATE_CONN_ESTABLISHED;
    canary = gBlockRecvBuffer[1][0];
    MP_LinkDeliverWholeBlock(MAX_LINK_PLAYERS, sData, 10);
    MP_LinkDeliverWholeBlock(0xFF, sData, 10);
    MP_LinkDeliverWholeBlock(1, NULL, 10);
    L_CHECK(gBlockRecvBuffer[1][0] == canary && !gBlockReceivedStatus[1], "PS-052: a whole block for a bad link id or with no data is ignored");
    MP_LinkDeliverWholeBlock(1, sData, 0);
    L_CHECK(!gBlockReceivedStatus[1] && MP_LinkGetRejectCount() == rejects + 1, "PS-052: a whole block of size 0 is rejected");
    gRemoteLinkPlayersNotReceived[1] = FALSE;
    MP_LinkDeliverWholeBlock(1, sData, 6);
    L_CHECK(gBlockReceivedStatus[1] && ((u8 *)gBlockRecvBuffer[1])[5] == 0x5A, "PS-052: a valid whole block sets the received flag");
    CloseLink();
    Leave();
    memcpy(sGames[G_A].link, sNeutralLink, sizeof(sGames[G_A].link));
    MpLink_TraceSelftestEndIncident(); // the rejection above dumped the trace once
    Report("PS-052 whole-block delivery bounds", failuresBefore);
}

// --- 4b. Measurements: a battle-like exchange ------------------------------------------------
//
// The link master sends a battle controller command (a 0x38-byte block: 8-byte header + 0x30), the
// slave waits for it and answers with a 12-byte block (header + one return value), the master
// waits for the answer: one exchange, as in a battle turn (CHOOSEACTION -> reply, MOVEANIMATION
// -> controller done, ...). MEASURE_EXCHANGES of them, timed in game frames.

#define MEASURE_EXCHANGES   30
#define MEASURE_CMD_SIZE    0x38
#define MEASURE_REPLY_SIZE  12

struct MeasureResult
{
    u32 frames;
    u32 exchanges;
    struct MpLinkCableStats a;
    struct MpLinkCableStats b;
};

static void MeasureFill(u8 *dst, u16 size, u8 seq, u8 who)
{
    u32 i;

    for (i = 0; i < size; i++)
        dst[i] = (u8)(seq * 3u + who * 101u + i);
}

static bool8 MeasureCheck(const u16 *recv, u16 size, u8 seq, u8 who)
{
    const u8 *p = (const u8 *)recv;
    u32 i;

    for (i = 0; i < size; i++)
    {
        if (p[i] != (u8)(seq * 3u + who * 101u + i))
            return FALSE;
    }
    return TRUE;
}

static void RunMeasure(bool8 hostIsMember, bool8 fast, bool8 catchUp, u8 net, const char *name, struct MeasureResult *res)
{
    struct MpLinkCableStats a0, b0;
    u32 failuresBefore = sFail;
    u32 start;
    u8 mask, laggy, phase = 0, seq = 0;
    bool8 dataOk = TRUE, linkError = FALSE;
    char what[160];

    memset(res, 0, sizeof(*res));
    L_CHECK(SetUpPair(hostIsMember, &mask, &laggy), "PS-052 measure: session set-up");
    MP_LinkSetFastBlocks(fast);
    MP_LinkSetRowCatchUp(catchUp);
    LinkUp(mask, "PS-052 measure: player-data exchange");
    Enter(G_A); MpLink_GetCableStats(&a0); Leave();
    Enter(G_B); MpLink_GetCableStats(&b0); Leave();

    start = MpTransport_GetTimeMs();
    while (res->exchanges < MEASURE_EXCHANGES && (u32)(MpTransport_GetTimeMs() - start) < LINK_TIME_LIMIT_MS && !linkError)
    {
        u8 g;

        NetFrame(net, laggy, res->frames);
        res->frames++;
        for (g = 0; g < G_COUNT; g++)
        {
            Enter(g);
            if (gMain.callback2 == CB2_LinkError || (gLinkStatus & LINK_STAT_ERRORS))
                linkError = TRUE;
            Leave();
        }
        // The games, after their tick (their callbacks).
        Enter(G_A);
        if (GetBlockReceivedStatus() & 1)
            ResetBlockReceivedFlag(0); // our own command's echo
        if (phase == 2 && (GetBlockReceivedStatus() & 2))
        {
            dataOk = dataOk && MeasureCheck(gBlockRecvBuffer[1], MEASURE_REPLY_SIZE, seq, 1);
            ResetBlockReceivedFlag(1);
            res->exchanges++;
            seq++;
            phase = 0;
        }
        if (phase == 0 && res->exchanges < MEASURE_EXCHANGES && IsLinkTaskFinished())
        {
            MeasureFill(gBlockSendBuffer, MEASURE_CMD_SIZE, seq, 0);
            if (SendBlock(0, gBlockSendBuffer, MEASURE_CMD_SIZE))
                phase = 1;
        }
        Leave();
        Enter(G_B);
        if (GetBlockReceivedStatus() & 2)
            ResetBlockReceivedFlag(1); // our own reply's echo
        if (phase == 1 && (GetBlockReceivedStatus() & 1) && IsLinkTaskFinished())
        {
            dataOk = dataOk && MeasureCheck(gBlockRecvBuffer[0], MEASURE_CMD_SIZE, seq, 0);
            ResetBlockReceivedFlag(0);
            MeasureFill(gBlockSendBuffer, MEASURE_REPLY_SIZE, seq, 1);
            if (SendBlock(0, gBlockSendBuffer, MEASURE_REPLY_SIZE))
                phase = 2;
        }
        Leave();
    }
    Enter(G_A); MpLink_GetCableStats(&res->a); Leave();
    Enter(G_B); MpLink_GetCableStats(&res->b); Leave();
    // Only this exchange (the player-data exchange ran on the same bus).
    res->a.blocksSentFast -= a0.blocksSentFast;  res->a.blocksSentRows -= a0.blocksSentRows;
    res->a.ownBlocks -= a0.ownBlocks;            res->a.ownBlockTicks -= a0.ownBlockTicks;
    res->a.rowsPlayed -= a0.rowsPlayed;          res->a.catchUpRows -= a0.catchUpRows;
    res->a.idleTicks -= a0.idleTicks;            res->a.waitTicks -= a0.waitTicks;
    res->b.rowsPlayed -= b0.rowsPlayed;          res->b.catchUpRows -= b0.catchUpRows;
    res->b.ownBlocks -= b0.ownBlocks;            res->b.ownBlockTicks -= b0.ownBlockTicks;
    res->b.idleTicks -= b0.idleTicks;            res->b.waitTicks -= b0.waitTicks;

    snprintf(what, sizeof(what), "PS-052 measure: %s", name);
    L_CHECK(res->exchanges == MEASURE_EXCHANGES, "PS-052 measure: every exchange completed in time");
    L_CHECK(dataOk, "PS-052 measure: every command and answer intact");
    L_CHECK(!linkError, "PS-052 measure: no link error");
    {
        u32 per10 = (res->exchanges != 0) ? res->frames * 10 / res->exchanges : 0;
        u32 ownA10 = (res->a.ownBlocks != 0) ? res->a.ownBlockTicks * 10 / res->a.ownBlocks : 0;
        u32 ownB10 = (res->b.ownBlocks != 0) ? res->b.ownBlockTicks * 10 / res->b.ownBlocks : 0;

        MP_LOG("selftest: link: PS-052 measure [%s]: %u exchanges in %u frames = %u.%u frames each; "
               "master: rows %u (catch-up %u), blocks %u whole + %u as rows, own block back after %u.%u ticks, nothing received %u ticks (%u waiting); "
               "slave: rows %u (catch-up %u), own block back after %u.%u ticks, nothing received %u ticks (%u waiting)\n",
               name, (unsigned)res->exchanges, (unsigned)res->frames, (unsigned)(per10 / 10), (unsigned)(per10 % 10),
               (unsigned)res->a.rowsPlayed, (unsigned)res->a.catchUpRows, (unsigned)res->a.blocksSentFast, (unsigned)res->a.blocksSentRows,
               (unsigned)(ownA10 / 10), (unsigned)(ownA10 % 10), (unsigned)res->a.idleTicks, (unsigned)res->a.waitTicks,
               (unsigned)res->b.rowsPlayed, (unsigned)res->b.catchUpRows, (unsigned)(ownB10 / 10), (unsigned)(ownB10 % 10),
               (unsigned)res->b.idleTicks, (unsigned)res->b.waitTicks);
    }
    LinkDown(failuresBefore);
    MP_LinkSetFastBlocks(TRUE);
    MP_LinkSetRowCatchUp(TRUE);
    LeaveAll();
    Report(what, failuresBefore);
}

static void ScenarioFastCable(void)
{
    struct MeasureResult rows, catchUp, fast;
    u32 failuresBefore;
    u8 net;
    char name[96];

    TestBlockParser();
    TestDeliverBounds();

    // 4a. Acceptance: blocks of 1..max size interleaved with rows, with lag and jitter.
    RunStreamCase(TRUE, TRUE, NET_CLEAN, FALSE);
    RunStreamCase(TRUE, TRUE, NET_JITTER, FALSE);
    RunStreamCase(TRUE, TRUE, NET_LAG, TRUE);
    RunStreamCase(FALSE, TRUE, NET_LAG, FALSE);
    RunStreamCase(FALSE, TRUE, NET_JITTER, TRUE);
    RunStreamCase(TRUE, FALSE, NET_JITTER, FALSE);   // the fallback: the GBA row path, with row pacing
    MP_LinkSetRowCatchUp(FALSE);
    RunStreamCase(TRUE, FALSE, NET_CLEAN, FALSE);    // the baseline: exactly the master branch's cable
    MP_LinkSetRowCatchUp(TRUE);

    // 4b. Measurements (host is the link master, as in the play-test set-up).
    for (net = NET_CLEAN; net <= NET_LAG; net++)
    {
        failuresBefore = sFail;
        snprintf(name, sizeof(name), "%s, GBA row path (baseline = master)", sNetNames[net]);
        RunMeasure(TRUE, FALSE, FALSE, net, name, &rows);
        snprintf(name, sizeof(name), "%s, row path + row pacing", sNetNames[net]);
        RunMeasure(TRUE, FALSE, TRUE, net, name, &catchUp);
        snprintf(name, sizeof(name), "%s, whole blocks + row pacing (PS-052 default)", sNetNames[net]);
        RunMeasure(TRUE, TRUE, TRUE, net, name, &fast);
        L_CHECK(fast.frames < rows.frames, "PS-052 measure: whole blocks are faster than the GBA row path");
        if (net != NET_JITTER) // random withholding differs from run to run: logged only
            L_CHECK(catchUp.frames <= rows.frames + rows.frames / 10, "PS-052 measure: row pacing is not slower than the row path");
        if (rows.frames != 0 && fast.frames != 0)
            MP_LOG("selftest: link: PS-052 measure [%s]: speed-up x%u.%u (%u -> %u frames for %u exchanges)\n", sNetNames[net],
                   (unsigned)(rows.frames * 10 / fast.frames / 10), (unsigned)(rows.frames * 10 / fast.frames % 10),
                   (unsigned)rows.frames, (unsigned)fast.frames, (unsigned)MEASURE_EXCHANGES);
        snprintf(name, sizeof(name), "PS-052 measure: %s: whole blocks beat the row path", sNetNames[net]);
        Report(name, failuresBefore);
    }
}

// No group: the cable is unplugged, exactly like single-player with the old stub.
static void ScenarioNoGroup(void)
{
    u32 failuresBefore = sFail;
    u32 i;
    bool8 everConnected = FALSE;

    InitGame(G_A, S_HOST, "SOLO", 1);
    Enter(G_A);
    gLinkType = LINKTYPE_TRADE_SETUP;
    OpenLink();
    ResetTasks();
    Leave();
    sGames[G_A].advanceIn = 5;
    for (i = 0; i < 120; i++)
    {
        TickGame(G_A);
        Enter(G_A);
        if ((gLinkStatus & (LINK_STAT_CONN_ESTABLISHED | LINK_STAT_ERRORS)) || GetLinkPlayerCount_2() != 0 || IsLinkMaster())
            everConnected = TRUE;
        Leave();
    }
    L_CHECK(!everConnected, "no group: status stays 0 (no players, never connected)");
    Enter(G_A);
    L_CHECK(!gReceivedRemoteLinkPlayers && gMain.callback2 != CB2_LinkError, "no group: nothing received, no error");
    CloseLink();
    Leave();
    Report("no group: the cable behaves as unplugged (single-player unchanged)", failuresBefore);
}

u32 MpSelftest_Link(void)
{
    u32 need;
    u8 i;

    sFail = 0;
    TestParsers();

    need = MP_LinkSelftestState(NULL, 0, FALSE);
    L_CHECK(need <= LINK_STATE_CAP, "link.c state fits the selftest buffer");
    if (need > LINK_STATE_CAP)
        return sFail;

    sSavedPtr1 = gSaveBlock1Ptr;
    sSavedPtr2 = gSaveBlock2Ptr;
    sSavedCallback2 = gMain.callback2;
    sSavedMainState = gMain.state;
    ResetTasks();
    MP_LinkSelftestState(sNeutralLink, sizeof(sNeutralLink), FALSE);
    for (i = 0; i < S_COUNT; i++)
        MpSession_ResetInstance(i);

    ScenarioNoGroup();
    ScenarioHostNotMember();
    ScenarioHostMember();
    ScenarioAgreedExit(TRUE);   // PS-051: the host is the leader (the usual play-test set-up)
    ScenarioAgreedExit(FALSE);  // PS-051: two clients, the host only sequences
    ScenarioFastCable();        // PS-052: whole blocks, row pacing, measurements

    // Leave the process as it was: the boot state of link.c, no sessions, PS-052 defaults.
    MP_LinkSetFastBlocks(TRUE);
    MP_LinkSetRowCatchUp(TRUE);
    MpLink_SetSelftestObserver(NULL);
    MP_LinkSelftestState(sNeutralLink, sizeof(sNeutralLink), TRUE);
    gSaveBlock1Ptr = sSavedPtr1;
    gSaveBlock2Ptr = sSavedPtr2;
    gMain.callback2 = sSavedCallback2;
    gMain.state = sSavedMainState;
    ResetTasks();
    Sel(0);
    if (sFail == 0)
        MP_LOG("selftest: link: all groups OK\n");
    return sFail;
}
