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

static FILE *sSaveFile = NULL;

struct PlatformArgs gPlatformArgs;

extern void AgbMain(void);
extern void MainLoop(void);
extern void DoSoftReset(void);

int DoMain(void *param);
void ProcessEvents(void);
void VDraw(SDL_Texture *texture);

static bool ReadSaveFile(const char *path);
static void StoreSaveFile(void);
static void CloseSaveFile(void);

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

    CloseSaveFile();

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
        "  --title <text>       Window title (default: \"" WINDOW_TITLE_BASE "\", plus [HOST]/[CLIENT])\n"
        "  --scale <n>          Initial window scale, 1-%d (default: 1)\n"
        "  -h, --help           Show this help and exit\n"
        "\n"
        "If a port is given more than once (--host <port>, --join ip:port, --port), the last one wins.\n",
        programName, PLATFORM_MP_DEFAULT_PORT, PLATFORM_MP_DEFAULT_PORT,
        PLATFORM_ARG_NAME_SIZE - 1, PLATFORM_MAX_SCALE);
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

static bool ReadSaveFile(const char *path)
{
    // Check whether the saveFile exists, and create it if not
    sSaveFile = fopen(path, "r+b");
    if (sSaveFile == NULL)
    {
        sSaveFile = fopen(path, "w+b");
    }
    if (sSaveFile == NULL)
    {
        fprintf(stderr, "Could not open or create save file '%s'.\n", path);
        return false;
    }

    fseek(sSaveFile, 0, SEEK_END);
    int fileSize = ftell(sSaveFile);
    fseek(sSaveFile, 0, SEEK_SET);

    // Only read as many bytes as fit inside the buffer
    // or as many bytes as are in the file
    int bytesToRead = (fileSize < sizeof(FLASH_BASE)) ? fileSize : sizeof(FLASH_BASE);

    int bytesRead = fread(FLASH_BASE, 1, bytesToRead, sSaveFile);

    // Fill the buffer if the savefile was just created or smaller than the buffer itself
    for (int i = bytesRead; i < sizeof(FLASH_BASE); i++)
    {
        FLASH_BASE[i] = 0xFF;
    }

    return true;
}

static void StoreSaveFile()
{
    if (sSaveFile != NULL)
    {
        fseek(sSaveFile, 0, SEEK_SET);
        fwrite(FLASH_BASE, 1, sizeof(FLASH_BASE), sSaveFile);
    }
}

void Platform_StoreSaveFile(void)
{
    StoreSaveFile();
}

void Platform_ReadFlash(u16 sectorNum, u32 offset, u8 *dest, u32 size)
{
    DBGPRINTF("ReadFlash(sectorNum=0x%04X,offset=0x%08X,size=0x%02X)\n",sectorNum,offset,size);
    FILE * savefile = fopen(gPlatformArgs.savePath, "r+b");
    if (savefile == NULL)
    {
        puts("Error opening save file.");
        return;
    }
    if (fseek(savefile, (sectorNum << gFlash->sector.shift) + offset, SEEK_SET))
    {
        fclose(savefile);
        return;
    }
    if (fread(dest, 1, size, savefile) != size)
    {
        fclose(savefile);
        return;
    }
    fclose(savefile);
}

void Platform_QueueAudio(float *audioBuffer, s32 samplesPerFrame)
{
    SDL_QueueAudio(1, audioBuffer, samplesPerFrame);
}


static void CloseSaveFile()
{
    if (sSaveFile != NULL)
    {
        fclose(sSaveFile);
    }
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
    exit(0);
}

#endif