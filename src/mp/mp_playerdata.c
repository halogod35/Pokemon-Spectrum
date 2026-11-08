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
#include "main.h"
#include "pokemon.h"
#include "pokemon_summary_screen.h"
#include "trainer_card.h"
#include "constants/characters.h"
#include "constants/easy_chat.h"
#include "constants/flags.h"
#include "constants/items.h"
#include "constants/moves.h"
#include "constants/pokemon.h"
#include "constants/species.h"
#include "mp/mp.h"
#include "mp/mp_internal.h"
#include "mp/mp_playerdata.h"
#include "mp/mp_protocol.h"
#include "mp/mp_selftest.h"
#include "mp/mp_summary.h"

STATIC_ASSERT(MP_TRAINER_CARD_PROFILE_WORDS == TRAINER_CARD_PROFILE_LENGTH, MpCardProfileWords);
STATIC_ASSERT(MP_PLAYER_NAME_SIZE == PLAYER_NAME_LENGTH + 1, MpCardNameSize);
STATIC_ASSERT(MP_CARD_EC_EMPTY_WORD == EC_EMPTY_WORD, MpCardEmptyWord);
// PS-043: a mon travels as the raw struct Pokemon (the link trade format). Its layout is all
// u8/u16/u32 fields at natural alignment with no pointers, so it is exactly 100 bytes on every
// build (32/64-bit, Windows/Linux). If this ever fails, the wire format needs a real serialiser.
STATIC_ASSERT(sizeof(struct BoxPokemon) == 80, MpBoxMonWireSize);
STATIC_ASSERT(sizeof(struct Pokemon) == MP_MON_WIRE_SIZE, MpMonWireSize);

#define MP_MON_MARKING_COMBOS 16 // sAnims_MarkingCombo in mon_markings.c

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
// PS-043: party (raw struct Pokemon, protocol.md 6.13)
// ---------------------------------------------------------------------------

// The remote party while its summary screen is open. Never gPlayerParty / gEnemyParty, never
// written to the save (AD-10). Static, so it outlives the summary screen's heap use.
static EWRAM_DATA struct Pokemon sMpViewParty[PARTY_SIZE] = {0};

// Bytes before the first EOS (or all `len` bytes) are printable characters, not text control
// codes (0xF7..0xFE: placeholders, prompts, EXT_CTRL_CODE, newline).
static bool8 MpIsPlainText(const u8 *text, u32 len)
{
    u32 i;

    for (i = 0; i < len && text[i] != EOS; i++)
    {
        if (text[i] >= CHAR_DYNAMIC)
            return FALSE;
    }
    return TRUE;
}

bool8 MP_ValidateMon(const u8 *raw)
{
    struct Pokemon mon;
    u32 species, i;

    if (raw == NULL)
        return FALSE;
    // Everything below works on this copy. GetMonData decrypts in place and encrypts again,
    // and marks a bad checksum (isBadEgg) in place, so it must never run on `raw` itself.
    memcpy(&mon, raw, sizeof(mon));
    if (mon.box.isBadEgg)
        return FALSE;
    species = GetMonData(&mon, MON_DATA_SPECIES); // decrypts and verifies the checksum
    if (mon.box.isBadEgg)
        return FALSE;                             // checksum mismatch: tampered or corrupt
    if (!GetMonData(&mon, MON_DATA_SANITY_HAS_SPECIES))
        return FALSE;
    if (species == SPECIES_NONE || species >= NUM_SPECIES
     || (species >= SPECIES_OLD_UNOWN_B && species <= SPECIES_OLD_UNOWN_Z))
        return FALSE;                             // gSpeciesInfo / pic / cry table index
    if (mon.level < 1 || mon.level > MAX_LEVEL)
        return FALSE;                             // gExperienceTables[..][level + 1]
    if (mon.hp > mon.maxHP)
        return FALSE;
    if (GetMonData(&mon, MON_DATA_HELD_ITEM) >= ITEMS_COUNT)
        return FALSE;
    for (i = 0; i < MAX_MON_MOVES; i++)
    {
        if (GetMonData(&mon, MON_DATA_MOVE1 + i) >= MOVES_COUNT)
            return FALSE;                         // gMoveNames / gBattleMoves / gContestMoves index
    }
    if (GetMonData(&mon, MON_DATA_MARKINGS) >= MP_MON_MARKING_COMBOS)
        return FALSE;                             // sprite animation index
    if (GetMonData(&mon, MON_DATA_LANGUAGE) > LANGUAGE_SPANISH)
        return FALSE;
    if (!MpIsPlainText(mon.box.nickname, POKEMON_NAME_LENGTH) || !MpIsPlainText(mon.box.otName, PLAYER_NAME_LENGTH))
        return FALSE;
    return TRUE;
}

u8 MP_BuildLocalParty(u8 mons[PARTY_SIZE][MP_MON_WIRE_SIZE])
{
    struct Pokemon copy;
    u8 count = 0;
    u32 i;

    for (i = 0; i < PARTY_SIZE; i++)
    {
        memcpy(&copy, &gPlayerParty[i], sizeof(copy)); // never call GetMonData on the party itself here
        if (GetMonData(&copy, MON_DATA_SPECIES) == SPECIES_NONE)
            continue;
        if (!MP_ValidateMon((const u8 *)&gPlayerParty[i]))
        {
            MP_LOG("view: party mon %u does not pass the checks, not sent\n", (unsigned)i);
            continue;
        }
        memcpy(mons[count], &gPlayerParty[i], MP_MON_WIRE_SIZE);
        count++;
    }
    return count;
}

void MpPlayerData_ShowParty(u8 mons[PARTY_SIZE][MP_MON_WIRE_SIZE], u8 count, void (*callback)(void))
{
    u32 i;

    if (count == 0 || count > PARTY_SIZE)
    {
        SetMainCallback2(callback);
        return;
    }
    memset(sMpViewParty, 0, sizeof(sMpViewParty));
    for (i = 0; i < count; i++)
        memcpy(&sMpViewParty[i], mons[i], MP_MON_WIRE_SIZE);
    // Read only: LOCK_MOVES (as in battle) cannot reorder moves, and the list is our copy.
    ShowPokemonSummaryScreen(SUMMARY_MODE_LOCK_MOVES, sMpViewParty, 0, count - 1, callback);
}

void MpPlayerData_MakeTestMon(u8 *raw, u16 species, u8 level, u32 personality)
{
    struct SaveBlock2 *realSave2 = gSaveBlock2Ptr;
    struct SaveBlock1 *realSave1 = gSaveBlock1Ptr;
    static struct SaveBlock2 sOtSave; // CreateMon copies the OT name / gender from SaveBlock2
    static struct SaveBlock1 sLocSave; // CreateBoxMon reads the current map for the met location
    struct Pokemon mon;

    memset(&sOtSave, 0, sizeof(sOtSave));
    memset(sOtSave.playerName, EOS, sizeof(sOtSave.playerName));
    sOtSave.playerName[0] = CHAR_T;
    sOtSave.playerName[1] = CHAR_E;
    sOtSave.playerName[2] = CHAR_S;
    sOtSave.playerName[3] = CHAR_T;
    memset(&sLocSave, 0, sizeof(sLocSave)); // map 0.0 is a valid map header
    gSaveBlock2Ptr = &sOtSave;
    gSaveBlock1Ptr = &sLocSave;
    CreateMon(&mon, species, level, 10, TRUE, personality, OT_ID_PRESET, 0x00051234);
    gSaveBlock2Ptr = realSave2;
    gSaveBlock1Ptr = realSave1;
    memcpy(raw, &mon, MP_MON_WIRE_SIZE);
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

// --- PS-043 ---------------------------------------------------------------

// struct BoxPokemon byte offsets (pokemon.h): the checksum and the encrypted substructs.
#define PD_OFS_FLAGS        19  // isBadEgg (bit 0), hasSpecies (bit 1), isEgg (bit 2)
#define PD_OFS_CHECKSUM     28
#define PD_OFS_UNKNOWN      30
#define PD_OFS_SECURE       32
#define PD_OFS_SECURE_END   80

static u32 sPdRng = 0x4D504D4Eu; // "MPMN"

static u32 PdRand(void)
{
    sPdRng ^= sPdRng << 13;
    sPdRng ^= sPdRng >> 17;
    sPdRng ^= sPdRng << 5;
    return sPdRng;
}

static struct Pokemon sPdPartyBackup[PARTY_SIZE];
static struct Pokemon sPdPartyBefore[PARTY_SIZE];

// Validates and checks that the input bytes were not modified.
static bool8 PdValidate(const u8 *raw)
{
    u8 before[MP_MON_WIRE_SIZE];
    bool8 ok;

    memcpy(before, raw, sizeof(before));
    ok = MP_ValidateMon(raw);
    PD_CHECK(memcmp(before, raw, sizeof(before)) == 0, "MP_ValidateMon never modifies its input");
    return ok;
}

// A valid mon with one field changed through SetMonData (which keeps the checksum right),
// so the check under test is the field rule, not the checksum.
static bool8 PdValidWith(const u8 *base, s32 field, u32 value)
{
    struct Pokemon mon;
    u8 b8 = (u8)value;
    u16 b16 = (u16)value;

    memcpy(&mon, base, sizeof(mon));
    switch (field)
    {
    case MON_DATA_SPECIES:
    case MON_DATA_HELD_ITEM:
    case MON_DATA_MOVE1:
    case MON_DATA_MOVE4:
    case MON_DATA_HP:
        SetMonData(&mon, field, &b16);
        break;
    default:
        SetMonData(&mon, field, &b8);
        break;
    }
    return PdValidate((const u8 *)&mon);
}

static void PdTestValidateMon(void)
{
    u8 mon[MP_MON_WIRE_SIZE];
    u8 copy[MP_MON_WIRE_SIZE];
    u8 name[POKEMON_NAME_LENGTH + 1];
    struct Pokemon tmp;
    u32 failuresBefore = sPdFail;
    u32 i, bit, accepted;
    bool8 r1, r2;

    MpPlayerData_MakeTestMon(mon, SPECIES_TREECKO, 5, 0x12345678);
    PD_CHECK(PdValidate(mon), "a freshly created mon is valid");
    MpPlayerData_MakeTestMon(copy, SPECIES_RAYQUAZA, MAX_LEVEL, 0xCAFEBABE);
    PD_CHECK(PdValidate(copy), "a level 100 mon is valid");
    PD_CHECK(!MP_ValidateMon(NULL), "NULL is rejected");

    // Targeted field rules
    PD_CHECK(!PdValidWith(mon, MON_DATA_SPECIES, SPECIES_NONE), "species 0 rejected");
    PD_CHECK(!PdValidWith(mon, MON_DATA_SPECIES, NUM_SPECIES), "species NUM_SPECIES rejected");
    PD_CHECK(!PdValidWith(mon, MON_DATA_SPECIES, 0xFFFF), "species 0xFFFF rejected");
    PD_CHECK(!PdValidWith(mon, MON_DATA_SPECIES, SPECIES_OLD_UNOWN_B), "an unused old-Unown species rejected");
    PD_CHECK(PdValidWith(mon, MON_DATA_SPECIES, SPECIES_PIKACHU), "another valid species accepted");
    PD_CHECK(!PdValidWith(mon, MON_DATA_MOVE1, MOVES_COUNT), "move MOVES_COUNT rejected");
    PD_CHECK(!PdValidWith(mon, MON_DATA_MOVE4, 0xFFFF), "move 0xFFFF in slot 4 rejected");
    PD_CHECK(!PdValidWith(mon, MON_DATA_HELD_ITEM, ITEMS_COUNT), "item ITEMS_COUNT rejected");
    PD_CHECK(PdValidWith(mon, MON_DATA_HELD_ITEM, ITEM_POTION), "a real held item accepted");
    PD_CHECK(!PdValidWith(mon, MON_DATA_LEVEL, 0), "level 0 rejected");
    PD_CHECK(!PdValidWith(mon, MON_DATA_LEVEL, MAX_LEVEL + 1), "level 101 rejected");
    PD_CHECK(!PdValidWith(mon, MON_DATA_HP, 0xFFFF), "hp above max hp rejected");
    PD_CHECK(!PdValidWith(mon, MON_DATA_MARKINGS, MP_MON_MARKING_COMBOS), "markings 16 rejected");
    PD_CHECK(!PdValidWith(mon, MON_DATA_LANGUAGE, LANGUAGE_SPANISH + 1), "language 8 rejected");

    memcpy(&tmp, mon, sizeof(tmp));
    memset(name, EOS, sizeof(name));
    name[0] = CHAR_A;
    name[1] = 0xFC; // EXT_CTRL_CODE_BEGIN
    SetMonData(&tmp, MON_DATA_NICKNAME, name);
    PD_CHECK(!PdValidate((const u8 *)&tmp), "nickname with a control code rejected");
    memcpy(&tmp, mon, sizeof(tmp));
    name[1] = 0xFD; // PLACEHOLDER_BEGIN
    SetMonData(&tmp, MON_DATA_OT_NAME, name);
    PD_CHECK(!PdValidate((const u8 *)&tmp), "OT name with a placeholder rejected");
    memcpy(&tmp, mon, sizeof(tmp));
    memset(name, CHAR_A, POKEMON_NAME_LENGTH);
    SetMonData(&tmp, MON_DATA_NICKNAME, name);
    PD_CHECK(PdValidate((const u8 *)&tmp), "a 10-letter nickname without EOS accepted");

    memcpy(copy, mon, sizeof(copy));
    copy[PD_OFS_FLAGS] |= 1; // isBadEgg
    PD_CHECK(!PdValidate(copy), "a bad egg rejected");

    // Every single-bit flip: deterministic, the input untouched; the checksum and every bit
    // of the encrypted substructs are always caught.
    accepted = 0;
    for (i = 0; i < MP_MON_WIRE_SIZE; i++)
    {
        for (bit = 0; bit < 8; bit++)
        {
            memcpy(copy, mon, sizeof(copy));
            copy[i] ^= (u8)(1 << bit);
            r1 = PdValidate(copy);
            r2 = PdValidate(copy);
            PD_CHECK(r1 == r2, "MON bit flip deterministic");
            if (r1)
                accepted++;
            if ((i >= PD_OFS_CHECKSUM && i < PD_OFS_UNKNOWN) || (i >= PD_OFS_SECURE && i < PD_OFS_SECURE_END))
                PD_CHECK(!r1, "a flipped checksum / encrypted bit is rejected");
        }
    }
    MP_LOG("selftest: view: %u of %u single-bit mon flips still valid (unchecked or cosmetic bytes)\n",
           (unsigned)accepted, (unsigned)(MP_MON_WIRE_SIZE * 8));

    // Garbage: random buffers, and a valid mon with a few random bytes replaced
    MP_LOG("selftest: view: mon garbage seed 0x%08X\n", (unsigned)sPdRng);
    accepted = 0;
    for (i = 0; i < 100000; i++)
    {
        u32 n, k;

        if (i & 1)
        {
            for (n = 0; n < MP_MON_WIRE_SIZE; n++)
                copy[n] = (u8)PdRand();
        }
        else
        {
            memcpy(copy, mon, sizeof(copy));
            k = 1 + PdRand() % 4;
            for (n = 0; n < k; n++)
                copy[PdRand() % MP_MON_WIRE_SIZE] = (u8)PdRand();
        }
        if (MP_ValidateMon(copy))
            accepted++;
    }
    MP_LOG("selftest: view: %u of 100000 garbage / damaged mons passed (no crash)\n", (unsigned)accepted);
    PdReport("MP_ValidateMon", failuresBefore);
}

static void PdTestBuildParty(void)
{
    u8 a[MP_MON_WIRE_SIZE], b[MP_MON_WIRE_SIZE];
    u8 out[PARTY_SIZE][MP_MON_WIRE_SIZE];
    u32 failuresBefore = sPdFail;
    u8 count;

    memcpy(sPdPartyBackup, gPlayerParty, sizeof(sPdPartyBackup));
    MpPlayerData_MakeTestMon(a, SPECIES_MUDKIP, 12, 0x00000101);
    MpPlayerData_MakeTestMon(b, SPECIES_ZIGZAGOON, 3, 0x00000202);
    memset(gPlayerParty, 0, sizeof(gPlayerParty));
    memcpy(&gPlayerParty[0], a, MP_MON_WIRE_SIZE);
    memcpy(&gPlayerParty[1], b, MP_MON_WIRE_SIZE);
    ((u8 *)&gPlayerParty[1])[PD_OFS_SECURE + 5] ^= 0x40; // corrupt: fails the checksum
    memcpy(&gPlayerParty[2], b, MP_MON_WIRE_SIZE);
    memcpy(sPdPartyBefore, gPlayerParty, sizeof(sPdPartyBefore));

    memset(out, 0, sizeof(out));
    count = MP_BuildLocalParty(out);
    PD_CHECK(count == 2, "the party has 2 sendable mons (the corrupt one is left out)");
    PD_CHECK(memcmp(out[0], a, MP_MON_WIRE_SIZE) == 0 && memcmp(out[1], b, MP_MON_WIRE_SIZE) == 0,
             "the party is sent as raw, unchanged mons in party order");
    PD_CHECK(memcmp(sPdPartyBefore, gPlayerParty, sizeof(sPdPartyBefore)) == 0, "building the party leaves gPlayerParty untouched");

    memset(gPlayerParty, 0, sizeof(gPlayerParty));
    PD_CHECK(MP_BuildLocalParty(out) == 0, "an empty party gives 0 mons");

    memcpy(gPlayerParty, sPdPartyBackup, sizeof(sPdPartyBackup));
    PdReport("party built from gPlayerParty (read only)", failuresBefore);
}

u32 MpPlayerData_SelfTest(void)
{
    sPdFail = 0;
    PdTestBuildCard();
    PdTestToTrainerCard();
    PdTestValidateMon();
    PdTestBuildParty();
    return sPdFail;
}
