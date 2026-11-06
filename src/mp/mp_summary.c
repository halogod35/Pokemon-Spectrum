// Pokemon Spectrum multiplayer: player summary (PS-013).
//
// Builds the 16-byte network player summary from the loaded save, writes and parses its
// wire form (docs/multiplayer/protocol.md section 5.1), converts names to ASCII for logs,
// and holds the --name override (MP_SetNameOverride, declared in mp.h).
//
// AD-10: this file NEVER writes to a save block. It only reads gSaveBlock2Ptr
// (playerName, playerGender, playerTrainerId) and the badge flags via FlagGet().
// MP_ReadPlayerSummary is a pure function (protocol.md 8.3): no global state, no I/O.

#include "global.h"
#include "event_data.h"
#include "constants/characters.h"
#include "constants/flags.h"
#include "mp/mp.h"
#include "mp/mp_summary.h"
#include "mp/mp_wire.h"

// Wire offsets (protocol.md 5.1).
#define SUMMARY_OFS_NAME           0
#define SUMMARY_OFS_GENDER         8
#define SUMMARY_OFS_AVATAR_STYLE   9
#define SUMMARY_OFS_BADGES         10
#define SUMMARY_OFS_FLAGS          11
#define SUMMARY_OFS_TRAINER_ID     12

// protocol.md 5.1.1: bytes 0xF7..0xFE are text control codes and are rejected before EOS.
#define MP_NAME_FIRST_CONTROL_CHAR CHAR_DYNAMIC // 0xF7

#define MP_NUM_BADGES 8 // FLAG_BADGE01_GET .. FLAG_BADGE08_GET are consecutive.

static bool8 sHasNameOverride = FALSE;
static u8 sNameOverride[MP_PLAYER_NAME_SIZE];

// Used when the save's name cannot be sent as is (empty or starts with a control code),
// or when no save is loaded. "PLAYER" in the game charset.
static const u8 sFallbackName[MP_PLAYER_NAME_SIZE] =
{
    CHAR_P, CHAR_L, CHAR_A, CHAR_Y, CHAR_E, CHAR_R, EOS, EOS
};

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

bool8 MP_IsValidPlayerName(const u8 *name)
{
    u32 i;

    if (name == NULL)
        return FALSE;
    if (name[0] == EOS)
        return FALSE; // Empty names are invalid.

    for (i = 0; i < MP_PLAYER_NAME_SIZE; i++)
    {
        if (name[i] == EOS)
            return TRUE; // i is 1..7 here.
        if (name[i] >= MP_NAME_FIRST_CONTROL_CHAR)
            return FALSE;
    }
    return FALSE; // No EOS within 8 bytes.
}

// Copies MP_PLAYER_NAME_SIZE bytes, replacing everything after the first EOS with EOS.
static void MpCopyNameEosPadded(u8 *dst, const u8 *src)
{
    u32 i;
    bool8 ended = FALSE;

    for (i = 0; i < MP_PLAYER_NAME_SIZE; i++)
    {
        if (!ended && src[i] == EOS)
            ended = TRUE;
        dst[i] = ended ? EOS : src[i];
    }
}

// Turns a save name (PLAYER_NAME_LENGTH + 1 bytes) into a name that passes
// MP_IsValidPlayerName: copies up to 7 bytes, stops at EOS or at the first control
// code, EOS-pads, and falls back to "PLAYER" if nothing is left. Never modifies src.
static void MpSanitizeSaveName(u8 *dst, const u8 *src)
{
    u32 i;

    memset(dst, EOS, MP_PLAYER_NAME_SIZE);
    for (i = 0; i < PLAYER_NAME_LENGTH; i++)
    {
        if (src[i] == EOS || src[i] >= MP_NAME_FIRST_CONTROL_CHAR)
            break;
        dst[i] = src[i];
    }

    if (i < PLAYER_NAME_LENGTH && src[i] != EOS)
        DBGPRINTF("[MP] summary: save name has control byte 0x%02X at %u, truncated\n", src[i], (unsigned)i);

    if (i == 0)
        memcpy(dst, sFallbackName, MP_PLAYER_NAME_SIZE);
}

static char MpCharToAscii(u8 c)
{
    if (c >= CHAR_A && c <= CHAR_Z)
        return (char)('A' + (c - CHAR_A));
    if (c >= CHAR_a && c <= CHAR_z)
        return (char)('a' + (c - CHAR_a));
    if (c >= CHAR_0 && c <= CHAR_9)
        return (char)('0' + (c - CHAR_0));

    switch (c)
    {
    case CHAR_SPACE:            return ' ';
    case CHAR_EXCL_MARK:        return '!';
    case CHAR_QUESTION_MARK:    return '?';
    case CHAR_PERIOD:           return '.';
    case CHAR_HYPHEN:           return '-';
    case CHAR_COMMA:            return ',';
    case CHAR_SLASH:            return '/';
    case CHAR_COLON:            return ':';
    case CHAR_SEMICOLON:        return ';';
    case CHAR_AMPERSAND:        return '&';
    case CHAR_PLUS:             return '+';
    case CHAR_EQUALS:           return '=';
    case CHAR_PERCENT:          return '%';
    case CHAR_LEFT_PAREN:       return '(';
    case CHAR_RIGHT_PAREN:      return ')';
    case CHAR_LESS_THAN:        return '<';
    case CHAR_GREATER_THAN:     return '>';
    case CHAR_SGL_QUOTE_RIGHT:  return 0x27; // apostrophe
    default:                    return '?';
    }
}

void MP_NameToAscii(const u8 *gameName, char *out, u32 outSize)
{
    u32 i;
    u32 n = 0;

    if (out == NULL || outSize == 0)
        return;

    if (gameName != NULL)
    {
        for (i = 0; i < MP_PLAYER_NAME_SIZE && n + 1 < outSize; i++)
        {
            if (gameName[i] == EOS)
                break;
            out[n++] = MpCharToAscii(gameName[i]);
        }
    }
    out[n] = '\0';
}

bool8 MP_SetNameOverride(const char *ascii)
{
    u8 name[MP_PLAYER_NAME_SIZE];
    u32 i;
    char c;

    if (ascii == NULL)
        return FALSE;

    memset(name, EOS, sizeof(name));
    for (i = 0; ascii[i] != '\0'; i++)
    {
        if (i >= PLAYER_NAME_LENGTH)
            return FALSE; // More than 7 characters.

        c = ascii[i];
        if (c >= 'A' && c <= 'Z')
            name[i] = CHAR_A + (c - 'A');
        else if (c >= 'a' && c <= 'z')
            name[i] = CHAR_a + (c - 'a');
        else if (c >= '0' && c <= '9')
            name[i] = CHAR_0 + (c - '0');
        else
            return FALSE;
    }
    if (i == 0)
        return FALSE; // Empty.

    memcpy(sNameOverride, name, sizeof(sNameOverride));
    sHasNameOverride = TRUE;
    DBGPRINTF("[MP] name override: %s (network summary only, never saved)\n", ascii);
    return TRUE;
}

// ---------------------------------------------------------------------------
// Build (read only from the save)
// ---------------------------------------------------------------------------

void MP_BuildPlayerSummary(struct MpPlayerSummary *out)
{
    const struct SaveBlock2 *save2 = gSaveBlock2Ptr;
    const u8 *tid;
    u32 i;

    if (out == NULL)
        return;

    memset(out, 0, sizeof(*out));
    out->avatarStyle = MP_AVATAR_STYLE_PLAYER;
    out->summaryFlags = 0;

    if (save2 != NULL)
    {
        MpSanitizeSaveName(out->name, save2->playerName);
        out->gender = (save2->playerGender == FEMALE) ? FEMALE : MALE;
        // Same value as GetTrainerId() (new_game.c), computed without signed shifts.
        tid = save2->playerTrainerId;
        out->trainerId = (u32)tid[0]
                       | ((u32)tid[1] << 8)
                       | ((u32)tid[2] << 16)
                       | ((u32)tid[3] << 24);
    }
    else
    {
        DBGPRINTF("[MP] summary: no save loaded, using a placeholder summary\n");
        memcpy(out->name, sFallbackName, MP_PLAYER_NAME_SIZE);
        out->gender = MALE;
        out->trainerId = 0;
    }

    // FlagGet only reads gSaveBlock1Ptr->flags for these ids (< SPECIAL_FLAGS_START).
    if (gSaveBlock1Ptr != NULL)
    {
        for (i = 0; i < MP_NUM_BADGES; i++)
        {
            if (FlagGet((u16)(FLAG_BADGE01_GET + i)))
                out->badges |= (u8)(1 << i);
        }
    }

    if (sHasNameOverride)
        memcpy(out->name, sNameOverride, MP_PLAYER_NAME_SIZE);
}

// ---------------------------------------------------------------------------
// Wire write / read (protocol.md 5.1)
// ---------------------------------------------------------------------------

u16 MP_WritePlayerSummary(u8 *buf, u16 cap, const struct MpPlayerSummary *in)
{
    struct MpWriter w;

    if (buf == NULL || in == NULL || cap < MP_PLAYER_SUMMARY_WIRE_SIZE)
        return 0;

    MpCopyNameEosPadded(&buf[SUMMARY_OFS_NAME], in->name);
    buf[SUMMARY_OFS_GENDER] = in->gender;
    buf[SUMMARY_OFS_AVATAR_STYLE] = in->avatarStyle;
    buf[SUMMARY_OFS_BADGES] = in->badges;
    buf[SUMMARY_OFS_FLAGS] = 0; // Reserved: the sender writes 0.
    MpWriter_Init(&w, &buf[SUMMARY_OFS_TRAINER_ID], 4);
    MpWriteU32(&w, in->trainerId);
    return MP_PLAYER_SUMMARY_WIRE_SIZE;
}

static bool8 MpParsePlayerSummary(const u8 *buf, u32 len, struct MpPlayerSummary *tmp)
{
    struct MpReader r;

    if (buf == NULL || len < MP_PLAYER_SUMMARY_WIRE_SIZE)
        return FALSE;
    if (!MP_IsValidPlayerName(&buf[SUMMARY_OFS_NAME]))
        return FALSE;
    if (buf[SUMMARY_OFS_GENDER] != MALE && buf[SUMMARY_OFS_GENDER] != FEMALE)
        return FALSE;
    if (buf[SUMMARY_OFS_AVATAR_STYLE] >= MP_AVATAR_STYLE_COUNT)
        return FALSE;

    memset(tmp, 0, sizeof(*tmp));
    MpCopyNameEosPadded(tmp->name, &buf[SUMMARY_OFS_NAME]);
    tmp->gender = buf[SUMMARY_OFS_GENDER];
    tmp->avatarStyle = buf[SUMMARY_OFS_AVATAR_STYLE];
    tmp->badges = buf[SUMMARY_OFS_BADGES];   // Any value.
    tmp->summaryFlags = 0;                   // Reserved: ignored on receive.
    MpReader_Init(&r, &buf[SUMMARY_OFS_TRAINER_ID], 4);
    tmp->trainerId = MpReadU32(&r);          // Any value.
    return TRUE;
}

bool8 MP_ReadPlayerSummary(const u8 *buf, u32 len, struct MpPlayerSummary *out)
{
    struct MpPlayerSummary tmp;

    if (out == NULL)
        return FALSE;

    // Parse into a temporary; copy out only after full validation (protocol.md 8.1.4).
    if (!MpParsePlayerSummary(buf, len, &tmp))
    {
        memset(out, 0, sizeof(*out));
        return FALSE;
    }
    *out = tmp;
    return TRUE;
}

// ---------------------------------------------------------------------------
// Self-test (called by the --mp-selftest runner, PS-011)
// ---------------------------------------------------------------------------

#define SELFTEST_GARBAGE_COUNT   100000
#define SELFTEST_GARBAGE_MAX_LEN 600
#define SELFTEST_SEED            0x50533133 // "PS13"

static u32 sSelfTestFailures;
static u32 sSelfTestRng;

// Scratch copies used by the build group. Kept static (they are large).
static struct SaveBlock1 sTestSave1;
static struct SaveBlock2 sTestSave2;
static u8 sGarbage[SELFTEST_GARBAGE_MAX_LEN];

#define SELFTEST_CHECK(cond)                                                            \
    do                                                                                  \
    {                                                                                   \
        if (!(cond))                                                                    \
        {                                                                               \
            sSelfTestFailures++;                                                        \
            DBGPRINTF("[MP] selftest summary: check failed (mp_summary.c:%d): %s\n",    \
                      __LINE__, #cond);                                                 \
        }                                                                               \
    } while (0)

static u32 MpSelfTestRandom(void)
{
    // xorshift32: deterministic, independent of the game's RNG.
    u32 x = sSelfTestRng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    sSelfTestRng = x;
    return x;
}

static bool8 MpSummaryEquals(const struct MpPlayerSummary *a, const struct MpPlayerSummary *b)
{
    return memcmp(a->name, b->name, MP_PLAYER_NAME_SIZE) == 0
        && a->gender == b->gender
        && a->avatarStyle == b->avatarStyle
        && a->badges == b->badges
        && a->summaryFlags == b->summaryFlags
        && a->trainerId == b->trainerId;
}

static bool8 MpSummaryIsZero(const struct MpPlayerSummary *s)
{
    struct MpPlayerSummary zero;

    memset(&zero, 0, sizeof(zero));
    return MpSummaryEquals(s, &zero);
}

// Invariants every successfully parsed summary must satisfy, plus canonical round trip:
// write(out) then read gives exactly out again.
static bool8 MpSummaryParsedIsSane(const struct MpPlayerSummary *s)
{
    u8 wire[MP_PLAYER_SUMMARY_WIRE_SIZE];
    struct MpPlayerSummary again;
    u32 i;
    bool8 ended = FALSE;

    if (!MP_IsValidPlayerName(s->name))
        return FALSE;
    for (i = 0; i < MP_PLAYER_NAME_SIZE; i++)
    {
        if (s->name[i] == EOS)
            ended = TRUE;
        else if (ended)
            return FALSE; // Bytes after EOS must be EOS in memory.
    }
    if (s->gender > FEMALE || s->avatarStyle >= MP_AVATAR_STYLE_COUNT || s->summaryFlags != 0)
        return FALSE;
    if (MP_WritePlayerSummary(wire, sizeof(wire), s) != MP_PLAYER_SUMMARY_WIRE_SIZE)
        return FALSE;
    if (!MP_ReadPlayerSummary(wire, sizeof(wire), &again))
        return FALSE;
    return MpSummaryEquals(s, &again);
}

static void MpSelfTestReport(const char *group, u32 failuresBefore)
{
    if (sSelfTestFailures == failuresBefore)
        DBGPRINTF("[MP] selftest summary: %s OK\n", group);
    else
        DBGPRINTF("[MP] selftest summary: %s FAILED (%u)\n", group, (unsigned)(sSelfTestFailures - failuresBefore));
}

// "ASH", male, badges 1-3, TID 12345, SID 5 (protocol.md section 10).
static void MpMakeFixtureSummary(struct MpPlayerSummary *s)
{
    memset(s, 0, sizeof(*s));
    memset(s->name, EOS, MP_PLAYER_NAME_SIZE);
    s->name[0] = CHAR_A;
    s->name[1] = CHAR_S;
    s->name[2] = CHAR_H;
    s->gender = MALE;
    s->avatarStyle = MP_AVATAR_STYLE_PLAYER;
    s->badges = 0x07;
    s->summaryFlags = 0;
    s->trainerId = 0x00053039;
}

static const u8 sFixtureWire[MP_PLAYER_SUMMARY_WIRE_SIZE] =
{
    0xBB, 0xCD, 0xC2, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // name "ASH" + EOS padding
    0x00,                                           // gender MALE
    0x00,                                           // avatarStyle PLAYER
    0x07,                                           // badges 0b00000111
    0x00,                                           // summaryFlags
    0x39, 0x30, 0x05, 0x00,                         // trainerId 0x00053039
};

static void MpSelfTestGolden(void)
{
    u32 before = sSelfTestFailures;
    struct MpPlayerSummary s, r;
    u8 wire[MP_PLAYER_SUMMARY_WIRE_SIZE];

    MpMakeFixtureSummary(&s);
    SELFTEST_CHECK(MP_WritePlayerSummary(wire, sizeof(wire), &s) == MP_PLAYER_SUMMARY_WIRE_SIZE);
    SELFTEST_CHECK(memcmp(wire, sFixtureWire, sizeof(wire)) == 0);
    SELFTEST_CHECK(MP_ReadPlayerSummary(sFixtureWire, sizeof(sFixtureWire), &r));
    SELFTEST_CHECK(MpSummaryEquals(&s, &r));
    MpSelfTestReport("golden round trip (protocol.md 10)", before);
}

static void MpSelfTestWriter(void)
{
    u32 before = sSelfTestFailures;
    struct MpPlayerSummary s;
    u8 buf[MP_PLAYER_SUMMARY_WIRE_SIZE + 4];
    u16 cap;
    u32 i;
    bool8 untouched;

    MpMakeFixtureSummary(&s);

    // cap < 16 -> 0 and nothing written.
    for (cap = 0; cap < MP_PLAYER_SUMMARY_WIRE_SIZE; cap++)
    {
        memset(buf, 0xA5, sizeof(buf));
        SELFTEST_CHECK(MP_WritePlayerSummary(buf, cap, &s) == 0);
        untouched = TRUE;
        for (i = 0; i < sizeof(buf); i++)
        {
            if (buf[i] != 0xA5)
                untouched = FALSE;
        }
        SELFTEST_CHECK(untouched);
    }

    // Exactly 16 bytes written even with a larger cap.
    memset(buf, 0xA5, sizeof(buf));
    SELFTEST_CHECK(MP_WritePlayerSummary(buf, sizeof(buf), &s) == MP_PLAYER_SUMMARY_WIRE_SIZE);
    for (i = MP_PLAYER_SUMMARY_WIRE_SIZE; i < sizeof(buf); i++)
        SELFTEST_CHECK(buf[i] == 0xA5);

    // NULL pointers.
    SELFTEST_CHECK(MP_WritePlayerSummary(NULL, sizeof(buf), &s) == 0);
    SELFTEST_CHECK(MP_WritePlayerSummary(buf, sizeof(buf), NULL) == 0);

    // Reserved summaryFlags written as 0; junk after EOS written as EOS.
    s.summaryFlags = 0x5A;
    s.name[4] = CHAR_Z;
    s.name[6] = EXT_CTRL_CODE_BEGIN;
    SELFTEST_CHECK(MP_WritePlayerSummary(buf, sizeof(buf), &s) == MP_PLAYER_SUMMARY_WIRE_SIZE);
    SELFTEST_CHECK(memcmp(buf, sFixtureWire, MP_PLAYER_SUMMARY_WIRE_SIZE) == 0);

    MpSelfTestReport("writer (cap, NULL, padding, reserved)", before);
}

static void MpSelfTestTruncated(void)
{
    u32 before = sSelfTestFailures;
    struct MpPlayerSummary r;
    u8 big[SELFTEST_GARBAGE_MAX_LEN];
    u32 len;

    for (len = 0; len < MP_PLAYER_SUMMARY_WIRE_SIZE; len++)
    {
        memset(&r, 0xCC, sizeof(r));
        SELFTEST_CHECK(!MP_ReadPlayerSummary(sFixtureWire, len, &r));
        SELFTEST_CHECK(MpSummaryIsZero(&r));
    }

    // NULL buffer / NULL out.
    memset(&r, 0xCC, sizeof(r));
    SELFTEST_CHECK(!MP_ReadPlayerSummary(NULL, MP_PLAYER_SUMMARY_WIRE_SIZE, &r));
    SELFTEST_CHECK(MpSummaryIsZero(&r));
    SELFTEST_CHECK(!MP_ReadPlayerSummary(sFixtureWire, sizeof(sFixtureWire), NULL));

    // A longer buffer is fine: exactly 16 bytes are read (message sizes are checked by
    // the message parsers).
    memset(big, 0xEE, sizeof(big));
    memcpy(big, sFixtureWire, sizeof(sFixtureWire));
    SELFTEST_CHECK(MP_ReadPlayerSummary(big, sizeof(big), &r));
    SELFTEST_CHECK(MpSummaryParsedIsSane(&r));

    MpSelfTestReport("truncated buffers", before);
}

// Copies the fixture, changes one byte, and returns the parse result.
static bool8 MpReadFixtureWith(u32 offset, u8 value, struct MpPlayerSummary *r)
{
    u8 wire[MP_PLAYER_SUMMARY_WIRE_SIZE];

    memcpy(wire, sFixtureWire, sizeof(wire));
    wire[offset] = value;
    return MP_ReadPlayerSummary(wire, sizeof(wire), r);
}

static void MpSelfTestTargeted(void)
{
    static const u8 sBadControlBytes[] = { 0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE };
    u32 before = sSelfTestFailures;
    struct MpPlayerSummary r;
    u8 wire[MP_PLAYER_SUMMARY_WIRE_SIZE];
    u32 i, pos;

    // Name with no EOS at all.
    memcpy(wire, sFixtureWire, sizeof(wire));
    memset(&wire[SUMMARY_OFS_NAME], CHAR_A, MP_PLAYER_NAME_SIZE);
    SELFTEST_CHECK(!MP_ReadPlayerSummary(wire, sizeof(wire), &r));
    SELFTEST_CHECK(MpSummaryIsZero(&r));

    // EOS at byte 0 (empty name).
    SELFTEST_CHECK(!MpReadFixtureWith(0, EOS, &r));

    // Control codes before EOS, at every position 0..2 of "ASH" and at the EOS position.
    for (i = 0; i < ARRAY_COUNT(sBadControlBytes); i++)
    {
        for (pos = 0; pos <= 3; pos++)
            SELFTEST_CHECK(!MpReadFixtureWith(pos, sBadControlBytes[i], &r));
    }

    // 0xF6 (the highest normal character) is fine.
    SELFTEST_CHECK(MpReadFixtureWith(1, 0xF6, &r));
    // CHAR_SPACE (0x00) is a normal character.
    SELFTEST_CHECK(MpReadFixtureWith(1, CHAR_SPACE, &r));

    // 7 characters with EOS at index 7.
    memcpy(wire, sFixtureWire, sizeof(wire));
    memset(&wire[SUMMARY_OFS_NAME], CHAR_B, PLAYER_NAME_LENGTH);
    wire[SUMMARY_OFS_NAME + PLAYER_NAME_LENGTH] = EOS;
    SELFTEST_CHECK(MP_ReadPlayerSummary(wire, sizeof(wire), &r));
    SELFTEST_CHECK(MpSummaryParsedIsSane(&r));

    // Junk (including control codes) after the first EOS is ignored and EOS-filled.
    memcpy(wire, sFixtureWire, sizeof(wire));
    wire[4] = EXT_CTRL_CODE_BEGIN;
    wire[5] = PLACEHOLDER_BEGIN;
    wire[6] = CHAR_A;
    wire[7] = 0x00;
    SELFTEST_CHECK(MP_ReadPlayerSummary(wire, sizeof(wire), &r));
    SELFTEST_CHECK(r.name[3] == EOS && r.name[4] == EOS && r.name[5] == EOS && r.name[6] == EOS && r.name[7] == EOS);
    SELFTEST_CHECK(MpSummaryParsedIsSane(&r));

    // Gender.
    SELFTEST_CHECK(MpReadFixtureWith(SUMMARY_OFS_GENDER, FEMALE, &r) && r.gender == FEMALE);
    SELFTEST_CHECK(!MpReadFixtureWith(SUMMARY_OFS_GENDER, 2, &r));
    SELFTEST_CHECK(MpSummaryIsZero(&r));
    SELFTEST_CHECK(!MpReadFixtureWith(SUMMARY_OFS_GENDER, 0xFF, &r));

    // Avatar style (only 0 in M1).
    SELFTEST_CHECK(!MpReadFixtureWith(SUMMARY_OFS_AVATAR_STYLE, MP_AVATAR_STYLE_COUNT, &r));
    SELFTEST_CHECK(!MpReadFixtureWith(SUMMARY_OFS_AVATAR_STYLE, 0xFF, &r));

    // Badges: any value. summaryFlags: ignored (stored as 0).
    SELFTEST_CHECK(MpReadFixtureWith(SUMMARY_OFS_BADGES, 0xFF, &r) && r.badges == 0xFF);
    SELFTEST_CHECK(MpReadFixtureWith(SUMMARY_OFS_FLAGS, 0xFF, &r) && r.summaryFlags == 0);

    // Trainer id: any value, little-endian.
    memcpy(wire, sFixtureWire, sizeof(wire));
    wire[12] = 0x01;
    wire[13] = 0x02;
    wire[14] = 0x03;
    wire[15] = 0xFE;
    SELFTEST_CHECK(MP_ReadPlayerSummary(wire, sizeof(wire), &r) && r.trainerId == 0xFE030201);

    MpSelfTestReport("targeted (no EOS, EOS at 0, control codes, gender, style)", before);
}

static void MpSelfTestBitFlips(void)
{
    u32 before = sSelfTestFailures;
    struct MpPlayerSummary r1, r2;
    u8 wire[MP_PLAYER_SUMMARY_WIRE_SIZE];
    u32 bit, byteIndex;
    bool8 ok1, ok2;

    for (bit = 0; bit < MP_PLAYER_SUMMARY_WIRE_SIZE * 8; bit++)
    {
        byteIndex = bit / 8;
        memcpy(wire, sFixtureWire, sizeof(wire));
        wire[byteIndex] ^= (u8)(1 << (bit % 8));

        ok1 = MP_ReadPlayerSummary(wire, sizeof(wire), &r1);
        ok2 = MP_ReadPlayerSummary(wire, sizeof(wire), &r2);
        SELFTEST_CHECK(ok1 == ok2);                  // Deterministic.
        SELFTEST_CHECK(MpSummaryEquals(&r1, &r2));
        if (ok1)
            SELFTEST_CHECK(MpSummaryParsedIsSane(&r1));
        else
            SELFTEST_CHECK(MpSummaryIsZero(&r1));

        // badges, summaryFlags and trainerId accept any value.
        if (byteIndex >= SUMMARY_OFS_BADGES)
            SELFTEST_CHECK(ok1);
        // Gender bit 0 gives FEMALE (valid); every other gender bit is invalid.
        if (byteIndex == SUMMARY_OFS_GENDER)
            SELFTEST_CHECK(ok1 == (bit % 8 == 0));
        // Any avatarStyle bit makes it >= MP_AVATAR_STYLE_COUNT.
        if (byteIndex == SUMMARY_OFS_AVATAR_STYLE)
            SELFTEST_CHECK(!ok1);
    }
    MpSelfTestReport("single-bit flips", before);
}

static void MpSelfTestGarbage(void)
{
    u32 before = sSelfTestFailures;
    struct MpPlayerSummary r;
    u32 n, i, len, accepted = 0;
    bool8 ok;

    sSelfTestRng = SELFTEST_SEED;
    for (n = 0; n < SELFTEST_GARBAGE_COUNT; n++)
    {
        len = MpSelfTestRandom() % (SELFTEST_GARBAGE_MAX_LEN + 1);
        for (i = 0; i < len; i++)
            sGarbage[i] = (u8)(MpSelfTestRandom() >> 24);
        // Bias a quarter of the cases towards plausible headers so the deeper rules run.
        if ((n & 3) == 0 && len >= MP_PLAYER_SUMMARY_WIRE_SIZE)
        {
            sGarbage[SUMMARY_OFS_GENDER] &= 1;
            sGarbage[SUMMARY_OFS_AVATAR_STYLE] = MP_AVATAR_STYLE_PLAYER;
            sGarbage[SUMMARY_OFS_NAME + (MpSelfTestRandom() % MP_PLAYER_NAME_SIZE)] = EOS;
        }

        ok = MP_ReadPlayerSummary(sGarbage, len, &r);
        if (len < MP_PLAYER_SUMMARY_WIRE_SIZE)
            SELFTEST_CHECK(!ok);
        if (ok)
        {
            accepted++;
            SELFTEST_CHECK(MpSummaryParsedIsSane(&r));
        }
        else
        {
            SELFTEST_CHECK(MpSummaryIsZero(&r));
        }
    }
    DBGPRINTF("[MP] selftest summary: garbage seed 0x%08X, %u buffers, %u accepted\n",
              (unsigned)SELFTEST_SEED, (unsigned)SELFTEST_GARBAGE_COUNT, (unsigned)accepted);
    MpSelfTestReport("random garbage", before);
}

static void MpSelfTestAscii(void)
{
    u32 before = sSelfTestFailures;
    char out[32];
    u8 name[MP_PLAYER_NAME_SIZE];
    // The converter must never read past MP_PLAYER_NAME_SIZE: the canary follows the name.
    struct { u8 name[MP_PLAYER_NAME_SIZE]; u8 canary[4]; } noEos;

    MP_NameToAscii(sFixtureWire, out, sizeof(out));
    SELFTEST_CHECK(strcmp(out, "ASH") == 0);

    MP_NameToAscii(sFixtureWire, out, 3);
    SELFTEST_CHECK(strcmp(out, "AS") == 0);
    MP_NameToAscii(sFixtureWire, out, 1);
    SELFTEST_CHECK(out[0] == '\0');

    out[0] = 'x';
    MP_NameToAscii(sFixtureWire, out, 0);
    SELFTEST_CHECK(out[0] == 'x');
    MP_NameToAscii(sFixtureWire, NULL, sizeof(out)); // Must not crash.

    MP_NameToAscii(NULL, out, sizeof(out));
    SELFTEST_CHECK(out[0] == '\0');

    memset(noEos.name, CHAR_z, sizeof(noEos.name));
    memset(noEos.canary, CHAR_A, sizeof(noEos.canary));
    MP_NameToAscii(noEos.name, out, sizeof(out));
    SELFTEST_CHECK(strcmp(out, "zzzzzzzz") == 0);

    memset(name, EOS, sizeof(name));
    name[0] = CHAR_0;
    name[1] = CHAR_9;
    name[2] = CHAR_SPACE;
    name[3] = PLACEHOLDER_BEGIN;
    name[4] = CHAR_PERIOD;
    name[5] = CHAR_MALE;
    MP_NameToAscii(name, out, sizeof(out));
    SELFTEST_CHECK(strcmp(out, "09 ?.?") == 0);

    MpSelfTestReport("name to ASCII", before);
}

static void MpSelfTestOverride(void)
{
    static const u8 sExpected[MP_PLAYER_NAME_SIZE] = { CHAR_A, CHAR_s, CHAR_h, CHAR_9, EOS, EOS, EOS, EOS };
    u32 before = sSelfTestFailures;
    bool8 savedHas = sHasNameOverride;
    u8 savedName[MP_PLAYER_NAME_SIZE];

    memcpy(savedName, sNameOverride, sizeof(savedName));
    sHasNameOverride = FALSE;

    SELFTEST_CHECK(!MP_SetNameOverride(NULL));
    SELFTEST_CHECK(!MP_SetNameOverride(""));
    SELFTEST_CHECK(!MP_SetNameOverride("ABCDEFGH"));
    SELFTEST_CHECK(!MP_SetNameOverride("A B"));
    SELFTEST_CHECK(!MP_SetNameOverride("Ash-1"));
    SELFTEST_CHECK(!MP_SetNameOverride("\xC3\xA9"));
    SELFTEST_CHECK(!sHasNameOverride); // Failures leave the state unchanged.

    SELFTEST_CHECK(MP_SetNameOverride("Ash9"));
    SELFTEST_CHECK(sHasNameOverride);
    SELFTEST_CHECK(memcmp(sNameOverride, sExpected, sizeof(sExpected)) == 0);
    SELFTEST_CHECK(MP_IsValidPlayerName(sNameOverride));

    SELFTEST_CHECK(MP_SetNameOverride("ABCDEFG"));
    SELFTEST_CHECK(MP_IsValidPlayerName(sNameOverride));
    SELFTEST_CHECK(!MP_SetNameOverride("toolongname"));
    SELFTEST_CHECK(sNameOverride[0] == CHAR_A && sNameOverride[6] == CHAR_G && sNameOverride[7] == EOS);

    sHasNameOverride = savedHas;
    memcpy(sNameOverride, savedName, sizeof(sNameOverride));
    MpSelfTestReport("--name override", before);
}

// Builds from whatever gSaveBlock1Ptr / gSaveBlock2Ptr point at, checks the result
// round-trips, and checks that building did not modify the save blocks.
static void MpSelfTestBuildFromCurrentSave(struct MpPlayerSummary *built)
{
    static struct SaveBlock1 sSnap1;
    static struct SaveBlock2 sSnap2;
    struct MpPlayerSummary r;
    u8 wire[MP_PLAYER_SUMMARY_WIRE_SIZE];

    if (gSaveBlock1Ptr != NULL)
        memcpy(&sSnap1, gSaveBlock1Ptr, sizeof(sSnap1));
    if (gSaveBlock2Ptr != NULL)
        memcpy(&sSnap2, gSaveBlock2Ptr, sizeof(sSnap2));

    MP_BuildPlayerSummary(built);

    if (gSaveBlock1Ptr != NULL)
        SELFTEST_CHECK(memcmp(&sSnap1, gSaveBlock1Ptr, sizeof(sSnap1)) == 0);
    if (gSaveBlock2Ptr != NULL)
        SELFTEST_CHECK(memcmp(&sSnap2, gSaveBlock2Ptr, sizeof(sSnap2)) == 0);

    SELFTEST_CHECK(MpSummaryParsedIsSane(built));
    SELFTEST_CHECK(MP_WritePlayerSummary(wire, sizeof(wire), built) == MP_PLAYER_SUMMARY_WIRE_SIZE);
    SELFTEST_CHECK(MP_ReadPlayerSummary(wire, sizeof(wire), &r));
    SELFTEST_CHECK(MpSummaryEquals(built, &r));
}

static void MpSetTestFlag(u16 id)
{
    sTestSave1.flags[id / 8] |= (u8)(1 << (id & 7));
}

static void MpSelfTestBuild(void)
{
    u32 before = sSelfTestFailures;
    struct SaveBlock1 *realSave1 = gSaveBlock1Ptr;
    struct SaveBlock2 *realSave2 = gSaveBlock2Ptr;
    bool8 savedHas = sHasNameOverride;
    struct MpPlayerSummary s;
    char ascii[MP_PLAYER_NAME_SIZE + 1];

    sHasNameOverride = FALSE;

    // 1. The real save, if one is loaded when the selftest runs.
    if (realSave2 != NULL)
    {
        MpSelfTestBuildFromCurrentSave(&s);
        MP_NameToAscii(s.name, ascii, sizeof(ascii));
        DBGPRINTF("[MP] selftest summary: loaded save -> name \"%s\", gender %u, badges 0x%02X, tid 0x%08X\n",
                  ascii, (unsigned)s.gender, (unsigned)s.badges, (unsigned)s.trainerId);
    }
    else
    {
        DBGPRINTF("[MP] selftest summary: no save loaded, real-save round trip skipped\n");
    }

    // 2. A synthetic save in scratch memory (the pointers are swapped and restored; the
    //    real save blocks are not touched).
    memset(&sTestSave1, 0, sizeof(sTestSave1));
    memset(&sTestSave2, 0, sizeof(sTestSave2));
    memset(sTestSave2.playerName, EOS, sizeof(sTestSave2.playerName));
    sTestSave2.playerName[0] = CHAR_M;
    sTestSave2.playerName[1] = CHAR_a;
    sTestSave2.playerName[2] = CHAR_y;
    sTestSave2.playerGender = FEMALE;
    sTestSave2.playerTrainerId[0] = 0x39;
    sTestSave2.playerTrainerId[1] = 0x30;
    sTestSave2.playerTrainerId[2] = 0x05;
    sTestSave2.playerTrainerId[3] = 0x80;
    MpSetTestFlag(FLAG_BADGE01_GET);
    MpSetTestFlag(FLAG_BADGE03_GET);
    MpSetTestFlag(FLAG_BADGE08_GET);
    gSaveBlock1Ptr = &sTestSave1;
    gSaveBlock2Ptr = &sTestSave2;

    MpSelfTestBuildFromCurrentSave(&s);
    SELFTEST_CHECK(s.name[0] == CHAR_M && s.name[1] == CHAR_a && s.name[2] == CHAR_y && s.name[3] == EOS);
    SELFTEST_CHECK(s.gender == FEMALE);
    SELFTEST_CHECK(s.avatarStyle == MP_AVATAR_STYLE_PLAYER);
    SELFTEST_CHECK(s.badges == 0x85);
    SELFTEST_CHECK(s.summaryFlags == 0);
    SELFTEST_CHECK(s.trainerId == 0x80053039);

    // The --name override replaces only the name.
    SELFTEST_CHECK(MP_SetNameOverride("Bob"));
    MpSelfTestBuildFromCurrentSave(&s);
    SELFTEST_CHECK(s.name[0] == CHAR_B && s.name[1] == CHAR_o && s.name[2] == CHAR_b && s.name[3] == EOS);
    SELFTEST_CHECK(s.gender == FEMALE && s.badges == 0x85);
    sHasNameOverride = FALSE;

    // Sanitizing: empty name -> "PLAYER"; control code -> truncated; no EOS -> 7 chars;
    // bad gender -> MALE.
    memset(sTestSave2.playerName, EOS, sizeof(sTestSave2.playerName));
    sTestSave2.playerGender = 5;
    MpSelfTestBuildFromCurrentSave(&s);
    SELFTEST_CHECK(memcmp(s.name, sFallbackName, MP_PLAYER_NAME_SIZE) == 0);
    SELFTEST_CHECK(s.gender == MALE);

    sTestSave2.playerName[0] = CHAR_A;
    sTestSave2.playerName[1] = CHAR_B;
    sTestSave2.playerName[2] = PLACEHOLDER_BEGIN;
    sTestSave2.playerName[3] = CHAR_C;
    MpSelfTestBuildFromCurrentSave(&s);
    SELFTEST_CHECK(s.name[0] == CHAR_A && s.name[1] == CHAR_B && s.name[2] == EOS);

    memset(sTestSave2.playerName, CHAR_x, sizeof(sTestSave2.playerName));
    MpSelfTestBuildFromCurrentSave(&s);
    SELFTEST_CHECK(s.name[PLAYER_NAME_LENGTH - 1] == CHAR_x && s.name[PLAYER_NAME_LENGTH] == EOS);

    // 3. No save loaded at all -> a valid placeholder, no crash.
    gSaveBlock1Ptr = NULL;
    gSaveBlock2Ptr = NULL;
    MP_BuildPlayerSummary(&s);
    SELFTEST_CHECK(MpSummaryParsedIsSane(&s));
    SELFTEST_CHECK(s.badges == 0 && s.trainerId == 0);

    gSaveBlock1Ptr = realSave1;
    gSaveBlock2Ptr = realSave2;
    sHasNameOverride = savedHas;
    MpSelfTestReport("build from save (read only)", before);
}

bool8 MpSummary_SelfTest(void)
{
    sSelfTestFailures = 0;

    MpSelfTestGolden();
    MpSelfTestWriter();
    MpSelfTestTruncated();
    MpSelfTestTargeted();
    MpSelfTestBitFlips();
    MpSelfTestGarbage();
    MpSelfTestAscii();
    MpSelfTestOverride();
    MpSelfTestBuild();

    if (sSelfTestFailures == 0)
        DBGPRINTF("[MP] selftest summary: all groups OK\n");
    else
        DBGPRINTF("[MP] selftest summary: %u check(s) FAILED\n", (unsigned)sSelfTestFailures);
    return sSelfTestFailures == 0;
}
