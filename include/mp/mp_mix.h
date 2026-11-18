#ifndef GUARD_MP_MIX_H
#define GUARD_MP_MIX_H

// Direct record mixing from the overworld (PS-089, src/mp/mp_mix.c): the pure part, so the
// selftest (mp_selftest_mix.c) can run it without a game. The game side is in mp.h / mp_menu.h.
//
// MP_MixValidateRecord checks a received struct PlayerRecordEmerald (include/record_mixing.h),
// another player's save contents and therefore untrusted network input, before record_mixing.c's
// Receive* functions index game tables with its fields. It reads only the record and compile-time
// constants: no save block, no link state, no I/O. Rules (what each Receive* / display path needs):
//   - species < NUM_SPECIES, moves < MOVES_COUNT, items < ITEMS_COUNT, levels <= MAX_LEVEL;
//   - easy chat words: EC_EMPTY_WORD or a group < EC_NUM_GROUPS (CopyEasyChatWord checks the index);
//   - strings that the game prints with StringCopy: an EOS inside the buffer, or an all-zero
//     (never written) buffer, which is what an unused record from a real save looks like;
//   - enum / index fields in their table's range: TV show kinds and the fields each kind indexes
//     with, PokeNews kinds, secret base ids and decorations, the Mauville old man's id and his
//     stat / question / decoration indices, the Battle Tower record's facility class and level
//     mode, the Lilycove lady's id / favor / quiz / contest fields, apprentice ids and modes;
//   - counts clamped: daycare mons <= 2, apprentice questions <= 9, seminar moves <= 3, ...
// Anything outside drops the WHOLE record (no partial apply): the caller shows "The records
// couldn't be mixed." and applies nothing.

#include "gba/types.h"

struct PlayerRecordEmerald;

// Result of MP_MixValidateRecord: which part of the record failed (MP_MIX_OK = none).
enum
{
    MP_MIX_OK,
    MP_MIX_BAD_SECRET_BASE,
    MP_MIX_BAD_TV_SHOW,
    MP_MIX_BAD_POKE_NEWS,
    MP_MIX_BAD_OLD_MAN,
    MP_MIX_BAD_TREND,
    MP_MIX_BAD_DAYCARE_MAIL,
    MP_MIX_BAD_TOWER_RECORD,
    MP_MIX_BAD_GIFT_ITEM,
    MP_MIX_BAD_LILYCOVE_LADY,
    MP_MIX_BAD_APPRENTICE,
    MP_MIX_BAD_HALL_RECORD,
    MP_MIX_REJECT_COUNT,
};

// Checks `record` (exactly sizeof(struct PlayerRecordEmerald) bytes). `fromLinkMaster`: the
// sender is link player 0; only its giftItem is ever read (PrepareExchangePacket leaves the
// field unset on the other players, so it is not checked for them). Returns MP_MIX_OK or the
// MP_MIX_BAD_* code of the first bad part; *detail (may be NULL) then gets the index of the bad
// element in that part (secret base / show / news / trend / mail / apprentice / hall record
// number, else 0) for the log. Pure and deterministic.
u8 MP_MixValidateRecord(const struct PlayerRecordEmerald *record, bool8 fromLinkMaster, u16 *detail);
// Log name of a result code ("ok", "secret base", ...); "?" out of range.
const char *MP_MixRejectName(u8 code);

#endif // GUARD_MP_MIX_H
