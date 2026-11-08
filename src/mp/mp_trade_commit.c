// Host-arbitrated commit of a direct trade's save (PS-044, protocol.md 6.15). No game state here.
//
// Director ruling on PS-044: the session host confirms that both trade partners reached "ready
// to commit" before either signs its save. Each member sends READY(groupId, seq) once its save
// is written except for the last sector's signature (CB2_SaveAndEndTrade). The host answers:
//   - COMMIT to both, the moment it holds READY from both members of the 2-member link group;
//   - ABORT if the group ends first (a member left the link or the session), if the round is not
//     complete within MP_TRADE_COMMIT_TIMEOUT_MS, or if the READY does not match the group.
// Every decision is final: it is recorded per slot and repeated if the same READY comes again,
// and a READY that arrives after its round was aborted gets ABORT. A member whose own session
// ends before a decision reaches it presumes ABORT (MP_TRADE_ABORT_IN_DOUBT).
//
// When the host is one of the partners, its own decision is delivered only after the COMMIT to
// the other member has been handed to the transport and flushed (MpTransport_Flush), so the
// host never signs before the client's COMMIT is on the wire.

#include <string.h>
#include "mp/mp_link.h"
#include "mp/mp_protocol.h"
#include "mp/mp_session_internal.h"
#include "mp/mp_trade_commit.h"
#include "mp/mp_trade_journal.h"
#include "mp/mp_transport.h"

struct MpTradeCommitMember
{
    u8 state;           // MP_TRADE_COMMIT_*
    u8 groupId;         // group of the current / last round
    u8 seq;             // number of the current / last round in that group
    u8 reason;          // MP_TRADE_ABORT_* for ABORT
};

struct MpTradeCommitDeferred
{
    bool8 valid;
    u8 op;
    u8 groupId;
    u8 seq;
    u8 reason;
};

static struct MpTradeCommitHost sTcHost[MP_SESSION_INSTANCE_COUNT];
static struct MpTradeCommitMember sTcMember[MP_SESSION_INSTANCE_COUNT];
static struct MpTradeCommitDeferred sTcDeferred;   // the host's own decision, delivered after the flush
static u8 sTcBuf[MP_MAX_PACKET_SIZE];

struct MpTrQueue   // PS-050: received TRADE_RECOVER, polled by the game side
{
    u8 head;
    u8 count;
    struct MpMsgTradeRecover items[MP_TRADE_RECOVER_QUEUE];
};

static struct MpTrQueue sTrQueue[MP_SESSION_INSTANCE_COUNT];

static struct MpTrQueue *MpTrQueueState(void)
{
    return &sTrQueue[gMpSession->instance < MP_SESSION_INSTANCE_COUNT ? gMpSession->instance : 0];
}

static struct MpTradeCommitHost *MpTcHostState(void)
{
    return &sTcHost[gMpSession->instance < MP_SESSION_INSTANCE_COUNT ? gMpSession->instance : 0];
}

static struct MpTradeCommitMember *MpTcMemberState(void)
{
    return &sTcMember[gMpSession->instance < MP_SESSION_INSTANCE_COUNT ? gMpSession->instance : 0];
}

static const char *MpTcOpName(u8 op)
{
    switch (op)
    {
    case MP_TRADE_OP_READY:
        return "READY";
    case MP_TRADE_OP_COMMIT:
        return "COMMIT";
    case MP_TRADE_OP_ABORT:
        return "ABORT";
    default:
        return "?";
    }
}

static const char *MpTcReasonName(u8 reason)
{
    switch (reason)
    {
    case MP_TRADE_ABORT_NONE:
        return "none";
    case MP_TRADE_ABORT_GROUP_ENDED:
        return "link group ended";
    case MP_TRADE_ABORT_TIMEOUT:
        return "partner not ready in time";
    case MP_TRADE_ABORT_MISMATCH:
        return "round mismatch";
    case MP_TRADE_ABORT_NOT_GROUPED:
        return "not in a link group";
    case MP_TRADE_ABORT_IN_DOUBT:
        return "session ended before the decision (presumed abort)";
    default:
        return "?";
    }
}

// ===========================================================================
// Pure decision logic
// ===========================================================================

static void MpTcRemember(struct MpTradeCommitHost *h, u32 tradeId, u8 op)
{
    u32 i;

    if (tradeId == 0)
        return;
    for (i = 0; i < MP_TRADE_DECISION_RING; i++)
    {
        if (h->ring[i].tradeId == tradeId)
        {
            h->ring[i].op = op; // the latest round of an identical trade wins
            return;
        }
    }
    h->ring[h->ringNext % MP_TRADE_DECISION_RING].tradeId = tradeId;
    h->ring[h->ringNext % MP_TRADE_DECISION_RING].op = op;
    h->ringNext = (u8)((h->ringNext + 1) % MP_TRADE_DECISION_RING);
}

u8 MpTradeCommit_HostLookup(const struct MpTradeCommitHost *h, u32 tradeId)
{
    u32 i;

    if (tradeId == 0)
        return MP_TJ_STATUS_NONE;
    for (i = 0; i < MP_TRADE_DECISION_RING; i++)
    {
        if (h->ring[i].tradeId == tradeId)
            return (h->ring[i].op == MP_TRADE_OP_COMMIT) ? MP_TJ_STATUS_COMMITTED : MP_TJ_STATUS_ABORTED;
    }
    return MP_TJ_STATUS_NONE;
}

static void MpTcDecide(struct MpTradeCommitHost *h, u8 slot, u8 op, u8 reason, MpTradeCommitSendFunc send)
{
    struct MpTradeCommitRec *r = &h->rec[slot];

    MpTcRemember(h, r->tradeId, op);

    r->state = (op == MP_TRADE_OP_COMMIT) ? MP_TRADE_REC_COMMIT : MP_TRADE_REC_ABORT;
    r->reason = (op == MP_TRADE_OP_COMMIT) ? MP_TRADE_ABORT_NONE : reason;
    if (op == MP_TRADE_OP_COMMIT)
        h->commits++;
    else
        h->aborts++;
    send(slot, op, r->groupId, r->seq, r->reason);
}

// Marks the partner's side of an aborted round, so its READY for that round (if it ever comes)
// gets the same ABORT. Never overwrites a decision for that very round.
static void MpTcMarkPartnerAborted(struct MpTradeCommitHost *h, const struct MpTradeCommitRec *r, u8 slot)
{
    struct MpTradeCommitRec *q;

    if (r->partner >= MP_MAX_PLAYERS)
        return;
    q = &h->rec[r->partner];
    if (q->state == MP_TRADE_REC_READY)
        return; // it waits for a round of its own, which ends by its own group end / timeout
    if (q->groupId == r->groupId && q->seq == r->seq && q->state != MP_TRADE_REC_NONE)
        return;
    q->state = MP_TRADE_REC_ABORT;
    q->groupId = r->groupId;
    q->seq = r->seq;
    q->partner = slot;
    q->reason = r->reason;
    q->startMs = 0;
}

void MpTradeCommit_HostReady(struct MpTradeCommitHost *h, u8 slot, u8 groupId, u8 seq, u32 tradeId, bool8 groupOk, u8 partner, u32 now, MpTradeCommitSendFunc send)
{
    struct MpTradeCommitRec *r;
    struct MpTradeCommitRec *q;

    if (slot >= MP_MAX_PLAYERS || groupId == 0 || seq == 0)
        return;
    r = &h->rec[slot];

    // Already decided: say it again (a decision is final).
    if (r->groupId == groupId && r->seq == seq
     && (r->state == MP_TRADE_REC_COMMIT || r->state == MP_TRADE_REC_ABORT))
    {
        send(slot, (r->state == MP_TRADE_REC_COMMIT) ? MP_TRADE_OP_COMMIT : MP_TRADE_OP_ABORT, groupId, seq, r->reason);
        return;
    }

    r->state = MP_TRADE_REC_READY;
    r->groupId = groupId;
    r->seq = seq;
    r->partner = partner;
    r->reason = MP_TRADE_ABORT_NONE;
    r->startMs = now;
    r->tradeId = tradeId;

    if (!groupOk || partner >= MP_MAX_PLAYERS || partner == slot)
    {
        r->partner = MP_SLOT_NONE;
        r->tradeId = 0; // PS-050: a READY outside a real group never enters the decision ring
        MpTcDecide(h, slot, MP_TRADE_OP_ABORT, MP_TRADE_ABORT_MISMATCH, send);
        return;
    }

    q = &h->rec[partner];
    if (q->groupId != groupId || q->partner != slot)
        return; // the partner has not reached this round yet: wait
    if (q->seq == seq && q->state == MP_TRADE_REC_READY && q->tradeId != tradeId)
    {
        // PS-050: same round, but the two journals describe different trades.
        MpTcDecide(h, partner, MP_TRADE_OP_ABORT, MP_TRADE_ABORT_MISMATCH, send);
        MpTcDecide(h, slot, MP_TRADE_OP_ABORT, MP_TRADE_ABORT_MISMATCH, send);
        return;
    }
    if (q->seq == seq)
    {
        switch (q->state)
        {
        case MP_TRADE_REC_READY:
            // Both ready: the one decision that writes. Partner first, then the sender.
            q->state = MP_TRADE_REC_COMMIT;
            q->reason = MP_TRADE_ABORT_NONE;
            MpTcRemember(h, tradeId, MP_TRADE_OP_COMMIT);
            send(partner, MP_TRADE_OP_COMMIT, groupId, seq, 0);
            MpTcDecide(h, slot, MP_TRADE_OP_COMMIT, 0, send); // counts the round once
            break;
        case MP_TRADE_REC_ABORT:
            MpTcDecide(h, slot, MP_TRADE_OP_ABORT, q->reason, send);
            break;
        case MP_TRADE_REC_COMMIT:
            // Cannot happen (COMMIT needs this slot's READY), but if it did the partner signs:
            // so must we, or a Pokemon would be lost.
            MpTcDecide(h, slot, MP_TRADE_OP_COMMIT, 0, send);
            break;
        }
    }
    else if (q->state == MP_TRADE_REC_READY)
    {
        // Both wait, but for different rounds: they disagree on how many trades happened.
        MpTcDecide(h, partner, MP_TRADE_OP_ABORT, MP_TRADE_ABORT_MISMATCH, send);
        MpTcDecide(h, slot, MP_TRADE_OP_ABORT, MP_TRADE_ABORT_MISMATCH, send);
    }
}

void MpTradeCommit_HostGroupEnded(struct MpTradeCommitHost *h, u8 memberMask, MpTradeCommitSendFunc send)
{
    u8 s;

    for (s = 0; s < MP_MAX_PLAYERS; s++)
    {
        if (!(memberMask & (1 << s)) || h->rec[s].state != MP_TRADE_REC_READY)
            continue;
        MpTcDecide(h, s, MP_TRADE_OP_ABORT, MP_TRADE_ABORT_GROUP_ENDED, send);
        MpTcMarkPartnerAborted(h, &h->rec[s], s);
    }
}

void MpTradeCommit_HostTick(struct MpTradeCommitHost *h, u32 now, MpTradeCommitSendFunc send)
{
    u8 s;

    for (s = 0; s < MP_MAX_PLAYERS; s++)
    {
        if (h->rec[s].state != MP_TRADE_REC_READY || (u32)(now - h->rec[s].startMs) < MP_TRADE_COMMIT_TIMEOUT_MS)
            continue;
        MpTcDecide(h, s, MP_TRADE_OP_ABORT, MP_TRADE_ABORT_TIMEOUT, send);
        MpTcMarkPartnerAborted(h, &h->rec[s], s);
    }
}

void MpTradeCommit_HostForgetSlot(struct MpTradeCommitHost *h, u8 slot)
{
    if (slot < MP_MAX_PLAYERS)
        memset(&h->rec[slot], 0, sizeof(h->rec[slot]));
}

// ===========================================================================
// Session wrapper
// ===========================================================================

static void MpTcMemberDeliver(u8 op, u8 groupId, u8 seq, u8 reason)
{
    struct MpTradeCommitMember *m = MpTcMemberState();

    if (m->state != MP_TRADE_COMMIT_WAITING || m->groupId != groupId || m->seq != seq)
    {
        MP_SDLOG("trade commit: ignoring %s for group %u round %u (not waiting for it)\n", MpTcOpName(op), (unsigned)groupId, (unsigned)seq);
        return;
    }
    m->state = (op == MP_TRADE_OP_COMMIT) ? MP_TRADE_COMMIT_COMMIT : MP_TRADE_COMMIT_ABORT;
    m->reason = (op == MP_TRADE_OP_COMMIT) ? MP_TRADE_ABORT_NONE : reason;
    if (op == MP_TRADE_OP_COMMIT)
        MP_SLOG("trade commit: group %u round %u: COMMIT\n", (unsigned)groupId, (unsigned)seq);
    else
        MP_SLOG("trade commit: group %u round %u: ABORT (%s)\n", (unsigned)groupId, (unsigned)seq, MpTcReasonName(reason));
}

// Host -> `slot`. The host's own slot is deferred until the network copies are flushed.
static void MpTcHostSend(u8 slot, u8 op, u8 groupId, u8 seq, u8 reason)
{
    struct MpMsgTradeCommit msg;
    u16 len;

    if (slot >= MP_MAX_PLAYERS || !gMpSession->slotUsed[slot])
        return;
    if (slot == gMpSession->localSlot)
    {
        sTcDeferred.valid = TRUE;
        sTcDeferred.op = op;
        sTcDeferred.groupId = groupId;
        sTcDeferred.seq = seq;
        sTcDeferred.reason = reason;
        return;
    }
    msg.op = op;
    msg.groupId = groupId;
    msg.seq = seq;
    msg.reason = reason;
    msg.tradeId = 0; // members match decisions by (groupId, seq)
    len = MpProto_WriteTradeCommit(sTcBuf, sizeof(sTcBuf), &msg);
    if (len != 0 && gMpSession->slotPeer[slot] != MP_PEER_NONE)
        MpSession_SendTo(gMpSession->slotPeer[slot], sTcBuf, len, MP_CHANNEL_CONTROL, TRUE);
    MP_SLOG("trade commit: group %u round %u: %s to slot %u\n", (unsigned)groupId, (unsigned)seq, MpTcOpName(op), (unsigned)slot);
}

static void MpTcHostFlushDeferred(void)
{
    struct MpTradeCommitDeferred d;

    if (!sTcDeferred.valid)
        return;
    MpTransport_Flush(); // the other member's decision is on the wire before ours is acted on
    d = sTcDeferred;
    sTcDeferred.valid = FALSE;
    MpTcMemberDeliver(d.op, d.groupId, d.seq, d.reason);
}

static void MpTcHostHandleReady(u8 slot, u8 groupId, u8 seq, u32 tradeId)
{
    u8 gid = 0, mask = 0, partner = MP_SLOT_NONE;
    bool8 groupOk = FALSE;
    u8 i;

    if (MpLink_HostGetGroup(slot, &gid, &mask) && gid == groupId && MpProto_LinkMaskCount(mask) == 2)
    {
        for (i = 0; i < MP_MAX_PLAYERS; i++)
        {
            if ((mask & (1 << i)) && i != slot)
                partner = i;
        }
        groupOk = (partner != MP_SLOT_NONE);
    }
    MP_SLOG("trade commit: READY from slot %u (group %u round %u)%s\n", (unsigned)slot, (unsigned)groupId, (unsigned)seq, groupOk ? "" : " - no such 2-member group");
    MpTradeCommit_HostReady(MpTcHostState(), slot, groupId, seq, tradeId, groupOk, partner, MpTransport_GetTimeMs(), MpTcHostSend);
    MpTcHostFlushDeferred();
}

void MpTradeCommit_ResetInstance(u8 instance)
{
    if (instance >= MP_SESSION_INSTANCE_COUNT)
        return;
    memset(&sTcHost[instance], 0, sizeof(sTcHost[instance]));
    memset(&sTcMember[instance], 0, sizeof(sTcMember[instance]));
    memset(&sTrQueue[instance], 0, sizeof(sTrQueue[instance]));
    sTcDeferred.valid = FALSE;
}

static void MpTcMemberSessionGone(void)
{
    struct MpTradeCommitMember *m = MpTcMemberState();

    if (m->state == MP_TRADE_COMMIT_WAITING)
    {
        m->state = MP_TRADE_COMMIT_ABORT;
        m->reason = MP_TRADE_ABORT_IN_DOUBT;
        MP_SLOG("trade commit: group %u round %u: ABORT (%s)\n", (unsigned)m->groupId, (unsigned)m->seq, MpTcReasonName(m->reason));
    }
}

void MpTradeCommit_OnSessionStart(void)
{
    struct MpTradeCommitMember *m = MpTcMemberState();

    memset(MpTcHostState(), 0, sizeof(struct MpTradeCommitHost));
    MpTcMemberSessionGone();
    m->groupId = 0; // group ids restart with the new host table
    m->seq = 0;
}

void MpTradeCommit_OnSessionEnd(void)
{
    memset(MpTrQueueState(), 0, sizeof(struct MpTrQueue));
    memset(MpTcHostState(), 0, sizeof(struct MpTradeCommitHost));
    MpTcMemberSessionGone();
}

void MpTradeCommit_HostOnGroupEnded(u8 memberMask)
{
    if (!gMpSession->isHost)
        return;
    MpTradeCommit_HostGroupEnded(MpTcHostState(), memberMask, MpTcHostSend);
    MpTcHostFlushDeferred();
}

void MpTradeCommit_HostOnSlotLeft(u8 slot)
{
    MpTradeCommit_HostForgetSlot(MpTcHostState(), slot);
}

void MpTradeCommit_HostUpdate(void)
{
    MpTradeCommit_HostTick(MpTcHostState(), MpTransport_GetTimeMs(), MpTcHostSend);
    MpTcHostFlushDeferred();
}

bool8 MpTradeCommit_OnHostReceive(u8 slot, const u8 *payload, u32 len)
{
    struct MpMsgTradeCommit msg;

    if (slot >= MP_MAX_PLAYERS || MpProto_ParseTradeCommit(payload, len, &msg) != MP_PARSE_OK || msg.op != MP_TRADE_OP_READY)
        return FALSE;
    MpTcHostHandleReady(slot, msg.groupId, msg.seq, msg.tradeId);
    return TRUE;
}

bool8 MpTradeCommit_OnClientReceive(const u8 *payload, u32 len)
{
    struct MpMsgTradeCommit msg;

    if (MpProto_ParseTradeCommit(payload, len, &msg) != MP_PARSE_OK || msg.op == MP_TRADE_OP_READY)
        return FALSE;
    MpTcMemberDeliver(msg.op, msg.groupId, msg.seq, msg.reason);
    return TRUE;
}

const struct MpTradeCommitHost *MpTradeCommit_DebugHost(void)
{
    return MpTcHostState();
}

// ===========================================================================
// Public API (mp.h)
// ===========================================================================

bool8 MP_TradeCommitReady(u32 tradeId)
{
    struct MpTradeCommitMember *m = MpTcMemberState();
    struct MpMsgTradeCommit msg;
    u8 groupId = (gMpSession->state == MP_STATE_IN_SESSION) ? MpLink_MemberGroupId() : 0;
    u16 len;

    if (groupId == 0 || tradeId == 0)
    {
        m->state = MP_TRADE_COMMIT_ABORT;
        m->reason = MP_TRADE_ABORT_NOT_GROUPED;
        MP_SLOG("trade commit: not ready: %s\n", MpTcReasonName(m->reason));
        return FALSE;
    }
    if (groupId != m->groupId)
    {
        m->groupId = groupId;
        m->seq = 0;
    }
    if (++m->seq == 0)
        m->seq = 1;
    m->state = MP_TRADE_COMMIT_WAITING;
    m->reason = MP_TRADE_ABORT_NONE;
    MP_SLOG("trade commit: group %u round %u: READY\n", (unsigned)groupId, (unsigned)m->seq);
    if (gMpSession->isHost)
    {
        MpTcHostHandleReady(gMpSession->localSlot, groupId, m->seq, tradeId);
        return TRUE;
    }
    msg.op = MP_TRADE_OP_READY;
    msg.groupId = groupId;
    msg.seq = m->seq;
    msg.reason = 0;
    msg.tradeId = tradeId;
    len = MpProto_WriteTradeCommit(sTcBuf, sizeof(sTcBuf), &msg);
    if (len != 0)
    {
        MpSession_SendTo(0, sTcBuf, len, MP_CHANNEL_CONTROL, TRUE);
        MpTransport_Flush();
    }
    return TRUE;
}

u8 MP_TradeCommitPoll(void)
{
    if (gMpSession->state != MP_STATE_IN_SESSION)
        MpTcMemberSessionGone();
    return MpTcMemberState()->state;
}

u8 MP_TradeCommitGetAbortReason(void)
{
    return MpTcMemberState()->reason;
}

void MP_TradeCommitReset(void)
{
    struct MpTradeCommitMember *m = MpTcMemberState();

    m->state = MP_TRADE_COMMIT_IDLE;
    m->reason = MP_TRADE_ABORT_NONE;
}

// ===========================================================================
// TRADE_RECOVER (PS-050, protocol.md 6.16): journal recovery between the partners
// ===========================================================================
//
// The host routes each message to its target slot (rewriting `slot` to the sender) and, if it
// still remembers its decision for that trade id, first sends that decision (slot NONE) to the
// sender and to the target. Every recipient therefore learns the host's decision before any
// partner status about that trade, which keeps the "both READY -> neither signed" rule safe.
// The host checks that the sender's trainer id is the one in its session table and that the
// target's is the receiver's; the game side checks the rest (MpTradeJournal_MatchesMsg).

static void MpTrQueuePush(const struct MpMsgTradeRecover *m)
{
    struct MpTrQueue *q = MpTrQueueState();

    if (q->count >= MP_TRADE_RECOVER_QUEUE)
    {
        q->head = (u8)((q->head + 1) % MP_TRADE_RECOVER_QUEUE); // drop the oldest; it is resent
        q->count--;
    }
    q->items[(q->head + q->count) % MP_TRADE_RECOVER_QUEUE] = *m;
    q->count++;
}

static void MpTrHostSendTo(u8 slot, const struct MpMsgTradeRecover *m)
{
    u16 len;

    if (slot >= MP_MAX_PLAYERS || !gMpSession->slotUsed[slot])
        return;
    if (slot == gMpSession->localSlot)
    {
        MpTrQueuePush(m);
        return;
    }
    len = MpProto_WriteTradeRecover(sTcBuf, sizeof(sTcBuf), m);
    if (len != 0 && gMpSession->slotPeer[slot] != MP_PEER_NONE)
        MpSession_SendTo(gMpSession->slotPeer[slot], sTcBuf, len, MP_CHANNEL_CONTROL, TRUE);
}

// The same trade seen from the receiver: sender and receiver swapped.
static void MpTrMirror(const struct MpMsgTradeRecover *m, struct MpMsgTradeRecover *out)
{
    *out = *m;
    out->senderTid = m->receiverTid;
    out->receiverTid = m->senderTid;
    out->senderGivenPersonality = m->senderRecvPersonality;
    out->senderGivenOtId = m->senderRecvOtId;
    out->senderRecvPersonality = m->senderGivenPersonality;
    out->senderRecvOtId = m->senderGivenOtId;
}

static void MpTrHostRoute(u8 from, const struct MpMsgTradeRecover *m)
{
    struct MpMsgTradeRecover out;
    u8 target = m->slot;
    bool8 targetOk;
    u8 decision;

    if (from >= MP_MAX_PLAYERS || !gMpSession->slotUsed[from] || gMpSession->players[from].summary.trainerId != m->senderTid)
    {
        MP_SDLOG("trade recover: dropping a message whose trainer id is not its sender's\n");
        return;
    }
    targetOk = target < MP_MAX_PLAYERS && target != from && gMpSession->slotUsed[target]
            && gMpSession->players[target].summary.trainerId == m->receiverTid;
    decision = MpTradeCommit_HostLookup(MpTcHostState(), m->tradeId);
    if (decision != MP_TJ_STATUS_NONE)
    {
        // The host's decision first, to both sides.
        MpTrMirror(m, &out);
        out.slot = MP_SLOT_NONE;
        out.status = decision;
        out.flags = 0;
        MpTrHostSendTo(from, &out);
        if (targetOk)
        {
            out = *m;
            out.slot = MP_SLOT_NONE;
            out.status = decision;
            out.flags = 0;
            MpTrHostSendTo(target, &out);
        }
    }
    if (targetOk)
    {
        out = *m;
        out.slot = from;
        MpTrHostSendTo(target, &out);
    }
}

bool8 MpTradeCommit_OnHostReceiveRecover(u8 slot, const u8 *payload, u32 len)
{
    struct MpMsgTradeRecover m;

    if (MpProto_ParseTradeRecover(payload, len, &m) != MP_PARSE_OK || m.status == MP_TJ_STATUS_ABORTED)
        return FALSE; // ABORTED is the host's own word
    MpTrHostRoute(slot, &m);
    return TRUE;
}

bool8 MpTradeCommit_OnClientReceiveRecover(const u8 *payload, u32 len)
{
    struct MpMsgTradeRecover m;

    if (MpProto_ParseTradeRecover(payload, len, &m) != MP_PARSE_OK)
        return FALSE;
    MpTrQueuePush(&m);
    return TRUE;
}

void MP_TradeRecoverSend(const struct MpMsgTradeRecover *msg)
{
    u16 len;

    if (gMpSession->state != MP_STATE_IN_SESSION || msg->status == MP_TJ_STATUS_ABORTED)
        return;
    if (gMpSession->isHost)
    {
        MpTrHostRoute(gMpSession->localSlot, msg);
        return;
    }
    len = MpProto_WriteTradeRecover(sTcBuf, sizeof(sTcBuf), msg);
    if (len != 0)
        MpSession_SendTo(0, sTcBuf, len, MP_CHANNEL_CONTROL, TRUE);
}

bool8 MP_TradeRecoverPoll(struct MpMsgTradeRecover *out)
{
    struct MpTrQueue *q = MpTrQueueState();

    if (q->count == 0)
        return FALSE;
    *out = q->items[q->head];
    q->head = (u8)((q->head + 1) % MP_TRADE_RECOVER_QUEUE);
    q->count--;
    return TRUE;
}
