// PS-080: the built-in Mystery Gift distributor. See mystery_gift_distributor.h.
#include "global.h"
#include "event_data.h"
#include "item.h"
#include "link.h"
#include "string_util.h"
#include "mystery_gift.h"
#include "mystery_gift_link.h"
#include "mystery_gift_client.h"
#include "mystery_gift_server.h"
#include "mystery_gift_distributor.h"
#include "constants/flags.h"
#include "constants/items.h"
#include "constants/mystery_gift.h"
#include "constants/species.h"
#include "constants/vars.h"

// The server reads the client's READY_END right after the client ends; give it this many
// frames before the session is closed regardless (it cannot hang the screen).
#define DISTRIBUTOR_FINISH_TIMEOUT 300

// Shown as the sender in the Mystery Gift menu texts that name the other side
// (gLinkPlayers[0].name, PLAYER_NAME_LENGTH characters at most).
static const u8 sText_DistributorName[] = _("EVENT");

// The gift scripts, assembled from data/scripts/gift_*.inc by data/mystery_gift.s.
extern const u8 MysteryGiftScript_EonTicket[];
extern const u8 MysteryGiftScript_EonTicket_End[];
extern const u8 MysteryGiftScript_AuroraTicket[];
extern const u8 MysteryGiftScript_AuroraTicket_End[];
extern const u8 MysteryGiftScript_MysticTicket[];
extern const u8 MysteryGiftScript_MysticTicket_End[];
extern const u8 MysteryGiftScript_OldSeaMap[];
extern const u8 MysteryGiftScript_OldSeaMap_End[];

// The Wonder Cards. flagId follows the game's WONDER_CARD_FLAG_OFFSET convention
// (sReceivedGiftFlags in mystery_gift.c): 1000 Aurora, 1001 Mystic, 1002 Old Sea Map, 1003 Eon.
// Texts are ours (the originals are not in the decomp): WONDER_CARD_TEXT_LENGTH (40) bytes per
// line including the terminator, and about 32 characters fit the card's 200-pixel window.
// SEND_TYPE_DISALLOWED: the cards are not passed on to a friend (no wireless on PC).
#define TICKET_CARD(flag, species, id, bg) \
    .flagId = (flag), .iconSpecies = (species), .idNumber = (id), \
    .type = CARD_TYPE_GIFT, .bgType = (bg), .sendType = SEND_TYPE_DISALLOWED, .maxStamps = 0

static const struct WonderCard sCard_EonTicket = {
    TICKET_CARD(WONDER_CARD_FLAG_OFFSET + 3, SPECIES_LATIOS, 1, 2),
    .titleText = _("EON TICKET"),
    .subtitleText = _("Southern Island"),
    .bodyText = {
        _("A ferry ticket to SOUTHERN"),
        _("ISLAND, far to the south."),
        _("A rare POKéMON is said to live"),
        _("there, hidden from the world."),
    },
    .footerLine1Text = _("Collect it from the man upstairs"),
    .footerLine2Text = _("in any POKéMON CENTER."),
};

static const struct WonderCard sCard_AuroraTicket = {
    TICKET_CARD(WONDER_CARD_FLAG_OFFSET + 0, SPECIES_DEOXYS, 2, 1),
    .titleText = _("AURORA TICKET"),
    .subtitleText = _("Birth Island"),
    .bodyText = {
        _("A ferry ticket to BIRTH ISLAND,"),
        _("a lone rock out at sea."),
        _("Something from far away waits"),
        _("there. Solve its puzzle."),
    },
    .footerLine1Text = _("Collect it from the man upstairs"),
    .footerLine2Text = _("in any POKéMON CENTER."),
};

static const struct WonderCard sCard_MysticTicket = {
    TICKET_CARD(WONDER_CARD_FLAG_OFFSET + 1, SPECIES_LUGIA, 3, 5),
    .titleText = _("MYSTIC TICKET"),
    .subtitleText = _("Navel Rock"),
    .bodyText = {
        _("A ferry ticket to NAVEL ROCK,"),
        _("a tower rising from the sea."),
        _("Climb to its top and go down to"),
        _("its depths. Two legends await."),
    },
    .footerLine1Text = _("Collect it from the man upstairs"),
    .footerLine2Text = _("in any POKéMON CENTER."),
};

static const struct WonderCard sCard_OldSeaMap = {
    TICKET_CARD(WONDER_CARD_FLAG_OFFSET + 2, SPECIES_MEW, 4, 3),
    .titleText = _("OLD SEA MAP"),
    .subtitleText = _("Faraway Island"),
    .bodyText = {
        _("A faded map that marks FARAWAY"),
        _("ISLAND, past the horizon."),
        _("Few have ever set foot there."),
        _("Watch the tall grass closely."),
    },
    .footerLine1Text = _("Collect it from the man upstairs"),
    .footerLine2Text = _("in any POKéMON CENTER."),
};

static const struct MysteryGiftDistributorGift sGifts[] = {
    {
        .card = &sCard_EonTicket,
        .script = MysteryGiftScript_EonTicket,
        .scriptEnd = MysteryGiftScript_EonTicket_End,
        .item = ITEM_EON_TICKET,
        .receivedFlag = FLAG_RECEIVED_EON_TICKET,
        .shipFlag = FLAG_ENABLE_SHIP_SOUTHERN_ISLAND,
        .latiIcon = TRUE,
    },
    {
        .card = &sCard_AuroraTicket,
        .script = MysteryGiftScript_AuroraTicket,
        .scriptEnd = MysteryGiftScript_AuroraTicket_End,
        .item = ITEM_AURORA_TICKET,
        .receivedFlag = FLAG_RECEIVED_AURORA_TICKET,
        .shipFlag = FLAG_ENABLE_SHIP_BIRTH_ISLAND,
    },
    {
        .card = &sCard_MysticTicket,
        .script = MysteryGiftScript_MysticTicket,
        .scriptEnd = MysteryGiftScript_MysticTicket_End,
        .item = ITEM_MYSTIC_TICKET,
        .receivedFlag = FLAG_RECEIVED_MYSTIC_TICKET,
        .shipFlag = FLAG_ENABLE_SHIP_NAVEL_ROCK,
    },
    {
        .card = &sCard_OldSeaMap,
        .script = MysteryGiftScript_OldSeaMap,
        .scriptEnd = MysteryGiftScript_OldSeaMap_End,
        .item = ITEM_OLD_SEA_MAP,
        .receivedFlag = FLAG_RECEIVED_OLD_SEA_MAP,
        .shipFlag = FLAG_ENABLE_SHIP_FARAWAY_ISLAND,
    },
};

static bool8 sRunning = FALSE;
static u16 sFinishTimer = 0;
// The card being sent. A copy, so the Eon Ticket's icon can follow the player's Lati.
static struct WonderCard sDistributedCard;

u32 MysteryGiftDistributor_GetGiftCount(void)
{
    return ARRAY_COUNT(sGifts);
}

const struct MysteryGiftDistributorGift *MysteryGiftDistributor_GetGift(u32 index)
{
    if (index >= ARRAY_COUNT(sGifts))
        return NULL;
    return &sGifts[index];
}

bool32 MysteryGiftDistributor_IsGiftOwned(u32 index)
{
    const struct MysteryGiftDistributorGift *gift = MysteryGiftDistributor_GetGift(index);

    if (gift == NULL)
        return TRUE;
    if (FlagGet(gift->receivedFlag))
        return TRUE;
    if (FlagGet(gift->shipFlag))
        return TRUE;
    if (CheckBagHasItem(gift->item, 1))
        return TRUE;
    return FALSE;
}

s32 MysteryGiftDistributor_NextGift(void)
{
    u32 i;

    for (i = 0; i < ARRAY_COUNT(sGifts); i++)
    {
        if (!MysteryGiftDistributor_IsGiftOwned(i))
            return i;
    }
    return -1;
}

static const struct MysteryGiftServerCmd *MgDistributor_PickScript(bool32 isWonderNews)
{
    const struct MysteryGiftDistributorGift *gift;
    s32 next;
    u32 size;

    if (isWonderNews)
        return gMysteryGiftServerScript_NoNews;

    next = MysteryGiftDistributor_NextGift();
    if (next < 0)
        return gMysteryGiftServerScript_NoMoreGifts;

    gift = &sGifts[next];
    size = (u32)(gift->scriptEnd - gift->script);
    if (size == 0 || size > sizeof(gSaveBlock1Ptr->ramScript.data.script))
        return gMysteryGiftServerScript_NoMoreGifts; // cannot happen with the scripts we ship

    sDistributedCard = *gift->card;
    if (gift->latiIcon)
    {
        // Southern Island has the Lati that is not roaming (SouthernIsland_Interior_OnTransition).
        if (VarGet(VAR_ROAMER_POKEMON) == 0)
            sDistributedCard.iconSpecies = SPECIES_LATIOS;
        else
            sDistributedCard.iconSpecies = SPECIES_LATIAS;
    }
    MysteryGiftScripts_SetDistributedGift(&sDistributedCard, gift->script, size);
    return gMysteryGiftServerScript_DistributeCard;
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
