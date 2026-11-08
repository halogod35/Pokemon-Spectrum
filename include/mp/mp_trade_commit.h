#ifndef GUARD_MP_MP_TRADE_COMMIT_H
#define GUARD_MP_MP_TRADE_COMMIT_H

// Host-arbitrated trade commit, session side (PS-044, protocol.md 6.15). Internal to src/mp.
// The public member API (MP_TradeCommitReady / Poll / Reset) is in mp.h.
//
// The decision logic is pure (struct MpTradeCommitHost + a send callback) so the selftest can
// drive it without a network; mp_trade_commit.c wraps it with the session and mp_link.c.

#include "gba/types.h"
#include "mp/mp.h"

enum
{
    MP_TRADE_REC_NONE,
    MP_TRADE_REC_READY,         // READY received, no decision yet
    MP_TRADE_REC_COMMIT,        // decided: COMMIT (final)
    MP_TRADE_REC_ABORT,         // decided: ABORT (final)
};

// The host's record of one slot's current round.
struct MpTradeCommitRec
{
    u8 state;                   // MP_TRADE_REC_*
    u8 groupId;
    u8 seq;
    u8 partner;                 // the other member's slot
    u8 reason;                  // MP_TRADE_ABORT_* once ABORT
    u32 startMs;                // when READY arrived (timeout)
};

struct MpTradeCommitHost
{
    struct MpTradeCommitRec rec[MP_MAX_PLAYERS];
    u32 commits;
    u32 aborts;
};

// Sends one decision (MP_TRADE_OP_COMMIT / ABORT) to `slot`.
typedef void (*MpTradeCommitSendFunc)(u8 slot, u8 op, u8 groupId, u8 seq, u8 reason);

// --- Pure decision logic (no session access) ---
// READY(groupId, seq) from `slot`. `groupOk`: the host sees `slot` in a 2-member link group with
// that id, whose other member is `partner`. Decisions go out through `send`; for COMMIT the
// partner is sent first and `slot` second (the wrapper reorders so the host's own slot is last).
void MpTradeCommit_HostReady(struct MpTradeCommitHost *h, u8 slot, u8 groupId, u8 seq, bool8 groupOk, u8 partner, u32 now, MpTradeCommitSendFunc send);
// The link group with members `memberMask` ended: every undecided round in it is aborted.
void MpTradeCommit_HostGroupEnded(struct MpTradeCommitHost *h, u8 memberMask, MpTradeCommitSendFunc send);
// Timeout: undecided rounds older than MP_TRADE_COMMIT_TIMEOUT_MS are aborted.
void MpTradeCommit_HostTick(struct MpTradeCommitHost *h, u32 now, MpTradeCommitSendFunc send);
// A slot left the table: forget its record.
void MpTradeCommit_HostForgetSlot(struct MpTradeCommitHost *h, u8 slot);

// --- Session hooks (from mp_link.c) ---
void MpTradeCommit_ResetInstance(u8 instance);
void MpTradeCommit_OnSessionStart(void);
void MpTradeCommit_OnSessionEnd(void);
void MpTradeCommit_HostOnGroupEnded(u8 memberMask);
void MpTradeCommit_HostOnSlotLeft(u8 slot);
void MpTradeCommit_HostUpdate(void);
// TRADE_COMMIT from `slot` (host) or from the host (client). FALSE = malformed / wrong direction.
bool8 MpTradeCommit_OnHostReceive(u8 slot, const u8 *payload, u32 len);
bool8 MpTradeCommit_OnClientReceive(const u8 *payload, u32 len);

// Selftest: the host table of the selected instance.
const struct MpTradeCommitHost *MpTradeCommit_DebugHost(void);

#endif // GUARD_MP_MP_TRADE_COMMIT_H
