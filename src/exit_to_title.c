// PS-088: START menu EXIT returns to the title screen without restarting the program.
//
// CB2_ExitToTitleScreen runs once, as the main callback, on the frame after the player
// confirmed EXIT (start_menu.c) and the field faded to black. It does what the game's own
// "leave the field from anywhere" paths do (CB2_LinkError + ReloadSave after a link error,
// CB2_StartCreditsSequence, MoveSaveBlocks_ResetHeap on every return to the field), reloads
// the save from flash exactly as ReloadSave() and the intro do, and hands over to
// CB2_InitTitleScreen, which then runs from the same state as after a program start.
//
// Why a plain reload is enough: the title menu's CONTINUE goes through CB2_ContinueSavedGame,
// which rebuilds the field from the save blocks (map, object events, weather, tasks, sprites,
// windows, script context, match call counters, play time). What that path does NOT reset,
// because a GBA boot zeroes it, is reset here by hand (see the "boot leaves it zero" block).
#include "global.h"
#include "main.h"
#include "battle.h"
#include "bg.h"
#include "dma3.h"
#include "event_data.h"
#include "event_object_lock.h"
#include "exit_to_title.h"
#include "field_control_avatar.h"
#include "field_specials.h"
#include "link.h"
#include "m4a.h"
#include "menu.h"
#include "overworld.h"
#include "palette.h"
#include "play_time.h"
#include "reload_save.h"
#include "scanline_effect.h"
#include "script.h"
#include "sound.h"
#include "sprite.h"
#include "task.h"
#include "title_screen.h"
#include "mp/mp.h"

static void ClearSpecialVars(void)
{
    gSpecialVar_0x8000 = 0;
    gSpecialVar_0x8001 = 0;
    gSpecialVar_0x8002 = 0;
    gSpecialVar_0x8003 = 0;
    gSpecialVar_0x8004 = 0;
    gSpecialVar_0x8005 = 0;
    gSpecialVar_0x8006 = 0;
    gSpecialVar_0x8007 = 0;
    gSpecialVar_0x8008 = 0;
    gSpecialVar_0x8009 = 0;
    gSpecialVar_0x800A = 0;
    gSpecialVar_0x800B = 0;
    gSpecialVar_Result = 0;
    gSpecialVar_LastTalked = 0;
    gSpecialVar_Facing = 0;
    gSpecialVar_MonBoxId = 0;
    gSpecialVar_MonBoxPos = 0;
    gSpecialVar_Unused_0x8014 = 0;
}

void CB2_ExitToTitleScreen(void)
{
    // 1. Multiplayer. Leave the session: BYE goes to the others (they get the "left" notice,
    //    the host drops our slot) and keeps flushing in the background. MP_Leave also ends a
    //    simulated link-cable group and cancels a pending connect; it is a no-op when offline.
    //    The title menu then offers MULTIPLAYER again as on a fresh start.
    MP_Leave(); // MP: only the MP_* API from game code

    // 2. Sound. Every music player (BGM, SE, cries) and the map music state machine, which
    //    MainLoop() keeps pumping on the title screen. CB2_InitTitleScreen starts MUS_TITLE.
    m4aMPlayAllStop();
    StopCryAndClearCrySongs();
    ResetMapMusic();

    // 3. Main-loop callbacks. Nothing of the field may run on the title screen:
    //    CB2_InitTitleScreen does not clear callback1 (a boot leaves it NULL), and the field
    //    VBlank callback would keep transferring buffers that are freed below.
    gMain.callback1 = NULL;
    SetVBlankCallback(NULL);
    SetHBlankCallback(NULL);
    gFieldCallback = NULL;
    gFieldCallback2 = NULL;
    ClearTrainerHillVBlankCounter();

    // 4. Close the START menu the way its other exits do (objects unfrozen, controls unlocked),
    //    so the field statics behind them are in their idle state.
    ScriptUnfreezeObjectEvents();
    UnlockPlayerFieldControls();

    // 5. Field state that a boot leaves zero and CB2_ContinueSavedGame does not touch.
    PlayTimeCounter_Stop();             // else the title screen's time counts into the reloaded save
    ResetInitialPlayerAvatarState();    // what CB2_WhiteOut / CB2_NewGame do before a reload
    ResetCyclingRoadChallengeData();    // only map transitions reset it, CONTINUE does not
    RestartWildEncounterImmunitySteps();
    ClearSpecialVars();
    gBattleTypeFlags = 0;               // EWRAM: zero after the GBA's ReloadSave
    gSoftResetDisabled = FALSE;

    // 6. Link state. The PC's own clean-up before ReloadSave() (PS-048): closes the cable if it
    //    was open, clears gReceivedRemoteLinkPlayers / gLinkCallback / gLinkErrorOccurred and
    //    the error buffer, forgets a direct trade or battle (none can be running from the START
    //    menu) and leaves the link group. Then the player table and the comm type, as at boot.
    MP_LinkPrepareErrorRecovery();
    ResetLinkPlayers();
    SetWirelessCommType0();

    // 7. Graphics tables: the same list as CB2_LinkError and CB2_StartCreditsSequence. The
    //    field's windows, its three BG tilemap buffers and the Mirage Tower effect are freed
    //    (and their pointers cleared) before the heap goes; pending VRAM copies out of those
    //    buffers are dropped.
    ScanlineEffect_Stop();
    ResetTasks();
    ResetSpriteData();
    FreeAllSpritePalettes();
    ResetPaletteFade();
    CleanupOverworldWindowsAndTilemaps();
    ClearScheduledBgCopiesToVram();
    ClearDma3Requests();
    ResetBgsAndClearDma3BusyFlags(0);

    // 8. The save, from flash, exactly as ReloadSave() and the intro's boot path: save block
    //    pointers, menu/mon globals (gDifferentSaveFile), save counters, LoadGameSave, defaults
    //    for an empty or corrupt save, cry stereo, and InitHeap (the field's remaining heap use
    //    is gone with it; every return to the field re-initialises the heap the same way).
    ReloadSaveFromFlash();

    // 9. The title screen, as after a program start (it clears VRAM/OAM/PLTT itself).
    SetMainCallback2(CB2_InitTitleScreen);
}
