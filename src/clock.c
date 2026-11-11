#include <stdio.h> // PC: PS-062 clock migration log line
#include "global.h"
#include "berry.h"
#include "dewford_trend.h"
#include "event_data.h"
#include "field_specials.h"
#include "field_weather.h"
#include "main.h"
#include "lottery_corner.h"
#include "overworld.h"
#include "rtc.h"
#include "time_events.h"
#include "tv.h"
#include "wallclock.h"

static void UpdatePerDay(struct Time *localTime);
static void UpdatePerMinute(struct Time *localTime);

static void InitTimeBasedEvents(void)
{
    FlagSet(FLAG_SYS_CLOCK_SET);
    RtcCalcLocalTime();
    gSaveBlock2Ptr->lastBerryTreeUpdate = gLocalTime;
    VarSet(VAR_DAYS, gLocalTime.days);
}

// PC: PS-062. Before the fix, the PC clock was frozen at 2000-01-01 00:00 (RTC error fallback), so
// a save whose clock was set then has localTimeOffset.days of about 0, while a clock set on a
// working RTC stores the RTC's day count then (2025+ is over 9000). With the real clock, such a
// save would suddenly see ~26 years pass: every berry tree would be wiped (BerryTreeTimeUpdate)
// and 26 years of daily updates would run. Instead rebase it once: keep its in-game day, take the
// time of day from the PC clock, and restart berry growth from now.
#define FROZEN_CLOCK_MAX_OFFSET_DAYS 1000
#define WORKING_CLOCK_MIN_DAYS       5000

static void MigrateFrozenClockSave(void)
{
    struct SiiRtcInfo rtc;
    s32 days;

    if (RtcGetErrorStatus() & RTC_ERR_FLAG_MASK)
        return; // still no working clock: leave the save alone
    if (gSaveBlock2Ptr->localTimeOffset.days >= FROZEN_CLOCK_MAX_OFFSET_DAYS)
        return;
    RtcGetInfo(&rtc);
    if (RtcGetDayCount(&rtc) < WORKING_CLOCK_MIN_DAYS)
        return; // the PC's own date is near 2000; nothing to rebase against

    days = VarGet(VAR_DAYS);
    if (gSaveBlock2Ptr->lastBerryTreeUpdate.days > days)
        days = gSaveBlock2Ptr->lastBerryTreeUpdate.days;
    if (days < 0)
        days = 0;
    RtcCalcLocalTimeOffset(days, ConvertBcdToBinary(rtc.hour), ConvertBcdToBinary(rtc.minute), ConvertBcdToBinary(rtc.second));
    RtcCalcLocalTime();
    gSaveBlock2Ptr->lastBerryTreeUpdate = gLocalTime;
    printf("[clock] save used the frozen pre-fix clock: rebased to day %d, %02d:%02d (berries grow from now)\n",
           (int)gLocalTime.days, (int)gLocalTime.hours, (int)gLocalTime.minutes);
}

void DoTimeBasedEvents(void)
{
    if (FlagGet(FLAG_SYS_CLOCK_SET))
        MigrateFrozenClockSave(); // PC: PS-062
    if (FlagGet(FLAG_SYS_CLOCK_SET) && !InPokemonCenter())
    {
        RtcCalcLocalTime();
        UpdatePerDay(&gLocalTime);
        UpdatePerMinute(&gLocalTime);
    }
}

static void UpdatePerDay(struct Time *localTime)
{
    u16 *days = GetVarPointer(VAR_DAYS);
    u16 daysSince;

    if (*days != localTime->days && *days <= localTime->days)
    {
        daysSince = localTime->days - *days;
        ClearDailyFlags();
        UpdateDewfordTrendPerDay(daysSince);
        UpdateTVShowsPerDay(daysSince);
        UpdateWeatherPerDay(daysSince);
        UpdatePartyPokerusTime(daysSince);
        UpdateMirageRnd(daysSince);
        UpdateBirchState(daysSince);
        UpdateFrontierManiac(daysSince);
        UpdateFrontierGambler(daysSince);
        SetShoalItemFlag(daysSince);
        SetRandomLotteryNumber(daysSince);
        *days = localTime->days;
    }
}

static void UpdatePerMinute(struct Time *localTime)
{
    struct Time difference;
    int minutes;

    CalcTimeDifference(&difference, &gSaveBlock2Ptr->lastBerryTreeUpdate, localTime);
    minutes = 24 * 60 * difference.days + 60 * difference.hours + difference.minutes;
    if (minutes != 0)
    {
        if (minutes >= 0)
        {
            BerryTreeTimeUpdate(minutes);
            gSaveBlock2Ptr->lastBerryTreeUpdate = *localTime;
        }
    }
}

static void ReturnFromStartWallClock(void)
{
    InitTimeBasedEvents();
    SetMainCallback2(CB2_ReturnToFieldContinueScriptPlayMapMusic);
}

void StartWallClock(void)
{
    SetMainCallback2(CB2_StartWallClock);
    gMain.savedCallback = ReturnFromStartWallClock;
}
