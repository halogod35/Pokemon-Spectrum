// PS-071: region table and lookups. Hoenn is entry 0 and keeps every value it had; other regions
// are data-driven entries below. docs/regions.md has the checklist for adding one.
#include "global.h"
#include "region.h"
#include "event_data.h"
#include "field_screen_effect.h"
#include "map_name_popup.h"
#include "overworld.h"
#include "region_map.h"
#include "string_util.h"
#include "strings.h"
#include "constants/flags.h"
#include "constants/heal_locations.h"
#include "constants/maps.h"
#include "constants/region_map_sections.h"

STATIC_ASSERT(METLOC_REGION_START == MAPSEC_NONE + 1, MetLocRegionStartFollowsMapSecNone);
STATIC_ASSERT(METLOC_REGION_END < METLOC_SPECIAL_EGG, MetLocRegionEndBeforeSpecials);
STATIC_ASSERT(MAPSEC_REGIONS_START > MAPSEC_NONE, MapSecRegionsStartAboveHoenn);
STATIC_ASSERT(REGION_BADGE_COUNT == NUM_BADGES, RegionBadgeCountMatchesHoenn);

const u8 gText_RegionLRButtons[] = _("{L_BUTTON}{R_BUTTON}");

static const u8 sText_RegionName_Hoenn[] = _("HOENN");
static const u8 sText_RegionName_Template[] = _("TEMPLATE"); // placeholder region, to be replaced

// Region map backgrounds. The tiles and palette are shared by every region for now; a region
// only needs its own 64x64 byte tilemap (see docs/regions.md).
static const u16 sHoennRegionMap_Pal[] = INCGFX_U16("graphics/pokenav/region_map/map.pal", ".gbapal");
static const u32 sHoennRegionMap_GfxLZ[] = INCGFX_U32("graphics/pokenav/region_map/map.png", ".8bpp.lz", "-num_tiles 233 -Wnum_tiles");
static const u32 sHoennRegionMap_TilemapLZ[] = INCGFX_U32("graphics/pokenav/region_map/map.bin", ".lz");
static const u32 sTemplateRegionMap_TilemapLZ[] = INCGFX_U32("graphics/pokenav/region_map/template_map.bin", ".lz");

#include "data/region_map/region_map_layout.h"
#include "data/region_map/template_region_map_layout.h"

static const struct RegionFlySpot sTemplateRegionFlySpots[] =
{
    { MAPSEC_TEMPLATE_TOWN, HEAL_LOCATION_TEMPLATE_TOWN, FLAG_VISITED_TEMPLATE_TOWN },
};

static const struct RegionInfo sRegions[REGIONS_COUNT] =
{
    [REGION_HOENN] =
    {
        .name = sText_RegionName_Hoenn,
        .mapSecStart = 0,
        .mapSecEnd = MAPSEC_REGIONS_START - 1, // every id of Emerald's own table, MAPSEC_NONE and the specials included
        .metLocation = 0,                      // Hoenn stores the map section itself (all below 0xD6)
        .mapLayout = sHoennRegionMapLayout,
        .mapTilesLZ = sHoennRegionMap_GfxLZ,
        .mapTilemapLZ = sHoennRegionMap_TilemapLZ,
        .mapPalette = sHoennRegionMap_Pal,
        .flySpots = NULL,                      // region_map.c keeps Emerald's Fly tables for Hoenn
        .flySpotCount = 0,
        .hasPokenavMap = TRUE,
        .popupTheme = MAPPOPUP_THEME_WOOD,     // unused: Hoenn has a theme per map section
        .firstBadgeFlag = FLAG_BADGE01_GET,
    },
    [REGION_TEMPLATE] =
    {
        .name = sText_RegionName_Template,
        .mapSecStart = MAPSEC_TEMPLATE_TOWN,
        .mapSecEnd = MAPSEC_TEMPLATE_ROUTE,
        .metLocation = METLOC_REGION_START,
        .mapLayout = sTemplateRegionMapLayout,
        .mapTilesLZ = sHoennRegionMap_GfxLZ,
        .mapTilemapLZ = sTemplateRegionMap_TilemapLZ,
        .mapPalette = sHoennRegionMap_Pal,
        .flySpots = sTemplateRegionFlySpots,
        .flySpotCount = ARRAY_COUNT(sTemplateRegionFlySpots),
        .hasPokenavMap = TRUE,
        .popupTheme = MAPPOPUP_THEME_WOOD,
        .firstBadgeFlag = FLAG_BADGE09_GET,
    },
};

// Region transit destinations: where a ferry / train NPC can take the player. A destination is
// offered when its flag is set and (unless asked otherwise) it lies in another region than the
// player's current one. Add one entry per hub when adding a region.
struct RegionTransitDestination
{
    mapsec_t mapSecId; // name shown in the menu
    u16 map;           // MAP_* to warp to
    u8 x;
    u8 y;
    u16 flag;          // FLAG_* that unlocks it
};

static const struct RegionTransitDestination sRegionTransitDestinations[] =
{
    { MAPSEC_LITTLEROOT_TOWN, MAP_LITTLEROOT_TOWN, 9, 10, FLAG_VISITED_LITTLEROOT_TOWN },
    { MAPSEC_TEMPLATE_TOWN,   MAP_TEMPLATE_TOWN,   9, 12, FLAG_VISITED_TEMPLATE_TOWN },
};

static EWRAM_DATA u8 sRegionTransitList[REGION_TRANSIT_MAX_ROWS] = {0};
static EWRAM_DATA u8 sRegionTransitListCount = 0;

const struct RegionInfo *GetRegionInfo(u8 region)
{
    if (region >= REGIONS_COUNT)
        region = REGION_HOENN;
    return &sRegions[region];
}

u8 GetRegionOfMapSec(mapsec_t mapSecId)
{
    u32 i;

    for (i = 0; i < REGIONS_COUNT; i++)
    {
        if (mapSecId >= sRegions[i].mapSecStart && mapSecId <= sRegions[i].mapSecEnd)
            return i;
    }
    return REGION_HOENN;
}

u8 GetRegionOfMap(u8 mapGroup, u8 mapNum)
{
    const struct MapHeader *header = Overworld_GetMapHeaderByGroupAndId(mapGroup, mapNum);

    if (header == NULL)
        return REGION_HOENN;
    return GetRegionOfMapSec(header->regionMapSectionId);
}

u8 GetCurrentRegion(void)
{
    return GetRegionOfMap(gSaveBlock1Ptr->location.mapGroup, gSaveBlock1Ptr->location.mapNum);
}

bool8 IsRegionBadgeSet(u8 region, u8 badge)
{
    if (badge >= REGION_BADGE_COUNT)
        return FALSE;
    return FlagGet(GetRegionInfo(region)->firstBadgeFlag + badge);
}

u8 GetRegionBadgeCount(u8 region)
{
    u8 i;
    u8 count = 0;

    for (i = 0; i < REGION_BADGE_COUNT; i++)
    {
        if (IsRegionBadgeSet(region, i))
            count++;
    }
    return count;
}

// Hoenn is always visited (the game starts there). Another region is visited once any of its
// Fly spots is unlocked.
bool8 IsRegionVisited(u8 region)
{
    const struct RegionInfo *info;
    u32 i;

    if (region == REGION_HOENN)
        return TRUE;
    if (region >= REGIONS_COUNT)
        return FALSE;
    info = &sRegions[region];
    for (i = 0; i < info->flySpotCount; i++)
    {
        if (FlagGet(info->flySpots[i].visitedFlag))
            return TRUE;
    }
    return FALSE;
}

u8 GetVisitedRegionCount(void)
{
    u8 i;
    u8 count = 0;

    for (i = 0; i < REGIONS_COUNT; i++)
    {
        if (IsRegionVisited(i))
            count++;
    }
    return count;
}

// The next visited region after (direction > 0) or before (direction < 0) the given one,
// wrapping around. Returns the same region when it is the only one visited.
u8 GetNextVisitedRegion(u8 region, s8 direction)
{
    u8 i;
    u8 next = region;

    if (region >= REGIONS_COUNT)
        return REGION_HOENN;
    for (i = 0; i < REGIONS_COUNT; i++)
    {
        if (direction < 0)
            next = (next == 0) ? REGIONS_COUNT - 1 : next - 1;
        else
            next = (next + 1) % REGIONS_COUNT;
        if (IsRegionVisited(next))
            return next;
    }
    return region;
}

// The region the PokéNav map should show for a player in the given region: that region if it
// has a PokéNav map, else the nearest visited region that has one (Hoenn in the end).
u8 GetRegionForPokenavMap(u8 region)
{
    u8 i;
    u8 next = region;

    if (region >= REGIONS_COUNT)
        return REGION_HOENN;
    if (sRegions[region].hasPokenavMap)
        return region;
    for (i = 0; i < REGIONS_COUNT; i++)
    {
        next = (next == 0) ? REGIONS_COUNT - 1 : next - 1;
        if (IsRegionVisited(next) && sRegions[next].hasPokenavMap)
            return next;
    }
    return REGION_HOENN;
}

const struct RegionFlySpot *GetRegionFlySpot(u8 region, mapsec_t mapSecId)
{
    const struct RegionInfo *info = GetRegionInfo(region);
    u32 i;

    for (i = 0; i < info->flySpotCount; i++)
    {
        if (info->flySpots[i].mapSecId == mapSecId)
            return &info->flySpots[i];
    }
    return NULL;
}

// TRUE if gRegionMapEntries[mapSecId] holds a name: Hoenn's own sections and every section of
// the other regions. MAPSEC_NONE, the met-location aliases and the METLOC_* specials have none.
bool8 MapSecHasName(mapsec_t mapSecId)
{
    if (mapSecId < MAPSEC_NONE)
        return TRUE;
    if (mapSecId >= MAPSEC_REGIONS_START && mapSecId < MAPSEC_COUNT)
        return TRUE;
    return FALSE;
}

// ---------------------------------------------------------------------------
// Met locations. Pokémon box data and TV shows store a u8: Hoenn sections fit as they are,
// a map in any other region stores that region's alias (METLOC_REGION_START..END), which
// GetMapName shows as the region's name.
// ---------------------------------------------------------------------------

metloc_u8_t GetMetLocationForMapSec(mapsec_t mapSecId)
{
    const struct RegionInfo *info = &sRegions[GetRegionOfMapSec(mapSecId)];

    if (info->metLocation != 0)
        return info->metLocation;
    if (mapSecId > MAPSEC_NONE)
        return MAPSEC_NONE; // never expected: a Hoenn id that does not fit a u8
    return mapSecId;
}

metloc_u8_t GetCurrentMetLocation(void)
{
    return GetMetLocationForMapSec(GetCurrentRegionMapSectionId());
}

u8 GetRegionOfMetLocation(metloc_u8_t metLocation)
{
    u32 i;

    if (metLocation < METLOC_REGION_START || metLocation > METLOC_REGION_END)
        return REGION_NONE;
    for (i = 1; i < REGIONS_COUNT; i++)
    {
        if (sRegions[i].metLocation == metLocation)
            return i;
    }
    return REGION_NONE;
}

bool8 MetLocationHasName(metloc_u8_t metLocation)
{
    if (metLocation < MAPSEC_NONE)
        return TRUE;
    return GetRegionOfMetLocation(metLocation) != REGION_NONE;
}

// ---------------------------------------------------------------------------
// Region transit helper. script_menu.c draws the menu from this list.
// ---------------------------------------------------------------------------

u8 RegionTransit_BuildList(bool8 includeCurrentRegion)
{
    u8 currentRegion = GetCurrentRegion();
    u32 i;

    sRegionTransitListCount = 0;
    for (i = 0; i < ARRAY_COUNT(sRegionTransitDestinations) && sRegionTransitListCount < REGION_TRANSIT_MAX_ROWS - 1; i++)
    {
        const struct RegionTransitDestination *dest = &sRegionTransitDestinations[i];

        if (!FlagGet(dest->flag))
            continue;
        if (!includeCurrentRegion && GetRegionOfMapSec(dest->mapSecId) == currentRegion)
            continue;
        sRegionTransitList[sRegionTransitListCount++] = i;
    }
    return sRegionTransitListCount;
}

u8 RegionTransit_GetListCount(void)
{
    return sRegionTransitListCount;
}

u8 *RegionTransit_GetListName(u8 row, u8 *dest)
{
    if (row >= sRegionTransitListCount)
        return StringCopy(dest, gText_Exit);
    return GetMapName(dest, sRegionTransitDestinations[sRegionTransitList[row]].mapSecId, 0);
}

// Index into the destination table for a menu row, or REGION_TRANSIT_CANCEL for the EXIT row.
u8 RegionTransit_GetListDestination(u8 row)
{
    if (row >= sRegionTransitListCount)
        return REGION_TRANSIT_CANCEL;
    return sRegionTransitList[row];
}

// Script special: VAR_RESULT = number of destinations reachable from here (VAR_0x8004 = 1 counts
// the current region's hubs too).
void RegionTransit_CountDestinations(void)
{
    gSpecialVar_Result = RegionTransit_BuildList(gSpecialVar_0x8004 == 1);
}

// Script special: warp to destination VAR_RESULT (as returned by GetRegionTransitSelection).
// Follow it with waitstate, like the warp command.
void RegionTransit_Warp(void)
{
    const struct RegionTransitDestination *dest;

    if (gSpecialVar_Result >= ARRAY_COUNT(sRegionTransitDestinations))
        return;
    dest = &sRegionTransitDestinations[gSpecialVar_Result];
    SetWarpDestination(MAP_GROUP(dest->map), MAP_NUM(dest->map), WARP_ID_NONE, dest->x, dest->y);
    DoWarp();
    ResetInitialPlayerAvatarState();
}
