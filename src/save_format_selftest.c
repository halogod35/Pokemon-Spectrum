// PS-070: --save-selftest group "format". Runs in CI. Checks, on "<save>.selftest":
//   - the TRAINER_FLAG() mapping and the flag / var range constants agree with each other;
//   - a save format 2 round trip (TrySavingData, then the file loaded again from disk) keeps
//     flags and vars at both ends of the vanilla and the new ranges;
//   - files of any other size are refused and FLASH_BASE is left alone;
//   - a file of the right size whose SaveBlock2 carries another save format version is refused
//     (saves from earlier versions are not supported: no migration until 1.0.0);
//   - a missing / empty file and an erased file load as an empty flash, a new game.
#include <stdio.h>
#include <string.h>
#include "global.h"
#include "agb_flash.h"
#include "gba/flash_internal.h"
#include "event_data.h"
#include "load_save.h"
#include "platform.h"
#include "save.h"
#include "constants/flags.h"
#include "constants/vars.h"

static u32 sFailures;

#define SF_CHECK(cond)                                                                      \
    do                                                                                      \
    {                                                                                       \
        if (!(cond))                                                                        \
        {                                                                                   \
            printf("[save] selftest: FAIL: format: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
            sFailures++;                                                                    \
        }                                                                                   \
    } while (0)

#define OLD_FLASH_SIZE (32 * SECTOR_SIZE) // the v0.2.0 (save format 1) file size, 131072

static u8 sFileBuf[FLASH_PC_SIZE + 1];

static void FillPattern(void *dst, u32 size, u32 seed)
{
    u8 *p = dst;
    u32 i;

    for (i = 0; i < size; i++)
    {
        seed = seed * 1103515245u + 12345u;
        p[i] = (u8)(seed >> 8);
    }
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

// Rewrites the save format version in every valid SaveBlock2 sector of the flash image (both
// slots), re-checksummed, so the file reads as written by another version of the game.
static u32 StampFlashFormatVersion(u8 version)
{
    struct SaveSector *sector;
    u32 i;
    u32 stamped = 0;

    for (i = 0; i < NUM_SECTORS_PER_SLOT * NUM_SAVE_SLOTS; i++)
    {
        sector = (struct SaveSector *)(FLASH_BASE + i * SECTOR_SIZE);
        if (sector->id != SECTOR_ID_SAVEBLOCK2 || sector->signature != SECTOR_SIGNATURE)
            continue;
        sector->data[offsetof(struct SaveBlock2, saveFormatVersion)] = version;
        sector->checksum = Save_SectorChecksum(sector->data, sizeof(struct SaveBlock2));
        stamped++;
    }
    return stamped;
}

// `path` must be gPlatformArgs.savePath (Platform_StoreSaveFile writes there).
u32 SaveSelftest_Format(const char *path)
{
    u16 oldTopFlag = VANILLA_FLAGS_COUNT - 1;
    u16 oldTopVar = VANILLA_VARS_END;

    sFailures = 0;

    // 1. The TRAINER_FLAG() mapping: the vanilla block, then the extended block, no overlap;
    //    the range constants agree with the struct and the flash.
    SF_CHECK(TRAINER_FLAG(0) == TRAINER_FLAGS_START);
    SF_CHECK(TRAINER_FLAG(MAX_TRAINERS_COUNT - 1) == TRAINER_FLAGS_END);
    SF_CHECK(TRAINER_FLAG(MAX_TRAINERS_COUNT) == TRAINER_FLAGS_EXT_START);
    SF_CHECK(TRAINER_FLAG(MAX_TRAINERS_TOTAL - 1) == TRAINER_FLAGS_EXT_END);
    SF_CHECK(TRAINER_FLAGS_EXT_START == VANILLA_FLAGS_COUNT);
    SF_CHECK(EVENT_FLAGS_EXT_END + 1 == FLAGS_COUNT && FLAGS_COUNT < SPECIAL_FLAGS_START);
    SF_CHECK(VARS_END - VARS_START + 1 == VARS_COUNT && VARS_COUNT >= 1024);
    SF_CHECK(sizeof(FLASH_BASE) == SECTORS_COUNT * SECTOR_SIZE);
    memset(FLASH_BASE, 0xFF, sizeof(FLASH_BASE));
    SF_CHECK(Save_FlashFormatVersion() == SAVE_FORMAT_VERSION); // erased flash: nothing to disagree with
    printf("[save] selftest: format: flag and var ranges, %s\n", sFailures ? "FAILED" : "OK");

    // 2. Format 2 round trip: flags and vars at both ends of the vanilla and the new ranges
    //    survive a save, and loading the file again from disk.
    remove(path);
    memset(FLASH_BASE, 0xFF, sizeof(FLASH_BASE));
    ResetGameState();
    gSaveBlock2Ptr->playerTrainerId[0] = 7;
    FlagSet(oldTopFlag);
    FlagSet(FLAGS_COUNT - 1);
    FlagSet(EVENT_FLAGS_EXT_START);
    FlagSet(SYSTEM_FLAGS_EXT_START);
    FlagSet(TRAINER_FLAG(MAX_TRAINERS_TOTAL - 1));
    FlagSet(TRAINER_FLAG(MAX_TRAINERS_COUNT));
    VarSet(oldTopVar, 0xCAFE);
    VarSet(VARS_END, 0xBEEF);
    VarSet(VARS_EXT_START, 0x1234);
    SF_CHECK(TrySavingData(SAVE_NORMAL) == SAVE_STATUS_OK);
    SF_CHECK(gSaveBlock2Ptr->saveFormatVersion == SAVE_FORMAT_VERSION);
    SF_CHECK(Save_FlashFormatVersion() == SAVE_FORMAT_VERSION);
    SF_CHECK(FileSize(path) == (s32)sizeof(FLASH_BASE));
    SF_CHECK(FileEquals(path, FLASH_BASE, sizeof(FLASH_BASE)));
    memset(FLASH_BASE, 0, sizeof(FLASH_BASE));
    memset(gSaveBlock1Ptr, 0, sizeof(*gSaveBlock1Ptr));
    memset(gSaveBlock2Ptr, 0, sizeof(*gSaveBlock2Ptr));
    SF_CHECK(Platform_LoadSaveFile(path));
    SF_CHECK(LoadGameSave(SAVE_NORMAL) == SAVE_STATUS_OK);
    SF_CHECK(gSaveBlock2Ptr->playerTrainerId[0] == 7);
    SF_CHECK(gSaveBlock2Ptr->saveFormatVersion == SAVE_FORMAT_VERSION);
    SF_CHECK(FlagGet(oldTopFlag) && FlagGet(FLAGS_COUNT - 1) && FlagGet(EVENT_FLAGS_EXT_START) && FlagGet(SYSTEM_FLAGS_EXT_START));
    SF_CHECK(FlagGet(TRAINER_FLAG(MAX_TRAINERS_TOTAL - 1)) && FlagGet(TRAINER_FLAG(MAX_TRAINERS_COUNT)));
    SF_CHECK(!FlagGet(oldTopFlag - 1) && !FlagGet(TRAINER_FLAG(MAX_TRAINERS_COUNT - 1)) && !FlagGet(EVENT_FLAGS_EXT_START + 1));
    SF_CHECK(VarGet(oldTopVar) == 0xCAFE && VarGet(VARS_END) == 0xBEEF && VarGet(VARS_EXT_START) == 0x1234);
    SF_CHECK(VarGet(oldTopVar - 1) == 0 && VarGet(VARS_EXT_START + 1) == 0);
    SF_CHECK(FlagGet(FLAGS_COUNT) == FALSE && VarGet(VARS_END + 1) == VARS_END + 1); // out of range: no flag, "immediate" var
    printf("[save] selftest: format: format %u round trip with the new flag and var ranges, %s\n",
           (unsigned int)SAVE_FORMAT_VERSION, sFailures ? "FAILED" : "OK");

    // 3. Files of any other size are refused, and the flash in memory is left alone.
    FillPattern(sFileBuf, sizeof(sFileBuf), 6);
    memset(FLASH_BASE, 0x5A, sizeof(FLASH_BASE));
    SF_CHECK(WriteFile(path, sFileBuf, 100000) && !Platform_LoadSaveFile(path));
    SF_CHECK(WriteFile(path, sFileBuf, OLD_FLASH_SIZE) && !Platform_LoadSaveFile(path)); // a v0.2.0 save
    SF_CHECK(WriteFile(path, sFileBuf, OLD_FLASH_SIZE - 1) && !Platform_LoadSaveFile(path));
    SF_CHECK(WriteFile(path, sFileBuf, sizeof(FLASH_BASE) - 1) && !Platform_LoadSaveFile(path));
    SF_CHECK(WriteFile(path, sFileBuf, sizeof(FLASH_BASE) + 1) && !Platform_LoadSaveFile(path));
    SF_CHECK(FLASH_BASE[0] == 0x5A && FLASH_BASE[sizeof(FLASH_BASE) - 1] == 0x5A && FLASH_BASE[OLD_FLASH_SIZE] == 0x5A);
    printf("[save] selftest: format: wrong sizes refused, %s\n", sFailures ? "FAILED" : "OK");

    // 4. A file of the right size from another version of the game (its SaveBlock2 sectors say
    //    format 1, or a future 3) is refused too. The game's own save, re-stamped, is accepted.
    memset(FLASH_BASE, 0xFF, sizeof(FLASH_BASE));
    ResetGameState();
    gSaveBlock2Ptr->playerTrainerId[0] = 7;
    SF_CHECK(TrySavingData(SAVE_NORMAL) == SAVE_STATUS_OK);
    SF_CHECK(StampFlashFormatVersion(1) >= 1);
    SF_CHECK(Save_FlashFormatVersion() == 1);
    SF_CHECK(WriteFile(path, FLASH_BASE, sizeof(FLASH_BASE)) && !Platform_LoadSaveFile(path));
    SF_CHECK(StampFlashFormatVersion(SAVE_FORMAT_VERSION + 1) >= 1);
    SF_CHECK(WriteFile(path, FLASH_BASE, sizeof(FLASH_BASE)) && !Platform_LoadSaveFile(path));
    SF_CHECK(StampFlashFormatVersion(SAVE_FORMAT_VERSION) >= 1);
    SF_CHECK(WriteFile(path, FLASH_BASE, sizeof(FLASH_BASE)) && Platform_LoadSaveFile(path));
    SF_CHECK(LoadGameSave(SAVE_NORMAL) == SAVE_STATUS_OK && gSaveBlock2Ptr->playerTrainerId[0] == 7);
    printf("[save] selftest: format: other save format versions refused, %s\n", sFailures ? "FAILED" : "OK");

    // 5. A missing or empty file and an erased file load as erased flash: a new game.
    remove(path);
    memset(FLASH_BASE, 0x5A, sizeof(FLASH_BASE));
    SF_CHECK(Platform_LoadSaveFile(path) && FlashIsErased());
    SF_CHECK(FileSize(path) == 0); // created, as before
    memset(FLASH_BASE, 0x5A, sizeof(FLASH_BASE));
    SF_CHECK(Platform_LoadSaveFile(path) && FlashIsErased()); // empty file
    memset(sFileBuf, 0xFF, sizeof(FLASH_BASE));
    memset(FLASH_BASE, 0x5A, sizeof(FLASH_BASE));
    SF_CHECK(WriteFile(path, sFileBuf, sizeof(FLASH_BASE)) && Platform_LoadSaveFile(path) && FlashIsErased());
    ResetGameState();
    SF_CHECK(LoadGameSave(SAVE_NORMAL) == SAVE_STATUS_EMPTY);
    SF_CHECK(TrySavingData(SAVE_NORMAL) == SAVE_STATUS_OK);
    SF_CHECK(FileSize(path) == (s32)sizeof(FLASH_BASE));
    printf("[save] selftest: format: empty and erased files, %s\n", sFailures ? "FAILED" : "OK");

    printf("[save] selftest: format (format %u round trip, refused sizes and versions, new flag/var ranges) %s\n",
           (unsigned int)SAVE_FORMAT_VERSION, sFailures ? "FAILED" : "OK");
    return sFailures;
}
