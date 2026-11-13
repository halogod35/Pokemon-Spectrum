#ifndef GUARD_DAY_NIGHT_H
#define GUARD_DAY_NIGHT_H

// PS-072: day/night palette tinting driven by the game clock (see docs/day-night.md).
//
// The tint is applied once per frame when gPlttBufferFaded is transferred to PLTT, so the
// palette buffers the game reads and writes (weather color maps, fades, late sprite loads)
// are never modified: the tint composes with all of them and is gone the moment it is
// switched off.

enum
{
    DAY_NIGHT_MORNING,
    DAY_NIGHT_DAY,
    DAY_NIGHT_EVENING,
    DAY_NIGHT_NIGHT,
    DAY_NIGHT_BAND_COUNT
};

// Once per overworld frame: re-reads the clock and recomputes the band multipliers
// (once a minute; immediately when the clock or the band table changes).
void DayNight_Update(void);

// TRUE while the tint should be drawn: the option is on, the clock has been set, the
// overworld is the active screen and the current map is outdoors.
bool8 DayNight_IsActive(void);

// Copies gPlttBufferFaded into PLTT through the tint. Returns FALSE (and copies nothing)
// when the tint is inactive or the current band leaves colors unchanged, so the caller
// does the plain copy.
bool8 DayNight_TintPlttBuffer(void);

// The band the clock is in right now (DAY_NIGHT_*), for debugging and tests.
u8 DayNight_GetBand(void);

#endif // GUARD_DAY_NIGHT_H
