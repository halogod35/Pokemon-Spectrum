// MP: BUG-003. --save-selftest group "link save": the trade's two-phase save sequence
// (CB2_SaveAndEndTrade states 50..42: LinkFullSave_Init, the WriteSector loop,
// ReplaceLastSector, SetLastSectorSignature) on a real save, so CI runs it (under gdb in the
// boot smoke job too). Checks:
//   - a crash before the signature leaves the previous save as the valid one (in memory and on disk);
//   - after the signature the new save is valid in memory AND written to disk (BUG-003: the
//     link save path never stored the file on PC).
#include <stdio.h>
#include <string.h>
#include "global.h"
#include "agb_flash.h"
#include "gba/flash_internal.h"
#include "load_save.h"
#include "overworld.h"
#include "platform.h"
#include "save.h"
#include "constants/game_stat.h"

static u32 sFailures;

#define LS_CHECK(cond)                                                                  \
    do                                                                                  \
    {                                                                                   \
        if (!(cond))                                                                    \
        {                                                                               \
            printf("[save] selftest: FAIL: link save: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
            sFailures++;                                                                \
        }                                                                               \
    } while (0)

static s32 ReadWholeFile(const char *path, u8 *buf, u32 cap)
{
    FILE *f = fopen(path, "rb");
    size_t got;

    if (f == NULL)
        return -1;
    got = fread(buf, 1, cap, f);
    fclose(f);
    return (s32)got;
}

static bool8 FileMatchesFlash(const char *path)
{
    static u8 sBuf[sizeof(FLASH_BASE)];
    s32 len = ReadWholeFile(path, sBuf, sizeof(sBuf));

    return len == (s32)sizeof(FLASH_BASE) && memcmp(sBuf, FLASH_BASE, sizeof(FLASH_BASE)) == 0;
}

static bool8 LoadFileIntoFlash(const char *path)
{
    return ReadWholeFile(path, FLASH_BASE, sizeof(FLASH_BASE)) == (s32)sizeof(FLASH_BASE);
}

// Runs the trade save up to (signed == FALSE) or including the signature.
static void RunLinkSave(bool8 sign)
{
    u32 n;

    LS_CHECK(!LinkFullSave_Init());
    for (n = 0; n < 64 && !LinkFullSave_WriteSector(); n++)
        ;
    LS_CHECK(n < 64);
    LinkFullSave_ReplaceLastSector();
    if (sign)
        LinkFullSave_SetLastSectorSignature();
    LS_CHECK(gDamagedSaveSectors == 0);
}

// `path` must be gPlatformArgs.savePath (Platform_StoreSaveFile writes there).
u32 SaveSelftest_LinkSave(const char *path)
{
    u32 counter;

    sFailures = 0;
    memset(FLASH_BASE, 0xFF, sizeof(FLASH_BASE));
    IdentifyFlash();
    gFlashMemoryPresent = TRUE;
    SetSaveBlocksPointers(0);
    memset(gSaveBlock2Ptr, 0, sizeof(*gSaveBlock2Ptr));
    memset(gSaveBlock1Ptr, 0, sizeof(*gSaveBlock1Ptr));
    gSaveBlock2Ptr->playerTrainerId[0] = 1;  // the "pre-trade" marker

    // A normal save first (as MpTrade_Start does before linking).
    LS_CHECK(TrySavingData(SAVE_NORMAL) == SAVE_STATUS_OK);
    LS_CHECK(FileMatchesFlash(path));
    LS_CHECK(LoadGameSave(SAVE_NORMAL) == SAVE_STATUS_OK && gSaveBlock2Ptr->playerTrainerId[0] == 1);
    counter = gSaveCounter;

    // The trade, state 50: the stats it bumps, then the sectors; crash before the signature.
    gSaveBlock2Ptr->playerTrainerId[0] = 2;  // the "post-trade" marker
    IncrementGameStat(GAME_STAT_POKEMON_TRADES);
    RunLinkSave(FALSE);
    LS_CHECK(LoadGameSave(SAVE_NORMAL) == SAVE_STATUS_OK && gSaveBlock2Ptr->playerTrainerId[0] == 1 && gSaveCounter == counter);
    LS_CHECK(LoadFileIntoFlash(path) && LoadGameSave(SAVE_NORMAL) == SAVE_STATUS_OK && gSaveBlock2Ptr->playerTrainerId[0] == 1);

    // The trade again, this time signed: valid in memory and on disk.
    gSaveBlock2Ptr->playerTrainerId[0] = 2;
    RunLinkSave(TRUE);
    LS_CHECK(FileMatchesFlash(path));
    LS_CHECK(LoadGameSave(SAVE_NORMAL) == SAVE_STATUS_OK && gSaveBlock2Ptr->playerTrainerId[0] == 2 && gSaveCounter == counter + 1);
    memset(FLASH_BASE, 0, sizeof(FLASH_BASE));
    LS_CHECK(LoadFileIntoFlash(path) && LoadGameSave(SAVE_NORMAL) == SAVE_STATUS_OK && gSaveBlock2Ptr->playerTrainerId[0] == 2);

    // And a normal save after it still works (the slots alternate correctly).
    gSaveBlock2Ptr->playerTrainerId[0] = 3;
    LS_CHECK(TrySavingData(SAVE_NORMAL) == SAVE_STATUS_OK && FileMatchesFlash(path));
    LS_CHECK(LoadGameSave(SAVE_NORMAL) == SAVE_STATUS_OK && gSaveBlock2Ptr->playerTrainerId[0] == 3);

    printf("[save] selftest: link save (trade two-phase save, crash before / after the signature) %s\n", sFailures ? "FAILED" : "OK");
    return sFailures;
}
