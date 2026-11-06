// Pokemon Spectrum multiplayer: Join Game IP address + port entry screen (PS-021, SK-UI).
//
// Layout (template: reset_rtc_screen.c):
//
//     Host IP address and port:
//                 ^
//     192.168.001.010:27500        <- one digit is selected (red), arrows above and below
//                 v
//   [ (Up/Down) CHANGE  (Left/Right) MOVE  (A) JOIN  (B) BACK
//     Or type it, or paste with Ctrl+V.                        ]
//
// D-pad: Left/Right moves between the 17 digits; Up/Down adds/subtracts that digit's place
// value (holding repeats). Each field is clamped (octets 0-255, port 1-65535), so no invalid
// address can be produced. A (or START/Enter) joins, B goes back to the Host/Join sub-menu.
//
// PC keyboard (platform hook in src/platform/sdl2_text_input.c): digits type into the
// current field (a field that fills up moves on to the next one), '.' jumps to the next
// octet, ':' to the port, Backspace deletes, Ctrl+V pastes "a.b.c.d" or "a.b.c.d:port".
//
// The last address used is kept in "<savePath>.mp.cfg" (never in the save, AD-10) and is
// pre-filled the next time. Until PS-022, confirming calls MP_Join and continues the save.

#include "global.h"
#include "bg.h"
#include "gpu_regs.h"
#include "main.h"
#include "main_menu.h"
#include "menu.h"
#include "overworld.h"
#include "palette.h"
#include "platform.h"
#include "scanline_effect.h"
#include "sound.h"
#include "sprite.h"
#include "strings.h"
#include "task.h"
#include "text.h"
#include "window.h"
#include "constants/rgb.h"
#include "constants/songs.h"
#include "mp/mp.h"
#include "mp_menu.h"

enum
{
    IP_FIELD_OCTET_1,
    IP_FIELD_OCTET_2,
    IP_FIELD_OCTET_3,
    IP_FIELD_OCTET_4,
    IP_FIELD_PORT,
    IP_FIELD_COUNT,
};

#define IP_DIGIT_COUNT      17  // 4 octets x 3 digits + 5 port digits
#define IP_CFG_LINE_SIZE    64
#define IP_TEXT_BUFFER_SIZE PLATFORM_TEXT_INPUT_QUEUE_SIZE

// Default address when there is no (valid) .mp.cfg yet: this machine, for the dev harness.
static const u16 sDefaultFields[IP_FIELD_COUNT] = {127, 0, 0, 1, MP_DEFAULT_PORT};

static const u8 sFieldDigits[IP_FIELD_COUNT]     = {3, 3, 3, 3, 5};
static const u8 sFieldFirstDigit[IP_FIELD_COUNT] = {0, 3, 6, 9, 12};
static const s32 sFieldMin[IP_FIELD_COUNT]       = {0, 0, 0, 0, 1};
static const s32 sFieldMax[IP_FIELD_COUNT]       = {255, 255, 255, 255, 65535};
static const s32 sPowersOf10[]                   = {1, 10, 100, 1000, 10000};

struct MpIpEntry
{
    u16 fields[IP_FIELD_COUNT]; // Always within sFieldMin..sFieldMax.
    u32 typedValue;             // Keyboard: the digits typed into the current field so far.
    u8 typedDigits;             // Keyboard: how many; 0 = the next digit starts the field afresh.
    u8 cursor;                  // Selected digit, 0..IP_DIGIT_COUNT-1.
    bool8 autoAdvanced;         // Keyboard: the last digit filled a field and moved on.
};

static EWRAM_DATA struct MpIpEntry sIpEntry = {0};

enum
{
    WIN_ADDRESS,
    WIN_HELP,
};

static const struct BgTemplate sBgTemplates[] =
{
    {
        .bg = 0,
        .charBaseIndex = 0,
        .mapBaseIndex = 31,
        .screenSize = 0,
        .paletteMode = 0,
        .priority = 0,
        .baseTile = 0
    }
};

// Tiles 1..0x144 are used; the frames are loaded at 0x200 (dialog) and 0x214 (std)
// by LoadMessageBoxAndBorderGfx.
static const struct WindowTemplate sWindowTemplates[] =
{
    [WIN_ADDRESS] = {
        .bg = 0,
        .tilemapLeft = 3,
        .tilemapTop = 4,
        .width = 24,
        .height = 9,
        .paletteNum = 15,
        .baseBlock = 0x6D
    },
    [WIN_HELP] = {
        .bg = 0,
        .tilemapLeft = 2,
        .tilemapTop = 15,
        .width = 27,
        .height = 4,
        .paletteNum = 15,
        .baseBlock = 1
    },
    DUMMY_WIN_TEMPLATE
};

#define DLG_FRAME_TILE      0x200
#define DLG_FRAME_PALETTE   15
#define STD_FRAME_TILE      0x214
#define STD_FRAME_PALETTE   14

// Address line geometry inside WIN_ADDRESS (192 x 72 px).
#define ADDRESS_LEFT        15
#define ADDRESS_UP_ARROW_Y  20
#define ADDRESS_DIGITS_Y    36
#define ADDRESS_DOWN_ARROW_Y 52
#define DIGIT_ADVANCE       8
#define DOT_ADVANCE         6
#define COLON_ADVANCE       8

static const u8 sTextColor_Normal[]   = {TEXT_COLOR_WHITE, TEXT_COLOR_DARK_GRAY, TEXT_COLOR_LIGHT_GRAY};
static const u8 sTextColor_Selected[] = {TEXT_COLOR_WHITE, TEXT_COLOR_RED, TEXT_COLOR_LIGHT_RED};

#define tState      data[0]
#define tExitAction data[1]

enum
{
    STATE_WAIT_FADE_IN,
    STATE_INPUT,
    STATE_EXIT,
};

enum
{
    EXIT_JOIN,
    EXIT_BACK,
};

static void CB2_MpIpEntry(void);
static void VBlankCB_MpIpEntry(void);
static void Task_MpIpEntry(u8 taskId);

// ---------------------------------------------------------------------------
// Address model (pure, no drawing)
// ---------------------------------------------------------------------------

static u8 GetCursorField(u8 cursor)
{
    if (cursor >= sFieldFirstDigit[IP_FIELD_PORT])
        return IP_FIELD_PORT;
    return cursor / 3;
}

static u16 ClampField(u8 field, s32 value)
{
    if (value < sFieldMin[field])
        value = sFieldMin[field];
    if (value > sFieldMax[field])
        value = sFieldMax[field];
    return value;
}

static void SetCursorToFieldEnd(u8 field)
{
    sIpEntry.cursor = sFieldFirstDigit[field] + sFieldDigits[field] - 1;
}

static void ResetTyping(void)
{
    sIpEntry.typedValue = 0;
    sIpEntry.typedDigits = 0;
    sIpEntry.autoAdvanced = FALSE;
}

// D-pad Up/Down: add `direction` (+1 / -1) times the selected digit's place value.
static bool8 ChangeSelectedDigit(s32 direction)
{
    u8 field = GetCursorField(sIpEntry.cursor);
    u8 lastDigit = sFieldFirstDigit[field] + sFieldDigits[field] - 1;
    s32 place = sPowersOf10[lastDigit - sIpEntry.cursor];
    u16 value = ClampField(field, (s32)sIpEntry.fields[field] + direction * place);

    ResetTyping();
    if (value == sIpEntry.fields[field])
        return FALSE;
    sIpEntry.fields[field] = value;
    return TRUE;
}

// Keyboard digit.
static bool8 TypeDigit(u8 digit)
{
    u8 field = GetCursorField(sIpEntry.cursor);

    if (sIpEntry.typedDigits >= sFieldDigits[field])
        return FALSE; // the port is full; extra digits are ignored
    sIpEntry.typedValue = sIpEntry.typedValue * 10 + digit;
    sIpEntry.typedDigits++;
    sIpEntry.autoAdvanced = FALSE;
    sIpEntry.fields[field] = ClampField(field, sIpEntry.typedValue);
    SetCursorToFieldEnd(field);
    if (sIpEntry.typedDigits == sFieldDigits[field] && field < IP_FIELD_PORT)
    {
        // A full octet moves on, so "192168001010" works too.
        SetCursorToFieldEnd(field + 1);
        ResetTyping();
        sIpEntry.autoAdvanced = TRUE;
    }
    return TRUE;
}

// Keyboard '.' or ':'.
static bool8 TypeSeparator(char c)
{
    u8 field = GetCursorField(sIpEntry.cursor);
    u8 next;

    if (c == '.')
    {
        // "192." after a full octet already moved on: the dot just confirms it.
        if (sIpEntry.autoAdvanced && field >= IP_FIELD_OCTET_2 && field <= IP_FIELD_OCTET_4)
        {
            sIpEntry.autoAdvanced = FALSE;
            return FALSE;
        }
        if (field >= IP_FIELD_OCTET_4)
            return FALSE;
        next = field + 1;
    }
    else
    {
        if (sIpEntry.autoAdvanced && field == IP_FIELD_PORT)
        {
            sIpEntry.autoAdvanced = FALSE;
            return FALSE;
        }
        if (field == IP_FIELD_PORT)
            return FALSE;
        next = IP_FIELD_PORT;
    }
    SetCursorToFieldEnd(next);
    ResetTyping();
    return TRUE;
}

static u8 MpCountDigits(u32 value)
{
    u8 count = 1;

    while (value >= 10)
    {
        value /= 10;
        count++;
    }
    return count;
}

// Keyboard Backspace: delete the last digit, crossing back into the previous field like a
// text box when nothing was typed in this one.
static bool8 TypeBackspace(void)
{
    u8 field = GetCursorField(sIpEntry.cursor);

    if (sIpEntry.typedDigits == 0)
    {
        // Nothing typed here yet. If the cursor just moved on from a full field, or this
        // field is already empty (at its minimum), go back to the previous field;
        // otherwise start deleting this field's current value.
        if ((sIpEntry.autoAdvanced || sIpEntry.fields[field] == sFieldMin[field]) && field > IP_FIELD_OCTET_1)
            field--;
        else if (sIpEntry.fields[field] == sFieldMin[field])
            return FALSE; // first octet already 0
        sIpEntry.typedValue = sIpEntry.fields[field];
        sIpEntry.typedDigits = MpCountDigits(sIpEntry.typedValue);
    }
    sIpEntry.typedDigits--;
    sIpEntry.typedValue /= 10;
    sIpEntry.autoAdvanced = FALSE;
    sIpEntry.fields[field] = ClampField(field, sIpEntry.typedValue);
    SetCursorToFieldEnd(field);
    return TRUE;
}

// Reads 1..maxDigits decimal digits.
static bool8 ParseNumber(const char **str, u8 maxDigits, u32 *out)
{
    const char *p = *str;
    u32 value = 0;
    u8 count = 0;

    while (*p >= '0' && *p <= '9')
    {
        if (count == maxDigits)
            return FALSE;
        value = value * 10 + (u32)(*p - '0');
        count++;
        p++;
    }
    if (count == 0)
        return FALSE;
    *str = p;
    *out = value;
    return TRUE;
}

static bool8 IsSpace(char c)
{
    return (c == ' ' || c == '\t' || c == '\r' || c == '\n');
}

// "a.b.c.d" or "a.b.c.d:port" (surrounding spaces allowed). Without a port the port is
// MP_DEFAULT_PORT. Writes `out` only on success. Used for paste and the .mp.cfg file.
static bool8 ParseAddress(const char *str, u16 *out)
{
    u16 fields[IP_FIELD_COUNT];
    u32 value;
    u8 i;

    if (str == NULL)
        return FALSE;
    while (IsSpace(*str))
        str++;
    for (i = IP_FIELD_OCTET_1; i <= IP_FIELD_OCTET_4; i++)
    {
        if (!ParseNumber(&str, 3, &value) || value > 255)
            return FALSE;
        fields[i] = value;
        if (i < IP_FIELD_OCTET_4)
        {
            if (*str != '.')
                return FALSE;
            str++;
        }
    }
    fields[IP_FIELD_PORT] = MP_DEFAULT_PORT;
    if (*str == ':')
    {
        str++;
        if (!ParseNumber(&str, 5, &value) || value < 1 || value > 65535)
            return FALSE;
        fields[IP_FIELD_PORT] = value;
    }
    while (IsSpace(*str))
        str++;
    if (*str != '\0')
        return FALSE;
    for (i = 0; i < IP_FIELD_COUNT; i++)
        out[i] = fields[i];
    return TRUE;
}

static char *AppendNumber(char *dest, u32 value)
{
    char digits[10];
    u8 count = 0;

    do
    {
        digits[count++] = '0' + (value % 10);
        value /= 10;
    } while (value != 0 && count < ARRAY_COUNT(digits));
    while (count > 0)
        *dest++ = digits[--count];
    return dest;
}

// "a.b.c.d" without leading zeros (a leading zero could be read as octal). `out` must hold
// MP_ADDRESS_MAX_LEN + 1 bytes.
static void FormatIp(char *out)
{
    u8 i;

    for (i = IP_FIELD_OCTET_1; i <= IP_FIELD_OCTET_4; i++)
    {
        out = AppendNumber(out, sIpEntry.fields[i]);
        if (i < IP_FIELD_OCTET_4)
            *out++ = '.';
    }
    *out = '\0';
}

// "a.b.c.d:port" for the .mp.cfg file. `out` must hold IP_CFG_LINE_SIZE bytes.
static void FormatConfigLine(char *out)
{
    FormatIp(out);
    while (*out != '\0')
        out++;
    *out++ = ':';
    out = AppendNumber(out, sIpEntry.fields[IP_FIELD_PORT]);
    *out = '\0';
}

static void LoadAddress(void)
{
    char line[IP_CFG_LINE_SIZE];
    u8 i;

    for (i = 0; i < IP_FIELD_COUNT; i++)
        sIpEntry.fields[i] = sDefaultFields[i];
    if (Platform_ReadMpConfig(line, sizeof(line)) && !ParseAddress(line, sIpEntry.fields))
        DBGPRINTF("[MP] Join Game: ignoring unreadable last address in .mp.cfg\n");
    ResetTyping();
    SetCursorToFieldEnd(IP_FIELD_OCTET_1);
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

static void PrintChar(u8 x, u8 y, u8 c, const u8 *color)
{
    u8 str[2];

    str[0] = c;
    str[1] = EOS;
    AddTextPrinterParameterized3(WIN_ADDRESS, FONT_NORMAL, x, y, color, TEXT_SKIP_DRAW, str);
}

static void PrintAddress(void)
{
    u8 field, digit;
    u8 digitIndex = 0;
    u8 x = ADDRESS_LEFT;

    FillWindowPixelBuffer(WIN_ADDRESS, PIXEL_FILL(TEXT_COLOR_WHITE));
    AddTextPrinterParameterized3(WIN_ADDRESS, FONT_NORMAL, 0, 1, sTextColor_Normal, TEXT_SKIP_DRAW, gText_MpHostAddress);
    for (field = 0; field < IP_FIELD_COUNT; field++)
    {
        for (digit = 0; digit < sFieldDigits[field]; digit++)
        {
            s32 place = sPowersOf10[sFieldDigits[field] - 1 - digit];
            u8 c = CHAR_0 + (sIpEntry.fields[field] / place) % 10;

            if (digitIndex == sIpEntry.cursor)
            {
                PrintChar(x, ADDRESS_UP_ARROW_Y, CHAR_UP_ARROW, sTextColor_Selected);
                PrintChar(x, ADDRESS_DIGITS_Y, c, sTextColor_Selected);
                PrintChar(x, ADDRESS_DOWN_ARROW_Y, CHAR_DOWN_ARROW, sTextColor_Selected);
            }
            else
            {
                PrintChar(x, ADDRESS_DIGITS_Y, c, sTextColor_Normal);
            }
            x += DIGIT_ADVANCE;
            digitIndex++;
        }
        if (field < IP_FIELD_OCTET_4)
        {
            PrintChar(x, ADDRESS_DIGITS_Y, CHAR_PERIOD, sTextColor_Normal);
            x += DOT_ADVANCE;
        }
        else if (field == IP_FIELD_OCTET_4)
        {
            PrintChar(x + 1, ADDRESS_DIGITS_Y, CHAR_COLON, sTextColor_Normal);
            x += COLON_ADVANCE;
        }
    }
    CopyWindowToVram(WIN_ADDRESS, COPYWIN_GFX);
}

static void DrawScreen(void)
{
    DrawStdFrameWithCustomTileAndPalette(WIN_ADDRESS, FALSE, STD_FRAME_TILE, STD_FRAME_PALETTE);
    PrintAddress();
    DrawDialogFrameWithCustomTileAndPalette(WIN_HELP, FALSE, DLG_FRAME_TILE, DLG_FRAME_PALETTE);
    AddTextPrinterParameterized(WIN_HELP, FONT_NORMAL, gText_MpIpEntryHelp, 0, 1, TEXT_SKIP_DRAW, NULL);
    CopyWindowToVram(WIN_HELP, COPYWIN_FULL);
    CopyWindowToVram(WIN_ADDRESS, COPYWIN_FULL);
    ScheduleBgCopyTilemapToVram(0);
}

// ---------------------------------------------------------------------------
// Screen
// ---------------------------------------------------------------------------

static void InitBgAndWindows(void)
{
    ClearScheduledBgCopiesToVram();
    ResetBgsAndClearDma3BusyFlags(0);
    InitBgsFromTemplates(0, sBgTemplates, ARRAY_COUNT(sBgTemplates));
    ChangeBgX(0, 0, BG_COORD_SET);
    ChangeBgY(0, 0, BG_COORD_SET);
    ScheduleBgCopyTilemapToVram(0);
    SetGpuReg(REG_OFFSET_DISPCNT, DISPCNT_OBJ_ON | DISPCNT_OBJ_1D_MAP);
    ShowBg(0);
    InitWindows(sWindowTemplates);
    DeactivateAllTextPrinters();
    LoadMessageBoxAndBorderGfx();
}

void CB2_MpInitIpEntryScreen(void)
{
    u32 i;

    SetGpuReg(REG_OFFSET_DISPCNT, 0);
    SetVBlankCallback(NULL);
    // The main menu leaves its highlight window and darken blend on; turn them off.
    SetGpuReg(REG_OFFSET_WIN0H, 0);
    SetGpuReg(REG_OFFSET_WIN0V, 0);
    SetGpuReg(REG_OFFSET_WININ, 0);
    SetGpuReg(REG_OFFSET_WINOUT, 0);
    SetGpuReg(REG_OFFSET_BLDCNT, 0);
    SetGpuReg(REG_OFFSET_BLDALPHA, 0);
    SetGpuReg(REG_OFFSET_BLDY, 0);
    DmaClear16(3, PLTT, PLTT_SIZE);
    DmaClearLarge16(3, (void *)VRAM, VRAM_SIZE, 0x1000);
    ResetOamRange(0, 128);
    LoadOam();
    ScanlineEffect_Stop();
    ScanlineEffect_Clear();
    ResetSpriteData();
    FreeAllSpritePalettes();
    ResetTasks();
    ResetPaletteFade();
    // Black backdrop: don't fade back in to the main menu's palettes.
    for (i = 0; i < PLTT_BUFFER_SIZE; i++)
    {
        gPlttBufferUnfaded[i] = RGB_BLACK;
        gPlttBufferFaded[i] = RGB_BLACK;
    }
    InitBgAndWindows();
    LoadAddress();
    DrawScreen();
    // Start black (applied immediately, so nothing flashes before the first frame).
    BeginNormalPaletteFade(PALETTES_ALL, 0, 16, 0, RGB_BLACK);
    SetVBlankCallback(VBlankCB_MpIpEntry);
    SetMainCallback2(CB2_MpIpEntry);
    CreateTask(Task_MpIpEntry, 80);
}

static void CB2_MpIpEntry(void)
{
    RunTasks();
    AnimateSprites();
    BuildOamBuffer();
    DoScheduledBgTilemapCopiesToVram();
    UpdatePaletteFade();
}

static void VBlankCB_MpIpEntry(void)
{
    ProcessSpriteCopyRequests();
    LoadOam();
    TransferPlttBuffer();
}

static void StartExit(u8 taskId, u8 action)
{
    s16 *data = gTasks[taskId].data;

    PlaySE(SE_SELECT);
    Platform_StopTextInput();
    BeginNormalPaletteFade(PALETTES_ALL, 0, 0, 16, RGB_BLACK);
    tExitAction = action;
    tState = STATE_EXIT;
}

// Keyboard and clipboard (PC). Returns TRUE if the address changed.
static bool8 HandleKeyboard(void)
{
    char text[IP_TEXT_BUFFER_SIZE];
    u32 count;
    u32 i;
    bool8 changed = FALSE;

    if (Platform_TakePastedText(text, sizeof(text)))
    {
        if (ParseAddress(text, sIpEntry.fields))
        {
            ResetTyping();
            SetCursorToFieldEnd(IP_FIELD_PORT);
            changed = TRUE;
        }
        else
        {
            PlaySE(SE_BOO);
        }
    }

    count = Platform_ReadTextInput(text, sizeof(text));
    for (i = 0; i < count; i++)
    {
        char c = text[i];

        if (c >= '0' && c <= '9')
            changed |= TypeDigit(c - '0');
        else if (c == '.' || c == ':')
            changed |= TypeSeparator(c);
        else if (c == PLATFORM_TEXT_BACKSPACE)
            changed |= TypeBackspace();
    }
    return changed;
}

static void HandleInput(u8 taskId)
{
    bool8 changed = HandleKeyboard();

    if (JOY_NEW(A_BUTTON | START_BUTTON))
    {
        StartExit(taskId, EXIT_JOIN);
        return;
    }
    if (JOY_NEW(B_BUTTON))
    {
        StartExit(taskId, EXIT_BACK);
        return;
    }

    if (JOY_REPEAT(DPAD_LEFT))
    {
        if (sIpEntry.cursor > 0)
        {
            sIpEntry.cursor--;
            ResetTyping();
            changed = TRUE;
        }
    }
    else if (JOY_REPEAT(DPAD_RIGHT))
    {
        if (sIpEntry.cursor < IP_DIGIT_COUNT - 1)
        {
            sIpEntry.cursor++;
            ResetTyping();
            changed = TRUE;
        }
    }
    else if (JOY_REPEAT(DPAD_UP))
    {
        changed |= ChangeSelectedDigit(1);
    }
    else if (JOY_REPEAT(DPAD_DOWN))
    {
        changed |= ChangeSelectedDigit(-1);
    }

    if (changed)
    {
        PlaySE(SE_SELECT);
        PrintAddress();
    }
}

static void JoinAndContinue(void)
{
    char ip[MP_ADDRESS_MAX_LEN + 1];
    char line[IP_CFG_LINE_SIZE];
    u16 port = sIpEntry.fields[IP_FIELD_PORT];
    u8 state;

    FormatIp(ip);
    FormatConfigLine(line);
    if (!Platform_WriteMpConfig(line))
        DBGPRINTF("[MP] Join Game: could not save the last address to .mp.cfg\n");

    // MP_Join precondition (mp.h): state is OFFLINE or ERROR.
    state = MP_GetState();
    if (state != MP_STATE_OFFLINE && state != MP_STATE_ERROR)
        DBGPRINTF("[MP] Join Game: already in a session (state %u); not joining\n", (unsigned int)state);
    else if (!MP_Join(ip, port))
        DBGPRINTF("[MP] Join Game: could not join %s:%u; playing single-player\n", ip, (unsigned int)port);

    // Placeholder until PS-022 adds the "Connecting..." screen: continue the save, the same
    // way the main menu's CONTINUE does.
    gPlttBufferUnfaded[0] = RGB_BLACK;
    gPlttBufferFaded[0] = RGB_BLACK;
    SetMainCallback2(CB2_ContinueSavedGame);
}

static void Task_MpIpEntry(u8 taskId)
{
    s16 *data = gTasks[taskId].data;

    switch (tState)
    {
    case STATE_WAIT_FADE_IN:
        if (!gPaletteFade.active)
        {
            Platform_StartTextInput();
            tState = STATE_INPUT;
        }
        break;
    case STATE_INPUT:
        HandleInput(taskId);
        break;
    case STATE_EXIT:
        if (!gPaletteFade.active)
        {
            if (tExitAction == EXIT_JOIN)
                JoinAndContinue();
            else
                SetMainCallback2(CB2_ReinitMainMenuMultiplayer);
            DestroyTask(taskId);
            FreeAllWindowBuffers();
        }
        break;
    }
}

#undef tState
#undef tExitAction
