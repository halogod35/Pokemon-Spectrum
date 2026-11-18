// Pokemon Spectrum multiplayer selftest: PS-089 direct record mixing.
//
// 1. INTERACT with the new kind 6 (MIX_RECORDS, protocol 12): round trips on all three INTERACT
//    types and every kind byte 0..255; the summary's CAN_MIX_RECORDS bit on the wire (5.1).
// 2. The record sanitizer (MP_MixValidateRecord, mp_mix_validate.c) on a struct
//    PlayerRecordEmerald: an all-zero record and a filled clean record pass; every crafted bad
//    field (one per rule) is rejected with the right code and element index; an unused apprentice
//    slot with garbage behind its EOS name and a non-master's unset gift item are not rejected.
// 3. A random-byte fuzz of the clean record: never crashes, deterministic, codes in range.
// The exchange itself needs two real windows (card PS-089, manual test).

#include <string.h>
#include "global.h"
#include "record_mixing.h"
#include "constants/apprentice.h"
#include "constants/characters.h"
#include "constants/decorations.h"
#include "constants/easy_chat.h"
#include "constants/game_stat.h"
#include "constants/items.h"
#include "constants/lilycove_lady.h"
#include "constants/mauville_old_man.h"
#include "constants/moves.h"
#include "constants/pokemon.h"
#include "constants/secret_bases.h"
#include "constants/species.h"
#include "constants/trainers.h"
#include "constants/tv.h"
#include "mp/mp.h"
#include "mp/mp_internal.h"
#include "mp/mp_mix.h"
#include "mp/mp_protocol.h"
#include "mp/mp_selftest.h"
#include "mp/mp_summary.h"

static u32 sFail;

#define X_CHECK(cond, what)                                                        \
    do                                                                             \
    {                                                                              \
        if (!(cond))                                                               \
        {                                                                          \
            MP_LOG("selftest: FAIL: mix: %s (%s:%d)\n", what, __FILE__, __LINE__); \
            sFail++;                                                               \
        }                                                                          \
    } while (0)

static void Report(const char *what, u32 failuresBefore)
{
    if (sFail == failuresBefore)
        MP_LOG("selftest: mix: %s OK\n", what);
    else
        MP_LOG("selftest: mix: %s FAILED (%u)\n", what, (unsigned)(sFail - failuresBefore));
}

static u32 sRng = 0x4D495853u; // "MIXS"

static u32 Rand(void)
{
    sRng ^= sRng << 13;
    sRng ^= sRng >> 17;
    sRng ^= sRng << 5;
    return sRng;
}

static struct PlayerRecordEmerald sRec;

#define GOOD_WORD   EC_WORD(EC_GROUP_CONDITIONS, 0)
#define BAD_WORD    EC_WORD(EC_NUM_GROUPS, 0)      // group 22: no such group
#define BAD_NAME_BYTE 0xAA                         // never EOS, never 0

// ===========================================================================
// 1. INTERACT kind MIX_RECORDS and the summary flag
// ===========================================================================

static void TestInteractKind(void)
{
    static const u8 sTypes[3] = { MP_MSG_INTERACT_REQUEST, MP_MSG_INTERACT_RESPONSE, MP_MSG_INTERACT_CANCEL };
    u8 buf[MP_MAX_PACKET_SIZE + 8];
    struct MpMsgInteract in, out;
    struct MpPlayerSummary s, r;
    u32 failuresBefore = sFail;
    u32 t, k;
    u16 len;
    u8 res;

    X_CHECK(MP_PROTOCOL_VERSION >= 12, "protocol version 12 or later (PS-089)");
    X_CHECK(MP_INTERACT_MIX_RECORDS == 6 && MP_INTERACT_KIND_COUNT == 7, "kind 6 is MIX_RECORDS");
    for (t = 0; t < 3; t++)
    {
        memset(&in, 0, sizeof(in));
        in.slot = 2;
        in.epoch = 7;
        in.kind = MP_INTERACT_MIX_RECORDS;
        in.requestId = 0x3A;
        in.result = (sTypes[t] == MP_MSG_INTERACT_RESPONSE) ? MP_INTERACT_RESULT_DECLINED : 0;
        len = MpProto_WriteInteract(buf, MP_MAX_PACKET_SIZE, sTypes[t], &in);
        X_CHECK(len == MP_HEADER_SIZE + MP_INTERACT_SIZE, "MIX_RECORDS write size");
        X_CHECK(MpProto_ParseInteract(sTypes[t], buf + MP_HEADER_SIZE, MP_INTERACT_SIZE, &out) == MP_PARSE_OK
             && out.kind == MP_INTERACT_MIX_RECORDS && out.slot == 2 && out.epoch == 7 && out.requestId == 0x3A, "MIX_RECORDS round trip");
        for (k = 0; k < 256; k++)
        {
            buf[MP_HEADER_SIZE + 2] = (u8)k;
            res = MpProto_ParseInteract(sTypes[t], buf + MP_HEADER_SIZE, MP_INTERACT_SIZE, &out);
            if (k >= 1 && k < MP_INTERACT_KIND_COUNT)
                X_CHECK(res == MP_PARSE_OK && out.kind == k, "kinds 1..6 accepted");
            else
                X_CHECK(res == MP_PARSE_ERR_BAD_FIELD, "kind 0 / 7..255 rejected");
        }
    }

    // The summary's CAN_MIX_RECORDS bit: written, read back, unknown bits dropped.
    memset(&s, 0, sizeof(s));
    memset(s.name, EOS, MP_PLAYER_NAME_SIZE);
    s.name[0] = CHAR_A;
    s.gender = MALE;
    s.avatarStyle = MP_AVATAR_STYLE_PLAYER;
    s.trainerId = 0x12345678;
    s.summaryFlags = MP_SUMMARY_FLAG_CAN_MIX_RECORDS;
    X_CHECK(MP_WritePlayerSummary(buf, sizeof(buf), &s) == MP_PLAYER_SUMMARY_WIRE_SIZE && buf[11] == MP_SUMMARY_FLAG_CAN_MIX_RECORDS, "summary flag written");
    X_CHECK(MP_ReadPlayerSummary(buf, MP_PLAYER_SUMMARY_WIRE_SIZE, &r) && r.summaryFlags == MP_SUMMARY_FLAG_CAN_MIX_RECORDS, "summary flag read back");
    buf[11] = 0xFF;
    X_CHECK(MP_ReadPlayerSummary(buf, MP_PLAYER_SUMMARY_WIRE_SIZE, &r) && r.summaryFlags == MP_SUMMARY_FLAG_CAN_MIX_RECORDS, "unknown summary bits dropped");
    s.summaryFlags = 0xFE;
    X_CHECK(MP_WritePlayerSummary(buf, sizeof(buf), &s) == MP_PLAYER_SUMMARY_WIRE_SIZE && buf[11] == 0, "unknown summary bits not sent");
    Report("INTERACT kind MIX_RECORDS and the summary flag", failuresBefore);
}

// ===========================================================================
// 2. The sanitizer
// ===========================================================================

static void SetName(u8 *dst, u32 size)
{
    memset(dst, EOS, size);
    dst[0] = CHAR_A;
    dst[1] = CHAR_S;
    dst[2] = CHAR_H;
}

static void SetWords(u16 *words, u32 count)
{
    u32 i;

    for (i = 0; i < count; i++)
        words[i] = (i & 1) ? EC_EMPTY_WORD : GOOD_WORD;
}

// A record as a real save would send it, with every part in use.
static void MakeCleanRecord(struct PlayerRecordEmerald *r)
{
    u32 i, j;
    TVShow *show;

    memset(r, 0, sizeof(*r));

    r->secretBases[0].secretBaseId = 1;     // SECRET_BASE_RED_CAVE1_1
    r->secretBases[0].decorations[0] = DECOR_SMALL_DESK;
    r->secretBases[0].decorationPositions[0] = 0x23;
    memcpy(r->secretBases[0].trainerName, "\xBB\xCD\xC2\xFF\xFF\xFF\xFF", PLAYER_NAME_LENGTH);
    r->secretBases[0].party.species[0] = SPECIES_TREECKO;
    r->secretBases[0].party.moves[0] = MOVE_POUND;
    r->secretBases[0].party.heldItems[0] = ITEM_POTION;
    r->secretBases[0].party.levels[0] = 50;
    r->secretBases[1].secretBaseId = 42;    // BROWN_CAVE1_2
    r->secretBases[1].decorations[0] = NUM_DECORATIONS; // the last valid id
    r->secretBases[1].party.species[0] = SPECIES_TORCHIC;
    r->secretBases[1].party.levels[0] = MAX_LEVEL;

    show = &r->tvShows[0];
    show->common.kind = TVSHOW_FAN_CLUB_LETTER;
    show->common.active = TRUE;
    show->fanclubLetter.species = SPECIES_TREECKO;
    SetWords(show->fanclubLetter.words, ARRAY_COUNT(show->fanclubLetter.words));
    SetName(show->fanclubLetter.playerName, sizeof(show->fanclubLetter.playerName));
    show = &r->tvShows[1];
    show->common.kind = TVSHOW_BRAVO_TRAINER_POKEMON_PROFILE;
    show->bravoTrainer.species = SPECIES_TORCHIC;
    SetWords(show->bravoTrainer.words, ARRAY_COUNT(show->bravoTrainer.words));
    SetName(show->bravoTrainer.pokemonNickname, sizeof(show->bravoTrainer.pokemonNickname));
    show->bravoTrainer.contestCategory = CONTEST_CATEGORY_CUTE;
    show->bravoTrainer.move = MOVE_SCRATCH;
    SetName(show->bravoTrainer.playerName, sizeof(show->bravoTrainer.playerName));
    show = &r->tvShows[2];
    show->common.kind = TVSHOW_POKEMON_TODAY_CAUGHT;
    SetName(show->pokemonToday.nickname, sizeof(show->pokemonToday.nickname));
    show->pokemonToday.ball = ITEM_POKE_BALL;
    show->pokemonToday.species = SPECIES_MUDKIP;
    SetName(show->pokemonToday.playerName, sizeof(show->pokemonToday.playerName));
    show = &r->tvShows[3];
    show->common.kind = TVSHOW_SECRET_BASE_VISIT;
    show->secretBaseVisit.numDecorations = 2;
    show->secretBaseVisit.decorations[0] = DECOR_SMALL_DESK;
    show->secretBaseVisit.decorations[1] = DECOR_POKEMON_DESK;
    show->secretBaseVisit.species = SPECIES_TREECKO;
    show->secretBaseVisit.move = MOVE_POUND;
    SetName(show->secretBaseVisit.playerName, sizeof(show->secretBaseVisit.playerName));
    show = &r->tvShows[4];
    show->common.kind = TVSHOW_BATTLE_SEMINAR;
    show->battleSeminar.move = MOVE_POUND;
    show->battleSeminar.foeSpecies = SPECIES_ZIGZAGOON;
    show->battleSeminar.species = SPECIES_TREECKO;
    show->battleSeminar.otherMoves[0] = MOVE_SCRATCH;
    show->battleSeminar.otherMoves[1] = MOVE_LEER;
    show->battleSeminar.otherMoves[2] = MOVE_ABSORB;
    show->battleSeminar.betterMove = MOVE_ABSORB;
    show->battleSeminar.nOtherMoves = 3;
    SetName(show->battleSeminar.playerName, sizeof(show->battleSeminar.playerName));
    show = &r->tvShows[5];
    show->common.kind = TVSHOW_MASS_OUTBREAK;
    show->massOutbreak.moves[0] = MOVE_POUND;
    show->massOutbreak.species = SPECIES_SEEDOT;
    show->massOutbreak.level = 13;
    show = &r->tvShows[6];
    show->common.kind = TVSHOW_TREND_WATCHER;
    SetWords(show->trendWatcher.words, ARRAY_COUNT(show->trendWatcher.words));
    SetName(show->trendWatcher.playerName, sizeof(show->trendWatcher.playerName));
    show = &r->tvShows[7];
    show->common.kind = TVSHOW_SMART_SHOPPER;
    show->smartshopperShow.itemIds[0] = ITEM_POTION;
    show->smartshopperShow.itemIds[1] = ITEM_ANTIDOTE;
    SetName(show->smartshopperShow.playerName, sizeof(show->smartshopperShow.playerName));
    show = &r->tvShows[8];
    show->common.kind = TVSHOW_CONTEST_LIVE_UPDATES;
    show->contestLiveUpdates.losingSpecies = SPECIES_TREECKO;
    show->contestLiveUpdates.winningSpecies = SPECIES_TORCHIC;
    SetName(show->contestLiveUpdates.losingTrainerName, sizeof(show->contestLiveUpdates.losingTrainerName));
    SetName(show->contestLiveUpdates.winningTrainerName, sizeof(show->contestLiveUpdates.winningTrainerName));
    show->contestLiveUpdates.move = MOVE_POUND;
    show->contestLiveUpdates.category = CONTEST_CATEGORY_TOUGH;
    show = &r->tvShows[9];
    show->common.kind = TVSHOW_SECRET_BASE_SECRETS;
    SetName(show->secretBaseSecrets.baseOwnersName, sizeof(show->secretBaseSecrets.baseOwnersName));
    show->secretBaseSecrets.item = ITEM_POTION;
    SetName(show->secretBaseSecrets.playerName, sizeof(show->secretBaseSecrets.playerName));

    r->pokeNews[0].kind = POKENEWS_SLATEPORT;
    r->pokeNews[0].state = POKENEWS_STATE_UPCOMING;
    r->pokeNews[0].dayCountdown = 3;

    r->oldMan.storyteller.id = MAUVILLE_MAN_STORYTELLER;
    r->oldMan.storyteller.gameStatIDs[0] = 50;  // the storyteller's alias of GAME_STAT_SAVED_GAME
    r->oldMan.storyteller.gameStatIDs[1] = GAME_STAT_SAVED_GAME + 1;
    memcpy(r->oldMan.storyteller.trainerNames[0], "\xBB\xCD\xC2\xFF\xFF\xFF\xFF", PLAYER_NAME_LENGTH);

    for (i = 0; i < SAVED_TRENDS_COUNT; i++)
    {
        r->dewfordTrends[i].words[0] = EC_WORD(EC_GROUP_CONDITIONS, i);
        r->dewfordTrends[i].words[1] = EC_WORD(EC_GROUP_LIFESTYLE, i);
    }

    r->daycareMail.numDaycareMons = 1;
    r->daycareMail.mail[0].message.itemId = ITEM_ORANGE_MAIL;
    SetWords(r->daycareMail.mail[0].message.words, MAIL_WORDS_COUNT);
    SetName(r->daycareMail.mail[0].message.playerName, sizeof(r->daycareMail.mail[0].message.playerName));
    r->daycareMail.mail[0].message.species = SPECIES_TREECKO;
    SetName(r->daycareMail.mail[0].otName, sizeof(r->daycareMail.mail[0].otName));
    SetName(r->daycareMail.mail[0].monName, sizeof(r->daycareMail.mail[0].monName));

    r->battleTowerRecord.lvlMode = FRONTIER_LVL_OPEN;
    r->battleTowerRecord.facilityClass = FACILITY_CLASSES_COUNT - 1;
    r->battleTowerRecord.winStreak = 7;
    SetName(r->battleTowerRecord.name, sizeof(r->battleTowerRecord.name));
    SetWords(r->battleTowerRecord.greeting, EASY_CHAT_BATTLE_WORDS_COUNT);
    SetWords(r->battleTowerRecord.speechWon, EASY_CHAT_BATTLE_WORDS_COUNT);
    SetWords(r->battleTowerRecord.speechLost, EASY_CHAT_BATTLE_WORDS_COUNT);
    r->battleTowerRecord.party[0].species = SPECIES_TREECKO;
    r->battleTowerRecord.party[0].heldItem = ITEM_POTION;
    r->battleTowerRecord.party[0].moves[0] = MOVE_POUND;
    r->battleTowerRecord.party[0].level = MAX_LEVEL;
    SetName(r->battleTowerRecord.party[0].nickname, sizeof(r->battleTowerRecord.party[0].nickname));

    r->giftItem = ITEM_EON_TICKET;

    r->lilycoveLady.quiz.id = LILYCOVE_LADY_QUIZ;
    SetWords(r->lilycoveLady.quiz.question, QUIZ_QUESTION_LEN);
    r->lilycoveLady.quiz.correctAnswer = GOOD_WORD;
    r->lilycoveLady.quiz.playerAnswer = EC_EMPTY_WORD;
    SetName(r->lilycoveLady.quiz.playerName, sizeof(r->lilycoveLady.quiz.playerName));
    r->lilycoveLady.quiz.prize = ITEM_POTION;
    r->lilycoveLady.quiz.questionId = 3;
    r->lilycoveLady.quiz.prevQuestionId = 16; // "none"

    memcpy(r->apprentices[0].playerName, "\xBB\xCD\xC2\xFF\xFF\xFF\xFF", PLAYER_NAME_LENGTH);
    r->apprentices[0].id = 3;
    r->apprentices[0].lvlMode = APPRENTICE_LVL_MODE_50;
    r->apprentices[0].numQuestions = 5;
    r->apprentices[0].party[0].species = SPECIES_TREECKO;
    r->apprentices[0].party[0].moves[0] = MOVE_POUND;
    r->apprentices[0].party[0].item = ITEM_POTION;
    SetWords(r->apprentices[0].speechWon, EASY_CHAT_BATTLE_WORDS_COUNT);
    // Unused slot, as GetSavedApprentices leaves it: EOS name, everything else never set.
    r->apprentices[1].playerName[0] = EOS;
    r->apprentices[1].id = 31;
    r->apprentices[1].lvlMode = 3;
    r->apprentices[1].party[0].species = 0xFFFF;

    for (i = 0; i < HALL_FACILITIES_COUNT; i++)
        for (j = 0; j < FRONTIER_LVL_MODE_COUNT; j++)
            SetName(r->hallRecords.onePlayer[i][j].name, sizeof(r->hallRecords.onePlayer[i][j].name));
    for (j = 0; j < FRONTIER_LVL_MODE_COUNT; j++)
    {
        SetName(r->hallRecords.twoPlayers[j].name1, sizeof(r->hallRecords.twoPlayers[j].name1));
        SetName(r->hallRecords.twoPlayers[j].name2, sizeof(r->hallRecords.twoPlayers[j].name2));
    }
}

// sRec has one bad field: expect `code` / `detail`, then restore the clean record.
static void Expect(const char *what, u8 code, u16 detail)
{
    u16 got = 0xFFFF;
    u8 res = MP_MixValidateRecord(&sRec, TRUE, &got);

    if (res != code || got != detail)
    {
        MP_LOG("selftest: FAIL: mix: %s: got %s (%u) index %u, expected %s (%u) index %u\n", what,
               MP_MixRejectName(res), (unsigned)res, (unsigned)got, MP_MixRejectName(code), (unsigned)code, (unsigned)detail);
        sFail++;
    }
    MakeCleanRecord(&sRec);
}

static void TestSanitizer(void)
{
    u32 failuresBefore = sFail;
    u32 i;
    u16 detail;

    X_CHECK(sizeof(struct PlayerRecordEmerald) == PLAYER_RECORD_EMERALD_SIZE, "record size 0x1444");
    X_CHECK(MP_MixValidateRecord(NULL, TRUE, &detail) != MP_MIX_OK, "NULL record rejected");

    memset(&sRec, 0, sizeof(sRec));
    X_CHECK(MP_MixValidateRecord(&sRec, TRUE, &detail) == MP_MIX_OK, "all-zero record (a fresh save) passes");
    X_CHECK(MP_MixValidateRecord(&sRec, FALSE, NULL) == MP_MIX_OK, "all-zero record passes, no detail");

    MakeCleanRecord(&sRec);
    X_CHECK(MP_MixValidateRecord(&sRec, TRUE, &detail) == MP_MIX_OK && detail == 0, "clean record passes (master)");
    X_CHECK(MP_MixValidateRecord(&sRec, FALSE, &detail) == MP_MIX_OK, "clean record passes (non-master)");

    // Secret bases.
    sRec.secretBases[2].secretBaseId = NUM_SECRET_BASE_GROUPS * 10;
    Expect("secret base id out of the entrance table", MP_MIX_BAD_SECRET_BASE, 2);
    sRec.secretBases[0].decorations[5] = NUM_DECORATIONS + 1;
    Expect("secret base decoration id", MP_MIX_BAD_SECRET_BASE, 0);
    sRec.secretBases[1].party.species[0] = NUM_SPECIES;
    Expect("secret base party species", MP_MIX_BAD_SECRET_BASE, 1);
    sRec.secretBases[1].party.moves[PARTY_SIZE * MAX_MON_MOVES - 1] = MOVES_COUNT;
    Expect("secret base party move", MP_MIX_BAD_SECRET_BASE, 1);
    sRec.secretBases[1].party.heldItems[3] = ITEMS_COUNT;
    Expect("secret base party item", MP_MIX_BAD_SECRET_BASE, 1);
    sRec.secretBases[1].party.levels[2] = MAX_LEVEL + 1;
    Expect("secret base party level", MP_MIX_BAD_SECRET_BASE, 1);
    sRec.secretBases[3].secretBaseId = 239; // the last id with an entrance group
    sRec.secretBases[3].decorations[0] = DECOR_NONE;
    X_CHECK(MP_MixValidateRecord(&sRec, TRUE, &detail) == MP_MIX_OK, "secret base id 239 accepted");
    MakeCleanRecord(&sRec);

    // TV shows.
    sRec.tvShows[0].common.kind = TVGROUP_NORMAL_END - 1; // 19: a kind with no show behind it
    Expect("TV show kind without a struct", MP_MIX_BAD_TV_SHOW, 0);
    sRec.tvShows[0].common.kind = TVGROUP_OUTBREAK_END;
    Expect("TV show kind 60", MP_MIX_BAD_TV_SHOW, 0);
    sRec.tvShows[0].fanclubLetter.species = NUM_SPECIES;
    Expect("TV show species (SpeciesToNationalPokedexNum)", MP_MIX_BAD_TV_SHOW, 0);
    sRec.tvShows[0].fanclubLetter.words[3] = BAD_WORD;
    Expect("TV show easy chat word group", MP_MIX_BAD_TV_SHOW, 0);
    sRec.tvShows[1].bravoTrainer.contestCategory = CONTEST_CATEGORIES_COUNT;
    Expect("TV show contest category", MP_MIX_BAD_TV_SHOW, 1);
    sRec.tvShows[1].bravoTrainer.move = MOVES_COUNT;
    Expect("TV show move (gMoveNames)", MP_MIX_BAD_TV_SHOW, 1);
    sRec.tvShows[2].pokemonToday.ball = ITEMS_COUNT;
    Expect("TV show ball item", MP_MIX_BAD_TV_SHOW, 2);
    memset(sRec.tvShows[2].pokemonToday.playerName, BAD_NAME_BYTE, sizeof(sRec.tvShows[2].pokemonToday.playerName));
    Expect("TV show player name without EOS", MP_MIX_BAD_TV_SHOW, 2);
    memset(sRec.tvShows[2].pokemonToday.nickname, BAD_NAME_BYTE, sizeof(sRec.tvShows[2].pokemonToday.nickname));
    Expect("TV show nickname without EOS", MP_MIX_BAD_TV_SHOW, 2);
    sRec.tvShows[3].secretBaseVisit.numDecorations = 5;
    Expect("TV show decoration count", MP_MIX_BAD_TV_SHOW, 3);
    sRec.tvShows[3].secretBaseVisit.decorations[3] = NUM_DECORATIONS + 1;
    Expect("TV show decoration id (gDecorations)", MP_MIX_BAD_TV_SHOW, 3);
    sRec.tvShows[4].battleSeminar.nOtherMoves = 4;
    Expect("TV show seminar move count", MP_MIX_BAD_TV_SHOW, 4);
    sRec.tvShows[4].battleSeminar.otherMoves[2] = MOVES_COUNT;
    Expect("TV show seminar move", MP_MIX_BAD_TV_SHOW, 4);
    sRec.tvShows[5].massOutbreak.level = MAX_LEVEL + 1;
    Expect("outbreak level", MP_MIX_BAD_TV_SHOW, 5);
    sRec.tvShows[5].massOutbreak.moves[1] = MOVES_COUNT;
    Expect("outbreak move", MP_MIX_BAD_TV_SHOW, 5);
    sRec.tvShows[7].smartshopperShow.itemIds[2] = ITEMS_COUNT;
    Expect("smart shopper item", MP_MIX_BAD_TV_SHOW, 7);
    sRec.tvShows[8].contestLiveUpdates.category = CONTEST_CATEGORIES_COUNT;
    Expect("contest live updates category", MP_MIX_BAD_TV_SHOW, 8);
    sRec.tvShows[9].secretBaseSecrets.item = ITEMS_COUNT;
    Expect("secret base secrets item", MP_MIX_BAD_TV_SHOW, 9);
    sRec.tvShows[24].common.kind = TVSHOW_FRONTIER;
    sRec.tvShows[24].frontier.species3 = NUM_SPECIES;
    Expect("frontier show species (last slot)", MP_MIX_BAD_TV_SHOW, 24);

    // PokeNews.
    sRec.pokeNews[3].kind = POKENEWS_BLENDMASTER + 1;
    Expect("PokeNews kind", MP_MIX_BAD_POKE_NEWS, 3);
    sRec.pokeNews[15].state = POKENEWS_STATE_ACTIVE + 1;
    Expect("PokeNews state", MP_MIX_BAD_POKE_NEWS, 15);

    // Mauville old man. Each of the five men with an all-zero body passes; then one bad field each.
    for (i = MAUVILLE_MAN_BARD; i <= MAUVILLE_MAN_GIDDY; i++)
    {
        memset(&sRec.oldMan, 0, sizeof(sRec.oldMan));
        sRec.oldMan.common.id = (u8)i;
        X_CHECK(MP_MixValidateRecord(&sRec, TRUE, &detail) == MP_MIX_OK, "every old man with a zero body passes");
    }
    MakeCleanRecord(&sRec);
    sRec.oldMan.common.id = MAUVILLE_MAN_UNUSED1;
    Expect("old man id", MP_MIX_BAD_OLD_MAN, 0);
    sRec.oldMan.storyteller.gameStatIDs[2] = NUM_GAME_STATS;
    Expect("storyteller game stat id (GetGameStat)", MP_MIX_BAD_OLD_MAN, 0);
    memset(&sRec.oldMan, 0, sizeof(sRec.oldMan));
    sRec.oldMan.giddy.id = MAUVILLE_MAN_GIDDY;
    sRec.oldMan.giddy.taleCounter = GIDDY_MAX_TALES + 1;
    Expect("giddy tale counter", MP_MIX_BAD_OLD_MAN, 0);
    memset(&sRec.oldMan, 0, sizeof(sRec.oldMan));
    sRec.oldMan.giddy.id = MAUVILLE_MAN_GIDDY;
    sRec.oldMan.giddy.questionList[0] = GIDDY_MAX_QUESTIONS;
    Expect("giddy question list (sGiddyQuestions)", MP_MIX_BAD_OLD_MAN, 0);
    memset(&sRec.oldMan, 0, sizeof(sRec.oldMan));
    sRec.oldMan.giddy.id = MAUVILLE_MAN_GIDDY;
    sRec.oldMan.giddy.randomWords[GIDDY_MAX_TALES - 1] = BAD_WORD;
    Expect("giddy random word", MP_MIX_BAD_OLD_MAN, 0);
    memset(&sRec.oldMan, 0, sizeof(sRec.oldMan));
    sRec.oldMan.trader.id = MAUVILLE_MAN_TRADER;
    sRec.oldMan.trader.decorations[2] = NUM_DECORATIONS + 1;
    Expect("trader decoration (gDecorations)", MP_MIX_BAD_OLD_MAN, 0);
    memset(&sRec.oldMan, 0, sizeof(sRec.oldMan));
    sRec.oldMan.trader.id = MAUVILLE_MAN_TRADER;
    memset(sRec.oldMan.trader.playerNames[1], BAD_NAME_BYTE, sizeof(sRec.oldMan.trader.playerNames[1]));
    Expect("trader player name without EOS", MP_MIX_BAD_OLD_MAN, 0);
    memset(&sRec.oldMan, 0, sizeof(sRec.oldMan));
    sRec.oldMan.bard.id = MAUVILLE_MAN_BARD;
    sRec.oldMan.bard.songLyrics[5] = BAD_WORD;
    Expect("bard lyric word", MP_MIX_BAD_OLD_MAN, 0);
    memset(&sRec.oldMan, 0, sizeof(sRec.oldMan));
    sRec.oldMan.bard.id = MAUVILLE_MAN_BARD;
    memset(sRec.oldMan.bard.playerName, BAD_NAME_BYTE, sizeof(sRec.oldMan.bard.playerName));
    Expect("bard player name without EOS", MP_MIX_BAD_OLD_MAN, 0);

    // Dewford trends.
    sRec.dewfordTrends[4].words[1] = BAD_WORD;
    Expect("trend word group", MP_MIX_BAD_TREND, 4);

    // Daycare mail.
    sRec.daycareMail.numDaycareMons = DAYCARE_MON_COUNT + 1;
    Expect("daycare mon count (indexes mail[] / canHoldItem[])", MP_MIX_BAD_DAYCARE_MAIL, 0);
    sRec.daycareMail.mail[1].message.itemId = ITEM_POTION;
    Expect("daycare mail item that is not a mail (GiveMailToMon held item)", MP_MIX_BAD_DAYCARE_MAIL, 1);
    sRec.daycareMail.mail[0].message.itemId = ITEMS_COUNT;
    Expect("daycare mail item out of range", MP_MIX_BAD_DAYCARE_MAIL, 0);
    sRec.daycareMail.mail[0].message.species = 40000;
    Expect("daycare mail species (icon)", MP_MIX_BAD_DAYCARE_MAIL, 0);
    sRec.daycareMail.mail[0].message.species = 30000 + 5; // UNOWN_OFFSET + letter
    X_CHECK(MP_MixValidateRecord(&sRec, TRUE, &detail) == MP_MIX_OK, "daycare mail unown species accepted");
    MakeCleanRecord(&sRec);
    sRec.daycareMail.mail[0].message.words[8] = BAD_WORD;
    Expect("daycare mail word", MP_MIX_BAD_DAYCARE_MAIL, 0);
    memset(sRec.daycareMail.mail[0].monName, BAD_NAME_BYTE, sizeof(sRec.daycareMail.mail[0].monName));
    Expect("daycare mail mon name without EOS", MP_MIX_BAD_DAYCARE_MAIL, 0);
    sRec.daycareMail.mail[1].message.itemId = ITEM_NONE;
    memset(sRec.daycareMail.mail[1].otName, BAD_NAME_BYTE, sizeof(sRec.daycareMail.mail[1].otName));
    X_CHECK(MP_MixValidateRecord(&sRec, TRUE, &detail) == MP_MIX_OK, "a mail without an item is not read: names unchecked");
    MakeCleanRecord(&sRec);

    // Battle Tower record.
    sRec.battleTowerRecord.lvlMode = FRONTIER_LVL_OPEN + 1;
    Expect("tower record level mode", MP_MIX_BAD_TOWER_RECORD, 0);
    sRec.battleTowerRecord.facilityClass = FACILITY_CLASSES_COUNT;
    Expect("tower record facility class (gFacilityClassToPicIndex)", MP_MIX_BAD_TOWER_RECORD, 0);
    memset(sRec.battleTowerRecord.name, BAD_NAME_BYTE, sizeof(sRec.battleTowerRecord.name));
    Expect("tower record name without EOS", MP_MIX_BAD_TOWER_RECORD, 0);
    sRec.battleTowerRecord.greeting[0] = BAD_WORD;
    Expect("tower record greeting word", MP_MIX_BAD_TOWER_RECORD, 0);
    sRec.battleTowerRecord.party[0].species = NUM_SPECIES;
    Expect("tower mon species (CreateMon)", MP_MIX_BAD_TOWER_RECORD, 0);
    sRec.battleTowerRecord.party[0].moves[3] = MOVES_COUNT;
    Expect("tower mon move (SetMonMoveSlot)", MP_MIX_BAD_TOWER_RECORD, 0);
    sRec.battleTowerRecord.party[0].heldItem = ITEMS_COUNT;
    Expect("tower mon item", MP_MIX_BAD_TOWER_RECORD, 0);
    sRec.battleTowerRecord.party[0].level = MAX_LEVEL + 1;
    Expect("tower mon level", MP_MIX_BAD_TOWER_RECORD, 0);
    memset(sRec.battleTowerRecord.party[0].nickname, BAD_NAME_BYTE, sizeof(sRec.battleTowerRecord.party[0].nickname));
    Expect("tower mon nickname without EOS (StringCopy)", MP_MIX_BAD_TOWER_RECORD, 0);
    sRec.battleTowerRecord.party[1].species = SPECIES_NONE;
    sRec.battleTowerRecord.party[1].moves[0] = MOVES_COUNT; // an empty slot is never used
    X_CHECK(MP_MixValidateRecord(&sRec, TRUE, &detail) == MP_MIX_OK, "empty tower slot unchecked");
    MakeCleanRecord(&sRec);

    // Gift item: only the link master's is read.
    sRec.giftItem = ITEMS_COUNT;
    X_CHECK(MP_MixValidateRecord(&sRec, FALSE, &detail) == MP_MIX_OK, "non-master gift item ignored (unset by the sender)");
    Expect("master gift item", MP_MIX_BAD_GIFT_ITEM, 0);

    // Lilycove lady.
    sRec.lilycoveLady.id = LILYCOVE_LADY_COUNT;
    Expect("lady id (sLilycoveLadyGfxId)", MP_MIX_BAD_LILYCOVE_LADY, 0);
    sRec.lilycoveLady.quiz.questionId = 16;
    Expect("quiz question id (sQuizLadyQuizQuestions)", MP_MIX_BAD_LILYCOVE_LADY, 0);
    sRec.lilycoveLady.quiz.prevQuestionId = 17;
    Expect("quiz previous question id", MP_MIX_BAD_LILYCOVE_LADY, 0);
    sRec.lilycoveLady.quiz.prize = ITEMS_COUNT;
    Expect("quiz prize item", MP_MIX_BAD_LILYCOVE_LADY, 0);
    sRec.lilycoveLady.quiz.question[QUIZ_QUESTION_LEN - 1] = BAD_WORD;
    Expect("quiz question word", MP_MIX_BAD_LILYCOVE_LADY, 0);
    sRec.lilycoveLady.quiz.correctAnswer = BAD_WORD;
    Expect("quiz answer word", MP_MIX_BAD_LILYCOVE_LADY, 0);
    memset(&sRec.lilycoveLady, 0, sizeof(sRec.lilycoveLady));
    sRec.lilycoveLady.favor.id = LILYCOVE_LADY_FAVOR;
    sRec.lilycoveLady.favor.favorId = 6;
    Expect("favor id (sFavorLadyRequests)", MP_MIX_BAD_LILYCOVE_LADY, 0);
    memset(&sRec.lilycoveLady, 0, sizeof(sRec.lilycoveLady));
    sRec.lilycoveLady.favor.id = LILYCOVE_LADY_FAVOR;
    sRec.lilycoveLady.favor.bestItem = ITEMS_COUNT;
    Expect("favor best item", MP_MIX_BAD_LILYCOVE_LADY, 0);
    memset(&sRec.lilycoveLady, 0, sizeof(sRec.lilycoveLady));
    sRec.lilycoveLady.contest.id = LILYCOVE_LADY_CONTEST;
    sRec.lilycoveLady.contest.category = CONTEST_CATEGORIES_COUNT;
    Expect("contest lady category (sContestLadyMonGfxId)", MP_MIX_BAD_LILYCOVE_LADY, 0);
    memset(&sRec.lilycoveLady, 0, sizeof(sRec.lilycoveLady));
    sRec.lilycoveLady.contest.id = LILYCOVE_LADY_CONTEST;
    sRec.lilycoveLady.contest.category = CONTEST_CATEGORY_TOUGH;
    X_CHECK(MP_MixValidateRecord(&sRec, TRUE, &detail) == MP_MIX_OK, "contest lady accepted");
    MakeCleanRecord(&sRec);

    // Apprentices.
    sRec.apprentices[0].id = NUM_APPRENTICES;
    Expect("apprentice id (gApprentices)", MP_MIX_BAD_APPRENTICE, 0);
    sRec.apprentices[0].lvlMode = APPRENTICE_LVL_MODE_OPEN + 1;
    Expect("apprentice level mode", MP_MIX_BAD_APPRENTICE, 0);
    sRec.apprentices[0].numQuestions = APPRENTICE_MAX_QUESTIONS + 1;
    Expect("apprentice question count (sApprenticeChallengeThreshold)", MP_MIX_BAD_APPRENTICE, 0);
    sRec.apprentices[0].party[2].species = NUM_SPECIES;
    Expect("apprentice mon species", MP_MIX_BAD_APPRENTICE, 0);
    sRec.apprentices[0].party[1].moves[3] = MOVES_COUNT;
    Expect("apprentice mon move", MP_MIX_BAD_APPRENTICE, 0);
    sRec.apprentices[0].party[0].item = ITEMS_COUNT;
    Expect("apprentice mon item", MP_MIX_BAD_APPRENTICE, 0);
    sRec.apprentices[0].speechWon[5] = BAD_WORD;
    Expect("apprentice speech word", MP_MIX_BAD_APPRENTICE, 0);
    sRec.apprentices[1].playerName[0] = CHAR_B; // now in use: its garbage counts
    Expect("second apprentice in use with a bad id", MP_MIX_BAD_APPRENTICE, 1);

    // Ranking Hall records.
    memset(sRec.hallRecords.onePlayer[2][1].name, BAD_NAME_BYTE, sizeof(sRec.hallRecords.onePlayer[2][1].name));
    Expect("hall record name without EOS", MP_MIX_BAD_HALL_RECORD, 2 * FRONTIER_LVL_MODE_COUNT + 1);
    memset(sRec.hallRecords.twoPlayers[1].name2, BAD_NAME_BYTE, sizeof(sRec.hallRecords.twoPlayers[1].name2));
    Expect("2P hall record name without EOS", MP_MIX_BAD_HALL_RECORD, HALL_FACILITIES_COUNT * FRONTIER_LVL_MODE_COUNT + 1);

    // Names: an EOS anywhere inside, or all zero, is fine; the first bad part wins.
    memset(sRec.battleTowerRecord.name, 0, sizeof(sRec.battleTowerRecord.name));
    X_CHECK(MP_MixValidateRecord(&sRec, TRUE, &detail) == MP_MIX_OK, "all-zero name accepted (unused record)");
    MakeCleanRecord(&sRec);
    sRec.tvShows[6].trendWatcher.words[0] = BAD_WORD;
    sRec.giftItem = ITEMS_COUNT;
    Expect("the first bad part is reported", MP_MIX_BAD_TV_SHOW, 6);

    X_CHECK(strcmp(MP_MixRejectName(MP_MIX_OK), "ok") == 0 && strcmp(MP_MixRejectName(MP_MIX_REJECT_COUNT), "?") == 0
         && strcmp(MP_MixRejectName(MP_MIX_BAD_HALL_RECORD), "hall record") == 0, "reject names");
    Report("record sanitizer (clean, every crafted bad field)", failuresBefore);
}

// ===========================================================================
// 3. Fuzz
// ===========================================================================

static void TestFuzz(void)
{
    static struct PlayerRecordEmerald rec;
    u32 failuresBefore = sFail;
    u32 n, i, flips, accepted = 0;
    u16 d1, d2;
    u8 r1, r2;
    u8 *bytes = (u8 *)&rec;

    for (n = 0; n < 6000; n++)
    {
        MakeCleanRecord(&rec);
        flips = 1 + (Rand() % 12);
        for (i = 0; i < flips; i++)
        {
            u32 at = Rand() % sizeof(rec);

            if (Rand() & 1)
                bytes[at] = (u8)Rand();
            else
                bytes[at] ^= (u8)(1 << (Rand() % 8));
        }
        r1 = MP_MixValidateRecord(&rec, (n & 1) != 0, &d1);
        r2 = MP_MixValidateRecord(&rec, (n & 1) != 0, &d2);
        X_CHECK(r1 == r2 && d1 == d2, "fuzz: deterministic");
        X_CHECK(r1 < MP_MIX_REJECT_COUNT, "fuzz: code in range");
        if (r1 == MP_MIX_OK)
            accepted++;
    }
    MP_LOG("selftest: mix: fuzz: %u of 6000 mutated records still passed\n", (unsigned)accepted);
    // Whole-record garbage: nearly always rejected, never a crash.
    for (n = 0; n < 2000; n++)
    {
        for (i = 0; i < sizeof(rec); i++)
            bytes[i] = (u8)Rand();
        r1 = MP_MixValidateRecord(&rec, TRUE, &d1);
        X_CHECK(r1 < MP_MIX_REJECT_COUNT, "garbage: code in range");
    }
    Report("record sanitizer fuzz (mutations, garbage)", failuresBefore);
}

u32 MpSelftest_Mix(void)
{
    sFail = 0;
    MP_LOG("selftest: mix: PS-089 direct record mixing\n");
    TestInteractKind();
    TestSanitizer();
    TestFuzz();
    if (sFail == 0)
        MP_LOG("selftest: mix: all OK\n");
    else
        MP_LOG("selftest: mix: %u failure(s)\n", (unsigned)sFail);
    return sFail;
}
