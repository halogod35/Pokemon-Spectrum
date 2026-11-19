// Pokemon Spectrum multiplayer: the direct record mix sanitizer (PS-089, mp/mp_mix.h).
//
// Pure: reads the record and compile-time constants only, so --mp-selftest (mp_selftest_mix.c)
// can run it without a game. Called from record_mixing.c's Task_ReceivePacket ("// MP: PS-089")
// through MP_MixAcceptReceived (mp_mix.c) on every partner record before ReceiveExchangePacket.
//
// What the vanilla Receive* functions and the later display paths index with (audited for PS-089,
// see the card's Handoff): gSpeciesNames / gSpeciesInfo / SpeciesToNationalPokedexNum by species,
// gMoveNames / gBattleMoves by move, gItems by item (GetItemName clamps, GiveMailToMonByItemId and
// CreateMon do not), gDecorations by decoration id, sSecretBaseEntrancePositions by base id / 10,
// gFacilityClassToPicIndex / gFacilityClassToTrainerClass by facility class, gApprentices by
// apprentice id, sApprenticeChallengeThreshold by numQuestions, GetGameStat by the storyteller's
// stat ids, sGiddyQuestions by the question list, sQuizLadyQuizQuestions by questionId,
// sFavorLadyRequests by favorId, sContestLadyMonGfxId by category, sLilycoveLadyGfxId by id,
// mail[j] / canHoldItem[i][j] by numDaycareMons; and StringCopy over the names and nicknames.

#include <string.h>
#include "global.h"
#include "record_mixing.h"
#include "constants/apprentice.h"
#include "constants/battle_frontier.h"
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
#include "mp/mp_mix.h"

// The wire layout record_mixing.c sends in 200-byte chunks (include/record_mixing.h).
typedef char MpMixRecordSizeCheck[(sizeof(struct PlayerRecordEmerald) == PLAYER_RECORD_EMERALD_SIZE) ? 1 : -1];

// Table sizes that live in static data the sanitizer cannot reference (src/data/lilycove_lady.h,
// src/mail_data.c). Checked against the real tables by the selftest where possible.
#define MP_MIX_QUIZ_QUESTION_COUNT   16     // sQuizLadyQuizQuestions (prevQuestionId == 16 means "none")
#define MP_MIX_FAVOR_REQUEST_COUNT   6      // sFavorLadyRequests / sFavorLadyAcceptedItemLists / sFavorLadyPrizes
#define MP_MIX_MAIL_UNOWN_OFFSET     30000  // mail_data.c UNOWN_OFFSET: a mail's species may be UNOWN_OFFSET + letter

#define STR_OK(field)      StrOk((field), sizeof(field))
#define EC_LIST_OK(field)  EcListOk((field), ARRAY_COUNT(field))

// A string the game prints with StringCopy: an EOS inside the buffer, or an all-zero buffer (an
// unused record in a real save, e.g. a Battle Tower record that was never written).
static bool8 StrOk(const u8 *s, u32 size)
{
    u32 i;
    bool8 allZero = TRUE;

    for (i = 0; i < size; i++)
    {
        if (s[i] == EOS)
            return TRUE;
        if (s[i] != 0)
            allZero = FALSE;
    }
    return allZero;
}

static bool8 EcOk(u16 word)
{
    return word == EC_EMPTY_WORD || EC_GROUP(word) < EC_NUM_GROUPS;
}

static bool8 EcListOk(const u16 *words, u32 count)
{
    u32 i;

    for (i = 0; i < count; i++)
    {
        if (!EcOk(words[i]))
            return FALSE;
    }
    return TRUE;
}

static bool8 SpeciesOk(u16 species)
{
    return species < NUM_SPECIES;
}

static bool8 MoveOk(u16 move)
{
    return move < MOVES_COUNT;
}

static bool8 MoveListOk(const u16 *moves, u32 count)
{
    u32 i;

    for (i = 0; i < count; i++)
    {
        if (!MoveOk(moves[i]))
            return FALSE;
    }
    return TRUE;
}

static bool8 ItemOk(u16 item)
{
    return item < ITEMS_COUNT;
}

static bool8 DecorOk(u8 decor)
{
    return decor <= NUM_DECORATIONS; // gDecorations has NUM_DECORATIONS + 1 entries (DECOR_NONE first)
}

// A daycare mail's item becomes the mon's held item on withdrawal (GiveMailToMon, no check there).
static bool8 MailItemOk(u16 item)
{
    return item == ITEM_NONE || (item >= FIRST_MAIL_INDEX && item <= ITEM_RETRO_MAIL);
}

static bool8 MailSpeciesOk(u16 species)
{
    return species < NUM_SPECIES
        || (species >= MP_MIX_MAIL_UNOWN_OFFSET && species < MP_MIX_MAIL_UNOWN_OFFSET + NUM_UNOWN_FORMS);
}

// ---------------------------------------------------------------------------
// The parts
// ---------------------------------------------------------------------------

static bool8 SecretBaseOk(const struct SecretBase *base)
{
    u32 i;

    if (base->secretBaseId == 0)
        return TRUE; // empty slot
    // SECRET_BASE_ID_TO_GROUP(id) = (id / 10) * 4 indexes sSecretBaseEntrancePositions[NUM_SECRET_BASE_GROUPS * 4].
    if (base->secretBaseId / 10 >= NUM_SECRET_BASE_GROUPS)
        return FALSE;
    for (i = 0; i < DECOR_MAX_SECRET_BASE; i++)
    {
        if (!DecorOk(base->decorations[i]))
            return FALSE;
    }
    for (i = 0; i < PARTY_SIZE; i++)
    {
        if (!SpeciesOk(base->party.species[i]) || !ItemOk(base->party.heldItems[i]) || base->party.levels[i] > MAX_LEVEL)
            return FALSE;
    }
    return MoveListOk(base->party.moves, PARTY_SIZE * MAX_MON_MOVES);
}

static bool8 TvShowOk(const TVShow *show)
{
    switch (show->common.kind)
    {
    case TVSHOW_OFF_AIR:
        return TRUE;
    case TVSHOW_FAN_CLUB_LETTER:
        return SpeciesOk(show->fanclubLetter.species) && EC_LIST_OK(show->fanclubLetter.words) && STR_OK(show->fanclubLetter.playerName);
    case TVSHOW_RECENT_HAPPENINGS:
        return SpeciesOk(show->recentHappenings.species) && EC_LIST_OK(show->recentHappenings.words) && STR_OK(show->recentHappenings.playerName);
    case TVSHOW_PKMN_FAN_CLUB_OPINIONS:
        return SpeciesOk(show->fanclubOpinions.species) && STR_OK(show->fanclubOpinions.playerName) && STR_OK(show->fanclubOpinions.nickname)
            && EC_LIST_OK(show->fanclubOpinions.words18) && EC_LIST_OK(show->fanclubOpinions.words);
    case TVSHOW_DUMMY:
        return SpeciesOk(show->dummy.species) && EC_LIST_OK(show->dummy.words) && STR_OK(show->dummy.name);
    case TVSHOW_NAME_RATER_SHOW:
        return SpeciesOk(show->nameRaterShow.species) && SpeciesOk(show->nameRaterShow.randomSpecies)
            && STR_OK(show->nameRaterShow.pokemonName) && STR_OK(show->nameRaterShow.trainerName);
    case TVSHOW_BRAVO_TRAINER_POKEMON_PROFILE:
        return SpeciesOk(show->bravoTrainer.species) && EC_LIST_OK(show->bravoTrainer.words) && STR_OK(show->bravoTrainer.pokemonNickname)
            && show->bravoTrainer.contestCategory < CONTEST_CATEGORIES_COUNT && MoveOk(show->bravoTrainer.move) && STR_OK(show->bravoTrainer.playerName);
    case TVSHOW_BRAVO_TRAINER_BATTLE_TOWER_PROFILE:
        return STR_OK(show->bravoTrainerTower.playerName) && SpeciesOk(show->bravoTrainerTower.species) && STR_OK(show->bravoTrainerTower.opponentName)
            && SpeciesOk(show->bravoTrainerTower.defeatedSpecies) && EC_LIST_OK(show->bravoTrainerTower.words);
    case TVSHOW_CONTEST_LIVE_UPDATES:
        return SpeciesOk(show->contestLiveUpdates.losingSpecies) && SpeciesOk(show->contestLiveUpdates.winningSpecies)
            && STR_OK(show->contestLiveUpdates.losingTrainerName) && STR_OK(show->contestLiveUpdates.winningTrainerName)
            && MoveOk(show->contestLiveUpdates.move) && show->contestLiveUpdates.category < CONTEST_CATEGORIES_COUNT;
    case TVSHOW_3_CHEERS_FOR_POKEBLOCKS:
        return STR_OK(show->threeCheers.worstBlenderName) && STR_OK(show->threeCheers.playerName);
    case TVSHOW_BATTLE_UPDATE:
        return SpeciesOk(show->battleUpdate.speciesOpponent) && SpeciesOk(show->battleUpdate.speciesPlayer) && MoveOk(show->battleUpdate.move)
            && STR_OK(show->battleUpdate.playerName) && STR_OK(show->battleUpdate.linkOpponentName);
    case TVSHOW_FAN_CLUB_SPECIAL:
        return STR_OK(show->fanClubSpecial.playerName) && STR_OK(show->fanClubSpecial.idolName) && EC_LIST_OK(show->fanClubSpecial.words);
    case TVSHOW_LILYCOVE_CONTEST_LADY:
        return STR_OK(show->contestLady.playerName) && show->contestLady.contestCategory < CONTEST_CATEGORIES_COUNT && STR_OK(show->contestLady.nickname);
    case TVSHOW_POKEMON_TODAY_CAUGHT:
        return STR_OK(show->pokemonToday.nickname) && ItemOk(show->pokemonToday.ball) && SpeciesOk(show->pokemonToday.species) && STR_OK(show->pokemonToday.playerName);
    case TVSHOW_SMART_SHOPPER:
        {
            u32 i;

            for (i = 0; i < SMARTSHOPPER_NUM_ITEMS; i++)
            {
                if (!ItemOk(show->smartshopperShow.itemIds[i]))
                    return FALSE;
            }
            return STR_OK(show->smartshopperShow.playerName);
        }
    case TVSHOW_POKEMON_TODAY_FAILED:
        return SpeciesOk(show->pokemonTodayFailed.species) && SpeciesOk(show->pokemonTodayFailed.species2) && STR_OK(show->pokemonTodayFailed.playerName);
    case TVSHOW_FISHING_ADVICE:
        return SpeciesOk(show->pokemonAngler.species) && STR_OK(show->pokemonAngler.playerName);
    case TVSHOW_WORLD_OF_MASTERS:
        return SpeciesOk(show->worldOfMasters.species) && STR_OK(show->worldOfMasters.playerName);
    case TVSHOW_TODAYS_RIVAL_TRAINER:
        return STR_OK(show->rivalTrainer.playerName);
    case TVSHOW_TREND_WATCHER:
        return EC_LIST_OK(show->trendWatcher.words) && STR_OK(show->trendWatcher.playerName);
    case TVSHOW_TREASURE_INVESTIGATORS:
        return ItemOk(show->treasureInvestigators.item) && STR_OK(show->treasureInvestigators.playerName);
    case TVSHOW_FIND_THAT_GAMER:
        return STR_OK(show->findThatGamer.playerName);
    case TVSHOW_BREAKING_NEWS:
        return SpeciesOk(show->breakingNews.lastOpponentSpecies) && SpeciesOk(show->breakingNews.poke1Species) && ItemOk(show->breakingNews.caughtMonBall)
            && MoveOk(show->breakingNews.lastUsedMove) && STR_OK(show->breakingNews.playerName);
    case TVSHOW_SECRET_BASE_VISIT:
        {
            u32 i;

            if (show->secretBaseVisit.numDecorations > ARRAY_COUNT(show->secretBaseVisit.decorations))
                return FALSE;
            for (i = 0; i < ARRAY_COUNT(show->secretBaseVisit.decorations); i++)
            {
                if (!DecorOk(show->secretBaseVisit.decorations[i]))
                    return FALSE;
            }
            return SpeciesOk(show->secretBaseVisit.species) && MoveOk(show->secretBaseVisit.move) && STR_OK(show->secretBaseVisit.playerName);
        }
    case TVSHOW_LOTTO_WINNER:
        return ItemOk(show->lottoWinner.item) && STR_OK(show->lottoWinner.playerName);
    case TVSHOW_BATTLE_SEMINAR:
        return MoveOk(show->battleSeminar.move) && SpeciesOk(show->battleSeminar.foeSpecies) && SpeciesOk(show->battleSeminar.species)
            && MoveListOk(show->battleSeminar.otherMoves, ARRAY_COUNT(show->battleSeminar.otherMoves)) && MoveOk(show->battleSeminar.betterMove)
            && show->battleSeminar.nOtherMoves <= ARRAY_COUNT(show->battleSeminar.otherMoves) && STR_OK(show->battleSeminar.playerName);
    case TVSHOW_TRAINER_FAN_CLUB:
        return EC_LIST_OK(show->trainerFanClub.words) && STR_OK(show->trainerFanClub.playerName);
    case TVSHOW_CUTIES:
        return STR_OK(show->cuties.nickname) && STR_OK(show->cuties.playerName);
    case TVSHOW_FRONTIER:
        return SpeciesOk(show->frontier.species1) && SpeciesOk(show->frontier.species2) && SpeciesOk(show->frontier.species3)
            && SpeciesOk(show->frontier.species4) && STR_OK(show->frontier.playerName);
    case TVSHOW_NUMBER_ONE:
        return STR_OK(show->numberOne.playerName);
    case TVSHOW_SECRET_BASE_SECRETS:
        return STR_OK(show->secretBaseSecrets.baseOwnersName) && ItemOk(show->secretBaseSecrets.item) && STR_OK(show->secretBaseSecrets.playerName);
    case TVSHOW_SAFARI_FAN_CLUB:
        return STR_OK(show->safariFanClub.playerName);
    case TVSHOW_MASS_OUTBREAK:
        return MoveListOk(show->massOutbreak.moves, MAX_MON_MOVES) && SpeciesOk(show->massOutbreak.species) && show->massOutbreak.level <= MAX_LEVEL;
    default:
        return FALSE; // 13..20, 40, 42..255: no show struct behind the kind
    }
}

static bool8 PokeNewsOk(const PokeNews *news)
{
    return news->kind <= POKENEWS_BLENDMASTER && news->state <= POKENEWS_STATE_ACTIVE;
}

static bool8 OldManOk(const OldMan *man)
{
    u32 i;

    switch (man->common.id)
    {
    case MAUVILLE_MAN_BARD:
        return EC_LIST_OK(man->bard.songLyrics) && EC_LIST_OK(man->bard.newSongLyrics) && STR_OK(man->bard.playerName);
    case MAUVILLE_MAN_HIPSTER:
        return TRUE;
    case MAUVILLE_MAN_TRADER:
        for (i = 0; i < NUM_TRADER_ITEMS; i++)
        {
            if (!DecorOk(man->trader.decorations[i]) || !STR_OK(man->trader.playerNames[i]))
                return FALSE;
        }
        return TRUE;
    case MAUVILLE_MAN_STORYTELLER:
        for (i = 0; i < NUM_STORYTELLER_TALES; i++)
        {
            if (man->storyteller.gameStatIDs[i] >= NUM_GAME_STATS) // GetGameStat indexes gSaveBlock1Ptr->gameStats (0 = no tale, 50 = SAVED_GAME)
                return FALSE;
        }
        return TRUE;
    case MAUVILLE_MAN_GIDDY:
        if (man->giddy.taleCounter > GIDDY_MAX_TALES || man->giddy.questionNum > GIDDY_MAX_QUESTIONS)
            return FALSE;
        for (i = 0; i < GIDDY_MAX_QUESTIONS; i++)
        {
            if (man->giddy.questionList[i] >= GIDDY_MAX_QUESTIONS)
                return FALSE;
        }
        return EC_LIST_OK(man->giddy.randomWords);
    default:
        return FALSE; // MAUVILLE_MAN_UNUSED1/2 and above are never set by the game
    }
}

static bool8 TrendOk(const struct DewfordTrend *trend)
{
    return EcOk(trend->words[0]) && EcOk(trend->words[1]);
}

// One daycare mail; the whole struct is swapped into our daycare and later given to the mon.
static bool8 DaycareMailOk(const struct DaycareMail *mail)
{
    if (!MailItemOk(mail->message.itemId))
        return FALSE;
    if (mail->message.itemId == ITEM_NONE)
        return TRUE; // no mail: the rest is not read
    return EC_LIST_OK(mail->message.words) && STR_OK(mail->message.playerName) && MailSpeciesOk(mail->message.species)
        && STR_OK(mail->otName) && STR_OK(mail->monName);
}

static bool8 TowerRecordOk(const struct EmeraldBattleTowerRecord *record)
{
    u32 i;

    if (record->lvlMode > FRONTIER_LVL_OPEN || record->facilityClass >= FACILITY_CLASSES_COUNT || !STR_OK(record->name))
        return FALSE;
    if (!EC_LIST_OK(record->greeting) || !EC_LIST_OK(record->speechWon) || !EC_LIST_OK(record->speechLost))
        return FALSE;
    for (i = 0; i < MAX_FRONTIER_PARTY_SIZE; i++)
    {
        const struct BattleTowerPokemon *mon = &record->party[i];

        if (!SpeciesOk(mon->species))
            return FALSE;
        if (mon->species == SPECIES_NONE)
            continue; // an empty slot is skipped by FillTrainerParty
        if (!ItemOk(mon->heldItem) || !MoveListOk(mon->moves, MAX_MON_MOVES) || mon->level > MAX_LEVEL || !STR_OK(mon->nickname))
            return FALSE;
    }
    return TRUE;
}

static bool8 LilycoveLadyOk(const LilycoveLady *lady)
{
    switch (lady->id)
    {
    case LILYCOVE_LADY_QUIZ:
        return EC_LIST_OK(lady->quiz.question) && EcOk(lady->quiz.correctAnswer) && EcOk(lady->quiz.playerAnswer)
            && STR_OK(lady->quiz.playerName) && ItemOk(lady->quiz.prize)
            && lady->quiz.questionId < MP_MIX_QUIZ_QUESTION_COUNT && lady->quiz.prevQuestionId <= MP_MIX_QUIZ_QUESTION_COUNT;
    case LILYCOVE_LADY_FAVOR:
        return lady->favor.favorId < MP_MIX_FAVOR_REQUEST_COUNT && ItemOk(lady->favor.itemId) && ItemOk(lady->favor.bestItem)
            && STR_OK(lady->favor.playerName);
    case LILYCOVE_LADY_CONTEST:
        return lady->contest.category < CONTEST_CATEGORIES_COUNT && STR_OK(lady->contest.playerName);
    default:
        return FALSE;
    }
}

static bool8 ApprenticeOk(const struct Apprentice *apprentice)
{
    u32 i;

    if (apprentice->playerName[0] == EOS)
        return TRUE; // unused slot (GetSavedApprentices marks it so and leaves the rest unset)
    if (apprentice->id >= NUM_APPRENTICES || apprentice->lvlMode > APPRENTICE_LVL_MODE_OPEN
     || apprentice->numQuestions > APPRENTICE_MAX_QUESTIONS)
        return FALSE;
    for (i = 0; i < MULTI_PARTY_SIZE; i++)
    {
        if (!SpeciesOk(apprentice->party[i].species) || !ItemOk(apprentice->party[i].item)
         || !MoveListOk(apprentice->party[i].moves, MAX_MON_MOVES))
            return FALSE;
    }
    return EC_LIST_OK(apprentice->speechWon);
}

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

u8 MP_MixValidateRecord(const struct PlayerRecordEmerald *record, bool8 fromLinkMaster, u16 *detail)
{
    u32 i, j;
    u16 unused;

    if (detail == NULL)
        detail = &unused;
    *detail = 0;
    if (record == NULL)
        return MP_MIX_BAD_SECRET_BASE;

    for (i = 0; i < SECRET_BASES_COUNT; i++)
    {
        if (!SecretBaseOk(&record->secretBases[i]))
        {
            *detail = i;
            return MP_MIX_BAD_SECRET_BASE;
        }
    }
    for (i = 0; i < TV_SHOWS_COUNT; i++)
    {
        if (!TvShowOk(&record->tvShows[i]))
        {
            *detail = i;
            return MP_MIX_BAD_TV_SHOW;
        }
    }
    for (i = 0; i < POKE_NEWS_COUNT; i++)
    {
        if (!PokeNewsOk(&record->pokeNews[i]))
        {
            *detail = i;
            return MP_MIX_BAD_POKE_NEWS;
        }
    }
    if (!OldManOk(&record->oldMan))
        return MP_MIX_BAD_OLD_MAN;
    for (i = 0; i < SAVED_TRENDS_COUNT; i++)
    {
        if (!TrendOk(&record->dewfordTrends[i]))
        {
            *detail = i;
            return MP_MIX_BAD_TREND;
        }
    }
    if (record->daycareMail.numDaycareMons > DAYCARE_MON_COUNT) // indexes mail[] and canHoldItem[][]
        return MP_MIX_BAD_DAYCARE_MAIL;
    for (i = 0; i < DAYCARE_MON_COUNT; i++)
    {
        if (!DaycareMailOk(&record->daycareMail.mail[i]))
        {
            *detail = i;
            return MP_MIX_BAD_DAYCARE_MAIL;
        }
    }
    if (!TowerRecordOk(&record->battleTowerRecord))
        return MP_MIX_BAD_TOWER_RECORD;
    if (fromLinkMaster && !ItemOk(record->giftItem))
        return MP_MIX_BAD_GIFT_ITEM;
    if (!LilycoveLadyOk(&record->lilycoveLady))
        return MP_MIX_BAD_LILYCOVE_LADY;
    for (i = 0; i < ARRAY_COUNT(record->apprentices); i++)
    {
        if (!ApprenticeOk(&record->apprentices[i]))
        {
            *detail = i;
            return MP_MIX_BAD_APPRENTICE;
        }
    }
    for (i = 0; i < HALL_FACILITIES_COUNT; i++)
    {
        for (j = 0; j < FRONTIER_LVL_MODE_COUNT; j++)
        {
            if (!STR_OK(record->hallRecords.onePlayer[i][j].name))
            {
                *detail = i * FRONTIER_LVL_MODE_COUNT + j;
                return MP_MIX_BAD_HALL_RECORD;
            }
        }
    }
    for (j = 0; j < FRONTIER_LVL_MODE_COUNT; j++)
    {
        if (!STR_OK(record->hallRecords.twoPlayers[j].name1) || !STR_OK(record->hallRecords.twoPlayers[j].name2))
        {
            *detail = HALL_FACILITIES_COUNT * FRONTIER_LVL_MODE_COUNT + j;
            return MP_MIX_BAD_HALL_RECORD;
        }
    }
    return MP_MIX_OK;
}

const char *MP_MixRejectName(u8 code)
{
    static const char *const sNames[MP_MIX_REJECT_COUNT] =
    {
        "ok", "secret base", "TV show", "PokeNews", "Mauville old man", "Dewford trend", "daycare mail",
        "Battle Tower record", "gift item", "Lilycove lady", "apprentice", "hall record",
    };

    return (code < MP_MIX_REJECT_COUNT) ? sNames[code] : "?";
}
