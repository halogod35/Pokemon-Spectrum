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
#else
#include <unistd.h>  // MP: fsync() for the crash-safe save (PS-014)
#endif

#include <SDL2/SDL.h>

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

extern void (*const gIntrTable[])(void);

SDL_Thread *mainLoopThread;
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
double timeScale = 1.0;
struct SiiRtcInfo internalClock;

// MP: PS-014. The save file is no longer kept open. FLASH_BASE is the authoritative copy:
// ReadSaveFile fills it once at startup, Platform_ReadFlash reads from it, and
// Platform_StoreSaveFile replaces the file atomically (write "<save>.tmp", flush, rename).
#define SAVE_TMP_SUFFIX          ".tmp"
#define SAVE_SELFTEST_SUFFIX     ".selftest"
#define SAVE_CHUNK_SIZE          0x1000  // one flash sector
#define SAVE_FLASH_SECTOR_SHIFT  12      // DUMMY_SAVE sector shift, used if gFlash is not set yet
#define SAVE_SLOW_DELAY_MS       50      // --debug-slow-save: per sector, 32 sectors = 1.6 s per file write
#define SAVE_REPLACE_RETRIES     5       // Windows: retry if another process briefly holds the file

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

static void UpdateInternalClock(void);

int main(int argc, char **argv)
{
    // Open an output console on Windows
#ifdef _WIN32
    AllocConsole() ;
    AttachConsole( GetCurrentProcessId() ) ;
    freopen( "CON", "w", stdout ) ;
    freopen( "CON", "w", stderr ) ; // MP: so argument errors are visible on Windows
#endif

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

    sdlWindow = SDL_CreateWindow(gPlatformArgs.windowTitle, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, DISPLAY_WIDTH * videoScale, DISPLAY_HEIGHT * videoScale, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (sdlWindow == NULL)
    {
        DBGPRINTF("Window could not be created! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }

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

    SDL_AudioSpec want;

    SDL_memset(&want, 0, sizeof(want)); /* or SDL_zero(want) */
    want.freq = 42048;
    want.format = AUDIO_F32;
    want.channels = 2;
    want.samples = 1024;
    cgb_audio_init(want.freq);


    if (SDL_OpenAudio(&want, 0) < 0)
        SDL_Log("Failed to open audio: %s", SDL_GetError());
    else
    {
        if (want.format != AUDIO_F32) /* we let this one thing change. */
            SDL_Log("We didn't get Float32 audio format.");
        SDL_PauseAudio(0);
    }

    MP_Init(); // MP: no socket is opened here
    MP_SetDebugLogging(gPlatformArgs.mpDebug); // MP:
    if (gPlatformArgs.hasFakeProtocol) // MP:
        MP_SetFakeProtocolVersion(gPlatformArgs.fakeProtocol);
    if (gPlatformArgs.fakeBuild[0] != '\0') // MP:
        MP_SetFakeBuildId(gPlatformArgs.fakeBuild);
    // MP: --host / --join start after Continue reaches the overworld (MP_OnContinueSavedGame)
    if (gPlatformArgs.mpRole == PLATFORM_MP_ROLE_HOST)
        MP_SetAutoStart(MP_AUTOSTART_HOST, NULL, gPlatformArgs.port);
    else if (gPlatformArgs.mpRole == PLATFORM_MP_ROLE_CLIENT)
        MP_SetAutoStart(MP_AUTOSTART_JOIN, gPlatformArgs.joinAddress, gPlatformArgs.port);

    AgbMain();

    double accumulator = 0.0;

    memset(&internalClock, 0, sizeof(internalClock));
    internalClock.status = SIIRTCINFO_24HOUR;
    UpdateInternalClock();

    bool isGameStepDrawn = false;
    while (isRunning)
    {
        double deltaTime;

        ProcessEvents();
        // MP: pump the network once per outer iteration, before the fixed-step
        // MainLoop() ticks; it also runs while paused (design.md section 5).
        MP_Update();

        curGameTime = SDL_GetPerformanceCounter();
        deltaTime = (double)((curGameTime - lastGameTime) / (double)SDL_GetPerformanceFrequency());
        deltaTime *= timeScale; //apply speedup

        if (!paused)
        {
            accumulator += deltaTime;

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

            //samples per frame is 701, that gets multipled by two when being queued and then multipled by four because samples are float32 which are 4 bytes long hence the divide by 8
            //this number is then checked against samples per frame multipled by three rounded down to 2000 to give it enough margin of error while not desyncing
            //this is all done to sync audio to gameplay
            if (SDL_GetQueuedAudioSize(1)/8 < 2000)
            {
                AudioUpdate();
            }

            if (videoScaleChanged)
            {
                SDL_SetWindowSize(sdlWindow, DISPLAY_WIDTH * videoScale, DISPLAY_HEIGHT * videoScale);
                videoScaleChanged = false;
            }
        }

        lastGameTime = curGameTime;

        SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, NULL);
        SDL_RenderPresent(sdlRenderer);
    }

    MP_Shutdown(); // MP:
    // MP: PS-014: nothing to close, the save file is only open while it is read or replaced.

    SDL_DestroyWindow(sdlWindow);
    SDL_Quit();
    return 0;
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
        "  --mp-selftest        Run the multiplayer self-test (no window), print OK/FAILED and exit\n"
        "  --mp-fake-protocol <n>  Test only: advertise protocol version n (host rejects with VERSION)\n"
        "  --mp-fake-build <id>    Test only: hash this build id instead of the real one (BUILD reject)\n"
        "  --debug-slow-save    Test only: pause %d ms per 4 KB sector while writing the save (kill test)\n"
        "  --save-selftest      Check the crash-safe save file I/O on <save>" SAVE_SELFTEST_SUFFIX ", print OK/FAILED and exit\n"
        "  --title <text>       Window title (default: \"" WINDOW_TITLE_BASE "\", plus [HOST]/[CLIENT])\n"
        "  --scale <n>          Initial window scale, 1-%d (default: 1)\n"
        "  -h, --help           Show this help and exit\n"
        "\n"
        "If a port is given more than once (--host <port>, --join ip:port, --port), the last one wins.\n",
        programName, PLATFORM_MP_DEFAULT_PORT, PLATFORM_MP_DEFAULT_PORT,
        PLATFORM_ARG_NAME_SIZE - 1, SAVE_SLOW_DELAY_MS, PLATFORM_MAX_SCALE);
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
    const char *programName = (argc > 0 && argv[0] != NULL) ? argv[0] : "pokeemerald";
    const char *title = NULL;
    int i;

    memset(&gPlatformArgs, 0, sizeof(gPlatformArgs));
    CopyArgString(gPlatformArgs.savePath, sizeof(gPlatformArgs.savePath), PLATFORM_DEFAULT_SAVE_PATH);
    gPlatformArgs.mpRole = PLATFORM_MP_ROLE_NONE;
    gPlatformArgs.port = PLATFORM_MP_DEFAULT_PORT;
    gPlatformArgs.scale = 1;

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
        else if (strcmp(arg, "--mp-selftest") == 0) // MP:
        {
            gPlatformArgs.mpSelftest = TRUE;
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
static bool ReadSaveFile(const char *path)
{
    FILE *file;
    size_t bytesRead = 0;
    size_t i;

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

    // Read at most sizeof(FLASH_BASE) bytes; a shorter or new file reads as erased flash.
    bytesRead = fread(FLASH_BASE, 1, sizeof(FLASH_BASE), file);
    fclose(file);

    for (i = bytesRead; i < sizeof(FLASH_BASE); i++)
    {
        FLASH_BASE[i] = 0xFF;
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

// MP: PS-014. Reads from FLASH_BASE, the in-memory copy of the save that every flash write
// goes to (agb_flash_dummy.c). It used to reopen the file for every read, which could
// return stale data for anything written since the last Platform_StoreSaveFile.
// Bytes outside the 128 KB flash read as 0xFF (erased).
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
    Platform_ReadFlash(31, 0x1000 - 16, buf, sizeof(buf)); // last 16 bytes of the flash + 48 outside
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

void Platform_QueueAudio(float *audioBuffer, s32 samplesPerFrame)
{
    SDL_QueueAudio(1, audioBuffer, samplesPerFrame);
}


// Key mappings
#define KEY_A_BUTTON      SDLK_z
#define KEY_B_BUTTON      SDLK_x
#define KEY_START_BUTTON  SDLK_RETURN
#define KEY_SELECT_BUTTON SDLK_BACKSLASH
#define KEY_L_BUTTON      SDLK_a
#define KEY_R_BUTTON      SDLK_s
#define KEY_DPAD_UP       SDLK_UP
#define KEY_DPAD_DOWN     SDLK_DOWN
#define KEY_DPAD_LEFT     SDLK_LEFT
#define KEY_DPAD_RIGHT    SDLK_RIGHT

#define HANDLE_KEYUP(key) \
case KEY_##key:  keys &= ~key; break;

#define HANDLE_KEYDOWN(key) \
case KEY_##key:  keys |= key; break;

static u16 keys;

void ProcessEvents(void)
{
    SDL_Event event;

    while (SDL_PollEvent(&event))
    {
        Platform_HandleTextInputEvent(&event); // MP: typed digits / Ctrl+V for the Join Game screen (PS-021)
        switch (event.type)
        {
        case SDL_QUIT:
            isRunning = false;
            break;
        case SDL_KEYUP:
            switch (event.key.keysym.sym)
            {
            HANDLE_KEYUP(A_BUTTON)
            HANDLE_KEYUP(B_BUTTON)
            HANDLE_KEYUP(START_BUTTON)
            HANDLE_KEYUP(SELECT_BUTTON)
            HANDLE_KEYUP(L_BUTTON)
            HANDLE_KEYUP(R_BUTTON)
            HANDLE_KEYUP(DPAD_UP)
            HANDLE_KEYUP(DPAD_DOWN)
            HANDLE_KEYUP(DPAD_LEFT)
            HANDLE_KEYUP(DPAD_RIGHT)
            case SDLK_SPACE:
                if (speedUp)
                {
                    speedUp = false;
                    timeScale = 1.0;
                    //SDL_ClearQueuedAudio(1);
                    //SDL_PauseAudio(0);
                }
                break;
            }
            break;
        case SDL_KEYDOWN:
            switch (event.key.keysym.sym)
            {
            HANDLE_KEYDOWN(A_BUTTON)
            HANDLE_KEYDOWN(B_BUTTON)
            HANDLE_KEYDOWN(START_BUTTON)
            HANDLE_KEYDOWN(SELECT_BUTTON)
            HANDLE_KEYDOWN(L_BUTTON)
            HANDLE_KEYDOWN(R_BUTTON)
            HANDLE_KEYDOWN(DPAD_UP)
            HANDLE_KEYDOWN(DPAD_DOWN)
            HANDLE_KEYDOWN(DPAD_LEFT)
            HANDLE_KEYDOWN(DPAD_RIGHT)
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
            case SDLK_SPACE:
                if (!speedUp)
                {
                    speedUp = true;
                    timeScale = 5.0;
                    //SDL_PauseAudio(1);
                }
                break;
            }
            break;
        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
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
        /* A */      xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_A) >> 12;
        /* B */      xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_X) >> 13;
        /* Start */  xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_START) >> 1;
        /* Select */ xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_BACK) >> 3;
        /* L */      xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) << 1;
        /* R */      xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) >> 1;
        /* Up */     xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_UP) << 6;
        /* Down */   xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) << 6;
        /* Left */   xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) << 3;
        /* Right */  xinputKeys |= (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) << 1;


        /* Control Stick */
        float xAxis = (float)state.Gamepad.sThumbLX / (float)SHRT_MAX;
        float yAxis = (float)state.Gamepad.sThumbLY / (float)SHRT_MAX;

        if (xAxis < -STICK_THRESHOLD) xinputKeys |= DPAD_LEFT;
        if (xAxis >  STICK_THRESHOLD) xinputKeys |= DPAD_RIGHT;
        if (yAxis < -STICK_THRESHOLD) xinputKeys |= DPAD_DOWN;
        if (yAxis >  STICK_THRESHOLD) xinputKeys |= DPAD_UP;


        /* Speedup */
        // Note: 'speedup' variable is only (un)set on keyboard input
        double oldTimeScale = timeScale;
        timeScale = (state.Gamepad.bRightTrigger > 0x80 || speedUp) ? 5.0 : 1.0;

        if (oldTimeScale != timeScale)
        {
            if (timeScale > 1.0)
            {
                SDL_PauseAudio(1);
            }
            else
            {
                SDL_ClearQueuedAudio(1);
                SDL_PauseAudio(0);
            }
        }
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

void SoftReset(u32 resetFlags)
{
    puts("Soft Reset called. Exiting.");
    MP_Shutdown(); // MP: leave the session before exiting
    exit(0);
}

#endif