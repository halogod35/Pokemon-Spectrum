#ifndef GUARD_CONSTANTS_REGIONS_H
#define GUARD_CONSTANTS_REGIONS_H

// PS-071: regions. This header is also included by data/event_scripts.s, so keep it to #defines.
// The table with each region's data is sRegions in src/region.c; docs/regions.md explains how to add one.

#define REGION_HOENN             0
#define REGION_TEMPLATE          1 // placeholder region made from Emerald's own tilesets, to be replaced (docs/regions.md)
#define REGIONS_COUNT            2
#define REGION_NONE              0xFF

#define REGION_BADGE_COUNT       8 // badges per region (NUM_BADGES in Hoenn)

// Map section ids below 0x100 belong to Hoenn (constants/region_map_sections.h, unchanged from
// Emerald). Every other region's sections start here, so Hoenn ids and the u8 met locations in
// Pokémon data keep their values.
#define MAPSEC_REGIONS_START     0x100

// Pokémon met locations and TV show locations are stored as a u8. A map in a region whose
// sections are above 0xFF stores the region's alias in this range instead (one per region),
// shown as the region's name ("met in TEMPLATE"). 0xD6 is MAPSEC_NONE + 1; 0xFD-0xFF are the
// METLOC_* specials.
#define METLOC_REGION_START      0xD6
#define METLOC_REGION_END        0xFC

// Region transit helper (data/scripts/region_transit.inc)
#define REGION_TRANSIT_CANCEL    0xFF // VAR_RESULT after the menu when EXIT or B was chosen
#define REGION_TRANSIT_MAX_ROWS  6    // destinations shown at once (the last row is EXIT)

#endif // GUARD_CONSTANTS_REGIONS_H
