// PS-070: --save-selftest group "migration". Runs in CI. Checks, on "<save>.selftest":
//   - a synthetic save format 1 file (128 KB, two slots with different counters and
//     rotations, Hall of Fame, Trainer Hill, Recorded Battle) migrates in memory and the game
//     loads exactly the same data from it (the newest slot, same counter and rotation);
//   - the file on disk is untouched until the first save; that save writes "<save>.v1.bak"
//     (byte-identical to the original) once, and the save itself in format 2 (144 KB);
//   - a format 2 round trip, with flags and vars at the ends of the vanilla and the new ranges;
//   - files of any other size are refused and FLASH_BASE is left alone;
//   - a missing / empty file and an erased format 1 file load as an empty flash, without backup.
#include <stdio.h>
#include <string.h>
#include "global.h"
#include "agb_flash.h"
#include "gba/flash_internal.h"
#include "decompress.h"
#include "event_data.h"
#include "load_save.h"
#include "platform.h"
#include "save.h"
#include "constants/flags.h"
#include "constants/vars.h"

static u32 sFailures;

#define MG_CHECK(cond)                                                                      \
    do                                                                                      \
    {                                                                                       \
        if (!(cond))                                                                        \
        {                                                                                   \
            printf("[save] selftest: FAIL: migration: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
            sFailures++;                                                                    \
        }                                                                                   \
    } while (0)

#define V1_SPECIAL_SECTOR_FIRST (SAVE_V1_SECTORS_PER_SLOT * NUM_SAVE_SLOTS) // 28
// The game writes save counter N into slot N % 2: the newest save (8) is in slot 1, the
// older one (7) in slot 2. Each slot has its own rotation (where sector id 0 sits).
#define NEWEST_COUNTER   8
#define NEWEST_ROTATION  3
#define OLDER_COUNTER    7
#define OLDER_ROTATION   5

static struct SaveBlock2 sSb2;         // the "game data" of the newest slot
static struct SaveBlock2 sSb2Older;    // the older slot's SaveBlock2
static struct SaveBlock1 sSb1;
static struct PokemonStorage sStorage;
static u8 sHallOfFame[NUM_HOF_SECTORS * SECTOR_DATA_SIZE];
static u8 sV1Image[SAVE_V1_FLASH_SIZE];
static u8 sFileBuf[FLASH_PC_SIZE + 1];

static u32 Rnd(u32 *state)
{
    *state = *state * 1103515245u + 12345u;
    return *state >> 8;
}

static void FillPattern(void *dst, u32 size, u32 seed)
{
    u8 *p = dst;
    u32 i;

    for (i = 0; i < size; i++)
        p[i] = (u8)Rnd(&seed);
}

static u16 ChunkSize(u32 totalSize, u32 chunkNum)
{
    u32 start = chunkNum * SECTOR_DATA_SIZE;

    if (totalSize < start)
        return 0;
    return (totalSize - start > SECTOR_DATA_SIZE) ? SECTOR_DATA_SIZE : (u16)(totalSize - start);
}

static s32 FileSize(const char *path)
{
    FILE *file = fopen(path, "rb");
    long size;

    if (file == NULL)
        return -1;
    fseek(file, 0, SEEK_END);
    size = ftell(file);
    fclose(file);
    return (s32)size;
}

static bool8 WriteFile(const char *path, const void *data, u32 size)
{
    FILE *file = fopen(path, "wb");
    bool8 ok;

    if (file == NULL)
        return FALSE;
    ok = (size == 0 || fwrite(data, 1, size, file) == size);
    fclose(file);
    return ok;
}

static bool8 FileEquals(const char *path, const void *data, u32 size)
{
    FILE *file = fopen(path, "rb");
    size_t got;

    if (file == NULL)
        return FALSE;
    got = fread(sFileBuf, 1, sizeof(sFileBuf), file);
    fclose(file);
    return got == size && memcmp(sFileBuf, data, size) == 0;
}

static struct SaveSector *V1Sector(u32 index)
{
    return (struct SaveSector *)(sV1Image + index * SECTOR_SIZE);
}

static void V1WriteSector(u32 index, u16 id, const void *data, u16 size, u32 counter)
{
    struct SaveSector *sector = V1Sector(index);

    memset(sector, 0, SECTOR_SIZE);
    memcpy(sector->data, data, size);
    sector->id = id;
    sector->checksum = Save_SectorChecksum(sector->data, size);
    sector->signature = SECTOR_SIGNATURE;
    sector->counter = counter;
}

// Writes a format 1 slot: SaveBlock2, SaveBlock1 narrowed to the format 1 layout, the boxes.
static void V1WriteSlot(u32 slot, u16 rotation, u32 counter, const struct SaveBlock2 *sb2,
                        const struct SaveBlock1 *sb1, const struct PokemonStorage *storage)
{
    static u8 sSb1V1[SAVE_V1_SB1_SIZE];
    const u8 *src = (const u8 *)sb1;
    u32 base = slot * SAVE_V1_SECTORS_PER_SLOT;
    u32 n;

    memcpy(sSb1V1, src, SAVE_V1_SB1_FLAGS_OFFSET);
    memcpy(sSb1V1 + SAVE_V1_SB1_FLAGS_OFFSET, src + offsetof(struct SaveBlock1, flags), SAVE_V1_SB1_NUM_FLAG_BYTES);
    memcpy(sSb1V1 + SAVE_V1_SB1_VARS_OFFSET, src + offsetof(struct SaveBlock1, vars), SAVE_V1_SB1_VARS_BYTES);
    memcpy(sSb1V1 + SAVE_V1_SB1_TAIL_OFFSET, src + offsetof(struct SaveBlock1, gameStats), SAVE_V1_SB1_SIZE - SAVE_V1_SB1_TAIL_OFFSET);

#define V1_POS(id) (base + ((id) + rotation) % SAVE_V1_SECTORS_PER_SLOT)
    V1WriteSector(V1_POS(SECTOR_ID_SAVEBLOCK2), SECTOR_ID_SAVEBLOCK2, sb2, SAVE_V1_SB2_SIZE, counter);
    for (n = 0; n <= SAVE_V1_SECTOR_ID_SB1_END - SECTOR_ID_SAVEBLOCK1_START; n++)
        V1WriteSector(V1_POS(SECTOR_ID_SAVEBLOCK1_START + n), SECTOR_ID_SAVEBLOCK1_START + n,
                      sSb1V1 + n * SECTOR_DATA_SIZE, ChunkSize(SAVE_V1_SB1_SIZE, n), counter);
    for (n = 0; n < SAVE_V1_SECTORS_PER_SLOT - SAVE_V1_SECTOR_ID_PKMN_START; n++)
        V1WriteSector(V1_POS(SAVE_V1_SECTOR_ID_PKMN_START + n), SAVE_V1_SECTOR_ID_PKMN_START + n,
                      (const u8 *)storage + n * SECTOR_DATA_SIZE, ChunkSize(SAVE_V1_PKMN_STORAGE_SIZE, n), counter);
#undef V1_POS
}

// The game data: random bytes, except that what format 1 cannot store is zero.
static void BuildGameData(void)
{
    u8 *sb1 = (u8 *)&sSb1;
    u32 tailOld = SAVE_V1_SB1_SIZE - SAVE_V1_SB1_TAIL_OFFSET;
    u32 tailNew = sizeof(struct SaveBlock1) - offsetof(struct SaveBlock1, gameStats);

    FillPattern(&sSb2, sizeof(sSb2), 1);
    sSb2.saveFormatVersion = 0; // unused filler in format 1
    sSb2Older = sSb2;
    sSb2Older.playerTrainerId[0] ^= 0xFF;

    FillPattern(&sSb1, sizeof(sSb1), 2);
    memset(sb1 + offsetof(struct SaveBlock1, flags) + SAVE_V1_SB1_NUM_FLAG_BYTES, 0, NUM_FLAG_BYTES - SAVE_V1_SB1_NUM_FLAG_BYTES);
    memset(sb1 + offsetof(struct SaveBlock1, vars) + SAVE_V1_SB1_VARS_BYTES, 0, VARS_COUNT * sizeof(u16) - SAVE_V1_SB1_VARS_BYTES);
    memset(sb1 + offsetof(struct SaveBlock1, gameStats) + tailOld, 0, tailNew - tailOld);

    FillPattern(&sStorage, sizeof(sStorage), 3);
    FillPattern(sHallOfFame, sizeof(sHallOfFame), 4);
}

// A complete format 1 image: slot 1 is an older save, slot 2 the newest, plus the special sectors.
static void BuildV1Image(void)
{
    struct SaveSector *sector;
    u32 sentinel = SPECIAL_SECTOR_SENTINEL;
    u32 n;

    memset(sV1Image, 0xFF, sizeof(sV1Image));
    V1WriteSlot(NEWEST_COUNTER % NUM_SAVE_SLOTS, NEWEST_ROTATION, NEWEST_COUNTER, &sSb2, &sSb1, &sStorage);
    V1WriteSlot(OLDER_COUNTER % NUM_SAVE_SLOTS, OLDER_ROTATION, OLDER_COUNTER, &sSb2Older, &sSb1, &sStorage);

    // Hall of Fame: HandleWriteSectorNBytes puts the checksum in the id field.
    for (n = 0; n < NUM_HOF_SECTORS; n++)
    {
        sector = V1Sector(V1_SPECIAL_SECTOR_FIRST + n);
        memset(sector, 0, SECTOR_SIZE);
        memcpy(sector->data, sHallOfFame + n * SECTOR_DATA_SIZE, SECTOR_DATA_SIZE);
        sector->id = Save_SectorChecksum(sector->data, SECTOR_DATA_SIZE);
        sector->signature = SECTOR_SIGNATURE;
    }
    // Trainer Hill: the sentinel, then data (TryWriteSpecialSaveSector). Recorded Battle: erased.
    sector = V1Sector(V1_SPECIAL_SECTOR_FIRST + NUM_HOF_SECTORS);
    FillPattern(sector, SECTOR_SIZE, 5);
    memcpy(sector->data, &sentinel, sizeof(sentinel));
}

static void ResetGameState(void)
{
    IdentifyFlash();
    gFlashMemoryPresent = TRUE;
    SetSaveBlocksPointers(0);
    memset(gSaveBlock2Ptr, 0, sizeof(*gSaveBlock2Ptr));
    memset(gSaveBlock1Ptr, 0, sizeof(*gSaveBlock1Ptr));
    memset(gPokemonStoragePtr, 0, sizeof(*gPokemonStoragePtr));
    Save_ResetSaveCounters();
}

static bool8 FlashIsErased(void)
{
    u32 i;

    for (i = 0; i < FLASH_PC_SIZE; i++)
    {
        if (FLASH_BASE[i] != 0xFF)
            return FALSE;
    }
    return TRUE;
}

// `path` must be gPlatformArgs.savePath (Platform_StoreSaveFile writes there).
u32 SaveSelftest_Migration(const char *path)
{
    char bakPath[PLATFORM_ARG_PATH_SIZE + 32];
    struct SaveBlock2 expectedSb2;
    const struct SaveSector *sector;
    static u8 sSpecial[SECTOR_SIZE];
    u32 n;
    u16 oldTopFlag = VANILLA_FLAGS_COUNT - 1;
    u16 oldTopVar = VANILLA_VARS_END;
    bool8 oldTopFlagValue;
    u16 oldTopVarValue;

    sFailures = 0;
    snprintf(bakPath, sizeof(bakPath), "%s.v1.bak", path);
    remove(bakPath);

    // The TRAINER_FLAG() mapping: the vanilla block, then the extended block, no overlap.
    MG_CHECK(TRAINER_FLAG(0) == TRAINER_FLAGS_START);
    MG_CHECK(TRAINER_FLAG(MAX_TRAINERS_COUNT - 1) == TRAINER_FLAGS_END);
    MG_CHECK(TRAINER_FLAG(MAX_TRAINERS_COUNT) == TRAINER_FLAGS_EXT_START);
    MG_CHECK(TRAINER_FLAG(MAX_TRAINERS_TOTAL - 1) == TRAINER_FLAGS_EXT_END);
    MG_CHECK(TRAINER_FLAGS_EXT_START == VANILLA_FLAGS_COUNT);
    MG_CHECK(EVENT_FLAGS_EXT_END + 1 == FLAGS_COUNT && FLAGS_COUNT < SPECIAL_FLAGS_START);
    MG_CHECK(VARS_END - VARS_START + 1 == VARS_COUNT && VARS_COUNT >= 1024);
    MG_CHECK(sizeof(FLASH_BASE) == SECTORS_COUNT * SECTOR_SIZE);

    // 1. Migration of a synthetic format 1 file: the game sees the newest slot, unchanged.
    BuildGameData();
    BuildV1Image();
    oldTopFlagValue = (sSb1.flags[oldTopFlag / 8] >> (oldTopFlag % 8)) & 1;
    oldTopVarValue = sSb1.vars[oldTopVar - VARS_START];
    MG_CHECK(WriteFile(path, sV1Image, sizeof(sV1Image)));
    ResetGameState();
    memset(FLASH_BASE, 0x55, sizeof(FLASH_BASE));
    MG_CHECK(Platform_LoadSaveFile(path));
    MG_CHECK(LoadGameSave(SAVE_NORMAL) == SAVE_STATUS_OK);
    MG_CHECK(gSaveCounter == NEWEST_COUNTER);
    MG_CHECK(gLastWrittenSector == NEWEST_ROTATION);
    expectedSb2 = sSb2;
    expectedSb2.saveFormatVersion = SAVE_FORMAT_VERSION;
    MG_CHECK(memcmp(gSaveBlock2Ptr, &expectedSb2, sizeof(expectedSb2)) == 0);
    MG_CHECK(memcmp(gSaveBlock1Ptr, &sSb1, sizeof(sSb1)) == 0);
    MG_CHECK(memcmp(gPokemonStoragePtr, &sStorage, sizeof(sStorage)) == 0);
    MG_CHECK(FlagGet(oldTopFlag) == oldTopFlagValue);
    MG_CHECK(VarGet(oldTopVar) == oldTopVarValue);
    MG_CHECK(!FlagGet(FLAGS_COUNT - 1) && VarGet(VARS_END) == 0); // the new space starts out clear
    // The older slot migrated too: its SaveBlock2 sits at its rotated position with its counter.
    sector = (const struct SaveSector *)(FLASH_BASE + ((OLDER_COUNTER % NUM_SAVE_SLOTS) * NUM_SECTORS_PER_SLOT
                                                       + (SECTOR_ID_SAVEBLOCK2 + OLDER_ROTATION) % NUM_SECTORS_PER_SLOT) * SECTOR_SIZE);
    MG_CHECK(sector->id == SECTOR_ID_SAVEBLOCK2 && sector->counter == OLDER_COUNTER && sector->signature == SECTOR_SIGNATURE);
    MG_CHECK(sector->data[offsetof(struct SaveBlock2, playerTrainerId)] == sSb2Older.playerTrainerId[0]);
    // The special sectors moved as they were.
    for (n = 0; n < SECTORS_COUNT - SECTOR_ID_HOF_1; n++)
        MG_CHECK(memcmp(FLASH_BASE + (SECTOR_ID_HOF_1 + n) * SECTOR_SIZE, sV1Image + (V1_SPECIAL_SECTOR_FIRST + n) * SECTOR_SIZE, SECTOR_SIZE) == 0);
    MG_CHECK(LoadGameSave(SAVE_HALL_OF_FAME) == SAVE_STATUS_OK && memcmp(gDecompressionBuffer, sHallOfFame, sizeof(sHallOfFame)) == 0);
    MG_CHECK(TryReadSpecialSaveSector(SECTOR_ID_TRAINER_HILL, sSpecial) == SAVE_STATUS_OK
          && memcmp(sSpecial, sV1Image + (V1_SPECIAL_SECTOR_FIRST + NUM_HOF_SECTORS) * SECTOR_SIZE + 4, 64) == 0);
    MG_CHECK(TryReadSpecialSaveSector(SECTOR_ID_RECORDED_BATTLE, sSpecial) == SAVE_STATUS_ERROR); // erased
    // Nothing on disk changed yet.
    MG_CHECK(FileEquals(path, sV1Image, sizeof(sV1Image)));
    MG_CHECK(FileSize(bakPath) < 0);
    printf("[save] selftest: migration: format 1 image loaded, %s\n", sFailures ? "FAILED" : "OK");

    // 2. The first save: backup once, then the save in format 2. Flags and vars at the ends of
    //    the old and the new ranges survive.
    FlagSet(FLAGS_COUNT - 1);
    FlagSet(EVENT_FLAGS_EXT_START);
    FlagSet(SYSTEM_FLAGS_EXT_START);
    FlagSet(TRAINER_FLAG(MAX_TRAINERS_TOTAL - 1));
    FlagSet(TRAINER_FLAG(MAX_TRAINERS_COUNT));
    VarSet(VARS_END, 0xBEEF);
    VarSet(VARS_EXT_START, 0x1234);
    MG_CHECK(TrySavingData(SAVE_NORMAL) == SAVE_STATUS_OK);
    MG_CHECK(gSaveCounter == NEWEST_COUNTER + 1);
    MG_CHECK(FileSize(path) == (s32)sizeof(FLASH_BASE));
    MG_CHECK(FileEquals(path, FLASH_BASE, sizeof(FLASH_BASE)));
    MG_CHECK(FileEquals(bakPath, sV1Image, sizeof(sV1Image)));
    // Round trip of the format 2 file.
    memset(FLASH_BASE, 0, sizeof(FLASH_BASE));
    memset(gSaveBlock1Ptr, 0, sizeof(*gSaveBlock1Ptr));
    memset(gSaveBlock2Ptr, 0, sizeof(*gSaveBlock2Ptr));
    MG_CHECK(Platform_LoadSaveFile(path));
    MG_CHECK(LoadGameSave(SAVE_NORMAL) == SAVE_STATUS_OK);
    MG_CHECK(gSaveCounter == NEWEST_COUNTER + 1);
    MG_CHECK(gSaveBlock2Ptr->saveFormatVersion == SAVE_FORMAT_VERSION);
    MG_CHECK(memcmp(gPokemonStoragePtr, &sStorage, sizeof(sStorage)) == 0);
    MG_CHECK(FlagGet(oldTopFlag) == oldTopFlagValue && VarGet(oldTopVar) == oldTopVarValue);
    MG_CHECK(FlagGet(FLAGS_COUNT - 1) && FlagGet(EVENT_FLAGS_EXT_START) && FlagGet(SYSTEM_FLAGS_EXT_START));
    MG_CHECK(FlagGet(TRAINER_FLAG(MAX_TRAINERS_TOTAL - 1)) && FlagGet(TRAINER_FLAG(MAX_TRAINERS_COUNT)));
    MG_CHECK(!FlagGet(TRAINER_FLAG(MAX_TRAINERS_COUNT - 1)) && !FlagGet(EVENT_FLAGS_EXT_START + 1));
    MG_CHECK(VarGet(VARS_END) == 0xBEEF && VarGet(VARS_EXT_START) == 0x1234 && VarGet(VARS_EXT_START + 1) == 0);
    MG_CHECK(FlagGet(FLAGS_COUNT) == FALSE && VarGet(VARS_END + 1) == VARS_END + 1); // out of range: no flag, "immediate" var
    // A later save does not write the backup again (it belongs to the migration, not to saving).
    remove(bakPath);
    MG_CHECK(TrySavingData(SAVE_NORMAL) == SAVE_STATUS_OK);
    MG_CHECK(FileSize(bakPath) < 0);
    printf("[save] selftest: migration: first save, backup and format 2 round trip, %s\n", sFailures ? "FAILED" : "OK");

    // 3. Files of any other size are refused, and the flash in memory is left alone.
    FillPattern(sFileBuf, sizeof(sFileBuf), 6);
    memset(FLASH_BASE, 0x5A, sizeof(FLASH_BASE));
    MG_CHECK(WriteFile(path, sFileBuf, 100000) && !Platform_LoadSaveFile(path));
    MG_CHECK(WriteFile(path, sFileBuf, SAVE_V1_FLASH_SIZE + 1) && !Platform_LoadSaveFile(path));
    MG_CHECK(WriteFile(path, sFileBuf, SAVE_V1_FLASH_SIZE - 1) && !Platform_LoadSaveFile(path));
    MG_CHECK(WriteFile(path, sFileBuf, sizeof(FLASH_BASE) + 1) && !Platform_LoadSaveFile(path));
    MG_CHECK(FLASH_BASE[0] == 0x5A && FLASH_BASE[sizeof(FLASH_BASE) - 1] == 0x5A && FLASH_BASE[SAVE_V1_FLASH_SIZE] == 0x5A);
    printf("[save] selftest: migration: wrong sizes refused, %s\n", sFailures ? "FAILED" : "OK");

    // 4. A missing or empty file and an erased format 1 file load as erased flash; saving a
    //    game on an erased format 1 file makes no backup (there was nothing to keep).
    remove(path);
    remove(bakPath);
    MG_CHECK(Platform_LoadSaveFile(path) && FlashIsErased());
    MG_CHECK(FileSize(path) == 0); // created, as before
    memset(FLASH_BASE, 0x5A, sizeof(FLASH_BASE));
    MG_CHECK(Platform_LoadSaveFile(path) && FlashIsErased()); // empty file
    memset(sFileBuf, 0xFF, SAVE_V1_FLASH_SIZE);
    memset(FLASH_BASE, 0x5A, sizeof(FLASH_BASE));
    MG_CHECK(WriteFile(path, sFileBuf, SAVE_V1_FLASH_SIZE) && Platform_LoadSaveFile(path) && FlashIsErased());
    ResetGameState();
    MG_CHECK(LoadGameSave(SAVE_NORMAL) == SAVE_STATUS_EMPTY);
    MG_CHECK(TrySavingData(SAVE_NORMAL) == SAVE_STATUS_OK);
    MG_CHECK(FileSize(path) == (s32)sizeof(FLASH_BASE));
    MG_CHECK(FileSize(bakPath) < 0);
    printf("[save] selftest: migration: empty and erased files, %s\n", sFailures ? "FAILED" : "OK");

    remove(bakPath);
    printf("[save] selftest: migration (format 1 -> 2, backup, refused sizes, new flag/var ranges) %s\n", sFailures ? "FAILED" : "OK");
    return sFailures;
}
