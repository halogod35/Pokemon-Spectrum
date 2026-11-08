#ifndef GUARD_PLATFORM_H
#define GUARD_PLATFORM_H

#include "global.h"
#include "siirtc.h"

void Platform_StoreSaveFile(void);
// MP: BUG-003, --save-selftest: the trade's two-phase link save on a real save (save_link_selftest.c).
// `path` must equal gPlatformArgs.savePath. Returns the number of failures.
u32 SaveSelftest_LinkSave(const char *path);
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
// See docs/multiplayer/dev-harness.md.

#define PLATFORM_DEFAULT_SAVE_PATH  "pokeemerald.sav"
#define PLATFORM_MP_DEFAULT_PORT    27500
#define PLATFORM_MAX_SCALE          10

// Buffer sizes, including the terminating NUL.
#define PLATFORM_ARG_PATH_SIZE      512
#define PLATFORM_ARG_ADDRESS_SIZE   256
#define PLATFORM_ARG_NAME_SIZE      32
#define PLATFORM_ARG_TITLE_SIZE     128
#define PLATFORM_ARG_FAKE_BUILD_SIZE 16  // MP: --mp-fake-build, 15 chars + NUL

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
    bool8 mpSelftest;                            // --mp-selftest (MP: PS-011)
    bool8 mpLinkTest;                            // --mp-link-test (MP: PS-046 virtual link cable harness)
    bool8 debugSlowSave;                         // --debug-slow-save (MP: PS-014, kill-during-save test)
    bool8 saveSelftest;                          // --save-selftest (MP: PS-014)
    bool8 hasFakeProtocol;                       // --mp-fake-protocol given (MP: PS-012)
    u16 fakeProtocol;                            // --mp-fake-protocol <0..65535>
    char fakeBuild[PLATFORM_ARG_FAKE_BUILD_SIZE]; // --mp-fake-build <1..15 printable ASCII>, empty if not given
    bool8 hasName;                               // TRUE if --name was given
    u16 port;                                    // listen port (host) or remote port (client), default PLATFORM_MP_DEFAULT_PORT
    char joinAddress[PLATFORM_ARG_ADDRESS_SIZE]; // host part of --join <ip[:port]>, empty otherwise
    char name[PLATFORM_ARG_NAME_SIZE];           // --name (UTF-8, as typed), empty if not given
    char windowTitle[PLATFORM_ARG_TITLE_SIZE];   // --title, or the default for the role
    u32 scale;                                   // --scale, 1..PLATFORM_MAX_SCALE, default 1
};

extern struct PlatformArgs gPlatformArgs;

// MP: keyboard entry and settings file for the Join Game IP screen (PS-021).
// Implemented in src/platform/sdl2_text_input.c (no-ops on non-SDL platforms).
// All strings are ASCII and NUL-terminated.
#define PLATFORM_TEXT_INPUT_QUEUE_SIZE  64
#define PLATFORM_TEXT_BACKSPACE         0x08    // queued for the Backspace key (ASCII BS)
// Starts queueing typed characters (printable ASCII, plus PLATFORM_TEXT_BACKSPACE) and
// Ctrl+V pastes. Clears anything queued before.
void Platform_StartTextInput(void);
void Platform_StopTextInput(void);
// Moves up to `outSize` queued characters into `out` (NOT NUL-terminated); returns how many.
u32 Platform_ReadTextInput(char *out, u32 outSize);
// If Ctrl+V was pressed since the last call, copies the clipboard text (printable ASCII only,
// truncated to outSize - 1) into `out` and returns TRUE.
bool8 Platform_TakePastedText(char *out, u32 outSize);
// First line of "<savePath>.mp.cfg" (the last Join Game address, "a.b.c.d:port"), without the
// line break. FALSE if the file doesn't exist or can't be read.
bool8 Platform_ReadMpConfig(char *out, u32 outSize);
// Replaces "<savePath>.mp.cfg" with `line` plus a line break. FALSE on error.
bool8 Platform_WriteMpConfig(const char *line);

// MP: this machine's IPv4 addresses for the Host Game screen (PS-022), excluding loopback and
// link-local. Writes up to `max` dotted quads separated by single spaces into `out` (ASCII,
// NUL-terminated) and returns how many. Never blocks (no DNS, no packets sent).
// Implemented in src/platform/net_info.c.
#define PLATFORM_LAN_ADDRESSES_MAX 4
u32 Platform_GetLanAddresses(char *out, u32 outSize, u32 max);
#endif // PORTABLE

#endif