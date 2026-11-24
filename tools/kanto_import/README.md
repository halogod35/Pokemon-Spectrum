# tools/kanto_import — Kanto map data from pret/pokefirered (PS-093 / PS-094 / PS-095)

`kanto_import.pl` turns map data of a [pret/pokefirered](https://github.com/pret/pokefirered)
checkout into ordinary Pokémon Spectrum (Emerald) data for `REGION_KANTO`: maps, layouts, tilesets,
map sections, heal locations, visited flags, map groups, the region map layout and Fly spots, and
the multiplayer group table. `kanto_region_map.pl` converts pokefirered's Kanto picture for the
Fly / PokéNav / wall map; `kanto_encounters.pl` writes FireRed's wild Pokémon tables (PS-097, see
below). Without `--events` they import **map data only**: no NPCs, trainers, signs, items, scripts
or dialogue (epic PS-093); the only hand-written additions are the `extras/` below (the sailor of
the boat back to Hoenn). With `--events <list>` (PS-100, epic PS-099) the maps in that list also get
pokefirered's story: every object, sign, hidden item and trigger, its map scripts and text, translated
mechanically (see "Story import" below). Perl with core modules (`JSON::PP`, `Compress::Zlib`) is all they need;
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
| `--events <file>` | PS-100: a second map list (a subset of `--maps`): for these maps pokefirered's object / bg / coord events, map scripts and text are imported too, with everything they need (shared scripts, sprites, trainers, flags, vars). `maps_ps094.txt` is the PS-100 slice. Without it the output is PS-095/096's (rocks and trees only) and the event outputs are not touched. See "Story import" below. |
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
| `data/maps/Kanto<Map>/map.json`, `scripts.inc` | `map.json`: header fields, connections and warps to other imported maps (others are dropped / redirected to the map itself with a note); events are dropped, except pokefirered's Rock Smash rocks and cut trees (PS-096: `OBJ_EVENT_GFX_ROCK_SMASH_ROCK` / `OBJ_EVENT_GFX_CUT_TREE` objects with its generic `EventScript_RockSmash` / `EventScript_CutTree`, written as Emerald's `OBJ_EVENT_GFX_BREAKABLE_ROCK` / `OBJ_EVENT_GFX_CUTTABLE_TREE` with Emerald's scripts of the same names and pokefirered's `FLAG_TEMP_*` flags, which exist here unchanged; the table is `%KEPT_OBJECTS` in the script, pokefirered's "clone" objects are dropped) and the `extras/` below. `scripts.inc`: towns and cities get an `ON_TRANSITION` that sets their visited flag, nothing else (plus the `extras/`). With `--events` the maps in that list get every pokefirered event and their scripts and text instead (PS-100, "Story import" below). |
| `data/layouts/Kanto<Layout>/map.bin`, `border.bin` + entries in `data/layouts/layouts.json` | copied byte for byte (same cell format). pokefirered borders can be bigger than 2x2 (Viridian Forest, Safari Zone: 3x2); Emerald tiles a 2x2 border, so the top-left 2x2 is kept. |
| `data/tilesets/primary/kanto_<name>/`, `data/tilesets/secondary/kanto_<name>[_<primary>]/` | the re-pack described below; `metatile_attributes.bin` converted from FRLG's 32-bit to Emerald's 16-bit attributes with `behaviors.tsv`. A secondary used with several primaries gets one copy per primary, suffixed with the primary's name. A tileset that draws with another tileset's graphics in pokefirered (`silph_co` uses `condominiums`') shares the `gTilesetTiles_` / `gTilesetPalettes_` symbols and only writes its own metatiles. |
| `data/tilesets/*/kanto_<name>/anim/<animation>/<n>.png` | PS-096: pokefirered's tileset animation frames, copied for the tilesets in the script's `%TILESET_ANIM_CALLBACKS` table (`general`: flowers, water / current / land edges, sand edge; `celadon_city` and `silph_co` fountains; `celadon_gym` flowers; `vermilion_gym` motorized door). The callbacks `InitTilesetAnim_Kanto*` in `src/tileset_anims.c` are ported from pokefirered's `src/tileset_anims.c` with its VRAM tile offsets, which the re-pack keeps (a primary's animated tiles are below 512, a secondary's own tiles start at VRAM tile 640 in both engines). A pokefirered tileset with an `anim/` directory and no table row (`mt_ember`, Sevii) is reported and stays still. |
| `src/data/tilesets/graphics.h`, `metatiles.h`, `headers.h` (between `// KANTO_IMPORT_BEGIN` / `END`) | `gTileset_Kanto<Name>` and its arrays; `.callback` is the animation callback from the table above or `NULL`. In the same files as Hoenn's so Porymap lists them (PS-094 used separate `kanto_*.h` files; they are gone). |
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
  Every row's note says what pokefirered does with the behaviour and what happens here. FRLG-only
  mechanics ported into the engine keep their FRLG names here: the directional stair warps
  (`MB_UP_LEFT_STAIR_WARP` etc., PS-098: a sideways step onto the drawn stairs and the warp), the
  spinners (`MB_SPIN_*`, `MB_STOP_SPINNING`, PS-096: `field_player_avatar.c`), the cycling road
  (`MB_CYCLING_ROAD_PULL_DOWN(_GRASS)`, PS-096: `bike.c`) and the rock stairs (`MB_ROCK_STAIRS`,
  PS-096). Decided without an engine change (PS-096): Seafoam's `MB_FAST_WATER` is plain surfable
  water (pokefirered's current is map scripts, the behaviour only refuses Surf onto it),
  `MB_STRENGTH_BUTTON` is plain floor (its effect is a coord-event script fired by a boulder object;
  neither is imported), `MB_SAND_CAVE` is Emerald's `MB_FOOTPRINTS`.
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

## Wild encounters (`kanto_encounters.pl`, PS-097)

```
perl tools/kanto_import/kanto_encounters.pl --frlg /tmp/pokefirered [--maps tools/kanto_import/maps_ps095.txt] [--dry-run] [--quiet]
```

Writes **FireRed's** wild Pokémon tables (not LeafGreen's; LeafGreen is a later option) for the
imported Kanto maps into `src/data/wild_encounters.json`, from pokefirered's
`src/data/wild_encounters.json`. The entries go into the one `gWildMonHeaders` group, after
Hoenn's: the engine (`GetCurrentMapWildMonHeaderId` in `src/wild_encounter.c`) walks only that
array, so a second group would never be read. jsonproc (`src/data/wild_encounters.json.txt`)
then generates `gKanto<Map>_LandMons` / `_WaterMons` / `_RockSmashMons` / `_FishingMons` and
the header entries like Hoenn's, with `MAP_KANTO_*` ids.

- **Kept:** entries whose `base_label` ends in `_FireRed` and whose map is in `--maps` (66 of
  pokefirered's 264: 56 land, 30 water, 4 rock smash, 30 fishing tables, 80 species). Dropped: the
  132 LeafGreen tables and the 66 FireRed tables of maps outside the import (Sevii Islands, the
  nine Altering Cave sets, Trainer Tower); there are no Navel Rock / Birth Island tables.
- **Renamed:** `MAP_X` -> the `id` of `data/maps/KantoX/map.json` (`MAP_KANTO_X`), `sX_FireRed` ->
  `gKantoX`. Tables and slot order are copied as they are: pokefirered's slot rates (12 land, 5
  water, 5 rock smash, 2 + 3 + 5 fishing slots) are checked to equal ours, since the probabilities
  are compiled from our json's `fields`.
- **Checked before writing** (any miss: printed, exit 1, nothing written): every map imported,
  every species in `include/constants/species.h` (all 386 exist), levels 1..100 with min <= max,
  table sizes, encounter rates 0..255, generated labels unique.
- **Idempotent:** a Kanto entry is one whose `"map"` starts with `MAP_KANTO_`; the run removes
  those and appends the new ones, so re-running changes nothing. The Hoenn text is untouched byte
  for byte (the written file is decoded again and every non-Kanto entry, the other groups and
  the field definitions are compared with the original), so the Hoenn tables of the generated
  `wild_encounters.h` are byte-identical.
- **Audit (encounter tiles):** pokefirered triggers encounters from a per-metatile attribute
  (encounter type land / water) that Emerald's attribute format does not have; `kanto_import.pl`
  drops it and Emerald decides from the metatile behaviour (`TILE_FLAG_HAS_ENCOUNTERS` in
  `src/metatile_behavior.c`). The script walks every imported layout with a table and reports,
  for the passable cells, how many of pokefirered's land / water encounter cells are encounter
  tiles here and which behaviours carry or lose them. Kanto (PS-097 run): 16795 of 17129 land
  cells (`MB_TALL_GRASS`, `MB_CAVE`, `MB_SAND_CAVE -> MB_CAVE`, `MB_CYCLING_ROAD_PULL_DOWN_GRASS ->
  MB_TALL_GRASS`), 11568 of 11821 water cells (`MB_OCEAN_WATER`, `MB_POND_WATER`,
  `MB_CYCLING_ROAD_WATER` / `MB_FAST_WATER -> MB_OCEAN_WATER`). Lost: Seafoam Islands B3F / B4F's
  current water (`MB_*WARD_CURRENT`, 229 cells + 20 waterfall cells: no encounters on Emerald's
  currents), cave ledge edges (`MB_IMPASSABLE_NORTH`), stair / arrow warps, holes, rock stairs and
  strength buttons (PS-096 stand-ins), 113 plain `MB_NORMAL` cave cells; no map loses more than a
  few percent. pokefirered's Kanto indoor encounter maps (Pokémon Mansion, Pokémon Tower, Power
  Plant) use `MB_CAVE` on their floors; `MB_INDOOR_ENCOUNTER` is on no passable Kanto cell (it
  exists here with the same meaning anyway). Route 21 South's land table is unreachable in
  pokefirered too (no passable land cell).

Rock Smash: the four tables (Cerulean Cave 1F / 2F / B1F, Rock Tunnel B1F) are imported, but the
breakable rocks are object events (`OBJ_EVENT_GFX_ROCK_SMASH_ROCK`, 40 in those maps) and
`kanto_import.pl` imports no events, so until the rocks are added (an `extras/` list) the tables
are not reachable in play.

## Measuring (`measure.pl`)

`perl tools/kanto_import/measure.pl <pokefirered> <repo>` prints the numbers behind the PS-094
decision: primary tileset usage versus Emerald's limits, layouts that reference primary metatiles
above 512, every metatile behaviour used by FRLG layouts with its Emerald status, and the `map.json`
field inventory.

## Story import (`--events`, PS-100 / epic PS-099)

`kanto_import.pl --events <list>` imports, for the maps in that list, everything pokefirered's
`map.json` events and scripts say, through `KantoEvents.pm` (the same run, the same ownership
rules). It is a mechanical translation: pokefirered's `.inc` files are copied and assembled with
**our** macros (the bytecode is Emerald's; only the macro layer differs), identifiers are renamed
into a Kanto namespace, and every identifier it cannot translate is printed and fails the run
(unless `--allow-unknown`). PS-100 ran it on `maps_ps094.txt` (Pallet Town, Route 1, Viridian City
and their interiors); PS-101 runs it on all of Kanto.

### Naming

| pokefirered | here | Where the id comes from |
|---|---|---|
| labels `PalletTown_EventScript_X`, `EventScript_Return`, `Text_X` | `KantoPalletTown_EventScript_X`, `KantoEventScript_Return`, `KantoText_X` | prefix |
| `FLAG_X` | `FLAG_KANTO_X` | `kanto_ids.tsv` (`EVENT_FLAGS_EXT_START + n`, append-only) |
| `FLAG_WORLD_MAP_X` (`setworldmapflag`) | `setflag FLAG_VISITED_KANTO_X` (PS-095's visited flags), else `FLAG_KANTO_WORLD_MAP_X` | |
| `FLAG_TEMP_*`, `VAR_TEMP_*`, `VAR_0x800x`, `VAR_RESULT`, `VAR_FACING`, `VAR_LAST_TALKED`, `VAR_OBJ_GFX_ID_*` | unchanged | engine-shared |
| `FLAG_SYS_POKEDEX_GET`, `FLAG_BADGE01_GET`, `VAR_CABLE_CLUB_STATE`, ... | Emerald's flag / var (`FLAG_BADGE09_GET` for the Kanto badge block) | `shared_ids.tsv` (also the FRLG names that exist here with another meaning and must not be shared: `VAR_STARTER_MON`, `FLAG_SYS_GAME_CLEAR`) |
| `VAR_X` | `VAR_KANTO_X` | `kanto_ids.tsv` (`VARS_EXT_START + n`) |
| `TRAINER_X` | `TRAINER_KANTO_X` = `TRAINER_KANTO_BASE` (1030) + pokefirered's id | stable by construction; entries in `src/data/kanto_trainers.h` / `kanto_trainer_parties.h`, classes and pics without a twin through `trainers.tsv` |
| `LOCALID_X` | `LOCALID_KANTO_X` (`local_id` in `map.json`, mapjson defines them) | |
| `OBJ_EVENT_GFX_X` | `OBJ_EVENT_GFX_KANTO_X` (256+, u16 graphics ids) | `kanto_sprites.tsv` (append-only); pokefirered's picture, pic table and graphics info are copied (`graphics/object_events/pics/kanto/`, `src/data/object_events/kanto/`) |
| `OBJ_EVENT_GFX_ROCK_SMASH_ROCK` / `CUT_TREE` / `PUSHABLE_BOULDER` | Emerald's `BREAKABLE_ROCK` / `CUTTABLE_TREE` / `PUSHABLE_BOULDER` with Emerald's field-move scripts (PS-096's rule, now the general case) | |
| `special X` | the same name when it exists here, else `specials.tsv`: an Emerald twin (`=Name`), a stub or a port in `src/kanto_specials.c` (`Kanto*`, 16/32-bit returns) | |
| `MAP_X`, `HEAL_LOCATION_X`, `LAYOUT_X`, `METATILE_X` | `MAP_KANTO_X`, `HEAL_LOCATION_KANTO_X`, `LAYOUT_KANTO_X`, `METATILE_KantoX` (labels block in `constants/metatile_labels.h`) | |
| `MUS_X` / `SE_X`, also `{MUS_X}` in text | `MUS_RG_X` when it exists, else `MUS_X`, else `constants.tsv` | |
| `ITEM_TM26_EARTHQUAKE` style names, `MOVETUTOR_X` | `ITEM_TM26`, `TUTOR_MOVE_X` | |
| `MULTICHOICE_*`, `SCR_MENU_CANCEL`, `POCKET_*`, `STDSTRING_*` | `script_constants.tsv` (`MULTI_KANTO_YES_NO` and `MULTI_KANTO_TRAINER_SCHOOL_WHITEBOARD` are PS-100 lists in `src/data/script_menu.h`) | |
| `HELPCONTEXT_*`, `FAMECHECKER_*`, `FCPICKSTATE_*`, `QL_*` | `0` (their shims ignore them) | |
| text: `‥`, `{FONT_MALE}`, `{FONT_FEMALE}` | `⋯`, dropped, dropped | `text.tsv`; every other character and `{PLACEHOLDER}` must be in our `charmap.txt` |

Shared scripts: the closure of everything the maps reference in `data/scripts`, `data/text` and
`data/event_scripts.s` (and in maps outside the event list) goes to `data/scripts/kanto_shared.inc`,
included first in the `KANTO_IMPORT` block of `data/event_scripts.s` (it sets `FIRERED` for the
scripts' `.ifdef FIRERED`). `shared_labels.tsv` lists the pokefirered labels that are **not** copied
because Emerald's script of the same meaning is used (the Pokémon Center 2F link-room attendants and
map scripts, the identical `Common_Movement_*`). `extra_roots.tsv` names scripts that pokefirered's C
code starts (the bedroom PC shutdown script), so they are imported with their map. Only referenced
labels are imported; pokefirered's unused scripts and text stay out.

FRLG-only script macros have shims in `asm/macros/event.inc` (`// PS-100`): `famechecker` and
`goto_if_questlog` are no-ops; `set_gym_trainers`, `giveitem_msg`, `msgreceiveditem` and
`putitemaway` call the imported `KantoEventScript_SetGymTrainers` / `KantoStd_ReceivedItem` /
`KantoStd_PutItemAway`; `trainerbattle_earlyrival` is a plain no-intro battle (losing blacks out as
usual; the rival controller is PS-102/PS-103 work). Clone objects (a neighbour map's tree drawn across
the border) are dropped with a note; a hidden item's `quantity` and `underfoot` are not in Emerald's
bg event (noted, one item is given).

Engine side (PS-100): graphics ids are u16 (`struct ObjectEvent` / `ObjectEventTemplate`, save format
4); a region's NPC palette set is in the region table (`OBJ_PAL_SET_KANTO` loads pokefirered's
`npc_blue / pink / green / white` into the four NPC slots, so the copied sprites keep their colours);
`{RIVAL}` and the bedroom PC shutdown script come from the region table too. Specials are listed in
`data/specials.inc` after the `PS-100` comment; `make checkspecials` covers them.

Outputs owned by the run (regenerated, never edited by hand): the event arrays and `scripts.inc` of
the maps in the list, `data/scripts/kanto_shared.inc`, `src/data/object_events/kanto/*.h`,
`graphics/object_events/pics/kanto/`, `graphics/object_events/palettes/kanto_*.pal`,
`src/data/kanto_trainers.h`, `src/data/kanto_trainer_parties.h`, the `KANTO_EVENTS` marker blocks of
`include/constants/{flags,vars,opponents,event_objects,metatile_labels}.h`, and the allocation tables
`kanto_ids.tsv` / `kanto_sprites.tsv` (committed: a re-run never renumbers an id a save may hold).

`measure_events.pl --frlg <dir> --maps <list>` prints the numbers behind the PS-100 decisions for any
map list: object events by kind and sprite, bg / coord events, flags, vars, specials, macros,
trainers (classes, pics, music), items, songs, text placeholders and charmap differences.
