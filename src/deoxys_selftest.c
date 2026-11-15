// PS-082: --deoxys-selftest. Checks the Deoxys formes headless (no window, no save file; the save
// blocks are zeroed in memory) and exits 0 (OK) or 1 (FAILED). Checks:
//   - a new Deoxys is SPECIES_DEOXYS (the Normal forme) with the Normal stats from the stat formula;
//   - GetNextDeoxysForme cycles Attack -> Defense -> Speed -> Normal, and applying it to a Pokémon
//     (the same calls ItemUseCB_Meteorite makes) gives each forme's base stats and computed stats;
//   - each forme's level-up list is the one from its Gen 3 game: Double Team at 10 only for Speed,
//     Taunt at 15 only for Attack, Spikes at 20 only for Defense, Snatch at 30 for Normal (and
//     Defense, as in LeafGreen) but not for Attack or Speed;
//   - all four map to dex number 386 and Hoenn 202, NationalPokedexNumToSpecies(386) is the Normal
//     forme, every forme has front/back pic, palette, icon and name table entries, the cry is shared,
//     and the "two-frame animation" rule excludes all four;
//   - a non-Deoxys is left alone by the forme cycle.
#include <stdio.h>
#include <string.h>
#include "global.h"
#include "data.h"
#include "load_save.h"
#include "malloc.h"
#include "pokemon.h"
#include "pokemon_icon.h"
#include "pokemon_storage_system.h"
#include "string_util.h"
#include "constants/abilities.h"
#include "constants/moves.h"
#include "constants/pokemon.h"
#include "constants/species.h"

extern const u8 *const gMonFootprintTable[]; // no header declares it (pokedex.c does the same)

static u32 sFailures;

#define DX_CHECK(cond)                                                                      \
    do                                                                                      \
    {                                                                                       \
        if (!(cond))                                                                        \
        {                                                                                   \
            sFailures++;                                                                    \
            printf("[deoxys] selftest: check failed (deoxys_selftest.c:%d): %s\n", __LINE__, #cond); \
        }                                                                                   \
    } while (0)

static void Report(const char *group, u32 failuresBefore)
{
    if (sFailures == failuresBefore)
        printf("[deoxys] selftest: %s OK\n", group);
    else
        printf("[deoxys] selftest: %s FAILED (%u)\n", group, (unsigned int)(sFailures - failuresBefore));
}

static void ResetGameState(void)
{
    static const u8 sName[] = _("TESTER");

    SetSaveBlocksPointers(0);
    memset(gSaveBlock2Ptr, 0, sizeof(*gSaveBlock2Ptr));
    memset(gSaveBlock1Ptr, 0, sizeof(*gSaveBlock1Ptr));
    memset(gPokemonStoragePtr, 0, sizeof(*gPokemonStoragePtr));
    StringCopy(gSaveBlock2Ptr->playerName, sName);
}

#define TEST_LEVEL 50
#define TEST_PERSONALITY 0 // nature 0 (Hardy, neutral), so the stat formula has no nature term

struct FormeExpect
{
    u16 species;
    u8 hp, atk, def, spd, spatk, spdef;
};

static const struct FormeExpect sFormes[] =
{
    { SPECIES_DEOXYS,          50, 150,  50, 150, 150,  50 },
    { SPECIES_DEOXYS_ATTACK,   50, 180,  20, 150, 180,  20 },
    { SPECIES_DEOXYS_DEFENSE,  50,  70, 160,  90,  70, 160 },
    { SPECIES_DEOXYS_SPEED,    50,  95,  90, 180,  95,  90 },
};

// Level 50, every IV 0, every EV 0, neutral nature: HP = 2*base*50/100 + 50 + 10, others = 2*base*50/100 + 5
static u16 ExpectedStat(u8 base, bool8 isHp)
{
    u32 stat = (2 * base * TEST_LEVEL) / 100;
    return isHp ? stat + TEST_LEVEL + 10 : stat + 5;
}

static void CheckFormeStats(struct Pokemon *mon, const struct FormeExpect *f)
{
    const struct SpeciesInfo *info = &gSpeciesInfo[f->species];

    DX_CHECK(GetMonData(mon, MON_DATA_SPECIES) == f->species);
    DX_CHECK(info->baseHP == f->hp && info->baseAttack == f->atk && info->baseDefense == f->def
          && info->baseSpeed == f->spd && info->baseSpAttack == f->spatk && info->baseSpDefense == f->spdef);
    DX_CHECK(GetMonData(mon, MON_DATA_MAX_HP) == ExpectedStat(f->hp, TRUE));
    DX_CHECK(GetMonData(mon, MON_DATA_ATK) == ExpectedStat(f->atk, FALSE));
    DX_CHECK(GetMonData(mon, MON_DATA_DEF) == ExpectedStat(f->def, FALSE));
    DX_CHECK(GetMonData(mon, MON_DATA_SPEED) == ExpectedStat(f->spd, FALSE));
    DX_CHECK(GetMonData(mon, MON_DATA_SPATK) == ExpectedStat(f->spatk, FALSE));
    DX_CHECK(GetMonData(mon, MON_DATA_SPDEF) == ExpectedStat(f->spdef, FALSE));
    DX_CHECK(GetMonData(mon, MON_DATA_LEVEL) == TEST_LEVEL);
}

static bool8 LearnsMoveAtLevel(u16 species, u8 level, u16 move)
{
    const u16 *learnset = gLevelUpLearnsets[species];
    u32 i;

    if (learnset == NULL)
        return FALSE;
    for (i = 0; i < MAX_LEVEL_UP_MOVES * 2 && learnset[i] != LEVEL_UP_END; i++)
    {
        if ((learnset[i] & LEVEL_UP_MOVE_ID) == move && (learnset[i] & LEVEL_UP_MOVE_LV) >> 9 == level)
            return TRUE;
    }
    return FALSE;
}

static void TestNewDeoxysIsNormal(void)
{
    u32 before = sFailures;
    struct Pokemon mon;

    CreateMon(&mon, SPECIES_DEOXYS, TEST_LEVEL, 0, TRUE, TEST_PERSONALITY, OT_ID_PRESET, 12345);
    CheckFormeStats(&mon, &sFormes[0]);

    Report("new Deoxys is the Normal forme", before);
}

static void TestMeteoriteCycle(void)
{
    u32 before = sFailures;
    struct Pokemon mon;
    u32 i;

    DX_CHECK(GetNextDeoxysForme(SPECIES_DEOXYS) == SPECIES_DEOXYS_ATTACK);
    DX_CHECK(GetNextDeoxysForme(SPECIES_DEOXYS_ATTACK) == SPECIES_DEOXYS_DEFENSE);
    DX_CHECK(GetNextDeoxysForme(SPECIES_DEOXYS_DEFENSE) == SPECIES_DEOXYS_SPEED);
    DX_CHECK(GetNextDeoxysForme(SPECIES_DEOXYS_SPEED) == SPECIES_DEOXYS);
    DX_CHECK(IsDeoxysForme(SPECIES_DEOXYS) && IsDeoxysForme(SPECIES_DEOXYS_ATTACK)
          && IsDeoxysForme(SPECIES_DEOXYS_DEFENSE) && IsDeoxysForme(SPECIES_DEOXYS_SPEED));

    // Four uses: Attack, Defense, Speed, back to Normal, each with its own stats (what the item does)
    CreateMon(&mon, SPECIES_DEOXYS, TEST_LEVEL, 0, TRUE, TEST_PERSONALITY, OT_ID_PRESET, 12345);
    for (i = 1; i <= ARRAY_COUNT(sFormes); i++)
    {
        u16 next = GetNextDeoxysForme(GetMonData(&mon, MON_DATA_SPECIES));

        DX_CHECK(next == sFormes[i % ARRAY_COUNT(sFormes)].species);
        SetMonData(&mon, MON_DATA_SPECIES, &next);
        CalculateMonStats(&mon);
        CheckFormeStats(&mon, &sFormes[i % ARRAY_COUNT(sFormes)]);
    }
    DX_CHECK(GetMonData(&mon, MON_DATA_SPECIES) == SPECIES_DEOXYS);

    Report("Meteorite cycle Attack -> Defense -> Speed -> Normal", before);
}

static void TestLearnsets(void)
{
    u32 before = sFailures;

    // Double Team at 10 only for Speed (Emerald); the others have Teleport there
    DX_CHECK(LearnsMoveAtLevel(SPECIES_DEOXYS_SPEED, 10, MOVE_DOUBLE_TEAM));
    DX_CHECK(!LearnsMoveAtLevel(SPECIES_DEOXYS, 10, MOVE_DOUBLE_TEAM));
    DX_CHECK(!LearnsMoveAtLevel(SPECIES_DEOXYS_ATTACK, 10, MOVE_DOUBLE_TEAM));
    DX_CHECK(!LearnsMoveAtLevel(SPECIES_DEOXYS_DEFENSE, 10, MOVE_DOUBLE_TEAM));
    DX_CHECK(LearnsMoveAtLevel(SPECIES_DEOXYS, 10, MOVE_TELEPORT));
    // Taunt at 15 only for Attack (FireRed)
    DX_CHECK(LearnsMoveAtLevel(SPECIES_DEOXYS_ATTACK, 15, MOVE_TAUNT));
    DX_CHECK(!LearnsMoveAtLevel(SPECIES_DEOXYS, 15, MOVE_TAUNT));
    DX_CHECK(!LearnsMoveAtLevel(SPECIES_DEOXYS_DEFENSE, 15, MOVE_TAUNT));
    DX_CHECK(!LearnsMoveAtLevel(SPECIES_DEOXYS_SPEED, 15, MOVE_TAUNT));
    // Spikes at 20 only for Defense (LeafGreen)
    DX_CHECK(LearnsMoveAtLevel(SPECIES_DEOXYS_DEFENSE, 20, MOVE_SPIKES));
    DX_CHECK(!LearnsMoveAtLevel(SPECIES_DEOXYS, 20, MOVE_SPIKES));
    DX_CHECK(!LearnsMoveAtLevel(SPECIES_DEOXYS_ATTACK, 20, MOVE_SPIKES));
    DX_CHECK(!LearnsMoveAtLevel(SPECIES_DEOXYS_SPEED, 20, MOVE_SPIKES));
    // Snatch at 30 for Normal (Ruby/Sapphire; LeafGreen too), Superpower / Swift for Attack / Speed
    DX_CHECK(LearnsMoveAtLevel(SPECIES_DEOXYS, 30, MOVE_SNATCH));
    DX_CHECK(LearnsMoveAtLevel(SPECIES_DEOXYS_DEFENSE, 30, MOVE_SNATCH));
    DX_CHECK(!LearnsMoveAtLevel(SPECIES_DEOXYS_ATTACK, 30, MOVE_SNATCH));
    DX_CHECK(!LearnsMoveAtLevel(SPECIES_DEOXYS_SPEED, 30, MOVE_SNATCH));
    DX_CHECK(LearnsMoveAtLevel(SPECIES_DEOXYS_ATTACK, 30, MOVE_SUPERPOWER));
    DX_CHECK(LearnsMoveAtLevel(SPECIES_DEOXYS_SPEED, 30, MOVE_SWIFT));
    // LeafGreen's double entries
    DX_CHECK(LearnsMoveAtLevel(SPECIES_DEOXYS_DEFENSE, 35, MOVE_IRON_DEFENSE));
    DX_CHECK(LearnsMoveAtLevel(SPECIES_DEOXYS_DEFENSE, 35, MOVE_AMNESIA));
    DX_CHECK(LearnsMoveAtLevel(SPECIES_DEOXYS_DEFENSE, 50, MOVE_COUNTER));
    DX_CHECK(LearnsMoveAtLevel(SPECIES_DEOXYS_DEFENSE, 50, MOVE_MIRROR_COAT));
    // Shared first moves
    DX_CHECK(LearnsMoveAtLevel(SPECIES_DEOXYS_ATTACK, 1, MOVE_LEER) && LearnsMoveAtLevel(SPECIES_DEOXYS_SPEED, 45, MOVE_PSYCHO_BOOST));

    Report("level-up lists per forme", before);
}

static void TestTables(void)
{
    u32 before = sFailures;
    u32 i;

    DX_CHECK(NationalPokedexNumToSpecies(NATIONAL_DEX_DEOXYS) == SPECIES_DEOXYS);
    DX_CHECK(HoennPokedexNumToSpecies(HOENN_DEX_DEOXYS) == SPECIES_DEOXYS);
    for (i = 0; i < ARRAY_COUNT(sFormes); i++)
    {
        u16 species = sFormes[i].species;

        DX_CHECK(species < NUM_SPECIES);
        DX_CHECK(SpeciesToNationalPokedexNum(species) == NATIONAL_DEX_DEOXYS);
        DX_CHECK(SpeciesToHoennPokedexNum(species) == HOENN_DEX_DEOXYS);
        DX_CHECK(gMonFrontPicTable[species].data != NULL && gMonFrontPicTable[species].tag == species);
        DX_CHECK(gMonBackPicTable[species].data != NULL && gMonBackPicTable[species].tag == species);
        DX_CHECK(gMonStillFrontPicTable[species].data != NULL);
        DX_CHECK(gMonPaletteTable[species].data != NULL && gMonPaletteTable[species].tag == species);
        DX_CHECK(gMonShinyPaletteTable[species].data != NULL);
        DX_CHECK(gMonIconTable[species] != NULL);
        DX_CHECK(gMonIconPaletteIndices[species] == gMonIconPaletteIndices[SPECIES_DEOXYS]);
        DX_CHECK(gMonFootprintTable[species] == gMonFootprintTable[SPECIES_DEOXYS]);
        DX_CHECK(gLevelUpLearnsets[species] != NULL);
        DX_CHECK(StringCompare(gSpeciesNames[species], gSpeciesNames[SPECIES_DEOXYS]) == 0);
        DX_CHECK(SpeciesToCryId(species - 1) == SpeciesToCryId(SPECIES_DEOXYS - 1));
        DX_CHECK(!HasTwoFramesAnimation(species));
        DX_CHECK(gSpeciesInfo[species].types[0] == TYPE_PSYCHIC && gSpeciesInfo[species].abilities[0] == ABILITY_PRESSURE);
    }
    // The three formes are different sprites and icons, not the Normal ones again
    DX_CHECK(gMonFrontPicTable[SPECIES_DEOXYS_ATTACK].data != gMonFrontPicTable[SPECIES_DEOXYS].data);
    DX_CHECK(gMonFrontPicTable[SPECIES_DEOXYS_DEFENSE].data != gMonFrontPicTable[SPECIES_DEOXYS_ATTACK].data);
    DX_CHECK(gMonFrontPicTable[SPECIES_DEOXYS_SPEED].data != gMonFrontPicTable[SPECIES_DEOXYS_DEFENSE].data);
    DX_CHECK(gMonIconTable[SPECIES_DEOXYS_ATTACK] != gMonIconTable[SPECIES_DEOXYS]);
    DX_CHECK(gMonIconTable[SPECIES_DEOXYS_SPEED] != gMonIconTable[SPECIES_DEOXYS]);
    // Pokédex flag bytes did not grow (no save layout change): 415 species still round to 52 bytes
    DX_CHECK(NUM_DEX_FLAG_BYTES == 52);

    Report("species tables", before);
}

static void TestNonDeoxysLeftAlone(void)
{
    u32 before = sFailures;
    struct Pokemon mon;
    u16 species;

    DX_CHECK(GetNextDeoxysForme(SPECIES_MEW) == SPECIES_NONE);
    DX_CHECK(GetNextDeoxysForme(SPECIES_CHIMECHO) == SPECIES_NONE);
    DX_CHECK(GetNextDeoxysForme(SPECIES_EGG) == SPECIES_NONE);
    DX_CHECK(GetNextDeoxysForme(SPECIES_NONE) == SPECIES_NONE);
    DX_CHECK(!IsDeoxysForme(SPECIES_JIRACHI));
    DX_CHECK(HasTwoFramesAnimation(SPECIES_JIRACHI));

    CreateMon(&mon, SPECIES_JIRACHI, TEST_LEVEL, 0, TRUE, TEST_PERSONALITY, OT_ID_PRESET, 12345);
    species = GetMonData(&mon, MON_DATA_SPECIES);
    DX_CHECK(GetNextDeoxysForme(species) == SPECIES_NONE);
    DX_CHECK(GetMonData(&mon, MON_DATA_SPECIES) == SPECIES_JIRACHI);

    Report("non-Deoxys left alone", before);
}

s32 DeoxysSelftest_Run(void)
{
    sFailures = 0;
    InitHeap(gHeap, HEAP_SIZE);
    ResetGameState();

    TestNewDeoxysIsNormal();
    TestMeteoriteCycle();
    TestLearnsets();
    TestTables();
    TestNonDeoxysLeftAlone();

    if (sFailures == 0)
    {
        printf("[deoxys] selftest OK\n");
        return 0;
    }
    printf("[deoxys] selftest FAILED (%u)\n", (unsigned int)sFailures);
    return 1;
}
