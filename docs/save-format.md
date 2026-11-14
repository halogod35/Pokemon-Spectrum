# Save format (PC build)

Pokémon Spectrum keeps Emerald's save system (`src/save.c`: 4 KB flash sectors with a 128-byte
footer, two alternating save slots, per-sector checksums) and mirrors the whole flash to one file,
`pokemon-spectrum.sav` (`--save <path>` to choose another). This page documents **save format 3**
(PS-079; format 2 was PS-070, written by v0.3.0) and what changed from the vanilla layout
(format 1, written by v0.2.0 and earlier).

Format 3 differs from format 2 only in `SaveBlock1`: the object event template is 0x20 bytes on
every build and the block has 4 bytes of tail padding, so the **same `.sav` loads on the 32- and
64-bit builds, Linux and Windows** (BUG-015). The flash layout and sector map are unchanged.

**Saves from earlier versions are not supported: start a new game.** There is no migration until
1.0.0 (PS-069 policy): the loader refuses any file that is not exactly one flash image of the
current format, prints a `[save]` message and exits without touching the file.

## Files next to the save

| File | What |
|---|---|
| `<save>` | The flash image, 147456 bytes (36 sectors × 4096). |
| `<save>.tmp` | Crash-safe write in progress (PS-014). Never loaded; the next save replaces it. |
| `<save>.mp.cfg` | Last Join Game address (PS-021). |
| `<save>.trade` | Pending trade journal (PS-050). Bound to the save by trainer id. |

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
| 1–6 | `struct SaveBlock1` (0x5270 bytes on every PC build, see below; 0x505C on the GBA; 6 chunks of 3968) | 21104 of 23808 bytes (2704 spare) |
| 7–15 | `struct PokemonStorage` (0x83D0 bytes, 9 chunks) | 33744 of 35712 bytes |

Format 1 had SaveBlock1 in 4 sectors (ids 1–4, 0x3D88 bytes), the boxes in ids 5–13, 14 sectors per
slot, slot 2 at sectors 14–27 and the special sectors at 28–31; its files are 131072 bytes.

Constants: `include/save.h` (`SECTOR_ID_*`, `NUM_SECTORS_PER_SLOT`, `SECTORS_COUNT`),
`include/gba/flash_internal.h` (`FLASH_PC_SECTORS`, `FLASH_PC_SIZE`, `SAVE_FORMAT_VERSION`).
`src/save.c` static-asserts that they agree with each other and with the struct sizes.

## Save format version and what the loader accepts

`SaveBlock2.saveFormatVersion` (offset 0x90, unused filler before) is written as `SAVE_FORMAT_VERSION`
(2) by every save (`src/save.c`). Bump the constant whenever the save layout changes.

`src/platform/sdl2.c` (`ReadSaveFile`) loads the file at startup:

1. 0 bytes (new or empty file): erased flash, a new game.
2. Exactly `FLASH_PC_SIZE` (147456) bytes: read into the flash. `Save_FlashFormatVersion()`
   (`src/save.c`) then reads `saveFormatVersion` from the newest SaveBlock2 sector with a valid
   signature and checksum (either slot; an erased image counts as current). If it is not
   `SAVE_FORMAT_VERSION`, the file is refused:
   `[save] '<file>' is from an older or unknown version of the game (format N, this build needs 2). Saves from earlier versions are not supported: move it away and start a new game.`
3. Any other size (a v0.2.0 save, a truncated file, not a save at all) is refused without being
   read: `[save] '<file>' is N bytes, not 147456: it is from an older or unknown version of the game. Saves from earlier versions are not supported: move it away and start a new game.`

In both refusal cases the game exits with status 1 and the file is never written.

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

## SaveBlock1 layout

Compared with format 1, only `flags[]` (0x12C → 0x600 bytes) and `vars[]` (0x200 → 0x1000 bytes)
grew, in place (format 2). The offset comments in `include/global.h` are GBA offsets; on PC
everything from `flags[]` on sits 0x200 further, because `struct ObjectEventTemplate` is 0x20
bytes there instead of 0x18.

**Format 3 (PS-079, BUG-015):** three things made the 32- and 64-bit layouts differ, so a `.sav`
could not move between them:
- That template holds a script pointer (4 bytes on 32-bit, 8 on 64-bit). It now lives in an
  8-byte slot with explicit tail padding: 0x20 bytes everywhere (`asm/macros/map.inc` mirrors it
  for the map data). The saved value is never used: `LoadSaveblockObjEventScripts()` refreshes it
  from the map header on continue.
- `SECRET_BASES_COUNT` was 10 on 64-bit builds (an upstream "quick hack for getting saves to
  work" from when SaveBlock1 had 4 sectors). It is 20 on every build again.
- `SaveBlock1.enigmaBerry` (`struct Berry2`, cast to `struct Berry`) holds two description
  pointers. Both structs now have 8-byte slots at 0x10 and 0x18 with explicit padding (0x28
  bytes), and `enigmaBerry` sits at an 8-aligned offset (0x46D0) with 4 pad bytes before it.

`SaveBlock1` ends with 4 pad bytes so its size is a multiple of 8 whatever the build's alignment.
`src/save.c` asserts at compile time: template 0x20 at 0xC70, berry structs 0x28 with the slots
at 0x10, `EnigmaBerry` 0x40 at 0x46D0, `SaveBlock1` 0x5270, `SaveBlock2` 0xF2C, `PokemonStorage`
0x83D0. CI builds all four targets, so a layout that differs between them does not build.

Growing SaveBlock1 or SaveBlock2 **at the end** within their sectors needs no new format: the
loader zero-fills sectors before writing and the checksum is a word sum, so existing files keep
validating and the new fields read as zero. Any other change (new sectors, fields in the middle,
a different flash size) needs a new `SAVE_FORMAT_VERSION`; existing saves are then refused.
Keep the asserts in `src/save.c` in step with any such change.

## Tests

`--save-selftest` (CI, Linux) runs, on `<save>.selftest`, the PS-014 I/O checks, the BUG-003 link
save, and the PS-070 group `format` (`src/save_format_selftest.c`): the `TRAINER_FLAG()` mapping
and the range constants; a format 2 round trip (save, then load the file from disk) with flags and
vars at both ends of the vanilla and the new ranges; files of other sizes (including 131072, a
v0.2.0 save) refused with the in-memory flash untouched; a file of the right size stamped with
save format 1 or 3 refused, the same file re-stamped with 2 accepted; empty and erased files load
as a new game.
