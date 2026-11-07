// Pokemon Spectrum multiplayer: shared UI pieces (SK-UI, PS-022, PS-033).
//
// - The plain black menu screen setup used by the Join Game and Host Game screens.
// - Game-charset text for MP_REASON_* codes.
// - MpUi_Update(): the single MP_PollEvent consumer (design.md 3.2), called once per frame
//   from sdl2.c right after MP_Update(). PLAYER_JOINED / PLAYER_LEFT queue toasts (PS-033).
// - MpUi_TryShowFieldNotice(): shows "Disconnected from host" (and other session errors)
//   in the field once the player is back in free control. Called from DoCB1_Overworld.
//
// Session errors are taken from MP_GetState() == MP_STATE_ERROR, not from events, so an
// error that happens during a battle, script or menu simply waits (ERROR is sticky) until
// the field can show it. The menus (Join/Host screens) handle their own errors and call
// MP_Leave(), so the field never shows those twice.

#include "global.h"
#include "bg.h"
#include "event_object_lock.h"
#include "event_object_movement.h"
#include "field_message_box.h"
#include "field_player_avatar.h"
#include "gpu_regs.h"
#include "main.h"
#include "menu.h"
#include "overworld.h"
#include "palette.h"
#include "region_map.h"
#include "scanline_effect.h"
#include "script.h"
#include "sound.h"
#include "sprite.h"
#include "string_util.h"
#include "strings.h"
#include "task.h"
#include "text.h"
#include "window.h"
#include "constants/rgb.h"
#include "constants/songs.h"
#include "mp/mp.h"
#include "mp_menu.h"

// ---------------------------------------------------------------------------
// Plain menu screen (template: reset_rtc_screen.c)
// ---------------------------------------------------------------------------

static const struct BgTemplate sMpMenuBgTemplates[] =
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

static void CB2_MpMenuScreen(void)
{
    RunTasks();
    AnimateSprites();
    BuildOamBuffer();
    DoScheduledBgTilemapCopiesToVram();
    UpdatePaletteFade();
}

static void VBlankCB_MpMenuScreen(void)
{
    ProcessSpriteCopyRequests();
    LoadOam();
    TransferPlttBuffer();
}

void MpMenu_InitScreen(const struct WindowTemplate *windows)
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
    // Black backdrop: don't fade back in to the previous screen's palettes.
    for (i = 0; i < PLTT_BUFFER_SIZE; i++)
    {
        gPlttBufferUnfaded[i] = RGB_BLACK;
        gPlttBufferFaded[i] = RGB_BLACK;
    }
    ClearScheduledBgCopiesToVram();
    ResetBgsAndClearDma3BusyFlags(0);
    InitBgsFromTemplates(0, sMpMenuBgTemplates, ARRAY_COUNT(sMpMenuBgTemplates));
    ChangeBgX(0, 0, BG_COORD_SET);
    ChangeBgY(0, 0, BG_COORD_SET);
    ScheduleBgCopyTilemapToVram(0);
    SetGpuReg(REG_OFFSET_DISPCNT, DISPCNT_OBJ_ON | DISPCNT_OBJ_1D_MAP);
    ShowBg(0);
    InitWindows(windows);
    DeactivateAllTextPrinters();
    LoadMessageBoxAndBorderGfx();
}

u8 MpMenu_StartScreen(void (*task)(u8 taskId))
{
    // Start black (applied immediately, so nothing flashes before the first frame).
    BeginNormalPaletteFade(PALETTES_ALL, 0, 16, 0, RGB_BLACK);
    SetVBlankCallback(VBlankCB_MpMenuScreen);
    SetMainCallback2(CB2_MpMenuScreen);
    return CreateTask(task, 80);
}

void MpMenu_AsciiToGameString(u8 *dest, const char *src, u32 destSize)
{
    u32 n = 0;

    if (destSize == 0)
        return;
    while (src != NULL && *src != '\0' && n < destSize - 1)
    {
        char c = *src++;

        if (c >= '0' && c <= '9')
            dest[n++] = CHAR_0 + (c - '0');
        else if (c == '.')
            dest[n++] = CHAR_PERIOD;
        else if (c == ':')
            dest[n++] = CHAR_COLON;
        else if (c == ' ')
            dest[n++] = CHAR_SPACE;
        else if (c == '\n')
            dest[n++] = CHAR_NEWLINE;
    }
    dest[n] = EOS;
}

// ---------------------------------------------------------------------------
// Reason texts
// ---------------------------------------------------------------------------

const u8 *MpUi_GetReasonText(u8 reason)
{
    switch (reason)
    {
    case MP_REASON_VERSION:
    case MP_REASON_BUILD:
        return gText_MpReasonVersion;
    case MP_REASON_FULL:
        return gText_MpReasonFull;
    case MP_REASON_BAD_DATA:
        return gText_MpReasonBadData;
    case MP_REASON_TIMEOUT:
        return gText_MpReasonTimeout;
    case MP_REASON_BAD_ADDRESS:
        return gText_MpReasonBadAddress;
    case MP_REASON_TRANSPORT_FAILED:
        return gText_MpReasonTransport;
    case MP_REASON_HOST_CLOSED:
        return gText_MpReasonHostClosed;
    case MP_REASON_KICKED:
        return gText_MpReasonKicked;
    case MP_REASON_CONNECTION_LOST:
    case MP_REASON_PROTOCOL_ERROR:
        return gText_MpReasonLost;
    case MP_REASON_BIND_FAILED:
        return gText_MpReasonBindFailed;
    default:
        return gText_MpReasonUnknown;
    }
}

// ---------------------------------------------------------------------------
// Event pump
// ---------------------------------------------------------------------------

// Join/leave notices (PS-033): a small queue of ready-to-print game-charset lines.
#define MP_TOAST_QUEUE_SIZE   6
#define MP_TOAST_TEXT_SIZE    40    // 7-char name + longest suffix (16) + EOS, with room to spare
#define MP_TOAST_FRAMES       180   // about 3 s on screen
#define MP_TOAST_MIN_FRAMES   90    // hidden sooner than this (script, menu...) -> shown again later

static EWRAM_DATA u8 sMpToastQueue[MP_TOAST_QUEUE_SIZE][MP_TOAST_TEXT_SIZE] = {0};
static EWRAM_DATA u8 sMpToastHead = 0;
static EWRAM_DATA u8 sMpToastCount = 0;

static void MpUi_QueueToast(const u8 *name, const u8 *suffix)
{
    u8 *dest;
    u32 i;

    if (sMpToastCount >= MP_TOAST_QUEUE_SIZE)
    {
        // Full: drop the oldest.
        sMpToastHead = (sMpToastHead + 1) % MP_TOAST_QUEUE_SIZE;
        sMpToastCount--;
    }
    dest = sMpToastQueue[(sMpToastHead + sMpToastCount) % MP_TOAST_QUEUE_SIZE];
    // The event's name is EOS-terminated by contract; copy at most 7 characters anyway.
    for (i = 0; i < MP_PLAYER_NAME_SIZE - 1 && name[i] != EOS; i++)
        dest[i] = name[i];
    dest[i] = EOS;
    if (i == 0)
        StringCopy(dest, gText_MpSomeone);
    StringAppend(dest, suffix);
    sMpToastCount++;
}

void MpUi_Update(void)
{
    struct MpEvent event;
    u32 guard;

    // Single consumer, bounded by the queue size. Session start/failure/disconnect are read
    // from MP_GetState() by the screens and the field notice, so only the player events
    // matter here.
    for (guard = 0; guard < MP_EVENT_QUEUE_SIZE && MP_PollEvent(&event); guard++)
    {
        switch (event.type)
        {
        case MP_EVENT_PLAYER_JOINED:
            MpUi_QueueToast(event.name, gText_MpToastJoined);
            break;
        case MP_EVENT_PLAYER_LEFT:
            MpUi_QueueToast(event.name, gText_MpToastLeft);
            break;
        case MP_EVENT_INTERACT_REQUEST:     // PS-041
        case MP_EVENT_INTERACT_RESULT:
        case MP_EVENT_INTERACT_CANCELLED:
            MpInteractUi_OnEvent(&event);
            break;
        case MP_EVENT_SESSION_STARTED:
        case MP_EVENT_CONNECT_FAILED:
        case MP_EVENT_DISCONNECTED:
        case MP_EVENT_PLAYER_UPDATED:
        default:
            break;
        }
    }
    MpInteractUi_Update(); // PS-041: busy bookkeeping for a waiting incoming request
}

// ---------------------------------------------------------------------------
// Join / leave toast (PS-033): non-blocking, top-left, about 3 s
// ---------------------------------------------------------------------------
//
// Started only in free field control (from MpUi_TryShowFieldNotice, i.e. DoCB1_Overworld with
// unlocked controls), never while a script runs or a message box or the map name pop-up is up.
// The player keeps walking. It disappears at once if field controls get locked (script, start
// menu, battle, warp), if the map name pop-up starts (it scrolls BG0), or if the field is left.
//
// Field BG0 tiles: the window uses 1..0x28. The map name pop-up is at 0x107, YES/NO at 0x125,
// the start menu at 0x139 and the dialog box at 0x194, and none of them can be up at the same
// time (they all lock controls or scroll BG0, which hides the toast first).

static const struct WindowTemplate sMpToastWindowTemplate =
{
    .bg = 0,
    .tilemapLeft = 1,
    .tilemapTop = 1,
    .width = 20,
    .height = 2,
    .paletteNum = 15,
    .baseBlock = 0x1
};

#define tTimer      data[0]
#define tWindowId   data[1]

static void Task_MpToast(u8 taskId)
{
    s16 *data = gTasks[taskId].data;

    if (gMain.callback2 != CB2_Overworld)
    {
        // The field's windows are gone (or going): don't touch the window.
        if (tTimer >= MP_TOAST_MIN_FRAMES && sMpToastCount > 0)
        {
            sMpToastHead = (sMpToastHead + 1) % MP_TOAST_QUEUE_SIZE;
            sMpToastCount--;
        }
        DestroyTask(taskId);
        return;
    }
    tTimer++;
    if (tTimer >= MP_TOAST_FRAMES
     || ArePlayerFieldControlsLocked()
     || ScriptContext_IsEnabled()
     || GetGpuReg(REG_OFFSET_BG0VOFS) != 0)
    {
        ClearStdWindowAndFrameToTransparent(tWindowId, TRUE);
        RemoveWindow(tWindowId);
        if (tTimer >= MP_TOAST_MIN_FRAMES && sMpToastCount > 0)
        {
            sMpToastHead = (sMpToastHead + 1) % MP_TOAST_QUEUE_SIZE;
            sMpToastCount--;
        }
        DestroyTask(taskId);
    }
}

static void MpUi_TryShowToast(void)
{
    u8 windowId;
    u8 taskId;

    if (sMpToastCount == 0 || FuncIsActiveTask(Task_MpToast))
        return;
    if (ScriptContext_IsEnabled() || !IsFieldMessageBoxHidden() || GetGpuReg(REG_OFFSET_BG0VOFS) != 0)
        return;

    windowId = AddWindow(&sMpToastWindowTemplate);
    if (windowId == WINDOW_NONE)
        return;
    LoadMessageBoxAndBorderGfx();
    PutWindowTilemap(windowId);
    DrawStdWindowFrame(windowId, FALSE);
    FillWindowPixelBuffer(windowId, PIXEL_FILL(TEXT_COLOR_WHITE));
    AddTextPrinterParameterized(windowId, FONT_NORMAL, sMpToastQueue[sMpToastHead], 0, 1, TEXT_SKIP_DRAW, NULL);
    CopyWindowToVram(windowId, COPYWIN_FULL);

    taskId = CreateTask(Task_MpToast, 90);
    gTasks[taskId].tTimer = 0;
    gTasks[taskId].tWindowId = windowId;
}

#undef tTimer
#undef tWindowId

// ---------------------------------------------------------------------------
// PLAYERS list (PS-033), shown by the start menu in place of its own window
// ---------------------------------------------------------------------------
//
//   PLAYERS  2/4
//   NAME     LOCATION            BADGES
//   BRENDAN  LITTLEROOT TOWN      3   HOST
//   MAY      ROUTE 104            1   YOU
//   ...
//   (A) LEAVE GAME  (B) BACK
//
// Tiles 1..0x16C of field BG0 (the start menu window at 0x139 is removed first; the toast,
// which also starts at tile 1, is hidden because the start menu locks controls).

#define PLAYER_LIST_COL_NAME        0
#define PLAYER_LIST_COL_LOCATION    50
#define PLAYER_LIST_COL_BADGES      152
#define PLAYER_LIST_COL_TAG         178
#define PLAYER_LIST_ROW_HEADER      17
#define PLAYER_LIST_ROW_FIRST       33
#define PLAYER_LIST_ROW_HELP        97
#define PLAYER_LIST_REFRESH_FRAMES  30

static EWRAM_DATA u8 sMpPlayerListWindowId = 0;
static EWRAM_DATA bool8 sMpPlayerListShown = FALSE;
static EWRAM_DATA u8 sMpPlayerListTimer = 0;

static u8 MpUi_CountBadges(u8 badges)
{
    u8 count = 0;

    while (badges != 0)
    {
        count += badges & 1;
        badges >>= 1;
    }
    return count;
}

// Region map section name of the player's map, or "---" (not in the field / unknown map).
static void MpUi_BufferLocation(u8 *dest, const struct MpPlayer *player)
{
    u16 mapsec;

    if (player->isLocal)
    {
        mapsec = gMapHeader.regionMapSectionId;
    }
    else if (player->hasPresence && MP_IsValidMap(player->presence.mapGroup, player->presence.mapNum))
    {
        // Network values: only used after MP_IsValidMap (mp.h).
        mapsec = Overworld_GetMapHeaderByGroupAndId(player->presence.mapGroup, player->presence.mapNum)->regionMapSectionId;
    }
    else
    {
        StringCopy(dest, gText_MpPlayersNoLocation);
        return;
    }
    GetMapName(dest, mapsec, 0);
}

static void MpUi_PrintPlayerList(void)
{
    u8 windowId = sMpPlayerListWindowId;
    u8 name[MP_PLAYER_NAME_SIZE];
    u8 text[32];
    u8 slot, row = 0, i;
    const struct MpPlayer *player;

    FillWindowPixelBuffer(windowId, PIXEL_FILL(TEXT_COLOR_WHITE));
    ConvertIntToDecimalStringN(gStringVar1, MP_GetPlayerCount(), STR_CONV_MODE_LEFT_ALIGN, 1);
    ConvertIntToDecimalStringN(gStringVar2, MP_MAX_PLAYERS, STR_CONV_MODE_LEFT_ALIGN, 1);
    StringExpandPlaceholders(gStringVar4, gText_MpPlayersTitle);
    AddTextPrinterParameterized(windowId, FONT_NORMAL, gStringVar4, 0, 1, TEXT_SKIP_DRAW, NULL);
    AddTextPrinterParameterized(windowId, FONT_SMALL, gText_MpPlayersColName, PLAYER_LIST_COL_NAME, PLAYER_LIST_ROW_HEADER, TEXT_SKIP_DRAW, NULL);
    AddTextPrinterParameterized(windowId, FONT_SMALL, gText_MpPlayersColLocation, PLAYER_LIST_COL_LOCATION, PLAYER_LIST_ROW_HEADER, TEXT_SKIP_DRAW, NULL);
    AddTextPrinterParameterized(windowId, FONT_SMALL, gText_MpPlayersColBadges, PLAYER_LIST_COL_BADGES - 8, PLAYER_LIST_ROW_HEADER, TEXT_SKIP_DRAW, NULL);

    for (slot = 0; slot < MP_MAX_PLAYERS; slot++)
    {
        u8 y;

        player = MP_GetPlayer(slot);
        if (player == NULL)
            continue;
        y = PLAYER_LIST_ROW_FIRST + row * 16;
        row++;

        for (i = 0; i < MP_PLAYER_NAME_SIZE - 1 && player->summary.name[i] != EOS; i++)
            name[i] = player->summary.name[i];
        name[i] = EOS;
        AddTextPrinterParameterized(windowId, FONT_NORMAL, name, PLAYER_LIST_COL_NAME, y, TEXT_SKIP_DRAW, NULL);

        MpUi_BufferLocation(text, player);
        AddTextPrinterParameterized(windowId, FONT_NARROW, text, PLAYER_LIST_COL_LOCATION, y, TEXT_SKIP_DRAW, NULL);

        ConvertIntToDecimalStringN(text, MpUi_CountBadges(player->summary.badges), STR_CONV_MODE_LEFT_ALIGN, 1);
        AddTextPrinterParameterized(windowId, FONT_NORMAL, text, PLAYER_LIST_COL_BADGES, y, TEXT_SKIP_DRAW, NULL);

        if (player->isHost)
            AddTextPrinterParameterized(windowId, FONT_SMALL, gText_MpPlayersHost, PLAYER_LIST_COL_TAG, y, TEXT_SKIP_DRAW, NULL);
        else if (player->isLocal)
            AddTextPrinterParameterized(windowId, FONT_SMALL, gText_MpPlayersYou, PLAYER_LIST_COL_TAG, y, TEXT_SKIP_DRAW, NULL);
    }
    AddTextPrinterParameterized(windowId, FONT_NORMAL, gText_MpPlayersHelp, 0, PLAYER_LIST_ROW_HELP, TEXT_SKIP_DRAW, NULL);
    CopyWindowToVram(windowId, COPYWIN_GFX);
}

void MpUi_ShowPlayerList(void)
{
    struct WindowTemplate template = CreateWindowTemplate(0, 1, 1, 26, 14, 15, 0x1);

    if (sMpPlayerListShown)
        return;
    sMpPlayerListWindowId = AddWindow(&template);
    if (sMpPlayerListWindowId == WINDOW_NONE)
        return;
    sMpPlayerListShown = TRUE;
    sMpPlayerListTimer = 0;
    LoadMessageBoxAndBorderGfx();
    PutWindowTilemap(sMpPlayerListWindowId);
    DrawStdWindowFrame(sMpPlayerListWindowId, FALSE);
    MpUi_PrintPlayerList();
    CopyWindowToVram(sMpPlayerListWindowId, COPYWIN_FULL);
}

void MpUi_UpdatePlayerList(void)
{
    // Locations and badges change while the list is open: redraw twice a second.
    if (!sMpPlayerListShown || ++sMpPlayerListTimer < PLAYER_LIST_REFRESH_FRAMES)
        return;
    sMpPlayerListTimer = 0;
    MpUi_PrintPlayerList();
}

void MpUi_HidePlayerList(void)
{
    if (!sMpPlayerListShown)
        return;
    ClearStdWindowAndFrame(sMpPlayerListWindowId, TRUE);
    RemoveWindow(sMpPlayerListWindowId);
    sMpPlayerListShown = FALSE;
}

// ---------------------------------------------------------------------------
// Field notice ("Disconnected from host", CLI auto-start failures)
// ---------------------------------------------------------------------------

#define tState data[0]

static EWRAM_DATA u8 sMpFieldNoticeText[256] = {0};
static EWRAM_DATA bool8 sMpFieldNoticeActive = FALSE;

static void Task_MpFieldNotice(u8 taskId)
{
    s16 *data = gTasks[taskId].data;

    switch (tState)
    {
    case 0:
        // ShowFieldMessage's printer finishes -> the box mode goes back to HIDDEN
        // (the window itself stays up until HideFieldMessageBox).
        if (IsFieldMessageBoxHidden())
            tState++;
        break;
    case 1:
        if (JOY_NEW(A_BUTTON | B_BUTTON))
        {
            PlaySE(SE_SELECT);
            HideFieldMessageBox();
            ScriptUnfreezeObjectEvents();
            UnlockPlayerFieldControls();
            sMpFieldNoticeActive = FALSE;
            DestroyTask(taskId);
        }
        break;
    }
}

#undef tState

bool8 MpUi_TryShowFieldNotice(void)
{
    u8 reason;
    u8 *end;

    if (sMpFieldNoticeActive)
        return FALSE;
    // Free field control only: standing still, no script, no message on screen.
    if (MP_GetState() != MP_STATE_ERROR
     || gPlayerAvatar.tileTransitionState != T_NOT_MOVING
     || ScriptContext_IsEnabled()
     || !IsFieldMessageBoxHidden())
    {
        // No session error to show. An incoming trade/battle request (PS-041) comes next; it
        // blocks like a message box and locks controls itself.
        if (MpInteractUi_TryShowIncoming())
            return TRUE;
        // Then maybe a join/leave toast (non-blocking, PS-033).
        MpUi_TryShowToast();
        return FALSE;
    }

    reason = MP_GetErrorReason();
    DBGPRINTF("[MP] field notice: %s\n", MP_GetReasonName(reason));
    end = StringCopy(sMpFieldNoticeText, MpUi_GetReasonText(reason));
    StringCopy(end, gText_MpNowOffline);
    MP_Leave(); // acknowledge the error: OFFLINE from now on, so it shows only once

    sMpFieldNoticeActive = TRUE;
    FreezeObjectEvents();
    PlayerFreeze();
    StopPlayerAvatar();
    LockPlayerFieldControls();
    ShowFieldMessage(sMpFieldNoticeText);
    CreateTask(Task_MpFieldNotice, 80);
    return TRUE;
}
