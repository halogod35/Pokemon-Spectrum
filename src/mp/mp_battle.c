// Pokemon Spectrum multiplayer: direct link battle from the overworld (PS-045, SK-LINK/NET).
//
// After PS-041's BATTLE (single) or BATTLE_DOUBLE request is accepted (mp_menu_interact.c),
// both players run MpBattle_Start(partner, kind). Report PS-040 section 5.2: the Cable Club's
// colosseum battle, started the way union_room_battle.c starts one, without the room:
//   1. Checks (plain overworld, the Cable Club's party rules), then a normal save (SaveMapView +
//      TrySavingData), as CableClub_EventScript_TryEnterColosseum does: from here on a link
//      error reloads this save (PS-048), so an interrupted battle has no cost and no result.
//   2. MP_LinkJoin(both slots), wait for GROUPED.
//   3. SetLocalLinkPlayerId, LINKTYPE_SINGLE/DOUBLE_BATTLE (TryBattleLinkup), OpenLinkTimed; the
//      link master advances the handshake; on EXCHANGE_COMPLETE (a single/double mismatch is
//      EXCHANGE_DIFF_SELECTIONS), SaveLinkPlayers(2). Pure part: MP_BattleFlowStep (selftest).
//   4. CableClub_EventScript_EnterColosseum: HealPlayerParty, SavePlayerParty, LoadPlayerBag
//      (Director ruling: vanilla, the party is fully healed for the battle and restored after).
//   5. Task_StartWiredCableClubBattle's steps: fade, LINKTYPE_BATTLE, close the link, the battle
//      flags (SetLinkBattleTypeFlags), TRAINER_LINK_OPPONENT, CB2_InitBattle with
//      savedCallback = CB2_ReturnFromCableClubBattle. battle_controllers.c reopens the link;
//      MpBattle_Update advances that handshake on the master (vanilla's LinkVSync does it on the
//      GBA, the PC cable needs gShouldAdvanceLinkState).
// The battle itself is vanilla and master-authoritative (link id 0 runs the engine).
// CB2_ReturnFromCableClubBattle (vanilla, "// MP: PS-045") restores the party and bag, records
// the result (UpdatePlayerLinkBattleRecords: the trainer card's W/L), saves, and then comes to
// CB2_MpReturnToFieldAfterBattle instead of the cable room.

#include <string.h>
#include "global.h"
#include "battle.h"
#include "battle_main.h"
#include "battle_setup.h"
#include "cable_club.h"
#include "event_data.h"
#include "event_object_lock.h"
#include "field_message_box.h"
#include "field_screen_effect.h"
#include "field_weather.h"
#include "fieldmap.h"
#include "link.h"
#include "load_save.h"
#include "main.h"
#include "overworld.h"
#include "palette.h"
#include "pokemon.h"
#include "save.h"
#include "script.h"
#include "script_pokemon_util.h"
#include "sound.h"
#include "string_util.h"
#include "strings.h"
#include "task.h"
#include "constants/battle.h"
#include "constants/cable_club.h"
#include "constants/field_weather.h"
#include "constants/songs.h"
#include "constants/species.h"
#include "constants/trainers.h"
#include "mp/mp.h"
#include "mp/mp_avatar.h"
#include "mp/mp_battle.h"
#include "mp/mp_internal.h"
#include "mp/mp_link.h"
#include "mp_menu.h"

#define MP_BATTLE_LEAVE_GRACE_FRAMES    180 // after the battle: ~3 s for the partner to close too
#define MP_BATTLE_GONE_FRAMES           180 // the group is gone while the battle's link is open
#define MP_BATTLE_START_DELAY_FRAMES    20  // Task_StartWiredCableClubBattle state 2

enum
{
    PHASE_NONE,
    PHASE_STARTING,     // the start task runs (field)
    PHASE_IN_BATTLE,    // CB2_InitBattle .. CB2_ReturnFromCableClubBattle
    PHASE_RETURNING,    // the post-battle save, until the field
};

enum
{
    BATTLE_STATE_SAVE,
    BATTLE_STATE_LINKUP,        // MP_BattleFlowStep
    BATTLE_STATE_FADE,
    BATTLE_STATE_WAIT_FADE,
    BATTLE_STATE_DELAY,
    BATTLE_STATE_WAIT_CLOSE,
    BATTLE_STATE_FAILED,        // message shown; A/B returns control
};

static u8 sPhase = PHASE_NONE;
static u8 sKind = MP_INTERACT_NONE;
static u8 sPartner = MP_SLOT_NONE;
static u8 sPartnerName[MP_PLAYER_NAME_SIZE];
static struct MpBattleFlow sFlow;
static bool8 sLeavePending = FALSE;
static u16 sLeaveTimer = 0;
static u16 sGoneFrames = 0;
static u16 sHandshakeFrames = 0;

// ---------------------------------------------------------------------------
// Pure logic (selftest: mp_selftest_battle.c)
// ---------------------------------------------------------------------------

u8 MP_BattleCheckParty(const u16 *speciesOrEgg, const u16 *hp, u32 count, bool8 isDouble)
{
    u32 i, mons = 0, usable = 0;

    for (i = 0; i < count && i < PARTY_SIZE; i++)
    {
        if (speciesOrEgg[i] == SPECIES_NONE || speciesOrEgg[i] == SPECIES_EGG)
            continue;
        mons++;
        if (hp[i] != 0)
            usable++;
    }
    if (mons == 0)
        return MP_BATTLE_PARTY_NO_MONS;
    if (isDouble && usable < 2)
        return MP_BATTLE_PARTY_NEED_TWO;
    return MP_BATTLE_PARTY_OK;
}

void MP_BattleFlowInit(struct MpBattleFlow *flow)
{
    flow->state = MP_BATTLE_FLOW_WAIT_GROUP;
    flow->timer = 0;
}

static u8 MpBattleFlow_Fail(struct MpBattleFlow *flow)
{
    flow->state = MP_BATTLE_FLOW_FAILED;
    return MP_BATTLE_ACT_FAIL;
}

u8 MP_BattleFlowStep(struct MpBattleFlow *flow, const struct MpBattleFlowInput *in)
{
    switch (flow->state)
    {
    case MP_BATTLE_FLOW_WAIT_GROUP:
    case MP_BATTLE_FLOW_WAIT_PLAYERS:
    case MP_BATTLE_FLOW_WAIT_EXCHANGE:
        // The other player vanished, or the session ended, before the battle started.
        if (!in->sessionActive || !in->partnerPresent)
            return MpBattleFlow_Fail(flow);
        break;
    default:
        return MP_BATTLE_ACT_NONE; // LINKED / FAILED are final
    }

    switch (flow->state)
    {
    case MP_BATTLE_FLOW_WAIT_GROUP:
        if (in->linkState == MP_LINK_STATE_NONE || ++flow->timer > MP_BATTLE_GROUP_TIMEOUT_FRAMES)
            return MpBattleFlow_Fail(flow);
        if (in->linkState != MP_LINK_STATE_GROUPED)
            return MP_BATTLE_ACT_NONE;
        flow->state = MP_BATTLE_FLOW_WAIT_PLAYERS;
        flow->timer = 0;
        return MP_BATTLE_ACT_OPEN_LINK;
    case MP_BATTLE_FLOW_WAIT_PLAYERS:
        if (in->linkState != MP_LINK_STATE_GROUPED || ++flow->timer > MP_BATTLE_LINKUP_TIMEOUT_FRAMES)
            return MpBattleFlow_Fail(flow);
        if (in->linkPlayerCount < 2)
            return MP_BATTLE_ACT_NONE;
        if (in->isLinkMaster && flow->timer < MP_BATTLE_MASTER_DELAY_FRAMES)
            return MP_BATTLE_ACT_NONE;
        flow->state = MP_BATTLE_FLOW_WAIT_EXCHANGE;
        flow->timer = 0;
        return in->isLinkMaster ? MP_BATTLE_ACT_ADVANCE_LINK : MP_BATTLE_ACT_NONE;
    case MP_BATTLE_FLOW_WAIT_EXCHANGE:
        if (in->linkState != MP_LINK_STATE_GROUPED || ++flow->timer > MP_BATTLE_LINKUP_TIMEOUT_FRAMES)
            return MpBattleFlow_Fail(flow);
        if (in->exchangeStatus == EXCHANGE_NOT_STARTED)
            return MP_BATTLE_ACT_NONE;
        if (in->exchangeStatus != EXCHANGE_COMPLETE)
            return MpBattleFlow_Fail(flow); // incl. DIFF_SELECTIONS: one asked single, one double
        flow->state = MP_BATTLE_FLOW_LINKED;
        return MP_BATTLE_ACT_LINKED;
    }
    return MP_BATTLE_ACT_NONE;
}

bool8 MP_IsBattleKind(u8 kind)
{
    return kind == MP_INTERACT_BATTLE || kind == MP_INTERACT_BATTLE_DOUBLE;
}

u8 MP_BattleServiceForKind(u8 kind)
{
    if (kind == MP_INTERACT_BATTLE)
        return USING_SINGLE_BATTLE;
    if (kind == MP_INTERACT_BATTLE_DOUBLE)
        return USING_DOUBLE_BATTLE;
    return 0;
}

// ---------------------------------------------------------------------------
// Checks (requester before it asks, target before it says YES, both before saving)
// ---------------------------------------------------------------------------

const u8 *MpBattle_CheckLocal(u8 kind)
{
    u16 species[PARTY_SIZE];
    u16 hp[PARTY_SIZE];
    u32 i;

    // Only from the plain overworld (as the trade, PS-044): no battle, no script, not on a
    // denied map, not linking or in another direct link activity.
    if (!MP_IsBattleKind(kind) || gMain.callback2 != CB2_Overworld || gMain.inBattle || ScriptContext_IsEnabled()
     || MpAvatar_IsMapDenied((u8)gSaveBlock1Ptr->location.mapGroup, (u8)gSaveBlock1Ptr->location.mapNum)
     || MP_LinkGetState() != MP_LINK_STATE_NONE || sPhase != PHASE_NONE || MP_TradeIsDirect())
        return gText_MpBattleCantHere;
    if (MP_TradeJournalIsPending())
        return gText_MpTradeJournalBlocks; // PS-050: the battle saves; finish the trade first
    for (i = 0; i < PARTY_SIZE; i++)
    {
        species[i] = GetMonData(&gPlayerParty[i], MON_DATA_SPECIES_OR_EGG);
        hp[i] = GetMonData(&gPlayerParty[i], MON_DATA_HP);
    }
    switch (MP_BattleCheckParty(species, hp, PARTY_SIZE, kind == MP_INTERACT_BATTLE_DOUBLE))
    {
    case MP_BATTLE_PARTY_NO_MONS:
        return gText_MpBattleNoMons;
    case MP_BATTLE_PARTY_NEED_TWO:
        return gText_MpBattleNeedTwoMons;
    }
    return NULL;
}

const u8 *MpBattle_CheckPartner(u8 slot)
{
    const struct MpPlayer *p = MP_GetPlayer(slot);

    if (p == NULL || p->isLocal)
        return gText_MpPlayerGone;
    // PS-041's busy states, as the partner's presence reports them.
    if (p->presence.busy == MP_BUSY_BATTLE || p->presence.busy == MP_BUSY_WARPING || p->presence.busy == MP_BUSY_NOT_IN_FIELD
     || MpAvatar_IsMapDenied(p->presence.mapGroup, p->presence.mapNum))
        return gText_MpBattlePartnerBusy;
    return NULL;
}

// ---------------------------------------------------------------------------
// The start task
// ---------------------------------------------------------------------------

#define tState  data[0]
#define tTimer  data[1]

static void MpBattle_Fail(u8 taskId, const u8 *text)
{
    MP_LOG("battle: cancelled before the battle\n");
    CloseLink();                // nothing is open yet in most cases; harmless then
    MP_LinkLeave();
    sPhase = PHASE_NONE;
    gTasks[taskId].tState = BATTLE_STATE_FAILED;
    StringCopy(gStringVar1, sPartnerName);
    if (!IsFieldMessageBoxHidden())
        HideFieldMessageBox();
    ShowFieldMessage(text);
}

// SetLinkBattleTypeFlags (cable_club.c, static), the 2-player services only.
static void MpBattle_SetBattleTypeFlags(u8 service)
{
    if (service == USING_DOUBLE_BATTLE)
        gBattleTypeFlags = BATTLE_TYPE_DOUBLE | BATTLE_TYPE_LINK | BATTLE_TYPE_TRAINER;
    else
        gBattleTypeFlags = BATTLE_TYPE_LINK | BATTLE_TYPE_TRAINER;
}

static void MpBattle_StepLinkup(u8 taskId)
{
    struct MpBattleFlowInput in;

    in.sessionActive = MP_IsActive();
    in.partnerPresent = MP_GetPlayer(sPartner) != NULL;
    in.linkState = MP_LinkGetState();
    in.linkPlayerCount = GetLinkPlayerCount_2();
    in.isLinkMaster = IsLinkMaster();
    in.exchangeStatus = (sFlow.state == MP_BATTLE_FLOW_WAIT_EXCHANGE) ? GetLinkPlayerDataExchangeStatusTimed(2, 2) : EXCHANGE_NOT_STARTED;

    switch (MP_BattleFlowStep(&sFlow, &in))
    {
    case MP_BATTLE_ACT_OPEN_LINK:
        SetLocalLinkPlayerId(MP_LinkGetLinkId());
        // TryBattleLinkup: both must pick the same mode, or the exchange says DIFF_SELECTIONS.
        gLinkType = (sKind == MP_INTERACT_BATTLE_DOUBLE) ? LINKTYPE_DOUBLE_BATTLE : LINKTYPE_SINGLE_BATTLE;
        OpenLinkTimed();
        break;
    case MP_BATTLE_ACT_ADVANCE_LINK:
        CheckShouldAdvanceLinkState();
        break;
    case MP_BATTLE_ACT_LINKED:
        SaveLinkPlayers(2);
        MP_LOG("battle: linked up as link id %u (%s); entering the battle\n", (unsigned)MP_LinkGetLinkId(),
               (sKind == MP_INTERACT_BATTLE_DOUBLE) ? "double" : "single");
        gTasks[taskId].tState = BATTLE_STATE_FADE;
        break;
    case MP_BATTLE_ACT_FAIL:
        MP_LOG("battle: linkup failed (link state %u, exchange %u)\n", (unsigned)in.linkState, (unsigned)in.exchangeStatus);
        MpBattle_Fail(taskId, gText_MpBattleLinkFailed);
        break;
    }
}

static void Task_MpDirectBattle(u8 taskId)
{
    s16 *data = gTasks[taskId].data;
    const u8 *problem;

    switch (tState)
    {
    case BATTLE_STATE_SAVE:
        problem = MpBattle_CheckLocal(sKind);
        if (problem != NULL)
        {
            MpBattle_Fail(taskId, problem);
            break;
        }
        sPhase = PHASE_STARTING;
        // Save first, as the Cable Club does: a link error from here on reloads this save.
        SaveMapView();
        if (TrySavingData(SAVE_NORMAL) != SAVE_STATUS_OK)
        {
            MpBattle_Fail(taskId, gText_MpBattleSaveFailed);
            break;
        }
        if (!MP_LinkJoin((u8)((1 << MP_GetLocalSlot()) | (1 << sPartner))))
        {
            MpBattle_Fail(taskId, gText_MpBattleLinkFailed);
            break;
        }
        MP_LOG("battle: saved; linking up with slot %u\n", (unsigned)sPartner);
        StringCopy(gStringVar1, sPartnerName);
        if (!IsFieldMessageBoxHidden())
            HideFieldMessageBox();
        ShowFieldMessage(gText_MpTradeLinking); // "Linking up with NAME... Don't turn off the power."
        MP_BattleFlowInit(&sFlow);
        tState = BATTLE_STATE_LINKUP;
        break;
    case BATTLE_STATE_LINKUP:
        MpBattle_StepLinkup(taskId);
        break;
    case BATTLE_STATE_FADE:
        // CableClub_EventScript_EnterColosseum, then Task_StartWiredCableClubBattle state 0.
        if (!IsFieldMessageBoxHidden())
            break;
        HideFieldMessageBox();
        HealPlayerParty();
        SavePlayerParty();
        LoadPlayerBag();
        gSpecialVar_0x8004 = MP_BattleServiceForKind(sKind); // read by CB2_ReturnFromCableClubBattle
        FadeScreen(FADE_TO_BLACK, 0);
        gLinkType = LINKTYPE_BATTLE;
        ClearLinkCallback_2();
        tState = BATTLE_STATE_WAIT_FADE;
        break;
    case BATTLE_STATE_WAIT_FADE:
        if (gPaletteFade.active)
            break;
        tTimer = 0;
        tState = BATTLE_STATE_DELAY;
        break;
    case BATTLE_STATE_DELAY:
        if (++tTimer <= MP_BATTLE_START_DELAY_FRAMES)
            break;
        SetCloseLinkCallback();
        tTimer = 0;
        tState = BATTLE_STATE_WAIT_CLOSE;
        break;
    case BATTLE_STATE_WAIT_CLOSE:
        if (gReceivedRemoteLinkPlayers)
        {
            // SetCloseLinkCallback does nothing while another link callback runs (BUG-005):
            // install it once that one is done.
            if (gLinkCallback == NULL)
                SetCloseLinkCallback();
            // Never hang on a black screen: the party is already healed for the battle, so
            // take the PS-048 recovery (the save from before the linkup is reloaded).
            if (++tTimer > MP_BATTLE_LINKUP_TIMEOUT_FRAMES)
            {
                MP_LOG("battle: the link didn't close before the battle; recovering from the last save\n");
                sPhase = PHASE_NONE;
                DestroyTask(taskId);
                CloseLink();
                SetMainCallback2(CB2_LinkError);
            }
            break;
        }
        if (gLinkPlayers[0].trainerId & 1)
            PlayMapChosenOrBattleBGM(MUS_VS_GYM_LEADER);
        else
            PlayMapChosenOrBattleBGM(MUS_VS_TRAINER);
        MpBattle_SetBattleTypeFlags(gSpecialVar_0x8004);
        CleanupOverworldWindowsAndTilemaps();
        gTrainerBattleOpponent_A = TRAINER_LINK_OPPONENT;
        sPhase = PHASE_IN_BATTLE;
        sGoneFrames = 0;
        sHandshakeFrames = 0;
        DestroyTask(taskId);
        SetMainCallback2(CB2_InitBattle);
        gMain.savedCallback = CB2_ReturnFromCableClubBattle;
        break;
    case BATTLE_STATE_FAILED:
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

void MpBattle_Start(u8 partnerSlot, const u8 *partnerName, u8 kind)
{
    u32 i;

    sPartner = partnerSlot;
    sKind = kind;
    for (i = 0; i < MP_PLAYER_NAME_SIZE - 1 && partnerName[i] != EOS; i++)
        sPartnerName[i] = partnerName[i];
    for (; i < MP_PLAYER_NAME_SIZE; i++)
        sPartnerName[i] = EOS;
    // Controls stay locked and the NPCs frozen (PS-041's menu / prompt did that).
    LockPlayerFieldControls();
    CreateTask(Task_MpDirectBattle, 80);
}

// ---------------------------------------------------------------------------
// During and after the battle
// ---------------------------------------------------------------------------

bool8 MP_BattleIsDirect(void)
{
    return sPhase == PHASE_IN_BATTLE || sPhase == PHASE_RETURNING;
}

// gMain.savedCallback of CB2_ReturnFromCableClubBattle for a direct battle (after the save of
// the link records): back to the normal overworld, where the player stood.
void CB2_MpReturnToFieldAfterBattle(void)
{
    MP_LOG("battle: back to the overworld (outcome %u)\n", (unsigned)gBattleOutcome);
    sPhase = PHASE_NONE;
    // BUG-004/005: leave the link group once both cables are closed (MpBattle_Update).
    sLeavePending = TRUE;
    sLeaveTimer = 0;
    gBattleTypeFlags = 0;
    MpTrade_ResetFieldLinkState(); // BUG-005: CB1_Overworld, no link callback / link players
    StopMapMusic();
    ScriptContext_Init();
    gFieldCallback = FieldCB_ReturnToFieldNoScriptCheckMusic; // unlocks controls, unfreezes NPCs
    CB2_ReturnToField();
}

void MP_BattleOnLinkErrorRecovery(void)
{
    if (sPhase != PHASE_NONE)
        MP_LOG("battle: ended by a link error; the last save is reloaded\n");
    sPhase = PHASE_NONE;
    sLeavePending = FALSE; // MP_LinkPrepareErrorRecovery leaves the group itself
    sGoneFrames = 0;
    // Latent GBA-only bug (like BUG-002/003): CB2_LinkError and ReloadSave re-init the heap
    // without freeing these. The GBA's ReloadSave zeroes EWRAM (RegisterRamReset), so they read
    // NULL there; RegisterRamReset is a stub on PC, so they would dangle into the new heap, and
    // FreeBattleResources / FreeMonSpritesGfx (both "if != NULL then Free") would free garbage
    // after the next battle or trade.
    gBattleStruct = NULL;
    gBattleResources = NULL;
    gLinkBattleSendBuffer = NULL;
    gLinkBattleRecvBuffer = NULL;
    gBattleAnimBgTileBuffer = NULL;
    gBattleAnimBgTilemapBuffer = NULL;
    gBattleSpritesDataPtr = NULL;
    gMonSpritesGfxPtr = NULL;
    gMain.inBattle = FALSE;
}

// Once per frame (MpUi_Update).
void MpBattle_Update(void)
{
    struct MpLinkCableInfo info;

    if (sLeavePending)
    {
        MpLink_CableGetInfo(&info);
        if (MP_LinkGetState() == MP_LINK_STATE_NONE || (!info.open && info.openCount == 0)
         || ++sLeaveTimer > MP_BATTLE_LEAVE_GRACE_FRAMES)
        {
            MP_LOG("battle: leaving the link group (%s)\n", (sLeaveTimer > MP_BATTLE_LEAVE_GRACE_FRAMES) ? "partner still open after the grace time" : "both cables closed");
            sLeavePending = FALSE;
            MP_LinkLeave();
        }
    }

    if (sPhase != PHASE_IN_BATTLE)
        return;

    // The battle reopens the link (HandleLinkBattleSetup). On the GBA the master's LinkVSync
    // starts that handshake by itself; the PC cable starts on gShouldAdvanceLinkState only.
    // CheckShouldAdvanceLinkState does nothing unless we are the master and see two players.
    if (!gReceivedRemoteLinkPlayers && MP_LinkGetState() == MP_LINK_STATE_GROUPED && MP_LinkGetLinkId() == 0)
    {
        if (GetLinkPlayerCount_2() >= 2 && ++sHandshakeFrames > MP_BATTLE_MASTER_DELAY_FRAMES)
            CheckShouldAdvanceLinkState();
    }
    else
    {
        sHandshakeFrames = 0;
    }

    // Never hang: the link group is gone while the battle's link is still open and the link
    // error didn't come (the cable normally reports it at once). One shot.
    if (gReceivedRemoteLinkPlayers && gMain.callback2 != CB2_LinkError
     && (!MP_IsActive() || MP_LinkGetState() != MP_LINK_STATE_GROUPED))
    {
        if (++sGoneFrames > MP_BATTLE_GONE_FRAMES)
        {
            MP_LOG("battle: the link group is gone during the battle; recovering from the last save\n");
            sGoneFrames = 0;
            sPhase = PHASE_RETURNING; // not IN_BATTLE any more: this does not fire again
            CloseLink();
            SetMainCallback2(CB2_LinkError);
        }
    }
    else
    {
        sGoneFrames = 0;
    }
}

// CB2_ReturnFromCableClubBattle (cable_club.c, "// MP: PS-045"): the battle is over; from
// here on only the local post-battle save and the field return are left.
void MP_BattleOnBattleEnd(void)
{
    if (sPhase == PHASE_IN_BATTLE)
        sPhase = PHASE_RETURNING;
}
