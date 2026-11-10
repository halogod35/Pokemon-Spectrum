// --mp-selftest, PS-053 group: the network traffic benchmark.
//
// Runs a real in-process session (host + 1 client, then host + 3 clients) over loopback ENet,
// pumped at 60 Hz like the game's main loop (one MP_Update and one ENet flush per frame and
// instance), and measures with the PS-053 counters (mp_netstats.c) what each player sends and
// receives per activity:
//   idle    everyone standing still (keep-alives, pings)
//   walk    everyone walking (a step every 16 frames, as in the game)
//   bike    everyone on the mach bike at top speed (a step every 4 frames: presence worst case)
//   view    one TRAINER_CARD and one VIEW_PARTY request (6 mons)
//   trade   an INTERACT request, a link group, the cable traffic of one trade, TRADE_COMMIT, LINK_DONE
//   battle  the same for a link battle (2 turns)
//
// The trade and battle rows drive the REAL session and link bus code (mp_link.c: LINK_CMD up,
// LINK_ROW down, the host as sequencer) with the command stream the game's link.c produces for
// those screens: SendBlock = one INIT_BLOCK command, 2 idle frames, then one CONT_BLOCK command
// (14 bytes) per frame, with the block sizes of trade.c / battle_main.c (party in 200-byte
// blocks, mail 220, ribbons 40, LinkPlayerBlock, LinkBattlerHeader). The game screens
// themselves (menus, animations, the battle engine) are not run: the waits between blocks and
// the per-turn battle traffic (BATTLE_TURN_* below) are estimates, so treat those two rows as
// "about this much", not as a recording.
//
// Bytes: "msg" = our packets (header + payload) handed to ENet; "ENet" = ENet's UDP payload
// (our data + ENet headers, acks, pings); "IP" = ENet + 28 bytes IPv4/UDP per datagram. The
// tables print as "[MP] netbench: ..." lines. Only a few sanity checks can fail.

#include <string.h>
#include "global.h"
#include "battle.h"     // struct LinkBattlerHeader
#include "link.h"       // struct LinkPlayerBlock, LINKCMD_*
#include "pokemon.h"
#include "constants/characters.h"
#include "constants/maps.h"
#include "constants/species.h"
#include "mp/mp.h"
#include "mp/mp_internal.h"
#include "mp/mp_link.h"
#include "mp/mp_netstats.h"
#include "mp/mp_protocol.h"
#include "mp/mp_selftest.h"
#include "mp/mp_session_internal.h"
#include "mp/mp_transport.h"

#define NB_PORT_FIRST       27510
#define NB_PORT_LAST        27519
#define NB_MAX_INSTANCES    4
#define NB_IDLE_MS          4000
#define NB_WALK_MS          5000
#define NB_BIKE_MS          3000
#define NB_SETTLE_MS        1500
#define NB_MAX_ROWS         16
#define NB_TOP_TYPES        3
#define NB_SCRIPT_MAX       160
#define NB_TRADE_ID         0x4E420001u

// Battle turn estimate (see the header comment): link id 0 runs the battle engine and sends
// most controller buffers; the partner sends its choices and acknowledgements.
#define BATTLE_TURNS                2
#define BATTLE_TURN_MASTER_BLOCKS   10
#define BATTLE_TURN_MASTER_BYTES    28
#define BATTLE_TURN_PARTNER_BLOCKS  4
#define BATTLE_TURN_PARTNER_BYTES   16

static u32 sFail;

#define NB_CHECK(cond, what)                                                        \
    do                                                                              \
    {                                                                               \
        if (!(cond))                                                                \
        {                                                                           \
            MP_LOG("selftest: FAIL: netbench: %s (%s:%d)\n", what, __FILE__, __LINE__); \
            sFail++;                                                                \
        }                                                                           \
    } while (0)

static u32 sRng = 0x4E455442u; // "NETB"

static u32 Rand(void)
{
    sRng ^= sRng << 13;
    sRng ^= sRng >> 17;
    sRng ^= sRng << 5;
    return sRng;
}

// ===========================================================================
// Instances and the 60 Hz pump
// ===========================================================================

struct NbPlayer
{
    struct MpPresence p;
    u8 stepEvery;           // frames per step, 0 = standing
    u8 stepTimer;
    u8 stepsInDir;
    s8 dir;
};

enum { OP_END, OP_BLOCK, OP_CMD, OP_WAIT };

struct NbOp
{
    u8 op;
    u16 arg;
    u16 arg2;
};

struct NbLinkMember
{
    bool8 active;
    struct NbOp script[NB_SCRIPT_MAX];
    u16 scriptLen;
    u16 pc;
    u16 wait;
    u8 delay;
    u16 blockLeft;
    bool8 scriptDone;
    u32 cmdsSent;
    u32 rowsPopped;
};

static u16 sPort;
static u8 sCount;                               // instances in the current session
static struct NbPlayer sPlayers[NB_MAX_INSTANCES];
static struct NbLinkMember sLink[NB_MAX_INSTANCES];
static u8 sResult[NB_MAX_INSTANCES];            // last MP_EVENT_INTERACT_RESULT result
static u32 sLastRequestMs[NB_MAX_INSTANCES];
static u32 sFrameBaseMs;
static u32 sFrameNo;

static void Sel(u8 instance)
{
    MpSession_SelectInstance(instance);
}

static void WaitNextFrame(void)
{
    u32 target, now;

    sFrameNo++;
    target = sFrameBaseMs + (sFrameNo * 50) / 3;
    now = MpTransport_GetTimeMs();
    if ((s32)(now - target) > 100)
    {
        // Far behind (a slow CI machine): do not burst to catch up, start a new frame grid.
        sFrameBaseMs = now;
        sFrameNo = 0;
        return;
    }
    while ((s32)(target - MpTransport_GetTimeMs()) > 0)
        ;
}

static void ResetFrameClock(void)
{
    sFrameBaseMs = MpTransport_GetTimeMs();
    sFrameNo = 0;
}

static void PresenceTick(u8 i)
{
    struct NbPlayer *pl = &sPlayers[i];

    if (pl->stepEvery != 0 && ++pl->stepTimer >= pl->stepEvery)
    {
        pl->stepTimer = 0;
        if (pl->stepsInDir >= 6)
        {
            pl->dir = (s8)-pl->dir;
            pl->stepsInDir = 0;
        }
        pl->p.x = (s16)(pl->p.x + pl->dir);
        pl->stepsInDir++;
        pl->p.facing = (pl->dir > 0) ? DIR_EAST : DIR_WEST;
        pl->p.stepId++;
    }
    MP_SetLocalPresence(&pl->p);
}

// One frame of the link screen's command stream (see the header comment). Returns TRUE and
// fills cmd when this frame sends a command.
static bool8 NextLinkCmd(struct NbLinkMember *m, u16 *cmd)
{
    const struct NbOp *op;
    u32 k;

    memset(cmd, 0, sizeof(u16) * MP_LINK_CMD_WORDS);
    if (m->wait != 0)
    {
        m->wait--;
        return FALSE;
    }
    if (m->delay != 0)
    {
        m->delay--;
        return FALSE;
    }
    if (m->blockLeft != 0)
    {
        // Pokemon data is encrypted: random words are a fair stand-in for the payload.
        cmd[0] = LINKCMD_CONT_BLOCK;
        for (k = 1; k < MP_LINK_CMD_WORDS; k++)
            cmd[k] = (u16)Rand();
        m->blockLeft = (m->blockLeft > 14) ? (u16)(m->blockLeft - 14) : 0;
        return TRUE;
    }
    if (m->pc >= m->scriptLen)
    {
        m->scriptDone = TRUE;
        return FALSE;
    }
    op = &m->script[m->pc++];
    switch (op->op)
    {
    case OP_BLOCK:
        cmd[0] = LINKCMD_INIT_BLOCK;
        cmd[1] = op->arg;
        cmd[2] = 0x80;
        m->blockLeft = op->arg;
        m->delay = 2;
        return TRUE;
    case OP_CMD:
        cmd[0] = op->arg;
        cmd[1] = op->arg2;
        return TRUE;
    case OP_WAIT:
        m->wait = (op->arg != 0) ? (u16)(op->arg - 1) : 0;
        return FALSE;
    default:
        m->scriptDone = TRUE;
        return FALSE;
    }
}

static void LinkTick(u8 i)
{
    struct NbLinkMember *m = &sLink[i];
    struct MpLinkCableInfo info;
    u16 cmd[MP_LINK_CMD_WORDS];
    u16 rows[MP_LINK_MAX_MEMBERS][MP_LINK_CMD_WORDS];
    u32 guard;

    if (!m->active)
        return;
    MpLink_CableGetInfo(&info);
    if (info.grouped && !info.open)
    {
        MpLink_CableOpen();
        MpLink_CableGetInfo(&info);
    }
    if (info.grouped && info.open && !info.busUp && info.linkId == 0)
        MpLink_CableRequestStart();
    if (info.busUp && !m->scriptDone && NextLinkCmd(m, cmd))
    {
        MpLink_CableSend(cmd);
        m->cmdsSent++;
    }
    // The game drains one row per frame; draining all keeps the queue short and sends nothing.
    for (guard = 0; guard < MP_LINK_RX_QUEUE_SIZE && MpLink_CablePop(rows) == MP_LINK_POP_ROW; guard++)
        m->rowsPopped++;
}

// One game frame on every instance: MP_Update (receive, send, flush) then the "game tick".
static void Frame(void)
{
    struct MpEvent ev;
    u8 i;

    for (i = 0; i < sCount; i++)
    {
        Sel(i);
        MP_Update();
        while (MP_PollEvent(&ev))
        {
            if (ev.type == MP_EVENT_INTERACT_REQUEST)
                MP_RespondInteract(MP_INTERACT_RESULT_ACCEPTED);
            else if (ev.type == MP_EVENT_INTERACT_RESULT)
                sResult[i] = ev.result;
        }
        PresenceTick(i);
        LinkTick(i);
    }
    WaitNextFrame();
}

static void FramesFor(u32 ms)
{
    u32 start = MpTransport_GetTimeMs();

    while ((u32)(MpTransport_GetTimeMs() - start) < ms)
        Frame();
}

typedef bool8 (*NbCond)(u8 instance);

static bool8 FramesUntil(NbCond cond, u8 instance, u32 timeoutMs)
{
    u32 start = MpTransport_GetTimeMs();

    for (;;)
    {
        bool8 ok;

        Sel(instance);
        ok = cond(instance);
        if (ok || (u32)(MpTransport_GetTimeMs() - start) >= timeoutMs)
            return ok;
        Frame();
    }
}

static bool8 CondInSession(u8 i)    { return MP_GetState() == MP_STATE_IN_SESSION; }
static bool8 CondGrouped(u8 i)      { return MP_LinkGetState() == MP_LINK_STATE_GROUPED; }
static bool8 CondUngrouped(u8 i)    { return MP_LinkGetState() == MP_LINK_STATE_NONE; }
static bool8 CondResult(u8 i)       { return sResult[i] != MP_INTERACT_RESULT_NONE; }
static bool8 CondScriptDone(u8 i)   { return !sLink[i].active || sLink[i].scriptDone; }
static bool8 CondDecided(u8 i)
{
    u8 st = MP_TradeCommitPoll();

    return st == MP_TRADE_COMMIT_COMMIT || st == MP_TRADE_COMMIT_ABORT;
}
static bool8 CondBusUp(u8 i)
{
    struct MpLinkCableInfo info;

    MpLink_CableGetInfo(&info);
    return info.busUp;
}
static bool8 CondTransportClosed(u8 i)
{
    u8 k;

    for (k = 0; k < sCount; k++)
    {
        Sel(k);
        if (MpTransport_IsOpen())
            return FALSE;
    }
    return TRUE;
}

// ===========================================================================
// Fixtures (cards and parties for the view requests; never the save)
// ===========================================================================

static u8 sFixtureParty[NB_MAX_INSTANCES][PARTY_SIZE][MP_MON_WIRE_SIZE];

static void NbCardProvider(struct MpTrainerCard *c)
{
    u8 inst = gMpSession->instance;
    u32 i;

    memset(c, 0, sizeof(*c));
    c->gender = inst & 1;
    c->stars = 2;
    c->hasPokedex = TRUE;
    c->hofDebutHours = 20;
    c->hofDebutMinutes = 1;
    c->caughtMonsCount = 120;
    c->trainerId = (u16)(0x1000 + inst);
    c->playTimeHours = 50;
    c->playTimeMinutes = 30;
    c->linkBattleWins = 7;
    c->pokemonTrades = 12;
    c->money = 50000;
    for (i = 0; i < MP_TRAINER_CARD_PROFILE_WORDS; i++)
        c->easyChatProfile[i] = MP_CARD_EC_EMPTY_WORD;
    memset(c->name, EOS, sizeof(c->name));
    c->name[0] = CHAR_N;
    c->name[1] = CHAR_B;
    c->name[2] = (u8)(CHAR_0 + inst);
}

static u8 NbPartyProvider(u8 mons[PARTY_SIZE][MP_MON_WIRE_SIZE])
{
    u8 inst = gMpSession->instance;
    u8 i;

    if (inst >= NB_MAX_INSTANCES)
        return 0;
    for (i = 0; i < PARTY_SIZE; i++)
        memcpy(mons[i], sFixtureParty[inst][i], MP_MON_WIRE_SIZE);
    return PARTY_SIZE;
}

static void MakeParties(void)
{
    static const u16 sSpecies[PARTY_SIZE] = {
        SPECIES_SCEPTILE, SPECIES_SWAMPERT, SPECIES_GARDEVOIR, SPECIES_AGGRON, SPECIES_SALAMENCE, SPECIES_METAGROSS,
    };
    u8 inst, i;

    for (inst = 0; inst < NB_MAX_INSTANCES; inst++)
    {
        for (i = 0; i < PARTY_SIZE; i++)
            MpPlayerData_MakeTestMon(sFixtureParty[inst][i], sSpecies[i], (u8)(40 + i * 5), 0x20000000u * (inst + 1) + 0x101u * (i + 1));
    }
}

// ===========================================================================
// Measurement
// ===========================================================================

struct NbSnap
{
    u32 ms;
    struct MpNetTotals tot[NB_MAX_INSTANCES];
    u32 typeBytes[MP_NETSTATS_TYPES];   // sent message bytes, all instances
    u32 typeCount[MP_NETSTATS_TYPES];
};

struct NbRow
{
    u8 players;
    const char *activity;
    u32 ms;
    u32 clientUpIp;         // B/s, average over clients
    u32 clientDownIp;
    u32 hostUpIp;
    u32 hostDownIp;
    u32 totalIp;            // bytes sent by all instances (IP estimate)
    u32 totalMsg;           // message bytes sent by all instances
    u8 topType[NB_TOP_TYPES];
    u32 topPermille[NB_TOP_TYPES];
    u8 topCount;
};

static struct NbSnap sBegin, sEnd;
static struct MpNetTypeStats sTypes[MP_NETSTATS_TYPES];
static struct NbRow sRows[NB_MAX_ROWS];
static u32 sRowCount;

static void Snapshot(struct NbSnap *s)
{
    u8 i;
    u32 t;

    memset(s, 0, sizeof(*s));
    s->ms = MpTransport_GetTimeMs();
    for (i = 0; i < sCount; i++)
    {
        Sel(i);
        MpNetStats_GetTotals(&s->tot[i]);
        MpNetStats_GetTypeTotals(sTypes);
        for (t = 0; t < MP_NETSTATS_TYPES; t++)
        {
            s->typeBytes[t] += sTypes[t].sentBytes;
            s->typeCount[t] += sTypes[t].sentCount;
        }
    }
}

static u32 PerSec(u32 amount, u32 ms)
{
    return (ms == 0) ? 0 : (u32)(((u64)amount * 1000) / ms);
}

static u32 IpBytes(u32 enetBytes, u32 datagrams)
{
    return enetBytes + datagrams * MP_NETSTATS_IP_UDP_OVERHEAD;
}

#define TENTHS(x) (unsigned)((x) / 10), (unsigned)((x) % 10)

static void PhaseBegin(void)
{
    Snapshot(&sBegin);
}

static void PhaseEnd(const char *activity)
{
    struct NbRow *row;
    u32 ms, i, t, k;
    u32 clientUp = 0, clientDown = 0, typeDelta[MP_NETSTATS_TYPES];

    Snapshot(&sEnd);
    ms = sEnd.ms - sBegin.ms;
    if (sRowCount >= NB_MAX_ROWS)
        return;
    row = &sRows[sRowCount++];
    memset(row, 0, sizeof(*row));
    row->players = sCount;
    row->activity = activity;
    row->ms = ms;

    for (i = 0; i < sCount; i++)
    {
        const struct MpNetTotals *a = &sBegin.tot[i];
        const struct MpNetTotals *b = &sEnd.tot[i];
        u32 msgUp = b->msgSentBytes - a->msgSentBytes;
        u32 msgDown = b->msgRecvBytes - a->msgRecvBytes;
        u32 enetUp = b->wireSentBytes - a->wireSentBytes;
        u32 enetDown = b->wireRecvBytes - a->wireRecvBytes;
        u32 dgUp = b->wireSentDatagrams - a->wireSentDatagrams;
        u32 dgDown = b->wireRecvDatagrams - a->wireRecvDatagrams;
        u32 ipUp = IpBytes(enetUp, dgUp);
        u32 ipDown = IpBytes(enetDown, dgDown);

        MP_LOG("netbench: %uP %-6s %s%u | up: msg %5u B/s, ENet %5u B/s, IP %5u B/s, %3u.%u dgram/s"
               " | down: msg %5u B/s, ENet %5u B/s, IP %5u B/s, %3u.%u dgram/s\n",
               (unsigned)sCount, activity, (i == 0) ? "host  i" : "client i", (unsigned)i,
               (unsigned)PerSec(msgUp, ms), (unsigned)PerSec(enetUp, ms), (unsigned)PerSec(ipUp, ms),
               TENTHS(PerSec(dgUp * 10, ms)),
               (unsigned)PerSec(msgDown, ms), (unsigned)PerSec(enetDown, ms), (unsigned)PerSec(ipDown, ms),
               TENTHS(PerSec(dgDown * 10, ms)));
        if (i == 0)
        {
            row->hostUpIp = PerSec(ipUp, ms);
            row->hostDownIp = PerSec(ipDown, ms);
        }
        else
        {
            clientUp += PerSec(ipUp, ms);
            clientDown += PerSec(ipDown, ms);
        }
        row->totalIp += ipUp;
        row->totalMsg += msgUp;
    }
    if (sCount > 1)
    {
        row->clientUpIp = clientUp / (sCount - 1);
        row->clientDownIp = clientDown / (sCount - 1);
    }

    for (t = 0; t < MP_NETSTATS_TYPES; t++)
        typeDelta[t] = sEnd.typeBytes[t] - sBegin.typeBytes[t];
    for (k = 0; k < NB_TOP_TYPES; k++)
    {
        u32 best = 0;
        s32 bestType = -1;

        for (t = 0; t < MP_NETSTATS_TYPES; t++)
        {
            bool8 taken = FALSE;

            for (i = 0; i < row->topCount; i++)
                taken = taken || (row->topType[i] == t);
            if (!taken && typeDelta[t] > best)
            {
                best = typeDelta[t];
                bestType = (s32)t;
            }
        }
        if (bestType < 0)
            break;
        row->topType[row->topCount] = (u8)bestType;
        row->topPermille[row->topCount] = (row->totalMsg == 0) ? 0 : (u32)(((u64)best * 1000) / row->totalMsg);
        row->topCount++;
    }
    for (t = 0; t < MP_NETSTATS_TYPES; t++)
    {
        u32 n = sEnd.typeCount[t] - sBegin.typeCount[t];

        if (n == 0)
            continue;
        MP_LOG("netbench: %uP %-6s   sent 0x%02X %-18s %6u msg %8u B (avg %u B/msg)\n", (unsigned)sCount, activity,
               (unsigned)t, MpNetStats_TypeName((u8)t), (unsigned)n, (unsigned)typeDelta[t], (unsigned)(typeDelta[t] / n));
    }
}

// Sent message bytes of one type during the last measured phase (sBegin .. sEnd).
static u32 PhaseTypeBytes(u8 type)
{
    return sEnd.typeBytes[type] - sBegin.typeBytes[type];
}

static void PrintSummary(void)
{
    u32 r, k;

    MP_LOG("netbench: ===== PS-053 traffic benchmark: loopback, 60 Hz pump, B/s on the wire (IP = ENet UDP payload + %u B IPv4/UDP per datagram) =====\n",
           (unsigned)MP_NETSTATS_IP_UDP_OVERHEAD);
    MP_LOG("netbench: %-3s %-6s %6s | %-18s | %-18s | %9s %9s %8s | %s\n", "P", "act", "secs",
           "client up/down B/s", "host up/down B/s", "all IP B", "all msg B", "overhead", "top messages (share of msg bytes)");
    for (r = 0; r < sRowCount; r++)
    {
        const struct NbRow *row = &sRows[r];
        char top[128];
        u32 pos = 0;
        u32 overhead = (row->totalIp <= row->totalMsg) ? 0 : (u32)(((u64)(row->totalIp - row->totalMsg) * 1000) / row->totalIp);

        top[0] = '\0';
        for (k = 0; k < row->topCount && pos < sizeof(top); k++)
        {
            int w = snprintf(&top[pos], sizeof(top) - pos, "%s%s %u.%u%%", (k == 0) ? "" : ", ",
                             MpNetStats_TypeName(row->topType[k]), TENTHS(row->topPermille[k]));

            if (w < 0)
                break;
            pos += (u32)w;
        }
        MP_LOG("netbench: %-3u %-6s %4u.%u | %7u / %-8u | %7u / %-8u | %9u %9u %6u.%u%% | %s\n",
               (unsigned)row->players, row->activity, TENTHS(row->ms / 100),
               (unsigned)row->clientUpIp, (unsigned)row->clientDownIp, (unsigned)row->hostUpIp, (unsigned)row->hostDownIp,
               (unsigned)row->totalIp, (unsigned)row->totalMsg, TENTHS(overhead), top);
    }
    MP_LOG("netbench: overhead = share of the IP bytes that is ENet + IPv4/UDP headers, acks and ENet pings (not our messages)\n");
    MP_LOG("netbench: view/trade/battle: read \"all IP B\" as the cost of one such exchange; trade and battle are modelled link screens (see mp_selftest_netbench.c)\n");
}

// ===========================================================================
// Session and activities
// ===========================================================================

static void SetupPlayers(void)
{
    u8 i;

    memset(sPlayers, 0, sizeof(sPlayers));
    for (i = 0; i < NB_MAX_INSTANCES; i++)
    {
        struct NbPlayer *pl = &sPlayers[i];

        pl->p.mapGroup = MAP_GROUP(MAP_LITTLEROOT_TOWN);
        pl->p.mapNum = MAP_NUM(MAP_LITTLEROOT_TOWN);
        pl->p.x = 4;
        pl->p.y = (s16)(4 + i);
        pl->p.facing = DIR_SOUTH;
        pl->p.movement = MP_MOVE_NONE;
        pl->p.avatarState = MP_AVATAR_ON_FOOT;
        pl->p.busy = MP_BUSY_NONE;
        pl->dir = 1;
    }
}

static void SetMovement(u8 stepEvery, u8 movement, u8 avatarState)
{
    u8 i;

    for (i = 0; i < sCount; i++)
    {
        sPlayers[i].stepEvery = stepEvery;
        sPlayers[i].stepTimer = 0;
        sPlayers[i].p.movement = movement;
        sPlayers[i].p.avatarState = avatarState;
    }
}

static bool8 StartSession(u8 players)
{
    static const char *const sNames[NB_MAX_INSTANCES] = { "NbHost", "NbA", "NbB", "NbC" };
    bool8 hosting = FALSE;
    u8 i;

    sCount = players;
    memset(sLink, 0, sizeof(sLink));
    memset(sResult, 0, sizeof(sResult));
    memset(sLastRequestMs, 0, sizeof(sLastRequestMs));
    SetupPlayers();
    for (i = 0; i < NB_MAX_INSTANCES; i++)
        MpSession_ResetInstance(i);
    for (i = 0; i < players; i++)
    {
        Sel(i);
        MP_SetLocalPresence(&sPlayers[i].p);
    }

    Sel(0);
    MP_SetNameOverride(sNames[0]);
    for (sPort = NB_PORT_FIRST; sPort <= NB_PORT_LAST && !hosting; sPort++)
        hosting = MP_Host(sPort);
    sPort--;
    NB_CHECK(hosting, "host on a port in 27510..27519");
    if (!hosting)
        return FALSE;
    ResetFrameClock();
    for (i = 1; i < players; i++)
    {
        Sel(i);
        MP_SetNameOverride(sNames[i]);
        NB_CHECK(MP_Join("127.0.0.1", sPort), "MP_Join");
        NB_CHECK(FramesUntil(CondInSession, i, 5000), "client in session");
    }
    for (i = 0; i < players; i++)
    {
        Sel(i);
        if (MP_GetState() != MP_STATE_IN_SESSION || MP_GetLocalSlot() != i)
        {
            MP_LOG("selftest: FAIL: netbench: instance %u is not in session as slot %u\n", (unsigned)i, (unsigned)i);
            sFail++;
            return FALSE;
        }
    }
    FramesFor(NB_SETTLE_MS); // the join burst (WELCOME, cached presence) is not part of any row
    return TRUE;
}

static void EndSession(void)
{
    u8 i;

    for (i = 0; i < sCount; i++)
    {
        Sel(i);
        MP_Leave();
    }
    FramesUntil(CondTransportClosed, 0, 3000);
    for (i = 0; i < sCount; i++)
    {
        Sel(i);
        MpTransport_Close();
        MpSession_ResetInstance(i);
    }
    Sel(0);
}

static void WaitRequestGap(u8 requester)
{
    while (sLastRequestMs[requester] != 0
        && (u32)(MpTransport_GetTimeMs() - sLastRequestMs[requester]) < MP_INTERACT_MIN_GAP_MS + 100)
        Frame();
}

// requester asks targetSlot; the target auto-accepts in Frame(). TRUE if ACCEPTED.
static bool8 Interact(u8 requester, u8 targetSlot, u8 kind)
{
    WaitRequestGap(requester);
    Sel(requester);
    sResult[requester] = MP_INTERACT_RESULT_NONE;
    sLastRequestMs[requester] = MpTransport_GetTimeMs();
    if (!MP_SendInteractRequest(targetSlot, kind))
        return FALSE;
    if (!FramesUntil(CondResult, requester, 4000))
        return FALSE;
    return sResult[requester] == MP_INTERACT_RESULT_ACCEPTED;
}

static void RunView(u8 requester, u8 target)
{
    void (*savedCard)(struct MpTrainerCard *) = gMpLocalTrainerCardFunc;
    u8 (*savedParty)(u8 mons[PARTY_SIZE][MP_MON_WIRE_SIZE]) = gMpLocalPartyFunc;
    u8 count = 0;
    static u8 sMons[PARTY_SIZE][MP_MON_WIRE_SIZE];

    gMpLocalTrainerCardFunc = NbCardProvider;
    gMpLocalPartyFunc = NbPartyProvider;
    WaitRequestGap(requester);
    PhaseBegin();
    NB_CHECK(Interact(requester, target, MP_INTERACT_TRAINER_CARD), "view: TRAINER_CARD accepted");
    NB_CHECK(Interact(requester, target, MP_INTERACT_VIEW_PARTY), "view: VIEW_PARTY accepted");
    Sel(requester);
    NB_CHECK(MP_GetViewedParty(target, &count, sMons) && count == PARTY_SIZE, "view: 6 mons received");
    FramesFor(200);
    PhaseEnd("view");
    NB_CHECK(PhaseTypeBytes(MP_MSG_PARTY_DATA) > 0 && PhaseTypeBytes(MP_MSG_TRAINER_CARD_DATA) > 0, "view: card and party data counted");
    Sel(requester);
    MP_ClearViewedData();
    gMpLocalTrainerCardFunc = savedCard;
    gMpLocalPartyFunc = savedParty;
}

static void AddOp(struct NbLinkMember *m, u8 op, u16 arg, u16 arg2)
{
    if (m->scriptLen >= NB_SCRIPT_MAX)
        return;
    m->script[m->scriptLen].op = op;
    m->script[m->scriptLen].arg = arg;
    m->script[m->scriptLen].arg2 = arg2;
    m->scriptLen++;
}

#define BLOCK(m, size)      AddOp(m, OP_BLOCK, (u16)(size), 0)
#define CMD(m, word, arg)   AddOp(m, OP_CMD, (u16)(word), (u16)(arg))
#define WAIT(m, frames)     AddOp(m, OP_WAIT, (u16)(frames), 0)

// trade.c: the player-data exchange, the party in 3 x 200-byte blocks, mail (220), ribbons (40),
// each requested by link id 0 (SEND_BLOCK_REQ); then the menu, the two 20-byte selection
// blocks, the two trade animation blocks and the close handshake.
static void BuildTradeScript(struct NbLinkMember *m, bool8 master)
{
    static const u16 sReqSizes[][2] = { { 1, 200 }, { 1, 200 }, { 1, 200 }, { 3, 220 }, { 4, 40 } };
    u32 i;

    WAIT(m, 2);
    BLOCK(m, sizeof(struct LinkPlayerBlock));
    WAIT(m, 30);
    for (i = 0; i < ARRAY_COUNT(sReqSizes); i++)
    {
        if (master)
            CMD(m, LINKCMD_SEND_BLOCK_REQ, sReqSizes[i][0]);
        else
            WAIT(m, 1);
        BLOCK(m, sReqSizes[i][1]);
        WAIT(m, 10);
    }
    WAIT(m, 90);    // choosing a mon
    BLOCK(m, 20);   // LINKCMD_SET_MONS_TO_TRADE
    WAIT(m, 45);    // the confirm prompt
    BLOCK(m, 20);   // LINKCMD_READY_TO_TRADE
    WAIT(m, 60);    // trade animation
    BLOCK(m, 20);
    WAIT(m, 120);
    BLOCK(m, 20);
    WAIT(m, 30);
    CMD(m, LINKCMD_READY_CLOSE_LINK, 0);
}

// battle_main.c: the player-data exchange, LinkBattlerHeader, the party in 3 x 200-byte
// blocks, link id 0's RNG seed; then BATTLE_TURNS estimated turns and the close handshake.
static void BuildBattleScript(struct NbLinkMember *m, bool8 master)
{
    u32 turn, i;

    WAIT(m, 2);
    BLOCK(m, sizeof(struct LinkPlayerBlock));
    WAIT(m, 30);
    BLOCK(m, sizeof(struct LinkBattlerHeader));
    WAIT(m, 10);
    for (i = 0; i < 3; i++)
    {
        BLOCK(m, sizeof(struct Pokemon) * 2);
        WAIT(m, 4);
    }
    if (master)
        BLOCK(m, sizeof(u32)); // gRecordedBattleRngSeed
    WAIT(m, 60);    // intro
    for (turn = 0; turn < BATTLE_TURNS; turn++)
    {
        CMD(m, LINKCMD_READY_EXIT_STANDBY, 0);
        WAIT(m, 30);
        if (master)
        {
            for (i = 0; i < BATTLE_TURN_MASTER_BLOCKS; i++)
            {
                BLOCK(m, BATTLE_TURN_MASTER_BYTES);
                WAIT(m, 10);
            }
        }
        else
        {
            for (i = 0; i < BATTLE_TURN_PARTNER_BLOCKS; i++)
            {
                BLOCK(m, BATTLE_TURN_PARTNER_BYTES);
                WAIT(m, 40);
            }
        }
        WAIT(m, 30);
    }
    CMD(m, LINKCMD_READY_CLOSE_LINK, 0);
}

static void SetBusy(u8 a, u8 b, u8 busy)
{
    sPlayers[a].p.busy = busy;
    sPlayers[b].p.busy = busy;
}

// a = link id 0 (the lower slot), b = the partner; b asks a (the INTERACT request), like the game.
static void RunLink(const char *activity, u8 a, u8 b, u8 kind, bool8 isTrade)
{
    u8 mask = (u8)((1 << a) | (1 << b));
    u8 members[2];
    u8 k;
    bool8 ok;

    members[0] = a;
    members[1] = b;
    memset(&sLink[a], 0, sizeof(sLink[a]));
    memset(&sLink[b], 0, sizeof(sLink[b]));
    if (isTrade)
    {
        BuildTradeScript(&sLink[a], TRUE);
        BuildTradeScript(&sLink[b], FALSE);
    }
    else
    {
        BuildBattleScript(&sLink[a], TRUE);
        BuildBattleScript(&sLink[b], FALSE);
    }
    NB_CHECK(sLink[a].scriptLen < NB_SCRIPT_MAX && sLink[b].scriptLen < NB_SCRIPT_MAX, "link script fits");

    WaitRequestGap(b);
    PhaseBegin();
    ok = Interact(b, a, kind);
    NB_CHECK(ok, "link: INTERACT request accepted");
    if (!ok)
    {
        PhaseEnd(activity);
        return;
    }
    SetBusy(a, b, isTrade ? MP_BUSY_MENU : MP_BUSY_BATTLE);
    for (k = 0; k < 2; k++)
    {
        Sel(members[k]);
        NB_CHECK(MP_LinkJoin(mask), "link: MP_LinkJoin");
    }
    ok = FramesUntil(CondGrouped, a, 3000) && FramesUntil(CondGrouped, b, 3000);
    NB_CHECK(ok, "link: group formed");
    sLink[a].active = TRUE;
    sLink[b].active = TRUE;
    ok = ok && FramesUntil(CondBusUp, a, 3000) && FramesUntil(CondBusUp, b, 3000);
    NB_CHECK(ok, "link: bus up");
    ok = ok && FramesUntil(CondScriptDone, a, 60000) && FramesUntil(CondScriptDone, b, 60000);
    NB_CHECK(ok, "link: both command streams finished");
    FramesFor(100); // the last rows

    if (isTrade && ok)
    {
        Sel(a);
        NB_CHECK(MP_TradeCommitReady(NB_TRADE_ID), "trade: READY (a)");
        Sel(b);
        NB_CHECK(MP_TradeCommitReady(NB_TRADE_ID), "trade: READY (b)");
        NB_CHECK(FramesUntil(CondDecided, a, 3000) && FramesUntil(CondDecided, b, 3000), "trade: commit decided");
        Sel(a);
        NB_CHECK(MP_TradeCommitPoll() == MP_TRADE_COMMIT_COMMIT, "trade: COMMIT (a)");
        MP_TradeCommitReset();
        Sel(b);
        NB_CHECK(MP_TradeCommitPoll() == MP_TRADE_COMMIT_COMMIT, "trade: COMMIT (b)");
        MP_TradeCommitReset();
    }

    // PS-051 agreed end: both say LINK_DONE, the host ends the group (DONE).
    for (k = 0; k < 2; k++)
    {
        Sel(members[k]);
        MpLink_MemberSetAgreed();
        MpLink_MemberSendDone();
    }
    ok = FramesUntil(CondUngrouped, a, 3000) && FramesUntil(CondUngrouped, b, 3000);
    NB_CHECK(ok, "link: group ended after LINK_DONE");
    for (k = 0; k < 2; k++)
    {
        Sel(members[k]);
        if (MP_LinkGetState() != MP_LINK_STATE_NONE)
            MP_LinkLeave();
        MpLink_CableClose(); // the game's CloseLink: back to CLOSED for the next activity
        sLink[members[k]].active = FALSE;
    }
    SetBusy(a, b, MP_BUSY_NONE);
    FramesFor(200);
    PhaseEnd(activity);
    MP_LOG("netbench: %uP %-6s link ids %u+%u: %u + %u LINK_CMD sent, %u + %u rows played\n", (unsigned)sCount, activity,
           (unsigned)a, (unsigned)b, (unsigned)sLink[a].cmdsSent, (unsigned)sLink[b].cmdsSent,
           (unsigned)sLink[a].rowsPopped, (unsigned)sLink[b].rowsPopped);
    NB_CHECK(PhaseTypeBytes(MP_MSG_LINK_ROW) > 0, "link: LINK_ROW traffic counted");
    NB_CHECK(a == 0 || PhaseTypeBytes(MP_MSG_LINK_CMD) > 0, "link: LINK_CMD traffic counted");
}

static void RunScenario(u8 players)
{
    u8 viewer = 1;
    u8 viewed = (players > 2) ? 2 : 0;
    u8 linkA = (players > 2) ? 1 : 0;
    u8 linkB = (players > 2) ? 2 : 1;
    struct MpNetOverlay ov;
    u32 rowWalk;

    MP_LOG("netbench: --- %u players (host + %u clients) ---\n", (unsigned)players, (unsigned)(players - 1));
    if (!StartSession(players))
    {
        EndSession();
        return;
    }

    SetMovement(0, MP_MOVE_NONE, MP_AVATAR_ON_FOOT);
    PhaseBegin();
    FramesFor(NB_IDLE_MS);
    PhaseEnd("idle");

    SetMovement(16, MP_MOVE_WALK, MP_AVATAR_ON_FOOT);
    PhaseBegin();
    FramesFor(NB_WALK_MS);
    PhaseEnd("walk");
    rowWalk = sRowCount - 1;
    NB_CHECK(PhaseTypeBytes(MP_MSG_PRESENCE) > 0, "walk: PRESENCE counted");
    NB_CHECK(sRows[rowWalk].clientUpIp < 2048, "walk: a client sends < 2 KB/s on the wire");

    Sel(0);
    MP_GetNetOverlay(&ov);
    NB_CHECK(ov.open && ov.inSession && ov.isHost && ov.players == players, "overlay: host session state");
    NB_CHECK(ov.upBitsPerSec > 0 && ov.downBitsPerSec > 0, "overlay: rates while walking");

    SetMovement(4, MP_MOVE_WALK_FASTER, MP_AVATAR_MACH_BIKE);
    PhaseBegin();
    FramesFor(NB_BIKE_MS);
    PhaseEnd("bike");

    SetMovement(0, MP_MOVE_NONE, MP_AVATAR_ON_FOOT);
    FramesFor(300);
    RunView(viewer, viewed);
    RunLink("trade", linkA, linkB, MP_INTERACT_TRADE, TRUE);
    RunLink("battle", linkA, linkB, MP_INTERACT_BATTLE, FALSE);

    // The 2-player run also shows the --mp-net-stats output once: the 10 s summary (due at
    // once, the session is older than that) and the full table of every instance at close.
    if (players == 2)
    {
        MP_SetNetStatsLogging(TRUE);
        Frame();
    }
    EndSession();
    if (players == 2)
        MP_SetNetStatsLogging(FALSE);
}

static void TestCounters(void)
{
    static const u8 sPacket[] = { MP_MSG_PING, 1, 4, 0, 1, 2, 3, 4 };
    struct MpNetTotals tot;
    u32 failuresBefore = sFail;

    // Out-of-range arguments must be ignored without touching anything.
    MpNetStats_OnSend(MP_TRANSPORT_INSTANCE_COUNT, 0, sPacket, sizeof(sPacket));
    MpNetStats_OnSend(7, MP_NETSTATS_PEERS, sPacket, sizeof(sPacket));
    MpNetStats_OnSend(7, 0, NULL, 8);
    MpNetStats_OnRecv(7, 0, sPacket, 0);
    MpNetStats_OnRecv(0xFF, 0xFF, NULL, 0xFFFFFFFFu);
    MpNetStats_OnClose(0xFF, NULL, 0);
    // Instance 7 is never opened by the other groups: count on it directly.
    MpNetStats_OnOpen(7, 1000);
    MpNetStats_OnSend(7, 1, sPacket, sizeof(sPacket));
    MpNetStats_OnRecv(7, 3, sPacket, 5);
    MpNetStats_OnSend(7, MP_NETSTATS_PEERS, sPacket, sizeof(sPacket)); // ignored
    MpTransport_SelectInstance(7);
    MpNetStats_GetTotals(&tot);
    MpNetStats_GetTypeTotals(sTypes);
    NB_CHECK(tot.msgSentCount == 1 && tot.msgSentBytes == sizeof(sPacket), "counters: one send counted");
    NB_CHECK(tot.msgRecvCount == 1 && tot.msgRecvBytes == 5, "counters: one receive counted");
    NB_CHECK(sTypes[MP_MSG_PING].sentCount == 1 && sTypes[MP_MSG_PING].recvBytes == 5, "counters: by type");
    NB_CHECK(!MpNetStats_GetPeerTypeTotals(MP_NETSTATS_PEERS, sTypes), "counters: peer out of range");
    NB_CHECK(MpNetStats_GetPeerTypeTotals(3, sTypes) && sTypes[MP_MSG_PING].recvCount == 1, "counters: per peer");
    MpNetStats_OnClose(7, NULL, 2000);
    MpTransport_SelectInstance(0);
    NB_CHECK(strcmp(MpNetStats_TypeName(MP_MSG_PRESENCE), "PRESENCE") == 0 && strcmp(MpNetStats_TypeName(0xEE), "?") == 0,
             "counters: type names");
    if (sFail == failuresBefore)
        MP_LOG("selftest: netbench: counters OK\n");
}

u32 MpSelftest_NetBench(void)
{
    u32 failuresBefore;

    sFail = 0;
    sRowCount = 0;
    TestCounters();
    MakeParties();
    failuresBefore = sFail;
    RunScenario(2);
    RunScenario(4);
    PrintSummary();
    Sel(0);
    if (sFail == failuresBefore)
        MP_LOG("selftest: netbench: benchmark scenarios OK\n");
    return sFail;
}
