#ifdef PLATFORM_SDL2
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <xinput.h>
#include <io.h>      // MP: _commit() for the crash-safe save (PS-014)
#include <stdint.h>  // MP: uintptr_t for the crash reporter
#else
#include <unistd.h>  // MP: fsync() for the crash-safe save (PS-014)
#endif

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h> // PS-076: the window handle for the drag audio hook

#include "global.h"
#include "platform.h"
#include "rtc.h"
#include "gba/defines.h"
#include "gba/m4a_internal.h"
#include "cgb_audio.h"
#include "gba/flash_internal.h"
#include "platform/dma.h"
#include "platform/framedraw.h"
#include "platform/system.h"
#include "mp/mp.h" // MP:
#include "platform/text_input.h" // MP: Join Game IP entry keyboard input (PS-021)
#include "mp_menu.h" // MP: MpUi_Update, the single MP event consumer (PS-022)
#include "platform/net_overlay.h" // MP: PS-053 F3 network overlay
#include "platform/audio.h" // PS-058 audio output, --audio-stats, --audio-dump
#include "platform/input.h" // PS-066 key / controller remapping

extern void (*const gIntrTable[])(void);

SDL_Thread *mainLoopThread;
static char **sArgv; // PS-060: the command line, so a soft reset can restart with it (Linux)
SDL_Window *sdlWindow;
SDL_Renderer *sdlRenderer;
SDL_Texture *sdlTexture;
SDL_sem *vBlankSemaphore;
SDL_atomic_t isFrameAvailable;
bool speedUp = false;
unsigned int videoScale = 1;
bool videoScaleChanged = false;
bool isRunning = true;
bool paused = false;
double simTime = 0;
double lastGameTime = 0;
double curGameTime = 0;
double fixedTimestep = 1.0 / 60.0; // 16.666667ms
#define MAX_CATCHUP_TICKS 4 // PS-076: game ticks replayed at most after a stall (about 67 ms)
double timeScale = 1.0;
struct SiiRtcInfo internalClock;

// MP: PS-014. The save file is no longer kept open. FLASH_BASE is the authoritative copy:
// ReadSaveFile fills it once at startup, Platform_ReadFlash reads from it, and
// Platform_StoreSaveFile replaces the file atomically (write "<save>.tmp", flush, rename).
#define SAVE_TMP_SUFFIX          ".tmp"
#define SAVE_SELFTEST_SUFFIX     ".selftest"
#define SAVE_CHUNK_SIZE          0x1000  // one flash sector
#define SAVE_FLASH_SECTOR_SHIFT  12      // DUMMY_SAVE sector shift, used if gFlash is not set yet
#define SAVE_SLOW_DELAY_MS       50      // --debug-slow-save: per sector, 36 sectors = 1.8 s per file write
#define SAVE_REPLACE_RETRIES     5       // Windows: retry if another process briefly holds the file

// PS-070: the save format version of the loaded flash image (save.h, src/save.c); save.h
// itself clashes with this file's gIntrTable, so it is declared here.
u32 Save_FlashFormatVersion(void);

struct PlatformArgs gPlatformArgs;

extern void AgbMain(void);
extern void MainLoop(void);
extern void DoSoftReset(void);

int DoMain(void *param);
void ProcessEvents(void);
void VDraw(SDL_Texture *texture);

static bool ReadSaveFile(const char *path);
static bool StoreSaveFile(void);
static bool WriteSaveFileAtomic(const char *path, const u8 *data, u32 size, u32 delayMsPerSector);
static int RunSaveSelftest(void);

static int ParsePlatformArgs(int argc, char **argv);
static void ThrottleLoop(void);

static void UpdateInternalClock(void);

#ifdef _WIN32
// MP: crash reporter (BUG-003). On an unhandled exception, print the faulting address and a
// stack of return addresses as offsets from the exe's base, to the console and to
// pokemon-spectrum_crash.txt next to the exe. Match the offsets against pokemon-spectrum.map (shipped with
// the CI artifact): offset + 0x140000000 (64-bit) or + 0x400000 (32-bit) = the address in the map.
static void MpCrashWrite(FILE *out, EXCEPTION_POINTERS *info, void **frames, USHORT count)
{
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    uintptr_t at = (uintptr_t)info->ExceptionRecord->ExceptionAddress;
    USHORT i;

    fprintf(out, "\n=== POKEMON SPECTRUM CRASH ===\n");
    fprintf(out, "exception 0x%08lX at exe+0x%llx (address %p, exe base %p)\n",
            (unsigned long)info->ExceptionRecord->ExceptionCode,
            (unsigned long long)(at - base), (void *)at, (void *)base);
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION
     && info->ExceptionRecord->NumberParameters >= 2)
        fprintf(out, "access violation: %s address %p\n",
                info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                (void *)info->ExceptionRecord->ExceptionInformation[1]);
    fprintf(out, "stack (exe offsets; frames outside the exe are DLLs):\n");
    for (i = 0; i < count; i++)
    {
        uintptr_t f = (uintptr_t)frames[i];

        fprintf(out, "  #%02u exe+0x%llx\n", (unsigned int)i, (unsigned long long)(f - base));
    }
    fprintf(out, "=== end of crash report ===\n");
    fflush(out);
}

static LONG WINAPI MpCrashHandler(EXCEPTION_POINTERS *info)
{
    void *frames[48];
    USHORT count = CaptureStackBackTrace(0, 48, frames, NULL);
    FILE *file = fopen("pokemon-spectrum_crash.txt", "w");

    MpCrashWrite(stderr, info, frames, count);
    if (file != NULL)
    {
        MpCrashWrite(file, info, frames, count);
        fclose(file);
    }
    MessageBoxA(NULL, "Pokemon Spectrum crashed.\n\nThe details were saved to pokemon-spectrum_crash.txt next to the game.\nPlease send that file (and the console text) to the developers.",
                "Pokemon Spectrum crash", MB_OK | MB_ICONERROR);
    return EXCEPTION_CONTINUE_SEARCH;
}

// PS-076. While the window is dragged or resized, Windows runs a modal loop inside SDL_PollEvent()
// and the main loop below stops: the game freezes (unchanged) and the audio queue, about
// PLATFORM_AUDIO_DEFAULT_QUEUE_MS long, ran dry, so the music faded out until the mouse button
// was released. SDL 2.0.16 sends no event of its own during that loop (nothing at all while the
// title bar is held still), and its SDL_SetWindowsMessageHook only runs from SDL's own
// PeekMessage loop, which the modal loop bypasses (it dispatches straight to the window
// procedure). So the window procedure itself is subclassed: WM_ENTERSIZEMOVE / WM_ENTERMENULOOP
// start a timer, each WM_TIMER tick (delivered by the modal loop, on the main thread) pumps the
// sound engine exactly as the main loop does, and WM_EXITSIZEMOVE / WM_EXITMENULOOP stop the
// timer. Everything else goes to SDL's procedure unchanged. The music follows the audio clock
// (the sequencer runs inside AudioUpdate), so it plays on while the picture stands still. No
// thread, no game state touched (AD-4). Linux window managers move the window themselves and
// never block the loop, so this is Windows only.
#define DRAG_AUDIO_TIMER_ID   0x5053 // "PS"
#define DRAG_AUDIO_TIMER_MS   8      // Windows rounds this up to its timer resolution (10-16 ms)

static WNDPROC sSdlWindowProc;
static bool sInModalLoop = false;
static bool sDragAudioPumping = false;

static void DragAudioPump(void)
{
    if (sDragAudioPumping)
        return;
    sDragAudioPumping = true;
    Platform_AudioPump(paused);
    sDragAudioPumping = false;
}

// The main loop is (or is about to be) blocked: deep queue, timer, and fill the queue now.
static void DragAudioBegin(HWND hWnd)
{
    sInModalLoop = true;
    SetTimer(hWnd, DRAG_AUDIO_TIMER_ID, DRAG_AUDIO_TIMER_MS, NULL);
    Platform_AudioSetDeepQueue(TRUE);
    DragAudioPump();
}

static void DragAudioEnd(HWND hWnd)
{
    sInModalLoop = false;
    KillTimer(hWnd, DRAG_AUDIO_TIMER_ID);
    Platform_AudioSetDeepQueue(FALSE); // what is queued still plays out over the loop's restart
}

static LRESULT CALLBACK DragAudioWindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_NCLBUTTONDOWN:
    {
        // A press on the title bar, a border or a caption button. Windows handles the whole
        // thing inside the default procedure, i.e. inside this CallWindowProc: first a wait for
        // the mouse to move past the drag threshold (or be released), only then the modal loop
        // with its WM_ENTERSIZEMOVE. That wait blocked us for 50-150 ms with no message of ours
        // (the timing log showed every remaining underrun there, including plain clicks), so
        // start pumping before handing the press over. Nested: WM_ENTERSIZEMOVE re-arms the
        // same timer, WM_EXITSIZEMOVE ends it; the end below covers a click without a drag.
        LRESULT result;

        DragAudioBegin(hWnd);
        result = CallWindowProc(sSdlWindowProc, hWnd, message, wParam, lParam);
        DragAudioEnd(hWnd);
        return result;
    }
    case WM_ENTERSIZEMOVE:
    case WM_ENTERMENULOOP:
        printf("[drag TEST] enter %s at %u ms\n", message == WM_ENTERSIZEMOVE ? "sizemove" : "menuloop", (unsigned)SDL_GetTicks()); // TEST ONLY
        DragAudioBegin(hWnd);
        break;
    case WM_EXITSIZEMOVE:
    case WM_EXITMENULOOP:
        printf("[drag TEST] exit %s at %u ms\n", message == WM_EXITSIZEMOVE ? "sizemove" : "menuloop", (unsigned)SDL_GetTicks()); // TEST ONLY
        DragAudioEnd(hWnd);
        break;
    case WM_TIMER:
        if (wParam == DRAG_AUDIO_TIMER_ID)
        {
            { static bool first = true; if (sInModalLoop && first) { printf("[drag TEST] first timer tick at %u ms\n", (unsigned)SDL_GetTicks()); first = false; } if (!sInModalLoop) first = true; } // TEST ONLY
            DragAudioPump();
            return 0;
        }
        break;
    default:
        // While the modal loop runs, every message it dispatches (mouse moves, WM_MOVING,
        // WM_SIZING, ...) is a chance to top the queue up between timer ticks. The pump is a
        // few comparisons when the queue is full.
        if (sInModalLoop)
            DragAudioPump();
        break;
    }
    return CallWindowProc(sSdlWindowProc, hWnd, message, wParam, lParam);
}

static void InstallDragAudioHook(SDL_Window *window)
{
    SDL_SysWMinfo info;

    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_WINDOWS)
    {
        printf("[audio] drag hook not installed: %s\n", SDL_GetError());
        return;
    }
    sSdlWindowProc = (WNDPROC)SetWindowLongPtr(info.info.win.window, GWLP_WNDPROC, (LONG_PTR)DragAudioWindowProc);
    if (sSdlWindowProc == NULL)
        printf("[audio] drag hook not installed: SetWindowLongPtr failed (%lu)\n", (unsigned long)GetLastError());
}
#endif

int main(int argc, char **argv)
{
    // Open an output console on Windows
#ifdef _WIN32
    SetUnhandledExceptionFilter(MpCrashHandler); // MP: crash reporter (BUG-003)
    AllocConsole() ;
    AttachConsole( GetCurrentProcessId() ) ;
    freopen( "CON", "w", stdout ) ;
    freopen( "CON", "w", stderr ) ; // MP: so argument errors are visible on Windows
#endif

    sArgv = argv; // PS-060
    int argsResult = ParsePlatformArgs(argc, argv);
    if (argsResult >= 0)
        return argsResult;

    if (gPlatformArgs.mpSelftest) // MP: --mp-selftest runs without a window or a save
    {
        MP_SetDebugLogging(gPlatformArgs.mpDebug);
        return MP_RunSelftest();
    }

    if (gPlatformArgs.saveSelftest) // MP: --save-selftest checks the crash-safe save I/O (PS-014)
        return RunSaveSelftest();

    // MP: --name is checked before anything opens (A-Z a-z 0-9, 1-7 chars)
    if (gPlatformArgs.hasName && !MP_SetNameOverride(gPlatformArgs.name))
    {
        fprintf(stderr, "--name: 1-7 characters, A-Z a-z 0-9 only\n");
        return 2;
    }

    if (!ReadSaveFile(gPlatformArgs.savePath))
        return 1;

    videoScale = gPlatformArgs.scale;

    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0)
    {
        DBGPRINTF("SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }
    Platform_InputLoadConfig(); // PS-066: pokemon-spectrum.ini next to the executable

    sdlWindow = SDL_CreateWindow(gPlatformArgs.windowTitle, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, DISPLAY_WIDTH * videoScale, DISPLAY_HEIGHT * videoScale, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (sdlWindow == NULL)
    {
        DBGPRINTF("Window could not be created! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }

#ifdef _WIN32
    InstallDragAudioHook(sdlWindow); // PS-076: the music keeps playing while the window is dragged
#endif

    sdlRenderer = SDL_CreateRenderer(sdlWindow, -1, SDL_RENDERER_PRESENTVSYNC);
    if (sdlRenderer == NULL)
    {
        DBGPRINTF("Renderer could not be created! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }

    SDL_SetRenderDrawColor(sdlRenderer, 255, 255, 255, 255);
    SDL_RenderClear(sdlRenderer);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_RenderSetLogicalSize(sdlRenderer, DISPLAY_WIDTH, DISPLAY_HEIGHT);

    sdlTexture = SDL_CreateTexture(sdlRenderer,
                                   SDL_PIXELFORMAT_ABGR1555,
                                   SDL_TEXTUREACCESS_STREAMING,
                                   DISPLAY_WIDTH, DISPLAY_HEIGHT);
    if (sdlTexture == NULL)
    {
        DBGPRINTF("Texture could not be created! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }

    simTime = curGameTime = lastGameTime = SDL_GetPerformanceCounter();

    isFrameAvailable.value = 0;
    vBlankSemaphore = SDL_CreateSemaphore(0);

    Platform_AudioOpen(); // PS-058: src/platform/sdl2_audio.c

    MP_Init(); // MP: no socket is opened here
    MP_SetDebugLogging(gPlatformArgs.mpDebug); // MP:
    MP_SetNetStatsLogging(gPlatformArgs.mpNetStats); // MP: PS-053 --mp-net-stats
    MP_SetLinkTest(gPlatformArgs.mpLinkTest); // MP: PS-046 --mp-link-test
    MP_LinkSetFastBlocks(!gPlatformArgs.mpLinkRows); // MP: PS-052 --mp-link-rows
    MP_SetLinkStats(gPlatformArgs.mpLinkStats); // MP: PS-052 --mp-link-stats
    MP_SetNetSimLoss(gPlatformArgs.mpNetLoss); // MP: PS-054 --mp-net-loss
    if (gPlatformArgs.hasFakeProtocol) // MP:
        MP_SetFakeProtocolVersion(gPlatformArgs.fakeProtocol);
    if (gPlatformArgs.fakeBuild[0] != '\0') // MP:
        MP_SetFakeBuildId(gPlatformArgs.fakeBuild);
    // MP: --host / --join start after Continue reaches the overworld (MP_OnContinueSavedGame)
    if (gPlatformArgs.mpRole == PLATFORM_MP_ROLE_HOST)
        MP_SetAutoStart(MP_AUTOSTART_HOST, NULL, gPlatformArgs.port);
    else if (gPlatformArgs.mpRole == PLATFORM_MP_ROLE_CLIENT)
        MP_SetAutoStart(MP_AUTOSTART_JOIN, gPlatformArgs.joinAddress, gPlatformArgs.port);

    // PS-062: the clock must be ready before AgbMain(): its RtcInit() probes the status. With the
    // status still 0 (12-hour mode) the probe "reset" the RTC, which left it locked on PC, so the
    // game fell back to a frozen 2000-01-01 00:00 (no berry growth, no day/night, no daily events).
    memset(&internalClock, 0, sizeof(internalClock));
    internalClock.status = SIIRTCINFO_24HOUR;
    UpdateInternalClock();

    AgbMain();
    if (RtcGetErrorStatus() & RTC_ERR_FLAG_MASK) // PS-062
        printf("[clock] RTC error 0x%04X: the game uses a frozen clock\n", (unsigned)RtcGetErrorStatus());
    else
        printf("[clock] RTC ok: the game uses the PC's local time\n");

    double accumulator = 0.0;

    bool isGameStepDrawn = false;
    while (isRunning)
    {
        double deltaTime;

        ProcessEvents();
        // MP: pump the network once per outer iteration, before the fixed-step
        // MainLoop() ticks; it also runs while paused (design.md section 5).
        MP_Update();
        MpUi_Update(); // MP: drain MP events right after the pump (PS-022; PS-033 notices)
        // MP: PS-048. The link stack counts its timeouts in frames on every member: no pause
        // or speed-up while this instance is in a link group (trade / battle).
        if (MP_LinkTimeControlsLocked())
        {
            static bool sTimeLockNoticeShown = false;

            if ((paused || speedUp || timeScale != 1.0) && !sTimeLockNoticeShown)
            {
                puts("[MP] pause and speed-up are disabled during a link (trade / battle)");
                sTimeLockNoticeShown = true;
            }
            paused = false;
            speedUp = false;
            timeScale = 1.0;
        }

        curGameTime = SDL_GetPerformanceCounter();
        deltaTime = (double)((curGameTime - lastGameTime) / (double)SDL_GetPerformanceFrequency());
        deltaTime *= timeScale; //apply speedup
        if (deltaTime > 0.05) printf("[loop TEST] iteration gap %.0f ms at %u ms\n", deltaTime * 1000.0, (unsigned)SDL_GetTicks()); // TEST ONLY

        if (!paused)
        {
            accumulator += deltaTime;
            // PS-076: after a stall (a window drag or resize on Windows, a slow save, the
            // debugger) catch up by a few frames at most instead of running every missed tick
            // back to back: a 5 s drag used to replay 300 ticks in one go, which fast-forwarded
            // the game and starved the audio queue (one underrun at the end of every drag).
            if (accumulator > fixedTimestep * MAX_CATCHUP_TICKS)
                accumulator = fixedTimestep * MAX_CATCHUP_TICKS;

            isGameStepDrawn = false;

            while (accumulator >= fixedTimestep)
            {
                //run game logic, draw frame and process DMAs and vblank
                ENTER_VBLANK(); //you must be in VBlank before running a game tick
                MainLoop();
                if (!isGameStepDrawn)
                {
                    VDraw(sdlTexture);
                    //SDL_RenderClear(sdlRenderer);
                    isGameStepDrawn = true;
                }
                RunDMAsAndVBlank();

                accumulator -= fixedTimestep;
            }

            if (videoScaleChanged)
            {
                SDL_SetWindowSize(sdlWindow, DISPLAY_WIDTH * videoScale, DISPLAY_HEIGHT * videoScale);
                videoScaleChanged = false;
            }
        }

        // PS-058: the sound engine runs here, paced by the audio queue (sdl2_audio.c), not by game ticks.
        Platform_AudioPump(paused);

        lastGameTime = curGameTime;

        SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, NULL);
        Platform_DrawNetOverlay(sdlRenderer); // MP: PS-053 F3 overlay, on top of the game picture
        SDL_RenderPresent(sdlRenderer);
        ThrottleLoop(); // PS-058: --debug-loop-hz
    }

    MP_Shutdown(); // MP:
    // MP: PS-014: nothing to close, the save file is only open while it is read or replaced.

    Platform_AudioClose(); // PS-058
    SDL_DestroyWindow(sdlWindow);
    SDL_Quit();
    return 0;
}

// PS-058: --debug-loop-hz <hz>. Test only: holds each main loop iteration to 1/hz seconds, like a
// monitor with that refresh rate under vsync (for example 59.94), so audio pacing can be tested
// headless. The loop still runs slower if it can't keep up.
static void ThrottleLoop(void)
{
    static u64 sNext;
    u64 freq, period, now;

    if (gPlatformArgs.debugLoopMilliHz == 0)
        return;
    freq = SDL_GetPerformanceFrequency();
    period = freq * 1000 / gPlatformArgs.debugLoopMilliHz;
    now = SDL_GetPerformanceCounter();
    if (sNext == 0 || now > sNext + period * 4)
        sNext = now; // first call, or far behind: start over
    sNext += period;
    while ((now = SDL_GetPerformanceCounter()) < sNext)
    {
        u64 leftMs = (sNext - now) * 1000 / freq;

        if (leftMs > 2)
            SDL_Delay((Uint32)(leftMs - 1));
    }
}

// ---------------------------------------------------------------------------
// Command-line options (dev harness, AD-12). Only parsed and stored here; the
// multiplayer session code acts on gPlatformArgs. See docs/multiplayer/dev-harness.md.
// ---------------------------------------------------------------------------

#define WINDOW_TITLE_BASE "Pok\xC3\xA9" "mon Spectrum" // "Pokémon Spectrum" in UTF-8

static void PrintUsage(FILE *out, const char *programName)
{
    fprintf(out,
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  --save <path>        Save file to use (default: " PLATFORM_DEFAULT_SAVE_PATH ")\n"
        "  --host [port]        Host a multiplayer game (listen on port, default %d)\n"
        "  --join <ip[:port]>   Join the multiplayer game at ip (default port %d)\n"
        "  --port <n>           Multiplayer port for --host / --join\n"
        "  --name <text>        Debug override for the displayed player name (max %d bytes)\n"
        "  --mp-debug           Verbose multiplayer/network logging\n"
        "  --mp-net-stats       Log network traffic every 10 s and a per-message table at session end (F3: overlay)\n"
        "  --mp-selftest        Run the multiplayer self-test (no window), print OK/FAILED and exit\n"
        "  --mp-link-test       Test only: with --host / --join, slots 0 and 1 loop the virtual link cable test\n"
        "  --mp-link-stats      Log link cable measurements (rows, blocks, waits) for each link activity\n"
        "  --mp-link-rows       Test only: send link blocks row by row like the GBA cable (no fast blocks)\n"
        "  --mp-net-loss <pct>  Test only: drop pct %% (0-100) of network packets at random, both directions\n"
        "  --mp-fake-protocol <n>  Test only: advertise protocol version n (host rejects with VERSION)\n"
        "  --mp-fake-build <id>    Test only: hash this build id instead of the real one (BUILD reject)\n"
        "  --debug-slow-save    Test only: pause %d ms per 4 KB sector while writing the save (kill test)\n"
        "  --save-selftest      Check the crash-safe save file I/O on <save>" SAVE_SELFTEST_SUFFIX ", print OK/FAILED and exit\n"
        "  --title <text>       Window title (default: \"" WINDOW_TITLE_BASE "\", plus [HOST]/[CLIENT])\n"
        "  --scale <n>          Initial window scale, 1-%d (default: 1)\n"
        "  --audio-stats        Log audio queue level, underruns, clipping and loop stalls every second\n"
        "                       (console and pokemon-spectrum_audio.txt)\n"
        "  --audio-dump <file.wav>  Write the exact audio stream sent to SDL (32-bit float WAV)\n"
        "  --audio-queue-ms <n> Audio kept ready ahead of the sound card, %d-%d ms (default %d)\n"
        "  --audio-legacy       Test only: v0.1.0 audio pacing without fades, to compare by ear\n"
        "  --debug-loop-hz <hz> Test only: run the main loop at this rate, e.g. 59.94 (simulated vsync)\n"
        "  -h, --help           Show this help and exit\n"
        "\n"
        "If a port is given more than once (--host <port>, --join ip:port, --port), the last one wins.\n",
        programName, PLATFORM_MP_DEFAULT_PORT, PLATFORM_MP_DEFAULT_PORT,
        PLATFORM_ARG_NAME_SIZE - 1, SAVE_SLOW_DELAY_MS, PLATFORM_MAX_SCALE,
        PLATFORM_AUDIO_MIN_QUEUE_MS, PLATFORM_AUDIO_MAX_QUEUE_MS, PLATFORM_AUDIO_DEFAULT_QUEUE_MS);
}

// Parses a decimal number in [minValue, maxValue]. The whole string must be digits.
static bool ParseUnsigned(const char *str, unsigned long minValue, unsigned long maxValue, unsigned long *out)
{
    unsigned long value = 0;
    const char *c;

    if (str == NULL || *str == '\0')
        return false;

    for (c = str; *c != '\0'; c++)
    {
        if (*c < '0' || *c > '9')
            return false;
        value = value * 10 + (unsigned long)(*c - '0');
        if (value > maxValue)
            return false;
    }

    if (value < minValue)
        return false;

    *out = value;
    return true;
}

// PS-058: parses "59.94" (up to 3 decimals) as thousandths (59940), in [minValue, maxValue].
static bool ParseMilli(const char *str, u32 minValue, u32 maxValue, u32 *out)
{
    u32 value = 0, decimals = 0;
    bool seenDot = false;
    const char *c;

    if (str == NULL || *str == '\0' || *str == '.')
        return false;
    for (c = str; *c != '\0'; c++)
    {
        if (*c == '.' && !seenDot)
        {
            seenDot = true;
            continue;
        }
        if (*c < '0' || *c > '9' || decimals == 3 || value > 100000000)
            return false;
        value = value * 10 + (u32)(*c - '0');
        if (seenDot)
            decimals++;
    }
    for (; decimals < 3; decimals++)
        value *= 10;
    if (value < minValue || value > maxValue)
        return false;
    *out = value;
    return true;
}

static bool ParsePort(const char *str, u16 *out)
{
    unsigned long value;

    if (!ParseUnsigned(str, 1, 65535, &value))
        return false;
    *out = (u16)value;
    return true;
}

// Copies src into dest (size bytes, always NUL-terminated). Fails if src does not fit.
static bool CopyArgString(char *dest, size_t size, const char *src)
{
    size_t len = strlen(src);

    if (len >= size)
        return false;
    memcpy(dest, src, len + 1);
    return true;
}

// Parses "<ip>" or "<ip>:<port>" into gPlatformArgs.joinAddress / port.
static bool ParseJoinAddress(const char *str)
{
    const char *colon = strchr(str, ':');
    size_t hostLen;

    if (colon == NULL)
    {
        hostLen = strlen(str);
    }
    else
    {
        // Only one ':' is allowed (no IPv6 literals).
        if (strchr(colon + 1, ':') != NULL)
            return false;
        if (!ParsePort(colon + 1, &gPlatformArgs.port))
            return false;
        hostLen = (size_t)(colon - str);
    }

    if (hostLen == 0 || hostLen >= sizeof(gPlatformArgs.joinAddress))
        return false;

    memcpy(gPlatformArgs.joinAddress, str, hostLen);
    gPlatformArgs.joinAddress[hostLen] = '\0';
    return true;
}

// Fills gPlatformArgs from the command line.
// Returns -1 to continue starting the game, otherwise the exit code main() should return.
static int ParsePlatformArgs(int argc, char **argv)
{
    const char *programName = (argc > 0 && argv[0] != NULL) ? argv[0] : "pokemon-spectrum";
    const char *title = NULL;
    int i;

    memset(&gPlatformArgs, 0, sizeof(gPlatformArgs));
    CopyArgString(gPlatformArgs.savePath, sizeof(gPlatformArgs.savePath), PLATFORM_DEFAULT_SAVE_PATH);
    gPlatformArgs.mpRole = PLATFORM_MP_ROLE_NONE;
    gPlatformArgs.port = PLATFORM_MP_DEFAULT_PORT;
    gPlatformArgs.scale = 1;
    gPlatformArgs.audioQueueMs = PLATFORM_AUDIO_DEFAULT_QUEUE_MS;

    for (i = 1; i < argc; i++)
    {
        const char *arg = argv[i];
        // Value of an option that requires one (NULL if it is missing)
        const char *value = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0 || strcmp(arg, "-?") == 0 || strcmp(arg, "/?") == 0)
        {
            PrintUsage(stdout, programName);
            return 0;
        }
        else if (strcmp(arg, "--save") == 0)
        {
            if (value == NULL || value[0] == '\0')
                goto missing_value;
            if (!CopyArgString(gPlatformArgs.savePath, sizeof(gPlatformArgs.savePath), value))
                goto bad_value;
            i++;
        }
        else if (strcmp(arg, "--host") == 0)
        {
            if (gPlatformArgs.mpRole == PLATFORM_MP_ROLE_CLIENT)
            {
                fprintf(stderr, "%s: --host and --join cannot be used together\n", programName);
                goto usage_error;
            }
            gPlatformArgs.mpRole = PLATFORM_MP_ROLE_HOST;
            // Optional port: only taken if the next argument starts with a digit.
            if (value != NULL && value[0] >= '0' && value[0] <= '9')
            {
                if (!ParsePort(value, &gPlatformArgs.port))
                    goto bad_value;
                i++;
            }
        }
        else if (strcmp(arg, "--join") == 0)
        {
            if (gPlatformArgs.mpRole == PLATFORM_MP_ROLE_HOST)
            {
                fprintf(stderr, "%s: --host and --join cannot be used together\n", programName);
                goto usage_error;
            }
            if (value == NULL || value[0] == '\0' || value[0] == '-')
                goto missing_value;
            if (!ParseJoinAddress(value))
                goto bad_value;
            gPlatformArgs.mpRole = PLATFORM_MP_ROLE_CLIENT;
            i++;
        }
        else if (strcmp(arg, "--port") == 0)
        {
            if (value == NULL)
                goto missing_value;
            if (!ParsePort(value, &gPlatformArgs.port))
                goto bad_value;
            i++;
        }
        else if (strcmp(arg, "--name") == 0)
        {
            if (value == NULL || value[0] == '\0')
                goto missing_value;
            if (!CopyArgString(gPlatformArgs.name, sizeof(gPlatformArgs.name), value))
                goto bad_value;
            gPlatformArgs.hasName = TRUE;
            i++;
        }
        else if (strcmp(arg, "--mp-debug") == 0)
        {
            gPlatformArgs.mpDebug = TRUE;
        }
        else if (strcmp(arg, "--mp-net-stats") == 0) // MP: PS-053
        {
            gPlatformArgs.mpNetStats = TRUE;
        }
        else if (strcmp(arg, "--mp-selftest") == 0) // MP:
        {
            gPlatformArgs.mpSelftest = TRUE;
        }
        else if (strcmp(arg, "--mp-link-test") == 0) // MP: PS-046
        {
            gPlatformArgs.mpLinkTest = TRUE;
        }
        else if (strcmp(arg, "--mp-link-stats") == 0) // MP: PS-052
        {
            gPlatformArgs.mpLinkStats = TRUE;
        }
        else if (strcmp(arg, "--mp-link-rows") == 0) // MP: PS-052
        {
            gPlatformArgs.mpLinkRows = TRUE;
        }
        else if (strcmp(arg, "--mp-net-loss") == 0) // MP: PS-054 simulated packet loss
        {
            unsigned long percent;

            if (value == NULL)
                goto missing_value;
            if (!ParseUnsigned(value, 0, 100, &percent))
                goto bad_value;
            gPlatformArgs.mpNetLoss = (u8)percent;
            i++;
        }
        else if (strcmp(arg, "--debug-slow-save") == 0) // MP: PS-014 kill-during-save test
        {
            gPlatformArgs.debugSlowSave = TRUE;
        }
        else if (strcmp(arg, "--save-selftest") == 0) // MP: PS-014
        {
            gPlatformArgs.saveSelftest = TRUE;
        }
        else if (strcmp(arg, "--mp-fake-protocol") == 0) // MP: test the VERSION reject
        {
            unsigned long version;

            if (value == NULL)
                goto missing_value;
            if (!ParseUnsigned(value, 0, 65535, &version))
                goto bad_value;
            gPlatformArgs.hasFakeProtocol = TRUE;
            gPlatformArgs.fakeProtocol = (u16)version;
            i++;
        }
        else if (strcmp(arg, "--mp-fake-build") == 0) // MP: test the BUILD reject
        {
            const char *c;

            if (value == NULL || value[0] == '\0')
                goto missing_value;
            for (c = value; *c != '\0'; c++)
            {
                if (*c <= ' ' || *c >= 0x7F)
                    goto bad_value;
            }
            if (!CopyArgString(gPlatformArgs.fakeBuild, sizeof(gPlatformArgs.fakeBuild), value))
                goto bad_value;
            i++;
        }
        else if (strcmp(arg, "--title") == 0)
        {
            if (value == NULL)
                goto missing_value;
            if (strlen(value) >= sizeof(gPlatformArgs.windowTitle))
                goto bad_value;
            title = value;
            i++;
        }
        else if (strcmp(arg, "--audio-stats") == 0) // PS-058
        {
            gPlatformArgs.audioStats = TRUE;
        }
        else if (strcmp(arg, "--audio-dump") == 0) // PS-058
        {
            if (value == NULL || value[0] == '\0')
                goto missing_value;
            if (!CopyArgString(gPlatformArgs.audioDump, sizeof(gPlatformArgs.audioDump), value))
                goto bad_value;
            i++;
        }
        else if (strcmp(arg, "--audio-legacy") == 0) // PS-058
        {
            gPlatformArgs.audioLegacy = TRUE;
        }
        else if (strcmp(arg, "--audio-queue-ms") == 0) // PS-058
        {
            unsigned long ms;

            if (value == NULL)
                goto missing_value;
            if (!ParseUnsigned(value, PLATFORM_AUDIO_MIN_QUEUE_MS, PLATFORM_AUDIO_MAX_QUEUE_MS, &ms))
                goto bad_value;
            gPlatformArgs.audioQueueMs = (u32)ms;
            i++;
        }
        else if (strcmp(arg, "--debug-loop-hz") == 0) // PS-058
        {
            if (value == NULL)
                goto missing_value;
            if (!ParseMilli(value, 10000, 1000000, &gPlatformArgs.debugLoopMilliHz))
                goto bad_value;
            i++;
        }
        else if (strcmp(arg, "--scale") == 0)
        {
            unsigned long scale;

            if (value == NULL)
                goto missing_value;
            if (!ParseUnsigned(value, 1, PLATFORM_MAX_SCALE, &scale))
                goto bad_value;
            gPlatformArgs.scale = (u32)scale;
            i++;
        }
        else
        {
            fprintf(stderr, "%s: unknown option '%s'\n", programName, arg);
            goto usage_error;
        }
        continue;

    missing_value:
        fprintf(stderr, "%s: option '%s' needs a value\n", programName, arg);
        goto usage_error;
    bad_value:
        fprintf(stderr, "%s: invalid value '%s' for option '%s'\n", programName, value, arg);
        goto usage_error;
    }

    if (title != NULL)
        CopyArgString(gPlatformArgs.windowTitle, sizeof(gPlatformArgs.windowTitle), title);
    else if (gPlatformArgs.mpRole == PLATFORM_MP_ROLE_HOST)
        CopyArgString(gPlatformArgs.windowTitle, sizeof(gPlatformArgs.windowTitle), WINDOW_TITLE_BASE " [HOST]");
    else if (gPlatformArgs.mpRole == PLATFORM_MP_ROLE_CLIENT)
        CopyArgString(gPlatformArgs.windowTitle, sizeof(gPlatformArgs.windowTitle), WINDOW_TITLE_BASE " [CLIENT]");
    else
        CopyArgString(gPlatformArgs.windowTitle, sizeof(gPlatformArgs.windowTitle), WINDOW_TITLE_BASE);

    if (gPlatformArgs.mpDebug)
    {
        static const char *const sRoleNames[] = { "none", "host", "client" };

        printf("[mp] args: save='%s' role=%s join='%s' port=%u name='%s' title='%s' scale=%u\n",
               gPlatformArgs.savePath,
               sRoleNames[gPlatformArgs.mpRole],
               gPlatformArgs.joinAddress,
               (unsigned int)gPlatformArgs.port,
               gPlatformArgs.hasName ? gPlatformArgs.name : "",
               gPlatformArgs.windowTitle,
               (unsigned int)gPlatformArgs.scale);
    }

    return -1;

usage_error:
    fprintf(stderr, "\n");
    PrintUsage(stderr, programName);
    return 2;
}

// MP: PS-014. Loads the save into FLASH_BASE and closes the file again (it is not kept
// open, so it can be replaced by rename). A missing save is created empty, as before,
// so an unwritable --save path still fails at startup. A "<save>.tmp" left behind by a
// killed save is ignored: the real save is still the previous complete one.
// PS-070: the file must be empty or exactly one flash image (FLASH_PC_SIZE bytes) whose
// SaveBlock2.saveFormatVersion is SAVE_FORMAT_VERSION. Anything else is refused with a
// message and the game exits, so a save from another version of the game is never
// overwritten by a fresh one (a wrong-size file is not even read: FLASH_BASE stays as it
// was). There is no migration until 1.0.0 (PS-069 policy): a player with an old save moves
// it away and starts a new game.
static bool ReadSaveFile(const char *path)
{
    FILE *file;
    size_t bytesRead = 0;
    size_t i;
    long fileSize;
    u32 formatVersion;

    file = fopen(path, "rb");
    if (file == NULL)
    {
        file = fopen(path, "w+b");
    }
    if (file == NULL)
    {
        fprintf(stderr, "Could not open or create save file '%s'.\n", path);
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0 || (fileSize = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0)
    {
        fprintf(stderr, "[save] could not read the size of '%s'.\n", path);
        fclose(file);
        return false;
    }

    if (fileSize != 0 && fileSize != (long)sizeof(FLASH_BASE))
    {
        // A v0.2.0 save (131072 bytes), a truncated file, or not a save at all. Not read.
        fprintf(stderr, "[save] '%s' is %ld bytes, not %u: it is from an older or unknown version of the game."
                        " Saves from earlier versions are not supported: move it away and start a new game.\n",
                path, fileSize, (unsigned int)sizeof(FLASH_BASE));
        fclose(file);
        return false;
    }
    if (fileSize != 0)
    {
        bytesRead = fread(FLASH_BASE, 1, sizeof(FLASH_BASE), file);
        if (bytesRead != sizeof(FLASH_BASE))
        {
            fprintf(stderr, "[save] could not read '%s' (%u of %u bytes).\n", path, (unsigned int)bytesRead, (unsigned int)sizeof(FLASH_BASE));
            fclose(file);
            return false;
        }
    }
    fclose(file);

    // A new or empty save reads as erased flash.
    for (i = bytesRead; i < sizeof(FLASH_BASE); i++)
    {
        FLASH_BASE[i] = 0xFF;
    }

    // The right size, but written by another version of the game (its SaveBlock2 says so).
    formatVersion = Save_FlashFormatVersion();
    if (formatVersion != SAVE_FORMAT_VERSION)
    {
        fprintf(stderr, "[save] '%s' is from an older or unknown version of the game (format %u, this build needs %u)."
                        " Saves from earlier versions are not supported: move it away and start a new game.\n",
                path, (unsigned int)formatVersion, (unsigned int)SAVE_FORMAT_VERSION);
        return false;
    }

    return true;
}

// MP: PS-014. Flushes the OS buffers of `file` to the disk. Returns 0 on success.
static int SyncSaveFileToDisk(FILE *file)
{
#ifdef _WIN32
    return _commit(_fileno(file));
#else
    return fsync(fileno(file));
#endif
}

// MP: PS-014. Atomically replaces `path` with `tmpPath`.
static bool ReplaceSaveWithTemp(const char *tmpPath, const char *path)
{
#ifdef _WIN32
    // rename() does not overwrite on Windows. MoveFileEx does, in one step.
    int attempt;

    for (attempt = 0; attempt < SAVE_REPLACE_RETRIES; attempt++)
    {
        if (MoveFileExA(tmpPath, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        // Antivirus or backup tools can hold the file for a moment.
        SDL_Delay(50);
    }
    fprintf(stderr, "[save] could not replace '%s' (Windows error %lu)\n", path, (unsigned long)GetLastError());
    return false;
#else
    // POSIX rename() replaces the destination atomically.
    if (rename(tmpPath, path) == 0)
        return true;
    perror("[save] rename");
    return false;
#endif
}

// MP: PS-014. Crash-safe save write. Writes `data` to "<path>.tmp" one sector at a time
// (sleeping delayMsPerSector after each for --debug-slow-save), flushes and syncs it,
// then renames it over `path`. If the process dies at any point, `path` is either the
// complete old save or the complete new one, never a mix. Returns false on any error;
// the old save is then left untouched.
static bool WriteSaveFileAtomic(const char *path, const u8 *data, u32 size, u32 delayMsPerSector)
{
    char tmpPath[PLATFORM_ARG_PATH_SIZE + sizeof(SAVE_TMP_SUFFIX) + sizeof(SAVE_SELFTEST_SUFFIX)];
    FILE *file;
    u32 written;
    u32 chunk;
    bool ok = true;
    int len;

    len = snprintf(tmpPath, sizeof(tmpPath), "%s%s", path, SAVE_TMP_SUFFIX);
    if (len < 0 || (size_t)len >= sizeof(tmpPath))
    {
        fprintf(stderr, "[save] save path too long: '%s'\n", path);
        return false;
    }

    file = fopen(tmpPath, "wb");
    if (file == NULL)
    {
        fprintf(stderr, "[save] could not create '%s'\n", tmpPath);
        return false;
    }

    for (written = 0; ok && written < size; written += chunk)
    {
        chunk = size - written;
        if (chunk > SAVE_CHUNK_SIZE)
            chunk = SAVE_CHUNK_SIZE;
        if (fwrite(data + written, 1, chunk, file) != chunk)
            ok = false;
        if (ok && delayMsPerSector != 0)
        {
            // Make the partial temp file visible on disk, then hold the kill window open.
            fflush(file);
            SDL_Delay(delayMsPerSector);
        }
    }

    if (fflush(file) != 0)
        ok = false;
    if (ok && SyncSaveFileToDisk(file) != 0)
        ok = false;
    if (fclose(file) != 0)
        ok = false;

    if (!ok)
    {
        fprintf(stderr, "[save] writing '%s' failed; the previous save is unchanged\n", tmpPath);
        remove(tmpPath);
        return false;
    }

    if (!ReplaceSaveWithTemp(tmpPath, path))
    {
        fprintf(stderr, "[save] the previous save '%s' is unchanged; the new data is in '%s'\n", path, tmpPath);
        return false;
    }
    return true;
}

static bool StoreSaveFile(void)
{
    u32 delay = gPlatformArgs.debugSlowSave ? SAVE_SLOW_DELAY_MS : 0;

    if (delay != 0)
        printf("[save] --debug-slow-save: writing '%s' over %u ms\n", gPlatformArgs.savePath,
               (unsigned int)(delay * (sizeof(FLASH_BASE) / SAVE_CHUNK_SIZE)));
    if (!WriteSaveFileAtomic(gPlatformArgs.savePath, FLASH_BASE, sizeof(FLASH_BASE), delay))
        return false;
    if (delay != 0)
        printf("[save] --debug-slow-save: save written\n");
    return true;
}

void Platform_StoreSaveFile(void)
{
    // MP: PS-014. Callers (save.c, clear_save_data_screen.c) ignore the result; a failure
    // is logged and the previous save file stays valid.
    StoreSaveFile();
}

// PS-070: for the save format selftest (save_format_selftest.c).
bool8 Platform_LoadSaveFile(const char *path)
{
    return ReadSaveFile(path) ? TRUE : FALSE;
}

// MP: PS-014. Reads from FLASH_BASE, the in-memory copy of the save that every flash write
// goes to (agb_flash_dummy.c). It used to reopen the file for every read, which could
// return stale data for anything written since the last Platform_StoreSaveFile.
// Bytes outside the flash (144 KB, PS-070) read as 0xFF (erased).
void Platform_ReadFlash(u16 sectorNum, u32 offset, u8 *dest, u32 size)
{
    u32 shift = (gFlash != NULL) ? gFlash->sector.shift : SAVE_FLASH_SECTOR_SHIFT;
    u32 start = ((u32)sectorNum << shift) + offset;
    u32 avail = 0;

    if (dest == NULL || size == 0)
        return;

    if (start < sizeof(FLASH_BASE))
    {
        avail = sizeof(FLASH_BASE) - start;
        if (avail > size)
            avail = size;
        memcpy(dest, FLASH_BASE + start, avail);
    }
    if (avail < size)
    {
        fprintf(stderr, "[save] ReadFlash(sector=%u, offset=0x%X, size=0x%X) is outside the flash\n",
                (unsigned int)sectorNum, (unsigned int)offset, (unsigned int)size);
        memset(dest + avail, 0xFF, size - avail);
    }
}

// ---------------------------------------------------------------------------
// MP: PS-014. --save-selftest: checks the crash-safe save I/O on "<save>.selftest"
// (never the real save) and exits 0 (OK) or 1 (FAILED). Runs without a window.
// ---------------------------------------------------------------------------

static unsigned int sSaveSelftestFailures;

#define SAVE_SELFTEST_CHECK(cond)                                                         \
    do                                                                                    \
    {                                                                                     \
        if (!(cond))                                                                      \
        {                                                                                 \
            sSaveSelftestFailures++;                                                      \
            printf("[save] selftest: check failed (sdl2.c:%d): %s\n", __LINE__, #cond);   \
        }                                                                                 \
    } while (0)

static void FillSelftestPattern(u8 seed)
{
    size_t i;

    for (i = 0; i < sizeof(FLASH_BASE); i++)
        FLASH_BASE[i] = (u8)((i * 7u + seed) ^ (i >> 8));
}

static bool SelftestPatternMatches(const u8 *data, u8 seed)
{
    size_t i;

    for (i = 0; i < sizeof(FLASH_BASE); i++)
    {
        if (data[i] != (u8)((i * 7u + seed) ^ (i >> 8)))
            return false;
    }
    return true;
}

static bool SelftestFileExists(const char *path)
{
    FILE *file = fopen(path, "rb");

    if (file == NULL)
        return false;
    fclose(file);
    return true;
}

// TRUE if `path` is exactly sizeof(FLASH_BASE) bytes of pattern `seed`.
static bool SelftestFileMatches(const char *path, u8 seed)
{
    static u8 sReadBack[sizeof(FLASH_BASE) + 1];
    FILE *file = fopen(path, "rb");
    size_t n;

    if (file == NULL)
        return false;
    n = fread(sReadBack, 1, sizeof(sReadBack), file);
    fclose(file);
    return n == sizeof(FLASH_BASE) && SelftestPatternMatches(sReadBack, seed);
}

static void SelftestReport(const char *group, unsigned int failuresBefore)
{
    if (sSaveSelftestFailures == failuresBefore)
        printf("[save] selftest: %s OK\n", group);
    else
        printf("[save] selftest: %s FAILED (%u)\n", group, sSaveSelftestFailures - failuresBefore);
}

static int RunSaveSelftest(void)
{
    char path[PLATFORM_ARG_PATH_SIZE + sizeof(SAVE_SELFTEST_SUFFIX)];
    char tmpPath[sizeof(path) + sizeof(SAVE_TMP_SUFFIX)];
    u8 buf[64];
    unsigned int before;
    FILE *file;
    size_t i;
    bool allFF;

    sSaveSelftestFailures = 0;
    snprintf(path, sizeof(path), "%s%s", gPlatformArgs.savePath, SAVE_SELFTEST_SUFFIX);
    snprintf(tmpPath, sizeof(tmpPath), "%s%s", path, SAVE_TMP_SUFFIX);
    printf("[save] selftest: using '%s'\n", path);
    remove(path);
    remove(tmpPath);

    // 1. Write a new file, then replace an existing one.
    before = sSaveSelftestFailures;
    FillSelftestPattern(1);
    SAVE_SELFTEST_CHECK(WriteSaveFileAtomic(path, FLASH_BASE, sizeof(FLASH_BASE), 0));
    SAVE_SELFTEST_CHECK(SelftestFileMatches(path, 1));
    SAVE_SELFTEST_CHECK(!SelftestFileExists(tmpPath));
    FillSelftestPattern(2);
    SAVE_SELFTEST_CHECK(WriteSaveFileAtomic(path, FLASH_BASE, sizeof(FLASH_BASE), 0));
    SAVE_SELFTEST_CHECK(SelftestFileMatches(path, 2));
    SAVE_SELFTEST_CHECK(!SelftestFileExists(tmpPath));
    SelftestReport("write and replace", before);

    // 2. Read back into FLASH_BASE.
    before = sSaveSelftestFailures;
    memset(FLASH_BASE, 0, sizeof(FLASH_BASE));
    SAVE_SELFTEST_CHECK(ReadSaveFile(path));
    SAVE_SELFTEST_CHECK(SelftestPatternMatches(FLASH_BASE, 2));
    SelftestReport("read back", before);

    // 3. A save killed mid-write leaves a partial "<save>.tmp": the save itself is still the
    //    old complete one, loading ignores the leftover, and the next save replaces it.
    before = sSaveSelftestFailures;
    file = fopen(tmpPath, "wb");
    SAVE_SELFTEST_CHECK(file != NULL);
    if (file != NULL)
    {
        FillSelftestPattern(3);
        SAVE_SELFTEST_CHECK(fwrite(FLASH_BASE, 1, sizeof(FLASH_BASE) / 2, file) == sizeof(FLASH_BASE) / 2);
        fclose(file);
    }
    SAVE_SELFTEST_CHECK(SelftestFileMatches(path, 2));
    memset(FLASH_BASE, 0, sizeof(FLASH_BASE));
    SAVE_SELFTEST_CHECK(ReadSaveFile(path));
    SAVE_SELFTEST_CHECK(SelftestPatternMatches(FLASH_BASE, 2));
    FillSelftestPattern(4);
    SAVE_SELFTEST_CHECK(WriteSaveFileAtomic(path, FLASH_BASE, sizeof(FLASH_BASE), 0));
    SAVE_SELFTEST_CHECK(SelftestFileMatches(path, 4));
    SAVE_SELFTEST_CHECK(!SelftestFileExists(tmpPath));
    SelftestReport("interrupted save leftover", before);

    // 4. The --debug-slow-save path (1 ms per sector here) writes the same bytes.
    before = sSaveSelftestFailures;
    FillSelftestPattern(5);
    SAVE_SELFTEST_CHECK(WriteSaveFileAtomic(path, FLASH_BASE, sizeof(FLASH_BASE), 1));
    SAVE_SELFTEST_CHECK(SelftestFileMatches(path, 5));
    SelftestReport("slow save", before);

    // 5. A write that cannot happen fails cleanly and leaves the old save alone.
    before = sSaveSelftestFailures;
    SAVE_SELFTEST_CHECK(!WriteSaveFileAtomic("pokemon-spectrum-no-such-dir/x.sav", FLASH_BASE, sizeof(FLASH_BASE), 0));
    SAVE_SELFTEST_CHECK(SelftestFileMatches(path, 5));
    SelftestReport("failed write", before);

    // 6. Platform_ReadFlash reads the in-memory flash, clamped to its size.
    before = sSaveSelftestFailures;
    FillSelftestPattern(6);
    Platform_ReadFlash(3, 0x10, buf, sizeof(buf));
    SAVE_SELFTEST_CHECK(memcmp(buf, FLASH_BASE + (3 << SAVE_FLASH_SECTOR_SHIFT) + 0x10, sizeof(buf)) == 0);
    memset(buf, 0, sizeof(buf));
    Platform_ReadFlash(FLASH_PC_SECTORS - 1, 0x1000 - 16, buf, sizeof(buf)); // last 16 bytes of the flash + 48 outside
    SAVE_SELFTEST_CHECK(memcmp(buf, FLASH_BASE + sizeof(FLASH_BASE) - 16, 16) == 0);
    allFF = true;
    for (i = 16; i < sizeof(buf); i++)
    {
        if (buf[i] != 0xFF)
            allFF = false;
    }
    SAVE_SELFTEST_CHECK(allFF);
    memset(buf, 0, sizeof(buf));
    Platform_ReadFlash(200, 0, buf, sizeof(buf));
    SAVE_SELFTEST_CHECK(buf[0] == 0xFF && buf[sizeof(buf) - 1] == 0xFF);
    SelftestReport("read flash from memory", before);

    // 7. MP: BUG-003. The trade's two-phase link save on a real save (save_link_selftest.c).
    //    Platform_StoreSaveFile writes gPlatformArgs.savePath, so point it at the test file.
    before = sSaveSelftestFailures;
    {
        char savedPath[sizeof(gPlatformArgs.savePath)];

        memcpy(savedPath, gPlatformArgs.savePath, sizeof(savedPath));
        snprintf(gPlatformArgs.savePath, sizeof(gPlatformArgs.savePath), "%s", path);
        sSaveSelftestFailures += SaveSelftest_LinkSave(path);
        memcpy(gPlatformArgs.savePath, savedPath, sizeof(savedPath));
    }
    SelftestReport("link save", before);

    // 8. PS-070. The save format: a format 2 round trip with the extended flag / var ranges,
    //    refused file sizes and save format versions (save_format_selftest.c). Same savePath
    //    dance as 7.
    before = sSaveSelftestFailures;
    {
        char savedPath[sizeof(gPlatformArgs.savePath)];

        memcpy(savedPath, gPlatformArgs.savePath, sizeof(savedPath));
        snprintf(gPlatformArgs.savePath, sizeof(gPlatformArgs.savePath), "%s", path);
        sSaveSelftestFailures += SaveSelftest_Format(path);
        memcpy(gPlatformArgs.savePath, savedPath, sizeof(savedPath));
    }
    SelftestReport("format", before);

    remove(path);
    remove(tmpPath);

    if (sSaveSelftestFailures == 0)
    {
        printf("[save] selftest OK\n");
        return 0;
    }
    printf("[save] selftest FAILED (%u)\n", sSaveSelftestFailures);
    return 1;
}

// PS-058: Platform_QueueAudio is in sdl2_audio.c.


// PS-066: the game buttons and fast-forward come from pokemon-spectrum.ini (sdl2_input.c);
// ProcessEvents() reads the held keys once per frame into `keys`.
static u16 keys;

// PS-063: Alt+Enter switches between the window and borderless fullscreen (desktop resolution).
// SDL_RenderSetLogicalSize keeps the 240x160 picture centred with black bars, as in the window.
static bool IsFullscreen(void)
{
    return sdlWindow != NULL && (SDL_GetWindowFlags(sdlWindow) & SDL_WINDOW_FULLSCREEN) != 0;
}

// The pointer's desktop position before a switch. SDL keeps the window-relative position, so
// without this the pointer jumps (e.g. 18,40 in the window becomes 18,40 on the whole screen).
// Restored right after the switch and again on the first resize event (Windows can resize late).
static bool sRestorePointer;
static int sRestorePointerX, sRestorePointerY;

static void RestorePointer(void)
{
    if (sRestorePointer)
        SDL_WarpMouseGlobal(sRestorePointerX, sRestorePointerY);
}

static void ToggleFullscreen(void)
{
    bool fullscreen = !IsFullscreen();

    SDL_GetGlobalMouseState(&sRestorePointerX, &sRestorePointerY);
    sRestorePointer = true;
    if (SDL_SetWindowFullscreen(sdlWindow, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) != 0)
    {
        printf("Fullscreen switch failed: %s\n", SDL_GetError());
        sRestorePointer = false;
        return;
    }
    RestorePointer();
}

// PS-063: the mouse pointer shows while the mouse moves over the game and hides after it has
// been still for CURSOR_HIDE_MS (window and fullscreen). SDL only hides it over our window.
#define CURSOR_HIDE_MS 2000

static u32 sCursorLastMoveMs;
static bool sCursorShown = true;

static void UpdateCursor(void)
{
    if (sCursorShown && SDL_GetTicks() - sCursorLastMoveMs >= CURSOR_HIDE_MS)
    {
        SDL_ShowCursor(SDL_DISABLE);
        sCursorShown = false;
    }
}

void ProcessEvents(void)
{
    SDL_Event event;

    while (SDL_PollEvent(&event))
    {
        Platform_HandleTextInputEvent(&event); // MP: typed digits / Ctrl+V for the Join Game screen (PS-021)
        switch (event.type)
        {
        case SDL_MOUSEMOTION: // PS-063
            sCursorLastMoveMs = SDL_GetTicks();
            if (!sCursorShown)
            {
                SDL_ShowCursor(SDL_ENABLE);
                sCursorShown = true;
            }
            break;
        case SDL_MOUSEBUTTONDOWN: // PS-063: double-click the game = fullscreen on / off, like Alt+Enter
            if (event.button.button == SDL_BUTTON_LEFT && event.button.clicks == 2)
                ToggleFullscreen();
            break;
        case SDL_QUIT:
            isRunning = false;
            break;
        case SDL_KEYDOWN: // PS-066: hotkeys only; the game buttons are polled below
            // PS-063: Alt+Enter is fullscreen, not Start.
            if ((event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_KP_ENTER)
             && (event.key.keysym.mod & KMOD_ALT))
            {
                if (!event.key.repeat)
                    ToggleFullscreen();
                break;
            }
            switch (event.key.keysym.sym)
            {
            case SDLK_r:
                if (event.key.keysym.mod & (KMOD_LCTRL | KMOD_RCTRL))
                {
                    DoSoftReset();
                }
                break;
            case SDLK_p:
                if (event.key.keysym.mod & (KMOD_LCTRL | KMOD_RCTRL))
                {
                    paused = !paused;
                }
                break;
            case SDLK_F3: // MP: PS-053 network overlay
                if (!event.key.repeat)
                    Platform_ToggleNetOverlay();
                break;
            }
            break;
        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED && sRestorePointer) // PS-063
            {
                RestorePointer();
                sRestorePointer = false;
            }
            // PS-063: no snapping to a multiple of 240x160 while fullscreen; the screen size is fixed.
            if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED && !IsFullscreen())
            {
                unsigned int w = event.window.data1;
                unsigned int h = event.window.data2;
                
                videoScale = 0;
                if (w / DISPLAY_WIDTH > videoScale)
                    videoScale = w / DISPLAY_WIDTH;
                if (h / DISPLAY_HEIGHT > videoScale)
                    videoScale = h / DISPLAY_HEIGHT;
                if (videoScale < 1)
                    videoScale = 1;

                videoScaleChanged = true;
            }
            break;
        }
    }
    UpdateCursor(); // PS-063

    // PS-066: the game buttons and fast-forward held on the keyboard, per pokemon-spectrum.ini.
    {
        bool8 fastForward;

        keys = Platform_InputPollKeyboard(&fastForward);
        if (fastForward != speedUp)
        {
            speedUp = fastForward;
            timeScale = speedUp ? 5.0 : 1.0;
        }
    }
}

#ifdef _WIN32
#define STICK_THRESHOLD 0.5f
u16 GetXInputKeys()
{
    XINPUT_STATE state;
    ZeroMemory(&state, sizeof(XINPUT_STATE));

    DWORD dwResult = XInputGetState(0, &state);
    u16 xinputKeys = 0;

    if (dwResult == ERROR_SUCCESS)
    {
        // PS-066: collect every controller input, then map them with pokemon-spectrum.ini.
        u32 padInputs = state.Gamepad.wButtons; // the PAD_* low bits are the XInput bits
        float xAxis = (float)state.Gamepad.sThumbLX / (float)SHRT_MAX;
        float yAxis = (float)state.Gamepad.sThumbLY / (float)SHRT_MAX;
        bool8 padFastForward;

        if (state.Gamepad.bLeftTrigger > 0x80)  padInputs |= PAD_LT;
        if (state.Gamepad.bRightTrigger > 0x80) padInputs |= PAD_RT;
        if (xAxis < -STICK_THRESHOLD) padInputs |= PAD_STICK_LEFT;
        if (xAxis >  STICK_THRESHOLD) padInputs |= PAD_STICK_RIGHT;
        if (yAxis < -STICK_THRESHOLD) padInputs |= PAD_STICK_DOWN;
        if (yAxis >  STICK_THRESHOLD) padInputs |= PAD_STICK_UP;
        xinputKeys = Platform_InputMapPad(padInputs, &padFastForward);

        // Speedup. 'speedUp' is the keyboard's fast-forward (ProcessEvents).
        // PS-058: the audio is no longer paused and flushed here. It keeps playing at normal speed,
        // the same as with Space on the keyboard. (Pausing cut the sound with a click, and during a
        // link the time lock reset timeScale every loop, which toggled the device on and off.)
        timeScale = (padFastForward || speedUp) ? 5.0 : 1.0;
    }

    return xinputKeys;
}
#endif // _WIN32

u16 Platform_GetKeyInput(void)
{
#ifdef _WIN32
    u16 gamepadKeys = GetXInputKeys();
    return (gamepadKeys != 0) ? gamepadKeys : keys;
#endif

    return keys;
}

void VDraw(SDL_Texture *texture)
{
    static uint16_t image[DISPLAY_WIDTH * DISPLAY_HEIGHT];

    memset(image, 0, sizeof(image));
    DrawFrame(image);
    SDL_UpdateTexture(texture, NULL, image, DISPLAY_WIDTH * sizeof (Uint16));
    REG_VCOUNT = 161; // prep for being in VBlank period
}

int DoMain(void *data)
{
    AgbMain();
}

void VBlankIntrWait(void)
{
    return;
}

u8 BinToBcd(u8 bin)
{
    int placeCounter = 1;
    u8 out = 0;
    do
    {
        out |= (bin % 10) * placeCounter;
        placeCounter *= 16;
    }
    while ((bin /= 10) > 0);

    return out;
}

void Platform_GetStatus(struct SiiRtcInfo *rtc)
{
    rtc->status = internalClock.status;
}

void Platform_SetStatus(struct SiiRtcInfo *rtc)
{
    internalClock.status = rtc->status;
}

static void UpdateInternalClock(void)
{
    time_t rawTime = time(NULL);
    struct tm *time = localtime(&rawTime);

    internalClock.year = BinToBcd(time->tm_year - 100);
    internalClock.month = BinToBcd(time->tm_mon + 1);
    internalClock.day = BinToBcd(time->tm_mday);
    internalClock.dayOfWeek = BinToBcd(time->tm_wday);
    internalClock.hour = BinToBcd(time->tm_hour);
    internalClock.minute = BinToBcd(time->tm_min);
    internalClock.second = BinToBcd(time->tm_sec);
}

void Platform_GetDateTime(struct SiiRtcInfo *rtc)
{
    UpdateInternalClock();

    rtc->year = internalClock.year;
    rtc->month = internalClock.month;
    rtc->day = internalClock.day;
    rtc->dayOfWeek = internalClock.dayOfWeek;
    rtc->hour = internalClock.hour;
    rtc->minute = internalClock.minute;
    rtc->second = internalClock.second;
    DBGPRINTF("GetDateTime: %d-%02d-%02d %02d:%02d:%02d\n", ConvertBcdToBinary(rtc->year),
                                                         ConvertBcdToBinary(rtc->month),
                                                         ConvertBcdToBinary(rtc->day),
                                                         ConvertBcdToBinary(rtc->hour),
                                                         ConvertBcdToBinary(rtc->minute),
                                                         ConvertBcdToBinary(rtc->second));
}

void Platform_SetDateTime(struct SiiRtcInfo *rtc)
{
    internalClock.month = rtc->month;
    internalClock.day = rtc->day;
    internalClock.dayOfWeek = rtc->dayOfWeek;
    internalClock.hour = rtc->hour;
    internalClock.minute = rtc->minute;
    internalClock.second = rtc->second;
}

void Platform_GetTime(struct SiiRtcInfo *rtc)
{
    UpdateInternalClock();

    rtc->hour = internalClock.hour;
    rtc->minute = internalClock.minute;
    rtc->second = internalClock.second;
    DBGPRINTF("GetTime: %02d:%02d:%02d\n", ConvertBcdToBinary(rtc->hour),
                                        ConvertBcdToBinary(rtc->minute),
                                        ConvertBcdToBinary(rtc->second));
}

void Platform_SetTime(struct SiiRtcInfo *rtc)
{
    internalClock.hour = rtc->hour;
    internalClock.minute = rtc->minute;
    internalClock.second = rtc->second;
}

void Platform_SetAlarm(u8 *alarmData)
{
    // TODO
}

// PS-060: a soft reset (Ctrl+R, A+B+Start+Select, or the game itself) restarts the program with the
// same command line. On PC the game's EWRAM/IWRAM variables are ordinary C globals mixed with the
// platform and network state, so they can't be cleared in place; a fresh process starts from
// exactly the state a GBA reset gives (title screen, unsaved progress lost).
// A process that was itself started by a reset and resets again within 3 s exits instead, so a
// reset during boot can't loop forever.
#define SOFT_RESET_ENV        "POKEMON_SPECTRUM_SOFT_RESET"
#define SOFT_RESET_LOOP_MS    3000

static void SoftResetRelaunch(void)
{
#ifdef _WIN32
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t *cmdLine = _wcsdup(GetCommandLineW()); // CreateProcessW may write to the buffer

    if (cmdLine == NULL)
        return;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    else
    {
        printf("Soft reset: could not restart (error %lu)\n", (unsigned long)GetLastError());
    }
    free(cmdLine);
#else
    if (sArgv != NULL && sArgv[0] != NULL)
    {
        execv("/proc/self/exe", sArgv);
        execvp(sArgv[0], sArgv); // only reached if /proc is unavailable
        printf("Soft reset: could not restart\n");
    }
#endif
}

void SoftReset(u32 resetFlags)
{
    bool8 relaunch = TRUE;

    if (getenv(SOFT_RESET_ENV) != NULL && SDL_GetTicks() < SOFT_RESET_LOOP_MS)
    {
        puts("Soft Reset called again right after a reset. Exiting.");
        relaunch = FALSE;
    }
    else
    {
        puts("Soft Reset called. Restarting.");
    }
    MP_Shutdown(); // MP: leave the session before restarting
    Platform_AudioClose(); // PS-058
    if (sdlWindow != NULL)
        SDL_DestroyWindow(sdlWindow);
    SDL_Quit();
    fflush(stdout);
    fflush(stderr);
    if (relaunch)
    {
        SDL_setenv(SOFT_RESET_ENV, "1", 1);
        SoftResetRelaunch();
    }
    exit(0);
}

#endif