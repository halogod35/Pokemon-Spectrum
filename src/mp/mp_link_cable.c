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

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "global.h"
#include "decompress.h"
#include "link.h"
#include "main.h"
#include "overworld.h"
#include "mp/mp.h"
#include "mp/mp_internal.h"
#include "mp/mp_link.h"
#include "mp/mp_summary.h"
#include "mp/mp_transport.h"

// The bus carries CMD_LENGTH words per member and at most MAX_LINK_PLAYERS members.
typedef char MpLinkCmdLengthCheck[(CMD_LENGTH == MP_LINK_CMD_WORDS) ? 1 : -1];
typedef char MpLinkMembersCheck[(MAX_LINK_PLAYERS == MP_LINK_MAX_MEMBERS) ? 1 : -1];
// PS-047: the bus pre-filter (mp_link.c) mirrors these link.c / decompress.h values.
typedef char MpLinkInitBlockCheck[(LINKCMD_INIT_BLOCK == MP_LINKCMD_INIT_BLOCK) ? 1 : -1];
typedef char MpLinkBlockReqCheck[(LINKCMD_SEND_BLOCK_REQ == MP_LINKCMD_SEND_BLOCK_REQ) ? 1 : -1];
typedef char MpLinkBlockReqCountCheck[(BLOCK_REQ_SIZE_40 + 1 == MP_LINK_BLOCK_REQ_COUNT) ? 1 : -1];
typedef char MpLinkMaxBlockCheck[(sizeof(gDecompressionBuffer) == MP_LINK_MAX_BLOCK_SIZE) ? 1 : -1];

// PS-051 flight recorder: what MP_LinkMain1 did this tick (MP_LinkTraceFrame records it).
static u16 sTraceSent;
static u16 sTraceRecv0;
static u16 sTraceRecv1;
static u8 sTraceRecvKind;   // 0 nothing, 1 a row, 2 BUS_END

enum
{
    TRACE_RECV_NONE,
    TRACE_RECV_ROW,
    TRACE_RECV_BUS_END,
};

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
        {
            MpLink_CableSend(sendCmd);
            sTraceSent = sendCmd[0];
        }
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
            sTraceRecvKind = TRACE_RECV_ROW;
            sTraceRecv0 = row[0][0];
            sTraceRecv1 = row[1][0];
            break;
        case MP_LINK_POP_BUS_END:
            sTraceRecvKind = TRACE_RECV_BUS_END;
            if (MpLink_MemberIsFinishing())
            {
                // PS-051: we or the partner are done with the activity (both agreed to stop):
                // the bus ending is the normal end, not a lost cable. Nothing more will come.
                MP_LOG("link: the bus ended after the agreed end of the link activity (not an error)\n");
            }
            else
            {
                // A bus member closed its cable while we were still on it (a unilateral
                // CloseLink, e.g. its own link error). On the real cable we would lose the
                // serial transfers: report a hardware error.
                gLink.hardwareError = TRUE;
                MP_LinkTraceDump("the bus ended while our cable was open: hardware error");
            }
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
    // MP: BUG-004. Only once the rows already received are played: the last ones may carry the
    // partner's READY_CLOSE_LINK, and the close handshake must see it to finish cleanly.
    // PS-051: not after the agreed end of the activity: then the group ending (DONE, or the
    // partner leaving) is the normal end; the cable just stays silent until we close it.
    if (info.lost && info.queued == 0 && gLink.state >= LINK_STATE_HANDSHAKE && !MpLink_MemberIsFinishing())
    {
        if (!gLink.hardwareError)
            MP_LinkTraceDump("the link group ended while our cable was open: hardware error");
        gLink.hardwareError = TRUE;
    }

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
// PS-047: data from a peer that the game must not use
// ---------------------------------------------------------------------------

static u32 sLinkRejectCount;

void MP_LinkRejectData(const char *what)
{
    sLinkRejectCount++;
    if (sLinkRejectCount <= 20 || sLinkRejectCount % 10000 == 0) // a hostile peer can repeat this every frame
        MP_LOG("link: rejected data from the link partner (%s, #%u); ending the link activity\n", what, (unsigned)sLinkRejectCount);
    // Like a cable fault: the next MP_LinkMain1 reports LINK_STAT_ERROR_HARDWARE and LinkMain2
    // runs the game's link-error path. CloseLink() zeroes gLink, which clears it again.
    if (gLink.state >= LINK_STATE_HANDSHAKE)
        gLink.hardwareError = TRUE;
    MP_LinkTraceDump("rejected data from the link partner"); // PS-051
}

void MP_LinkRejectDataF(const char *fmt, ...)
{
    char what[160];
    va_list args;

    va_start(args, fmt);
    vsnprintf(what, sizeof(what), fmt, args);
    va_end(args);
    MP_LinkRejectData(what);
}

u32 MP_LinkGetRejectCount(void)
{
    return sLinkRejectCount;
}

// ---------------------------------------------------------------------------
// PS-051: the agreed end of a direct link activity (trade menu cancel, link battle end)
// ---------------------------------------------------------------------------
//
// Vanilla ends a cable activity with the close-link handshake (SetCloseLinkCallback), which
// assumes GBA lockstep: both games see the same serial row on the same frame. Over the
// host-sequenced cable one side closes first and the other may still be mid-handshake when the
// bus or the group goes away (BUG-004/005/007). Once both players have agreed to stop there is
// nothing left to exchange, so each side ends on its own:
//   MP_LinkActivityAgreed   at the moment of agreement: link errors are suppressed and the cable
//                           no longer reports the bus / group ending as a hardware error.
//   MP_LinkActivityTryFinish once our own last command is out: LINK_DONE to the host, then
//                           CloseLink() locally. No handshake, no waiting for the partner.
// The host ends the group (END(DONE)) once every member is done, or MP_LINK_DONE_GRACE_MS after
// the first one (mp_link.c).

#define MP_LINK_FINISH_WAIT_FRAMES  120 // our last block still going out; then close anyway

void MP_LinkActivityAgreed(const char *who)
{
    if (MpLink_MemberIsAgreed())
        return;
    MpLink_MemberSetAgreed();
    SetSuppressLinkErrorMessage(TRUE);
    MP_LOG("link: %s: both players agreed to stop (link id %u); the exit no longer depends on the partner\n",
           who, (unsigned)MP_LinkGetLinkId());
}

bool8 MP_LinkActivityIsAgreed(void)
{
    return MpLink_MemberIsAgreed();
}

bool8 MP_LinkIsFinishing(void)
{
    return MpLink_MemberIsFinishing();
}

bool8 MP_LinkActivityTryFinish(void)
{
    bool8 quiet = (gLinkCallback == NULL);
    u16 waited;
    u32 i;

    if (!MpLink_MemberIsAgreed())
        MP_LinkActivityAgreed("exit");
    for (i = 0; i < CMD_LENGTH; i++)
    {
        if (gSendCmd[i] != 0)
            quiet = FALSE;
    }
    // While the cable is up, our last command (the leader's BOTH_CANCEL block, ...) must be on
    // the bus first: it goes out in the next MP_LinkMain1. A closed cable has nothing to send.
    if (gLink.state == LINK_STATE_CONN_ESTABLISHED && !quiet)
    {
        waited = MpLink_MemberBumpFinishWait();
        if (waited == 1)
            MP_LOG("link: the exit waits for our last link command to go out\n");
        if (waited < MP_LINK_FINISH_WAIT_FRAMES)
            return FALSE;
        MP_LOG("link: closing with a link callback still running (waited %u frames)\n", (unsigned)waited);
    }
    MpLink_MemberSendDone();
    CloseLink();
    ClearLinkCallback();
    MP_LOG("link: our cable is closed locally (agreed exit, no close handshake; link error seen: %u)\n", (unsigned)gLinkErrorOccurred);
    return TRUE;
}

// ---------------------------------------------------------------------------
// PS-051: link flight recorder
// ---------------------------------------------------------------------------
//
// Once per tick (HandleLinkConnection) while a link group exists or the cable is open, plus
// MP_TRACE_TAIL_FRAMES after: the main / link callbacks (as exe offsets, so pokeemerald.map
// resolves them), the link flags, the MP link state and the cable, the trade menu's callbackId
// and the first word of the command sent / row received. MP_LinkTraceDump writes the last
// MP_TRACE_FRAMES ticks to the log file on an incident (CB2_LinkError, a rejection, a hardware
// error, a link error seen by TrySetLinkErrorBuffer); one dump per incident (triggers within
// MP_TRACE_INCIDENT_MS are the same incident), then the ticks after it as an "aftermath".

#define MP_TRACE_FRAMES         600     // 10 s
#define MP_TRACE_TAIL_FRAMES    180     // keep recording 3 s after the link is gone
#define MP_TRACE_INCIDENT_MS    2000
#define MP_TRACE_ALSO_MAX       6       // "same incident" lines per incident
#define MP_TRACE_CONSOLE_LINES  12      // without a log file (selftest): the last ticks only

struct MpTraceEntry
{
    u32 frame;
    u32 cb2;
    u32 cb1;
    u32 linkCb;
    u32 linkStatus;
    u16 sent;
    u16 recv0;
    u16 recv1;
    u16 queued;
    u8 recvKind;
    u8 remotePlayers;   // gReceivedRemoteLinkPlayers
    u8 suppress;        // gSuppressLinkErrorMessage
    u8 errorOccurred;   // gLinkErrorOccurred
    u8 linkState;       // gLink.state
    u8 hwError;         // gLink.hardwareError
    u8 mpState;         // MP_LinkGetState
    u8 endReason;
    u8 open;
    u8 openCount;
    u8 lost;
    u8 busUp;
    u8 agreed;
    u8 peerDone;
    u8 tradeCb;         // the trade menu's callbackId, 0xFF if none
};

static struct MpTraceEntry sTrace[MP_TRACE_FRAMES];
static u32 sTraceWritten;       // entries ever written
static u32 sTraceTick;          // ticks ever seen
static u16 sTraceTail;
static u32 sTraceDumps;
static bool8 sIncidentOpen;
static u32 sIncidentMs;
static u32 sIncidentFirstEntry; // sTraceWritten at the dump
static u8 sIncidentAlso;
static char sIncidentReason[96];

// Everything but the tick number (the fields from cb2 to tradeCb have no padding between them).
static bool8 MpTraceSame(const struct MpTraceEntry *a, const struct MpTraceEntry *b)
{
    size_t from = offsetof(struct MpTraceEntry, cb2);
    size_t to = offsetof(struct MpTraceEntry, tradeCb) + sizeof(a->tradeCb);

    return memcmp((const u8 *)a + from, (const u8 *)b + from, to - from) == 0;
}

static void MpTraceLine(bool8 toFile, const struct MpTraceEntry *e)
{
    static const char sRecv[] = { '-', 'R', 'E' };
    char line[256];

    snprintf(line, sizeof(line),
             "  %8u cb2=exe+%06X cb1=exe+%06X lcb=exe+%06X st=%08X rrp=%u sup=%u err=%u ls=%u hw=%u | mp=%u end=%u open=%u oc=%u q=%u lost=%u bus=%u agr=%u pd=%X | tcb=%u | tx=%04X rx%c=%04X,%04X\n",
             (unsigned)e->frame, (unsigned)e->cb2, (unsigned)e->cb1, (unsigned)e->linkCb, (unsigned)e->linkStatus,
             (unsigned)e->remotePlayers, (unsigned)e->suppress, (unsigned)e->errorOccurred, (unsigned)e->linkState, (unsigned)e->hwError,
             (unsigned)e->mpState, (unsigned)e->endReason, (unsigned)e->open, (unsigned)e->openCount, (unsigned)e->queued,
             (unsigned)e->lost, (unsigned)e->busUp, (unsigned)e->agreed, (unsigned)e->peerDone, (unsigned)e->tradeCb,
             (unsigned)e->sent, sRecv[e->recvKind < 3 ? e->recvKind : 0], (unsigned)e->recv0, (unsigned)e->recv1);
    if (toFile)
        MpLog_FilePrintf("%s", line);
    else
        printf("%s", line);
}

// Writes entries [first, sTraceWritten) (at most the ring), collapsing identical runs.
static void MpTraceWrite(const char *title, const char *reason, u32 first)
{
    bool8 toFile = MpLog_FileIsOpen();
    const struct MpTraceEntry *prev = NULL;
    u32 count, i, start, skipped = 0;

    if (sTraceWritten - first > MP_TRACE_FRAMES)
        first = sTraceWritten - MP_TRACE_FRAMES;
    count = sTraceWritten - first;
    if (!toFile && count > MP_TRACE_CONSOLE_LINES)
    {
        first = sTraceWritten - MP_TRACE_CONSOLE_LINES;
        count = MP_TRACE_CONSOLE_LINES;
    }
    start = first;
    if (toFile)
    {
        MpLog_FilePrintf("[MP] ==== %s (%s) ==== tick %u, %u ticks recorded, exe base %p\n", title, reason,
                         (unsigned)sTraceTick, (unsigned)count, (void *)MpLog_ExeBase());
        MpLog_FilePrintf("[MP] known: CB2_LinkError=exe+%06X CB2_Overworld=exe+%06X CB2_ReturnToField=exe+%06X CB1_Overworld=exe+%06X\n",
                         (unsigned)MpLog_ExeOffset((const void *)CB2_LinkError), (unsigned)MpLog_ExeOffset((const void *)CB2_Overworld),
                         (unsigned)MpLog_ExeOffset((const void *)CB2_ReturnToField), (unsigned)MpLog_ExeOffset((const void *)CB1_Overworld));
        MpLog_FilePrintf("[MP] columns: tick, gMain.callback2/1, gLinkCallback, gLinkStatus, gReceivedRemoteLinkPlayers, gSuppressLinkErrorMessage,"
                         " gLinkErrorOccurred, gLink.state, gLink.hardwareError | MP link state, end reason, cable open, open count, queued rows,"
                         " lost, bus up, agreed, peers done | trade menu callbackId (255 none) | cmd sent, row received (R row, E bus end): word 0 of link id 0,1\n");
    }
    else
    {
        printf("[MP] ==== %s (%s) ==== tick %u, last %u ticks (no log file)\n", title, reason, (unsigned)sTraceTick, (unsigned)count);
    }
    for (i = start; i != sTraceWritten; i++)
    {
        const struct MpTraceEntry *e = &sTrace[i % MP_TRACE_FRAMES];

        if (prev != NULL && MpTraceSame(prev, e) && i + 1 != sTraceWritten)
        {
            skipped++;
            continue;
        }
        if (skipped != 0)
        {
            if (toFile)
                MpLog_FilePrintf("  ... %u identical ticks\n", (unsigned)skipped);
            else
                printf("  ... %u identical ticks\n", (unsigned)skipped);
            skipped = 0;
        }
        MpTraceLine(toFile, e);
        prev = e;
    }
    if (toFile)
    {
        MpLog_FilePrintf("[MP] ==== end of %s ====\n", title);
        MpLog_FileFlush();
    }
    else
    {
        printf("[MP] ==== end of %s ====\n", title);
        fflush(stdout);
    }
}

void MP_LinkTraceDump(const char *reason)
{
    u32 now = MpTransport_GetTimeMs();

    if (sIncidentOpen && (u32)(now - sIncidentMs) < MP_TRACE_INCIDENT_MS)
    {
        if (sIncidentAlso < MP_TRACE_ALSO_MAX)
        {
            sIncidentAlso++;
            MP_LOG("link trace: same incident, also: %s (tick %u)\n", reason, (unsigned)sTraceTick);
        }
        return;
    }
    sIncidentOpen = TRUE;
    sIncidentMs = now;
    sIncidentAlso = 0;
    sIncidentFirstEntry = sTraceWritten;
    snprintf(sIncidentReason, sizeof(sIncidentReason), "%s", reason);
    sTraceDumps++;
    if (MpLog_FileIsOpen())
        MP_LOG("link trace #%u: %s (tick %u); the last %u ticks are in %s\n", (unsigned)sTraceDumps, reason,
               (unsigned)sTraceTick, (unsigned)MP_TRACE_FRAMES, MpLog_GetPath());
    else
        MP_LOG("link trace #%u: %s (tick %u)\n", (unsigned)sTraceDumps, reason, (unsigned)sTraceTick);
    MpTraceWrite("link trace", reason, 0);
}

u32 MP_LinkTraceGetDumpCount(void)
{
    return sTraceDumps;
}

// --mp-selftest: the next trigger is a new incident (the test cases run faster than 2 s apart).
void MpLink_TraceSelftestEndIncident(void)
{
    sIncidentOpen = FALSE;
}

void MP_LinkTraceFrame(void)
{
    struct MpLinkCableInfo info;
    struct MpTraceEntry *e;
    bool8 active;

    sTraceTick++;
    MpLink_CableGetInfo(&info);
    active = MP_LinkGetState() != MP_LINK_STATE_NONE || info.open || gReceivedRemoteLinkPlayers
          || gLink.state != LINK_STATE_START0 || sIncidentOpen;
    if (active)
        sTraceTail = MP_TRACE_TAIL_FRAMES;
    if (active || sTraceTail != 0)
    {
        if (!active)
            sTraceTail--;
        e = &sTrace[sTraceWritten % MP_TRACE_FRAMES];
        memset(e, 0, sizeof(*e));
        e->frame = sTraceTick;
        e->cb2 = MpLog_ExeOffset((const void *)gMain.callback2);
        e->cb1 = MpLog_ExeOffset((const void *)gMain.callback1);
        e->linkCb = MpLog_ExeOffset((const void *)gLinkCallback);
        e->linkStatus = gLinkStatus;
        e->sent = sTraceSent;
        e->recv0 = sTraceRecv0;
        e->recv1 = sTraceRecv1;
        e->recvKind = sTraceRecvKind;
        e->queued = info.queued;
        e->remotePlayers = gReceivedRemoteLinkPlayers;
        e->suppress = gSuppressLinkErrorMessage;
        e->errorOccurred = gLinkErrorOccurred;
        e->linkState = gLink.state;
        e->hwError = gLink.hardwareError;
        e->mpState = MP_LinkGetState();
        e->endReason = MP_LinkGetEndReason();
        e->open = info.open;
        e->openCount = info.openCount;
        e->lost = info.lost;
        e->busUp = info.busUp;
        e->agreed = MpLink_MemberIsAgreed();
        e->peerDone = MpLink_MemberPeerDoneMask();
        e->tradeCb = MP_TradeMenuTraceId();
        sTraceWritten++;
    }
    sTraceSent = 0;
    sTraceRecv0 = 0;
    sTraceRecv1 = 0;
    sTraceRecvKind = TRACE_RECV_NONE;

    // The incident window closed: write what happened after the dump.
    if (sIncidentOpen && (u32)(MpTransport_GetTimeMs() - sIncidentMs) >= MP_TRACE_INCIDENT_MS)
    {
        sIncidentOpen = FALSE;
        if (sTraceWritten != sIncidentFirstEntry && MpLog_FileIsOpen()) // the console (selftest) gets the dump only
            MpTraceWrite("link trace aftermath", sIncidentReason, sIncidentFirstEntry);
    }
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
    char name[MP_PLAYER_NAME_SIZE + 1];
    u32 i;
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
            MP_NameToAscii(gLinkPlayers[i].name, name, sizeof(name)); // logs only (PS-013 helper)
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
