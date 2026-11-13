// PS-070: in-memory migration of a save format 1 flash image (vanilla Emerald layout, 128 KB,
// written by Pokémon Spectrum v0.2.0 and earlier) to save format 2 (144 KB).
//
// What changed between the formats (see docs/save-format.md):
//   - struct SaveBlock1: flags[] grew from 0x12C to 0x600 bytes and vars[] from 0x200 to
//     0x1000 bytes. Everything before `flags` and everything from `gameStats` on is unchanged.
//     SaveBlock1 now needs 6 sectors instead of 4.
//   - A save slot has 16 sectors instead of 14 (ids 0 SaveBlock2, 1-6 SaveBlock1, 7-15 PC
//     boxes). Slot 2 starts at sector 16 (was 14); the Hall of Fame, Trainer Hill and Recorded
//     Battle sectors moved from 28-31 to 32-35.
//   - SaveBlock2.saveFormatVersion (unused filler before) is SAVE_FORMAT_VERSION.
//
// The conversion works sector by sector on the footers the game itself writes (id, checksum,
// signature, counter), so a slot that was unreadable in format 1 stays unreadable: only
// sectors with a valid signature and checksum are converted. The in-slot rotation and the
// save counters are kept, so the game picks the same slot it would have picked before.
//
// This file never touches the disk. src/platform/sdl2.c calls Save_MigrateV1Flash() after
// reading a 128 KB file into FLASH_BASE, and writes "<save>.v1.bak" before the first save.
#include <stdio.h>
#include <string.h>
#include "global.h"
#include "gba/flash_internal.h"
#include "save.h"

// The migration assumes that only flags[] and vars[] grew, in place.
STATIC_ASSERT(offsetof(struct SaveBlock1, flags) == SAVE_V1_SB1_FLAGS_OFFSET, SaveBlock1HeadUnchanged);
STATIC_ASSERT(offsetof(struct SaveBlock1, vars) == SAVE_V1_SB1_FLAGS_OFFSET + NUM_FLAG_BYTES, SaveBlock1VarsFollowFlags);
STATIC_ASSERT(offsetof(struct SaveBlock1, gameStats) == offsetof(struct SaveBlock1, vars) + VARS_COUNT * sizeof(u16), SaveBlock1TailFollowsVars);
STATIC_ASSERT(NUM_FLAG_BYTES >= SAVE_V1_SB1_NUM_FLAG_BYTES, SaveBlock1FlagsOnlyGrew);
STATIC_ASSERT(VARS_COUNT * sizeof(u16) >= SAVE_V1_SB1_VARS_BYTES, SaveBlock1VarsOnlyGrew);
STATIC_ASSERT(sizeof(struct SaveBlock1) - offsetof(struct SaveBlock1, gameStats) >= SAVE_V1_SB1_SIZE - SAVE_V1_SB1_TAIL_OFFSET, SaveBlock1TailNotShorter);
STATIC_ASSERT(sizeof(struct SaveBlock2) >= SAVE_V1_SB2_SIZE, SaveBlock2NotShorter);
STATIC_ASSERT(offsetof(struct SaveBlock2, saveFormatVersion) == 0x90, SaveFormatVersionOffset);
STATIC_ASSERT(sizeof(struct PokemonStorage) >= SAVE_V1_PKMN_STORAGE_SIZE, PokemonStorageNotShorter);
STATIC_ASSERT(SAVE_V1_FLASH_SIZE <= FLASH_PC_SIZE, FlashGrewOnly);

#define V1_SB1_CHUNKS   (SAVE_V1_SECTOR_ID_SB1_END - SECTOR_ID_SAVEBLOCK1_START + 1) // 4
#define V2_SB1_CHUNKS   (SECTOR_ID_SAVEBLOCK1_END - SECTOR_ID_SAVEBLOCK1_START + 1)  // 6
#define PKMN_CHUNKS     (SECTOR_ID_PKMN_STORAGE_END - SECTOR_ID_PKMN_STORAGE_START + 1) // 9
#define NUM_SPECIAL_SECTORS (SECTORS_COUNT - SECTOR_ID_HOF_1) // HoF 1/2, Trainer Hill, Recorded Battle

static u8 sOldFlash[SAVE_V1_FLASH_SIZE];   // the format 1 image, copied out of FLASH_BASE
static u8 sOldSaveBlock1[SAVE_V1_SB1_SIZE]; // SaveBlock1 reassembled from its 4 format 1 chunks
static struct SaveBlock1 sNewSaveBlock1;    // the converted SaveBlock1

static u16 ChunkSize(u32 totalSize, u32 chunkNum)
{
    u32 start = chunkNum * SECTOR_DATA_SIZE;

    if (totalSize < start)
        return 0;
    return (totalSize - start > SECTOR_DATA_SIZE) ? SECTOR_DATA_SIZE : (u16)(totalSize - start);
}

static const struct SaveSector *OldSector(u32 index)
{
    return (const struct SaveSector *)(sOldFlash + index * SECTOR_SIZE);
}

static struct SaveSector *NewSector(u32 index)
{
    return (struct SaveSector *)(FLASH_BASE + index * SECTOR_SIZE);
}

// Data size of a format 1 slot sector, by id (what its checksum covers).
static u16 OldSectorDataSize(u16 id)
{
    if (id == SECTOR_ID_SAVEBLOCK2)
        return ChunkSize(SAVE_V1_SB2_SIZE, 0);
    if (id <= SAVE_V1_SECTOR_ID_SB1_END)
        return ChunkSize(SAVE_V1_SB1_SIZE, id - SECTOR_ID_SAVEBLOCK1_START);
    return ChunkSize(SAVE_V1_PKMN_STORAGE_SIZE, id - SAVE_V1_SECTOR_ID_PKMN_START);
}

static bool8 OldSectorValid(const struct SaveSector *sector)
{
    if (sector->signature != SECTOR_SIGNATURE || sector->id >= SAVE_V1_SECTORS_PER_SLOT)
        return FALSE;
    return sector->checksum == Save_SectorChecksum(sector->data, OldSectorDataSize(sector->id));
}

// Writes one format 2 slot sector with a fresh footer.
static void WriteNewSector(u32 index, u16 id, const void *data, u16 size, u32 counter)
{
    struct SaveSector *sector = NewSector(index);

    memset(sector, 0, SECTOR_SIZE);
    memcpy(sector->data, data, size);
    sector->id = id;
    sector->checksum = Save_SectorChecksum(sector->data, size);
    sector->signature = SECTOR_SIGNATURE;
    sector->counter = counter;
}

// Position of sector `id` in format 2 slot `slot`, keeping the format 1 rotation.
static u32 NewPosition(u32 slot, u16 rotation, u16 id)
{
    return slot * NUM_SECTORS_PER_SLOT + (id + rotation) % NUM_SECTORS_PER_SLOT;
}

// Converts the 4 format 1 SaveBlock1 chunks into the 6 format 2 chunks.
static void ConvertSaveBlock1(const struct SaveSector *const *best, u32 slot, u16 rotation)
{
    u8 *dst = (u8 *)&sNewSaveBlock1;
    u32 oldTail = SAVE_V1_SB1_SIZE - SAVE_V1_SB1_TAIL_OFFSET;
    u32 newTail = sizeof(struct SaveBlock1) - offsetof(struct SaveBlock1, gameStats);
    u32 counter = best[SECTOR_ID_SAVEBLOCK1_START]->counter;
    u32 n;

    for (n = 0; n < V1_SB1_CHUNKS; n++)
        memcpy(sOldSaveBlock1 + n * SECTOR_DATA_SIZE,
               best[SECTOR_ID_SAVEBLOCK1_START + n]->data,
               ChunkSize(SAVE_V1_SB1_SIZE, n));

    // Head, flags, vars and tail; the new flag and var space reads as zero (unset).
    memset(dst, 0, sizeof(struct SaveBlock1));
    memcpy(dst, sOldSaveBlock1, SAVE_V1_SB1_FLAGS_OFFSET);
    memcpy(dst + offsetof(struct SaveBlock1, flags), sOldSaveBlock1 + SAVE_V1_SB1_FLAGS_OFFSET, SAVE_V1_SB1_NUM_FLAG_BYTES);
    memcpy(dst + offsetof(struct SaveBlock1, vars), sOldSaveBlock1 + SAVE_V1_SB1_VARS_OFFSET, SAVE_V1_SB1_VARS_BYTES);
    memcpy(dst + offsetof(struct SaveBlock1, gameStats), sOldSaveBlock1 + SAVE_V1_SB1_TAIL_OFFSET,
           (oldTail < newTail) ? oldTail : newTail);

    for (n = 0; n < V2_SB1_CHUNKS; n++)
        WriteNewSector(NewPosition(slot, rotation, SECTOR_ID_SAVEBLOCK1_START + n),
                       SECTOR_ID_SAVEBLOCK1_START + n,
                       dst + n * SECTOR_DATA_SIZE,
                       ChunkSize(sizeof(struct SaveBlock1), n),
                       counter);
}

// Converts one save slot. Returns the number of format 2 sectors written.
static u32 MigrateSlot(u32 slot)
{
    const struct SaveSector *best[SAVE_V1_SECTORS_PER_SLOT];
    u32 oldBase = slot * SAVE_V1_SECTORS_PER_SLOT;
    u32 written = 0;
    u16 rotation = 0;
    u32 i;
    u16 id;
    bool8 haveSaveBlock1 = TRUE;

    // The best sector for each id: valid, and the highest counter if an id occurs twice
    // (a save interrupted between two sector writes).
    memset(best, 0, sizeof(best));
    for (i = 0; i < SAVE_V1_SECTORS_PER_SLOT; i++)
    {
        const struct SaveSector *sector = OldSector(oldBase + i);

        if (!OldSectorValid(sector))
            continue;
        id = sector->id;
        if (best[id] == NULL || sector->counter > best[id]->counter)
            best[id] = sector;
        if (id == SECTOR_ID_SAVEBLOCK2)
            rotation = i; // the game derives the rotation from where id 0 sits
    }

    // SaveBlock2: same bytes, plus the format version (the field was unused filler).
    if (best[SECTOR_ID_SAVEBLOCK2] != NULL)
    {
        struct SaveSector *sector = NewSector(NewPosition(slot, rotation, SECTOR_ID_SAVEBLOCK2));

        memcpy(sector, best[SECTOR_ID_SAVEBLOCK2], SECTOR_SIZE);
        sector->data[offsetof(struct SaveBlock2, saveFormatVersion)] = SAVE_FORMAT_VERSION;
        sector->checksum = Save_SectorChecksum(sector->data, ChunkSize(sizeof(struct SaveBlock2), 0));
        written++;
    }

    // SaveBlock1: needs all 4 chunks; otherwise it cannot be re-sliced and is left out (the
    // slot was incomplete in format 1 too, and stays so).
    for (id = SECTOR_ID_SAVEBLOCK1_START; id <= SAVE_V1_SECTOR_ID_SB1_END; id++)
    {
        if (best[id] == NULL)
            haveSaveBlock1 = FALSE;
    }
    if (haveSaveBlock1)
    {
        ConvertSaveBlock1(best, slot, rotation);
        written += V2_SB1_CHUNKS;
    }

    // PC boxes: same bytes, new id (7-15 instead of 5-13). The checksum covers data only.
    for (i = 0; i < PKMN_CHUNKS; i++)
    {
        const struct SaveSector *old = best[SAVE_V1_SECTOR_ID_PKMN_START + i];
        struct SaveSector *sector;

        if (old == NULL)
            continue;
        sector = NewSector(NewPosition(slot, rotation, SECTOR_ID_PKMN_STORAGE_START + i));
        memcpy(sector, old, SECTOR_SIZE);
        sector->id = SECTOR_ID_PKMN_STORAGE_START + i;
        written++;
    }

    printf("[save] migration: slot %u: %u of %u sectors converted%s\n", (unsigned int)(slot + 1),
           (unsigned int)written, (unsigned int)NUM_SECTORS_PER_SLOT,
           (written == 0) ? " (empty)" : (written == NUM_SECTORS_PER_SLOT) ? "" : " (incomplete in format 1 too)");
    return written;
}

u32 Save_MigrateV1Flash(void)
{
    u32 migrated = 0;
    u32 slot;
    u32 i;

    memcpy(sOldFlash, FLASH_BASE, SAVE_V1_FLASH_SIZE);
    memset(FLASH_BASE, 0xFF, FLASH_PC_SIZE); // erased

    for (slot = 0; slot < NUM_SAVE_SLOTS; slot++)
        migrated += MigrateSlot(slot);

    // Hall of Fame (2 sectors), Trainer Hill, Recorded Battle: same bytes, new positions.
    // They are self-contained (their own signature or sentinel), so erased ones stay erased.
    for (i = 0; i < NUM_SPECIAL_SECTORS; i++)
    {
        const struct SaveSector *old = OldSector(SAVE_V1_SECTORS_PER_SLOT * NUM_SAVE_SLOTS + i);
        u32 sentinel;

        memcpy(NewSector(SECTOR_ID_HOF_1 + i), old, SECTOR_SIZE);
        memcpy(&sentinel, old->data, sizeof(sentinel));
        if (old->signature == SECTOR_SIGNATURE || sentinel == SPECIAL_SECTOR_SENTINEL)
            migrated++;
    }

    return migrated;
}
