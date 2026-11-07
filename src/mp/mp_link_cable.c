// Virtual link cable, game side (PS-046): MP_LinkMain1, the PC replacement for the SIO
// LinkMain1() in src/link.c, and the --mp-link-test driver.
//
// MP_LinkMain1 has LinkMain1's contract (research/PS-040-link-report.md section 2.1):
//  - gLink.state goes START0 -> START1 -> (shouldAdvance == 1) HANDSHAKE -> CONN_ESTABLISHED.
//    CloseLink() zeroes gLink, which puts us back in START0, where the cable is closed.
//  - HANDSHAKE: the player count is the number of group members the host has seen open, and
//    link id 0 is the master. When the game advances the handshake on the master
//    (CheckShouldAdvanceLinkState), the host starts the bus once every member is open.
//  - CONN_ESTABLISHED: a non-zero gSendCmd goes to the bus (and is cleared); one row per tick
//    comes back into recvCmds (own command included, as on the cable), or zeros +
//    LINK_STAT_RECEIVED_NOTHING. At most QUEUE_CAPACITY rows are reported as queued.
//  - The SIO id bits of REG_SIOCNT carry our link id, so GetMultiplayerId() works unchanged.
//  - The group ending or a bus member closing while we are on the bus sets the hardware
//    error bit: LinkMain2 then runs the game's own link-error path (CB2_LinkError).
// Without a link group nothing is ever received: the same as single-player with the old stub
// (gLinkStatus stays 0, so the Cable Club times out exactly as before).

#include "global.h"
#include "link.h"
#include "main.h"
#include "overworld.h"
#include "mp/mp.h"
#include "mp/mp_internal.h"
#include "mp/mp_link.h"

// The bus carries CMD_LENGTH words per member and at most MAX_LINK_PLAYERS members.
typedef char MpLinkCmdLengthCheck[(CMD_LENGTH == MP_LINK_CMD_WORDS) ? 1 : -1];
typedef char MpLinkMembersCheck[(MAX_LINK_PLAYERS == MP_LINK_MAX_MEMBERS) ? 1 : -1];

u32 MP_LinkMain1(u8 *shouldAdvanceLinkState, u16 *sendCmd, u16 (*recvCmds)[CMD_LENGTH])
{
    struct MpLinkCableInfo info;
    u16 row[MP_LINK_MAX_MEMBERS][MP_LINK_CMD_WORDS];
    u16 any;
    u32 status;
    u32 i, j;

    MpLink_CableGetInfo(&info);

    switch (gLink.state)
    {
    case LINK_STATE_START0:
        MpLink_CableClose(); // nothing to do unless the cable was open
        gLink.state = LINK_STATE_START1;
        break;
    case LINK_STATE_START1:
        if (*shouldAdvanceLinkState == 1)
            gLink.state = LINK_STATE_HANDSHAKE;
        break;
    case LINK_STATE_HANDSHAKE:
        if (*shouldAdvanceLinkState == 2)
        {
            MpLink_CableClose();
            gLink.state = LINK_STATE_START0;
            break;
        }
        // The group may form after OpenLink: announce the open as soon as we are grouped.
        if (info.grouped && !info.open)
            MpLink_CableOpen();
        if (*shouldAdvanceLinkState == 1 && info.grouped && info.linkId == 0)
            MpLink_CableRequestStart();
        MpLink_CableGetInfo(&info);
        if (info.busUp)
            gLink.state = LINK_STATE_CONN_ESTABLISHED;
        break;
    case LINK_STATE_INIT_TIMER:
        gLink.state = LINK_STATE_CONN_ESTABLISHED;
        // fallthrough
    case LINK_STATE_CONN_ESTABLISHED:
        any = 0;
        for (i = 0; i < CMD_LENGTH; i++)
            any |= sendCmd[i];
        if (any != 0)
            MpLink_CableSend(sendCmd);
        for (i = 0; i < CMD_LENGTH; i++)
            sendCmd[i] = 0;

        switch (MpLink_CablePop(row))
        {
        case MP_LINK_POP_ROW:
            for (i = 0; i < MAX_LINK_PLAYERS; i++)
            {
                for (j = 0; j < CMD_LENGTH; j++)
                    recvCmds[i][j] = row[i][j];
            }
            gLink.receivedNothing = FALSE;
            break;
        case MP_LINK_POP_BUS_END:
            // A bus member closed its cable while we were still on it (a unilateral
            // CloseLink, e.g. its own link error). On the real cable we would lose the
            // serial transfers: report a hardware error.
            gLink.hardwareError = TRUE;
            // fallthrough
        default:
            // Clear every player row, not only gLink.playerCount: ProcessRecvCmds reads 4.
            for (i = 0; i < MAX_LINK_PLAYERS; i++)
            {
                for (j = 0; j < CMD_LENGTH; j++)
                    recvCmds[i][j] = 0;
            }
            gLink.receivedNothing = TRUE;
            break;
        }
        break;
    }
    *shouldAdvanceLinkState = 0;

    MpLink_CableGetInfo(&info);
    if (info.lost && gLink.state >= LINK_STATE_HANDSHAKE)
        gLink.hardwareError = TRUE;

    // Fields other code reads (GetLinkRecvQueueLength, the overworld send-queue check,
    // LinkCB_Standby / LinkCB_ReadyCloseLink via gLastRecvQueueCount).
    if (gLink.state == LINK_STATE_CONN_ESTABLISHED)
        gLink.playerCount = info.count;
    else if (gLink.state == LINK_STATE_HANDSHAKE)
        gLink.playerCount = info.openCount;
    else
        gLink.playerCount = 0;
    if (info.grouped || info.open)
    {
        gLink.localId = info.linkId;
        gLink.isMaster = (info.linkId == 0) ? LINK_MASTER : LINK_SLAVE;
        SIO_MULTI_CNT->id = info.linkId;
    }
    else
    {
        gLink.localId = 0;
        gLink.isMaster = LINK_SLAVE;
    }
    gLink.recvQueue.count = (info.queued > QUEUE_CAPACITY) ? QUEUE_CAPACITY : (u8)info.queued;
    gLink.sendQueue.count = 0;
    gLink.lag = 0;
    gLastRecvQueueCount = gLink.recvQueue.count;
    gLastSendQueueCount = 0;

    // The status word, built as LinkMain1 does (link.c).
    status = gLink.localId;
    status |= (u32)gLink.playerCount << LINK_STAT_PLAYER_COUNT_SHIFT;
    if (gLink.isMaster == LINK_MASTER)
        status |= LINK_STAT_MASTER;
    if (gLink.state == LINK_STATE_CONN_ESTABLISHED)
        status |= LINK_STAT_CONN_ESTABLISHED;
    status |= (u32)gLink.receivedNothing << LINK_STAT_RECEIVED_NOTHING_SHIFT;
    status |= (u32)gLink.hardwareError << LINK_STAT_ERROR_HARDWARE_SHIFT;
    status |= (u32)gLink.queueFull << LINK_STAT_ERROR_QUEUE_FULL_SHIFT;
    return status;
}

// ---------------------------------------------------------------------------
// --mp-link-test (dev harness, PS-046 card item 6)
// ---------------------------------------------------------------------------
//
// Both instances: --mp-link-test plus the usual --host / --join. Once the session has slots 0
// and 1 and the overworld is running, they form a link group, OpenLink(), run the player-data
// exchange, do MP_LINK_TEST_ROUNDS SendBlock rounds (200 bytes each way, a pattern derived
// from the sender's trainer id and the round), close the link, wait a second and start again.
// Every step is logged with "[MP] link test:". Killing one instance must make the other log
// "link-error path reached" (the game shows its link error screen; recovery is PS-048).

#define MP_LINK_TEST_ROUNDS     3
#define MP_LINK_TEST_BLOCK_SIZE 200
#define MP_LINK_TEST_STEP_TICKS (60 * 20)   // a step taking longer than 20 s fails the cycle

enum
{
    LT_IDLE,
    LT_WAIT_GROUP,
    LT_WAIT_PLAYERS,
    LT_WAIT_EXCHANGE,
    LT_SEND_BLOCK,
    LT_WAIT_BLOCK,
    LT_WAIT_CLOSE,
    LT_PAUSE,
    LT_DONE,
};

static bool8 sLinkTestEnabled = FALSE;
static u8 sLinkTestState = LT_IDLE;
static u16 sLinkTestTimer;
static u8 sLinkTestRound;
static u32 sLinkTestCycle;
static u32 sLinkTestBlocksOk;
static u32 sLinkTestBlocksBad;

void MP_SetLinkTest(bool8 enabled)
{
    sLinkTestEnabled = enabled ? TRUE : FALSE;
    sLinkTestState = LT_IDLE;
}

static u8 MpLinkTestPatternByte(u32 trainerId, u8 round, u32 i)
{
    return (u8)((trainerId * 31u) + round * 17u + i * 7u + (i >> 3));
}

static void MpLinkTestFillBlock(void)
{
    u32 tid = gLinkPlayers[GetMultiplayerId()].trainerId;
    u32 i;

    for (i = 0; i < MP_LINK_TEST_BLOCK_SIZE; i++)
        gBlockSendBuffer[i] = MpLinkTestPatternByte(tid, sLinkTestRound, i);
}

static bool8 MpLinkTestCheckBlock(u8 who)
{
    const u8 *data = (const u8 *)gBlockRecvBuffer[who];
    u32 tid = gLinkPlayers[who].trainerId;
    u32 i;

    for (i = 0; i < MP_LINK_TEST_BLOCK_SIZE; i++)
    {
        if (data[i] != MpLinkTestPatternByte(tid, sLinkTestRound, i))
            return FALSE;
    }
    return TRUE;
}

static void MpLinkTestGoto(u8 state)
{
    sLinkTestState = state;
    sLinkTestTimer = 0;
}

static void MpLinkTestFail(const char *what)
{
    MP_LOG("link test: cycle %u FAILED: %s; closing and retrying\n", (unsigned)sLinkTestCycle, what);
    CloseLink();
    MP_LinkLeave();
    MpLinkTestGoto(LT_PAUSE);
}

void MP_LinkTestTick(void)
{
    char name[PLAYER_NAME_LENGTH + 1];
    u32 i, j;
    u8 me;

    if (!sLinkTestEnabled || sLinkTestState == LT_DONE)
        return;

    if (gMain.callback2 == CB2_LinkError)
    {
        MP_LOG("link test: link-error path reached (CB2_LinkError, link status 0x%08X); test stopped\n", (unsigned)gLinkStatus);
        sLinkTestState = LT_DONE;
        return;
    }
    if (sLinkTestState != LT_IDLE && sLinkTestState != LT_PAUSE && ++sLinkTestTimer > MP_LINK_TEST_STEP_TICKS)
    {
        MpLinkTestFail("step timed out");
        return;
    }

    switch (sLinkTestState)
    {
    case LT_IDLE:
        // Slots 0 and 1, in the overworld (so the save is loaded for InitLocalLinkPlayer).
        if (!MP_IsActive() || MP_GetPlayer(0) == NULL || MP_GetPlayer(1) == NULL
         || MP_GetLocalSlot() > 1 || gMain.callback2 != CB2_Overworld)
            break;
        sLinkTestCycle++;
        MP_LOG("link test: cycle %u: asking for a link group with slots 0 and 1\n", (unsigned)sLinkTestCycle);
        if (MP_LinkJoin(0x03))
            MpLinkTestGoto(LT_WAIT_GROUP);
        break;
    case LT_WAIT_GROUP:
        if (MP_LinkGetState() == MP_LINK_STATE_NONE)
        {
            MpLinkTestFail("link request ended");
            break;
        }
        if (MP_LinkGetState() != MP_LINK_STATE_GROUPED)
            break;
        MP_LOG("link test: grouped as link id %u; OpenLink()\n", (unsigned)MP_LinkGetLinkId());
        gLinkType = LINKTYPE_TRADE_SETUP;
        OpenLink(); // Task_TriggerHandshake advances the handshake 5 frames later
        sLinkTestRound = 0;
        MpLinkTestGoto(LT_WAIT_PLAYERS);
        break;
    case LT_WAIT_PLAYERS:
        if (GetLinkPlayerCount_2() < 2)
            break;
        if (IsLinkMaster())
        {
            if (sLinkTestTimer > 30)
            {
                MP_LOG("link test: 2 players on the cable; master advances the handshake\n");
                CheckShouldAdvanceLinkState();
                MpLinkTestGoto(LT_WAIT_EXCHANGE);
            }
        }
        else
        {
            MpLinkTestGoto(LT_WAIT_EXCHANGE);
        }
        break;
    case LT_WAIT_EXCHANGE:
        if (gReceivedRemoteLinkPlayers != TRUE)
            break;
        for (i = 0; i < GetLinkPlayerCount(); i++)
        {
            for (j = 0; j < PLAYER_NAME_LENGTH && gLinkPlayers[i].name[j] != EOS; j++)
            {
                u8 c = gLinkPlayers[i].name[j];
                // Debug only: A-Z, a-z, 0-9 in the game charset, '?' for anything else.
                if (c >= CHAR_A && c <= CHAR_Z)
                    name[j] = 'A' + (c - CHAR_A);
                else if (c >= CHAR_a && c <= CHAR_z)
                    name[j] = 'a' + (c - CHAR_a);
                else if (c >= CHAR_0 && c <= CHAR_9)
                    name[j] = '0' + (c - CHAR_0);
                else
                    name[j] = '?';
            }
            name[j] = '\0';
            MP_LOG("link test: gLinkPlayers[%u] = %s, trainer id 0x%08X, gender %u, version 0x%04X%s\n",
                   (unsigned)i, name, (unsigned)gLinkPlayers[i].trainerId, (unsigned)gLinkPlayers[i].gender,
                   (unsigned)gLinkPlayers[i].version, (i == GetMultiplayerId()) ? " (me)" : "");
        }
        MpLinkTestGoto(LT_SEND_BLOCK);
        break;
    case LT_SEND_BLOCK:
        if (!IsLinkTaskFinished())
            break;
        MpLinkTestFillBlock();
        if (SendBlock(0, gBlockSendBuffer, MP_LINK_TEST_BLOCK_SIZE))
            MpLinkTestGoto(LT_WAIT_BLOCK);
        break;
    case LT_WAIT_BLOCK:
        if ((GetBlockReceivedStatus() & 3) != 3)
            break;
        me = GetMultiplayerId();
        if (MpLinkTestCheckBlock(me) && MpLinkTestCheckBlock(me ^ 1))
        {
            sLinkTestBlocksOk++;
            MP_LOG("link test: round %u: %u-byte blocks exchanged and verified (%u ticks)\n",
                   (unsigned)(sLinkTestRound + 1), (unsigned)MP_LINK_TEST_BLOCK_SIZE, (unsigned)sLinkTestTimer);
        }
        else
        {
            sLinkTestBlocksBad++;
            MP_LOG("link test: round %u: block MISMATCH\n", (unsigned)(sLinkTestRound + 1));
        }
        ResetBlockReceivedFlags();
        if (++sLinkTestRound < MP_LINK_TEST_ROUNDS)
        {
            MpLinkTestGoto(LT_SEND_BLOCK);
        }
        else
        {
            SetCloseLinkCallback();
            MpLinkTestGoto(LT_WAIT_CLOSE);
        }
        break;
    case LT_WAIT_CLOSE:
        if (gReceivedRemoteLinkPlayers)
            break;
        MP_LOG("link test: cycle %u complete (blocks ok %u, bad %u); link closed\n",
               (unsigned)sLinkTestCycle, (unsigned)sLinkTestBlocksOk, (unsigned)sLinkTestBlocksBad);
        // Keep the group: the next cycle re-opens the cable on the same group (a new bus).
        MpLinkTestGoto(LT_PAUSE);
        break;
    case LT_PAUSE:
        if (++sLinkTestTimer < 60)
            break;
        if (MP_LinkGetState() == MP_LINK_STATE_GROUPED)
        {
            sLinkTestCycle++;
            MP_LOG("link test: cycle %u: OpenLink() again on the same group\n", (unsigned)sLinkTestCycle);
            gLinkType = LINKTYPE_TRADE_SETUP;
            OpenLink();
            sLinkTestRound = 0;
            MpLinkTestGoto(LT_WAIT_PLAYERS);
        }
        else
        {
            MpLinkTestGoto(LT_IDLE);
        }
        break;
    }
}
