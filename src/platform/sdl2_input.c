// Pokemon Spectrum (PS-066): key and controller remapping.
//
// pokemon-spectrum.ini sits next to the executable:
//
//   [keyboard]
//   A = Z, J          ; one or more SDL key names, comma separated ("None" = unbound)
//   [controller]
//   A = A             ; Xbox-style controller inputs (see sPadNames)
//
// Actions: A B Start Select L R Up Down Left Right FastForward. A missing file is created with
// the defaults below; a missing action or an unknown name keeps / skips with a console warning.
// The hotkeys (Ctrl+R, Ctrl+P, F3, Alt+Enter) are fixed.

#ifdef PLATFORM_SDL2
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include <SDL2/SDL.h>

#include "global.h"
#include "platform/input.h"

#define INPUT_CONFIG_NAME   "pokemon-spectrum.ini"
#define MAX_BINDINGS        4
#define LINE_MAX_LEN        256

enum
{
    ACTION_A,
    ACTION_B,
    ACTION_START,
    ACTION_SELECT,
    ACTION_L,
    ACTION_R,
    ACTION_UP,
    ACTION_DOWN,
    ACTION_LEFT,
    ACTION_RIGHT,
    ACTION_FAST_FORWARD,
    ACTION_COUNT
};

struct ActionInfo
{
    const char *name;
    u16 gbaButtons;          // 0 for fast-forward
    const char *defaultKeys; // keyboard, SDL key names
    const char *defaultPad;  // controller, sPadNames
};

static const struct ActionInfo sActions[ACTION_COUNT] =
{
    [ACTION_A]            = {"A",           A_BUTTON,      "Z",         "A"},
    [ACTION_B]            = {"B",           B_BUTTON,      "X",         "X"},
    [ACTION_START]        = {"Start",       START_BUTTON,  "Return",    "Start"},
    [ACTION_SELECT]       = {"Select",      SELECT_BUTTON, "\\",        "Back"},
    [ACTION_L]            = {"L",           L_BUTTON,      "A",         "LB"},
    [ACTION_R]            = {"R",           R_BUTTON,      "S",         "RB"},
    [ACTION_UP]           = {"Up",          DPAD_UP,       "Up",        "DPadUp, StickUp"},
    [ACTION_DOWN]         = {"Down",        DPAD_DOWN,     "Down",      "DPadDown, StickDown"},
    [ACTION_LEFT]         = {"Left",        DPAD_LEFT,     "Left",      "DPadLeft, StickLeft"},
    [ACTION_RIGHT]        = {"Right",       DPAD_RIGHT,    "Right",     "DPadRight, StickRight"},
    [ACTION_FAST_FORWARD] = {"FastForward", 0,             "Space",     "RT"},
};

struct PadName
{
    const char *name;
    u32 bit;
};

static const struct PadName sPadNames[] =
{
    {"A", PAD_A}, {"B", PAD_B}, {"X", PAD_X}, {"Y", PAD_Y},
    {"LB", PAD_LB}, {"RB", PAD_RB}, {"LT", PAD_LT}, {"RT", PAD_RT},
    {"Start", PAD_START}, {"Back", PAD_BACK},
    {"LS", PAD_LEFT_THUMB}, {"RS", PAD_RIGHT_THUMB},
    {"DPadUp", PAD_DPAD_UP}, {"DPadDown", PAD_DPAD_DOWN},
    {"DPadLeft", PAD_DPAD_LEFT}, {"DPadRight", PAD_DPAD_RIGHT},
    {"StickUp", PAD_STICK_UP}, {"StickDown", PAD_STICK_DOWN},
    {"StickLeft", PAD_STICK_LEFT}, {"StickRight", PAD_STICK_RIGHT},
};

static SDL_Keycode sKeyBindings[ACTION_COUNT][MAX_BINDINGS]; // SDLK_UNKNOWN = unused
static u32 sPadBindings[ACTION_COUNT];                       // PAD_* bits

static const char sConfigHeader[] =
    "; Pokemon Spectrum controls. Edit, save, and restart the game.\n"
    "; One or more inputs per button, separated by commas. \"None\" leaves a button unbound.\n"
    "; Delete this file to get the defaults back. Comments must be on their own line.\n"
    ";\n"
    "; [keyboard] uses SDL key names, for example: Z, X, Return, Space, Backspace, Tab,\n"
    ";   Left Shift, Right Ctrl, Up, Down, Left, Right, Keypad 8, F1, \\, ., /, [, ].\n"
    ";   (The comma key can't be bound: the comma separates keys.)\n"
    "; [controller] (Xbox-style, Windows): A, B, X, Y, LB, RB, LT, RT, Start, Back, LS, RS,\n"
    ";   DPadUp, DPadDown, DPadLeft, DPadRight, StickUp, StickDown, StickLeft, StickRight.\n"
    "; FastForward runs the game at 5x while held (not during a link trade or battle).\n"
    "; Fixed hotkeys: Ctrl+R reset, Ctrl+P pause, F3 network overlay, Alt+Enter fullscreen.\n";

static void Trim(char *s)
{
    char *start = s;
    size_t len;

    while (*start != '\0' && isspace((unsigned char)*start))
        start++;
    if (start != s)
        memmove(s, start, strlen(start) + 1);
    len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1]))
        s[--len] = '\0';
}

static int FindAction(const char *name)
{
    int i;

    for (i = 0; i < ACTION_COUNT; i++)
    {
        if (SDL_strcasecmp(name, sActions[i].name) == 0)
            return i;
    }
    return -1;
}

static u32 FindPadInput(const char *name)
{
    size_t i;

    for (i = 0; i < ARRAY_COUNT(sPadNames); i++)
    {
        if (SDL_strcasecmp(name, sPadNames[i].name) == 0)
            return sPadNames[i].bit;
    }
    return 0;
}

// Parses a comma-separated list into the keyboard or controller bindings of one action.
// `where` names the source in warnings ("default" or "line N").
static void ParseBindings(int action, bool8 keyboard, const char *value, const char *where)
{
    char list[LINE_MAX_LEN];
    char *item, *next;
    int keyCount = 0;

    if (keyboard)
    {
        for (keyCount = 0; keyCount < MAX_BINDINGS; keyCount++)
            sKeyBindings[action][keyCount] = SDLK_UNKNOWN;
        keyCount = 0;
    }
    else
    {
        sPadBindings[action] = 0;
    }

    snprintf(list, sizeof(list), "%s", value);
    for (item = list; item != NULL; item = next)
    {
        next = strchr(item, ',');
        if (next != NULL)
            *next++ = '\0';
        Trim(item);
        if (item[0] == '\0' || SDL_strcasecmp(item, "None") == 0)
            continue;
        if (keyboard)
        {
            SDL_Keycode key = SDL_GetKeyFromName(item);

            if (key == SDLK_UNKNOWN)
                printf("[input] %s: unknown key \"%s\" for %s (ignored)\n", where, item, sActions[action].name);
            else if (keyCount >= MAX_BINDINGS)
                printf("[input] %s: more than %d keys for %s; \"%s\" ignored\n", where, MAX_BINDINGS, sActions[action].name, item);
            else
                sKeyBindings[action][keyCount++] = key;
        }
        else
        {
            u32 bit = FindPadInput(item);

            if (bit == 0)
                printf("[input] %s: unknown controller input \"%s\" for %s (ignored)\n", where, item, sActions[action].name);
            else
                sPadBindings[action] |= bit;
        }
    }
}

static void SetDefaults(void)
{
    int i;

    for (i = 0; i < ACTION_COUNT; i++)
    {
        ParseBindings(i, TRUE, sActions[i].defaultKeys, "default");
        ParseBindings(i, FALSE, sActions[i].defaultPad, "default");
    }
}

static void WriteDefaultConfig(const char *path)
{
    FILE *f = fopen(path, "w");
    int i;

    if (f == NULL)
    {
        printf("[input] could not create %s; using the default controls\n", path);
        return;
    }
    fputs(sConfigHeader, f);
    fputs("\n[keyboard]\n", f);
    for (i = 0; i < ACTION_COUNT; i++)
        fprintf(f, "%-11s = %s\n", sActions[i].name, sActions[i].defaultKeys);
    fputs("\n[controller]\n", f);
    for (i = 0; i < ACTION_COUNT; i++)
        fprintf(f, "%-11s = %s\n", sActions[i].name, sActions[i].defaultPad);
    fclose(f);
    printf("[input] created %s with the default controls\n", path);
}

static void ReadConfig(FILE *f, const char *path)
{
    char line[LINE_MAX_LEN];
    char where[32];
    int lineNumber = 0;
    int section = -1; // 0 keyboard, 1 controller

    while (fgets(line, sizeof(line), f) != NULL)
    {
        char *eq;
        int action;

        lineNumber++;
        snprintf(where, sizeof(where), "line %d", lineNumber);
        Trim(line);
        // Whole-line comments only, so the keys ";" and "#" can still be bound after '='.
        if (line[0] == '\0' || line[0] == ';' || line[0] == '#')
            continue;
        if (line[0] == '[')
        {
            if (SDL_strncasecmp(line, "[keyboard]", 10) == 0)
                section = 0;
            else if (SDL_strncasecmp(line, "[controller]", 12) == 0)
                section = 1;
            else
            {
                section = -1;
                printf("[input] %s %s: unknown section %s (ignored)\n", path, where, line);
            }
            continue;
        }
        eq = strchr(line, '=');
        if (eq == NULL || section < 0)
        {
            printf("[input] %s %s: not understood (ignored): %s\n", path, where, line);
            continue;
        }
        *eq = '\0';
        Trim(line);
        Trim(eq + 1);
        action = FindAction(line);
        if (action < 0)
        {
            printf("[input] %s %s: unknown button \"%s\" (ignored)\n", path, where, line);
            continue;
        }
        ParseBindings(action, section == 0, eq + 1, where);
    }
}

void Platform_InputLoadConfig(void)
{
    char path[1024];
    char *base = SDL_GetBasePath();
    FILE *f;

    snprintf(path, sizeof(path), "%s%s", base != NULL ? base : "", INPUT_CONFIG_NAME);
    SDL_free(base);

    SetDefaults();
    f = fopen(path, "r");
    if (f == NULL)
    {
        WriteDefaultConfig(path);
        return;
    }
    ReadConfig(f, path);
    fclose(f);
    printf("[input] controls read from %s\n", path);
}

u16 Platform_InputPollKeyboard(bool8 *fastForward)
{
    const Uint8 *state = SDL_GetKeyboardState(NULL);
    bool8 alt = (SDL_GetModState() & KMOD_ALT) != 0;
    u16 buttons = 0;
    int i, j;

    *fastForward = FALSE;
    for (i = 0; i < ACTION_COUNT; i++)
    {
        for (j = 0; j < MAX_BINDINGS && sKeyBindings[i][j] != SDLK_UNKNOWN; j++)
        {
            SDL_Keycode key = sKeyBindings[i][j];

            // Alt+Enter is fullscreen (PS-063), never a game button.
            if (alt && (key == SDLK_RETURN || key == SDLK_KP_ENTER))
                continue;
            if (state[SDL_GetScancodeFromKey(key)])
            {
                if (i == ACTION_FAST_FORWARD)
                    *fastForward = TRUE;
                else
                    buttons |= sActions[i].gbaButtons;
                break;
            }
        }
    }
    return buttons;
}

u16 Platform_InputMapPad(u32 padInputs, bool8 *fastForward)
{
    u16 buttons = 0;
    int i;

    *fastForward = FALSE;
    for (i = 0; i < ACTION_COUNT; i++)
    {
        if ((padInputs & sPadBindings[i]) == 0)
            continue;
        if (i == ACTION_FAST_FORWARD)
            *fastForward = TRUE;
        else
            buttons |= sActions[i].gbaButtons;
    }
    return buttons;
}
#endif // PLATFORM_SDL2
