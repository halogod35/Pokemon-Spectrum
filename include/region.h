#ifndef GUARD_REGION_H
#define GUARD_REGION_H

#include "constants/regions.h"

// PS-071: region table. One entry per region (Hoenn plus the user's own regions). Everything a
// region needs that is not plain map data lives here: name, map section range, region map
// graphics and layout, Fly spots, PokéNav map availability, badge flags, popup theme.
// See docs/regions.md for the "add a region" checklist.

#define REGION_MAP_WIDTH  28 // cells of the region map that the cursor can visit
#define REGION_MAP_HEIGHT 15

struct RegionFlySpot
{
    mapsec_t mapSecId;    // map section the Fly icon sits on (its gRegionMapEntries position)
    u16 healLocation;     // HEAL_LOCATION_* the player lands on
    u16 visitedFlag;      // FLAG_* that unlocks it (and marks the region as visited)
};

struct RegionInfo
{
    const u8 *name;                                // Fly map, wall map, trainer card, met location. Up to 10 characters fit the Fly window.
    mapsec_t mapSecStart;                          // inclusive map section id range of the region
    mapsec_t mapSecEnd;
    metloc_u8_t metLocation;                       // u8 stored in Pokémon/TV data for maps of this region (0: the map section itself)
    const mapsec_t (*mapLayout)[REGION_MAP_WIDTH]; // REGION_MAP_HEIGHT rows: the map section under each region map cell
    const u32 *mapTilesLZ;                         // region map background: 8bpp tiles (LZ), 64x64 byte tilemap (LZ), 3 palettes
    const u32 *mapTilemapLZ;
    const u16 *mapPalette;
    const struct RegionFlySpot *flySpots;          // NULL for Hoenn: region_map.c keeps the original Hoenn tables
    u8 flySpotCount;
    bool8 hasPokenavMap;                           // FALSE: the PokéNav map shows another visited region that has one
    u8 popupTheme;                                 // MAPPOPUP_THEME_* used by map name popups in this region
    u16 firstBadgeFlag;                            // REGION_BADGE_COUNT consecutive flags
};

const struct RegionInfo *GetRegionInfo(u8 region);
u8 GetRegionOfMapSec(mapsec_t mapSecId);
u8 GetRegionOfMap(u8 mapGroup, u8 mapNum);
u8 GetCurrentRegion(void);
u8 GetRegionBadgeCount(u8 region);
bool8 IsRegionBadgeSet(u8 region, u8 badge);
bool8 IsRegionVisited(u8 region);
u8 GetVisitedRegionCount(void);
u8 GetNextVisitedRegion(u8 region, s8 direction);
u8 GetRegionForPokenavMap(u8 region);
const struct RegionFlySpot *GetRegionFlySpot(u8 region, mapsec_t mapSecId);
bool8 MapSecHasName(mapsec_t mapSecId);

// Met locations (u8 in Pokémon and TV data)
metloc_u8_t GetMetLocationForMapSec(mapsec_t mapSecId);
metloc_u8_t GetCurrentMetLocation(void);
u8 GetRegionOfMetLocation(metloc_u8_t metLocation);
bool8 MetLocationHasName(metloc_u8_t metLocation);

// Region transit helper (ferry / train NPCs), see data/scripts/region_transit.inc
u8 RegionTransit_BuildList(bool8 includeCurrentRegion);
u8 RegionTransit_GetListCount(void);
u8 *RegionTransit_GetListName(u8 row, u8 *dest);
u8 RegionTransit_GetListDestination(u8 row);
void RegionTransit_CountDestinations(void);
void RegionTransit_Warp(void);

extern const u8 gText_RegionLRButtons[];

#endif // GUARD_REGION_H
