// Pokemon Spectrum multiplayer: Host Game screen (PS-022, SK-UI).
//
// CB2_MpStartHosting() calls MP_Host(MP_DEFAULT_PORT) and then shows one of:
//
//   Hosting on port 27500 (UDP).          | Couldn't open port 27500.
//   Friends can join at:                  | Is another game already hosting
//   192.168.1.20  10.0.0.5                | on this PC?
//   Online play needs port forwarding.    |
//   [A START THE GAME / B STOP HOSTING]   | [A B BACK]
//
// Success: A continues the save with the session live (the same exit as the main menu's
// CONTINUE); B stops hosting (MP_Leave) and goes back to the Host/Join sub-menu.
// Failure: A or B acknowledges the error (MP_Leave) and goes back to the sub-menu.

#include "global.h"
#include "main.h"
#include "main_menu.h"
#include "menu.h"
#include "overworld.h"
#include "palette.h"
#include "platform.h"
#include "sound.h"
#include "string_util.h"
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
    WIN_INFO,
    WIN_HELP,
};

// Tiles 1..0x1A4; frames at 0x200 / 0x214.
static const struct WindowTemplate sWindowTemplates[] =
{
    [WIN_INFO] = {
        .bg = 0,
        .tilemapLeft = 2,
        .tilemapTop = 2,
        .width = 26,
        .height = 11,
        .paletteNum = 15,
        .baseBlock = 1
    },
    [WIN_HELP] = {
        .bg = 0,
        .tilemapLeft = 2,
        .tilemapTop = 15,
        .width = 27,
        .height = 4,
        .paletteNum = 15,
        .baseBlock = 0x11F
    },
    DUMMY_WIN_TEMPLATE
};

#define DLG_FRAME_TILE      0x200
#define DLG_FRAME_PALETTE   15
#define STD_FRAME_TILE      0x214
#define STD_FRAME_PALETTE   14

#define tState      data[0]
#define tHosting    data[1]    // TRUE: MP_Host succeeded
#define tExitAction data[2]

enum
{
    STATE_WAIT_FADE_IN,
    STATE_INPUT,
    STATE_EXIT,
};

enum
{
    EXIT_CONTINUE,
    EXIT_BACK,
};

// The info window has room for one line of addresses: two dotted quads.
#define HOST_SCREEN_MAX_ADDRESSES 2

static void BufferLanAddresses(u8 *dest, u32 destSize)
{
    char ascii[HOST_SCREEN_MAX_ADDRESSES * 16 + 4];
    char spaced[HOST_SCREEN_MAX_ADDRESSES * 17 + 4];
    u32 count = Platform_GetLanAddresses(ascii, sizeof(ascii), HOST_SCREEN_MAX_ADDRESSES);
    u32 i, j = 0;

    if (count == 0)
    {
        StringCopy(dest, gText_MpNoLanAddress);
        return;
    }
    // "a b" -> "a  b" (two spaces read better in the proportional font)
    for (i = 0; ascii[i] != '\0' && j < sizeof(spaced) - 3; i++)
    {
        if (ascii[i] == ' ')
            spaced[j++] = ' ';
        spaced[j++] = ascii[i];
    }
    spaced[j] = '\0';
    MpMenu_AsciiToGameString(dest, spaced, destSize);
}

static void DrawScreen(bool8 hosting)
{
    u16 port = MP_DEFAULT_PORT;

    ConvertIntToDecimalStringN(gStringVar1, port, STR_CONV_MODE_LEFT_ALIGN, 5);
    if (hosting)
    {
        BufferLanAddresses(gStringVar2, 0x100);
        StringExpandPlaceholders(gStringVar4, gText_MpHostInfo);
    }
    else
    {
        StringExpandPlaceholders(gStringVar4, gText_MpHostFailed);
    }

    DrawStdFrameWithCustomTileAndPalette(WIN_INFO, FALSE, STD_FRAME_TILE, STD_FRAME_PALETTE);
    AddTextPrinterParameterized(WIN_INFO, FONT_NORMAL, gStringVar4, 0, 1, TEXT_SKIP_DRAW, NULL);
    DrawDialogFrameWithCustomTileAndPalette(WIN_HELP, FALSE, DLG_FRAME_TILE, DLG_FRAME_PALETTE);
    AddTextPrinterParameterized(WIN_HELP, FONT_NORMAL, hosting ? gText_MpHostInfoHelp : gText_MpHostFailedHelp, 0, 1, TEXT_SKIP_DRAW, NULL);
    CopyWindowToVram(WIN_INFO, COPYWIN_FULL);
    CopyWindowToVram(WIN_HELP, COPYWIN_FULL);
    ScheduleBgCopyTilemapToVram(0);
}

static void Task_MpHostScreen(u8 taskId)
{
    s16 *data = gTasks[taskId].data;

    switch (tState)
    {
    case STATE_WAIT_FADE_IN:
        if (!gPaletteFade.active)
            tState = STATE_INPUT;
        break;
    case STATE_INPUT:
        if (JOY_NEW(A_BUTTON) || JOY_NEW(B_BUTTON))
        {
            tExitAction = (tHosting && JOY_NEW(A_BUTTON)) ? EXIT_CONTINUE : EXIT_BACK;
            PlaySE(SE_SELECT);
            BeginNormalPaletteFade(PALETTES_ALL, 0, 0, 16, RGB_BLACK);
            tState = STATE_EXIT;
        }
        break;
    case STATE_EXIT:
        if (!gPaletteFade.active)
        {
            if (tExitAction == EXIT_CONTINUE)
            {
                // Same exit as CONTINUE in the main menu. The session stays live.
                gPlttBufferUnfaded[0] = RGB_BLACK;
                gPlttBufferFaded[0] = RGB_BLACK;
                SetMainCallback2(CB2_ContinueSavedGame);
            }
            else
            {
                // Stop hosting, or acknowledge the failure (ERROR -> OFFLINE).
                MP_Leave();
                SetMainCallback2(CB2_ReinitMainMenuMultiplayerHost);
            }
            DestroyTask(taskId);
            FreeAllWindowBuffers();
        }
        break;
    }
}

void CB2_MpStartHosting(void)
{
    bool8 hosting;
    u8 taskId;

    // The menu decides now: a pending --host/--join auto-start must not fire as well
    // when the save continues.
    MP_SetAutoStart(MP_AUTOSTART_NONE, NULL, 0);
    if (MP_GetState() != MP_STATE_OFFLINE)
        MP_Leave();
    hosting = MP_Host(MP_DEFAULT_PORT);
    if (!hosting)
        DBGPRINTF("[MP] Host Game: could not host on port %u (%s)\n", (unsigned int)MP_DEFAULT_PORT, MP_GetReasonName(MP_GetErrorReason()));

    MpMenu_InitScreen(sWindowTemplates);
    DrawScreen(hosting);
    taskId = MpMenu_StartScreen(Task_MpHostScreen);
    gTasks[taskId].tHosting = hosting;
}

#undef tState
#undef tHosting
#undef tExitAction
