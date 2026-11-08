#ifndef GUARD_MP_MP_TRADE_JOURNAL_H
#define GUARD_MP_MP_TRADE_JOURNAL_H

// Trade journal, pure part (PS-050, design.md "Trade journal"). No game state, no files:
// the record, its file format, the trade id, and the recovery rules. The game side
// (src/mp/mp_trade.c) does the file I/O, the save inspection and the roll forward.

#include "gba/types.h"
#include "mp/mp.h"

#define MP_TJ_FILE_SIZE     156
#define MP_TJ_MAGIC         0x4A545350u     // "PSTJ"
#define MP_TJ_VERSION       1

enum
{
    MP_TJ_STATE_NONE,
    MP_TJ_STATE_READY,      // written before READY; this save has NOT been signed with the trade
    MP_TJ_STATE_COMMIT,     // the outcome is COMMIT; this save has the trade or must get it
};

struct MpTradeMonId
{
    u32 personality;
    u32 otId;
    u16 species;
};

struct MpTradeJournal
{
    u8 state;                       // MP_TJ_STATE_READY / COMMIT
    u8 partyIndex;                  // where the given mon was (< PARTY_SIZE)
    u32 tradeId;                    // MpTradeJournal_ComputeId(myTid, given, partnerTid, received)
    u32 myTid;                      // this save's full trainer id
    u32 partnerTid;
    u8 partnerName[MP_PLAYER_NAME_SIZE]; // game charset, EOS terminated
    struct MpTradeMonId given;      // the mon this side gave
    struct MpTradeMonId received;   // the mon this side got, as it arrived (before evolution)
    u16 receivedFinalSpecies;       // after a trade evolution (== received.species otherwise)
    u8 receivedMon[MP_MON_WIRE_SIZE]; // struct Pokemon as it is saved (after evolution)
};

enum
{
    MP_TJ_PARSE_OK,
    MP_TJ_PARSE_BAD_SIZE,
    MP_TJ_PARSE_BAD_MAGIC,
    MP_TJ_PARSE_BAD_CHECKSUM,
    MP_TJ_PARSE_BAD_FIELD,
};

// The trade id both sides compute alike: FNV-1a over the two (trainer id, given mon personality,
// given mon OT id) triples, the lower trainer id first. Never 0.
u32 MpTradeJournal_ComputeId(u32 tidA, const struct MpTradeMonId *givenByA, u32 tidB, const struct MpTradeMonId *givenByB);
// Fixed little-endian layout with magic, version and an FNV-1a checksum (design.md).
void MpTradeJournal_Serialize(const struct MpTradeJournal *j, u8 out[MP_TJ_FILE_SIZE]);
// Untrusted input (AD-9): size, magic, version, checksum, state, party index, the trade id against
// the fields, the stored mon's personality / OT id against `received`, the name terminated.
// The game side then also requires myTid == this save and MP_ValidateMon(receivedMon).
u8 MpTradeJournal_Parse(const u8 *buf, u32 len, struct MpTradeJournal *out);

// What this side reports for a trade (TRADE_RECOVER status).
//   journalState: MP_TJ_STATE_* of our journal for that trade (NONE if we have none for it)
//   damaged: our journal file exists but is unreadable
//   liveWaiting: that very trade is still waiting for the host's decision here
//   haveTheirMon: (only used without a journal) the partner's given mon is in our party / boxes
u8 MpTradeJournal_LocalStatus(u8 journalState, bool8 damaged, bool8 liveWaiting, bool8 haveTheirMon);

enum
{
    MP_TJ_ACTION_KEEP,          // nothing yet
    MP_TJ_ACTION_COMMIT,        // READY -> COMMIT: write the journal as COMMIT, apply the trade
    MP_TJ_ACTION_ABORT,         // READY -> gone: nobody signed; delete the journal
    MP_TJ_ACTION_DONE,          // COMMIT and the partner is done or does not need us: delete
};
// The recovery rule: our journal state + what the other side (the partner, or the host's decision)
// reports. A save is only ever rolled forward: a signed save always means the outcome is COMMIT.
u8 MpTradeJournal_Resolve(u8 myState, u8 otherStatus);

// Binding of a received TRADE_RECOVER to our journal: same trade id, the sender is our partner,
// we are the receiver, and the mons are ours swapped. Pure.
struct MpMsgTradeRecover;
bool8 MpTradeJournal_MatchesMsg(const struct MpTradeJournal *j, const struct MpMsgTradeRecover *m);
// The trade id the message's own fields give (receiver = given the sender's recv mon).
u32 MpTradeJournal_IdOfMsg(const struct MpMsgTradeRecover *m);
// Fill a TRADE_RECOVER for our journal.
void MpTradeJournal_FillMsg(const struct MpTradeJournal *j, u8 status, u8 slot, u8 flags, struct MpMsgTradeRecover *out);

#endif // GUARD_MP_MP_TRADE_JOURNAL_H
