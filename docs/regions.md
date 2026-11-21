# Regions (PS-071)

Pokémon Spectrum can hold several regions next to Hoenn. Hoenn is region 0 and keeps every
value it had in Emerald (map sections, flags, scripts, Fly table). Every other region is an
entry in the **region table** (`src/region.c`, `sRegions[]`, types in `include/region.h`,
ids in `include/constants/regions.h`) plus ordinary map data.

This file is the checklist for adding one. Regions are either the user's own designs, built in
Porymap, or map data imported mechanically from a pret GBA decomp (user decision 2026-11-21, epic
PS-093: Kanto from `pokefirered`, see "Kanto" below). Story content (NPCs, trainers, items,
dialogue) is never imported.

## What the engine does per region

| Feature | Where | Notes |
|---|---|---|
| Map sections (`MAPSEC_*`) | `src/data/region_map/region_map_sections.json` | 16-bit ids. Hoenn: 0x00-0xD5 (unchanged). Other regions start at `MAPSEC_REGIONS_START` (0x100). |
| Region map (Fly, PokéNav, Pokémon Center wall map) | `RegionInfo.mapLayout`, `mapTilesLZ`, `mapTilemapLZ`, `mapPalette` | The layout says which section each of the 28x15 cursor cells belongs to; the tilemap is the picture behind it. |
| Fly spots | `RegionInfo.flySpots[]` | `{ mapSecId, healLocation, visitedFlag }`. The flag unlocks the spot and marks the region as visited. Hoenn keeps Emerald's tables in `region_map.c` (`flySpots = NULL`). |
| Region switching | `region_map.c` (`SwitchRegionMapRegion`) | With more than one region visited, L/R on the full (zoomed-out) map shows the previous/next visited region. Fly map: line 2 of the name window shows `{L}{R} <region>`. PokéNav: the help bar says `{L}{R}REGION`. Wall map: the title names the region. |
| PokéNav map | `RegionInfo.hasPokenavMap` | FALSE: the PokéNav opens on the nearest visited region that has a map. City zoom pictures exist for Hoenn only; other regions show the section name. |
| Map name popups | `RegionInfo.popupTheme` | One `MAPPOPUP_THEME_*` per region (Hoenn keeps its per-section table). |
| Badges | `RegionInfo.firstBadgeFlag` | 8 consecutive flags. Hoenn: `FLAG_BADGE01..08`. Regions 2 and 3: `FLAG_BADGE09..16` and `FLAG_BADGE17..24` (`REGION_FLAGS_START` block in `constants/flags.h`). The trainer card's badge row shows the badges of the region the player is in and, outside Hoenn, the region's name. |
| Met locations | `RegionInfo.metLocation` | See below. |
| Region transit | `sRegionTransitDestinations[]` in `src/region.c`, `data/scripts/region_transit.inc` | Ferry/train helper for scripts. |
| Multiplayer | `src/mp/mp_presence.c` `sMapGroupSizes` | Presence keys on map group/num; one table entry per new map group. See `docs/multiplayer/design.md`, "Regions". |

### Met locations (u8 in the save)

A Pokémon's met location and the TV show locations are one byte in the save, so they cannot
hold a section id above 0xFF. Rule (`GetMetLocationForMapSec`, `region.c`):

- Hoenn maps store the map section itself (all below 0xD6), as before.
- A map of any other region stores the region's **alias**: `RegionInfo.metLocation`, a value
  in `METLOC_REGION_START..METLOC_REGION_END` (0xD6-0xFC, one per region). `GetMapName()`
  shows the region's name for it, so the summary screen reads "met in KANTO" rather than
  the exact town. 0xFD-0xFF stay the `METLOC_*` specials.
- Writers must go through `GetCurrentMetLocation()` / `GetMetLocationForMapSec()`, never store
  `gMapHeader.regionMapSectionId` in a u8 (pokemon.c, egg_hatch.c, tv.c do).

## Add a region: checklist

1. **Id.** `include/constants/regions.h`: add `REGION_<NAME>` before `REGIONS_COUNT`, bump the count.
2. **Map sections.** `src/data/region_map/region_map_sections.json`: append the region's sections
   after the existing ones (after `MAPSEC_NONE`; Kanto's generated ones come last). The first section of the
   first non-Hoenn region carries `"value": "0x100"`; later ones just follow. `x`, `y`, `width`,
   `height` are the section's cell(s) on **that region's** map. The build regenerates
   `include/constants/region_map_sections.h` and `gRegionMapEntries`.
3. **Maps.** Porymap: new maps with `"region_map_section": "MAPSEC_<...>"`, in a new map group
   (`data/maps/map_groups.json`, e.g. `gMapGroup_<Name>`), layouts in `data/layouts/layouts.json`.
   Add `.include "data/maps/<Map>/scripts.inc"` lines to `data/event_scripts.s`.
4. **Flags.** Each town that can be flown to needs a visited flag (`setflag` in the map's
   `ON_TRANSITION` script, like Hoenn's towns). Append new flags to the `REGION_FLAGS_START`
   block in `include/constants/flags.h`; never renumber Hoenn's.
5. **Heal locations.** `src/data/heal_locations.json`: one `HEAL_LOCATION_*` per Fly spot
   (and per Pokémon Center).
6. **Region map picture.** A 64x64 byte tilemap (`graphics/pokenav/region_map/<name>_map.bin`,
   8-bit tile indices into `map.png`'s 233 tiles; the Hoenn picture is `map.bin`). Own tiles and
   palette are possible: point `mapTilesLZ`/`mapPalette` at them (8bpp tiles, 3 palettes from
   `BG_PLTT_ID(7)`). Tile 0x0D is Hoenn's inland town marker, 0x00 is open sea.
7. **Layout.** A `[REGION_MAP_HEIGHT][REGION_MAP_WIDTH]` array of `MAPSEC_*` (`MAPSEC_NONE`
   for empty cells), like the generated `src/data/region_map/kanto_region.h`. It must agree
   with the `x`/`y` in step 2: cell `(x, y)` of the layout is drawn on tile `(x + 1, y + 2)` of the
   picture's tilemap (`MAPCURSOR_X_MIN` / `Y_MIN` in `src/region_map.c`).
8. **Table entry.** `src/region.c` `sRegions[REGION_<NAME>]`: name (up to 10 characters fit
   the Fly window line), `mapSecStart`/`mapSecEnd`, `metLocation` (next free alias:
   `METLOC_REGION_START + n`), layout, graphics, Fly spots, `hasPokenavMap`, `popupTheme`,
   `firstBadgeFlag`.
9. **Transit.** Add the region's hub(s) to `sRegionTransitDestinations[]` and give a ferry/train
   NPC the script from `data/scripts/region_transit.inc` (example in the file).
10. **Multiplayer.** `src/mp/mp_presence.c`: append `MP_GROUP_SIZE(<last map of the group>)` and
    `MP_CHECK_GROUP(<index>, ...)` for the new map group (the build fails until you do).
11. **Badges.** Gym scripts `setflag FLAG_BADGE<nn>_GET` from the region's block. Field moves
    (Surf, Fly, ...) still check Hoenn's badges; a per-region rule is future work.
12. Build through CI, then check: map popups, the wall map, Fly to and from the region (L/R),
    PokéNav map, trainer card, and two players seeing each other there.

## Kanto (PS-093, imported from pret/pokefirered)

`REGION_KANTO` is region 1: every Kanto map of FireRed/LeafGreen (PS-095: 256 maps in 29 groups,
180 layouts, 51 tilesets, 53 map sections, 11 Fly spots, 13 heal locations; the Sevii Islands,
FRLG's link rooms, Navel Rock / Birth Island and the Trainer Tower are left out, see
`tools/kanto_import/maps_ps095.txt`). It is **map data only**: no NPCs, trainers, items, signs,
scripts or dialogue (epic PS-093), no wild encounters (PS-097), no tileset animations, door
animations, spinners or current water (PS-096). The two sailors of the boat are the only NPCs.

**Generated** by `tools/kanto_import/kanto_import.pl` from a pokefirered checkout (README in that
directory): maps `data/maps/Kanto*`, layouts `data/layouts/Kanto*`, tilesets
`data/tilesets/*/kanto_*` (re-packed to Emerald's 512-tile / 512-metatile / 6-palette primary
split, see the card PS-094) with their entries between the `KANTO_IMPORT` markers of
`src/data/tilesets/{graphics,metatiles,headers}.h` (so Porymap lists `gTileset_Kanto*`), the
`MAPSEC_KANTO_*` sections, `HEAL_LOCATION_KANTO_*`, the `FLAG_VISITED_KANTO_*` block in
`constants/flags.h`, the `gMapGroup_Kanto*` groups, `src/data/region_map/kanto_region.h` (layout,
Fly spots, section range) and `src/mp/kanto_mp_groups.h`. The Kanto picture
(`graphics/pokenav/region_map/kanto_map.{png,pal,bin}`) is converted from pokefirered's region map
by `tools/kanto_import/kanto_region_map.pl`. Edit the converter, its tables or its `extras/`, never
the output; re-run both scripts after a change.

**Hand-written**: the table entry in `src/region.c` (picture, `hasPokenavMap = TRUE`, popup theme,
badges 9-16), `REGION_KANTO` in `constants/regions.h`, the marker hooks listed in the converter's
README, and the boat: `sRegionTransitDestinations[]` holds one harbour per region, Slateport City's
ferry terminal and Vermilion City's S.S. ANNE pier. The sailor in Slateport's harbour
(`data/maps/SlateportCity_Harbor/scripts.inc`) sails to Vermilion once `FLAG_SYS_GAME_CLEAR` is
set (Hoenn's Hall of Fame); the sailor on Vermilion's pier (`tools/kanto_import/extras/`) always
sails back to Slateport. Both use `data/scripts/region_transit.inc`.

**Region map.** The sections keep pokefirered's cell coordinates: towns and routes sit on the 22x15
cursor grid as in FRLG; dungeons and indoor-only sections are off the grid but carry the cell of
FRLG's dungeon icon or of the map that leads into them, so the cursor opens where the player is
(the name shown is the dungeon's, moving the cursor shows the route's). Fly spots: the eleven towns
with a heal location (every Pokémon Center town, Pallet Town and the Indigo Plateau), each with a
visited flag set by its `ON_TRANSITION`. `kanto_region_map.pl` places FRLG's picture so that each
cursor cell covers the same piece of the picture as in FRLG (FRLG tile `(gx + 4, gy + 4)` lands on
Emerald's `(gx + 1, gy + 2)`), without FRLG's frame and buttons; 172 8bpp tiles and 25 colours.

## Flag space note

The `REGION_FLAGS_START` block lives in the system-flag extension that PS-070 (save expansion) added
(`SYSTEM_FLAGS_EXT_START`, 0xE00..0xFFF), inside the saved flag space, so region badges and the
Kanto visited flags are saved like any other flag. A compile-time check in `constants/flags.h` keeps the
block inside that range.
