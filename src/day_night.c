// PS-072: day/night palette tinting driven by the game clock. See docs/day-night.md.
//
// How it works
//   - DayNight_Update() runs once per overworld frame (hooked in overworld.c). It reads the
//     game clock (RtcCalcLocalTime, the same clock berries and the wall clock use) once a
//     second and turns the minute of the day into three channel multipliers (sMul*), blending
//     between bands over the last sBands[].blendMinutes of each band.
//   - DayNight_TintPlttBuffer() runs from TransferPlttBuffer() (palette.c) each frame. When
//     the tint is active it writes gPlttBufferFaded * multipliers into PLTT instead of the
//     plain copy. Nothing in gPlttBufferUnfaded / gPlttBufferFaded is changed, so weather
//     color maps, palette fades, BlendPalettes(), sprites loaded later and remote players'
//     avatars all compose with it, and switching it off restores vanilla instantly.
//   - Palette fades still reach their target color: the tint strength shrinks with the fade
//     coefficient of a normal palette fade toward a non-black color, and black stays black
//     because the tint only scales channels (never adds).
//   - Exempt palette slots (sExemptPalettes) are copied unchanged: text boxes and window
//     frames stay readable at night.

#include "global.h"
#include "day_night.h"
#include "event_data.h"
#include "main.h"
#include "overworld.h"
#include "palette.h"
#include "rtc.h"
#include "constants/map_types.h"
#include "constants/rgb.h"

#define MINUTES_PER_DAY     (HOURS_PER_DAY * MINUTES_PER_HOUR)
#define DAY_NIGHT_UNIT      256     // channel multiplier that leaves a color unchanged
#define DAY_NIGHT_MAX_LEVEL 31      // 5-bit color channel
#define CLOCK_REREAD_FRAMES 60      // re-read the RTC once a second (the bands move once a minute)

struct DayNightBand
{
    u16 startMinute;    // minute of the day the band starts at (inclusive); bands are in day order
    u16 blendMinutes;   // the band's last blendMinutes blend linearly into the next band
    u16 r, g, b;        // channel multipliers, DAY_NIGHT_UNIT = unchanged, larger = brighter
};

// The one table to tune. Times are the game clock (what the wall clock in the bedroom shows).
static const struct DayNightBand sBands[DAY_NIGHT_BAND_COUNT] =
{
    [DAY_NIGHT_MORNING] = { .startMinute =  4 * 60, .blendMinutes = 30, .r = 246, .g = 230, .b = 240 }, // soft, slightly pink dawn
    [DAY_NIGHT_DAY]     = { .startMinute = 10 * 60, .blendMinutes = 30, .r = 256, .g = 256, .b = 256 }, // vanilla
    [DAY_NIGHT_EVENING] = { .startMinute = 17 * 60, .blendMinutes = 30, .r = 256, .g = 198, .b = 140 }, // orange sunset
    [DAY_NIGHT_NIGHT]   = { .startMinute = 20 * 60, .blendMinutes = 30, .r = 108, .g = 122, .b = 200 }, // dark blue night
};

// Palette slots that are never tinted (bit n = BG palette n for n < 16, OBJ palette n - 16
// otherwise). 14 is the window frame / standard menu palette, 15 the dialogue box and its
// text, the same two slots the weather color maps leave alone (field_weather.c).
static const u32 sExemptPalettes = (1u << 14) | (1u << 15);

static u16 sMinuteOfDay;        // game clock minute of the day the multipliers were computed for
static u8 sBand;                // DAY_NIGHT_* for sMinuteOfDay
static u16 sMulR, sMulG, sMulB; // current multipliers after the band blend
static bool8 sClockValid;       // the player has set the clock (FLAG_SYS_CLOCK_SET)
static u8 sClockTimer;          // frames until the next RTC read

static u8 FindBand(u16 minute)
{
    u8 band = DAY_NIGHT_BAND_COUNT - 1; // before the first band start: still in the last band (past midnight)
    u8 i;

    for (i = 0; i < DAY_NIGHT_BAND_COUNT; i++)
    {
        if (minute >= sBands[i].startMinute)
            band = i;
    }
    return band;
}

static u16 BlendMultiplier(u16 from, u16 to, u16 step, u16 steps)
{
    return from + (s32)(to - from) * step / steps;
}

static void ComputeMultipliers(u16 minute)
{
    u8 band = FindBand(minute);
    u8 next = (band + 1) % DAY_NIGHT_BAND_COUNT;
    u16 end = sBands[next].startMinute; // first minute of the next band
    u16 blend = sBands[band].blendMinutes;
    u16 untilEnd;

    if (end <= sBands[band].startMinute)
        end += MINUTES_PER_DAY; // the last band wraps past midnight
    if (minute < sBands[band].startMinute)
        minute += MINUTES_PER_DAY;
    untilEnd = end - minute; // 1 .. band length

    sBand = band;
    if (blend == 0 || untilEnd > blend)
    {
        sMulR = sBands[band].r;
        sMulG = sBands[band].g;
        sMulB = sBands[band].b;
    }
    else
    {
        // The last `blend` minutes: step 1 .. blend of blend + 1, so the minute after is the
        // next band exactly and no two minutes look the same.
        u16 step = blend + 1 - untilEnd;

        sMulR = BlendMultiplier(sBands[band].r, sBands[next].r, step, blend + 1);
        sMulG = BlendMultiplier(sBands[band].g, sBands[next].g, step, blend + 1);
        sMulB = BlendMultiplier(sBands[band].b, sBands[next].b, step, blend + 1);
    }
}

void DayNight_Update(void)
{
    u16 minute;

    if (sClockTimer != 0)
    {
        sClockTimer--;
        return;
    }
    sClockTimer = CLOCK_REREAD_FRAMES - 1;

    sClockValid = FlagGet(FLAG_SYS_CLOCK_SET);
    if (!sClockValid)
        return;

    RtcCalcLocalTime(); // the game only refreshes gLocalTime every ~68 s (field_tasks.c); we want the minute
    minute = (u16)gLocalTime.hours * MINUTES_PER_HOUR + gLocalTime.minutes;
    if (minute >= MINUTES_PER_DAY)
        minute = 0; // never with a sane clock; keeps the table lookups in range
    if (minute != sMinuteOfDay || sMulR == 0)
    {
        sMinuteOfDay = minute;
        ComputeMultipliers(minute);
    }
}

static bool8 IsOutdoorMapType(u8 mapType)
{
    // Towns, cities and routes (land and sea). Not underwater, caves, indoors, secret bases.
    return mapType == MAP_TYPE_TOWN
        || mapType == MAP_TYPE_CITY
        || mapType == MAP_TYPE_ROUTE
        || mapType == MAP_TYPE_OCEAN_ROUTE;
}

bool8 DayNight_IsActive(void)
{
    // Only while the overworld itself is on screen: battles, menus, the title and the intro
    // run their own callbacks and load their own palettes. Checked first: the save block
    // pointer is not set during the first boot frames, and this runs from every VBlank.
    if (gMain.callback2 != CB2_Overworld && gMain.callback2 != CB2_OverworldBasic)
        return FALSE;
    if (gSaveBlock2Ptr == NULL || gSaveBlock2Ptr->optionsDayNightOff)
        return FALSE;
    if (!sClockValid || sMulR == 0)
        return FALSE;
    return IsOutdoorMapType(gMapHeader.mapType);
}

// 0 .. 16: how much of the tint to apply this frame. A normal palette fade toward a
// non-black color (white-outs, flashes) replaces the colors by blendColor at y = 16; the
// tint shrinks with it so the screen really reaches that color. Fades to black need no
// help (black * anything = black), and keeping the full tint there keeps the darkening
// natural. gPaletteFade.y keeps its final value after a fade, so a finished white-out
// stays white until the next fade starts.
static u8 GetFadeStrength(void)
{
    if (gPaletteFade.mode == NORMAL_FADE && gPaletteFade.blendColor != RGB_BLACK)
        return 16 - gPaletteFade.y;
    return 16;
}

static void BuildChannelLut(u8 *lut, u16 mul, u8 strength)
{
    s32 effective = DAY_NIGHT_UNIT + ((s32)mul - DAY_NIGHT_UNIT) * strength / 16;
    u32 level;

    for (level = 0; level <= DAY_NIGHT_MAX_LEVEL; level++)
    {
        s32 out = (s32)(level * effective + DAY_NIGHT_UNIT / 2) / DAY_NIGHT_UNIT;

        if (out < 0)
            out = 0;
        if (out > DAY_NIGHT_MAX_LEVEL)
            out = DAY_NIGHT_MAX_LEVEL;
        lut[level] = out;
    }
}

bool8 DayNight_TintPlttBuffer(void)
{
    u8 lutR[DAY_NIGHT_MAX_LEVEL + 1];
    u8 lutG[DAY_NIGHT_MAX_LEVEL + 1];
    u8 lutB[DAY_NIGHT_MAX_LEVEL + 1];
    const u16 *src = gPlttBufferFaded;
    u16 *dst = (u16 *)PLTT;
    u8 strength;
    u32 slot;

    if (!DayNight_IsActive())
        return FALSE;
    if (sMulR == DAY_NIGHT_UNIT && sMulG == DAY_NIGHT_UNIT && sMulB == DAY_NIGHT_UNIT)
        return FALSE; // daytime: vanilla colors, let the caller copy
    strength = GetFadeStrength();
    if (strength == 0)
        return FALSE;

    BuildChannelLut(lutR, sMulR, strength);
    BuildChannelLut(lutG, sMulG, strength);
    BuildChannelLut(lutB, sMulB, strength);

    for (slot = 0; slot < PLTT_BUFFER_SIZE / 16; slot++)
    {
        u32 i;

        if (sExemptPalettes & (1u << slot))
        {
            for (i = 0; i < 16; i++)
                dst[i] = src[i];
        }
        else
        {
            for (i = 0; i < 16; i++)
            {
                u16 color = src[i];

                dst[i] = (color & 0x8000)
                       | RGB(lutR[color & 0x1F], lutG[(color >> 5) & 0x1F], lutB[(color >> 10) & 0x1F]);
            }
        }
        src += 16;
        dst += 16;
    }
    return TRUE;
}

u8 DayNight_GetBand(void)
{
    return sBand;
}
