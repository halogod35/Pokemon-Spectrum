// Link receive-path hardening (PS-047, AD-9): validation of game structures that came from
// another game over a link. See include/mp/mp.h ("Link receive-path hardening") for the
// contract and docs/multiplayer/design.md ("PS-047") for where each helper is called.
//
// Everything here works on the data alone (no transport state), so the decomp consumers can
// call it whatever delivered the data. MP_LinkRejectData lives in mp_link_cable.c, next to
// the cable state it raises the error on.

#include "global.h"
#include "link.h"
#include "mail.h"
#include "pokemon.h"
#include "trainer_card.h"
#include "constants/abilities.h"
#include "constants/characters.h"
#include "constants/contest.h"
#include "constants/items.h"
#include "constants/moves.h"
#include "constants/pokemon.h"
#include "constants/species.h"
#include "mp/mp.h"

#define MP_MAX_CONTEST_RIBBON_COUNT (CONTEST_RANK_MASTER + 1) // Normal, Super, Hyper, Master

u8 MP_SanitizeLanguage(u8 language)
{
    if (language < LANGUAGE_JAPANESE || language > NUM_LANGUAGES)
        return LANGUAGE_ENGLISH;
    return language;
}

bool8 MP_ValidateMon(struct Pokemon *mon)
{
    // Work on a copy: GetMonData decrypts in place and marks a bad checksum as a Bad Egg.
    struct Pokemon copy = *mon;
    u32 species, value, i;
    u8 u8value;
    u16 u16value;

    species = GetMonData(&copy, MON_DATA_SPECIES); // decrypts, checks the checksum
    if (GetMonData(&copy, MON_DATA_SANITY_IS_BAD_EGG))
    {
        ZeroMonData(mon);
        return FALSE; // bad checksum (or a Bad Egg): nothing in it can be trusted
    }
    if (species == SPECIES_NONE)
    {
        ZeroMonData(mon); // an empty slot; make sure nothing else is left in it
        return TRUE;
    }
    if (species >= NUM_SPECIES)
    {
        ZeroMonData(mon);
        return FALSE;
    }
    if (GetMonData(&copy, MON_DATA_HELD_ITEM) >= ITEMS_COUNT)
    {
        ZeroMonData(mon);
        return FALSE;
    }
    for (i = 0; i < MAX_MON_MOVES; i++)
    {
        if (GetMonData(&copy, MON_DATA_MOVE1 + i) >= MOVES_COUNT)
        {
            ZeroMonData(mon);
            return FALSE;
        }
    }
    value = GetMonData(&copy, MON_DATA_LEVEL);
    if (value < 1 || value > MAX_LEVEL)
    {
        ZeroMonData(mon);
        return FALSE;
    }

    // Clamps: out of range but harmless once fixed.
    u8value = GetMonData(&copy, MON_DATA_LANGUAGE);
    if (MP_SanitizeLanguage(u8value) != u8value)
    {
        u8value = MP_SanitizeLanguage(u8value);
        SetMonData(mon, MON_DATA_LANGUAGE, &u8value);
    }
    u8value = GetMonData(&copy, MON_DATA_MAIL); // an index into the sender's mail (gTradeMail)
    if (u8value != MAIL_NONE && u8value >= PARTY_SIZE)
    {
        u8value = MAIL_NONE;
        SetMonData(mon, MON_DATA_MAIL, &u8value);
    }
    u16value = GetMonData(&copy, MON_DATA_MAX_HP);
    if (GetMonData(&copy, MON_DATA_HP) > u16value)
        SetMonData(mon, MON_DATA_HP, &u16value);
    for (i = MON_DATA_COOL_RIBBON; i <= MON_DATA_TOUGH_RIBBON; i++)
    {
        if (GetMonData(&copy, i) > MP_MAX_CONTEST_RIBBON_COUNT)
        {
            u8value = MP_MAX_CONTEST_RIBBON_COUNT;
            SetMonData(mon, i, &u8value); // encrypted field: SetMonData redoes the checksum
        }
    }
    return TRUE;
}

bool8 MP_ValidateParty(struct Pokemon *party, u32 count)
{
    bool8 ok = TRUE;
    u32 i;

    for (i = 0; i < count; i++)
    {
        if (!MP_ValidateMon(&party[i]))
            ok = FALSE;
    }
    return ok;
}

#define MP_CLAMP_FIELD(field, limit, fallback) \
    do                                          \
    {                                           \
        if ((field) >= (limit))                 \
        {                                       \
            (field) = (fallback);               \
            changed = TRUE;                     \
        }                                       \
    } while (0)

bool8 MP_SanitizeBattleMon(struct BattlePokemon *mon)
{
    bool8 changed = FALSE;
    u32 i;

    MP_CLAMP_FIELD(mon->species, NUM_SPECIES, SPECIES_NONE);
    for (i = 0; i < MAX_MON_MOVES; i++)
        MP_CLAMP_FIELD(mon->moves[i], MOVES_COUNT, MOVE_NONE);
    MP_CLAMP_FIELD(mon->item, ITEMS_COUNT, ITEM_NONE);

    // Silent clamps: honest games leave some of these unset in transit (the engine recomputes
    // ability and types from the species, and resets the stat stages), so they say nothing
    // about the peer, but nothing may index a table with them either.
    if (mon->ability >= ABILITIES_COUNT)
        mon->ability = ABILITY_NONE;
    if (mon->types[0] >= NUMBER_OF_MON_TYPES)
        mon->types[0] = TYPE_NORMAL;
    if (mon->types[1] >= NUMBER_OF_MON_TYPES)
        mon->types[1] = TYPE_NORMAL;
    if (mon->level > MAX_LEVEL)
        mon->level = MAX_LEVEL;
    for (i = 0; i < NUM_BATTLE_STATS; i++)
    {
        if (mon->statStages[i] < MIN_STAT_STAGE || mon->statStages[i] > MAX_STAT_STAGE)
            mon->statStages[i] = DEFAULT_STAT_STAGE;
    }
    if (mon->hp > mon->maxHP)
        mon->hp = mon->maxHP;
    mon->nickname[POKEMON_NAME_LENGTH] = EOS;
    mon->otName[PLAYER_NAME_LENGTH] = EOS;
    return !changed;
}

bool8 MP_SanitizeLinkPlayer(struct LinkPlayer *player)
{
    bool8 changed = FALSE;
    u8 language = MP_SanitizeLanguage(player->language);
    u32 i, maxLen;

    if (language != player->language)
    {
        player->language = language;
        changed = TRUE;
    }
    // ConvertInternationalString (via ConvertLinkPlayerName) wraps a Japanese name in 4
    // control bytes in place; the name field holds 8, and the bytes after it (progressFlags,
    // neverRead, progressFlagsCopy, gender) are what an over-long name would overwrite.
    // Japanese player names are at most 5 characters.
    maxLen = (language == LANGUAGE_JAPANESE) ? 5 : PLAYER_NAME_LENGTH;
    for (i = 0; i < maxLen && player->name[i] != EOS; i++)
        ;
    for (; i <= PLAYER_NAME_LENGTH; i++)
    {
        if (player->name[i] != EOS)
            changed = TRUE;
        player->name[i] = EOS;
    }
    MP_CLAMP_FIELD(player->gender, GENDER_COUNT, MALE);
    MP_CLAMP_FIELD(player->id, MAX_LINK_PLAYERS, 0);
    return !changed;
}

bool8 MP_SanitizeTrainerCard(struct TrainerCard *card)
{
    bool8 changed = FALSE;
    u32 i;

    MP_CLAMP_FIELD(card->stars, 5, 4);  // sHoennTrainerCardPals / sKantoTrainerCardPals: 5 entries
    MP_CLAMP_FIELD(card->gender, GENDER_COUNT, MALE);
    if (card->playerName[PLAYER_NAME_LENGTH] != EOS)
    {
        card->playerName[PLAYER_NAME_LENGTH] = EOS;
        changed = TRUE;
    }
    // FRLG-only fields, used when the card's version says FireRed / LeafGreen.
    for (i = 0; i < PARTY_SIZE; i++)
        MP_CLAMP_FIELD(card->monSpecies[i], NUM_SPECIES, SPECIES_NONE);
    for (i = 0; i < TRAINER_CARD_STICKER_TYPES; i++)
        MP_CLAMP_FIELD(card->stickers[i], 5, 0);  // 1..4 index paletteSlots[sticker - 1]
    return !changed;
}

bool8 MP_SanitizeMail(struct Mail *mail)
{
    bool8 changed = FALSE;

    if (mail->itemId != ITEM_NONE && !ItemIsMail(mail->itemId))
    {
        ClearMail(mail); // the mail screens index their tables with ITEM_TO_MAIL(itemId)
        return FALSE;
    }
    mail->playerName[PLAYER_NAME_LENGTH] = EOS; // silently: not a sign of a bad peer
    MP_CLAMP_FIELD(mail->species, NUM_SPECIES, SPECIES_NONE);
    // The easy chat words are checked by CopyEasyChatWord (IsEasyChatWordInvalid) when shown.
    return !changed;
}

#undef MP_CLAMP_FIELD
