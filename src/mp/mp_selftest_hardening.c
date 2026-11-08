// --mp-selftest, PS-047 group: the hardened link receive path (AD-9).
//
// Drives the real decomp consumers directly with crafted data, no network:
//  1. link.c ProcessRecvCmds (MP_LinkSelftestProcessRecvCmds): INIT_BLOCK sizes beyond
//     gDecompressionBuffer (R1), runaway CONT_BLOCKs, the 0x100-byte block that ran into the
//     next buffer, SEND_BLOCK_REQ out of range (R2, local memory sent to the peer), the
//     LinkPlayer name / language / gender / id (R6), unterminated magic strings, then a
//     random-command fuzz with canaries.
//  2. battle_controllers.c TryReceiveLinkBattleData + the copy task on a test ring
//     (MP_BattleLinkSelftest*): block sizes (R3), battler index / size (R4), the controller
//     command filter (DMA3 / raw mon data / indices), engine replies, controller-done ids.
//  3. MP_LinkValidateMon / MP_SanitizeBattleMon (R5), trainer card, mail, text buffers, and the
//     host bus pre-filter (MpLink_IsCmdAllowed).
// Every out-of-bounds case must be refused AND counted by MP_LinkRejectData; every legit case
// must still pass. All global state touched is saved and restored.

#include "global.h"
#include "battle.h"
#include "battle_controllers.h"
#include "battle_message.h"
#include "data.h"
#include "decompress.h"
#include "link.h"
#include "mail.h"
#include "main.h"
#include "pokemon.h"
#include "string_util.h"
#include "task.h"
#include "trainer_card.h"
#include "constants/abilities.h" // ABILITIES_COUNT
#include "constants/battle_string_ids.h"
#include "constants/characters.h"
#include "constants/items.h"
#include "constants/moves.h"
#include "constants/songs.h"
#include "constants/species.h"
#include "mp/mp.h"
#include "mp/mp_internal.h"
#include "mp/mp_link.h"
#include "mp/mp_selftest.h"

static u32 sFail;

#define H_CHECK(cond, what)                                                             \
    do                                                                                  \
    {                                                                                   \
        if (!(cond))                                                                    \
        {                                                                               \
            MP_LOG("selftest: FAIL: hardening: %s (%s:%d)\n", what, __FILE__, __LINE__); \
            sFail++;                                                                    \
        }                                                                               \
    } while (0)

static void Report(const char *what, u32 failuresBefore)
{
    if (sFail == failuresBefore)
        MP_LOG("selftest: hardening: %s OK\n", what);
    else
        MP_LOG("selftest: hardening: %s FAILED (%u)\n", what, (unsigned)(sFail - failuresBefore));
}

static u32 sRng = 0x48415244u; // "HARD"

static u32 Rand(void)
{
    sRng ^= sRng << 13;
    sRng ^= sRng >> 17;
    sRng ^= sRng << 5;
    return sRng;
}

#define STATE_CAP 16384
static u8 sSavedLink[STATE_CAP];    // link.c as we found it
static u8 sCleanLink[STATE_CAP];    // the per-case starting point
static struct SaveBlock1 sFixtureSave1;
static struct SaveBlock2 sFixtureSave2;
static struct SaveBlock1 *sSavedPtr1;
static struct SaveBlock2 *sSavedPtr2;
static MainCallback sSavedCallback2;
static u8 sSavedMainState;
static u8 sSavedDecompression[sizeof(gDecompressionBuffer)];

static const char sMagic[] = "GameFreak inc.";

// A fresh link.c with the cable "up" (so a rejection sets the hardware-error bit).
static void CleanLink(void)
{
    MP_LinkSelftestState(sCleanLink, sizeof(sCleanLink), TRUE);
    gMain.callback2 = NULL;
    gMain.state = 0;
}

static void FeedRow(u8 player, u16 w0, u16 w1, u16 w2, const u16 *rest)
{
    u32 j;

    memset(gRecvCmds, 0, sizeof(gRecvCmds));
    gRecvCmds[player][0] = w0;
    gRecvCmds[player][1] = w1;
    gRecvCmds[player][2] = w2;
    if (rest != NULL)
    {
        for (j = 1; j < CMD_LENGTH; j++)
            gRecvCmds[player][j] = rest[j - 1];
    }
    MP_LinkSelftestProcessRecvCmds();
}

// Sends `size` bytes from `data` as INIT_BLOCK + CONT_BLOCKs from `player`.
static void FeedBlock(u8 player, const u8 *data, u16 size)
{
    u16 words[CMD_LENGTH - 1];
    u32 pos, j;

    FeedRow(player, LINKCMD_INIT_BLOCK, size, player + 0x80, NULL);
    for (pos = 0; pos < size; pos += (CMD_LENGTH - 1) * 2)
    {
        for (j = 0; j < CMD_LENGTH - 1; j++)
        {
            u32 at = pos + j * 2;
            u8 lo = (at < size) ? data[at] : 0;
            u8 hi = (at + 1 < size) ? data[at + 1] : 0;

            words[j] = lo | (hi << 8);
        }
        FeedRow(player, LINKCMD_CONT_BLOCK, 0, 0, words);
    }
}

static void FillRow(u8 row, u16 value)
{
    u32 i;

    for (i = 0; i < BLOCK_BUFFER_SIZE / 2; i++)
        gBlockRecvBuffer[row][i] = value;
}

static bool8 RowIs(u8 row, u16 value)
{
    u32 i;

    for (i = 0; i < BLOCK_BUFFER_SIZE / 2; i++)
    {
        if (gBlockRecvBuffer[row][i] != value)
            return FALSE;
    }
    return TRUE;
}

static bool8 DecompressionIs(u8 value)
{
    u32 i;

    for (i = 0; i < sizeof(gDecompressionBuffer); i++)
    {
        if (gDecompressionBuffer[i] != value)
            return FALSE;
    }
    return TRUE;
}

// ===========================================================================
// 1. link.c receive path
// ===========================================================================

static void TestBlocks(void)
{
    static u8 big[0x4000];
    u32 failuresBefore = sFail;
    u32 rejects, i;
    u16 words[CMD_LENGTH - 1];

    // R1: a size above gDecompressionBuffer is refused; the CONT_BLOCKs after it write nothing.
    CleanLink();
    memset(gDecompressionBuffer, 0xA5, sizeof(gDecompressionBuffer));
    rejects = MP_LinkGetRejectCount();
    FeedRow(1, LINKCMD_INIT_BLOCK, 0x5000, 0x81, NULL);
    H_CHECK(MP_LinkGetRejectCount() == rejects + 1, "INIT_BLOCK of 0x5000 bytes is rejected");
    H_CHECK(gLink.hardwareError, "the rejection raises the cable's hardware-error bit");
    for (i = 0; i < CMD_LENGTH - 1; i++)
        words[i] = 0x1111;
    for (i = 0; i < 0x5000 / 14 + 4; i++)
        FeedRow(1, LINKCMD_CONT_BLOCK, 0, 0, words);
    H_CHECK(DecompressionIs(0xA5), "CONT_BLOCKs after a refused INIT_BLOCK write nothing");
    H_CHECK(!(GetBlockReceivedStatus() & 2), "no block is reported received");
    Report("R1 block size above gDecompressionBuffer refused", failuresBefore);

    // R1: CONT_BLOCKs with no block in progress (the old code let `pos` run through memory).
    failuresBefore = sFail;
    CleanLink();
    for (i = 1; i < MAX_RFU_PLAYERS; i++)
        FillRow(i, 0x5A5A);
    FeedRow(0, LINKCMD_INIT_BLOCK, 14, 0x80, NULL);
    for (i = 0; i < CMD_LENGTH - 1; i++)
        words[i] = (u16)(0x0100 + i);
    for (i = 0; i < 3000; i++)
        FeedRow(0, LINKCMD_CONT_BLOCK, 0, 0, words);
    H_CHECK(gBlockRecvBuffer[0][0] == 0x0100 && gBlockRecvBuffer[0][6] == 0x0106, "the 14-byte block arrived");
    H_CHECK(GetBlockReceivedStatus() & 1, "the 14-byte block is reported received");
    for (i = 1; i < MAX_RFU_PLAYERS; i++)
        H_CHECK(RowIs(i, 0x5A5A), "3000 stray CONT_BLOCKs leave the other players' buffers alone");
    // A stray CONT_BLOCK before any INIT_BLOCK on a fresh link: ignored.
    CleanLink();
    FillRow(2, 0x5A5A);
    FeedRow(2, LINKCMD_CONT_BLOCK, 0, 0, words);
    H_CHECK(RowIs(2, 0x5A5A) && !(GetBlockReceivedStatus() & 4), "a CONT_BLOCK without an INIT_BLOCK is ignored");
    Report("R1 stray CONT_BLOCKs cannot walk past the receive buffers", failuresBefore);

    // A 0x100-byte block: its last CONT_BLOCK used to write 5 words into the next buffer.
    failuresBefore = sFail;
    CleanLink();
    FillRow(4, 0x5A5A);
    for (i = 0; i < 0x100; i++)
        big[i] = (u8)(i * 7 + 3);
    FeedBlock(3, big, 0x100);
    H_CHECK(memcmp(gBlockRecvBuffer[3], big, 0x100) == 0, "a 0x100-byte block arrives intact");
    H_CHECK(RowIs(4, 0x5A5A), "a 0x100-byte block stays inside its own buffer");
    H_CHECK(GetBlockReceivedStatus() & 8, "the 0x100-byte block is reported received");
    Report("full 0x100-byte block stays in bounds", failuresBefore);

    // Legit large blocks still go to gDecompressionBuffer: 0x200 bytes and exactly 0x4000.
    failuresBefore = sFail;
    CleanLink();
    rejects = MP_LinkGetRejectCount();
    for (i = 0; i < sizeof(big); i++)
        big[i] = (u8)(i ^ (i >> 8) ^ 0x3C);
    FeedBlock(2, big, 0x200);
    H_CHECK(memcmp(gDecompressionBuffer, big, 0x200) == 0 && (GetBlockReceivedStatus() & 4), "a 0x200-byte block arrives in gDecompressionBuffer");
    CleanLink();
    FeedBlock(1, big, 0x4000);
    H_CHECK(memcmp(gDecompressionBuffer, big, 0x4000) == 0 && (GetBlockReceivedStatus() & 2), "a 0x4000-byte block (the maximum) arrives intact");
    H_CHECK(MP_LinkGetRejectCount() == rejects && !gLink.hardwareError, "legit blocks are not rejected");
    Report("legit blocks up to 0x4000 bytes still work", failuresBefore);
}

static void TestBlockRequest(void)
{
    u32 failuresBefore = sFail;
    u32 rejects;
    static const u16 sBad[] = { 5, 6, 0x00FF, 0x7FFF, 0xFFFF };
    u32 i;

    for (i = 0; i < ARRAY_COUNT(sBad); i++)
    {
        CleanLink();
        rejects = MP_LinkGetRejectCount();
        FeedRow(1, LINKCMD_SEND_BLOCK_REQ, sBad[i], 0, NULL);
        H_CHECK(MP_LinkGetRejectCount() == rejects + 1, "SEND_BLOCK_REQ with a type past sBlockRequests is rejected");
        H_CHECK(IsLinkTaskFinished(), "no block (of whatever memory) is sent in reply");
        H_CHECK(gSendCmd[0] == 0, "no INIT_BLOCK is queued");
    }
    CleanLink();
    rejects = MP_LinkGetRejectCount();
    FeedRow(1, LINKCMD_SEND_BLOCK_REQ, BLOCK_REQ_SIZE_100, 0, NULL);
    H_CHECK(MP_LinkGetRejectCount() == rejects && !IsLinkTaskFinished(), "a valid SEND_BLOCK_REQ still sends the block");
    H_CHECK(gSendCmd[0] == LINKCMD_INIT_BLOCK && gSendCmd[1] == 100, "it is the 100-byte gBlockSendBuffer block");
    Report("R2 SEND_BLOCK_REQ index out of range refused (no local memory leak)", failuresBefore);
}

static void BuildPlayerBlock(struct LinkPlayerBlock *b, const u8 *name, u16 language, u8 gender, u16 id)
{
    memset(b, 0, sizeof(*b));
    memcpy(b->magic1, sMagic, sizeof(sMagic));
    memcpy(b->magic2, sMagic, sizeof(sMagic));
    b->linkPlayer.version = VERSION_EMERALD;
    b->linkPlayer.trainerId = 0x12345678;
    memcpy(b->linkPlayer.name, name, PLAYER_NAME_LENGTH + 1);
    b->linkPlayer.language = language;
    b->linkPlayer.gender = gender;
    b->linkPlayer.id = id;
    b->linkPlayer.linkType = LINKTYPE_TRADE_SETUP;
}

static void TestLinkPlayer(void)
{
    struct LinkPlayerBlock block;
    u8 name[PLAYER_NAME_LENGTH + 1];
    u32 failuresBefore = sFail;
    u32 i;

    // R6: no EOS, a language past the tables, gender and battler id out of range.
    for (i = 0; i < PLAYER_NAME_LENGTH + 1; i++)
        name[i] = CHAR_A + i;
    CleanLink();
    gLinkStatus = 2 << LINK_STAT_PLAYER_COUNT_SHIFT;
    gRemoteLinkPlayersNotReceived[0] = FALSE;
    gRemoteLinkPlayersNotReceived[1] = TRUE;
    BuildPlayerBlock(&block, name, 0xEE, 9, 77);
    FeedBlock(1, (const u8 *)&block, sizeof(block));
    H_CHECK(gLinkPlayers[1].trainerId == 0x12345678, "the link player was received");
    H_CHECK(gLinkPlayers[1].name[PLAYER_NAME_LENGTH] == EOS, "an unterminated name is terminated");
    H_CHECK(gLinkPlayers[1].language == LANGUAGE_ENGLISH, "an out-of-range language is clamped");
    H_CHECK(gLinkPlayers[1].gender <= FEMALE, "an out-of-range gender is clamped");
    H_CHECK(gLinkPlayers[1].id < MAX_LINK_PLAYERS, "an out-of-range battler id is clamped");
    H_CHECK(gMain.callback2 != CB2_LinkError && gReceivedRemoteLinkPlayers, "valid magic: the exchange completes");

    // Japanese: ConvertInternationalString grows the name by 4 bytes in place. A 7-letter
    // Japanese name used to run into the fields after it (gender became 0xFF).
    CleanLink();
    gLinkStatus = 2 << LINK_STAT_PLAYER_COUNT_SHIFT;
    gRemoteLinkPlayersNotReceived[1] = TRUE;
    for (i = 0; i < PLAYER_NAME_LENGTH; i++)
        name[i] = CHAR_A + i;
    name[PLAYER_NAME_LENGTH] = EOS;
    BuildPlayerBlock(&block, name, LANGUAGE_JAPANESE, FEMALE, 1);
    FeedBlock(1, (const u8 *)&block, sizeof(block));
    H_CHECK(gLinkPlayers[1].gender == FEMALE, "a long Japanese name does not overwrite the gender");
    H_CHECK(gLinkPlayers[1].name[0] == EXT_CTRL_CODE_BEGIN && gLinkPlayers[1].name[1] == EXT_CTRL_CODE_JPN, "the Japanese name is still converted");

    // Unterminated magic: refused (link error), without strcmp running off the buffer.
    CleanLink();
    gLinkStatus = 2 << LINK_STAT_PLAYER_COUNT_SHIFT;
    gRemoteLinkPlayersNotReceived[1] = TRUE;
    BuildPlayerBlock(&block, name, LANGUAGE_ENGLISH, MALE, 1);
    memset(block.magic1, 'G', sizeof(block.magic1));
    FillRow(1, 0x4747); // the rest of the buffer has no NUL either
    FeedBlock(1, (const u8 *)&block, sizeof(block));
    H_CHECK(gMain.callback2 == CB2_LinkError, "unterminated magic: the link-error path");
    Report("R6 link player name / language / gender / id, magic strings", failuresBefore);
}

static void TestRecvFuzz(void)
{
    static const u16 sCmds[] =
    {
        LINKCMD_INIT_BLOCK, LINKCMD_CONT_BLOCK, LINKCMD_CONT_BLOCK, LINKCMD_CONT_BLOCK,
        LINKCMD_SEND_BLOCK_REQ, LINKCMD_READY_CLOSE_LINK, LINKCMD_READY_EXIT_STANDBY,
        LINKCMD_SEND_HELD_KEYS, LINKCMD_BLENDER_SEND_KEYS, LINKCMD_DUMMY_1, LINKCMD_SEND_LINK_TYPE,
    };
    u32 failuresBefore = sFail;
    u32 n, p, j, rejectsBefore = MP_LinkGetRejectCount();

    CleanLink();
    gLinkStatus = 4 << LINK_STAT_PLAYER_COUNT_SHIFT;
    FillRow(4, 0x5A5A); // ProcessRecvCmds never touches player 4
    for (n = 0; n < 200000; n++)
    {
        memset(gRecvCmds, 0, sizeof(gRecvCmds));
        for (p = 0; p < MAX_LINK_PLAYERS; p++)
        {
            if (Rand() % 3 == 0)
                continue;
            gRecvCmds[p][0] = (Rand() % 8 == 0) ? (u16)Rand() : sCmds[Rand() % ARRAY_COUNT(sCmds)];
            for (j = 1; j < CMD_LENGTH; j++)
                gRecvCmds[p][j] = (Rand() % 4 == 0) ? (u16)(Rand() % 0x120) : (u16)Rand();
        }
        if (Rand() % 64 == 0)
            gRemoteLinkPlayersNotReceived[Rand() % MAX_LINK_PLAYERS] = TRUE;
        if (Rand() % 16 == 0)
            gLinkCallback = NULL; // let SEND_LINK_TYPE / SEND_BLOCK_REQ start new sends
        MP_LinkSelftestProcessRecvCmds();
    }
    H_CHECK(RowIs(4, 0x5A5A), "200000 random rows: the buffer past the last player is untouched");
    for (p = 0; p < MAX_LINK_PLAYERS; p++)
        H_CHECK(gLinkPlayers[p].name[PLAYER_NAME_LENGTH] == EOS || gLinkPlayers[p].trainerId == 0, "fuzzed link players are terminated");
    H_CHECK(MP_LinkGetRejectCount() > rejectsBefore, "the fuzz hit the rejection paths");
    Report("ProcessRecvCmds fuzz (200000 random rows, canaries intact)", failuresBefore);
}

// ===========================================================================
// 2. battle_controllers.c link receive path
// ===========================================================================

#define RING_GUARD 64
static u8 sRing[BATTLE_BUFFER_LINK_SIZE + RING_GUARD];
static u8 sSavedBufA[sizeof(gBattleBufferA)];
static u8 sSavedBufB[sizeof(gBattleBufferB)];

static bool8 RingGuardOk(void)
{
    u32 i;

    for (i = 0; i < RING_GUARD; i++)
    {
        if (sRing[BATTLE_BUFFER_LINK_SIZE + i] != 0xC3)
            return FALSE;
    }
    return TRUE;
}

// One battle message from `player`: header + payload, through TryReceiveLinkBattleData.
static void PutBattleMsg(u8 player, u8 bufferId, u8 battler, u16 size, const u8 *payload, u8 attacker)
{
    u8 *b = (u8 *)gBlockRecvBuffer[player];

    memset(b, 0, BLOCK_BUFFER_SIZE);
    b[0] = bufferId;
    b[1] = battler;
    b[2] = attacker;
    b[3] = 0;
    b[4] = (u8)size;
    b[5] = (u8)(size >> 8);
    if (payload != NULL)
        memcpy(&b[8], payload, (size < BLOCK_BUFFER_SIZE - 8) ? size : BLOCK_BUFFER_SIZE - 8);
    gBlockReceivedStatus[player] = TRUE;
    TryReceiveLinkBattleData();
}

// A controller command for battler 1, received and copied. TRUE if it reached the controller.
static bool8 ControllerCmd(const u8 *cmd, u16 size)
{
    u32 flagsBefore;

    gBattleControllerExecFlags = 0;
    memset(gBattleBufferA[1], 0, sizeof(gBattleBufferA[1]));
    PutBattleMsg(0, B_COMM_TO_CONTROLLER, 1, size, cmd, 0);
    flagsBefore = gBattleControllerExecFlags;
    MP_BattleLinkSelftestRunCopy();
    return gBattleControllerExecFlags != flagsBefore;
}

static void TestBattle(void)
{
    u8 cmd[0x100];
    struct BattlePokemon mon;
    struct BattleMsgData msg;
    u32 failuresBefore = sFail;
    u32 savedTypeFlags = gBattleTypeFlags;
    u32 savedExecFlags = gBattleControllerExecFlags;
    u8 savedAttacker = gBattlerAttacker, savedTarget = gBattlerTarget, savedEffect = gBattlerTarget;
    u8 savedAbsent = gAbsentBattlerFlags;
    u32 rejects;

    savedEffect = gEffectBattler;
    memcpy(sSavedBufA, gBattleBufferA, sizeof(sSavedBufA));
    memcpy(sSavedBufB, gBattleBufferB, sizeof(sSavedBufB));
    CleanLink();
    gReceivedRemoteLinkPlayers = TRUE;
    gLinkStatus = 2 << LINK_STAT_PLAYER_COUNT_SHIFT;
    gBattleTypeFlags = BATTLE_TYPE_LINK | BATTLE_TYPE_TRAINER | BATTLE_TYPE_LINK_IN_BATTLE; // not master
    memset(sRing, 0, BATTLE_BUFFER_LINK_SIZE);
    memset(&sRing[BATTLE_BUFFER_LINK_SIZE], 0xC3, RING_GUARD);
    MP_BattleLinkSelftestBegin(sRing);

    // R3: the block's own size field.
    rejects = MP_LinkGetRejectCount();
    PutBattleMsg(0, B_COMM_TO_CONTROLLER, 1, 0xFFFF, NULL, 0);
    PutBattleMsg(1, B_COMM_TO_CONTROLLER, 1, BLOCK_BUFFER_SIZE - 8 + 1, NULL, 0);
    H_CHECK(MP_LinkGetRejectCount() == rejects + 2, "battle blocks bigger than the block buffer are rejected");
    H_CHECK(MP_BattleLinkSelftestRingEnd() == 0 && RingGuardOk(), "nothing is copied into the ring");
    PutBattleMsg(0, B_COMM_TO_ENGINE, 1, BLOCK_BUFFER_SIZE - 8, NULL, 0);
    H_CHECK(MP_LinkGetRejectCount() == rejects + 2 && MP_BattleLinkSelftestRingEnd() == BLOCK_BUFFER_SIZE, "the largest legit block is accepted");
    MP_BattleLinkSelftestRunCopy();
    // Many maximum blocks: the ring wraps without passing its end.
    {
        u32 k;

        for (k = 0; k < 40; k++)
        {
            PutBattleMsg(k & 1, B_COMM_TO_ENGINE, 1, BLOCK_BUFFER_SIZE - 8, NULL, 0);
            MP_BattleLinkSelftestRunCopy();
        }
    }
    H_CHECK(RingGuardOk(), "the ring wraps inside its 0x1000 bytes");
    Report("R3 TryReceiveLinkBattleData block size", failuresBefore);

    // R4: battler index (gBattleBufferA[battler], gBitTable[battler]) and the player id.
    failuresBefore = sFail;
    rejects = MP_LinkGetRejectCount();
    memset(cmd, 0x77, sizeof(cmd));
    PutBattleMsg(0, B_COMM_TO_CONTROLLER, 200, 0x40, cmd, 0);
    MP_BattleLinkSelftestRunCopy();
    PutBattleMsg(0, B_COMM_TO_ENGINE, MAX_BATTLERS_COUNT, 0x40, cmd, 0);
    MP_BattleLinkSelftestRunCopy();
    H_CHECK(MP_LinkGetRejectCount() == rejects + 2, "battle messages for battler >= 4 are rejected");
    gBattleControllerExecFlags = 0x00F0F0F0;
    cmd[0] = 9; // player id: a shift of 36 bits
    PutBattleMsg(0, B_COMM_CONTROLLER_IS_DONE, 1, 4, cmd, 0);
    MP_BattleLinkSelftestRunCopy();
    H_CHECK(MP_LinkGetRejectCount() == rejects + 3 && gBattleControllerExecFlags == 0x00F0F0F0, "controller-done with player id >= 4 is rejected");
    // The attacker / target bytes in the header index battle arrays on the slave.
    cmd[0] = CONTROLLER_RETURNMONTOBALL;
    cmd[1] = 0;
    PutBattleMsg(0, B_COMM_TO_CONTROLLER, 1, 4, cmd, 250);
    gBattleControllerExecFlags = 0;
    MP_BattleLinkSelftestRunCopy();
    H_CHECK(gBattlerAttacker < MAX_BATTLERS_COUNT, "the attacker id from the header is kept in range");
    Report("R4 battler index / player id into gBattleBufferA", failuresBefore);

    // The controller command filter (the link master drives our controllers).
    failuresBefore = sFail;
    rejects = MP_LinkGetRejectCount();
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = CONTROLLER_DMA3TRANSFER;  // address 0x08000000, 0x100 bytes
    cmd[4] = 0x08; cmd[5] = 0x00; cmd[6] = 0x01;
    H_CHECK(!ControllerCmd(cmd, 0x10) && gBattleBufferA[1][0] == CONTROLLER_TERMINATOR_NOP, "DMA3TRANSFER (write anywhere) never reaches a controller");
    cmd[0] = CONTROLLER_SETRAWMONDATA; cmd[1] = 0xF0; cmd[2] = 0xF0;
    H_CHECK(!ControllerCmd(cmd, 0x10), "SETRAWMONDATA (write past the party) is refused");
    cmd[0] = CONTROLLER_GETRAWMONDATA;
    H_CHECK(!ControllerCmd(cmd, 0x10), "GETRAWMONDATA (read past the party) is refused");
    cmd[0] = CONTROLLER_SWITCHINANIM; cmd[1] = PARTY_SIZE + 3;
    H_CHECK(!ControllerCmd(cmd, 4), "SWITCHINANIM to party slot 9 is refused");
    cmd[0] = CONTROLLER_MOVEANIMATION; cmd[1] = 0xFF; cmd[2] = 0xFF;
    H_CHECK(!ControllerCmd(cmd, 0x20), "MOVEANIMATION of move 0xFFFF is refused");
    cmd[0] = CONTROLLER_PRINTSTRING; cmd[2] = 0xFF; cmd[3] = 0xFF;
    H_CHECK(!ControllerCmd(cmd, 4 + sizeof(struct BattleMsgData)), "PRINTSTRING of string 0xFFFF is refused");
    cmd[0] = CONTROLLER_PLAYSE; cmd[1] = 0xFF; cmd[2] = 0x7F;
    H_CHECK(!ControllerCmd(cmd, 4), "PLAYSE past the song table is refused");
    cmd[0] = CONTROLLER_BATTLEANIMATION; cmd[1] = 200;
    H_CHECK(!ControllerCmd(cmd, 4), "BATTLEANIMATION past the table is refused");
    memset(&mon, 0, sizeof(mon));
    mon.species = 0xFFFF;
    cmd[0] = CONTROLLER_SETMONDATA; cmd[1] = REQUEST_ALL_BATTLE; cmd[2] = 0;
    memcpy(&cmd[3], &mon, sizeof(mon));
    H_CHECK(!ControllerCmd(cmd, 3 + sizeof(mon)), "SETMONDATA with species 0xFFFF is refused");
    H_CHECK(MP_LinkGetRejectCount() == rejects + 9, "each refused command is counted as a link error");
    // Legit commands still work, cosmetic indices are clamped.
    rejects = MP_LinkGetRejectCount();
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = CONTROLLER_SWITCHINANIM; cmd[1] = 2;
    H_CHECK(ControllerCmd(cmd, 4) && gBattleBufferA[1][0] == CONTROLLER_SWITCHINANIM && gBattleBufferA[1][1] == 2, "a valid SWITCHINANIM reaches the controller");
    memset(&msg, 0, sizeof(msg));
    msg.scrActive = 200;
    msg.bakScriptPartyIdx = 40;
    msg.currentMove = 0xFFFF;
    cmd[0] = CONTROLLER_PRINTSTRING; cmd[2] = STRINGID_ATTACKMISSED & 0xFF; cmd[3] = STRINGID_ATTACKMISSED >> 8;
    memcpy(&cmd[4], &msg, sizeof(msg));
    H_CHECK(ControllerCmd(cmd, 4 + sizeof(msg)), "a valid PRINTSTRING reaches the controller");
    memcpy(&msg, &gBattleBufferA[1][4], sizeof(msg));
    H_CHECK(msg.scrActive < MAX_BATTLERS_COUNT && msg.bakScriptPartyIdx < PARTY_SIZE && msg.currentMove < MOVES_COUNT, "its message indices are clamped");
    H_CHECK(MP_LinkGetRejectCount() == rejects, "valid commands are not counted as errors");
    Report("battle controller command filter (DMA3, raw mon data, indices)", failuresBefore);

    // Engine replies (the other side answers our engine).
    failuresBefore = sFail;
    rejects = MP_LinkGetRejectCount();
    memset(cmd, 0, sizeof(cmd));
    cmd[0] = CONTROLLER_CHOSENMONRETURNVALUE; cmd[1] = PARTY_SIZE + 1;
    PutBattleMsg(1, B_COMM_TO_ENGINE, 1, 8, cmd, 0);
    MP_BattleLinkSelftestRunCopy();
    H_CHECK(MP_LinkGetRejectCount() == rejects + 1 && gBattleBufferB[1][0] == CONTROLLER_TERMINATOR_NOP, "a chosen party slot past PARTY_SIZE is refused");
    cmd[1] = PARTY_SIZE; cmd[2] = 0xFF; cmd[3] = 0x21; cmd[4] = 0x54;
    PutBattleMsg(1, B_COMM_TO_ENGINE, 1, 8, cmd, 0);
    MP_BattleLinkSelftestRunCopy();
    H_CHECK(MP_LinkGetRejectCount() == rejects + 1 && gBattleBufferB[1][1] == PARTY_SIZE, "PARTY_SIZE (nothing chosen) is accepted");
    H_CHECK(gBattleBufferB[1][2] == 0x00 && gBattleBufferB[1][3] == 0x21 && gBattleBufferB[1][4] == 0x54, "party order nibbles >= 6 are cleared, valid ones kept");
    Report("battle engine replies (chosen mon, party order)", failuresBefore);

    MP_BattleLinkSelftestEnd();
    gBattleTypeFlags = savedTypeFlags;
    gBattleControllerExecFlags = savedExecFlags;
    gBattlerAttacker = savedAttacker;
    gBattlerTarget = savedTarget;
    gEffectBattler = savedEffect;
    gAbsentBattlerFlags = savedAbsent;
    memcpy(gBattleBufferA, sSavedBufA, sizeof(sSavedBufA));
    memcpy(gBattleBufferB, sSavedBufB, sizeof(sSavedBufB));
}

// ===========================================================================
// 3. Received structures
// ===========================================================================

static void MakeMon(struct Pokemon *mon)
{
    CreateMon(mon, SPECIES_PIKACHU, 30, 15, TRUE, 0x01020304, OT_ID_PRESET, 0x0000ABCD);
}

static void TestMon(void)
{
    struct Pokemon mon, ref;
    struct BattlePokemon bmon;
    u32 failuresBefore = sFail;
    u32 value, i, accepted = 0;

    MakeMon(&ref);
    mon = ref;
    H_CHECK(MP_LinkValidateMon(&mon) && memcmp(&mon, &ref, sizeof(mon)) == 0, "a valid mon is accepted unchanged");
    memset(&mon, 0, sizeof(mon));
    H_CHECK(MP_LinkValidateMon(&mon) && GetMonData(&mon, MON_DATA_SPECIES) == SPECIES_NONE, "an empty slot is accepted");
    memset(&mon, 0x37, sizeof(mon));
    mon.box.secure.raw[0] ^= 0x1234;
    H_CHECK(!MP_LinkValidateMon(&mon), "garbage (bad checksum) is rejected");
    H_CHECK(GetMonData(&mon, MON_DATA_SPECIES) == SPECIES_NONE && GetMonData(&mon, MON_DATA_LEVEL) == 0, "a rejected mon is zeroed");
    mon = ref;
    mon.box.secure.raw[3] ^= 0x00010000;
    H_CHECK(!MP_LinkValidateMon(&mon), "a flipped bit (checksum) is rejected");

    mon = ref; value = 999;
    SetMonData(&mon, MON_DATA_SPECIES, &value);
    H_CHECK(!MP_LinkValidateMon(&mon), "species 999 is rejected");
    mon = ref; value = ITEMS_COUNT + 5;
    SetMonData(&mon, MON_DATA_HELD_ITEM, &value);
    H_CHECK(!MP_LinkValidateMon(&mon), "a held item past the item table is rejected");
    mon = ref; value = MOVES_COUNT;
    SetMonData(&mon, MON_DATA_MOVE3, &value);
    H_CHECK(!MP_LinkValidateMon(&mon), "a move past the move table is rejected");
    mon = ref; mon.level = 0;
    H_CHECK(!MP_LinkValidateMon(&mon), "level 0 is rejected");
    mon = ref; mon.level = MAX_LEVEL + 1;
    H_CHECK(!MP_LinkValidateMon(&mon), "level 101 is rejected");

    mon = ref; value = 0x4F;
    SetMonData(&mon, MON_DATA_LANGUAGE, &value);
    value = 9;
    SetMonData(&mon, MON_DATA_MAIL, &value);
    mon.hp = mon.maxHP + 50;
    value = 7;
    SetMonData(&mon, MON_DATA_COOL_RIBBON, &value);
    H_CHECK(MP_LinkValidateMon(&mon), "harmless out-of-range fields are accepted");
    H_CHECK(GetMonData(&mon, MON_DATA_LANGUAGE) == LANGUAGE_ENGLISH, "language clamped");
    H_CHECK(GetMonData(&mon, MON_DATA_MAIL) == MAIL_NONE, "mail index clamped (gTradeMail has 6)");
    H_CHECK(GetMonData(&mon, MON_DATA_HP) == GetMonData(&mon, MON_DATA_MAX_HP), "hp clamped to max hp");
    H_CHECK(GetMonData(&mon, MON_DATA_COOL_RIBBON) == 4, "contest ribbon count clamped");
    H_CHECK(MP_LinkValidateMon(&mon) && GetMonData(&mon, MON_DATA_SPECIES) == SPECIES_PIKACHU, "the clamped mon still has a valid checksum");

    // Fuzz: random corruptions are either rejected or leave every table index in range.
    for (i = 0; i < 20000; i++)
    {
        u32 k, n = 1 + Rand() % 8;

        mon = ref;
        for (k = 0; k < n; k++)
            ((u8 *)&mon)[Rand() % sizeof(mon)] ^= (u8)(1 << (Rand() % 8));
        if (MP_LinkValidateMon(&mon))
        {
            accepted++;
            if (GetMonData(&mon, MON_DATA_SPECIES) >= NUM_SPECIES || GetMonData(&mon, MON_DATA_HELD_ITEM) >= ITEMS_COUNT
             || GetMonData(&mon, MON_DATA_MOVE1) >= MOVES_COUNT || GetMonData(&mon, MON_DATA_LEVEL) > MAX_LEVEL)
            {
                H_CHECK(FALSE, "an accepted fuzzed mon has an out-of-range index");
                break;
            }
        }
    }
    H_CHECK(accepted > 0 && accepted < 20000, "the mon fuzz exercised both outcomes");
    Report("R5 MP_LinkValidateMon (checksum, species, item, moves, level, clamps, 20000 fuzz)", failuresBefore);

    failuresBefore = sFail;
    memset(&bmon, 0, sizeof(bmon));
    bmon.species = SPECIES_PIKACHU;
    bmon.moves[0] = MOVE_TACKLE;
    bmon.maxHP = 50;
    bmon.hp = 40;
    bmon.statStages[0] = 99;
    bmon.ability = 0xFF;
    bmon.level = 200;
    memset(bmon.nickname, CHAR_A, sizeof(bmon.nickname));
    H_CHECK(MP_SanitizeBattleMon(&bmon), "a battle mon with only cosmetic damage is accepted");
    H_CHECK(bmon.statStages[0] == DEFAULT_STAT_STAGE && bmon.ability < ABILITIES_COUNT && bmon.level == MAX_LEVEL
         && bmon.nickname[POKEMON_NAME_LENGTH] == EOS, "and silently clamped");
    bmon.moves[2] = 0xFFFF;
    H_CHECK(!MP_SanitizeBattleMon(&bmon) && bmon.moves[2] == MOVE_NONE, "a battle mon with move 0xFFFF is rejected and cleared");
    bmon.species = 0xFFFF;
    H_CHECK(!MP_SanitizeBattleMon(&bmon) && bmon.species == SPECIES_NONE, "a battle mon with species 0xFFFF is rejected and cleared");
    Report("MP_SanitizeBattleMon (GetMonData replies, SETMONDATA)", failuresBefore);
}

static void TestCardMailText(void)
{
    struct TrainerCard card;
    struct Mail mail;
    u32 failuresBefore = sFail;
    u8 savedBuff1[TEXT_BUFF_ARRAY_COUNT];
    u8 src[4];
    u8 out[64];
    u32 savedTypeFlags = gBattleTypeFlags;

    memset(&card, 0, sizeof(card));
    card.stars = 200;
    card.gender = 7;
    memset(card.playerName, CHAR_A, sizeof(card.playerName));
    card.monSpecies[3] = 0xFFFF;
    card.stickers[1] = 9;
    MP_SanitizeTrainerCard(&card);
    H_CHECK(card.stars <= 4 && card.gender <= FEMALE, "trainer card stars / gender clamped");
    H_CHECK(card.playerName[PLAYER_NAME_LENGTH] == EOS && card.monSpecies[3] == SPECIES_NONE && card.stickers[1] == 0, "trainer card name, icon species and sticker clamped");
    card.stars = 3;
    card.gender = FEMALE;
    MP_SanitizeTrainerCard(&card);
    H_CHECK(card.stars == 3 && card.gender == FEMALE, "valid stars / gender kept");
    Report("trainer card stars / gender / FRLG fields", failuresBefore);

    failuresBefore = sFail;
    ClearMail(&mail);
    H_CHECK(MP_SanitizeMail(&mail), "an empty mail is accepted");
    mail.itemId = ITEM_ORANGE_MAIL;
    mail.species = SPECIES_PIKACHU;
    H_CHECK(MP_SanitizeMail(&mail) && mail.itemId == ITEM_ORANGE_MAIL, "a real mail is accepted");
    mail.itemId = ITEM_POTION;
    H_CHECK(!MP_SanitizeMail(&mail) && mail.itemId == ITEM_NONE, "a mail whose item is not mail is cleared");
    ClearMail(&mail);
    mail.itemId = ITEM_ORANGE_MAIL;
    mail.species = 0xFFFF;
    H_CHECK(!MP_SanitizeMail(&mail) && mail.species == SPECIES_NONE, "a mail species past the table is cleared");
    Report("trade mail", failuresBefore);

    // Battle text buffers: an unknown placeholder type used to loop forever, indices past
    // their tables were read. Expand "{B_BUFF1}" with crafted buffers.
    failuresBefore = sFail;
    memcpy(savedBuff1, gBattleTextBuff1, sizeof(savedBuff1));
    gBattleTypeFlags = 0;
    src[0] = PLACEHOLDER_BEGIN;
    src[1] = B_TXT_BUFF1;
    src[2] = EOS;
    memset(gBattleTextBuff1, 0x55, sizeof(savedBuff1)); // unknown type, no terminator
    gBattleTextBuff1[0] = B_BUFF_PLACEHOLDER_BEGIN;
    BattleStringExpandPlaceholders(src, out);
    H_CHECK(TRUE, "an unknown placeholder type terminates"); // reaching here is the check
    gBattleTextBuff1[1] = B_BUFF_MOVE; gBattleTextBuff1[2] = 0xFF; gBattleTextBuff1[3] = 0xFF; gBattleTextBuff1[4] = B_BUFF_EOS;
    BattleStringExpandPlaceholders(src, out);
    H_CHECK(gStringVar1[0] == EOS, "move 0xFFFF expands to nothing");
    gBattleTextBuff1[1] = B_BUFF_MON_NICK; gBattleTextBuff1[2] = 200; gBattleTextBuff1[3] = 200;
    BattleStringExpandPlaceholders(src, out);
    H_CHECK(gStringVar1[0] == EOS, "a nickname of battler 200 / party slot 200 expands to nothing");
    gBattleTextBuff1[1] = B_BUFF_NUMBER; gBattleTextBuff1[2] = 4; gBattleTextBuff1[3] = 200;
    BattleStringExpandPlaceholders(src, out);
    H_CHECK(gStringVar1[0] == EOS, "a 200-digit number expands to nothing");
    gBattleTextBuff1[1] = B_BUFF_MOVE; gBattleTextBuff1[2] = MOVE_TACKLE & 0xFF; gBattleTextBuff1[3] = MOVE_TACKLE >> 8;
    BattleStringExpandPlaceholders(src, out);
    H_CHECK(StringCompare(gStringVar1, gMoveNames[MOVE_TACKLE]) == 0, "a valid move placeholder still expands");
    memcpy(gBattleTextBuff1, savedBuff1, sizeof(savedBuff1));
    gBattleTypeFlags = savedTypeFlags;
    Report("battle text buffer placeholders", failuresBefore);
}

static void TestBusFilter(void)
{
    u16 cmd[MP_LINK_CMD_WORDS];
    u32 failuresBefore = sFail;

    memset(cmd, 0, sizeof(cmd));
    cmd[0] = LINKCMD_INIT_BLOCK; cmd[1] = 0x4000;
    H_CHECK(MpLink_IsCmdAllowed(cmd), "bus: INIT_BLOCK of 0x4000 allowed");
    cmd[1] = 0x4001;
    H_CHECK(!MpLink_IsCmdAllowed(cmd), "bus: INIT_BLOCK of 0x4001 refused");
    cmd[1] = 0xFFFF;
    H_CHECK(!MpLink_IsCmdAllowed(cmd), "bus: INIT_BLOCK of 0xFFFF refused");
    cmd[0] = LINKCMD_SEND_BLOCK_REQ; cmd[1] = BLOCK_REQ_SIZE_40;
    H_CHECK(MpLink_IsCmdAllowed(cmd), "bus: SEND_BLOCK_REQ type 4 allowed");
    cmd[1] = BLOCK_REQ_SIZE_40 + 1;
    H_CHECK(!MpLink_IsCmdAllowed(cmd), "bus: SEND_BLOCK_REQ type 5 refused");
    cmd[0] = LINKCMD_CONT_BLOCK; cmd[1] = 0xFFFF;
    H_CHECK(MpLink_IsCmdAllowed(cmd), "bus: block data is not filtered");
    Report("host bus pre-filter (MpLink_IsCmdAllowed)", failuresBefore);
}

u32 MpSelftest_LinkHardening(void)
{
    u32 need = MP_LinkSelftestState(NULL, 0, FALSE);

    sFail = 0;
    H_CHECK(need <= STATE_CAP, "link.c state fits the selftest buffer");
    if (need > STATE_CAP)
        return sFail;

    // Save everything this group touches.
    MP_LinkSelftestState(sSavedLink, sizeof(sSavedLink), FALSE);
    memcpy(sSavedDecompression, gDecompressionBuffer, sizeof(sSavedDecompression));
    sSavedPtr1 = gSaveBlock1Ptr;
    sSavedPtr2 = gSaveBlock2Ptr;
    sSavedCallback2 = gMain.callback2;
    sSavedMainState = gMain.state;
    memset(&sFixtureSave1, 0, sizeof(sFixtureSave1));
    memset(&sFixtureSave2, 0, sizeof(sFixtureSave2));
    memset(sFixtureSave2.playerName, EOS, sizeof(sFixtureSave2.playerName));
    gSaveBlock1Ptr = &sFixtureSave1;
    gSaveBlock2Ptr = &sFixtureSave2;

    // The clean starting point: link.c as booted, the cable "up", single wired link.
    gWirelessCommType = 0;
    gLink.state = LINK_STATE_CONN_ESTABLISHED;
    gLink.hardwareError = FALSE;
    gLinkCallback = NULL;
    MP_LinkSelftestState(sCleanLink, sizeof(sCleanLink), FALSE);

    TestBlocks();
    TestBlockRequest();
    TestLinkPlayer();
    TestRecvFuzz();
    TestBattle();
    TestMon();
    TestCardMailText();
    TestBusFilter();

    MP_LinkSelftestState(sSavedLink, sizeof(sSavedLink), TRUE);
    memcpy(gDecompressionBuffer, sSavedDecompression, sizeof(sSavedDecompression));
    gSaveBlock1Ptr = sSavedPtr1;
    gSaveBlock2Ptr = sSavedPtr2;
    gMain.callback2 = sSavedCallback2;
    gMain.state = sSavedMainState;
    if (sFail == 0)
        MP_LOG("selftest: hardening: all groups OK\n");
    return sFail;
}
