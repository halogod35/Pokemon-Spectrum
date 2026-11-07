// Pokemon Spectrum multiplayer: player data for "view another player" (PS-042, SK-SAVE).
//
// Sender side: MP_BuildLocalTrainerCard reads the loaded save through the game's own
// trainer card code. TrainerCard_GenerateCardForPlayer is static in trainer_card.c, so this
// uses the public TrainerCard_GenerateCardForLinkPlayer (the same SetPlayerCardData, in the
// 0x60-byte link layout the cable and Union Room send) followed by CopyTrainerCardData, which
// turns it back into an Emerald card exactly as a receiving game would. The result is then
// copied field by field into struct MpTrainerCard (never a raw struct on the wire,
// protocol.md 2) and clamped to the same limits the receiver enforces.
//
// Receiver side: MpPlayerData_ToTrainerCard / MpPlayerData_ShowTrainerCard display a card
// that MpProto_ParseTrainerCardData already validated.
//
// AD-10: nothing here writes to a save block. The only game globals written are
// gTrainerCards[1] and gLinkPlayers[1].language, and only for the duration of the
// ShowTrainerCardInLink call, after which they are restored.

#include "global.h"
#include "event_data.h"
#include "link.h"
#include "trainer_card.h"
#include "constants/characters.h"
#include "constants/easy_chat.h"
#include "constants/flags.h"
#include "mp/mp.h"
#include "mp/mp_internal.h"
#include "mp/mp_playerdata.h"
#include "mp/mp_protocol.h"
#include "mp/mp_selftest.h"
#include "mp/mp_summary.h"

STATIC_ASSERT(MP_TRAINER_CARD_PROFILE_WORDS == TRAINER_CARD_PROFILE_LENGTH, MpCardProfileWords);
STATIC_ASSERT(MP_PLAYER_NAME_SIZE == PLAYER_NAME_LENGTH + 1, MpCardNameSize);
STATIC_ASSERT(MP_CARD_EC_EMPTY_WORD == EC_EMPTY_WORD, MpCardEmptyWord);

// The card screen draws one of 5 palettes per star count (sHoennTrainerCardPals).
#define MP_CARD_PALETTE_COUNT (MP_CARD_MAX_STARS + 1)

static u16 MpClamp16(u32 value, u16 max)
{
    return (value > max) ? max : (u16)value;
}

static u8 MpClamp8(u32 value, u8 max)
{
    return (value > max) ? max : (u8)value;
}

void MP_BuildLocalTrainerCard(struct MpTrainerCard *out)
{
    struct TrainerCard link;
    struct TrainerCard card;
    struct MpPlayerSummary summary;
    u32 i;

    memset(out, 0, sizeof(*out));
    // The name, gender and trainer ID the other players already know us by (honours --name,
    // and a save name that breaks the wire rules is sanitised the same way).
    MP_BuildPlayerSummary(&summary);
    memcpy(out->name, summary.name, MP_PLAYER_NAME_SIZE);
    out->gender = (summary.gender == FEMALE) ? FEMALE : MALE;
    out->trainerId = (u16)(summary.trainerId & 0xFFFF);
    for (i = 0; i < MP_TRAINER_CARD_PROFILE_WORDS; i++)
        out->easyChatProfile[i] = EC_EMPTY_WORD;
    if (gSaveBlock1Ptr == NULL || gSaveBlock2Ptr == NULL)
        return; // no save loaded (cannot happen in a session, AD-11): name-only card

    memset(&link, 0, sizeof(link));
    TrainerCard_GenerateCardForLinkPlayer(&link);
    CopyTrainerCardData(&card, &link, VERSION_EMERALD);

    out->stars = MpClamp8(card.stars, MP_CARD_MAX_STARS);
    out->hasPokedex = card.hasPokedex ? TRUE : FALSE;
    out->caughtAllHoenn = card.caughtAllHoenn ? TRUE : FALSE;
    out->hasAllPaintings = card.hasAllPaintings ? TRUE : FALSE;
    out->hasAllFrontierSymbols = card.hasAllFrontierSymbols ? TRUE : FALSE;
    out->hofDebutHours = MpClamp16(card.hofDebutHours, MP_CARD_MAX_HOURS);
    out->hofDebutMinutes = MpClamp8(card.hofDebutMinutes, MP_CARD_MAX_MINUTES);
    out->hofDebutSeconds = MpClamp8(card.hofDebutSeconds, MP_CARD_MAX_MINUTES);
    out->caughtMonsCount = MpClamp16(card.caughtMonsCount, MP_CARD_MAX_COUNT_3);
    out->playTimeHours = MpClamp16(card.playTimeHours, MP_CARD_MAX_HOURS);
    out->playTimeMinutes = MpClamp8(card.playTimeMinutes, MP_CARD_MAX_MINUTES);
    out->linkBattleWins = MpClamp16(card.linkBattleWins, MP_CARD_MAX_COUNT_4);
    out->linkBattleLosses = MpClamp16(card.linkBattleLosses, MP_CARD_MAX_COUNT_4);
    out->battleTowerWins = MpClamp16(card.battleTowerWins, MP_CARD_MAX_COUNT_4);
    out->battleTowerStraightWins = MpClamp16(card.battleTowerStraightWins, MP_CARD_MAX_COUNT_4);
    out->contestsWithFriends = MpClamp16(card.contestsWithFriends, MP_CARD_MAX_COUNT_3);
    out->pokeblocksWithFriends = card.pokeblocksWithFriends;
    out->pokemonTrades = card.pokemonTrades;
    out->frontierBP = MpClamp16(card.frontierBP, MP_CARD_MAX_COUNT_4);
    out->money = (card.money > MP_CARD_MAX_MONEY) ? MP_CARD_MAX_MONEY : card.money;
    for (i = 0; i < MP_TRAINER_CARD_PROFILE_WORDS; i++)
    {
        u16 word = card.easyChatProfile[i];

        out->easyChatProfile[i] = (word == EC_EMPTY_WORD || EC_GROUP(word) < EC_NUM_GROUPS) ? word : EC_EMPTY_WORD;
    }
}

void MpPlayerData_ToTrainerCard(const struct MpTrainerCard *in, struct TrainerCard *out)
{
    u32 i;

    memset(out, 0, sizeof(*out));
    out->version = VERSION_EMERALD;
    out->gender = (in->gender == FEMALE) ? FEMALE : MALE;                 // sTrainerPicOffset[..][gender]
    out->stars = (in->stars < MP_CARD_PALETTE_COUNT) ? in->stars : MP_CARD_MAX_STARS; // palette index
    out->hasPokedex = in->hasPokedex ? TRUE : FALSE;
    out->caughtAllHoenn = in->caughtAllHoenn ? TRUE : FALSE;
    out->hasAllPaintings = in->hasAllPaintings ? TRUE : FALSE;
    out->hofDebutHours = MpClamp16(in->hofDebutHours, MP_CARD_MAX_HOURS);
    out->hofDebutMinutes = MpClamp8(in->hofDebutMinutes, MP_CARD_MAX_MINUTES);
    out->hofDebutSeconds = MpClamp8(in->hofDebutSeconds, MP_CARD_MAX_MINUTES);
    out->caughtMonsCount = MpClamp16(in->caughtMonsCount, MP_CARD_MAX_COUNT_3);
    out->trainerId = in->trainerId;
    out->playTimeHours = MpClamp16(in->playTimeHours, MP_CARD_MAX_HOURS);
    out->playTimeMinutes = MpClamp8(in->playTimeMinutes, MP_CARD_MAX_MINUTES);
    out->linkBattleWins = MpClamp16(in->linkBattleWins, MP_CARD_MAX_COUNT_4);
    out->linkBattleLosses = MpClamp16(in->linkBattleLosses, MP_CARD_MAX_COUNT_4);
    out->battleTowerWins = MpClamp16(in->battleTowerWins, MP_CARD_MAX_COUNT_4);
    out->battleTowerStraightWins = MpClamp16(in->battleTowerStraightWins, MP_CARD_MAX_COUNT_4);
    out->contestsWithFriends = MpClamp16(in->contestsWithFriends, MP_CARD_MAX_COUNT_3);
    out->pokeblocksWithFriends = in->pokeblocksWithFriends;
    out->pokemonTrades = in->pokemonTrades;
    out->money = (in->money > MP_CARD_MAX_MONEY) ? MP_CARD_MAX_MONEY : in->money;
    for (i = 0; i < TRAINER_CARD_PROFILE_LENGTH; i++)
    {
        u16 word = in->easyChatProfile[i];

        out->easyChatProfile[i] = (word == EC_EMPTY_WORD || EC_GROUP(word) < EC_NUM_GROUPS) ? word : EC_EMPTY_WORD;
    }
    // Name: the parser enforced the 5.1.1 rules; make sure of the terminator anyway.
    memcpy(out->playerName, in->name, PLAYER_NAME_LENGTH + 1);
    out->playerName[PLAYER_NAME_LENGTH] = EOS;
    // Emerald fields, as CopyTrainerCardData sets them for an Emerald partner.
    out->hasAllFrontierSymbols = in->hasAllFrontierSymbols ? TRUE : FALSE;
    out->frontierBP = MpClamp16(in->frontierBP, MP_CARD_MAX_COUNT_4);
    // unionRoomClass stays 0: it is only used inside the Union Room with a link open.
}

void MpPlayerData_ShowTrainerCard(const struct MpTrainerCard *card, void (*callback)(void))
{
    struct TrainerCard savedCard = gTrainerCards[1];
    u16 savedLanguage = gLinkPlayers[1].language;

    MpPlayerData_ToTrainerCard(card, &gTrainerCards[1]);
    gLinkPlayers[1].language = GAME_LANGUAGE;
    ShowTrainerCardInLink(1, callback); // copies both into its own heap data at once
    gTrainerCards[1] = savedCard;
    gLinkPlayers[1].language = savedLanguage;
}

// ---------------------------------------------------------------------------
// Self-test (--mp-selftest, from MpSelftest_View)
// ---------------------------------------------------------------------------

static u32 sPdFail;

#define PD_CHECK(cond, what)                                                        \
    do                                                                              \
    {                                                                               \
        if (!(cond))                                                                \
        {                                                                           \
            MP_LOG("selftest: FAIL: %s (%s:%d)\n", what, __FILE__, __LINE__);       \
            sPdFail++;                                                              \
        }                                                                           \
    } while (0)

// In-memory save blocks (large, so static). The real save pointers are swapped and restored.
static struct SaveBlock1 sPdSave1;
static struct SaveBlock2 sPdSave2;
static struct SaveBlock1 sPdSnap1;
static struct SaveBlock2 sPdSnap2;

static void PdReport(const char *what, u32 failuresBefore)
{
    if (sPdFail == failuresBefore)
        MP_LOG("selftest: view: %s OK\n", what);
}

static void PdTestBuildCard(void)
{
    struct SaveBlock1 *realSave1 = gSaveBlock1Ptr;
    struct SaveBlock2 *realSave2 = gSaveBlock2Ptr;
    struct MpMsgTrainerCardData msg, parsed;
    u8 buf[MP_MAX_PACKET_SIZE];
    u32 failuresBefore = sPdFail;
    u16 len;

    memset(&sPdSave1, 0, sizeof(sPdSave1));
    memset(&sPdSave2, 0, sizeof(sPdSave2));
    memset(sPdSave2.playerName, EOS, sizeof(sPdSave2.playerName));
    sPdSave2.playerName[0] = CHAR_M;
    sPdSave2.playerName[1] = CHAR_a;
    sPdSave2.playerName[2] = CHAR_y;
    sPdSave2.playerGender = FEMALE;
    sPdSave2.playerTrainerId[0] = 0x39;
    sPdSave2.playerTrainerId[1] = 0x30;
    sPdSave2.playerTrainerId[2] = 0x05;
    sPdSave2.playerTrainerId[3] = 0x80;
    sPdSave2.playTimeHours = 12;
    sPdSave2.playTimeMinutes = 34;
    sPdSave1.money = 1234567; // over the card's 6 digits: clamped (encryption key 0)
    sPdSave1.easyChatProfile[0] = EC_WORD(EC_GROUP_PEOPLE, 3);
    sPdSave1.easyChatProfile[1] = 0xFE00; // no such group: sent as EC_EMPTY_WORD
    sPdSave1.easyChatProfile[2] = EC_EMPTY_WORD;
    sPdSave1.easyChatProfile[3] = EC_EMPTY_WORD;
    gSaveBlock1Ptr = &sPdSave1;
    gSaveBlock2Ptr = &sPdSave2;
    FlagSet(FLAG_SYS_POKEDEX_GET);
    memcpy(&sPdSnap1, &sPdSave1, sizeof(sPdSnap1));
    memcpy(&sPdSnap2, &sPdSave2, sizeof(sPdSnap2));

    memset(&msg, 0, sizeof(msg));
    MP_BuildLocalTrainerCard(&msg.card);
    PD_CHECK(memcmp(&sPdSnap1, &sPdSave1, sizeof(sPdSnap1)) == 0, "building the card leaves SaveBlock1 untouched");
    PD_CHECK(memcmp(&sPdSnap2, &sPdSave2, sizeof(sPdSnap2)) == 0, "building the card leaves SaveBlock2 untouched");
    PD_CHECK(msg.card.gender == FEMALE && msg.card.trainerId == 0x3039, "card gender and public trainer id");
    PD_CHECK(msg.card.playTimeHours == 12 && msg.card.playTimeMinutes == 34, "card play time");
    PD_CHECK(msg.card.money == MP_CARD_MAX_MONEY, "card money clamped to 999999");
    PD_CHECK(msg.card.hasPokedex == TRUE, "card has the POKeDEX flag");
    PD_CHECK(msg.card.stars <= MP_CARD_MAX_STARS, "card stars in range");
    PD_CHECK(msg.card.easyChatProfile[0] == EC_WORD(EC_GROUP_PEOPLE, 3) && msg.card.easyChatProfile[1] == EC_EMPTY_WORD,
             "card easy chat profile (unknown group sent as empty)");
    PD_CHECK(MP_IsValidPlayerName(msg.card.name), "card name follows the wire rules");

    // What the builder makes always passes the receiver's parser, unchanged.
    msg.slot = 1;
    msg.epoch = 1;
    msg.requestId = 7;
    len = MpProto_WriteTrainerCardData(buf, sizeof(buf), &msg);
    PD_CHECK(len == MP_HEADER_SIZE + MP_TRAINER_CARD_DATA_SIZE, "built card writes");
    PD_CHECK(MpProto_ParseTrainerCardData(&buf[MP_HEADER_SIZE], MP_TRAINER_CARD_DATA_SIZE, &parsed) == MP_PARSE_OK
          && memcmp(&parsed.card, &msg.card, sizeof(parsed.card)) == 0, "built card parses back identically");

    // No save loaded: a name-only card, still valid.
    gSaveBlock1Ptr = NULL;
    gSaveBlock2Ptr = NULL;
    MP_BuildLocalTrainerCard(&msg.card);
    len = MpProto_WriteTrainerCardData(buf, sizeof(buf), &msg);
    PD_CHECK(MpProto_ParseTrainerCardData(&buf[MP_HEADER_SIZE], MP_TRAINER_CARD_DATA_SIZE, &parsed) == MP_PARSE_OK,
             "card without a save still parses");

    gSaveBlock1Ptr = realSave1;
    gSaveBlock2Ptr = realSave2;
    PdReport("trainer card built from a save (read only)", failuresBefore);
}

static void PdTestToTrainerCard(void)
{
    struct MpTrainerCard in;
    struct TrainerCard out;
    u32 failuresBefore = sPdFail;

    memset(&in, 0, sizeof(in));
    in.gender = 7;              // never passes the parser; the conversion clamps anyway
    in.stars = 9;
    in.hasPokedex = TRUE;
    in.hasAllFrontierSymbols = TRUE;
    in.frontierBP = 60000;
    in.money = 0xFFFFFFFF;
    in.playTimeHours = 5000;
    in.easyChatProfile[0] = 0xFE00;
    in.easyChatProfile[1] = EC_WORD(EC_GROUP_PEOPLE, 1);
    memset(in.name, CHAR_A, sizeof(in.name)); // no EOS: the conversion terminates it
    MpPlayerData_ToTrainerCard(&in, &out);
    PD_CHECK(out.version == VERSION_EMERALD, "converted card is an Emerald card");
    PD_CHECK(out.gender == MALE && out.stars == MP_CARD_MAX_STARS, "converted card: gender and stars are valid indices");
    PD_CHECK(out.playerName[PLAYER_NAME_LENGTH] == EOS && out.playerName[0] == CHAR_A, "converted card: name terminated");
    PD_CHECK(out.hasPokedex == TRUE && out.hasAllFrontierSymbols == TRUE && out.frontierBP == MP_CARD_MAX_COUNT_4,
             "converted card: frontier fields in the Emerald place");
    PD_CHECK(out.money == MP_CARD_MAX_MONEY && out.playTimeHours == MP_CARD_MAX_HOURS, "converted card: counters clamped");
    PD_CHECK(out.easyChatProfile[0] == EC_EMPTY_WORD && out.easyChatProfile[1] == EC_WORD(EC_GROUP_PEOPLE, 1),
             "converted card: easy chat words");
    PD_CHECK(out.unionRoomClass == 0 && out.linkPoints.frontier == 0, "converted card: link-only fields zero");
    PdReport("conversion to struct TrainerCard", failuresBefore);
}

u32 MpPlayerData_SelfTest(void)
{
    sPdFail = 0;
    PdTestBuildCard();
    PdTestToTrainerCard();
    return sPdFail;
}
