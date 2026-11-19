// Pokemon Spectrum multiplayer: direct record mixing from the overworld (PS-089, SK-LINK/SAVE/UI).
//
// After PS-041's MIX RECORDS request is accepted (mp_menu_interact.c), both players run
// MpMix_Start(partner). Emerald's Record Corner exchange (record_mixing.c) without the room:
//   1. Checks (plain overworld, the Record Corner's own unlock FLAG_VISITED_MAUVILLE_CITY on both
//      sides, no pending trade journal), then a normal save (SaveMapView + TrySavingData), as
//      CableClub_EventScript_RecordCorner's Common_EventScript_SaveGame does: a link error from
//      here on reloads this save (PS-048), so an interrupted mix leaves nothing half-applied.
//   2. MP_LinkJoin(both slots), wait for GROUPED. The linkup is PS-045's MP_BattleFlowStep.
//   3. SetLocalLinkPlayerId, LINKTYPE_RECORD_MIX_BEFORE (TryRecordMixLinkup), OpenLinkTimed; the
//      link master advances the handshake; on EXCHANGE_COMPLETE, SaveLinkPlayers(2).
//   4. RecordMixing_StartDirect: Task_RecordMixing_Main with the link open (what the Record
//      Corner's seat does after CreateTask_EnterCableClubSeat). Vanilla from there:
//      PrepareExchangePacket, 27 SendBlocks of 200 bytes each way (LINK_BLOCK, PS-052), the
//      partner's record is checked (MP_MixAcceptReceived -> mp_mix_validate.c) and applied
//      (ReceiveExchangePacket), Task_LinkFullSave on both sides (save.c "// MP: PS-089": at the
//      player's position, no Cable Club dynamic warp), then the vanilla close-link handshake.
//   5. "// MP: PS-089" in Task_RecordMixing_Main: no CreateTask_ReestablishCableClubLink and no
//      SetLinkWaitingForScript. MP_MixOnFinished sends LINK_DONE, lets the host end the group,
//      and unlocks the field where the player stands (MpTrade_ResetFieldLinkState, BUG-005).
// The field stays locked from the request to MP_MixOnFinished: PS-041's menu / prompt froze the
// NPCs and locked controls, MpMix_Start keeps the lock, the texts ("Mixing records...",
// "...complete" / "couldn't be mixed") are drawn by record_mixing.c on field window 0 as in the
// Record Corner (our own "Linking up..." message box is hidden first).
// Disconnects: the cable's hardware error -> CB2_LinkError -> PS-048 "Continue from last save"
// (the pre-mix save). MpMix_Update has the one-shot watchdog for a link group that is gone
// while the mix's link is still open, as the battle has.

#include <string.h>
#include "global.h"
#include "event_data.h"
#include "event_object_lock.h"
#include "field_message_box.h"
#include "fieldmap.h"
#include "link.h"
#include "main.h"
#include "overworld.h"
#include "record_mixing.h"
#include "save.h"
#include "script.h"
#include "sound.h"
#include "string_util.h"
#include "strings.h"
#include "task.h"
#include "constants/characters.h"
#include "constants/flags.h"
#include "constants/songs.h"
#include "mp/mp.h"
#include "mp/mp_avatar.h"
#include "mp/mp_battle.h"
#include "mp/mp_internal.h"
#include "mp/mp_link.h"
#include "mp/mp_mix.h"
#include "mp/mp_summary.h"
#include "mp_menu.h"

#define MP_MIX_LEAVE_FALLBACK_FRAMES 300 // after the mix: the host ends the group (DONE) within ~3 s
#define MP_MIX_GONE_FRAMES           180 // the group is gone while the mix's link is open

enum
{
    PHASE_NONE,
    PHASE_STARTING,     // the start task runs (save, linkup)
    PHASE_MIXING,       // Task_RecordMixing_Main runs, until MP_MixOnFinished
};

enum
{
    MIX_STATE_SAVE,
    MIX_STATE_LINKUP,       // MP_BattleFlowStep
    MIX_STATE_START,        // linked: hand over to record_mixing.c
    MIX_STATE_FAILED,       // message shown; A/B returns control
};

static u8 sPhase = PHASE_NONE;
static u8 sPartner = MP_SLOT_NONE;
static u8 sPartnerName[MP_PLAYER_NAME_SIZE];
static struct MpBattleFlow sFlow;
static bool8 sRejected = FALSE;     // the partner's record failed the sanitizer: nothing of it was applied
static bool8 sLeavePending = FALSE;
static u16 sLeaveTimer = 0;
static u16 sGoneFrames = 0;

// ---------------------------------------------------------------------------
// Checks (requester before it asks, target before it says YES, both before saving)
// ---------------------------------------------------------------------------

const u8 *MpMix_CheckLocal(void)
{
    // Only from the plain overworld (as the trade and the battle): no battle, no script, not on
    // a denied map, not linking or in another direct link activity.
    if (gMain.callback2 != CB2_Overworld || gMain.inBattle || ScriptContext_IsEnabled()
     || MpAvatar_IsMapDenied((u8)gSaveBlock1Ptr->location.mapGroup, (u8)gSaveBlock1Ptr->location.mapNum)
     || MP_LinkGetState() != MP_LINK_STATE_NONE || sPhase != PHASE_NONE || MP_TradeIsDirect() || MP_BattleIsDirect())
        return gText_MpMixCantHere;
    if (MP_TradeJournalIsPending())
        return gText_MpTradeJournalBlocks; // PS-050: the mix saves; finish the trade first
    // The Cable Club offers the Record Corner from FLAG_VISITED_MAUVILLE_CITY (cable_club.inc).
    if (!FlagGet(FLAG_VISITED_MAUVILLE_CITY))
        return gText_MpMixNotYet;
    return NULL;
}

const u8 *MpMix_CheckPartner(u8 slot)
{
    const struct MpPlayer *p = MP_GetPlayer(slot);

    if (p == NULL || p->isLocal)
        return gText_MpPlayerGone;
    // PS-041's busy states, as the partner's presence reports them.
    if (p->presence.busy == MP_BUSY_BATTLE || p->presence.busy == MP_BUSY_WARPING || p->presence.busy == MP_BUSY_NOT_IN_FIELD
     || MpAvatar_IsMapDenied(p->presence.mapGroup, p->presence.mapNum))
        return gText_MpMixPartnerBusy;
    // Their Record Corner unlock travels in the player summary (protocol.md 5.1, version 12).
    if (!(p->summary.summaryFlags & MP_SUMMARY_FLAG_CAN_MIX_RECORDS))
        return gText_MpMixPartnerNotYet;
    return NULL;
}

// ---------------------------------------------------------------------------
// The start task
// ---------------------------------------------------------------------------

#define tState  data[0]

static void MpMix_Fail(u8 taskId, const u8 *text)
{
    MP_LOG("mix: cancelled before the exchange\n");
    CloseLink();                // nothing is open yet in most cases; harmless then
    MP_LinkLeave();
    sPhase = PHASE_NONE;
    gTasks[taskId].tState = MIX_STATE_FAILED;
    StringCopy(gStringVar1, sPartnerName);
    if (!IsFieldMessageBoxHidden())
        HideFieldMessageBox();
    ShowFieldMessage(text);
}

static void MpMix_StepLinkup(u8 taskId)
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
        gLinkType = LINKTYPE_RECORD_MIX_BEFORE; // TryRecordMixLinkup; both must ask for a mix
        OpenLinkTimed();
        break;
    case MP_BATTLE_ACT_ADVANCE_LINK:
        CheckShouldAdvanceLinkState();
        break;
    case MP_BATTLE_ACT_LINKED:
        SaveLinkPlayers(2);
        gTasks[taskId].tState = MIX_STATE_START;
        break;
    case MP_BATTLE_ACT_FAIL:
        MP_LOG("mix: linkup failed (link state %u, exchange %u)\n", (unsigned)in.linkState, (unsigned)in.exchangeStatus);
        MpMix_Fail(taskId, gText_MpMixLinkFailed);
        break;
    }
}

static void Task_MpDirectMix(u8 taskId)
{
    s16 *data = gTasks[taskId].data;
    const u8 *problem;

    switch (tState)
    {
    case MIX_STATE_SAVE:
        problem = MpMix_CheckLocal();
        if (problem != NULL)
        {
            MpMix_Fail(taskId, problem);
            break;
        }
        sPhase = PHASE_STARTING;
        // Save first, as the Record Corner script does: a link error from here on reloads this save.
        SaveMapView();
        if (TrySavingData(SAVE_NORMAL) != SAVE_STATUS_OK)
        {
            MpMix_Fail(taskId, gText_MpMixSaveFailed);
            break;
        }
        if (!MP_LinkJoin((u8)((1 << MP_GetLocalSlot()) | (1 << sPartner))))
        {
            MpMix_Fail(taskId, gText_MpMixLinkFailed);
            break;
        }
        MP_LOG("mix: saved; linking up with slot %u\n", (unsigned)sPartner);
        StringCopy(gStringVar1, sPartnerName);
        if (!IsFieldMessageBoxHidden())
            HideFieldMessageBox();
        ShowFieldMessage(gText_MpTradeLinking); // "Linking up with NAME... Don't turn off the power."
        MP_BattleFlowInit(&sFlow);
        tState = MIX_STATE_LINKUP;
        break;
    case MIX_STATE_LINKUP:
        MpMix_StepLinkup(taskId);
        break;
    case MIX_STATE_START:
        if (!IsFieldMessageBoxHidden())
            break; // "Linking up..." still printing
        HideFieldMessageBox(); // record_mixing.c draws its own texts on window 0
        // Task_RecordMixing_Main reads the seat id from gSpecialVar_0x8005 for SetLocalLinkPlayerId.
        gSpecialVar_0x8005 = MP_LinkGetLinkId();
        sRejected = FALSE;
        sGoneFrames = 0;
        sPhase = PHASE_MIXING;
        MP_LOG("mix: linked up as link id %u; mixing records with slot %u\n", (unsigned)MP_LinkGetLinkId(), (unsigned)sPartner);
        DestroyTask(taskId);
        RecordMixing_StartDirect();
        break;
    case MIX_STATE_FAILED:
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

void MpMix_Start(u8 partnerSlot, const u8 *partnerName)
{
    u32 i;

    sPartner = partnerSlot;
    for (i = 0; i < MP_PLAYER_NAME_SIZE - 1 && partnerName[i] != EOS; i++)
        sPartnerName[i] = partnerName[i];
    for (; i < MP_PLAYER_NAME_SIZE; i++)
        sPartnerName[i] = EOS;
    // Controls stay locked and the NPCs frozen (PS-041's menu / prompt did that).
    LockPlayerFieldControls();
    CreateTask(Task_MpDirectMix, 80);
}

// ---------------------------------------------------------------------------
// During the mix (record_mixing.c hooks, "// MP: PS-089")
// ---------------------------------------------------------------------------

bool8 MP_MixIsDirect(void)
{
    return sPhase == PHASE_MIXING;
}

// One secret base for the log: "id 12, owner 'MAY' (tid 1a2b3c4d, gender 1)". `name` is the
// caller's PLAYER_NAME_LENGTH + 1 buffer; the id is 0 for an empty entry.
static void MpMix_LogBase(const char *what, const struct SecretBase *b, char *name)
{
    MP_NameToAscii(b->trainerName, name, PLAYER_NAME_LENGTH + 1);
    MP_LOG("mix: %s: id %u, owner '%s' (tid %02x%02x%02x%02x, gender %u, entered %u times)\n", what,
           (unsigned)b->secretBaseId, name, b->trainerId[0], b->trainerId[1], b->trainerId[2], b->trainerId[3],
           (unsigned)b->gender, (unsigned)b->numTimesEntered);
}

// What the mix carries for secret bases, so a partner's base that does not appear afterwards
// can be traced from the log: ReceiveSecretBasesData skips them all without Secret Power, and
// drops a base whose owner matches us (name, trainer id and gender).
static void MpMix_LogReceivedBases(const u8 *records, u32 recordSize, u32 playerCount, u32 myId)
{
    char name[PLAYER_NAME_LENGTH + 1];
    u32 i, j, others;

    MP_LOG("mix: %u link players, we are link id %u; Secret Power %s\n", (unsigned)playerCount, (unsigned)myId,
           FlagGet(FLAG_RECEIVED_SECRET_POWER) ? "received" : "NOT received (no base can be taken over)");
    MpMix_LogBase("our own base", &gSaveBlock1Ptr->secretBases[0], name);
    for (i = 0; i < playerCount; i++)
    {
        const struct PlayerRecordEmerald *rec = (const struct PlayerRecordEmerald *)(records + i * recordSize);
        char what[48];

        if (i == myId)
            continue;
        others = 0;
        for (j = 1; j < SECRET_BASES_COUNT; j++)
        {
            if (rec->secretBases[j].secretBaseId != 0)
                others++;
        }
        snprintf(what, sizeof(what), "link player %u's own base (version %u, %u other bases)", (unsigned)i,
                 (unsigned)(gLinkPlayers[i].version & 0xFF), (unsigned)others);
        MpMix_LogBase(what, &rec->secretBases[0], name);
    }
}

// After the records were applied and saved: the other players' bases the save now holds.
static void MpMix_LogSavedBases(void)
{
    char name[PLAYER_NAME_LENGTH + 1];
    char what[32];
    u32 i, n = 0;

    for (i = 1; i < SECRET_BASES_COUNT; i++)
    {
        const struct SecretBase *b = &gSaveBlock1Ptr->secretBases[i];

        if (b->secretBaseId == 0)
            continue;
        n++;
        snprintf(what, sizeof(what), "save slot %u (registry %u)", (unsigned)i, (unsigned)b->registryStatus);
        MpMix_LogBase(what, b, name);
    }
    if (n == 0)
        MP_LOG("mix: the save holds no other player's base\n");
}

// Task_ReceivePacket, before ReceiveExchangePacket: every partner record (link ids other than
// ours, `playerCount` of them in `records`, `recordSize` bytes apart) through the sanitizer.
// FALSE drops them all: nothing is applied and the end text says so.
bool8 MP_MixAcceptReceived(const void *records, u32 recordSize, u32 playerCount, u32 myId)
{
    const u8 *base = records;
    u16 detail = 0;
    u32 i;
    u8 code;

    if (records == NULL || recordSize != sizeof(struct PlayerRecordEmerald))
    {
        MP_LOG("mix: REJECTED: record size %u is not %u\n", (unsigned)recordSize, (unsigned)sizeof(struct PlayerRecordEmerald));
        sRejected = TRUE;
        return FALSE;
    }
    if (playerCount > MAX_LINK_PLAYERS)
        playerCount = MAX_LINK_PLAYERS;
    for (i = 0; i < playerCount; i++)
    {
        if (i == myId)
            continue;
        code = MP_MixValidateRecord((const struct PlayerRecordEmerald *)(base + i * recordSize), i == 0, &detail);
        if (code != MP_MIX_OK)
        {
            MP_LOG("mix: REJECTED: link player %u's record: bad %s (index %u); nothing of it is applied\n",
                   (unsigned)i, MP_MixRejectName(code), (unsigned)detail);
            sRejected = TRUE;
            return FALSE;
        }
    }
    MP_LOG("mix: the partner's record passed the checks; applying it\n");
    MpMix_LogReceivedBases(base, recordSize, playerCount, myId);
    return TRUE;
}

// Task_RecordMixing_Main state 3: the text shown for the 60 frames before the field returns.
const u8 *MP_MixCompleteText(void)
{
    return sRejected ? gText_MpMixRejected : gText_RecordMixingComplete;
}

// Task_RecordMixing_Main state 5 (direct mix): the link closed (vanilla handshake in
// Task_DoRecordMixing), the records are saved, the dialogue frame is cleared. Back to the
// plain field where the player stands.
void MP_MixOnFinished(void)
{
    MP_LOG("mix: finished (%s); back to the field\n", sRejected ? "the partner's record was rejected" : "records mixed and saved");
    if (!sRejected)
        MpMix_LogSavedBases();
    sPhase = PHASE_NONE;
    // PS-051: LINK_DONE to the host; it ends the group once both are done (or after its grace
    // time). MpMix_Update only leaves it if that never comes.
    MpLink_MemberSendDone();
    sLeavePending = TRUE;
    sLeaveTimer = 0;
    MpTrade_ResetFieldLinkState(); // BUG-005: no link callback / link players, CB1_Overworld
    ScriptUnfreezeObjectEvents();
    UnlockPlayerFieldControls();
}

void MP_MixOnLinkErrorRecovery(void)
{
    if (sPhase != PHASE_NONE)
        MP_LOG("mix: ended by a link error; the last save (from before the mix) is reloaded\n");
    sPhase = PHASE_NONE;
    sRejected = FALSE;
    sLeavePending = FALSE; // MP_LinkPrepareErrorRecovery leaves the group itself
    sGoneFrames = 0;
}

// Once per frame (MpUi_Update): the deferred link leave after a mix, and the watchdog.
void MpMix_Update(void)
{
    if (sLeavePending)
    {
        if (MP_LinkGetState() == MP_LINK_STATE_NONE)
        {
            MP_LOG("mix: the link group is over (end reason %u)\n", (unsigned)MP_LinkGetEndReason());
            sLeavePending = FALSE;
        }
        else if (++sLeaveTimer > MP_MIX_LEAVE_FALLBACK_FRAMES)
        {
            MP_LOG("mix: the host did not end the link group after the mix; leaving it\n");
            sLeavePending = FALSE;
            MP_LinkLeave();
        }
    }

    if (sPhase != PHASE_MIXING)
        return;

    // Never hang: the link group is gone (partner left, session ended) while the mix's link is
    // still open and the cable's own link error did not come. One shot: the PS-048 recovery
    // reloads the save from before the mix.
    if (gReceivedRemoteLinkPlayers && gMain.callback2 != CB2_LinkError
     && (!MP_IsActive() || MP_LinkGetState() != MP_LINK_STATE_GROUPED))
    {
        if (++sGoneFrames > MP_MIX_GONE_FRAMES)
        {
            MP_LOG("mix: the link group is gone during the mix (link state %u, end reason %u, session %u); recovering from the last save\n",
                   (unsigned)MP_LinkGetState(), (unsigned)MP_LinkGetEndReason(), (unsigned)MP_IsActive());
            sGoneFrames = 0;
            sPhase = PHASE_NONE; // this does not fire again
            CloseLink();
            SetMainCallback2(CB2_LinkError);
        }
    }
    else
    {
        sGoneFrames = 0;
    }
}
