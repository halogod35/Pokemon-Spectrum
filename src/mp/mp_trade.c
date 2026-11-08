// Pokemon Spectrum multiplayer: direct trade from the overworld (PS-044, SK-LINK/SAVE/UI).
//
// After PS-041's TRADE request is accepted (mp_menu_interact.c), both players run
// MpTrade_Start(partner). Report PS-040 section 4.2, as the Cable Club does it but without the room:
//   1. Checks (two or more POKEMON, no ENIGMA BERRY, not on a denied map), then a normal save
//      (SaveMapView + TrySavingData(SAVE_NORMAL), as the start menu's SAVE): from here on a link
//      error always reloads this save (PS-048), so a failed trade can never cost anything.
//   2. MP_LinkJoin(both slots), wait for GROUPED.
//   3. SetLocalLinkPlayerId, LINKTYPE_TRADE_SETUP, OpenLinkTimed; the link master advances the
//      handshake once both are on the cable; on EXCHANGE_COMPLETE, SaveLinkPlayers(2).
//   4. Task_StartWiredTrade's steps (cable_club.c): fade out, zero the selected positions, close
//      the link, then CB2_StartCreateTradeMenu. trade.c reopens the link and runs as in the room.
// trade.c ("// MP: PS-044") then uses MP_TradeIsDirect() to
//   - commit the trade's save through the host (mp_trade_commit.c) instead of the standby, and
//   - return to the normal overworld (CB2_MpReturnToFieldAfterTrade) instead of the cable room.
// The trade menu's own rules (the last POKEMON able to battle, eggs, ...) apply unchanged.

#include "global.h"
#include "event_object_lock.h"
#include "event_object_movement.h"
#include "field_message_box.h"
#include "field_player_avatar.h"
#include "field_screen_effect.h"
#include "field_weather.h"
#include "fieldmap.h"
#include "link.h"
#include "m4a.h"
#include "main.h"
#include "overworld.h"
#include "palette.h"
#include "pokemon.h"
#include "save.h"
#include "script.h"
#include "sound.h"
#include "string_util.h"
#include "strings.h"
#include "task.h"
#include "trade.h"
#include "constants/characters.h"
#include "constants/field_weather.h"
#include "constants/items.h"
#include "constants/songs.h"
#include "constants/species.h"
#include "constants/trade.h"
#include "mp/mp.h"
#include "mp/mp_avatar.h"
#include "mp/mp_internal.h"
#include "mp_menu.h"

#define MP_TRADE_GROUP_TIMEOUT_FRAMES   (60 * 10)   // both players must ask for the group within ~10 s
#define MP_TRADE_LINKUP_TIMEOUT_FRAMES  (60 * 15)   // the cable handshake (OpenLinkTimed has its own 600)
#define MP_TRADE_MASTER_DELAY_FRAMES    30          // as --mp-link-test: let both see two players first

enum
{
    TRADE_STATE_SAVE,
    TRADE_STATE_WAIT_GROUP,
    TRADE_STATE_WAIT_PLAYERS,
    TRADE_STATE_WAIT_EXCHANGE,
    TRADE_STATE_FADE,
    TRADE_STATE_WAIT_FADE,
    TRADE_STATE_WAIT_CLOSE,
    TRADE_STATE_FAILED,         // message shown; A/B returns control
};

static bool8 sDirectTrade = FALSE;      // TRUE from CB2_StartCreateTradeMenu until back in the field
static u8 sTradePartner = MP_SLOT_NONE;
static u8 sTradePartnerName[MP_PLAYER_NAME_SIZE];

#define tState  data[0]
#define tTimer  data[1]

static bool8 MpTrade_PartyHasEnigmaBerry(void)
{
    u32 i;

    for (i = 0; i < PARTY_SIZE; i++)
    {
        if (GetMonData(&gPlayerParty[i], MON_DATA_SPECIES) != SPECIES_NONE
         && GetMonData(&gPlayerParty[i], MON_DATA_HELD_ITEM) == ITEM_ENIGMA_BERRY)
            return TRUE;
    }
    return FALSE;
}

const u8 *MpTrade_CheckLocal(void)
{
    // Only from the plain overworld: no battle, no script, not on a denied map, not linking.
    // (PS-041's menu and prompt are only reachable in free field control; this re-checks.)
    if (gMain.callback2 != CB2_Overworld || gMain.inBattle || ScriptContext_IsEnabled()
     || MpAvatar_IsMapDenied((u8)gSaveBlock1Ptr->location.mapGroup, (u8)gSaveBlock1Ptr->location.mapNum)
     || MP_LinkGetState() != MP_LINK_STATE_NONE || sDirectTrade)
        return gText_MpTradeCantHere;
    // The Cable Club's own checks (CableClub_EventScript_CheckPartyTradeRequirements).
    if (CalculatePlayerPartyCount() < 2)
        return gText_MpTradeNeedTwoMons;
    if (MpTrade_PartyHasEnigmaBerry())
        return gText_MpTradeEnigmaBerry;
    return NULL;
}

const u8 *MpTrade_CheckPartner(u8 slot)
{
    const struct MpPlayer *p = MP_GetPlayer(slot);

    if (p == NULL || p->isLocal)
        return gText_MpPlayerGone;
    // PS-041's busy states, as the partner's presence reports them.
    if (p->presence.busy == MP_BUSY_BATTLE || p->presence.busy == MP_BUSY_WARPING || p->presence.busy == MP_BUSY_NOT_IN_FIELD
     || MpAvatar_IsMapDenied(p->presence.mapGroup, p->presence.mapNum))
        return gText_MpTradePartnerBusy;
    return NULL;
}

static void MpTrade_Fail(u8 taskId, const u8 *text)
{
    MP_LOG("trade: cancelled before the trade menu\n");
    CloseLink();                // nothing is open yet in most cases; harmless then
    MP_LinkLeave();
    gTasks[taskId].tState = TRADE_STATE_FAILED;
    StringCopy(gStringVar1, sTradePartnerName);
    if (!IsFieldMessageBoxHidden())
        HideFieldMessageBox();
    ShowFieldMessage(text);
}

static void Task_MpDirectTrade(u8 taskId)
{
    s16 *data = gTasks[taskId].data;
    const u8 *problem;
    u8 status;

    // The other player vanished, or the session ended, before the trade menu took over.
    if (tState >= TRADE_STATE_WAIT_GROUP && tState <= TRADE_STATE_WAIT_EXCHANGE
     && (!MP_IsActive() || MP_GetPlayer(sTradePartner) == NULL))
    {
        MpTrade_Fail(taskId, gText_MpTradeLinkFailed);
        return;
    }

    switch (tState)
    {
    case TRADE_STATE_SAVE:
        problem = MpTrade_CheckLocal();
        if (problem != NULL)
        {
            MpTrade_Fail(taskId, problem);
            break;
        }
        // Save first, as the Cable Club does: a link error from here on reloads this save.
        SaveMapView();
        if (TrySavingData(SAVE_NORMAL) != SAVE_STATUS_OK)
        {
            MpTrade_Fail(taskId, gText_MpTradeSaveFailed);
            break;
        }
        if (!MP_LinkJoin((u8)((1 << MP_GetLocalSlot()) | (1 << sTradePartner))))
        {
            MpTrade_Fail(taskId, gText_MpTradeLinkFailed);
            break;
        }
        MP_LOG("trade: saved; linking up with slot %u\n", (unsigned)sTradePartner);
        StringCopy(gStringVar1, sTradePartnerName);
        if (!IsFieldMessageBoxHidden())
            HideFieldMessageBox();
        ShowFieldMessage(gText_MpTradeLinking);
        tTimer = 0;
        tState = TRADE_STATE_WAIT_GROUP;
        break;
    case TRADE_STATE_WAIT_GROUP:
        if (MP_LinkGetState() == MP_LINK_STATE_NONE || ++tTimer > MP_TRADE_GROUP_TIMEOUT_FRAMES)
        {
            MpTrade_Fail(taskId, gText_MpTradeLinkFailed);
            break;
        }
        if (MP_LinkGetState() != MP_LINK_STATE_GROUPED)
            break;
        SetLocalLinkPlayerId(MP_LinkGetLinkId());
        gLinkType = LINKTYPE_TRADE_SETUP;
        OpenLinkTimed();
        tTimer = 0;
        tState = TRADE_STATE_WAIT_PLAYERS;
        break;
    case TRADE_STATE_WAIT_PLAYERS:
        if (MP_LinkGetState() != MP_LINK_STATE_GROUPED || ++tTimer > MP_TRADE_LINKUP_TIMEOUT_FRAMES)
        {
            MpTrade_Fail(taskId, gText_MpTradeLinkFailed);
            break;
        }
        if (GetLinkPlayerCount_2() < 2)
            break;
        if (IsLinkMaster())
        {
            if (tTimer < MP_TRADE_MASTER_DELAY_FRAMES)
                break;
            CheckShouldAdvanceLinkState();
        }
        tTimer = 0;
        tState = TRADE_STATE_WAIT_EXCHANGE;
        break;
    case TRADE_STATE_WAIT_EXCHANGE:
        if (++tTimer > MP_TRADE_LINKUP_TIMEOUT_FRAMES)
        {
            MpTrade_Fail(taskId, gText_MpTradeLinkFailed);
            break;
        }
        status = GetLinkPlayerDataExchangeStatusTimed(2, 2);
        if (status == EXCHANGE_NOT_STARTED)
            break;
        if (status != EXCHANGE_COMPLETE)
        {
            MP_LOG("trade: player exchange failed (status %u)\n", (unsigned)status);
            MpTrade_Fail(taskId, gText_MpTradeLinkFailed);
            break;
        }
        SaveLinkPlayers(2);
        MP_LOG("trade: linked up as link id %u; entering the trade menu\n", (unsigned)MP_LinkGetLinkId());
        tState = TRADE_STATE_FADE;
        break;
    case TRADE_STATE_FADE:  // Task_StartWiredTrade (cable_club.c), states 0..3
        if (!IsFieldMessageBoxHidden())
            break;
        HideFieldMessageBox();
        FadeScreen(FADE_TO_BLACK, 0);
        ClearLinkCallback_2();
        tState = TRADE_STATE_WAIT_FADE;
        break;
    case TRADE_STATE_WAIT_FADE:
        if (gPaletteFade.active)
            break;
        gSelectedTradeMonPositions[TRADE_PLAYER] = 0;
        gSelectedTradeMonPositions[TRADE_PARTNER] = 0;
        m4aMPlayAllStop();
        SetCloseLinkCallback();
        tState = TRADE_STATE_WAIT_CLOSE;
        break;
    case TRADE_STATE_WAIT_CLOSE:
        if (gReceivedRemoteLinkPlayers)
            break;
        PlayRainStoppingSoundEffect();
        CleanupOverworldWindowsAndTilemaps();
        sDirectTrade = TRUE;
        DestroyTask(taskId);
        SetMainCallback2(CB2_StartCreateTradeMenu);
        break;
    case TRADE_STATE_FAILED:
        if (IsFieldMessageBoxHidden() && JOY_NEW(A_BUTTON | B_BUTTON))
        {
            PlaySE(SE_SELECT);
            HideFieldMessageBox();
            ScriptUnfreezeObjectEvents();
            UnlockPlayerFieldControls();
            DestroyTask(taskId);
        }
        break;
    }
}

#undef tState
#undef tTimer

void MpTrade_Start(u8 partnerSlot, const u8 *partnerName)
{
    u32 i;

    sTradePartner = partnerSlot;
    for (i = 0; i < MP_PLAYER_NAME_SIZE - 1 && partnerName[i] != EOS; i++)
        sTradePartnerName[i] = partnerName[i];
    for (; i < MP_PLAYER_NAME_SIZE; i++)
        sTradePartnerName[i] = EOS;
    // Controls stay locked and the NPCs frozen (PS-041's menu / prompt did that).
    LockPlayerFieldControls();
    CreateTask(Task_MpDirectTrade, 80);
}

bool8 MP_TradeIsDirect(void)
{
    return sDirectTrade;
}

// The trade menu's exit (cancel, or done after the last trade) for a direct trade.
void CB2_MpReturnToFieldAfterTrade(void)
{
    MP_LOG("trade: back to the overworld\n");
    sDirectTrade = FALSE;
    MP_TradeCommitReset();
    MP_LinkLeave();
    StopMapMusic();
    ScriptContext_Init();
    gFieldCallback = FieldCB_ReturnToFieldNoScriptCheckMusic; // unlocks controls, unfreezes NPCs
    CB2_ReturnToField();
}

void MP_TradeOnLinkErrorRecovery(void)
{
    if (sDirectTrade)
        MP_LOG("trade: ended by a link error; the last save is reloaded\n");
    sDirectTrade = FALSE;
    MP_TradeCommitReset();
}
