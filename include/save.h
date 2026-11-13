#ifndef GUARD_SAVE_H
#define GUARD_SAVE_H

#include "main.h"

// Each 4 KiB flash sector contains 3968 bytes of actual data followed by a 128 byte footer.
// Only 12 bytes of the footer are used.
#define SECTOR_DATA_SIZE 3968
#define SECTOR_FOOTER_SIZE 128
#define SECTOR_SIZE (SECTOR_DATA_SIZE + SECTOR_FOOTER_SIZE)

#define NUM_SAVE_SLOTS 2

// If the sector's signature field is not this value then the sector is either invalid or empty.
#define SECTOR_SIGNATURE 0x8012025

#define SPECIAL_SECTOR_SENTINEL 0xB39D

// PS-070 (save format 2): SaveBlock1 grew from 4 to 6 sectors (more flags and vars), so a
// slot has 16 sectors and the PC flash 36 (144 KB). See docs/save-format.md. The format
// version, SAVE_FORMAT_VERSION, lives in gba/flash_internal.h; files of another version or
// size are refused by the PC loader (no migration until 1.0.0).
#define SECTOR_ID_SAVEBLOCK2          0
#define SECTOR_ID_SAVEBLOCK1_START    1
#define SECTOR_ID_SAVEBLOCK1_END      6
#define SECTOR_ID_PKMN_STORAGE_START  7
#define SECTOR_ID_PKMN_STORAGE_END   15
#define NUM_SECTORS_PER_SLOT         16
// Save Slot 1: 0-15;  Save Slot 2: 16-31
#define SECTOR_ID_HOF_1              32
#define SECTOR_ID_HOF_2              33
#define SECTOR_ID_TRAINER_HILL       34
#define SECTOR_ID_RECORDED_BATTLE    35
#define SECTORS_COUNT                36

#define NUM_HOF_SECTORS 2

#define SAVE_STATUS_EMPTY    0
#define SAVE_STATUS_OK       1
#define SAVE_STATUS_CORRUPT  2
#define SAVE_STATUS_NO_FLASH 4
#define SAVE_STATUS_ERROR    0xFF

// Special sector id value for certain save functions to
// indicate that no specific sector should be used.
#define FULL_SAVE_SLOT 0xFFFF

// SetDamagedSectorBits states
enum
{
    ENABLE,
    DISABLE,
    CHECK // unused
};

// Do save types
enum
{
    SAVE_NORMAL,
    SAVE_LINK, // Link / Battle Frontier
    SAVE_EREADER, // deprecated in Emerald
    SAVE_HALL_OF_FAME,
    SAVE_OVERWRITE_DIFFERENT_FILE,
    SAVE_HALL_OF_FAME_ERASE_BEFORE // unused
};

// A save sector location holds a pointer to the data for a particular sector
// and the size of that data. Size cannot be greater than SECTOR_DATA_SIZE.
struct SaveSectorLocation
{
    void *data;
    u16 size;
};

struct SaveSector
{
    u8 data[SECTOR_DATA_SIZE];
    u8 unused[SECTOR_FOOTER_SIZE - 12]; // Unused portion of the footer
    u16 id;
    u16 checksum;
    u32 signature;
    u32 counter;
}; // size is SECTOR_SIZE (0x1000)

#define SECTOR_SIGNATURE_OFFSET offsetof(struct SaveSector, signature)
#define SECTOR_COUNTER_OFFSET   offsetof(struct SaveSector, counter)

extern u16 gLastWrittenSector;
extern u32 gLastSaveCounter;
extern u16 gLastKnownGoodSector;
extern u64 gDamagedSaveSectors; // one bit per flash sector (PS-070: 36 sectors, so 64 bits)
extern u32 gSaveCounter;
extern struct SaveSector *gFastSaveSector;
extern u16 gIncrementalSectorId;
extern u16 gSaveFileStatus;
extern MainCallback gGameContinueCallback;
extern struct SaveSectorLocation gRamSaveSectorLocations[];

extern struct SaveSector gSaveDataBuffer;

void ClearSaveData(void);
void Save_ResetSaveCounters(void);
u8 HandleSavingData(u8 saveType);
u8 TrySavingData(u8 saveType);
bool8 LinkFullSave_Init(void);
bool8 LinkFullSave_WriteSector(void);
bool8 LinkFullSave_ReplaceLastSector(void);
bool8 LinkFullSave_SetLastSectorSignature(void);
bool8 WriteSaveBlock2(void);
bool8 WriteSaveBlock1Sector(void);
u8 LoadGameSave(u8 saveType);
u16 GetSaveBlocksPointersBaseOffset(void);
u32 TryReadSpecialSaveSector(u8 sector, u8 *dst);
u32 TryWriteSpecialSaveSector(u8 sector, u8 *src);
void Task_LinkFullSave(u8 taskId);
// PS-070: the sector checksum used by every save sector footer.
u16 Save_SectorChecksum(const void *data, u16 size);
// PS-070: the save format version of the flash image in FLASH_BASE (the newest valid
// SaveBlock2 sector's saveFormatVersion), or SAVE_FORMAT_VERSION if there is none (erased
// flash, a new game). The PC loader refuses a file whose version is not SAVE_FORMAT_VERSION.
u32 Save_FlashFormatVersion(void);
// --save-selftest group "format" (save_format_selftest.c). `path` must equal
// gPlatformArgs.savePath. Returns the number of failures.
u32 SaveSelftest_Format(const char *path);

// save_failed_screen.c
void DoSaveFailedScreen(u8 saveType);

#endif // GUARD_SAVE_H
