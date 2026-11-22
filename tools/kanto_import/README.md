# tools/kanto_import — Kanto map data from pret/pokefirered (PS-093 / PS-094 / PS-095)

`kanto_import.pl` turns map data of a [pret/pokefirered](https://github.com/pret/pokefirered)
checkout into ordinary Pokémon Spectrum (Emerald) data for `REGION_KANTO`: maps, layouts, tilesets,
map sections, heal locations, visited flags, map groups, the region map layout and Fly spots, and
the multiplayer group table. `kanto_region_map.pl` converts pokefirered's Kanto picture for the
Fly / PokéNav / wall map. They import **map data only**: no NPCs, trainers, signs, items, scripts
or dialogue (epic PS-093); the only hand-written additions are the `extras/` below (the sailor of
the boat back to Hoenn). Perl with core modules (`JSON::PP`, `Compress::Zlib`) is all they need;
nothing from the pokefirered checkout is executed, its files are read as data.

The output is committed, so **no build step changes**: `make linux` / `make winwsl` build the
Kanto maps like any other map. Re-run the scripts only after changing them, their tables or the
map list.

## Run

```
git clone --depth 1 https://github.com/pret/pokefirered /tmp/pokefirered    # outside the repository
perl tools/kanto_import/kanto_import.pl --frlg /tmp/pokefirered --maps tools/kanto_import/maps_ps095.txt
perl tools/kanto_import/kanto_region_map.pl --frlg /tmp/pokefirered
```

`kanto_import.pl` options:

| Option | Meaning |
|---|---|
| `--frlg <dir>` | pokefirered checkout (required) |
| `--maps <file>` | one pokefirered map name per line (`data/maps/<name>`), `#` comments. `maps_ps095.txt` is all of Kanto (256 maps; the excluded maps are listed in it, commented out, with a reason each); `maps_ps094.txt` was the PS-094 slice |
| `--repo <dir>` | repository root (default: two levels above this script) |
| `--allow-unknown` | continue past translation problems (unknown behaviour on a used metatile, metatile out of range, bad warp target or warp id, connection offsets that disagree, unknown music): they become `MB_NORMAL` / notes instead of a failure |
| `--dry-run` | translate, audit and report, write nothing |
| `--quiet` | only errors |

`kanto_region_map.pl` takes `--frlg <dir>`, `--repo <dir>` and `--preview <png>` (a x3 render of
the written picture with the cursor grid and the Kanto sections marked, to check the alignment).

The run is **idempotent**: it owns every `data/maps/Kanto*`, `data/layouts/Kanto*`,
`data/tilesets/*/kanto_*` directory (they are deleted and regenerated, so a shorter map list leaves
no stale maps) and replaces its own entries and marker blocks in the shared files below. It prints
every item it could not translate and exits 1 without writing anything unless `--allow-unknown` is
given.

## Audits

Every run checks and reports (the last lines of the output, `audit: ...`); any miss is an error:

- **warps**: every warp's target map is imported and the warp id exists there. A warp to a map
  outside the import (pokefirered's Union Room / Trade Center from the Pokémon Center upper floors)
  is redirected to the map itself (it then sits on a plain metatile and never triggers); a target
  that is no pokefirered map at all is an error. A `MAP_DYNAMIC` warp (the elevators of the Celadon
  Dept. Store, Silph Co. and the Rocket Hideout, which pokefirered's scripts point at the chosen
  floor) leads back to the first map that warps into its map, at that warp (the floor the elevator
  is first entered from), reported as "dynamic -> first entrance" (PS-098).
- **connections**: the target is imported (else the connection is dropped) and the other side
  connects back with the negated offset. A map with no connection back is reported as one-way
  (Saffron City's four routes point at pokefirered's display-only `SaffronCity_Connection`
  stand-in, whose own offsets disagree with the routes' in pokefirered itself; kept as it is, the
  routes' gates block that border).
- **layouts**: every metatile id of every layout (borders included) is inside its primary or
  secondary tileset.
- **music**: every map's track exists here (`MUS_RG_*` for the FRLG soundtrack, see Tables).
- **behaviours**: every metatile behaviour on a used metatile is translated (by name or
  `behaviors.tsv`).

## What it writes

| Output | From |
|---|---|
| `data/maps/Kanto<Map>/map.json`, `scripts.inc` | `map.json`: header fields, connections and warps to other imported maps (others are dropped / redirected to the map itself with a note); events are dropped, except the `extras/` below. `scripts.inc`: towns and cities get an `ON_TRANSITION` that sets their visited flag, nothing else (plus the `extras/`). |
| `data/layouts/Kanto<Layout>/map.bin`, `border.bin` + entries in `data/layouts/layouts.json` | copied byte for byte (same cell format). pokefirered borders can be bigger than 2x2 (Viridian Forest, Safari Zone: 3x2); Emerald tiles a 2x2 border, so the top-left 2x2 is kept. |
| `data/tilesets/primary/kanto_<name>/`, `data/tilesets/secondary/kanto_<name>[_<primary>]/` | the re-pack described below; `metatile_attributes.bin` converted from FRLG's 32-bit to Emerald's 16-bit attributes with `behaviors.tsv`. A secondary used with several primaries gets one copy per primary, suffixed with the primary's name. A tileset that draws with another tileset's graphics in pokefirered (`silph_co` uses `condominiums`') shares the `gTilesetTiles_` / `gTilesetPalettes_` symbols and only writes its own metatiles. |
| `src/data/tilesets/graphics.h`, `metatiles.h`, `headers.h` (between `// KANTO_IMPORT_BEGIN` / `END`) | `gTileset_Kanto<Name>` and its arrays (callback `NULL`: animations are PS-096). In the same files as Hoenn's so Porymap lists them (PS-094 used separate `kanto_*.h` files; they are gone). |
| `data/maps/map_groups.json` | `gMapGroup_Kanto<FRLG group>` groups, FRLG order |
| `src/data/region_map/region_map_sections.json` | `MAPSEC_KANTO_<X>` with FRLG's name, in FRLG's order; the first one carries `"value": "0x100"` (`MAPSEC_REGIONS_START`); entries are marked `"comment": "kanto_import: ..."` so the converter finds its own entries again. A name Emerald already uses (`MAPSEC_KANTO_VICTORY_ROAD`, `MAPSEC_KANTO_SAFARI_ZONE`: met-location names for traded Pokémon) gets `_MAP`. Coordinates: sections on pokefirered's cursor grid (`region_map_layout_kanto.h`, `LAYER_MAP`) keep FRLG's x/y/width/height; the others (dungeons, indoor-only sections, x=0,y=0 in FRLG) get the cell of FRLG's dungeon icon (`LAYER_DUNGEON`) or, failing that, the cell of the first already placed map that warps into them (an outdoor map's warp position scaled to its section), 1x1, off the cursor grid. |
| `src/data/heal_locations.json` | `HEAL_LOCATION_KANTO_<X>` for every imported map that has one in pokefirered (the towns' spots, Route 4 and Route 10) |
| `include/constants/flags.h` (between `// KANTO_IMPORT_BEGIN` / `END`) | `FLAG_VISITED_KANTO_<SECTION>` per town/city section from `REGION_FLAGS_START + 0x10`, in FRLG's section order, and `KANTO_FLAGS_END` |
| `data/event_scripts.s` (between `@ KANTO_IMPORT_BEGIN` / `END`) | the `.include` lines |
| `src/data/region_map/kanto_region.h` | `MAPSEC_KANTO_FIRST/LAST`, `sKantoRegionMapLayout[15][28]` (the on-grid sections at FRLG's coordinates), `sKantoRegionFlySpots[]` (towns with a heal location) |
| `src/mp/kanto_mp_groups.h` | `KANTO_MP_GROUP_SIZES` / `KANTO_MP_GROUP_CHECKS` for `src/mp/mp_presence.c` |
| `graphics/door_anims/kanto/<name>.png` | PS-098: pokefirered's door animation sheets (three frames of 16x16 or 16x32 px, 4bpp) for the Kanto door table in `src/field_door.c` (`sKantoDoorAnimGraphicsTable`, chosen by the map's primary tileset). The metatile ids and palette slots that table uses are pokefirered's own, which the prefix re-pack keeps. The list of sheets is in the script (`@DOOR_ANIMS`); the directory belongs to the converter. |
| `graphics/object_events/pics/misc/kanto_ss_anne.png`, `palettes/kanto_ss_anne.pal` | PS-098: pokefirered's S.S. ANNE sprite (128x64) and palette for `OBJ_EVENT_GFX_SS_ANNE` (`src/data/object_events`), placed at its dock by `extras/SSAnne_Exterior.objects.json` (pokefirered draws the ship as an object event, not as part of the layout). |

`kanto_region_map.pl` writes `graphics/pokenav/region_map/kanto_map.png` (8bpp tiles, pixel =
`0x70` + colour slot), `kanto_map.pal` (48 colours, loaded at `BG_PLTT_ID(7)`) and `kanto_map.bin`
(the 64x64 byte affine tilemap). pokefirered draws its map on a 30x20 text background
(`graphics/region_map/kanto.bin`, 4bpp tiles, 4 palettes, flips; cursor cell `(gx, gy)` on tile
`(gx + 4, gy + 4)`); Emerald's zooms (8-bit tile ids, 8bpp tiles; cell `(gx, gy)` on tile
`(gx + 1, gy + 2)`). The script flattens each used tile/palette/flip combination into one 8bpp tile
and places FRLG's tile `(fx, fy)` at `(fx - 3, fy - 2)`, so every cursor cell covers the same piece
of the picture as in FRLG (the sections keep FRLG's coordinates). FRLG's frame columns and its
SWITCH / CANCEL buttons become open sea, as does everything outside the picture (tile 0). Limits:
256 tiles, 48 colours (Kanto needs 172 and 25).

One-time hooks the scripts expect (already in place; `kanto_import.pl` warns if one is missing):
the marker pairs above (in `constants/flags.h`, `data/event_scripts.s`,
`src/data/tilesets/graphics.h`, `metatiles.h`, `headers.h`), `#include "data/region_map/kanto_region.h"`,
the `REGION_KANTO` entry and the `sKantoRegionMap_*` INCGFX lines in `src/region.c`,
`#include "kanto_mp_groups.h"` plus `KANTO_MP_GROUP_SIZES` / `KANTO_MP_GROUP_CHECKS` in
`src/mp/mp_presence.c`, `REGION_KANTO` in `include/constants/regions.h`.

## Extras (hand-written additions to generated maps)

`extras/<Map>.objects.json` (a JSON array of object events in this repository's `map.json` form)
is spliced into that map's `object_events`; `extras/<Map>.scripts.inc` is appended verbatim to its
`scripts.inc`. `<Map>` is the pokefirered map name (`VermilionCity`). Every object's `script` must
be defined in the `.scripts.inc`. PS-095 uses them for the sailor on Vermilion City's pier (the
boat back to Hoenn), PS-098 for the S.S. ANNE object at its dock (`SSAnne_Exterior.objects.json`);
nothing else.

## Tables

- `behaviors.tsv`: FRLG metatile behaviour -> Emerald behaviour. A behaviour whose name exists in
  Emerald needs no row. `-` means "no equivalent" (reported; `MB_NORMAL` with `--allow-unknown`).
  `?` in the note marks a stand-in for PS-096 (spinners as slides, cycling-road pull, strength
  buttons, rock stairs, current water as plain water). FRLG's four directional stair warps
  (`MB_UP_LEFT_STAIR_WARP` etc.) exist here under the same names since PS-098 (the warp tile beside
  a drawn staircase: a sideways step onto the stairs and the warp).
  FRLG's terrain and encounter-type attribute bits have no Emerald counterpart and are dropped (Emerald
  derives encounters from behaviours); the layer type is kept.
- `constants.tsv`: map header constants. Music is mapped `MUS_X` -> `MUS_RG_X` automatically (Emerald
  ships the FRLG soundtrack as `MUS_RG_*`), else `MUS_X` if Emerald has it, else the table.
  Battle scenes: FRLG's Elite Four backgrounds onto Emerald's Elite Four slots.

## The tileset re-pack ("prefix re-pack")

FRLG primaries hold 640 tiles, 640 metatiles and 7 palettes; Emerald's engine splits the shared
1024-tile / 1024-metatile / 13-palette space at 512 / 512 / 6 (`include/fieldmap.h`). Tile ids,
metatile ids and palette slots are absolute, only the split point differs, so the converter writes the
Kanto primary with its first 512 tiles / 512 metatiles / palettes 0..5, and **prepends to every
secondary paired with it** the primary's tiles 512..639 (Emerald loads them at VRAM tile 512: the same
ids), its metatiles 512..639 (Emerald numbers them 512+n: the same ids) and its palette 6 (Emerald
loads slot 6 from the secondary). The secondary's own tiles then start at 640 and its metatiles at
640, exactly as in FRLG, so `map.bin`, `border.bin` and every metatile's tile references are copied
unchanged. 128 + 384 = 512 fits Emerald's secondary space exactly. Hoenn's engine code and data are
untouched and Porymap opens the Kanto tilesets with the normal Emerald config. A secondary used
with both primaries gets one copy per primary (in Kanto proper none is; pokefirered's
`seafoam_islands` is paired with `building` only by a Sevii house).

## Naming

Everything imported is prefixed so it never collides with Hoenn: maps `Kanto<Map>` /
`MAP_KANTO_<MAP>`, layouts `LAYOUT_KANTO_<X>` in `data/layouts/Kanto<Layout>`, tilesets
`gTileset_Kanto<Name>` in `kanto_<name>`, groups `gMapGroup_Kanto<Group>`, sections
`MAPSEC_KANTO_<X>`, heal locations `HEAL_LOCATION_KANTO_<X>`, flags `FLAG_VISITED_KANTO_<X>`.
Section ids and visited flags follow pokefirered's section order; the converter regenerates both
blocks as a whole on every run.

## Measuring (`measure.pl`)

`perl tools/kanto_import/measure.pl <pokefirered> <repo>` prints the numbers behind the PS-094
decision: primary tileset usage versus Emerald's limits, layouts that reference primary metatiles
above 512, every metatile behaviour used by FRLG layouts with its Emerald status, and the `map.json`
field inventory.
