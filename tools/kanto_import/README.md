# tools/kanto_import — Kanto map data from pret/pokefirered (PS-093 / PS-094)

`kanto_import.pl` turns map data of a [pret/pokefirered](https://github.com/pret/pokefirered)
checkout into ordinary Pokémon Spectrum (Emerald) data for `REGION_KANTO`: maps, layouts, tilesets,
map sections, heal locations, visited flags, map groups, the region map layout and Fly spots, and
the multiplayer group table. It imports **map data only**: no NPCs, trainers, signs, items, scripts
or dialogue (epic PS-093). Perl with core modules (`JSON::PP`, `Compress::Zlib`) is all it needs;
nothing from the pokefirered checkout is executed, its files are read as data.

The converter's output is committed, so **no build step changes**: `make linux` / `make winwsl`
build the Kanto maps like any other map. Re-run the converter only to import more maps or after
changing it or its tables.

## Run

```
git clone --depth 1 https://github.com/pret/pokefirered /tmp/pokefirered    # outside the repository
perl tools/kanto_import/kanto_import.pl --frlg /tmp/pokefirered --maps tools/kanto_import/maps_ps094.txt
```

| Option | Meaning |
|---|---|
| `--frlg <dir>` | pokefirered checkout (required) |
| `--maps <file>` | one pokefirered map name per line (`data/maps/<name>`), `#` comments; `maps_ps094.txt` is the PS-094 slice |
| `--repo <dir>` | repository root (default: two levels above this script) |
| `--allow-unknown` | continue past translation problems (unknown behaviour on a used metatile, metatile out of range, bad warp target): they become `MB_NORMAL` / notes instead of a failure |
| `--dry-run` | translate and report, write nothing |
| `--quiet` | only errors |

The run is **idempotent**: it owns every `data/maps/Kanto*`, `data/layouts/Kanto*`,
`data/tilesets/*/kanto_*` directory (they are deleted and regenerated, so a shorter map list leaves
no stale maps) and replaces its own entries in the shared files below. It prints every item it could
not translate and exits 1 without writing anything unless `--allow-unknown` is given.

## What it writes

| Output | From |
|---|---|
| `data/maps/Kanto<Map>/map.json`, `scripts.inc` | `map.json`: header fields, connections and warps to other imported maps (others are dropped / redirected to the map itself with a note); events are dropped. `scripts.inc`: towns and cities get an `ON_TRANSITION` that sets their visited flag, nothing else. |
| `data/layouts/Kanto<Layout>/map.bin`, `border.bin` + entries in `data/layouts/layouts.json` | copied byte for byte (same cell format) |
| `data/tilesets/primary/kanto_<name>/`, `data/tilesets/secondary/kanto_<name>/` | the re-pack described below; `metatile_attributes.bin` converted from FRLG's 32-bit to Emerald's 16-bit attributes with `behaviors.tsv` |
| `src/data/tilesets/kanto_graphics.h`, `kanto_metatiles.h`, `kanto_headers.h` | `gTileset_Kanto<Name>` and its arrays (callback `NULL`: animations are PS-096) |
| `data/maps/map_groups.json` | `gMapGroup_Kanto<FRLG group>` groups, FRLG order |
| `src/data/region_map/region_map_sections.json` | `MAPSEC_KANTO_<X>` with FRLG's name and map coordinates; the first one carries `"value": "0x100"` (`MAPSEC_REGIONS_START`); entries are marked `"comment": "kanto_import: ..."` so the converter finds its own entries again. A name Emerald already uses (`MAPSEC_KANTO_VICTORY_ROAD`, `MAPSEC_KANTO_SAFARI_ZONE`: met-location names for traded Pokémon) gets `_MAP`. |
| `src/data/heal_locations.json` | `HEAL_LOCATION_KANTO_<X>` for imported maps that have one (respawn fields dropped) |
| `include/constants/flags.h` (between `// KANTO_IMPORT_BEGIN` / `END`) | `FLAG_VISITED_KANTO_<SECTION>` per town/city section from `REGION_FLAGS_START + 0x10`, and `KANTO_FLAGS_END` |
| `data/event_scripts.s` (between `@ KANTO_IMPORT_BEGIN` / `END`) | the `.include` lines |
| `src/data/region_map/kanto_region.h` | `MAPSEC_KANTO_FIRST/LAST`, `sKantoRegionMapLayout[15][28]` (outdoor sections at FRLG's coordinates), `sKantoRegionFlySpots[]` (towns with a heal location) |
| `src/data/kanto_mp_groups.h` | `KANTO_MP_GROUP_SIZES` / `KANTO_MP_GROUP_CHECKS` for `src/mp/mp_presence.c` |
| `graphics/pokenav/region_map/kanto_map.bin` | placeholder region picture: town (`0x0D`) and route (`0x4D`) markers on open sea at the sections' cells; PS-095 draws the real one |

One-time hooks the converter expects (already in place; it warns if one is missing): the two marker
pairs above, `#include "data/tilesets/kanto_*.h"` in `src/tilesets.c`,
`#include "data/region_map/kanto_region.h"` and the `REGION_KANTO` entry in `src/region.c`,
`#include "data/kanto_mp_groups.h"` plus `KANTO_MP_GROUP_SIZES` / `KANTO_MP_GROUP_CHECKS` in
`src/mp/mp_presence.c`, `REGION_KANTO` in `include/constants/regions.h`.

## Tables

- `behaviors.tsv`: FRLG metatile behaviour -> Emerald behaviour. A behaviour whose name exists in
  Emerald needs no row. `-` means "no equivalent" (reported; `MB_NORMAL` with `--allow-unknown`).
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
untouched and Porymap opens the Kanto tilesets with the normal Emerald config. The one FRLG secondary
used with both primaries (`seafoam_islands`) would need one copy per primary (the converter stops
with a message if a map list needs that).

## Naming

Everything imported is prefixed so it never collides with Hoenn: maps `Kanto<Map>` /
`MAP_KANTO_<MAP>`, layouts `LAYOUT_KANTO_<X>` in `data/layouts/Kanto<Layout>`, tilesets
`gTileset_Kanto<Name>` in `kanto_<name>`, groups `gMapGroup_Kanto<Group>`, sections
`MAPSEC_KANTO_<X>`, heal locations `HEAL_LOCATION_KANTO_<X>`, flags `FLAG_VISITED_KANTO_<X>`.

## Measuring (`measure.pl`)

`perl tools/kanto_import/measure.pl <pokefirered> <repo>` prints the numbers behind the PS-094
decision: primary tileset usage versus Emerald's limits, layouts that reference primary metatiles
above 512, every metatile behaviour used by FRLG layouts with its Emerald status, and the `map.json`
field inventory.
