// PS-080: the built-in Mystery Gift distributor. See mystery_gift_distributor.h.
#include "global.h"
#include "link.h"
#include "string_util.h"
#include "mystery_gift.h"
#include "mystery_gift_link.h"
#include "mystery_gift_client.h"
#include "mystery_gift_server.h"
#include "mystery_gift_distributor.h"

// The server reads the client's READY_END right after the client ends; give it this many
// frames before the session is closed regardless (it cannot hang the screen).
#define DISTRIBUTOR_FINISH_TIMEOUT 300

// Shown as the sender in the Mystery Gift menu texts that name the other side
// (gLinkPlayers[0].name, PLAYER_NAME_LENGTH characters at most).
static const u8 sText_DistributorName[] = _("EVENT");

static bool8 sRunning = FALSE;
static u16 sFinishTimer = 0;

static const struct MysteryGiftServerCmd *MgDistributor_PickScript(bool32 isWonderNews)
{
    if (isWonderNews)
        return gMysteryGiftServerScript_NoNews;
    return gMysteryGiftServerScript_NoMoreGifts;
}

void MysteryGiftDistributor_Start(bool32 isWonderNews)
{
    MysteryGiftDistributor_Stop();
    MysteryGiftLink_StartLoopback();
    MysterGiftServer_CreateForScript(MgDistributor_PickScript(isWonderNews));

    // The menu copies gLinkPlayers[0].name into the "received from" / toss texts.
    memset(&gLinkPlayers[0], 0, sizeof(gLinkPlayers[0]));
    StringCopy(gLinkPlayers[0].name, sText_DistributorName);

    sRunning = TRUE;
    sFinishTimer = 0;
}

bool32 MysteryGiftDistributor_IsRunning(void)
{
    return sRunning;
}

void MysteryGiftDistributor_Tick(void)
{
    u16 serverResult;

    if (!sRunning || !MysterGiftServer_IsActive())
        return;
    MysterGiftServer_Run(&serverResult); // frees the server when its script returns
}

bool32 MysteryGiftDistributor_Failed(void)
{
    return sRunning && MysteryGiftLink_LoopbackFailed();
}

bool32 MysteryGiftDistributor_Finish(void)
{
    if (!sRunning)
        return TRUE;
    if (MysterGiftServer_IsActive() && !MysteryGiftLink_LoopbackFailed() && ++sFinishTimer < DISTRIBUTOR_FINISH_TIMEOUT)
        return FALSE;
    MysteryGiftDistributor_Stop();
    return TRUE;
}

void MysteryGiftDistributor_Stop(void)
{
    MysteryGiftClient_Destroy();
    MysterGiftServer_Destroy();
    MysteryGiftLink_StopLoopback();
    sRunning = FALSE;
    sFinishTimer = 0;
}
