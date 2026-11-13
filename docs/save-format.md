# Save format (PC build)

Pokémon Spectrum keeps Emerald's save system (`src/save.c`: 4 KB flash sectors with a 128-byte
footer, two alternating save slots, per-sector checksums) and mirrors the whole flash to one file,
`pokemon-spectrum.sav` (`--save <path>` to choose another). This page documents **save format 2**
(PS-070), the differences from the vanilla layout (**format 1**, written by v0.2.0 and earlier),
and how old files are migrated.

## Files next to the save

| File | What |
|---|---|
| `<save>` | The flash image, 147456 bytes (36 sectors × 4096). Format 1 files are 131072 bytes (32 sectors). |
| `<save>.tmp` | Crash-safe write in progress (PS-014). Never loaded; the next save replaces it. |
| `<save>.v1.bak` | The untouched format 1 file, written once by the first save after a migration (see below). |
| `<save>.mp.cfg` | Last Join Game address (PS-021). |
| `<save>.trade` | Pending trade journal (PS-050). Bound to the save by trainer id, independent of the save format. |

## Flash layout (format 2)

| Sectors | Content |
|---|---|
| 0–15 | Save slot 1 (16 sectors) |
| 16–31 | Save slot 2 |
| 32–33 | Hall of Fame |
| 34 | Trainer Hill (e-Reader challenge) |
| 35 | Recorded Battle |

A slot holds the sector ids 0–15, rotated by `gLastWrittenSector` (the game derives the rotation
from where id 0 sits). Save counter N goes to slot N % 2.

| Sector id (in slot) | Data | Size used |
|---|---|---|
| 0 | `struct SaveBlock2` | 0xF2C of 3968 bytes |
| 1–6 | `struct SaveBlock1` (0x505C bytes, 6 chunks of 3968) | 20572 of 23808 bytes (3236 spare) |
| 7–15 | `struct PokemonStorage` (0x83D0 bytes, 9 chunks) | 33744 of 35712 bytes |

Format 1 had SaveBlock1 in 4 sectors (ids 1–4, 0x3D88 bytes), the boxes in ids 5–13, 14 sectors per
slot, slot 2 at sectors 14–27 and the special sectors at 28–31.

Constants: `include/save.h` (`SECTOR_ID_*`, `NUM_SECTORS_PER_SLOT`, `SECTORS_COUNT`,
`SAVE_FORMAT_VERSION`, `SAVE_V1_*`), `include/gba/flash_internal.h` (`FLASH_PC_SECTORS`,
`FLASH_PC_SIZE`). `src/save.c` static-asserts that they agree with each other and with the struct sizes.

`SaveBlock2.saveFormatVersion` (offset 0x90, unused filler before) is written as `SAVE_FORMAT_VERSION`
(2) by every save. Format 1 files have 0 there. The loader does not depend on it (the file size is
what decides), but a future format 3 can.

## Flags (`SaveBlock1.flags`, `FLAGS_COUNT` = 0x3000 bits = 1536 bytes; was 0x960 / 300 bytes)

Append only: every vanilla flag id is unchanged. `include/constants/flags.h`.

| Range | Flags | Count | Use |
|---|---|---|---|
| 0x000–0x01F | `FLAG_TEMP_*` | 32 | cleared on every map load (vanilla) |
| 0x020–0x4FF | event flags | 1248 | Hoenn scripts, hidden items, object events (vanilla) |
| 0x500–0x85F | trainer flags, trainer ids 0–863 | 864 | `TRAINER_FLAG(id)` (vanilla block) |
| 0x860–0x91F | `FLAG_SYS_*`, badges, landmarks, visited towns | 192 | vanilla system flags |
| 0x920–0x95F | `FLAG_DAILY_*` | 64 | cleared daily (vanilla) |
| 0x960–0xDFF | `TRAINER_FLAGS_EXT_*`, trainer ids 864–2047 | 1184 | **new**: `TRAINER_FLAG(id)` for id ≥ `MAX_TRAINERS_COUNT` |
| 0xE00–0xFFF | `SYSTEM_FLAGS_EXT_START + n` | 512 | **new**: engine flags of new regions (badges, visited, landmarks, Fly) |
| 0x1000–0x2FFF | `EVENT_FLAGS_EXT_START + n` | 8192 | **new**: event flags of new regions |
| 0x4000–0x407F | `FLAG_SPECIAL_*` | 128 | RAM only, not saved (vanilla) |

Trainer flags: the vanilla block and the extended block are not contiguous, so code must map a
trainer id with `TRAINER_FLAG(trainerId)` (`src/battle_setup.c` does; scripts go through
`HasTrainerBeenFought` / `SetTrainerFlag` / `ClearTrainerFlag`). `MAX_TRAINERS_TOTAL` is 2048;
`MAX_TRAINERS_COUNT` (864, `include/constants/opponents.h`) must not change, it fixes the vanilla
block. Adding trainers 864+ also needs `gTrainers[]` to grow (not part of PS-070).

`FlagGet/Set/Clear` ignore ids in 0x3000–0x3FFF and above 0x407F (they used to index out of bounds).

## Vars (`SaveBlock1.vars`, `VARS_COUNT` = 2048 × u16 = 4096 bytes; was 256 / 512 bytes)

| Range | Vars | Use |
|---|---|---|
| 0x4000–0x400F | `VAR_TEMP_*` | cleared on every map load (vanilla) |
| 0x4010–0x40FF | vanilla vars | Hoenn (`VANILLA_VARS_END` = 0x40FF) |
| 0x4100–0x47FF | `VARS_EXT_START + n` | **new**: vars of new regions |
| 0x8000–0x8015 | special vars | RAM only (vanilla) |

`VarGet` returns the id itself (the "immediate value" convention) for ids in 0x4800–0x7FFF and
above 0x8015 instead of indexing out of bounds.

## SaveBlock1 layout change

Only `flags[]` (0x12C → 0x600 bytes) and `vars[]` (0x200 → 0x1000 bytes) grew, in place, at offset
0x1270. Everything before `flags` is byte-identical to format 1; everything from `gameStats` on is
the same bytes shifted by 0x12D4. The offset comments in `include/global.h` are the format 2 ones.
`src/save_migrate.c` static-asserts these assumptions, so a later change to SaveBlock1 that breaks
them fails the build instead of the migration.

Growing SaveBlock1 or SaveBlock2 **at the end** within their sectors needs no new format: the
loader zero-fills sectors before writing and the checksum is a word sum, so old files keep
validating and the new fields read as zero. Any other change (new sectors, fields in the middle)
needs a format 3 with its own migration.

## Migration of format 1 files (v0.2.0 and earlier)

Done by `src/platform/sdl2.c` (`ReadSaveFile`) and `src/save_migrate.c` (`Save_MigrateV1Flash`),
after the PS-056 rename of `pokeemerald.*` files and before anything reads the save:

1. The file size decides: 0 bytes = new game; 131072 = format 1; 147456 = format 2; anything
   else is refused (the game exits with a `[save]` message and never overwrites the file).
2. A format 1 image is converted **in memory**, sector by sector, using the footers the game
   wrote (id, checksum, signature, counter). Only sectors with a valid signature and checksum are
   converted; if an id occurs twice (interrupted save), the higher counter wins. Per slot:
   SaveBlock2 is copied (plus `saveFormatVersion` = 2, re-checksummed); SaveBlock1 is reassembled
   from its 4 chunks, widened (new flag and var space zero), and re-sliced into 6 chunks; the 9
   box sectors are copied with ids 7–15. Counters and the in-slot rotation are kept, so the game
   picks the slot it would have picked before. A slot that was incomplete in format 1 stays
   incomplete (its SaveBlock1 chunks are dropped); an erased slot stays erased. Hall of Fame,
   Trainer Hill and Recorded Battle are copied as they are to sectors 32–35.
3. The original 128 KB bytes are kept in memory. The **first write** of the save (any in-game save,
   trade save, link save or "clear save data") first writes them crash-safely to `<save>.v1.bak`,
   then writes the save in format 2. An existing `.v1.bak` is kept, not overwritten. If the
   backup cannot be written, a warning is logged and the save is written anyway. An all-erased
   format 1 file (no save data) gets no backup.
4. The trade journal (`<save>.trade`) is read after the save is loaded (`MP_TradeJournalOnLoad`),
   so it works on the migrated data; it needs no conversion.

Console output: `[save] migrated '<save>' from save format 1 (128 KB) to format 2 (144 KB) in
memory, N sectors (backup: '<save>.v1.bak', written before the first save)`, then on the first
save `[save] backup of the format 1 save written (backup: '<save>.v1.bak')`.

To go back to v0.2.0: quit, rename `<save>.v1.bak` to `<save>` (progress made in format 2 is lost).

## Tests

`--save-selftest` (CI, Linux) runs, on `<save>.selftest`, the PS-014 I/O checks, the BUG-003 link
save, and the PS-070 group `migration` (`src/save_migrate_selftest.c`): a synthetic format 1 file
with two slots (different counters and rotations), Hall of Fame and Trainer Hill migrates and the
game loads byte-identical data from the newest slot; nothing on disk changes until the first save,
which writes the `.v1.bak` (byte-identical to the original) and the 144 KB save; a format 2 round
trip with flags and vars at both ends of the vanilla and the new ranges; files of other sizes are
refused with the in-memory flash untouched; empty and erased files load as a new game without a
backup.
