// --mp-selftest, PS-042 "view another player" group:
//  1. TRAINER_CARD_DATA parser fuzzing (protocol.md 8.3): round trip, truncated, oversized,
//     every single-bit flip (deterministic, an OK result is always in range), 100 000 garbage
//     buffers from a logged seed, and targeted cases (gender, stars, name rules, clamps, easy
//     chat words, reserved bytes, slot / epoch).
//  2. Game-side checks (mp_playerdata.c, MpPlayerData_SelfTest): the card built from a test
//     save passes the parser and leaves the save untouched; the conversion to the game's
//     struct TrainerCard keeps every table index in range.
//  3. In-process host + 2 clients (session instances over loopback), with fixture cards:
//     client -> client (relayed), client -> host, host -> client, a card request while the
//     target has a trade prompt open (not busy), a tampered card (rejected: BAD_DATA, nothing
//     to show), a malformed card on the client receive path, and the requester leaving
//     mid-transfer (the host drops the late data silently, nobody is kicked).

#include <string.h>
#include "mp/mp.h"
#include "mp/mp_internal.h"
#include "mp/mp_protocol.h"
#include "mp/mp_selftest.h"
#include "mp/mp_session_internal.h"
#include "mp/mp_transport.h"
#include "constants/characters.h"
#include "constants/easy_chat.h"
#include "constants/maps.h"

static u32 sFail;

#define V_CHECK(cond, what)                                                         \
    do                                                                              \
    {                                                                               \
        if (!(cond))                                                                \
        {                                                                           \
            MP_LOG("selftest: FAIL: %s (%s:%d)\n", what, __FILE__, __LINE__);       \
            sFail++;                                                                \
        }                                                                           \
    } while (0)

static u32 sRng = 0x4D505643u; // "MPVC"

static u32 Rand(void)
{
    sRng ^= sRng << 13;
    sRng ^= sRng >> 17;
    sRng ^= sRng << 5;
    return sRng;
}

static void Report(const char *what, u32 failuresBefore)
{
    if (sFail == failuresBefore)
        MP_LOG("selftest: view: %s OK\n", what);
}

// ===========================================================================
// Fixtures
// ===========================================================================

static void FixtureCard(struct MpTrainerCard *c, u8 variant)
{
    u32 i;

    memset(c, 0, sizeof(*c));
    c->gender = variant & 1;
    c->stars = 1 + (variant % 4);
    c->hasPokedex = TRUE;
    c->caughtAllHoenn = (variant & 1) ? TRUE : FALSE;
    c->hasAllPaintings = FALSE;
    c->hasAllFrontierSymbols = (variant & 2) ? TRUE : FALSE;
    c->hofDebutHours = 12 + variant;
    c->hofDebutMinutes = 34;
    c->hofDebutSeconds = 56;
    c->caughtMonsCount = 150 + variant;
    c->trainerId = 0x3039 + variant;
    c->playTimeHours = 40 + variant;
    c->playTimeMinutes = 7;
    c->linkBattleWins = 3;
    c->linkBattleLosses = 2;
    c->battleTowerWins = 10;
    c->battleTowerStraightWins = 5;
    c->contestsWithFriends = 4;
    c->pokeblocksWithFriends = 300;
    c->pokemonTrades = 1000;
    c->frontierBP = 77;
    c->money = 123456 + variant;
    for (i = 0; i < MP_TRAINER_CARD_PROFILE_WORDS; i++)
        c->easyChatProfile[i] = (u16)EC_WORD(EC_GROUP_PEOPLE, i + variant);
    c->easyChatProfile[3] = MP_CARD_EC_EMPTY_WORD;
    memset(c->name, EOS, sizeof(c->name));
    c->name[0] = CHAR_C;
    c->name[1] = CHAR_A + variant;
    c->name[2] = CHAR_R;
    c->name[3] = CHAR_D;
}

// Field by field: struct copies need not preserve padding bytes.
static bool8 SameCard(const struct MpTrainerCard *a, const struct MpTrainerCard *b)
{
    return a->gender == b->gender && a->stars == b->stars && a->hasPokedex == b->hasPokedex
        && a->caughtAllHoenn == b->caughtAllHoenn && a->hasAllPaintings == b->hasAllPaintings
        && a->hasAllFrontierSymbols == b->hasAllFrontierSymbols && a->hofDebutHours == b->hofDebutHours
        && a->hofDebutMinutes == b->hofDebutMinutes && a->hofDebutSeconds == b->hofDebutSeconds
        && a->caughtMonsCount == b->caughtMonsCount && a->trainerId == b->trainerId
        && a->playTimeHours == b->playTimeHours && a->playTimeMinutes == b->playTimeMinutes
        && a->linkBattleWins == b->linkBattleWins && a->linkBattleLosses == b->linkBattleLosses
        && a->battleTowerWins == b->battleTowerWins && a->battleTowerStraightWins == b->battleTowerStraightWins
        && a->contestsWithFriends == b->contestsWithFriends && a->pokeblocksWithFriends == b->pokeblocksWithFriends
        && a->pokemonTrades == b->pokemonTrades && a->frontierBP == b->frontierBP && a->money == b->money
        && memcmp(a->easyChatProfile, b->easyChatProfile, sizeof(a->easyChatProfile)) == 0
        && memcmp(a->name, b->name, sizeof(a->name)) == 0;
}

static void FixtureCardMsg(struct MpMsgTrainerCardData *m)
{
    memset(m, 0, sizeof(*m));
    m->slot = 2;
    m->epoch = 9;
    m->requestId = 0x5A;
    FixtureCard(&m->card, 1);
}

// ===========================================================================
// 1. Parser
// ===========================================================================

// Payload offsets (protocol.md 6.12)
#define CARD_OFS_SLOT       0
#define CARD_OFS_EPOCH      1
#define CARD_OFS_RESERVED3  3
#define CARD_OFS_GENDER     4
#define CARD_OFS_STARS      5
#define CARD_OFS_FLAGS      6
#define CARD_OFS_HOF_HOURS  8
#define CARD_OFS_HOF_MIN    10
#define CARD_OFS_PLAY_MIN   18
#define CARD_OFS_WINS       20
#define CARD_OFS_MONEY      36
#define CARD_OFS_EC0        40
#define CARD_OFS_NAME       48

static bool8 CardInRange(const struct MpMsgTrainerCardData *m)
{
    const struct MpTrainerCard *c = &m->card;
    u32 i;

    if (m->slot >= MP_MAX_PLAYERS || m->epoch == 0 || c->gender > 1 || c->stars > MP_CARD_MAX_STARS)
        return FALSE;
    if (c->hofDebutHours > MP_CARD_MAX_HOURS || c->hofDebutMinutes > MP_CARD_MAX_MINUTES
     || c->hofDebutSeconds > MP_CARD_MAX_MINUTES || c->playTimeHours > MP_CARD_MAX_HOURS
     || c->playTimeMinutes > MP_CARD_MAX_MINUTES || c->caughtMonsCount > MP_CARD_MAX_COUNT_3
     || c->contestsWithFriends > MP_CARD_MAX_COUNT_3 || c->linkBattleWins > MP_CARD_MAX_COUNT_4
     || c->linkBattleLosses > MP_CARD_MAX_COUNT_4 || c->battleTowerWins > MP_CARD_MAX_COUNT_4
     || c->battleTowerStraightWins > MP_CARD_MAX_COUNT_4 || c->frontierBP > MP_CARD_MAX_COUNT_4
     || c->money > MP_CARD_MAX_MONEY)
        return FALSE;
    if (c->hasPokedex > 1 || c->caughtAllHoenn > 1 || c->hasAllPaintings > 1 || c->hasAllFrontierSymbols > 1)
        return FALSE;
    for (i = 0; i < MP_TRAINER_CARD_PROFILE_WORDS; i++)
    {
        if (c->easyChatProfile[i] != MP_CARD_EC_EMPTY_WORD && EC_GROUP(c->easyChatProfile[i]) >= EC_NUM_GROUPS)
            return FALSE;
    }
    if (c->name[0] == EOS || c->name[MP_PLAYER_NAME_SIZE - 1] != EOS)
        return FALSE;
    for (i = 0; i < MP_PLAYER_NAME_SIZE && c->name[i] != EOS; i++)
    {
        if (c->name[i] >= CHAR_DYNAMIC)
            return FALSE;
    }
    return TRUE;
}

static u8 ParseWith(const u8 *payload, u32 offset, u8 value, struct MpMsgTrainerCardData *out)
{
    u8 copy[MP_TRAINER_CARD_DATA_SIZE];

    memcpy(copy, payload, sizeof(copy));
    copy[offset] = value;
    return MpProto_ParseTrainerCardData(copy, sizeof(copy), out);
}

static void TestCardParser(void)
{
    u8 buf[MP_MAX_PACKET_SIZE + 8];
    u8 copy[MP_MAX_PACKET_SIZE + 8];
    const u8 *payload = &buf[MP_HEADER_SIZE];
    struct MpMsgTrainerCardData in, out, out2;
    struct MpMsgHeader hdr;
    u32 failuresBefore = sFail;
    u32 i, bit, n, okFlips;
    u16 len;
    u8 r1, r2;

    FixtureCardMsg(&in);
    len = MpProto_WriteTrainerCardData(buf, MP_MAX_PACKET_SIZE, &in);
    V_CHECK(len == MP_HEADER_SIZE + MP_TRAINER_CARD_DATA_SIZE, "CARD write size");
    V_CHECK(MpProto_ParseHeader(buf, len, &hdr) && hdr.type == MP_MSG_TRAINER_CARD_DATA
         && hdr.version == MP_PROTOCOL_VERSION && hdr.length == MP_TRAINER_CARD_DATA_SIZE, "CARD header");
    // Round trip
    V_CHECK(MpProto_ParseTrainerCardData(payload, MP_TRAINER_CARD_DATA_SIZE, &out) == MP_PARSE_OK
         && out.slot == in.slot && out.epoch == in.epoch && out.requestId == in.requestId
         && SameCard(&out.card, &in.card), "CARD round trip");
    // Truncated
    for (n = 0; n < MP_TRAINER_CARD_DATA_SIZE; n++)
        V_CHECK(MpProto_ParseTrainerCardData(payload, n, &out) == MP_PARSE_ERR_TRUNCATED, "CARD truncated");
    V_CHECK(MpProto_ParseTrainerCardData(NULL, 0, &out) == MP_PARSE_ERR_TRUNCATED, "CARD NULL");
    // Oversized: +1, padded to 512 and 513
    memset(copy, 0, sizeof(copy));
    memcpy(copy, payload, MP_TRAINER_CARD_DATA_SIZE);
    V_CHECK(MpProto_ParseTrainerCardData(copy, MP_TRAINER_CARD_DATA_SIZE + 1, &out) == MP_PARSE_ERR_OVERSIZED, "CARD +1");
    V_CHECK(MpProto_ParseTrainerCardData(copy, 512, &out) == MP_PARSE_ERR_OVERSIZED, "CARD 512");
    V_CHECK(MpProto_ParseTrainerCardData(copy, 513, &out) == MP_PARSE_ERR_OVERSIZED, "CARD 513");
    V_CHECK(MpProto_WriteTrainerCardData(copy, MP_HEADER_SIZE + MP_TRAINER_CARD_DATA_SIZE - 1, &in) == 0, "CARD write into small buffer");
    // Bit flips
    okFlips = 0;
    for (i = 0; i < MP_TRAINER_CARD_DATA_SIZE; i++)
    {
        for (bit = 0; bit < 8; bit++)
        {
            memcpy(copy, payload, MP_TRAINER_CARD_DATA_SIZE);
            copy[i] ^= (u8)(1 << bit);
            r1 = MpProto_ParseTrainerCardData(copy, MP_TRAINER_CARD_DATA_SIZE, &out);
            r2 = MpProto_ParseTrainerCardData(copy, MP_TRAINER_CARD_DATA_SIZE, &out2);
            V_CHECK(r1 == r2 && (r1 != MP_PARSE_OK || memcmp(&out, &out2, sizeof(out)) == 0), "CARD bit flip deterministic");
            if (r1 == MP_PARSE_OK)
            {
                okFlips++;
                V_CHECK(CardInRange(&out), "CARD flip result in range");
            }
            else
            {
                V_CHECK(out.slot == 0 && out.epoch == 0 && out.card.name[0] == 0, "CARD error zeroes out");
            }
        }
    }
    V_CHECK(okFlips > 0, "some CARD bit flips are still valid");

    // Targeted cases
    V_CHECK(ParseWith(payload, CARD_OFS_SLOT, MP_MAX_PLAYERS, &out) == MP_PARSE_ERR_BAD_FIELD, "CARD slot 4 rejected");
    V_CHECK(ParseWith(payload, CARD_OFS_EPOCH, 0, &out) == MP_PARSE_ERR_BAD_FIELD, "CARD epoch 0 rejected");
    V_CHECK(ParseWith(payload, CARD_OFS_GENDER, 2, &out) == MP_PARSE_ERR_BAD_FIELD, "CARD gender 2 rejected");
    V_CHECK(ParseWith(payload, CARD_OFS_GENDER, 0xFF, &out) == MP_PARSE_ERR_BAD_FIELD, "CARD gender 0xFF rejected");
    V_CHECK(ParseWith(payload, CARD_OFS_STARS, MP_CARD_MAX_STARS + 1, &out) == MP_PARSE_ERR_BAD_FIELD, "CARD stars 5 rejected");
    V_CHECK(ParseWith(payload, CARD_OFS_STARS, MP_CARD_MAX_STARS, &out) == MP_PARSE_OK && out.card.stars == MP_CARD_MAX_STARS, "CARD stars 4 accepted");
    V_CHECK(ParseWith(payload, CARD_OFS_NAME, EOS, &out) == MP_PARSE_ERR_BAD_SUMMARY, "CARD empty name rejected");
    V_CHECK(ParseWith(payload, CARD_OFS_NAME + 1, 0xFC, &out) == MP_PARSE_ERR_BAD_SUMMARY, "CARD name with 0xFC rejected");
    V_CHECK(ParseWith(payload, CARD_OFS_NAME + 1, 0xFD, &out) == MP_PARSE_ERR_BAD_SUMMARY, "CARD name with 0xFD rejected");
    memcpy(copy, payload, MP_TRAINER_CARD_DATA_SIZE);
    for (i = 0; i < MP_PLAYER_NAME_SIZE; i++)
        copy[CARD_OFS_NAME + i] = CHAR_A;
    V_CHECK(MpProto_ParseTrainerCardData(copy, MP_TRAINER_CARD_DATA_SIZE, &out) == MP_PARSE_ERR_BAD_SUMMARY, "CARD name without EOS rejected");
    memcpy(copy, payload, MP_TRAINER_CARD_DATA_SIZE);
    copy[CARD_OFS_NAME + 6] = CHAR_Z; // after the first EOS: ignored, stored as EOS
    V_CHECK(MpProto_ParseTrainerCardData(copy, MP_TRAINER_CARD_DATA_SIZE, &out) == MP_PARSE_OK && out.card.name[6] == EOS, "CARD bytes after EOS normalised");
    // Clamps
    memcpy(copy, payload, MP_TRAINER_CARD_DATA_SIZE);
    copy[CARD_OFS_HOF_HOURS] = 0xFF; copy[CARD_OFS_HOF_HOURS + 1] = 0xFF;
    copy[CARD_OFS_HOF_MIN] = 200;
    copy[CARD_OFS_PLAY_MIN] = 60;
    copy[CARD_OFS_WINS] = 0xFF; copy[CARD_OFS_WINS + 1] = 0xFF;
    copy[CARD_OFS_MONEY] = 0xFF; copy[CARD_OFS_MONEY + 1] = 0xFF; copy[CARD_OFS_MONEY + 2] = 0xFF; copy[CARD_OFS_MONEY + 3] = 0xFF;
    copy[CARD_OFS_EC0] = 0x00; copy[CARD_OFS_EC0 + 1] = 0xFE; // group 127: no such group
    copy[CARD_OFS_FLAGS] = 0xFF;
    copy[CARD_OFS_RESERVED3] = 0xFF;
    V_CHECK(MpProto_ParseTrainerCardData(copy, MP_TRAINER_CARD_DATA_SIZE, &out) == MP_PARSE_OK, "CARD with out-of-range counters parses");
    V_CHECK(out.card.hofDebutHours == MP_CARD_MAX_HOURS && out.card.hofDebutMinutes == MP_CARD_MAX_MINUTES
         && out.card.playTimeMinutes == MP_CARD_MAX_MINUTES && out.card.linkBattleWins == MP_CARD_MAX_COUNT_4
         && out.card.money == MP_CARD_MAX_MONEY, "CARD counters clamped");
    V_CHECK(out.card.easyChatProfile[0] == MP_CARD_EC_EMPTY_WORD, "CARD easy chat word of an unknown group becomes empty");
    V_CHECK(out.card.hasPokedex == TRUE && out.card.caughtAllHoenn == TRUE && out.card.hasAllPaintings == TRUE
         && out.card.hasAllFrontierSymbols == TRUE, "CARD flags parse as booleans, reserved bits ignored");
    V_CHECK(CardInRange(&out), "CARD clamped result in range");

    // Garbage
    MP_LOG("selftest: view: card garbage seed 0x%08X\n", (unsigned)sRng);
    for (i = 0; i < 100000; i++)
    {
        u32 glen = Rand() % 601;

        if (glen > sizeof(copy))
            glen = sizeof(copy);
        for (n = 0; n < glen; n++)
            copy[n] = (u8)Rand();
        // Every 4th buffer has the right size, so the field checks get exercised too.
        if ((i & 3) == 0)
            glen = MP_TRAINER_CARD_DATA_SIZE;
        if (MpProto_ParseTrainerCardData(copy, glen, &out) == MP_PARSE_OK)
            V_CHECK(glen == MP_TRAINER_CARD_DATA_SIZE && CardInRange(&out), "CARD garbage accepted only when well formed");
    }
    Report("TRAINER_CARD_DATA parser fuzzing", failuresBefore);
}

// ===========================================================================
// 3. Host + 2 clients
// ===========================================================================

#define V_HOST  0
#define V_C1    1
#define V_C2    2
#define INSTANCES 3

#define VIEW_PORT_FIRST 27540
#define VIEW_PORT_LAST  27549

struct ViewSeen
{
    u8 requests;
    u8 results;
    u8 resultSlot;
    u8 resultKind;
    u8 result;
    u8 cancels;
};

static struct ViewSeen sSeen[INSTANCES];
static u16 sPort;
static u8 sWaitInstance;
static u8 sWaitSlot;
static u8 sWaitCount;
static u16 sWaitMap;
static bool8 sTamperCard[INSTANCES];

// gMpLocalTrainerCardFunc for the test: the card of whichever instance is selected.
static void TestCardProvider(struct MpTrainerCard *out)
{
    u8 instance = gMpSession->instance;

    FixtureCard(out, instance);
    if (instance < INSTANCES && sTamperCard[instance])
        out->stars = MP_CARD_MAX_STARS + 3; // out of range: the host's parser rejects it
}

static void Sel(u8 instance)
{
    MpSession_SelectInstance(instance);
}

static void PumpOnce(void)
{
    struct MpEvent ev;
    u8 i;

    for (i = 0; i < INSTANCES; i++)
    {
        Sel(i);
        MP_Update();
        while (MP_PollEvent(&ev))
        {
            struct ViewSeen *s = &sSeen[i];

            switch (ev.type)
            {
            case MP_EVENT_INTERACT_REQUEST:
                s->requests++;
                break;
            case MP_EVENT_INTERACT_RESULT:
                s->results++;
                s->resultSlot = ev.slot;
                s->resultKind = ev.kind;
                s->result = ev.result;
                break;
            case MP_EVENT_INTERACT_CANCELLED:
                s->cancels++;
                break;
            }
        }
    }
}

static void PumpFor(u32 ms)
{
    u32 start = MpTransport_GetTimeMs();

    while ((u32)(MpTransport_GetTimeMs() - start) < ms)
        PumpOnce();
}

static bool8 PumpUntil(bool8 (*cond)(void), u32 timeoutMs)
{
    u32 start = MpTransport_GetTimeMs();

    while (!cond() && (u32)(MpTransport_GetTimeMs() - start) < timeoutMs)
        PumpOnce();
    return cond();
}

static bool8 CondInSession(void)    { Sel(sWaitInstance); return MP_GetState() == MP_STATE_IN_SESSION; }
static bool8 CondRequests(void)     { return sSeen[sWaitInstance].requests >= sWaitCount; }
static bool8 CondResults(void)      { return sSeen[sWaitInstance].results >= sWaitCount; }

static bool8 CondHostHasPresence(void)
{
    const struct MpPlayer *p;

    Sel(V_HOST);
    p = MP_GetPlayer(sWaitSlot);
    return p != NULL && p->hasPresence && p->presence.mapGroup == MAP_GROUP(sWaitMap) && p->presence.mapNum == MAP_NUM(sWaitMap);
}

static bool8 CondSlotGone(void)
{
    Sel(sWaitInstance);
    return MP_GetPlayer(sWaitSlot) == NULL;
}

static bool8 CondAllClosed(void)
{
    u8 i;

    for (i = 0; i < INSTANCES; i++)
    {
        Sel(i);
        if (MpTransport_IsOpen())
            return FALSE;
    }
    return TRUE;
}

static bool8 Wait(bool8 (*cond)(void), u8 instance, u8 count, u32 timeoutMs)
{
    sWaitInstance = instance;
    sWaitCount = count;
    return PumpUntil(cond, timeoutMs);
}

static void SetMap(u8 instance, u16 mapId)
{
    struct MpPresence p;

    memset(&p, 0, sizeof(p));
    p.mapGroup = (u8)MAP_GROUP(mapId);
    p.mapNum = (u8)MAP_NUM(mapId);
    p.x = 5 + instance;
    p.y = 5;
    p.facing = DIR_SOUTH;
    p.movement = MP_MOVE_NONE;
    p.avatarState = MP_AVATAR_ON_FOOT;
    p.busy = MP_BUSY_NONE;
    p.stepId = (u8)(mapId + instance);
    Sel(instance);
    MP_SetLocalPresence(&p);
}

static bool8 JoinAs(u8 instance, const char *name)
{
    Sel(instance);
    if (!MP_SetNameOverride(name))
        return FALSE;
    return MP_Join("127.0.0.1", sPort);
}

static void CleanUp(void)
{
    u8 i;

    for (i = 0; i < INSTANCES; i++)
    {
        Sel(i);
        MP_Leave();
    }
    PumpUntil(CondAllClosed, 2000);
    for (i = 0; i < INSTANCES; i++)
    {
        Sel(i);
        MpTransport_Close();
        MpSession_ResetInstance(i);
    }
    Sel(0);
}

static void WaitGap(void)
{
    PumpFor(MP_INTERACT_MIN_GAP_MS + 100);
}

// Host-side malformed count of the client in `slot`.
static u8 HostMalformedOf(u8 slot)
{
    u8 peer;

    Sel(V_HOST);
    if (slot >= MP_MAX_PLAYERS)
        return 0xFF;
    peer = gMpSession->slotPeer[slot];
    if (peer >= MP_HOST_PEER_COUNT)
        return 0xFF;
    return gMpSession->peers[peer].malformed;
}

// `requester` asks `target` for its card; waits for the result. Returns the result.
static u8 AskCard(u8 requester, u8 target)
{
    u8 before = sSeen[requester].results;

    Sel(requester);
    if (!MP_SendInteractRequest(target, MP_INTERACT_TRAINER_CARD))
        return 0xFF;
    if (!Wait(CondResults, requester, before + 1, 2000))
        return 0xFE;
    return sSeen[requester].result;
}

static void CheckViewedCard(u8 viewer, u8 target, u8 variant, const char *what)
{
    struct MpTrainerCard got, want;

    FixtureCard(&want, variant);
    Sel(viewer);
    V_CHECK(MP_GetViewedTrainerCard(target, &got) && SameCard(&got, &want), what);
    V_CHECK(!MP_GetViewedTrainerCard((u8)((target + 1) % MP_MAX_PLAYERS), &got), "the card is only there for that slot");
    MP_ClearViewedData();
    V_CHECK(!MP_GetViewedTrainerCard(target, &got), "MP_ClearViewedData forgets the card");
}

static void TestScenario(void)
{
    u32 failuresBefore;
    u8 i;
    bool8 hosting = FALSE;
    const u16 sameMap = MAP_LITTLEROOT_TOWN;
    void (*savedProvider)(struct MpTrainerCard *) = gMpLocalTrainerCardFunc;

    memset(sSeen, 0, sizeof(sSeen));
    memset(sTamperCard, 0, sizeof(sTamperCard));
    gMpLocalTrainerCardFunc = TestCardProvider;
    for (i = 0; i < INSTANCES; i++)
        MpSession_ResetInstance(i);

    // --- session: host + 2 clients on one map ---
    failuresBefore = sFail;
    Sel(V_HOST);
    MP_SetNameOverride("Host");
    for (sPort = VIEW_PORT_FIRST; sPort <= VIEW_PORT_LAST && !hosting; sPort++)
        hosting = MP_Host(sPort);
    sPort--;
    V_CHECK(hosting, "view host on a port in 27540..27549");
    if (!hosting)
    {
        gMpLocalTrainerCardFunc = savedProvider;
        return;
    }
    V_CHECK(JoinAs(V_C1, "ViewA"), "client 1 MP_Join");
    V_CHECK(Wait(CondInSession, V_C1, 0, 5000), "client 1 in session");
    V_CHECK(JoinAs(V_C2, "ViewB"), "client 2 MP_Join");
    V_CHECK(Wait(CondInSession, V_C2, 0, 5000), "client 2 in session");
    for (i = 0; i < INSTANCES; i++)
        SetMap(i, sameMap);
    for (i = 0; i < INSTANCES; i++)
    {
        sWaitSlot = i;
        sWaitMap = sameMap;
        V_CHECK(PumpUntil(CondHostHasPresence, 2000), "host has everyone's presence");
    }
    Report("session for the view tests", failuresBefore);

    // --- client -> client, relayed by the host; no prompt on the target ---
    failuresBefore = sFail;
    V_CHECK(AskCard(V_C1, 2) == MP_INTERACT_RESULT_ACCEPTED, "C1 gets C2's card");
    V_CHECK(sSeen[V_C1].resultSlot == 2 && sSeen[V_C1].resultKind == MP_INTERACT_TRAINER_CARD, "result names C2 and TRAINER_CARD");
    V_CHECK(sSeen[V_C2].requests == 0, "C2 was not prompted");
    Sel(V_C2);
    V_CHECK(!MP_HasIncomingInteract(), "C2 has no incoming request");
    CheckViewedCard(V_C1, 2, V_C2, "C1 sees C2's card exactly");
    Report("trainer card client -> client", failuresBefore);

    // --- client -> host (the host is the target) ---
    failuresBefore = sFail;
    V_CHECK(AskCard(V_C2, MP_HOST_SLOT) == MP_INTERACT_RESULT_ACCEPTED, "C2 gets the host's card");
    V_CHECK(sSeen[V_HOST].requests == 0, "the host was not prompted");
    CheckViewedCard(V_C2, MP_HOST_SLOT, V_HOST, "C2 sees the host's card exactly");
    Report("trainer card client -> host", failuresBefore);

    // --- host -> client (the host is the requester) ---
    failuresBefore = sFail;
    V_CHECK(AskCard(V_HOST, 1) == MP_INTERACT_RESULT_ACCEPTED, "the host gets C1's card");
    CheckViewedCard(V_HOST, 1, V_C1, "the host sees C1's card exactly");
    Report("trainer card host -> client", failuresBefore);

    // --- a card request is not refused while the target has a trade prompt open ---
    failuresBefore = sFail;
    WaitGap();
    Sel(V_C1);
    V_CHECK(MP_SendInteractRequest(2, MP_INTERACT_TRADE), "C1 asks C2 to trade");
    V_CHECK(Wait(CondRequests, V_C2, 1, 2000), "C2 gets the trade prompt");
    WaitGap();
    V_CHECK(AskCard(V_HOST, 2) == MP_INTERACT_RESULT_ACCEPTED, "the host still gets C2's card");
    CheckViewedCard(V_HOST, 2, V_C2, "the host sees C2's card");
    Sel(V_C2);
    V_CHECK(MP_HasIncomingInteract(), "C2's trade prompt is untouched");
    MP_RespondInteract(MP_INTERACT_RESULT_DECLINED);
    V_CHECK(Wait(CondResults, V_C1, 2, 2000) && sSeen[V_C1].result == MP_INTERACT_RESULT_DECLINED, "C1: C2 declined the trade");
    Report("trainer card while the target has a prompt", failuresBefore);

    // --- a tampered card is rejected: BAD_DATA, nothing to show ---
    failuresBefore = sFail;
    WaitGap();
    sTamperCard[V_C2] = TRUE;
    V_CHECK(AskCard(V_C1, 2) == MP_INTERACT_RESULT_BAD_DATA, "C1: C2's tampered card is BAD_DATA");
    {
        struct MpTrainerCard got;

        Sel(V_C1);
        V_CHECK(!MP_GetViewedTrainerCard(2, &got), "nothing to show after BAD_DATA");
    }
    V_CHECK(HostMalformedOf(2) == 1, "the host counted C2's bad card as malformed");
    sTamperCard[V_C2] = FALSE;
    Report("tampered trainer card rejected", failuresBefore);

    // --- malformed card on the client receive path ---
    failuresBefore = sFail;
    {
        u8 junk[MP_TRAINER_CARD_DATA_SIZE];

        memset(junk, 0, sizeof(junk));
        Sel(V_C1);
        V_CHECK(!MpInteract_OnClientReceive(MP_MSG_TRAINER_CARD_DATA, junk, sizeof(junk)), "client: zeroed card is malformed");
        V_CHECK(!MpInteract_OnClientReceive(MP_MSG_TRAINER_CARD_DATA, junk, 7), "client: short card is malformed");
    }
    Report("malformed TRAINER_CARD_DATA", failuresBefore);

    // --- the requester leaves mid-transfer ---
    failuresBefore = sFail;
    WaitGap();
    Sel(V_C1);
    V_CHECK(MP_SendInteractRequest(2, MP_INTERACT_TRAINER_CARD), "C1 asks C2 for its card");
    MP_Leave(); // the request and the BYE are on their way; C2's answer finds no route
    sWaitInstance = V_HOST;
    sWaitSlot = 1;
    V_CHECK(PumpUntil(CondSlotGone, 3000), "the host removed C1");
    PumpFor(300);
    V_CHECK(HostMalformedOf(2) == 1, "C2's late card was dropped silently (no new malformed count)");
    Sel(V_C2);
    V_CHECK(MP_GetState() == MP_STATE_IN_SESSION, "C2 is still in the session");
    Report("requester leaves mid-transfer", failuresBefore);

    CleanUp();
    gMpLocalTrainerCardFunc = savedProvider;
}

u32 MpSelftest_View(void)
{
    sFail = 0;
    TestCardParser();
    sFail += MpPlayerData_SelfTest();
    TestScenario();
    return sFail;
}
