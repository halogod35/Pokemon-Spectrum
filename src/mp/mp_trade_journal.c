// Trade journal, pure part (PS-050). See include/mp/mp_trade_journal.h and design.md.
//
// File layout (MP_TJ_FILE_SIZE = 156 bytes, little-endian):
//   0  u32 magic "PSTJ"      4  u16 version      6  u8 state      7  u8 partyIndex
//   8  u32 tradeId          12  u32 myTid       16  u32 partnerTid
//  20  u8[8] partnerName
//  28  given:    u32 personality, u32 otId, u16 species
//  38  received: u32 personality, u32 otId, u16 species
//  48  u16 receivedFinalSpecies               50  u16 reserved (0)
//  52  u8[100] receivedMon (struct Pokemon)  152  u32 FNV-1a of bytes 0..151

#include <string.h>
#include "mp/mp_protocol.h"
#include "mp/mp_trade_journal.h"
#include "mp/mp_wire.h"
#include "constants/characters.h"
#include "constants/species.h"

#define TJ_CHECKSUM_OFFSET  (MP_TJ_FILE_SIZE - 4)

static u32 TjFnv(u32 h, const u8 *data, u32 len)
{
    u32 i;

    for (i = 0; i < len; i++)
    {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

static u32 TjFnvU32(u32 h, u32 v)
{
    u8 b[4];

    b[0] = (u8)v;
    b[1] = (u8)(v >> 8);
    b[2] = (u8)(v >> 16);
    b[3] = (u8)(v >> 24);
    return TjFnv(h, b, 4);
}

u32 MpTradeJournal_ComputeId(u32 tidA, const struct MpTradeMonId *givenByA, u32 tidB, const struct MpTradeMonId *givenByB)
{
    u32 h = 2166136261u;
    bool8 aFirst = (tidA < tidB) || (tidA == tidB && givenByA->personality <= givenByB->personality);
    const struct MpTradeMonId *first = aFirst ? givenByA : givenByB;
    const struct MpTradeMonId *second = aFirst ? givenByB : givenByA;

    h = TjFnvU32(h, aFirst ? tidA : tidB);
    h = TjFnvU32(h, first->personality);
    h = TjFnvU32(h, first->otId);
    h = TjFnvU32(h, aFirst ? tidB : tidA);
    h = TjFnvU32(h, second->personality);
    h = TjFnvU32(h, second->otId);
    return (h == 0) ? 1 : h;
}

static void TjWriteMonId(struct MpWriter *w, const struct MpTradeMonId *id)
{
    MpWriteU32(w, id->personality);
    MpWriteU32(w, id->otId);
    MpWriteU16(w, id->species);
}

static void TjReadMonId(struct MpReader *r, struct MpTradeMonId *id)
{
    id->personality = MpReadU32(r);
    id->otId = MpReadU32(r);
    id->species = MpReadU16(r);
}

void MpTradeJournal_Serialize(const struct MpTradeJournal *j, u8 out[MP_TJ_FILE_SIZE])
{
    struct MpWriter w;

    memset(out, 0, MP_TJ_FILE_SIZE);
    MpWriter_Init(&w, out, MP_TJ_FILE_SIZE);
    MpWriteU32(&w, MP_TJ_MAGIC);
    MpWriteU16(&w, MP_TJ_VERSION);
    MpWriteU8(&w, j->state);
    MpWriteU8(&w, j->partyIndex);
    MpWriteU32(&w, j->tradeId);
    MpWriteU32(&w, j->myTid);
    MpWriteU32(&w, j->partnerTid);
    MpWriteBytes(&w, j->partnerName, MP_PLAYER_NAME_SIZE);
    TjWriteMonId(&w, &j->given);
    TjWriteMonId(&w, &j->received);
    MpWriteU16(&w, j->receivedFinalSpecies);
    MpWriteU16(&w, 0);
    MpWriteBytes(&w, j->receivedMon, MP_MON_WIRE_SIZE);
    MpWriteU32(&w, TjFnv(2166136261u, out, TJ_CHECKSUM_OFFSET));
}

static bool8 TjSpeciesOk(u16 species)
{
    return species != SPECIES_NONE && species < NUM_SPECIES;
}

u8 MpTradeJournal_Parse(const u8 *buf, u32 len, struct MpTradeJournal *out)
{
    struct MpTradeJournal j;
    struct MpReader r;
    u32 checksum;
    u32 i;
    bool8 ended;

    memset(out, 0, sizeof(*out));
    if (buf == NULL || len != MP_TJ_FILE_SIZE)
        return MP_TJ_PARSE_BAD_SIZE;
    MpReader_Init(&r, buf, len);
    if (MpReadU32(&r) != MP_TJ_MAGIC || MpReadU16(&r) != MP_TJ_VERSION)
        return MP_TJ_PARSE_BAD_MAGIC;
    checksum = (u32)buf[TJ_CHECKSUM_OFFSET] | ((u32)buf[TJ_CHECKSUM_OFFSET + 1] << 8)
             | ((u32)buf[TJ_CHECKSUM_OFFSET + 2] << 16) | ((u32)buf[TJ_CHECKSUM_OFFSET + 3] << 24);
    if (checksum != TjFnv(2166136261u, buf, TJ_CHECKSUM_OFFSET))
        return MP_TJ_PARSE_BAD_CHECKSUM;
    memset(&j, 0, sizeof(j));
    j.state = MpReadU8(&r);
    j.partyIndex = MpReadU8(&r);
    j.tradeId = MpReadU32(&r);
    j.myTid = MpReadU32(&r);
    j.partnerTid = MpReadU32(&r);
    MpReadBytes(&r, j.partnerName, MP_PLAYER_NAME_SIZE);
    TjReadMonId(&r, &j.given);
    TjReadMonId(&r, &j.received);
    j.receivedFinalSpecies = MpReadU16(&r);
    MpReadU16(&r); // reserved
    MpReadBytes(&r, j.receivedMon, MP_MON_WIRE_SIZE);
    if (r.error)
        return MP_TJ_PARSE_BAD_SIZE;

    if ((j.state != MP_TJ_STATE_READY && j.state != MP_TJ_STATE_COMMIT) || j.partyIndex >= PARTY_SIZE)
        return MP_TJ_PARSE_BAD_FIELD;
    if (!TjSpeciesOk(j.given.species) || !TjSpeciesOk(j.received.species) || !TjSpeciesOk(j.receivedFinalSpecies))
        return MP_TJ_PARSE_BAD_FIELD;
    if (j.tradeId != MpTradeJournal_ComputeId(j.myTid, &j.given, j.partnerTid, &j.received))
        return MP_TJ_PARSE_BAD_FIELD;
    // struct Pokemon starts with u32 personality, u32 otId (unencrypted).
    if ((u32)(j.receivedMon[0] | (j.receivedMon[1] << 8) | (j.receivedMon[2] << 16) | ((u32)j.receivedMon[3] << 24)) != j.received.personality
     || (u32)(j.receivedMon[4] | (j.receivedMon[5] << 8) | (j.receivedMon[6] << 16) | ((u32)j.receivedMon[7] << 24)) != j.received.otId)
        return MP_TJ_PARSE_BAD_FIELD;
    // The name must be terminated; everything after the first EOS is EOS.
    for (i = 0, ended = FALSE; i < MP_PLAYER_NAME_SIZE; i++)
    {
        if (j.partnerName[i] == EOS)
            ended = TRUE;
        else if (ended)
            j.partnerName[i] = EOS;
    }
    if (!ended)
        return MP_TJ_PARSE_BAD_FIELD;
    *out = j;
    return MP_TJ_PARSE_OK;
}

u8 MpTradeJournal_LocalStatus(u8 journalState, bool8 damaged, bool8 liveWaiting, bool8 haveTheirMon)
{
    if (damaged)
        return MP_TJ_STATUS_DAMAGED;
    switch (journalState)
    {
    case MP_TJ_STATE_READY:
        return liveWaiting ? MP_TJ_STATUS_BUSY : MP_TJ_STATUS_READY;
    case MP_TJ_STATE_COMMIT:
        return MP_TJ_STATUS_COMMITTED;
    default:
        return haveTheirMon ? MP_TJ_STATUS_NONE_HAS : MP_TJ_STATUS_NONE_HASNOT;
    }
}

u8 MpTradeJournal_Resolve(u8 myState, u8 otherStatus)
{
    if (myState == MP_TJ_STATE_READY)
    {
        switch (otherStatus)
        {
        case MP_TJ_STATUS_COMMITTED:    // the other side knows the outcome is COMMIT
        case MP_TJ_STATUS_NONE_HAS:     // the other side has our mon: it happened there
            return MP_TJ_ACTION_COMMIT;
        case MP_TJ_STATUS_READY:        // neither signed, and nobody can learn COMMIT any more
        case MP_TJ_STATUS_NONE_HASNOT:  // the other side never signed it (or aborted)
        case MP_TJ_STATUS_ABORTED:      // the host's final decision
            return MP_TJ_ACTION_ABORT;
        default:
            return MP_TJ_ACTION_KEEP;
        }
    }
    if (myState == MP_TJ_STATE_COMMIT)
    {
        switch (otherStatus)
        {
        case MP_TJ_STATUS_COMMITTED:    // it knows: it will apply it from its own journal
        case MP_TJ_STATUS_NONE_HAS:     // it has applied it and is done
        case MP_TJ_STATUS_NONE_HASNOT:  // it has no record and not the mon: nothing we can do for it
            return MP_TJ_ACTION_DONE;
        default:                        // READY: it learns COMMITTED from us; keep answering
            return MP_TJ_ACTION_KEEP;
        }
    }
    return MP_TJ_ACTION_KEEP;
}

u32 MpTradeJournal_IdOfMsg(const struct MpMsgTradeRecover *m)
{
    struct MpTradeMonId a, b;

    a.personality = m->senderGivenPersonality;
    a.otId = m->senderGivenOtId;
    a.species = 0;
    b.personality = m->senderRecvPersonality;
    b.otId = m->senderRecvOtId;
    b.species = 0;
    return MpTradeJournal_ComputeId(m->senderTid, &a, m->receiverTid, &b);
}

bool8 MpTradeJournal_MatchesMsg(const struct MpTradeJournal *j, const struct MpMsgTradeRecover *m)
{
    return j->state != MP_TJ_STATE_NONE
        && m->tradeId == j->tradeId
        && m->receiverTid == j->myTid
        && m->senderTid == j->partnerTid
        && m->senderGivenPersonality == j->received.personality
        && m->senderGivenOtId == j->received.otId
        && m->senderRecvPersonality == j->given.personality
        && m->senderRecvOtId == j->given.otId;
}

void MpTradeJournal_FillMsg(const struct MpTradeJournal *j, u8 status, u8 slot, u8 flags, struct MpMsgTradeRecover *out)
{
    memset(out, 0, sizeof(*out));
    out->slot = slot;
    out->status = status;
    out->flags = flags;
    out->tradeId = j->tradeId;
    out->senderTid = j->myTid;
    out->receiverTid = j->partnerTid;
    out->senderGivenPersonality = j->given.personality;
    out->senderGivenOtId = j->given.otId;
    out->senderRecvPersonality = j->received.personality;
    out->senderRecvOtId = j->received.otId;
}
