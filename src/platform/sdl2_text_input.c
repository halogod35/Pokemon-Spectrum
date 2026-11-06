// Pokemon Spectrum multiplayer (PS-021): keyboard text entry, clipboard paste and the
// "<savePath>.mp.cfg" settings file for the Join Game IP screen. See include/platform.h.
//
// All SDL calls for this feature live here. The only hook in sdl2.c is the
// Platform_HandleTextInputEvent() call in ProcessEvents(). Everything runs on the main
// thread (ProcessEvents and the game tasks share it).

#ifdef PLATFORM_SDL2
#include <stdio.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "global.h"
#include "platform.h"
#include "platform/text_input.h"

#define MP_CFG_SUFFIX ".mp.cfg"

static bool8 sCapturing = FALSE;
static bool8 sStoppedTextInputBefore = FALSE;
static char sQueue[PLATFORM_TEXT_INPUT_QUEUE_SIZE];
static u32 sQueueCount = 0;
static bool8 sPastePending = FALSE;
static char sPaste[PLATFORM_TEXT_INPUT_QUEUE_SIZE];

static void QueueChar(char c)
{
    if (sQueueCount < PLATFORM_TEXT_INPUT_QUEUE_SIZE)
        sQueue[sQueueCount++] = c;
}

void Platform_StartTextInput(void)
{
    sQueueCount = 0;
    sPastePending = FALSE;
    sPaste[0] = '\0';
    sCapturing = TRUE;
    // SDL2 normally has text input enabled on desktop already. If it wasn't, enable it
    // and remember to turn it off again.
    sStoppedTextInputBefore = (SDL_IsTextInputActive() == SDL_FALSE);
    if (sStoppedTextInputBefore)
        SDL_StartTextInput();
}

void Platform_StopTextInput(void)
{
    if (!sCapturing)
        return;
    sCapturing = FALSE;
    sQueueCount = 0;
    sPastePending = FALSE;
    if (sStoppedTextInputBefore)
        SDL_StopTextInput();
    sStoppedTextInputBefore = FALSE;
}

u32 Platform_ReadTextInput(char *out, u32 outSize)
{
    u32 count;
    u32 i;

    if (out == NULL || outSize == 0)
        return 0;
    count = sQueueCount;
    if (count > outSize)
        count = outSize;
    for (i = 0; i < count; i++)
        out[i] = sQueue[i];
    for (i = count; i < sQueueCount; i++)
        sQueue[i - count] = sQueue[i];
    sQueueCount -= count;
    return count;
}

bool8 Platform_TakePastedText(char *out, u32 outSize)
{
    u32 i;

    if (!sPastePending || out == NULL || outSize == 0)
        return FALSE;
    for (i = 0; i < outSize - 1 && sPaste[i] != '\0'; i++)
        out[i] = sPaste[i];
    out[i] = '\0';
    sPastePending = FALSE;
    return TRUE;
}

static void TakeClipboard(void)
{
    char *clip = SDL_GetClipboardText(); // never NULL in SDL2 ("" on error), must be SDL_free'd
    u32 i;
    u32 n = 0;

    if (clip == NULL)
        return;
    for (i = 0; clip[i] != '\0' && n < sizeof(sPaste) - 1; i++)
    {
        // Printable ASCII only; UTF-8 bytes (>= 0x80) and control characters are dropped.
        if (clip[i] >= 0x20 && clip[i] < 0x7F)
            sPaste[n++] = clip[i];
    }
    sPaste[n] = '\0';
    sPastePending = TRUE;
    SDL_free(clip);
}

void Platform_HandleTextInputEvent(const union SDL_Event *event)
{
    u32 i;

    if (!sCapturing || event == NULL)
        return;

    switch (event->type)
    {
    case SDL_TEXTINPUT:
        for (i = 0; i < SDL_TEXTINPUTEVENT_TEXT_SIZE && event->text.text[i] != '\0'; i++)
        {
            char c = event->text.text[i];
            if (c >= 0x20 && c < 0x7F)
                QueueChar(c);
        }
        break;
    case SDL_KEYDOWN:
        if (event->key.keysym.sym == SDLK_BACKSPACE)
            QueueChar(PLATFORM_TEXT_BACKSPACE);
        else if (event->key.keysym.sym == SDLK_v && (event->key.keysym.mod & (KMOD_LCTRL | KMOD_RCTRL)) && !event->key.repeat)
            TakeClipboard();
        break;
    }
}

// Builds "<savePath>.mp.cfg". FALSE if it doesn't fit.
static bool8 GetMpConfigPath(char *out, u32 outSize)
{
    size_t len = strlen(gPlatformArgs.savePath);

    if (len == 0 || len + sizeof(MP_CFG_SUFFIX) > outSize)
        return FALSE;
    memcpy(out, gPlatformArgs.savePath, len);
    memcpy(out + len, MP_CFG_SUFFIX, sizeof(MP_CFG_SUFFIX)); // includes the NUL
    return TRUE;
}

bool8 Platform_ReadMpConfig(char *out, u32 outSize)
{
    char path[PLATFORM_ARG_PATH_SIZE + sizeof(MP_CFG_SUFFIX)];
    FILE *file;
    size_t len;

    if (out == NULL || outSize == 0)
        return FALSE;
    out[0] = '\0';
    if (!GetMpConfigPath(path, sizeof(path)))
        return FALSE;
    file = fopen(path, "r");
    if (file == NULL)
        return FALSE;
    if (fgets(out, (int)outSize, file) == NULL)
    {
        out[0] = '\0';
        fclose(file);
        return FALSE;
    }
    fclose(file);
    out[outSize - 1] = '\0';
    len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r'))
        out[--len] = '\0';
    return TRUE;
}

bool8 Platform_WriteMpConfig(const char *line)
{
    char path[PLATFORM_ARG_PATH_SIZE + sizeof(MP_CFG_SUFFIX)];
    FILE *file;
    bool8 ok;

    if (line == NULL || !GetMpConfigPath(path, sizeof(path)))
        return FALSE;
    file = fopen(path, "w");
    if (file == NULL)
        return FALSE;
    ok = (fputs(line, file) >= 0 && fputc('\n', file) != EOF);
    if (fclose(file) != 0)
        ok = FALSE;
    return ok;
}

#elif defined(PORTABLE)
// Other PC platforms (PLATFORM_WIN32): no keyboard text entry or settings file.
#include "global.h"
#include "platform.h"

void Platform_StartTextInput(void)
{
}

void Platform_StopTextInput(void)
{
}

u32 Platform_ReadTextInput(char *out, u32 outSize)
{
    return 0;
}

bool8 Platform_TakePastedText(char *out, u32 outSize)
{
    return FALSE;
}

bool8 Platform_ReadMpConfig(char *out, u32 outSize)
{
    if (out != NULL && outSize > 0)
        out[0] = '\0';
    return FALSE;
}

bool8 Platform_WriteMpConfig(const char *line)
{
    return FALSE;
}
#endif
