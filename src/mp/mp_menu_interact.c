// Pokemon Spectrum multiplayer: the player menu and trade/battle requests (PS-041, SK-UI).
//
// Requester side
//   Facing a remote avatar, A (field_control_avatar.c, "// MP:") calls MpInteractUi_TryOpenMenu.
//   A start-menu-style window opens on the right with the player's name as header and
//       TRAINER CARD / POKEMON / TRADE / BATTLE / EXIT
//   Field controls stay locked while it is open (ProcessPlayerFieldInput returned TRUE); our
//   own NPCs are frozen like for the start menu, the remote player is not affected.
//   - TRAINER CARD (PS-042): a TRAINER_CARD request (no prompt on the other side). On
//     ACCEPTED with a valid card the screen fades out and trainer_card.c's link-partner mode
//     shows it (A flips, B closes); it returns to the field through CB2_ReturnToField with
//     FieldCB_ReturnToFieldNoScript, which unlocks controls and unfreezes the NPCs.
//   - POKEMON: "That isn't available yet." (PS-043 plugs in at MpInteractUi_OpenPokemon),
//     then back to the menu.
//   - TRADE / BATTLE: MP_SendInteractRequest, "Waiting for NAME... (B) CANCEL", then the
//     outcome (accepted / declined / busy / timed out / unavailable). On ACCEPTED, for now
//     "Trading/Battling isn't available yet." (PS-044 / PS-045 plug in at MpInteractUi_OnAccepted).
//   - B / EXIT closes it. If the player leaves or the session ends, it closes with a message.
//
// Target side
//   MP_EVENT_INTERACT_REQUEST (from MpUi_Update) is queued here. The prompt
//       "NAME wants to trade. / Accept?" YES / NO
//   is shown only in free field control (MpInteractUi_TryShowIncoming, called from the PS-022
//   field hook), never over a script, battle or menu. If the player stays busy for
//   MP_INTERACT_UI_BUSY_GRACE_FRAMES the request is answered BUSY automatically. The session
//   answers TIMEOUT after MP_INTERACT_TIMEOUT_MS (MP_EVENT_INTERACT_CANCELLED), and a
//   withdrawn request closes the prompt with a message.

#include "global.h"
#include "event_object_lock.h"
#include "event_object_movement.h"
#include "field_message_box.h"
#include "field_player_avatar.h"
#include "field_screen_effect.h"
#include "field_weather.h"
#include "main.h"
#include "menu.h"
#include "overworld.h"
#include "palette.h"
#include "script.h"
#include "sound.h"
#include "string_util.h"
#include "strings.h"
#include "task.h"
#include "text.h"
#include "window.h"
#include "constants/field_weather.h"
#include "constants/songs.h"
#include "mp/mp.h"
#include "mp/mp_playerdata.h"
#include "mp_menu.h"

#define MP_INTERACT_UI_BUSY_GRACE_FRAMES    180     // ~3 s: still busy -> answer BUSY
#define MP_INTERACT_UI_TEXT_SIZE            160

enum
{
    MENU_ITEM_TRAINER_CARD,
    MENU_ITEM_POKEMON,
    MENU_ITEM_TRADE,
    MENU_ITEM_BATTLE,
    MENU_ITEM_EXIT,
    MENU_ITEM_COUNT,
};

static const u8 *const sMenuTexts[MENU_ITEM_COUNT] =
{
    gText_MpInteractTrainerCard,
    gText_MpInteractPokemon,
    gText_MpInteractTrade,
    gText_MpInteractBattle,
    gText_MenuExit,
};

// Field BG0 tiles 0x29..0xB7: clear of the toast (1..0x28), the map pop-up (0x107), YES/NO
// (0x125), the start menu (0x139) and the dialog box (0x194). Right side like the start menu.
static const struct WindowTemplate sMenuWindowTemplate =
{
    .bg = 0,
    .tilemapLeft = 18,
    .tilemapTop = 1,
    .width = 11,
    .height = 13,
    .paletteNum = 15,
    .baseBlock = 0x29
};

#define MENU_HEADER_Y   1
#define MENU_FIRST_Y    17
#define MENU_LINE       16

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

// Requester (the menu task)
static EWRAM_DATA bool8 sMenuActive = FALSE;
static EWRAM_DATA u8 sMenuSlot = 0;
static EWRAM_DATA u8 sMenuEpoch = 0;
static EWRAM_DATA u8 sMenuName[MP_PLAYER_NAME_SIZE] = {0};
static EWRAM_DATA u8 sMenuWindowId = 0;
static EWRAM_DATA u8 sMenuCursor = 0;
static EWRAM_DATA u8 sMenuKind = 0;           // the request we sent
static EWRAM_DATA u8 sMenuResult = 0;         // MP_INTERACT_RESULT_* once it arrived, else NONE

// Target (the prompt)
static EWRAM_DATA bool8 sIncomingActive = FALSE;
static EWRAM_DATA bool8 sIncomingShown = FALSE;
static EWRAM_DATA u8 sIncomingSlot = 0;
static EWRAM_DATA u8 sIncomingKind = 0;
static EWRAM_DATA u8 sIncomingName[MP_PLAYER_NAME_SIZE] = {0};
static EWRAM_DATA u16 sIncomingAge = 0;
static EWRAM_DATA u8 sIncomingWithdrawn = 0;  // MP_INTERACT_RESULT_CANCELLED / TIMEOUT once withdrawn

static EWRAM_DATA u8 sText[MP_INTERACT_UI_TEXT_SIZE] = {0};

// PS-042: the received card, copied out of the session before the screen fades, so it
// cannot vanish (player leaving, session ending) between the fade and the card screen.
// Never written to the save (AD-10).
static EWRAM_DATA struct MpTrainerCard sViewCard = {0};

static void Task_MpInteractMenu(u8 taskId);
static void Task_MpIncomingPrompt(u8 taskId);

static void CopyName(u8 *dest, const u8 *src)
{
    u32 i;

    for (i = 0; i < MP_PLAYER_NAME_SIZE - 1 && src[i] != EOS; i++)
        dest[i] = src[i];
    for (; i < MP_PLAYER_NAME_SIZE; i++)
        dest[i] = EOS;
}

// ShowFieldMessage with NAME in {STR_VAR_1}. `text` may be sText (it is expanded into gStringVar4).
static void ShowMessageWithName(const u8 *name, const u8 *text)
{
    StringCopy(gStringVar1, name);
    ShowFieldMessage(text);
}

static void FreezeForUi(void)
{
    FreezeObjectEvents();
    PlayerFreeze();
    StopPlayerAvatar();
}

static void UnfreezeAfterUi(void)
{
    ScriptUnfreezeObjectEvents();
    UnlockPlayerFieldControls();
}

// ---------------------------------------------------------------------------
// Plug-in points for later cards
// ---------------------------------------------------------------------------

// PS-043 replaces this. It returns the message to show.
static const u8 *MpInteractUi_OpenPokemon(u8 slot)
{
    return gText_MpNotAvailableYet;
}

// PS-042: copies the data of an ACCEPTED view request out of the session. FALSE if there is
// none (then nothing will be shown).
static bool8 MpInteractUi_TakeViewData(u8 kind, u8 slot)
{
    bool8 ok = FALSE;

    if (kind == MP_INTERACT_TRAINER_CARD)
        ok = MP_GetViewedTrainerCard(slot, &sViewCard);
    MP_ClearViewedData();
    return ok;
}

// PS-042: leaves the field for the view screen. The screen is black, the menu task is gone
// and the overworld windows are cleaned up. The screen returns through CB2_ReturnToField;
// gFieldCallback (FieldCB_ReturnToFieldNoScript) then unlocks controls and unfreezes NPCs.
static void MpInteractUi_OpenView(u8 kind)
{
    gFieldCallback = FieldCB_ReturnToFieldNoScript;
    if (kind == MP_INTERACT_TRAINER_CARD)
        MpPlayerData_ShowTrainerCard(&sViewCard, CB2_ReturnToField);
    else
        SetMainCallback2(CB2_ReturnToField); // nothing to show: straight back (cannot happen)
}

static bool8 IsViewKind(u8 kind)
{
    return kind == MP_INTERACT_TRAINER_CARD;
}

// PS-044 / PS-045: both sides agreed. Returns the message shown after "NAME accepted!" on the
// requester and after YES on the target.
static const u8 *MpInteractUi_OnAccepted(u8 kind, u8 otherSlot)
{
    return (kind == MP_INTERACT_BATTLE) ? gText_MpBattleNotYet : gText_MpTradeNotYet;
}

// ---------------------------------------------------------------------------
// Requester: the player menu
// ---------------------------------------------------------------------------

enum
{
    MENU_STATE_OPEN,
    MENU_STATE_INPUT,
    MENU_STATE_WAITING,         // request out, "Waiting for NAME..."
    MENU_STATE_MESSAGE_CLOSE,   // a message; A/B closes everything
    MENU_STATE_MESSAGE_REOPEN,  // a message; A/B goes back to the menu
    MENU_STATE_FADE_TO_VIEW,    // PS-042: fading out to the trainer card / summary screen
};

#define tState data[0]

static void DrawMenu(void)
{
    u8 i;

    sMenuWindowId = AddWindow(&sMenuWindowTemplate);
    if (sMenuWindowId == WINDOW_NONE)
        return;
    LoadMessageBoxAndBorderGfx();
    PutWindowTilemap(sMenuWindowId);
    DrawStdWindowFrame(sMenuWindowId, FALSE);
    FillWindowPixelBuffer(sMenuWindowId, PIXEL_FILL(TEXT_COLOR_WHITE));
    AddTextPrinterParameterized(sMenuWindowId, FONT_NORMAL, sMenuName, 0, MENU_HEADER_Y, TEXT_SKIP_DRAW, NULL);
    for (i = 0; i < MENU_ITEM_COUNT; i++)
        AddTextPrinterParameterized(sMenuWindowId, FONT_NORMAL, sMenuTexts[i], 8, MENU_FIRST_Y + i * MENU_LINE, TEXT_SKIP_DRAW, NULL);
    sMenuCursor = InitMenuNormal(sMenuWindowId, FONT_NORMAL, 0, MENU_FIRST_Y, MENU_LINE, MENU_ITEM_COUNT, sMenuCursor);
    CopyWindowToVram(sMenuWindowId, COPYWIN_FULL);
}

static void RemoveMenuWindow(void)
{
    if (sMenuWindowId == WINDOW_NONE)
        return;
    ClearStdWindowAndFrame(sMenuWindowId, TRUE);
    RemoveWindow(sMenuWindowId);
    sMenuWindowId = WINDOW_NONE;
}

static void CloseMenu(u8 taskId)
{
    RemoveMenuWindow();
    HideFieldMessageBox(); // the dialog box (window 0) may hold our last message; harmless if not
    UnfreezeAfterUi();
    sMenuActive = FALSE;
    DestroyTask(taskId);
}

static bool8 TargetStillHere(void)
{
    const struct MpPlayer *p;

    if (!MP_IsActive())
        return FALSE;
    p = MP_GetPlayer(sMenuSlot);
    return p != NULL && p->epoch == sMenuEpoch;
}

static void ShowMenuMessage(u8 taskId, const u8 *text, u8 nextState)
{
    gTasks[taskId].tState = nextState;
    ShowMessageWithName(sMenuName, text);
}

static void HandleMenuInput(u8 taskId)
{
    s8 choice;

    if (!TargetStillHere())
    {
        RemoveMenuWindow();
        ShowMenuMessage(taskId, gText_MpPlayerGone, MENU_STATE_MESSAGE_CLOSE);
        return;
    }
    choice = Menu_ProcessInput();
    if (choice == MENU_NOTHING_CHOSEN)
        return;
    if (choice == MENU_B_PRESSED || choice == MENU_ITEM_EXIT)
    {
        if (choice == MENU_B_PRESSED)
            PlaySE(SE_SELECT);
        CloseMenu(taskId);
        return;
    }
    sMenuCursor = (u8)choice;
    RemoveMenuWindow();
    switch (choice)
    {
    case MENU_ITEM_POKEMON:
        ShowMenuMessage(taskId, MpInteractUi_OpenPokemon(sMenuSlot), MENU_STATE_MESSAGE_REOPEN);
        break;
    case MENU_ITEM_TRAINER_CARD:    // PS-042: no prompt on their side, the card comes back at once
    case MENU_ITEM_TRADE:
    case MENU_ITEM_BATTLE:
        if (choice == MENU_ITEM_TRAINER_CARD)
            sMenuKind = MP_INTERACT_TRAINER_CARD;
        else
            sMenuKind = (choice == MENU_ITEM_TRADE) ? MP_INTERACT_TRADE : MP_INTERACT_BATTLE;
        sMenuResult = MP_INTERACT_RESULT_NONE;
        if (!MP_SendInteractRequest(sMenuSlot, sMenuKind))
            ShowMenuMessage(taskId, gText_MpRequestFailed, MENU_STATE_MESSAGE_CLOSE);
        else
            ShowMenuMessage(taskId, gText_MpWaitingFor, MENU_STATE_WAITING);
        break;
    }
}

static void ShowResult(u8 taskId)
{
    u8 *end;

    switch (sMenuResult)
    {
    case MP_INTERACT_RESULT_ACCEPTED:
        if (IsViewKind(sMenuKind))
        {
            // PS-042: show their data. Copy it out now; then fade out to the screen.
            if (!MpInteractUi_TakeViewData(sMenuKind, sMenuSlot))
            {
                ShowMenuMessage(taskId, gText_MpViewBadData, MENU_STATE_MESSAGE_CLOSE);
                break;
            }
            HideFieldMessageBox();
            FadeScreen(FADE_TO_BLACK, 0);
            gTasks[taskId].tState = MENU_STATE_FADE_TO_VIEW;
            break;
        }
        end = StringCopy(sText, gText_MpResultAccepted);
        StringCopy(end, MpInteractUi_OnAccepted(sMenuKind, sMenuSlot));
        ShowMenuMessage(taskId, sText, MENU_STATE_MESSAGE_CLOSE);
        break;
    case MP_INTERACT_RESULT_DECLINED:
        ShowMenuMessage(taskId, gText_MpResultDeclined, MENU_STATE_MESSAGE_CLOSE);
        break;
    case MP_INTERACT_RESULT_BUSY:
        ShowMenuMessage(taskId, gText_MpResultBusy, MENU_STATE_MESSAGE_CLOSE);
        break;
    case MP_INTERACT_RESULT_TIMEOUT:
        ShowMenuMessage(taskId, gText_MpResultTimeout, MENU_STATE_MESSAGE_CLOSE);
        break;
    case MP_INTERACT_RESULT_BAD_DATA:   // PS-042: their data failed validation
        ShowMenuMessage(taskId, gText_MpViewBadData, MENU_STATE_MESSAGE_CLOSE);
        break;
    default:
        ShowMenuMessage(taskId, gText_MpResultUnavailable, MENU_STATE_MESSAGE_CLOSE);
        break;
    }
}

static void Task_MpInteractMenu(u8 taskId)
{
    s16 *data = gTasks[taskId].data;

    switch (tState)
    {
    case MENU_STATE_OPEN:
        DrawMenu();
        if (sMenuWindowId == WINDOW_NONE)
        {
            CloseMenu(taskId);
            return;
        }
        tState = MENU_STATE_INPUT;
        break;
    case MENU_STATE_INPUT:
        HandleMenuInput(taskId);
        break;
    case MENU_STATE_WAITING:
        if (!IsFieldMessageBoxHidden())
            break; // "Waiting for..." still printing
        if (sMenuResult != MP_INTERACT_RESULT_NONE)
        {
            ShowResult(taskId);
        }
        else if (!MP_IsActive() || !MP_IsInteractRequestPending())
        {
            // The session ended (or the request vanished without a result).
            MP_CancelInteractRequest();
            ShowMenuMessage(taskId, gText_MpResultUnavailable, MENU_STATE_MESSAGE_CLOSE);
        }
        else if (JOY_NEW(B_BUTTON))
        {
            PlaySE(SE_SELECT);
            MP_CancelInteractRequest();
            ShowMenuMessage(taskId, gText_MpRequestCancelled, MENU_STATE_MESSAGE_CLOSE);
        }
        break;
    case MENU_STATE_MESSAGE_CLOSE:
    case MENU_STATE_MESSAGE_REOPEN:
        if (!IsFieldMessageBoxHidden())
            break;
        if (JOY_NEW(A_BUTTON | B_BUTTON))
        {
            PlaySE(SE_SELECT);
            if (tState == MENU_STATE_MESSAGE_REOPEN && TargetStillHere())
            {
                HideFieldMessageBox();
                tState = MENU_STATE_OPEN;
            }
            else
            {
                CloseMenu(taskId);
            }
        }
        break;
    case MENU_STATE_FADE_TO_VIEW:   // PS-042
        if (gPaletteFade.active)
            break;
        {
            u8 kind = sMenuKind;

            // Like the start menu does before opening a screen. Controls stay locked and the
            // NPCs frozen until the field callback set by MpInteractUi_OpenView runs.
            PlayRainStoppingSoundEffect();
            CleanupOverworldWindowsAndTilemaps();
            sMenuWindowId = WINDOW_NONE;
            sMenuActive = FALSE;
            DestroyTask(taskId);
            MpInteractUi_OpenView(kind);
        }
        break;
    }
}

bool8 MpInteractUi_TryOpenMenu(s16 x, s16 y)
{
    const struct MpPlayer *player;
    u8 slot;

    if (!MP_IsActive() || sMenuActive || sIncomingShown || FuncIsActiveTask(Task_MpInteractMenu))
        return FALSE;
    slot = MP_GetRemoteAvatarSlotAt(x, y);
    if (slot == MP_SLOT_NONE)
        return FALSE;
    player = MP_GetPlayer(slot);
    if (player == NULL || player->isLocal)
        return FALSE;

    PlaySE(SE_SELECT);
    sMenuActive = TRUE;
    sMenuSlot = slot;
    sMenuEpoch = player->epoch;
    CopyName(sMenuName, player->summary.name);
    sMenuWindowId = WINDOW_NONE;
    sMenuCursor = 0;
    sMenuKind = MP_INTERACT_NONE;
    sMenuResult = MP_INTERACT_RESULT_NONE;
    FreezeForUi();
    CreateTask(Task_MpInteractMenu, 80);
    // The caller (ProcessPlayerFieldInput returning TRUE) locks field controls.
    return TRUE;
}

#undef tState

// ---------------------------------------------------------------------------
// Target: the incoming prompt
// ---------------------------------------------------------------------------

enum
{
    PROMPT_STATE_PRINTING,
    PROMPT_STATE_YES_NO,
    PROMPT_STATE_MESSAGE,       // final message; A/B closes
};

#define tState data[0]

static void ClosePrompt(u8 taskId)
{
    HideFieldMessageBox();
    UnfreezeAfterUi();
    sIncomingActive = FALSE;
    sIncomingShown = FALSE;
    sIncomingWithdrawn = 0;
    DestroyTask(taskId);
}

static void Task_MpIncomingPrompt(u8 taskId)
{
    s16 *data = gTasks[taskId].data;
    s8 choice;

    // Withdrawn (requester cancelled / left, timed out) or the session ended.
    if (tState != PROMPT_STATE_MESSAGE && (sIncomingWithdrawn != 0 || !MP_IsActive()))
    {
        if (!IsFieldMessageBoxHidden())
            return; // let the question finish printing first
        if (tState == PROMPT_STATE_YES_NO)
            EraseYesNoWindow();
        tState = PROMPT_STATE_MESSAGE;
        ShowMessageWithName(sIncomingName, (sIncomingWithdrawn == MP_INTERACT_RESULT_TIMEOUT) ? gText_MpIncomingTimedOut : gText_MpIncomingWithdrawn);
        return;
    }

    switch (tState)
    {
    case PROMPT_STATE_PRINTING:
        if (IsFieldMessageBoxHidden())
        {
            DisplayYesNoMenuDefaultYes();
            tState = PROMPT_STATE_YES_NO;
        }
        break;
    case PROMPT_STATE_YES_NO:
        choice = Menu_ProcessInputNoWrapClearOnChoose();
        if (choice == 0)
        {
            MP_RespondInteract(MP_INTERACT_RESULT_ACCEPTED);
            tState = PROMPT_STATE_MESSAGE;
            ShowMessageWithName(sIncomingName, MpInteractUi_OnAccepted(sIncomingKind, sIncomingSlot));
        }
        else if (choice == 1 || choice == MENU_B_PRESSED)
        {
            MP_RespondInteract(MP_INTERACT_RESULT_DECLINED);
            ClosePrompt(taskId);
        }
        break;
    case PROMPT_STATE_MESSAGE:
        if (IsFieldMessageBoxHidden() && JOY_NEW(A_BUTTON | B_BUTTON))
        {
            PlaySE(SE_SELECT);
            ClosePrompt(taskId);
        }
        break;
    }
}

#undef tState

bool8 MpInteractUi_TryShowIncoming(void)
{
    if (!sIncomingActive || sIncomingShown || sMenuActive)
        return FALSE;
    if (!MP_HasIncomingInteract())
    {
        sIncomingActive = FALSE; // gone in the meantime
        return FALSE;
    }
    // Free field control (the caller checked unlocked controls): no script, no message box,
    // not in the middle of a step.
    if (ScriptContext_IsEnabled() || !IsFieldMessageBoxHidden())
        return FALSE;
    if (gPlayerAvatar.tileTransitionState != T_NOT_MOVING && gPlayerAvatar.tileTransitionState != T_TILE_CENTER)
        return FALSE;

    sIncomingShown = TRUE;
    sIncomingWithdrawn = 0;
    FreezeForUi();
    LockPlayerFieldControls();
    ShowMessageWithName(sIncomingName, (sIncomingKind == MP_INTERACT_BATTLE) ? gText_MpIncomingBattle : gText_MpIncomingTrade);
    CreateTask(Task_MpIncomingPrompt, 80);
    return TRUE;
}

// ---------------------------------------------------------------------------
// Events and per-frame bookkeeping (from MpUi_Update)
// ---------------------------------------------------------------------------

void MpInteractUi_OnEvent(const struct MpEvent *ev)
{
    switch (ev->type)
    {
    case MP_EVENT_INTERACT_REQUEST:
        if (sIncomingShown)
            break; // cannot happen (one request per target at the host); the old prompt wins
        sIncomingActive = TRUE;
        sIncomingShown = FALSE;
        sIncomingSlot = ev->slot;
        sIncomingKind = ev->kind;
        sIncomingAge = 0;
        sIncomingWithdrawn = 0;
        CopyName(sIncomingName, ev->name);
        break;
    case MP_EVENT_INTERACT_CANCELLED:
        if (!sIncomingActive || ev->slot != sIncomingSlot)
            break;
        if (sIncomingShown)
            sIncomingWithdrawn = (ev->result == MP_INTERACT_RESULT_TIMEOUT) ? MP_INTERACT_RESULT_TIMEOUT : MP_INTERACT_RESULT_CANCELLED;
        else
            sIncomingActive = FALSE; // never shown: just forget it
        break;
    case MP_EVENT_INTERACT_RESULT:
        if (sMenuActive && ev->slot == sMenuSlot && ev->kind == sMenuKind)
            sMenuResult = ev->result;
        break;
    }
}

void MpInteractUi_Update(void)
{
    if (!MP_IsActive())
    {
        if (!sIncomingShown)
            sIncomingActive = FALSE;
        return;
    }
    // Busy for too long (battle, menu, script, another screen): answer BUSY.
    if (sIncomingActive && !sIncomingShown && ++sIncomingAge >= MP_INTERACT_UI_BUSY_GRACE_FRAMES)
    {
        DBGPRINTF("[MP] interact: still busy, answering BUSY\n");
        MP_RespondInteract(MP_INTERACT_RESULT_BUSY);
        sIncomingActive = FALSE;
    }
}
