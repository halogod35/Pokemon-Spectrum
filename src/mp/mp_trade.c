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

#include <stdio.h>
#include <string.h>
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
#include "platform.h"
#include "pokedex.h"
#include "pokemon.h"
#include "pokemon_storage_system.h"
#include "save.h"
#include "script.h"
#include "sound.h"
#include "string_util.h"
#include "strings.h"
#include "task.h"
#include "trade.h"
#include "constants/characters.h"
#include "constants/field_weather.h"
#include "constants/game_stat.h"
#include "constants/items.h"
#include "constants/songs.h"
#include "constants/species.h"
#include "constants/trade.h"
#include "mp/mp.h"
#include "mp/mp_avatar.h"
#include "mp/mp_internal.h"
#include "mp/mp_link.h"
#include "mp/mp_protocol.h"
#include "mp/mp_trade_journal.h"
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
static bool8 sLeavePending = FALSE;     // BUG-004: leave the link group once the partner closed too
static u16 sLeaveTimer = 0;
static u16 sGroupGoneFrames = 0;       // BUG-004: watchdog, the group is gone while still trading

#define MP_TRADE_LEAVE_GRACE_FRAMES     180 // ~3 s for the partner to finish its close handshake
#define MP_TRADE_GONE_FRAMES            180 // ~3 s for the trade's own link-error path
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
    if (MP_TradeJournalIsPending())
        return gText_MpTradeJournalBlocks; // PS-050: finish the unfinished trade first
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
    // BUG-004: not yet. The partner may still be in its close-link handshake; leaving now
    // would end the group under it. MpTrade_Update leaves once both cables are closed.
    sLeavePending = TRUE;
    sLeaveTimer = 0;
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
    sLeavePending = FALSE; // MP_LinkPrepareErrorRecovery leaves the group itself
    sGroupGoneFrames = 0;
    MP_TradeCommitReset();
}

// BUG-004. Once per frame (MpUi_Update):
//  - the deferred MP_LinkLeave after a direct trade: when both cables are closed (the partner
//    finished its close handshake too), or after MP_TRADE_LEAVE_GRACE_FRAMES;
//  - never hang: if the link group is gone while a direct trade is still on screen (and no
//    commit is waiting for the host), give the trade's own error path a moment, then take the
//    PS-048 recovery (CB2_LinkError -> "Continue from last save").
void MpTrade_Update(void)
{
    struct MpLinkCableInfo info;

    if (sLeavePending)
    {
        MpLink_CableGetInfo(&info);
        if (MP_LinkGetState() == MP_LINK_STATE_NONE || (!info.open && info.openCount == 0)
         || ++sLeaveTimer > MP_TRADE_LEAVE_GRACE_FRAMES)
        {
            sLeavePending = FALSE;
            MP_LinkLeave();
        }
    }

    if (sDirectTrade && gMain.callback2 != CB2_LinkError
     && (!MP_IsActive() || MP_LinkGetState() != MP_LINK_STATE_GROUPED)
     && MP_TradeCommitPoll() != MP_TRADE_COMMIT_WAITING)
    {
        if (++sGroupGoneFrames > MP_TRADE_GONE_FRAMES)
        {
            MP_LOG("trade: the link group is gone while trading; recovering from the last save\n");
            sGroupGoneFrames = 0;
            CloseLink();
            SetMainCallback2(CB2_LinkError);
        }
    }
    else
    {
        sGroupGoneFrames = 0;
    }
}
// ===========================================================================
// Trade journal, game side (PS-050, design.md "Trade journal", protocol.md 6.16)
// ===========================================================================
//
// sJournal mirrors "<save>.trade". Life of a direct trade:
//   TradeMons      MP_TradeJournalStage: the two mons, our slot, the partner
//   state 41       MP_TradeJournalWriteReady: file READY (durable), then READY to the host
//   COMMIT         MP_TradeJournalMarkCommit: file COMMIT (durable), then the save is signed
//                  and stored (MP_TradeJournalOnSigned: "applied")
//   final ABORT    MP_TradeJournalOnAbort: file deleted (nobody can have signed)
//   IN_DOUBT       file kept READY; recovery decides
// Recovery (MpTrade_JournalUpdate, every frame): while a journal is pending and we are in a
// session, send our status to the partner (or only to the host if the partner is not here)
// every MP_TJ_SEND_INTERVAL frames; answer the partner's status; apply MpTradeJournal_Resolve:
// COMMIT -> file COMMIT, then the trade is applied to the save in the field (roll forward);
// ABORT -> delete; DONE -> delete (after it is applied). A save is never rolled back: a
// signed save always means the outcome is COMMIT.

#define MP_TJ_SEND_INTERVAL 120     // frames (~2 s)
#define MP_TJ_PATH_SIZE     (PLATFORM_ARG_PATH_SIZE + 8)

enum
{
    TJ_NOTICE_NONE,
    TJ_NOTICE_PENDING,      // after a load with a journal
    TJ_NOTICE_COMPLETED,    // rolled forward
    TJ_NOTICE_CANCELLED,    // resolved as "neither traded"
    TJ_NOTICE_DAMAGED,      // unusable journal file, or the mons can't be found
};

static struct MpTradeJournal sJournal;      // state NONE: no usable journal
static bool8 sJournalDamaged;               // a file exists but cannot be used (kept, never dropped)
static bool8 sJournalApplied;               // COMMIT and the save has the trade
static bool8 sJournalDonePending;           // DONE arrived before the trade was applied
static u8 sJournalNotice;
static u16 sJournalSendTimer;
static struct MpTradeJournal sStage;
static bool8 sStaged;
static u8 sNoticeName[MP_PLAYER_NAME_SIZE];   // the partner of the journal, kept for the notices

static bool8 MpTj_Path(char *out)
{
    int len = snprintf(out, MP_TJ_PATH_SIZE, "%s.trade", gPlatformArgs.savePath);

    return len > 0 && len < MP_TJ_PATH_SIZE;
}

static bool8 MpTj_WriteFile(const struct MpTradeJournal *j)
{
    char path[MP_TJ_PATH_SIZE];
    u8 buf[MP_TJ_FILE_SIZE];

    if (!MpTj_Path(path))
        return FALSE;
    MpTradeJournal_Serialize(j, buf);
    if (!Platform_WriteFileAtomic(path, buf, sizeof(buf)))
    {
        MP_LOG("trade journal: could not write '%s'\n", path);
        return FALSE;
    }
    return TRUE;
}

static void MpTj_Delete(void)
{
    char path[MP_TJ_PATH_SIZE];

    if (MpTj_Path(path) && !Platform_RemoveFile(path))
    {
        MP_LOG("trade journal: could not delete '%s'; it stays pending\n", path);
        return;
    }
    MP_LOG("trade journal: trade %08X resolved, journal deleted\n", (unsigned)sJournal.tradeId);
    memcpy(sNoticeName, sJournal.partnerName, sizeof(sNoticeName));
    memset(&sJournal, 0, sizeof(sJournal));
    sJournalApplied = FALSE;
    sJournalDonePending = FALSE;
}

static u32 MpTj_MyTid(void)
{
    return T1_READ_32(gSaveBlock2Ptr->playerTrainerId);
}

static void MpTj_IdOf(const struct Pokemon *mon, struct MpTradeMonId *id)
{
    id->personality = GetMonData(mon, MON_DATA_PERSONALITY);
    id->otId = GetMonData(mon, MON_DATA_OT_ID);
    id->species = GetMonData(mon, MON_DATA_SPECIES);
}

// Where the mon (personality, OT id) is: party index 0..5, 100 + box * IN_BOX_COUNT + pos, or -1.
static s32 MpTj_Find(u32 personality, u32 otId)
{
    u32 i, b, p;

    for (i = 0; i < PARTY_SIZE; i++)
    {
        if (GetMonData(&gPlayerParty[i], MON_DATA_SPECIES) != SPECIES_NONE
         && GetMonData(&gPlayerParty[i], MON_DATA_PERSONALITY) == personality
         && GetMonData(&gPlayerParty[i], MON_DATA_OT_ID) == otId)
            return (s32)i;
    }
    for (b = 0; b < TOTAL_BOXES_COUNT; b++)
    {
        for (p = 0; p < IN_BOX_COUNT; p++)
        {
            if (GetBoxMonDataAt(b, p, MON_DATA_SPECIES) != SPECIES_NONE
             && GetBoxMonDataAt(b, p, MON_DATA_PERSONALITY) == personality
             && GetBoxMonDataAt(b, p, MON_DATA_OT_ID) == otId)
                return (s32)(100 + b * IN_BOX_COUNT + p);
        }
    }
    return -1;
}

bool8 MP_TradeJournalIsPending(void)
{
    return sJournal.state != MP_TJ_STATE_NONE || sJournalDamaged;
}

void MP_TradeJournalOnLoad(void)
{
    char path[MP_TJ_PATH_SIZE];
    u8 buf[MP_TJ_FILE_SIZE + 1];
    s32 len;
    s32 given, received;

    memset(&sJournal, 0, sizeof(sJournal));
    sJournalDamaged = FALSE;
    sJournalApplied = FALSE;
    sJournalDonePending = FALSE;
    sJournalNotice = TJ_NOTICE_NONE;
    sJournalSendTimer = 0;
    sStaged = FALSE;
    if (!MpTj_Path(path))
        return;
    len = Platform_ReadSmallFile(path, buf, MP_TJ_FILE_SIZE);
    if (len < 0)
        return; // no journal: the normal case
    // Untrusted input (AD-9): structure, binding to this save, and the stored mon.
    if (MpTradeJournal_Parse(buf, (u32)len, &sJournal) != MP_TJ_PARSE_OK
     || sJournal.myTid != MpTj_MyTid() || !MP_ValidateMon(sJournal.receivedMon))
    {
        MP_LOG("trade journal: '%s' is damaged or not for this save; it is kept and shown\n", path);
        memset(&sJournal, 0, sizeof(sJournal));
        sJournalDamaged = TRUE;
        sJournalNotice = TJ_NOTICE_DAMAGED;
        return;
    }
    given = MpTj_Find(sJournal.given.personality, sJournal.given.otId);
    received = MpTj_Find(sJournal.received.personality, sJournal.received.otId);
    if (sJournal.state == MP_TJ_STATE_COMMIT)
    {
        if (received >= 0 && given < 0)
            sJournalApplied = TRUE;         // signed before the crash
        else if (!(given >= 0 && received < 0))
            sJournalDamaged = TRUE;         // neither or both: can't tell where the trade stands
    }
    else if (given < 0)
    {
        sJournalDamaged = TRUE;             // READY, but the mon we gave is not here any more
    }
    MP_LOG("trade journal: pending trade %08X with trainer %08X (%s%s)\n", (unsigned)sJournal.tradeId,
           (unsigned)sJournal.partnerTid, (sJournal.state == MP_TJ_STATE_COMMIT) ? "COMMIT" : "READY",
           sJournalDamaged ? ", mons not found" : (sJournalApplied ? ", applied" : ""));
    sJournalNotice = sJournalDamaged ? TJ_NOTICE_DAMAGED : TJ_NOTICE_PENDING;
}

void MP_TradeJournalStage(const struct Pokemon *given, const struct Pokemon *received, u8 partyIndex, u32 partnerTid, const u8 *partnerName)
{
    u32 i;

    memset(&sStage, 0, sizeof(sStage));
    MpTj_IdOf(given, &sStage.given);
    MpTj_IdOf(received, &sStage.received);
    sStage.partyIndex = (partyIndex < PARTY_SIZE) ? partyIndex : 0;
    sStage.partnerTid = partnerTid;
    for (i = 0; i < MP_PLAYER_NAME_SIZE - 1 && partnerName[i] != EOS; i++)
        sStage.partnerName[i] = partnerName[i];
    for (; i < MP_PLAYER_NAME_SIZE; i++)
        sStage.partnerName[i] = EOS;
    sStaged = (partyIndex < PARTY_SIZE);
}

bool8 MP_TradeJournalWriteReady(u32 *tradeIdOut)
{
    struct MpTradeMonId now;
    const struct Pokemon *mon;

    *tradeIdOut = 0;
    if (!sStaged || MP_TradeJournalIsPending())
    {
        MP_LOG("trade journal: not ready (%s)\n", sStaged ? "a journal is still pending" : "nothing staged");
        return FALSE;
    }
    mon = &gPlayerParty[sStage.partyIndex];
    MpTj_IdOf(mon, &now);
    if (now.personality != sStage.received.personality || now.otId != sStage.received.otId)
        return FALSE;
    sStage.state = MP_TJ_STATE_READY;
    sStage.myTid = MpTj_MyTid();
    sStage.receivedFinalSpecies = now.species;
    memcpy(sStage.receivedMon, mon, MP_MON_WIRE_SIZE);
    sStage.tradeId = MpTradeJournal_ComputeId(sStage.myTid, &sStage.given, sStage.partnerTid, &sStage.received);
    if (!MpTj_WriteFile(&sStage))
        return FALSE;
    sJournal = sStage;
    sJournalApplied = FALSE;
    sJournalDonePending = FALSE;
    sStaged = FALSE;
    *tradeIdOut = sJournal.tradeId;
    MP_LOG("trade journal: trade %08X READY\n", (unsigned)sJournal.tradeId);
    return TRUE;
}

bool8 MP_TradeJournalMarkCommit(void)
{
    struct MpTradeJournal j = sJournal;

    if (j.state == MP_TJ_STATE_NONE)
        return FALSE;
    j.state = MP_TJ_STATE_COMMIT;
    if (!MpTj_WriteFile(&j))
        return FALSE;
    sJournal = j;
    MP_LOG("trade journal: trade %08X COMMIT\n", (unsigned)sJournal.tradeId);
    return TRUE;
}

void MP_TradeJournalOnSigned(void)
{
    sJournalApplied = TRUE;
    sJournalSendTimer = 0; // tell the partner at once
}

void MP_TradeJournalOnAbort(void)
{
    sStaged = FALSE;
    if (sJournal.state == MP_TJ_STATE_READY)
        MpTj_Delete();
}

// Roll forward: the outcome is COMMIT and this save does not have the trade yet.
static bool8 MpTj_Apply(void)
{
    struct Pokemon mon;
    s32 at = MpTj_Find(sJournal.given.personality, sJournal.given.otId);
    u16 species[2];
    u32 i;

    if (at < 0 || MpTj_Find(sJournal.received.personality, sJournal.received.otId) >= 0)
        return FALSE;
    memcpy(&mon, sJournal.receivedMon, sizeof(mon));
    if (at < PARTY_SIZE)
        gPlayerParty[at] = mon;
    else
        SetBoxMonAt((u8)((at - 100) / IN_BOX_COUNT), (u8)((at - 100) % IN_BOX_COUNT), &mon.box);
    if (!GetMonData(&mon, MON_DATA_IS_EGG))
    {
        species[0] = sJournal.received.species;
        species[1] = sJournal.receivedFinalSpecies;
        for (i = 0; i < 2; i++)
        {
            u16 dex = SpeciesToNationalPokedexNum(species[i]);

            GetSetPokedexFlag(dex, FLAG_SET_SEEN);
            HandleSetPokedexFlag(dex, FLAG_SET_CAUGHT, sJournal.received.personality);
        }
    }
    IncrementGameStat(GAME_STAT_POKEMON_TRADES);
    SaveMapView();
    if (TrySavingData(SAVE_NORMAL) != SAVE_STATUS_OK)
        return FALSE;
    return TRUE;
}

static u8 MpTj_PartnerSlot(void)
{
    const struct MpPlayer *p;
    u8 i;

    for (i = 0; i < MP_MAX_PLAYERS; i++)
    {
        p = MP_GetPlayer(i);
        if (p != NULL && !p->isLocal && p->summary.trainerId == sJournal.partnerTid)
            return i;
    }
    return MP_SLOT_NONE;
}

static bool8 MpTj_LiveWaiting(void)
{
    return sDirectTrade && MP_TradeCommitPoll() == MP_TRADE_COMMIT_WAITING;
}

static void MpTj_SendStatus(u8 slot, u8 flags)
{
    struct MpMsgTradeRecover m;

    MpTradeJournal_FillMsg(&sJournal, MpTradeJournal_LocalStatus(sJournal.state, FALSE, MpTj_LiveWaiting(), FALSE), slot, flags, &m);
    MP_TradeRecoverSend(&m);
}

static void MpTj_Act(u8 action)
{
    switch (action)
    {
    case MP_TJ_ACTION_COMMIT:
        if (MP_TradeJournalMarkCommit())
        {
            sJournalApplied = FALSE; // READY means this save was never signed with it
            sJournalSendTimer = 0;
        }
        break;
    case MP_TJ_ACTION_ABORT:
        MpTj_Delete();
        if (sJournal.state == MP_TJ_STATE_NONE)
            sJournalNotice = TJ_NOTICE_CANCELLED;
        break;
    case MP_TJ_ACTION_DONE:
        if (sJournalApplied)
            MpTj_Delete();
        else
            sJournalDonePending = TRUE;
        break;
    }
}

static void MpTj_OnMessage(const struct MpMsgTradeRecover *m)
{
    const struct MpPlayer *from = NULL;
    struct MpMsgTradeRecover reply;
    bool8 mine;
    u8 status;

    // Binding (AD-9): the trade id is the one its fields give, it is for us, and a relayed
    // message comes from the player whose trainer id it claims.
    if (m->tradeId != MpTradeJournal_IdOfMsg(m) || m->receiverTid != MpTj_MyTid())
        return;
    if (m->slot != MP_SLOT_NONE)
    {
        from = MP_GetPlayer(m->slot);
        if (from == NULL || from->isLocal || from->summary.trainerId != m->senderTid)
            return;
    }
    mine = !sJournalDamaged && MpTradeJournal_MatchesMsg(&sJournal, m);
    if (mine && !(sDirectTrade && sJournal.state == MP_TJ_STATE_READY))
    {
        MP_LOG("trade journal: trade %08X: %s says status %u\n", (unsigned)sJournal.tradeId, from ? "partner" : "host", (unsigned)m->status);
        MpTj_Act(MpTradeJournal_Resolve(sJournal.state, m->status));
    }
    if (from == NULL || !(m->flags & MP_TRADE_RECOVER_FLAG_REPLY))
        return;
    // Answer with what we know about that trade.
    if (mine && sJournal.state != MP_TJ_STATE_NONE)
        status = MpTradeJournal_LocalStatus(sJournal.state, FALSE, MpTj_LiveWaiting(), FALSE);
    else if (sJournalDamaged)
        status = MP_TJ_STATUS_DAMAGED;
    else if (sDirectTrade)
        status = MP_TJ_STATUS_BUSY;     // the party is mid-trade in memory: ask again later
    else
        status = MpTradeJournal_LocalStatus(MP_TJ_STATE_NONE, FALSE, FALSE, MpTj_Find(m->senderGivenPersonality, m->senderGivenOtId) >= 0);
    reply = *m;
    reply.slot = m->slot;
    reply.status = status;
    reply.flags = 0;
    reply.senderTid = m->receiverTid;
    reply.receiverTid = m->senderTid;
    reply.senderGivenPersonality = m->senderRecvPersonality;
    reply.senderGivenOtId = m->senderRecvOtId;
    reply.senderRecvPersonality = m->senderGivenPersonality;
    reply.senderRecvOtId = m->senderGivenOtId;
    MP_TradeRecoverSend(&reply);
}

void MpTrade_JournalUpdate(void)
{
    struct MpMsgTradeRecover m;
    u32 guard;

    for (guard = 0; guard < MP_TRADE_RECOVER_QUEUE && MP_TradeRecoverPoll(&m); guard++)
        MpTj_OnMessage(&m);
    if (!MP_IsActive() || sJournal.state == MP_TJ_STATE_NONE || sJournalDamaged)
        return;
    if (sJournal.state == MP_TJ_STATE_COMMIT && !sJournalApplied)
        return; // apply it first (field), then tell
    if (MpTj_LiveWaiting())
        return; // the live commit decides
    if (sJournalSendTimer > 0)
    {
        sJournalSendTimer--;
        return;
    }
    sJournalSendTimer = MP_TJ_SEND_INTERVAL;
    MpTj_SendStatus(MpTj_PartnerSlot(), MP_TRADE_RECOVER_FLAG_REPLY);
}

static void Task_MpJournalNotice(u8 taskId)
{
    if (IsFieldMessageBoxHidden() && JOY_NEW(A_BUTTON | B_BUTTON))
    {
        PlaySE(SE_SELECT);
        HideFieldMessageBox();
        ScriptUnfreezeObjectEvents();
        UnlockPlayerFieldControls();
        DestroyTask(taskId);
    }
}

bool8 MpTrade_TryShowJournalNotice(void)
{
    const u8 *text = NULL;

    if (gPlayerAvatar.tileTransitionState != T_NOT_MOVING || ScriptContext_IsEnabled()
     || !IsFieldMessageBoxHidden() || FuncIsActiveTask(Task_MpJournalNotice) || sDirectTrade)
        return FALSE;
    if (sJournal.state == MP_TJ_STATE_COMMIT && !sJournalApplied && !sJournalDamaged)
    {
        if (MpTj_Apply())
        {
            sJournalApplied = TRUE;
            sJournalSendTimer = 0;
            sJournalNotice = TJ_NOTICE_COMPLETED;
            MP_LOG("trade journal: trade %08X completed on this save (rolled forward)\n", (unsigned)sJournal.tradeId);
            if (sJournalDonePending)
                MpTj_Delete();
        }
        else
        {
            sJournalDamaged = TRUE;
            sJournalNotice = TJ_NOTICE_DAMAGED;
            MP_LOG("trade journal: trade %08X could not be applied; it stays pending\n", (unsigned)sJournal.tradeId);
        }
    }
    switch (sJournalNotice)
    {
    case TJ_NOTICE_PENDING:
        text = gText_MpTradeJournalPending;
        break;
    case TJ_NOTICE_COMPLETED:
        text = gText_MpTradeJournalCompleted;
        break;
    case TJ_NOTICE_CANCELLED:
        text = gText_MpTradeJournalCancelled;
        break;
    case TJ_NOTICE_DAMAGED:
        text = gText_MpTradeJournalDamaged;
        break;
    }
    if (text == NULL)
        return FALSE;
    if (sJournal.state != MP_TJ_STATE_NONE)
        memcpy(sNoticeName, sJournal.partnerName, sizeof(sNoticeName));
    sNoticeName[MP_PLAYER_NAME_SIZE - 1] = EOS;
    StringCopy(gStringVar1, sNoticeName);
    sJournalNotice = TJ_NOTICE_NONE;
    FreezeObjectEvents();
    PlayerFreeze();
    StopPlayerAvatar();
    LockPlayerFieldControls();
    ShowFieldMessage(text);
    CreateTask(Task_MpJournalNotice, 80);
    return TRUE;
}
