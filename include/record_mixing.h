#ifndef GUARD_RECORD_MIXING_H
#define GUARD_RECORD_MIXING_H

#include "daycare.h" // MP: PS-089 struct RecordMixingDaycareMail (for struct PlayerRecordEmerald below)

struct PlayerHallRecords
{
    struct RankingHall1P onePlayer[HALL_FACILITIES_COUNT][FRONTIER_LVL_MODE_COUNT];
    struct RankingHall2P twoPlayers[FRONTIER_LVL_MODE_COUNT];
};

// MP: PS-089. The Emerald record as it travels over the link (27 SendBlocks of 200 bytes), moved
// here from record_mixing.c so the direct-mix sanitizer (src/mp/mp_mix_validate.c) and its
// selftest can check a received one field by field. Layout unchanged: 0x1444 bytes.
struct PlayerRecordEmerald
{
    /* 0x0000 */ struct SecretBase secretBases[SECRET_BASES_COUNT];
    /* 0x0C80 */ TVShow tvShows[TV_SHOWS_COUNT];
    /* 0x1004 */ PokeNews pokeNews[POKE_NEWS_COUNT];
    /* 0x1044 */ OldMan oldMan;
    /* 0x1084 */ struct DewfordTrend dewfordTrends[SAVED_TRENDS_COUNT];
    /* 0x10AC */ struct RecordMixingDaycareMail daycareMail;
    /* 0x1124 */ struct EmeraldBattleTowerRecord battleTowerRecord;
    /* 0x1210 */ u16 giftItem;
    /* 0x1214 */ LilycoveLady lilycoveLady;
    /* 0x1254 */ struct Apprentice apprentices[2];
    /* 0x12DC */ struct PlayerHallRecords hallRecords;
    /* 0x1434 */ u8 filler_1434[16];
}; // 0x1444

#define PLAYER_RECORD_EMERALD_SIZE 0x1444 // MP: PS-089, checked against sizeof at compile time in mp_mix_validate.c

void RecordMixingPlayerSpotTriggered(void);
void GetPlayerHallRecords(struct PlayerHallRecords *dst);
// MP: PS-089. Starts the vanilla mixing task directly (no Cable Club seat exchange): the link is
// already open with both players (LINKTYPE_RECORD_MIX_BEFORE) and gSpecialVar_0x8005 holds our
// link id. Only src/mp/mp_mix.c calls this.
void RecordMixing_StartDirect(void);

#endif //GUARD_RECORD_MIXING_H
