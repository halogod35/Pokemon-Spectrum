// Virtual link cable, session side (PS-046, protocol.md 6.12). No game state here.
//
// Host ("the bus"): link groups of 2..4 slots, formed when every member asked for the same
// mask (LINK_JOIN). Per group it tracks which members have the cable open (LINK_OPEN /
// LINK_CLOSE, with the member's open sequence), starts the bus when link id 0 asks and
// everyone is open (LINK_START -> BUS_START with a new bus epoch), queues each member's
// commands (LINK_CMD) and, once per MP_Update, emits rows: one command (or zeros) per member,
// sent to every member including the sender (LINK_ROW). A member closing or re-opening ends
// the bus (BUS_END). The host sequences groups it is not part of too (Director ruling).
//
// Member: the request / group state, the open sequence, the received-row queue that
// MP_LinkMain1 drains one row per game tick, and a watchdog on our own commands' echoes.
//
// The host's own member (slot 0) talks to the host side through direct calls, never the
// network. Everything else goes over channel 0 (reliable, ordered), so a member always sees
// BUS_START, its rows and BUS_END in the order the host produced them.

#include <string.h>
#include "mp/mp_link.h"
#include "mp/mp_session_internal.h"

enum
{
    MP_LINK_PHASE_CLOSED,   // the game's cable is not open (or not yet announced)
    MP_LINK_PHASE_OPEN,     // LINK_OPEN sent, waiting for BUS_START
    MP_LINK_PHASE_BUS,      // BUS_START received: rows flow
};

enum
{
    MP_LINK_RX_ROW,
    MP_LINK_RX_BUS_END,
};

struct MpLinkHostGroup
{
    bool8 used;
    u8 id;
    u8 memberMask;                              // slots
    u8 count;
    u8 slots[MP_LINK_MAX_MEMBERS];              // [link id] -> slot
    bool8 open[MP_LINK_MAX_MEMBERS];
    u8 openSeq[MP_LINK_MAX_MEMBERS];
    bool8 startPending;                         // link id 0 asked while someone was closed
    bool8 busUp;
    u8 busEpoch;
    u16 rowSeq;
    u8 fifoHead[MP_LINK_MAX_MEMBERS];
    u8 fifoCount[MP_LINK_MAX_MEMBERS];
    u16 fifo[MP_LINK_MAX_MEMBERS][MP_LINK_HOST_FIFO_SIZE][MP_LINK_CMD_WORDS];
};

struct MpLinkHost
{
    u8 nextGroupId;
    u8 nextBusEpoch;
    u8 requestMask[MP_MAX_PLAYERS];             // pending LINK_JOIN per slot, 0 = none
    struct MpLinkHostGroup groups[MP_LINK_MAX_GROUPS];
    u32 rowsEmitted;
    u32 groupsFormed;
    u32 cmdsRefused;                            // PS-047 pre-filter
};

struct MpLinkRxItem
{
    u8 type;                                    // MP_LINK_RX_*
    u16 cmds[MP_LINK_MAX_MEMBERS][MP_LINK_CMD_WORDS];
};

struct MpLinkMember
{
    u8 state;                                   // MP_LINK_STATE_*
    u8 requestMask;
    u8 endReason;                               // MP_LINK_END_*
    u8 groupId;
    u8 memberMask;
    u8 linkId;
    u8 count;
    u8 phase;                                   // MP_LINK_PHASE_*
    u8 openSeq;
    u8 openMask;                                // by link id, from LINK_STATUS
    bool8 startSent;
    u8 busEpoch;
    bool8 busEnded;                             // BUS_END queued: stop sending
    bool8 lost;
    u32 outstanding;                            // own commands sent but not yet echoed
    u32 lastProgressMs;
    u16 rxHead;
    u16 rxCount;
    struct MpLinkRxItem rx[MP_LINK_RX_QUEUE_SIZE];
    u32 cmdsSent;
    u32 rowsReceived;
    u16 rxPeak;
};

static struct MpLinkHost sLinkHost[MP_SESSION_INSTANCE_COUNT];
static struct MpLinkMember sLinkMember[MP_SESSION_INSTANCE_COUNT];
static u8 sLinkBuf[MP_MAX_PACKET_SIZE];

static struct MpLinkHost *MpLinkHostState(void)
{
    return &sLinkHost[gMpSession->instance < MP_SESSION_INSTANCE_COUNT ? gMpSession->instance : 0];
}

static struct MpLinkMember *MpLinkMemberState(void)
{
    return &sLinkMember[gMpSession->instance < MP_SESSION_INSTANCE_COUNT ? gMpSession->instance : 0];
}

static u8 MpLinkRankOf(u8 mask, u8 slot)
{
    u8 rank = 0;
    u32 i;

    for (i = 0; i < slot && i < MP_LINK_MAX_MEMBERS; i++)
    {
        if (mask & (1 << i))
            rank++;
    }
    return rank;
}

static const char *MpLinkEndName(u8 reason)
{
    switch (reason)
    {
    case MP_LINK_END_NONE:        return "NONE";
    case MP_LINK_END_LEFT:        return "LEFT";
    case MP_LINK_END_LOST:        return "LOST";
    case MP_LINK_END_OVERFLOW:    return "OVERFLOW";
    case MP_LINK_END_REFUSED:     return "REFUSED";
    case MP_LINK_END_SESSION:     return "SESSION";
    case MP_LINK_END_WATCHDOG:    return "WATCHDOG";
    case MP_LINK_END_RX_OVERFLOW: return "RX_OVERFLOW";
    default:                      return "UNKNOWN";
    }
}

static void MpLinkMemberHandleCtl(u8 type, const struct MpMsgLinkCtl *ctl);
static void MpLinkMemberHandleRow(const struct MpMsgLinkRow *row);
static void MpLinkHostHandleCtl(u8 slot, u8 type, const struct MpMsgLinkCtl *ctl);
static void MpLinkHostHandleCmd(u8 slot, const struct MpMsgLinkCmd *cmd);

// ===========================================================================
// Sending
// ===========================================================================

// Member -> host. The host's own member calls the host side directly.
static void MpLinkMemberSendCtl(u8 type, u8 groupId, u8 arg1, u8 arg2)
{
    struct MpMsgLinkCtl ctl;
    u16 len;

    ctl.groupId = groupId;
    ctl.arg1 = arg1;
    ctl.arg2 = arg2;
    if (gMpSession->isHost)
    {
        MpLinkHostHandleCtl(gMpSession->localSlot, type, &ctl);
        return;
    }
    len = MpProto_WriteLinkCtl(sLinkBuf, sizeof(sLinkBuf), type, &ctl);
    MpSession_SendTo(0, sLinkBuf, len, MP_CHANNEL_CONTROL, TRUE);
}

// Host -> member `slot`. The host's own slot gets a direct call.
static void MpLinkHostSendCtl(u8 slot, u8 type, u8 groupId, u8 arg1, u8 arg2)
{
    struct MpMsgLinkCtl ctl;
    u16 len;

    if (slot >= MP_MAX_PLAYERS || !gMpSession->slotUsed[slot])
        return;
    ctl.groupId = groupId;
    ctl.arg1 = arg1;
    ctl.arg2 = arg2;
    if (slot == gMpSession->localSlot)
    {
        MpLinkMemberHandleCtl(type, &ctl);
        return;
    }
    len = MpProto_WriteLinkCtl(sLinkBuf, sizeof(sLinkBuf), type, &ctl);
    if (gMpSession->slotPeer[slot] != MP_PEER_NONE)
        MpSession_SendTo(gMpSession->slotPeer[slot], sLinkBuf, len, MP_CHANNEL_CONTROL, TRUE);
}

// ===========================================================================
// Host
// ===========================================================================

static struct MpLinkHostGroup *MpLinkHostGroupOfSlot(u8 slot)
{
    struct MpLinkHost *h = MpLinkHostState();
    u32 i;

    if (slot >= MP_MAX_PLAYERS)
        return NULL;
    for (i = 0; i < MP_LINK_MAX_GROUPS; i++)
    {
        if (h->groups[i].used && (h->groups[i].memberMask & (1 << slot)))
            return &h->groups[i];
    }
    return NULL;
}

static void MpLinkHostBroadcastCtl(struct MpLinkHostGroup *g, u8 type, u8 arg1, u8 exceptSlot)
{
    u8 groupId = g->id;
    u8 slots[MP_LINK_MAX_MEMBERS];
    u8 count = g->count;
    u32 i;

    // Copy first: a direct call into the host's own member must not see a half-changed group.
    memcpy(slots, g->slots, sizeof(slots));
    for (i = 0; i < count; i++)
    {
        if (slots[i] != exceptSlot)
            MpLinkHostSendCtl(slots[i], type, groupId, arg1, 0);
    }
}

static u8 MpLinkHostOpenMask(const struct MpLinkHostGroup *g)
{
    u8 mask = 0;
    u32 i;

    for (i = 0; i < g->count; i++)
    {
        if (g->open[i])
            mask |= (u8)(1 << i);
    }
    return mask;
}

static bool8 MpLinkHostAllOpen(const struct MpLinkHostGroup *g)
{
    return MpLinkHostOpenMask(g) == (u8)((1 << g->count) - 1);
}

static void MpLinkHostClearFifos(struct MpLinkHostGroup *g)
{
    memset(g->fifoHead, 0, sizeof(g->fifoHead));
    memset(g->fifoCount, 0, sizeof(g->fifoCount));
}

static void MpLinkHostBusDown(struct MpLinkHostGroup *g)
{
    if (!g->busUp)
        return;
    g->busUp = FALSE;
    MpLinkHostClearFifos(g);
    MP_SDLOG("link group %u: bus %u ended\n", (unsigned)g->id, (unsigned)g->busEpoch);
    MpLinkHostBroadcastCtl(g, MP_MSG_LINK_BUS_END, g->busEpoch, MP_SLOT_NONE);
}

static void MpLinkHostStartBus(struct MpLinkHostGroup *g)
{
    struct MpLinkHost *h = MpLinkHostState();
    u8 slots[MP_LINK_MAX_MEMBERS];
    u8 seqs[MP_LINK_MAX_MEMBERS];
    u8 groupId = g->id;
    u8 count = g->count;
    u8 epoch;
    u32 i;

    if (h->nextBusEpoch == 0)
        h->nextBusEpoch = 1;
    epoch = h->nextBusEpoch++;
    g->busEpoch = epoch;
    g->busUp = TRUE;
    g->startPending = FALSE;
    g->rowSeq = 0;
    MpLinkHostClearFifos(g);
    MP_SLOG("link group %u: bus %u started (%u members)\n", (unsigned)groupId, (unsigned)epoch, (unsigned)count);
    memcpy(slots, g->slots, sizeof(slots));
    memcpy(seqs, g->openSeq, sizeof(seqs));
    for (i = 0; i < count; i++)
        MpLinkHostSendCtl(slots[i], MP_MSG_LINK_BUS_START, groupId, epoch, seqs[i]);
}

static void MpLinkHostBroadcastStatus(struct MpLinkHostGroup *g)
{
    MpLinkHostBroadcastCtl(g, MP_MSG_LINK_STATUS, MpLinkHostOpenMask(g), MP_SLOT_NONE);
}

// Ends a group: every member except `exceptSlot` gets LINK_END(reason).
static void MpLinkHostEndGroup(struct MpLinkHostGroup *g, u8 reason, u8 exceptSlot)
{
    struct MpLinkHostGroup copy;

    if (!g->used)
        return;
    MP_SLOG("link group %u ended: %s\n", (unsigned)g->id, MpLinkEndName(reason));
    copy = *g;  // the group is gone before any direct call reaches our own member
    memset(g, 0, sizeof(*g));
    MpLinkHostBroadcastCtl(&copy, MP_MSG_LINK_END, reason, exceptSlot);
}

static void MpLinkHostTryForm(u8 mask)
{
    struct MpLinkHost *h = MpLinkHostState();
    struct MpLinkHostGroup *g = NULL;
    u8 slots[MP_LINK_MAX_MEMBERS];
    u8 count = 0;
    u8 groupId;
    u32 i;

    for (i = 0; i < MP_MAX_PLAYERS; i++)
    {
        if (!(mask & (1 << i)))
            continue;
        if (h->requestMask[i] != mask || MpLinkHostGroupOfSlot((u8)i) != NULL)
            return;
        slots[count++] = (u8)i;
    }
    for (i = 0; i < MP_LINK_MAX_GROUPS; i++)
    {
        if (!h->groups[i].used)
        {
            g = &h->groups[i];
            break;
        }
    }
    if (g == NULL)
        return; // cannot happen with 4 players and groups of >= 2

    if (h->nextGroupId == 0)
        h->nextGroupId = 1;
    groupId = h->nextGroupId++;
    memset(g, 0, sizeof(*g));
    g->used = TRUE;
    g->id = groupId;
    g->memberMask = mask;
    g->count = count;
    memcpy(g->slots, slots, sizeof(g->slots));
    h->groupsFormed++;
    for (i = 0; i < count; i++)
        h->requestMask[slots[i]] = 0;
    MP_SLOG("link group %u formed: slots mask 0x%X (%u members)\n", (unsigned)groupId, (unsigned)mask, (unsigned)count);
    for (i = 0; i < count; i++)
        MpLinkHostSendCtl(slots[i], MP_MSG_LINK_GROUP, groupId, mask, (u8)i);
}

static void MpLinkHostHandleJoin(u8 slot, u8 mask)
{
    struct MpLinkHost *h = MpLinkHostState();
    struct MpLinkHostGroup *g;
    u32 i;

    if (!(mask & (1 << slot)))
    {
        MpLinkHostSendCtl(slot, MP_MSG_LINK_END, 0, MP_LINK_END_REFUSED, 0);
        return;
    }
    for (i = 0; i < MP_MAX_PLAYERS; i++)
    {
        if ((mask & (1 << i)) && !gMpSession->slotUsed[i])
        {
            MpLinkHostSendCtl(slot, MP_MSG_LINK_END, 0, MP_LINK_END_REFUSED, 0);
            return;
        }
    }
    g = MpLinkHostGroupOfSlot(slot);
    if (g != NULL)
        MpLinkHostEndGroup(g, MP_LINK_END_LEFT, slot);
    h->requestMask[slot] = mask;
    MP_SDLOG("link: slot %u asks for mask 0x%X\n", (unsigned)slot, (unsigned)mask);
    MpLinkHostTryForm(mask);
}

static void MpLinkHostHandleLeave(u8 slot)
{
    struct MpLinkHost *h = MpLinkHostState();
    struct MpLinkHostGroup *g = MpLinkHostGroupOfSlot(slot);

    h->requestMask[slot] = 0;
    if (g != NULL)
        MpLinkHostEndGroup(g, MP_LINK_END_LEFT, slot);
}

static void MpLinkHostHandleCtl(u8 slot, u8 type, const struct MpMsgLinkCtl *ctl)
{
    struct MpLinkHostGroup *g;
    u8 li;

    if (slot >= MP_MAX_PLAYERS)
        return;
    switch (type)
    {
    case MP_MSG_LINK_JOIN:
        MpLinkHostHandleJoin(slot, ctl->arg1);
        return;
    case MP_MSG_LINK_LEAVE:
        MpLinkHostHandleLeave(slot);
        return;
    }

    // OPEN / CLOSE / START name a group; a different id is a stale message (the group ended).
    g = MpLinkHostGroupOfSlot(slot);
    if (g == NULL || g->id != ctl->groupId)
        return;
    li = MpLinkRankOf(g->memberMask, slot);
    switch (type)
    {
    case MP_MSG_LINK_OPEN:
        MpLinkHostBusDown(g); // re-opened without closing: the old bus is dead
        g->open[li] = TRUE;
        g->openSeq[li] = ctl->arg1;
        MP_SDLOG("link group %u: link id %u open (seq %u)\n", (unsigned)g->id, (unsigned)li, (unsigned)ctl->arg1);
        MpLinkHostBroadcastStatus(g);
        if (g->startPending && MpLinkHostAllOpen(g))
            MpLinkHostStartBus(g);
        break;
    case MP_MSG_LINK_CLOSE:
        if (!g->open[li] || g->openSeq[li] != ctl->arg1)
            break; // stale
        g->open[li] = FALSE;
        g->startPending = FALSE;
        MP_SDLOG("link group %u: link id %u closed\n", (unsigned)g->id, (unsigned)li);
        MpLinkHostBusDown(g);
        MpLinkHostBroadcastStatus(g);
        break;
    case MP_MSG_LINK_START:
        if (li != 0 || !g->open[0] || g->openSeq[0] != ctl->arg1 || g->busUp)
            break; // only the link master starts the bus, once per open
        if (MpLinkHostAllOpen(g))
            MpLinkHostStartBus(g);
        else
            g->startPending = TRUE;
        break;
    }
}

bool8 MpLink_IsCmdAllowed(const u16 *cmd)
{
    switch (cmd[0])
    {
    case MP_LINKCMD_INIT_BLOCK:     // word 1 = block size
        return cmd[1] <= MP_LINK_MAX_BLOCK_SIZE;
    case MP_LINKCMD_SEND_BLOCK_REQ: // word 1 = index into link.c's sBlockRequests
        return cmd[1] < MP_LINK_BLOCK_REQ_COUNT;
    }
    return TRUE;
}

static void MpLinkHostHandleCmd(u8 slot, const struct MpMsgLinkCmd *cmd)
{
    struct MpLinkHostGroup *g = MpLinkHostGroupOfSlot(slot);
    u8 li, index;

    if (g == NULL || g->id != cmd->groupId || !g->busUp || g->busEpoch != cmd->busEpoch)
        return; // stale: the bus it was sent on has ended
    li = MpLinkRankOf(g->memberMask, slot);
    if (!MpLink_IsCmdAllowed(cmd->cmd))
    {
        // PS-047: a command no honest game sends (it would overflow or leak memory in the
        // receivers' link.c, which also refuses it). Take the sender off the cable: the group
        // ends for everyone, as if it had left (LINK_END(LEFT)), so all members see a link error.
        MP_SLOG("link group %u: link id %u sent a refused link command (0x%04X 0x%04X)\n",
                (unsigned)g->id, (unsigned)li, (unsigned)cmd->cmd[0], (unsigned)cmd->cmd[1]);
        MpLinkHostState()->cmdsRefused++;
        MpLinkHostEndGroup(g, MP_LINK_END_LEFT, MP_SLOT_NONE);
        return;
    }
    if (g->fifoCount[li] >= MP_LINK_HOST_FIFO_SIZE)
    {
        MP_SLOG("link group %u: link id %u overflowed the bus\n", (unsigned)g->id, (unsigned)li);
        MpLinkHostEndGroup(g, MP_LINK_END_OVERFLOW, MP_SLOT_NONE);
        return;
    }
    index = (u8)((g->fifoHead[li] + g->fifoCount[li]) % MP_LINK_HOST_FIFO_SIZE);
    memcpy(g->fifo[li][index], cmd->cmd, sizeof(g->fifo[li][index]));
    g->fifoCount[li]++;
}

static void MpLinkHostEmitRows(struct MpLinkHostGroup *g)
{
    struct MpLinkHost *h = MpLinkHostState();
    struct MpMsgLinkRow row;
    u8 slots[MP_LINK_MAX_MEMBERS];
    u16 len;
    u32 i, guard;

    // Every member queues at most one command per game tick and the rows are drained one per
    // tick, so pairing the FIFOs keeps rows <= the busiest member's commands (as on the cable).
    for (guard = 0; guard < MP_LINK_HOST_FIFO_SIZE && g->used && g->busUp; guard++)
    {
        bool8 any = FALSE;

        memset(&row, 0, sizeof(row));
        row.groupId = g->id;
        row.busEpoch = g->busEpoch;
        row.count = g->count;
        for (i = 0; i < g->count; i++)
        {
            if (g->fifoCount[i] == 0)
                continue;
            memcpy(row.cmds[i], g->fifo[i][g->fifoHead[i]], sizeof(row.cmds[i]));
            g->fifoHead[i] = (u8)((g->fifoHead[i] + 1) % MP_LINK_HOST_FIFO_SIZE);
            g->fifoCount[i]--;
            any = TRUE;
        }
        if (!any)
            break;
        row.rowSeq = g->rowSeq++;
        h->rowsEmitted++;
        // Copy the slots: delivering to our own member can end the group (receive overflow).
        memcpy(slots, g->slots, sizeof(slots));
        len = MpProto_WriteLinkRow(sLinkBuf, sizeof(sLinkBuf), &row);
        for (i = 0; i < row.count; i++)
        {
            u8 slot = slots[i];

            if (slot == gMpSession->localSlot)
            {
                MpLinkMemberHandleRow(&row);
                len = MpProto_WriteLinkRow(sLinkBuf, sizeof(sLinkBuf), &row); // sLinkBuf may have been reused
            }
            else if (slot < MP_MAX_PLAYERS && gMpSession->slotPeer[slot] != MP_PEER_NONE)
            {
                MpSession_SendTo(gMpSession->slotPeer[slot], sLinkBuf, len, MP_CHANNEL_CONTROL, TRUE);
            }
        }
    }
}

void MpLink_HostUpdate(void)
{
    struct MpLinkHost *h = MpLinkHostState();
    u32 i;

    for (i = 0; i < MP_LINK_MAX_GROUPS; i++)
    {
        if (h->groups[i].used && h->groups[i].busUp)
            MpLinkHostEmitRows(&h->groups[i]);
    }
}

void MpLink_HostOnSlotLeft(u8 slot)
{
    struct MpLinkHost *h = MpLinkHostState();
    struct MpLinkHostGroup *g;
    u32 i;

    if (slot >= MP_MAX_PLAYERS)
        return;
    h->requestMask[slot] = 0;
    // Requests waiting for this slot can never complete: refuse them.
    for (i = 0; i < MP_MAX_PLAYERS; i++)
    {
        if (h->requestMask[i] & (1 << slot))
        {
            h->requestMask[i] = 0;
            MpLinkHostSendCtl((u8)i, MP_MSG_LINK_END, 0, MP_LINK_END_LOST, 0);
        }
    }
    g = MpLinkHostGroupOfSlot(slot);
    if (g != NULL)
        MpLinkHostEndGroup(g, MP_LINK_END_LOST, slot);
}

bool8 MpLink_OnHostReceive(u8 slot, u8 channel, u8 type, const u8 *payload, u32 len)
{
    struct MpMsgLinkCtl ctl;
    struct MpMsgLinkCmd cmd;

    if (channel != MP_CHANNEL_CONTROL || slot >= MP_MAX_PLAYERS)
        return FALSE;
    switch (type)
    {
    case MP_MSG_LINK_JOIN:
    case MP_MSG_LINK_LEAVE:
    case MP_MSG_LINK_OPEN:
    case MP_MSG_LINK_CLOSE:
    case MP_MSG_LINK_START:
        if (MpProto_ParseLinkCtl(type, payload, len, &ctl) != MP_PARSE_OK)
            return FALSE;
        MpLinkHostHandleCtl(slot, type, &ctl);
        return TRUE;
    case MP_MSG_LINK_CMD:
        if (MpProto_ParseLinkCmd(payload, len, &cmd) != MP_PARSE_OK)
            return FALSE;
        MpLinkHostHandleCmd(slot, &cmd);
        return TRUE;
    default:
        return FALSE; // host -> client types, or unassigned LINK_* numbers
    }
}

// ===========================================================================
// Member
// ===========================================================================

static void MpLinkMemberClearRx(struct MpLinkMember *m)
{
    m->rxHead = 0;
    m->rxCount = 0;
}

// The group or request is over. If the game's cable is open, the game sees a hardware error.
static void MpLinkMemberEnd(u8 reason)
{
    struct MpLinkMember *m = MpLinkMemberState();

    if (m->state == MP_LINK_STATE_NONE)
        return;
    MP_SLOG("link %s: %s\n", (m->state == MP_LINK_STATE_GROUPED) ? "group ended" : "request ended", MpLinkEndName(reason));
    if (m->phase != MP_LINK_PHASE_CLOSED)
        m->lost = TRUE;
    m->state = MP_LINK_STATE_NONE;
    m->endReason = reason;
    m->requestMask = 0;
    m->groupId = 0;
    m->memberMask = 0;
    m->openMask = 0;
}

static void MpLinkMemberHandleCtl(u8 type, const struct MpMsgLinkCtl *ctl)
{
    struct MpLinkMember *m = MpLinkMemberState();

    switch (type)
    {
    case MP_MSG_LINK_GROUP:
        // A group from an older request (we asked again since) is ignored; the host ends it
        // when our LINK_LEAVE arrives.
        if (m->state != MP_LINK_STATE_REQUESTED || ctl->arg1 != m->requestMask
         || ctl->arg2 != MpLinkRankOf(ctl->arg1, gMpSession->localSlot))
            break;
        m->state = MP_LINK_STATE_GROUPED;
        m->groupId = ctl->groupId;
        m->memberMask = ctl->arg1;
        m->linkId = ctl->arg2;
        m->count = MpProto_LinkMaskCount(ctl->arg1);
        m->openMask = 0;
        m->endReason = MP_LINK_END_NONE;
        MP_SLOG("link group %u joined as link id %u of %u\n", (unsigned)m->groupId, (unsigned)m->linkId, (unsigned)m->count);
        break;
    case MP_MSG_LINK_END:
        if ((ctl->groupId == 0 && m->state == MP_LINK_STATE_REQUESTED)
         || (ctl->groupId != 0 && m->state == MP_LINK_STATE_GROUPED && ctl->groupId == m->groupId))
            MpLinkMemberEnd(ctl->arg1);
        break;
    case MP_MSG_LINK_STATUS:
        if (m->state == MP_LINK_STATE_GROUPED && ctl->groupId == m->groupId)
            m->openMask = ctl->arg1 & (u8)((1 << m->count) - 1);
        break;
    case MP_MSG_LINK_BUS_START:
        if (m->state != MP_LINK_STATE_GROUPED || ctl->groupId != m->groupId
         || m->phase != MP_LINK_PHASE_OPEN || ctl->arg2 != m->openSeq)
            break; // for an earlier open of ours
        m->phase = MP_LINK_PHASE_BUS;
        m->busEpoch = ctl->arg1;
        m->busEnded = FALSE;
        m->outstanding = 0;
        MpLinkMemberClearRx(m);
        MP_SDLOG("link: bus %u up\n", (unsigned)m->busEpoch);
        break;
    case MP_MSG_LINK_BUS_END:
        if (m->state != MP_LINK_STATE_GROUPED || ctl->groupId != m->groupId
         || m->phase != MP_LINK_PHASE_BUS || ctl->arg1 != m->busEpoch || m->busEnded)
            break;
        m->busEnded = TRUE;
        m->outstanding = 0;
        if (m->rxCount < MP_LINK_RX_QUEUE_SIZE)
        {
            struct MpLinkRxItem *item = &m->rx[(m->rxHead + m->rxCount) % MP_LINK_RX_QUEUE_SIZE];

            item->type = MP_LINK_RX_BUS_END;
            m->rxCount++;
        }
        else
        {
            m->lost = TRUE;
        }
        break;
    }
}

static void MpLinkMemberHandleRow(const struct MpMsgLinkRow *row)
{
    struct MpLinkMember *m = MpLinkMemberState();
    struct MpLinkRxItem *item;
    u32 i;
    u16 own = 0;

    if (m->state != MP_LINK_STATE_GROUPED || row->groupId != m->groupId || m->phase != MP_LINK_PHASE_BUS
     || row->busEpoch != m->busEpoch || m->busEnded || row->count != m->count)
        return; // stale (an older bus or group)
    if (m->rxCount >= MP_LINK_RX_QUEUE_SIZE)
    {
        MP_SLOG("link: receive queue overflow (%u rows)\n", (unsigned)m->rxCount);
        if (gMpSession->state == MP_STATE_IN_SESSION)
            MpLinkMemberSendCtl(MP_MSG_LINK_LEAVE, 0, 0, 0); // the others get LINK_END(LEFT)
        MpLinkMemberEnd(MP_LINK_END_RX_OVERFLOW);
        return;
    }
    item = &m->rx[(m->rxHead + m->rxCount) % MP_LINK_RX_QUEUE_SIZE];
    item->type = MP_LINK_RX_ROW;
    memset(item->cmds, 0, sizeof(item->cmds));
    for (i = 0; i < row->count && i < MP_LINK_MAX_MEMBERS; i++)
        memcpy(item->cmds[i], row->cmds[i], sizeof(item->cmds[i]));
    m->rxCount++;
    m->rowsReceived++;
    if (m->rxCount > m->rxPeak)
        m->rxPeak = m->rxCount;

    for (i = 0; i < MP_LINK_CMD_WORDS; i++)
        own |= row->cmds[m->linkId][i];
    if (own != 0 && m->outstanding > 0)
    {
        m->outstanding--;
        m->lastProgressMs = MpTransport_GetTimeMs();
    }
}

bool8 MpLink_OnClientReceive(u8 channel, u8 type, const u8 *payload, u32 len)
{
    struct MpMsgLinkCtl ctl;
    struct MpMsgLinkRow row;

    if (channel != MP_CHANNEL_CONTROL)
        return FALSE;
    switch (type)
    {
    case MP_MSG_LINK_GROUP:
    case MP_MSG_LINK_END:
    case MP_MSG_LINK_STATUS:
    case MP_MSG_LINK_BUS_START:
    case MP_MSG_LINK_BUS_END:
        if (MpProto_ParseLinkCtl(type, payload, len, &ctl) != MP_PARSE_OK)
            return FALSE;
        MpLinkMemberHandleCtl(type, &ctl);
        return TRUE;
    case MP_MSG_LINK_ROW:
        if (MpProto_ParseLinkRow(payload, len, &row) != MP_PARSE_OK)
            return FALSE;
        MpLinkMemberHandleRow(&row);
        return TRUE;
    default:
        return FALSE;
    }
}

// ===========================================================================
// Lifecycle
// ===========================================================================

void MpLink_ResetInstance(u8 instance)
{
    if (instance >= MP_SESSION_INSTANCE_COUNT)
        return;
    memset(&sLinkHost[instance], 0, sizeof(sLinkHost[instance]));
    memset(&sLinkMember[instance], 0, sizeof(sLinkMember[instance]));
}

void MpLink_OnSessionStart(void)
{
    struct MpLinkMember *m = MpLinkMemberState();

    memset(MpLinkHostState(), 0, sizeof(struct MpLinkHost));
    // The member keeps its open/lost state: the game's cable may still be open from before.
    if (m->state != MP_LINK_STATE_NONE)
        MpLinkMemberEnd(MP_LINK_END_SESSION);
}

void MpLink_OnSessionEnd(void)
{
    memset(MpLinkHostState(), 0, sizeof(struct MpLinkHost));
    MpLinkMemberEnd(MP_LINK_END_SESSION);
}

// ===========================================================================
// Public API (mp.h)
// ===========================================================================

bool8 MP_LinkJoin(u8 slotMask)
{
    struct MpLinkMember *m = MpLinkMemberState();
    struct MpSession *s = gMpSession;
    u32 i;

    if (s->state != MP_STATE_IN_SESSION || s->localSlot >= MP_MAX_PLAYERS)
        return FALSE;
    if ((slotMask & ~0x0F) != 0 || !(slotMask & (1 << s->localSlot)) || MpProto_LinkMaskCount(slotMask) < 2)
        return FALSE;
    for (i = 0; i < MP_MAX_PLAYERS; i++)
    {
        if ((slotMask & (1 << i)) && !s->slotUsed[i])
            return FALSE;
    }
    if (m->state != MP_LINK_STATE_NONE)
        MP_LinkLeave();
    m->state = MP_LINK_STATE_REQUESTED;
    m->requestMask = slotMask;
    m->endReason = MP_LINK_END_NONE;
    MP_SLOG("link: asking for a group with slot mask 0x%X\n", (unsigned)slotMask);
    MpLinkMemberSendCtl(MP_MSG_LINK_JOIN, 0, slotMask, 0);
    return TRUE;
}

void MP_LinkLeave(void)
{
    struct MpLinkMember *m = MpLinkMemberState();

    if (m->state == MP_LINK_STATE_NONE)
        return;
    if (gMpSession->state == MP_STATE_IN_SESSION)
        MpLinkMemberSendCtl(MP_MSG_LINK_LEAVE, 0, 0, 0);
    MpLinkMemberEnd(MP_LINK_END_LEFT);
}

u8 MP_LinkGetState(void)
{
    return MpLinkMemberState()->state;
}

// PS-048: while this instance is in (or asking for) a link group, the game's frame-counted link
// timeouts (300 / 600 frames) run on every member, so the PC may not pause or speed up.
bool8 MP_LinkTimeControlsLocked(void)
{
    return MpLinkMemberState()->state != MP_LINK_STATE_NONE;
}

u8 MP_LinkGetLinkId(void)
{
    struct MpLinkMember *m = MpLinkMemberState();

    return (m->state == MP_LINK_STATE_GROUPED) ? m->linkId : MP_SLOT_NONE;
}

u8 MP_LinkGetEndReason(void)
{
    return MpLinkMemberState()->endReason;
}

// ===========================================================================
// Cable interface (mp_link_cable.c)
// ===========================================================================

void MpLink_CableGetInfo(struct MpLinkCableInfo *out)
{
    struct MpLinkMember *m = MpLinkMemberState();

    if (m->phase == MP_LINK_PHASE_BUS && m->outstanding > 0 && !m->lost
     && (u32)(MpTransport_GetTimeMs() - m->lastProgressMs) >= MP_LINK_WATCHDOG_MS)
    {
        MP_SLOG("link: watchdog: %u commands not echoed for %u ms\n", (unsigned)m->outstanding, (unsigned)MP_LINK_WATCHDOG_MS);
        if (m->state != MP_LINK_STATE_NONE && gMpSession->state == MP_STATE_IN_SESSION)
            MpLinkMemberSendCtl(MP_MSG_LINK_LEAVE, 0, 0, 0);
        MpLinkMemberEnd(MP_LINK_END_WATCHDOG);
        m->lost = TRUE;
    }
    memset(out, 0, sizeof(*out));
    out->grouped = (m->state == MP_LINK_STATE_GROUPED);
    out->open = (m->phase != MP_LINK_PHASE_CLOSED);
    out->busUp = (m->phase == MP_LINK_PHASE_BUS);
    out->lost = m->lost;
    out->linkId = m->linkId;
    out->count = m->count;
    out->openCount = MpProto_LinkMaskCount(m->openMask);
    out->queued = m->rxCount;
}

void MpLink_CableOpen(void)
{
    struct MpLinkMember *m = MpLinkMemberState();

    if (m->state != MP_LINK_STATE_GROUPED || m->phase != MP_LINK_PHASE_CLOSED)
        return;
    m->openSeq++;
    m->phase = MP_LINK_PHASE_OPEN; // before sending: the host's reply may be a direct call
    m->startSent = FALSE;
    m->busEnded = FALSE;
    m->lost = FALSE;
    MpLinkMemberClearRx(m);
    MpLinkMemberSendCtl(MP_MSG_LINK_OPEN, m->groupId, m->openSeq, 0);
}

void MpLink_CableClose(void)
{
    struct MpLinkMember *m = MpLinkMemberState();
    bool8 wasOpen = (m->phase != MP_LINK_PHASE_CLOSED);

    m->phase = MP_LINK_PHASE_CLOSED;
    m->startSent = FALSE;
    m->busEnded = FALSE;
    m->lost = FALSE;
    m->outstanding = 0;
    MpLinkMemberClearRx(m);
    if (wasOpen && m->state == MP_LINK_STATE_GROUPED && gMpSession->state == MP_STATE_IN_SESSION)
        MpLinkMemberSendCtl(MP_MSG_LINK_CLOSE, m->groupId, m->openSeq, 0);
}

void MpLink_CableRequestStart(void)
{
    struct MpLinkMember *m = MpLinkMemberState();

    if (m->state != MP_LINK_STATE_GROUPED || m->phase != MP_LINK_PHASE_OPEN || m->linkId != 0 || m->startSent)
        return;
    m->startSent = TRUE;
    MpLinkMemberSendCtl(MP_MSG_LINK_START, m->groupId, m->openSeq, 0);
}

void MpLink_CableSend(const u16 *cmd)
{
    struct MpLinkMember *m = MpLinkMemberState();
    struct MpMsgLinkCmd msg;
    u16 len;

    if (m->state != MP_LINK_STATE_GROUPED || m->phase != MP_LINK_PHASE_BUS || m->busEnded)
        return;
    msg.groupId = m->groupId;
    msg.busEpoch = m->busEpoch;
    memcpy(msg.cmd, cmd, sizeof(msg.cmd));
    if (m->outstanding == 0)
        m->lastProgressMs = MpTransport_GetTimeMs();
    m->outstanding++;
    m->cmdsSent++;
    if (gMpSession->isHost)
    {
        MpLinkHostHandleCmd(gMpSession->localSlot, &msg);
        return;
    }
    len = MpProto_WriteLinkCmd(sLinkBuf, sizeof(sLinkBuf), &msg);
    MpSession_SendTo(0, sLinkBuf, len, MP_CHANNEL_CONTROL, TRUE);
}

u8 MpLink_CablePop(u16 (*cmds)[MP_LINK_CMD_WORDS])
{
    struct MpLinkMember *m = MpLinkMemberState();
    struct MpLinkRxItem *item;

    if (m->rxCount == 0)
        return MP_LINK_POP_NONE;
    item = &m->rx[m->rxHead];
    m->rxHead = (u16)((m->rxHead + 1) % MP_LINK_RX_QUEUE_SIZE);
    m->rxCount--;
    if (item->type == MP_LINK_RX_BUS_END)
        return MP_LINK_POP_BUS_END;
    memcpy(cmds, item->cmds, sizeof(item->cmds));
    return MP_LINK_POP_ROW;
}

void MpLink_GetStats(struct MpLinkStats *out)
{
    struct MpLinkMember *m = MpLinkMemberState();
    struct MpLinkHost *h = MpLinkHostState();

    memset(out, 0, sizeof(*out));
    out->cmdsSent = m->cmdsSent;
    out->rowsReceived = m->rowsReceived;
    out->rxPeak = m->rxPeak;
    out->rowsEmitted = h->rowsEmitted;
    out->groupsFormed = h->groupsFormed;
    out->cmdsRefused = h->cmdsRefused;
}
