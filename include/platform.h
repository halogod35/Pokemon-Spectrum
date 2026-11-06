#ifndef GUARD_PLATFORM_H
#define GUARD_PLATFORM_H

#include "global.h"
#include "siirtc.h"

void Platform_StoreSaveFile(void);
void Platform_ReadFlash(u16 sectorNum, u32 offset, u8 *dest, u32 size);
void Platform_QueueAudio(float *audioBuffer, s32 samplesPerFrame);
u16 Platform_GetKeyInput(void);
void Platform_GetStatus(struct SiiRtcInfo *rtc);
void Platform_SetStatus(struct SiiRtcInfo *rtc);
static void UpdateInternalClock(void);
void Platform_GetDateTime(struct SiiRtcInfo *rtc);
void Platform_SetDateTime(struct SiiRtcInfo *rtc);
void Platform_GetTime(struct SiiRtcInfo *rtc);
void Platform_SetTime(struct SiiRtcInfo *rtc);
void Platform_SetAlarm(u8 *alarmData);

#ifdef PORTABLE
// Command-line options of the PC build (parsed in src/platform/sdl2.c main()).
// See docs/multiplayer/testing.md.

#define PLATFORM_DEFAULT_SAVE_PATH  "pokeemerald.sav"
#define PLATFORM_MP_DEFAULT_PORT    27500
#define PLATFORM_MAX_SCALE          10

// Buffer sizes, including the terminating NUL.
#define PLATFORM_ARG_PATH_SIZE      512
#define PLATFORM_ARG_ADDRESS_SIZE   256
#define PLATFORM_ARG_NAME_SIZE      32
#define PLATFORM_ARG_TITLE_SIZE     128

enum
{
    PLATFORM_MP_ROLE_NONE,   // single-player (no --host / --join)
    PLATFORM_MP_ROLE_HOST,   // --host
    PLATFORM_MP_ROLE_CLIENT, // --join
};

struct PlatformArgs
{
    char savePath[PLATFORM_ARG_PATH_SIZE];       // --save, default PLATFORM_DEFAULT_SAVE_PATH
    u8 mpRole;                                   // PLATFORM_MP_ROLE_*
    bool8 mpDebug;                               // --mp-debug
    bool8 hasName;                               // TRUE if --name was given
    u16 port;                                    // listen port (host) or remote port (client), default PLATFORM_MP_DEFAULT_PORT
    char joinAddress[PLATFORM_ARG_ADDRESS_SIZE]; // host part of --join <ip[:port]>, empty otherwise
    char name[PLATFORM_ARG_NAME_SIZE];           // --name (UTF-8, as typed), empty if not given
    char windowTitle[PLATFORM_ARG_TITLE_SIZE];   // --title, or the default for the role
    u32 scale;                                   // --scale, 1..PLATFORM_MAX_SCALE, default 1
};

extern struct PlatformArgs gPlatformArgs;
#endif // PORTABLE

#endif