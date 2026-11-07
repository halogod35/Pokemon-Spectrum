#ifndef GUARD_MP_MP_PLAYERDATA_H
#define GUARD_MP_MP_PLAYERDATA_H

// Pokemon Spectrum multiplayer: player data for "view another player" (PS-042 trainer card).
// Game-side half of src/mp/mp_playerdata.c: it reads the save (never writes it, AD-10) and
// converts received, already validated data into the game's own structs for display.
//
// INTERNAL header: src/mp/*.c only. Needs global.h before it (struct TrainerCard).

#include "mp/mp.h"

struct TrainerCard;

// Fills a game struct TrainerCard (as CopyTrainerCardData would for an Emerald link partner:
// version VERSION_EMERALD, frontier fields in place) from a received card. Clamps once more,
// so the card screen's table indices (stars, gender) are always in range.
void MpPlayerData_ToTrainerCard(const struct MpTrainerCard *in, struct TrainerCard *out);

// Shows `card` with trainer_card.c's link-partner mode (front and back, B closes), returning
// to `callback`. Borrows gTrainerCards[1] and gLinkPlayers[1].language only for the call
// (ShowTrainerCardInLink copies them at once) and restores them before returning.
// The caller has already faded the screen out and cleaned up the overworld windows.
void MpPlayerData_ShowTrainerCard(const struct MpTrainerCard *card, void (*callback)(void));

// PS-043: shows `count` (1..6) validated raw mons in the summary screen, read only
// (SUMMARY_MODE_LOCK_MOVES), from a private static copy (never gPlayerParty / gEnemyParty).
// Up/Down switches between them, B returns to `callback`. count 0 goes straight to `callback`.
void MpPlayerData_ShowParty(u8 mons[PARTY_SIZE][MP_MON_WIRE_SIZE], u8 count, void (*callback)(void));

#endif // GUARD_MP_MP_PLAYERDATA_H
