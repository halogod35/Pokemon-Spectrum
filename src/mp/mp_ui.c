// Pokemon Spectrum multiplayer: shared UI pieces (SK-UI, PS-022).
//
// - The plain black menu screen setup used by the Join Game and Host Game screens.
// - Game-charset text for MP_REASON_* codes.
// - MpUi_Update(): the single MP_PollEvent consumer (design.md 3.2), called once per frame
//   from sdl2.c right after MP_Update(). PS-033 adds join/leave notices here.
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
#include "palette.h"
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

void MpUi_Update(void)
{
    struct MpEvent event;
    u32 guard;

    // Single consumer. Nothing needs the events yet (state is polled instead); PS-033
    // turns PLAYER_JOINED / PLAYER_LEFT into notices here. Bounded by the queue size.
    for (guard = 0; guard < MP_EVENT_QUEUE_SIZE && MP_PollEvent(&event); guard++)
    {
        switch (event.type)
        {
        case MP_EVENT_SESSION_STARTED:
        case MP_EVENT_CONNECT_FAILED:
        case MP_EVENT_DISCONNECTED:
        case MP_EVENT_PLAYER_JOINED:
        case MP_EVENT_PLAYER_LEFT:
        case MP_EVENT_PLAYER_UPDATED:
        default:
            break;
        }
    }
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

    if (sMpFieldNoticeActive || MP_GetState() != MP_STATE_ERROR)
        return FALSE;
    // Free field control only: standing still, no script, no message on screen.
    if (gPlayerAvatar.tileTransitionState != T_NOT_MOVING
     || ScriptContext_IsEnabled()
     || !IsFieldMessageBoxHidden())
        return FALSE;

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
