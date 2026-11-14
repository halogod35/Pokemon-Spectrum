// PS-080: --mg-selftest. Runs the built-in Mystery Gift distributor headless (no window, no
// save file; the save blocks are zeroed in memory) and exits 0 (OK) or 1 (FAILED). Checks:
//   - every gift script is non-empty and fits the saved RAM script (995 bytes);
//   - a fresh game is offered the Eon Ticket, the card and its RAM script are saved and valid
//     (ValidateSavedWonderCard, GetSavedRamScriptIfValid), the RAM script is the gift script;
//   - asking again before collecting gives "already had this card";
//   - once a ticket is owned (received flag, bag, ship flag) the next one is offered, through
//     the game's toss prompt: keeping the old card cancels, tossing it saves the new one;
//   - with all four owned the "no more gifts" message comes back and nothing is saved;
//   - Wonder News gets its "no news" message and nothing is saved;
//   - refused blocks on the loopback (size over the buffer, wrong ident, bad CRC) flag a failure
//     and nothing crashes; a well-formed block is accepted;
//   - a client script with an instruction the client does not run (CLI_RUN_BUFFER_SCRIPT, an
//     unknown one) or without CLI_RETURN ends with the communication error (AD-9).
#include <stdio.h>
#include <string.h>
#include "global.h"
#include "event_data.h"
#include "item.h"
#include "load_save.h"
#include "malloc.h"
#include "pokemon_storage_system.h"
#include "script.h"
#include "string_util.h"
#include "util.h"
#include "mystery_gift.h"
#include "mystery_gift_link.h"
#include "mystery_gift_client.h"
#include "mystery_gift_server.h"
#include "mystery_gift_distributor.h"
#include "constants/flags.h"
#include "constants/items.h"
#include "constants/mystery_gift.h"

static u32 sFailures;

#define MG_CHECK(cond)                                                                      \
    do                                                                                      \
    {                                                                                       \
        if (!(cond))                                                                        \
        {                                                                                   \
            printf("[mg] selftest: FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__);         \
            sFailures++;                                                                    \
        }                                                                                   \
    } while (0)

static void Report(const char *group, u32 failuresBefore)
{
    if (sFailures == failuresBefore)
        printf("[mg] selftest: %s OK\n", group);
    else
        printf("[mg] selftest: %s FAILED (%u)\n", group, (unsigned int)(sFailures - failuresBefore));
}

// Zeroed save blocks with a terminated player name (MysteryGift_LoadLinkGameData copies it).
static void ResetGameState(void)
{
    static const u8 sName[] = _("TESTER");

    SetSaveBlocksPointers(0);
    memset(gSaveBlock2Ptr, 0, sizeof(*gSaveBlock2Ptr));
    memset(gSaveBlock1Ptr, 0, sizeof(*gSaveBlock1Ptr));
    memset(gPokemonStoragePtr, 0, sizeof(*gPokemonStoragePtr));
    StringCopy(gSaveBlock2Ptr->playerName, sName);
    ClearMysteryGift();
    ClearRamScript();
}

enum {
    SESSION_ENDED,   // the client reached CLI_RETURN and the server returned too
    SESSION_FAILED,  // a block was refused on the loopback
    SESSION_TIMEOUT, // nobody finished within the frame budget
};

#define SESSION_FRAME_BUDGET 2000

// Drives one Mystery Gift session the way Task_MysteryGift does, one client step and one server
// step per "frame". `keepOldCard` is the answer to the toss prompt (TRUE = keep it, cancel).
static u32 RunSession(bool32 isWonderNews, bool32 keepOldCard, u16 *endMsg, u8 *msgOut)
{
    u32 frame;
    u32 ret = CLI_RET_INIT;

    *endMsg = 0xFFFF;
    msgOut[0] = EOS;
    MysteryGiftDistributor_Start(isWonderNews);
    MysteryGiftClient_Create(isWonderNews);
    for (frame = 0; frame < SESSION_FRAME_BUDGET; frame++)
    {
        MysteryGiftDistributor_Tick();
        if (MysteryGiftDistributor_Failed())
        {
            MysteryGiftDistributor_Stop();
            return SESSION_FAILED;
        }
        ret = MysteryGiftClient_Run(endMsg);
        if (ret == CLI_RET_END)
            break;
        switch (ret)
        {
        case CLI_RET_COPY_MSG:
            memcpy(msgOut, MysteryGiftClient_GetMsg(), CLIENT_MAX_MSG_SIZE);
            MysteryGiftClient_AdvanceState();
            break;
        case CLI_RET_PRINT_MSG:
        case CLI_RET_YES_NO:
            MysteryGiftClient_SetParam(FALSE);
            MysteryGiftClient_AdvanceState();
            break;
        case CLI_RET_ASK_TOSS:
            // MG_STATE_CLIENT_ASK_TOSS: Yes (toss) sends FALSE, No (keep) sends TRUE
            MysteryGiftClient_SetParam(keepOldCard ? TRUE : FALSE);
            MysteryGiftClient_AdvanceState();
            break;
        }
    }
    if (ret != CLI_RET_END)
    {
        MysteryGiftDistributor_Stop();
        return SESSION_TIMEOUT;
    }
    // The server still has the client's READY_END to read.
    for (frame = 0; frame < 100 && MysterGiftServer_IsActive(); frame++)
        MysteryGiftDistributor_Tick();
    MG_CHECK(!MysterGiftServer_IsActive());
    MG_CHECK(MysteryGiftDistributor_Finish());
    MG_CHECK(!MysteryGiftDistributor_IsRunning());
    MG_CHECK(!MysteryGiftLink_IsLoopback());
    return SESSION_ENDED;
}

// The saved card is `gift`'s card and the saved RAM script is `gift`'s script, zero-padded.
static void CheckSavedGift(const struct MysteryGiftDistributorGift *gift)
{
    const u8 *ramScript;
    u32 size = gift->scriptEnd - gift->script;
    u32 i;

    MG_CHECK(ValidateSavedWonderCard());
    MG_CHECK(GetSavedWonderCard()->flagId == gift->card->flagId);
    MG_CHECK(GetWonderCardFlagID() == gift->card->flagId);
    MG_CHECK(memcmp(GetSavedWonderCard()->titleText, gift->card->titleText, WONDER_CARD_TEXT_LENGTH) == 0);
    ramScript = GetSavedRamScriptIfValid();
    MG_CHECK(ramScript != NULL);
    if (ramScript != NULL)
    {
        MG_CHECK(memcmp(ramScript, gift->script, size) == 0);
        for (i = size; i < sizeof(gSaveBlock1Ptr->ramScript.data.script); i++)
        {
            if (ramScript[i] != 0)
            {
                MG_CHECK(ramScript[i] == 0); // "padding after the gift script is zero"
                break;
            }
        }
    }
}

static void TestScriptSizes(void)
{
    u32 before = sFailures;
    u32 i;

    for (i = 0; i < MysteryGiftDistributor_GetGiftCount(); i++)
    {
        const struct MysteryGiftDistributorGift *gift = MysteryGiftDistributor_GetGift(i);
        u32 size = gift->scriptEnd - gift->script;

        printf("[mg] selftest: gift %u (flagId %u): script %u bytes\n", (unsigned int)i,
               (unsigned int)gift->card->flagId, (unsigned int)size);
        MG_CHECK(gift->scriptEnd > gift->script);
        MG_CHECK(size < sizeof(gSaveBlock1Ptr->ramScript.data.script));
        MG_CHECK(gift->card->flagId >= WONDER_CARD_FLAG_OFFSET && gift->card->flagId < WONDER_CARD_FLAG_OFFSET + NUM_WONDER_CARD_FLAGS);
        MG_CHECK(gift->card->type == CARD_TYPE_GIFT);
        MG_CHECK(gift->card->sendType == SEND_TYPE_DISALLOWED);
        MG_CHECK(gift->card->bgType < NUM_WONDER_BGS);
    }
    MG_CHECK(MysteryGiftDistributor_GetGift(MysteryGiftDistributor_GetGiftCount()) == NULL);
    Report("gift scripts", before);
}

static void TestDistribution(void)
{
    u32 before = sFailures;
    u16 endMsg;
    u8 msg[CLIENT_MAX_MSG_SIZE];
    const struct MysteryGiftDistributorGift *eon = MysteryGiftDistributor_GetGift(0);
    const struct MysteryGiftDistributorGift *aurora = MysteryGiftDistributor_GetGift(1);
    const struct MysteryGiftDistributorGift *mystic = MysteryGiftDistributor_GetGift(2);
    const struct MysteryGiftDistributorGift *oldSeaMap = MysteryGiftDistributor_GetGift(3);

    ResetGameState();
    MG_CHECK(!ValidateSavedWonderCard());
    MG_CHECK(MysteryGiftDistributor_NextGift() == 0);

    // 1. A fresh game receives the Eon Ticket card.
    MG_CHECK(RunSession(FALSE, FALSE, &endMsg, msg) == SESSION_ENDED);
    MG_CHECK(endMsg == CLI_MSG_CARD_RECEIVED);
    CheckSavedGift(eon);
    MG_CHECK(IsSavedWonderCardGiftNotReceived() == TRUE); // the toss prompt's second question applies
    MG_CHECK(MysteryGiftDistributor_NextGift() == 0);      // not collected yet

    // 2. Asking again before collecting: "already had this card".
    MG_CHECK(RunSession(FALSE, FALSE, &endMsg, msg) == SESSION_ENDED);
    MG_CHECK(endMsg == CLI_MSG_HAD_CARD);
    CheckSavedGift(eon);

    // 3. Collected (the gift script sets the received flag): the Aurora Ticket is next. Keeping
    //    the old card at the toss prompt cancels; nothing changes.
    FlagSet(FLAG_RECEIVED_EON_TICKET);
    MG_CHECK(IsSavedWonderCardGiftNotReceived() == FALSE);
    MG_CHECK(MysteryGiftDistributor_NextGift() == 1);
    MG_CHECK(RunSession(FALSE, TRUE, &endMsg, msg) == SESSION_ENDED);
    MG_CHECK(endMsg == CLI_MSG_BUFFER_FAILURE);
    MG_CHECK(msg[0] != EOS); // "Canceled reading the Card."
    CheckSavedGift(eon);

    // 4. Tossing it saves the Aurora Ticket card.
    MG_CHECK(RunSession(FALSE, FALSE, &endMsg, msg) == SESSION_ENDED);
    MG_CHECK(endMsg == CLI_MSG_CARD_RECEIVED);
    CheckSavedGift(aurora);

    // 5. The ticket in the bag counts as owned: the Mystic Ticket is next.
    MG_CHECK(AddBagItem(ITEM_AURORA_TICKET, 1));
    MG_CHECK(MysteryGiftDistributor_NextGift() == 2);
    MG_CHECK(RunSession(FALSE, FALSE, &endMsg, msg) == SESSION_ENDED);
    MG_CHECK(endMsg == CLI_MSG_CARD_RECEIVED);
    CheckSavedGift(mystic);

    // 6. The ship flag counts as owned: the Old Sea Map is next.
    FlagSet(FLAG_ENABLE_SHIP_NAVEL_ROCK);
    MG_CHECK(MysteryGiftDistributor_NextGift() == 3);
    MG_CHECK(RunSession(FALSE, FALSE, &endMsg, msg) == SESSION_ENDED);
    MG_CHECK(endMsg == CLI_MSG_CARD_RECEIVED);
    CheckSavedGift(oldSeaMap);

    // 7. All four owned: "There are no more gifts right now.", the saved card is untouched.
    FlagSet(FLAG_RECEIVED_OLD_SEA_MAP);
    MG_CHECK(MysteryGiftDistributor_NextGift() == -1);
    MG_CHECK(RunSession(FALSE, FALSE, &endMsg, msg) == SESSION_ENDED);
    MG_CHECK(endMsg == CLI_MSG_BUFFER_FAILURE);
    MG_CHECK(msg[0] != EOS);
    CheckSavedGift(oldSeaMap);

    // 8. Wonder News: "There is no WONDER NEWS right now.", nothing saved.
    MG_CHECK(RunSession(TRUE, FALSE, &endMsg, msg) == SESSION_ENDED);
    MG_CHECK(endMsg == CLI_MSG_BUFFER_FAILURE);
    MG_CHECK(msg[0] != EOS);
    MG_CHECK(!ValidateSavedWonderNews());
    CheckSavedGift(oldSeaMap);

    Report("distribution", before);
}

// Puts a block for the client (pipe 0) with the given header, then runs the client. The block is
// a client script that ends, so TRUE means the client accepted it and ran it to CLI_RETURN
// (`endMsg` is its result); FALSE means the block was refused (or the client never ended).
static u32 FeedClient(u16 ident, u16 crc, u16 size, const void *data, u32 dataSize, u16 *endMsg)
{
    u32 frame;

    MG_CHECK(MysteryGiftLink_LoopbackInject(0, ident, crc, size, data, dataSize));
    for (frame = 0; frame < SESSION_FRAME_BUDGET; frame++)
    {
        if (MysteryGiftLink_LoopbackFailed())
            return FALSE;
        if (MysteryGiftClient_Run(endMsg) == CLI_RET_END)
            return TRUE;
    }
    return FALSE;
}

// Runs the client until it ends or the budget is spent.
static u32 RunClientToEnd(u16 *endMsg)
{
    u32 frame;

    for (frame = 0; frame < SESSION_FRAME_BUDGET; frame++)
    {
        if (MysteryGiftClient_Run(endMsg) == CLI_RET_END)
            return TRUE;
    }
    return FALSE;
}

static void StartRawClient(void)
{
    MysteryGiftDistributor_Stop();
    MysteryGiftLink_StartLoopback();
    MysteryGiftClient_Create(FALSE);
}

static void TestRefusedBlocks(void)
{
    u32 before = sFailures;
    u16 endMsg;
    struct MysteryGiftClientCmd script[2];
    u16 crc;

    memset(script, 0, sizeof(script));
    script[0].instr = CLI_RETURN;
    script[0].parameter = CLI_MSG_NOTHING_SENT;
    crc = CalcCRC16WithTable((const u8 *)script, sizeof(script[0]));

    // Size over the buffer (the receiver must refuse before reading it).
    StartRawClient();
    MG_CHECK(!FeedClient(MG_LINKID_CLIENT_SCRIPT, crc, MG_LINK_BUFFER_SIZE + 1, script, sizeof(script[0]), &endMsg));
    MG_CHECK(MysteryGiftLink_LoopbackFailed());
    MG_CHECK(MysteryGiftClient_IsActive()); // the client is still there; the menu tears it down
    MysteryGiftDistributor_Stop();
    MG_CHECK(!MysteryGiftClient_IsActive());
    MG_CHECK(!MysteryGiftLink_LoopbackFailed());

    // Zero size.
    StartRawClient();
    MG_CHECK(!FeedClient(MG_LINKID_CLIENT_SCRIPT, crc, 0, script, sizeof(script[0]), &endMsg));
    MG_CHECK(MysteryGiftLink_LoopbackFailed());
    MysteryGiftDistributor_Stop();

    // Wrong ident (the client is waiting for MG_LINKID_CLIENT_SCRIPT).
    StartRawClient();
    MG_CHECK(!FeedClient(MG_LINKID_CARD, crc, sizeof(script[0]), script, sizeof(script[0]), &endMsg));
    MG_CHECK(MysteryGiftLink_LoopbackFailed());
    MysteryGiftDistributor_Stop();

    // Bad CRC.
    StartRawClient();
    MG_CHECK(!FeedClient(MG_LINKID_CLIENT_SCRIPT, crc ^ 0x5555, sizeof(script[0]), script, sizeof(script[0]), &endMsg));
    MG_CHECK(MysteryGiftLink_LoopbackFailed());
    MysteryGiftDistributor_Stop();

    // A well-formed block is accepted: the client runs the script and returns NOTHING_SENT.
    StartRawClient();
    MG_CHECK(FeedClient(MG_LINKID_CLIENT_SCRIPT, crc, sizeof(script[0]), script, sizeof(script[0]), &endMsg));
    MG_CHECK(!MysteryGiftLink_LoopbackFailed());
    MG_CHECK(endMsg == CLI_MSG_NOTHING_SENT);
    MG_CHECK(!MysteryGiftClient_IsActive());
    MysteryGiftDistributor_Stop();

    // Injecting into a pipe that does not exist, or more data than the buffer, is refused.
    MysteryGiftLink_StartLoopback();
    MG_CHECK(!MysteryGiftLink_LoopbackInject(2, MG_LINKID_CLIENT_SCRIPT, crc, 8, script, 8));
    MG_CHECK(!MysteryGiftLink_LoopbackInject(0, MG_LINKID_CLIENT_SCRIPT, crc, 8, script, MG_LINK_BUFFER_SIZE + 1));
    MysteryGiftLink_StopLoopback();
    MG_CHECK(!MysteryGiftLink_LoopbackInject(0, MG_LINKID_CLIENT_SCRIPT, crc, 8, script, 8)); // not enabled

    Report("refused blocks", before);
}

// AD-9: the client never runs received code, and a script without CLI_RETURN ends.
static void TestClientNeverExecutes(void)
{
    u32 before = sFailures;
    u16 endMsg;
    struct MysteryGiftClientCmd script[MG_LINK_BUFFER_SIZE / sizeof(struct MysteryGiftClientCmd)];
    u16 crc;

    // CLI_RUN_BUFFER_SCRIPT (then a CLI_RETURN that must never be reached).
    memset(script, 0, sizeof(script));
    script[0].instr = CLI_RUN_BUFFER_SCRIPT;
    script[1].instr = CLI_RETURN;
    script[1].parameter = CLI_MSG_CARD_RECEIVED;
    crc = CalcCRC16WithTable((const u8 *)script, sizeof(script[0]) * 2);
    StartRawClient();
    MG_CHECK(FeedClient(MG_LINKID_CLIENT_SCRIPT, crc, sizeof(script[0]) * 2, script, sizeof(script[0]) * 2, &endMsg));
    MG_CHECK(endMsg == CLI_MSG_COMM_ERROR);
    MysteryGiftDistributor_Stop();

    // CLI_RUN_MEVENT_SCRIPT.
    script[0].instr = CLI_RUN_MEVENT_SCRIPT;
    crc = CalcCRC16WithTable((const u8 *)script, sizeof(script[0]) * 2);
    StartRawClient();
    MG_CHECK(FeedClient(MG_LINKID_CLIENT_SCRIPT, crc, sizeof(script[0]) * 2, script, sizeof(script[0]) * 2, &endMsg));
    MG_CHECK(endMsg == CLI_MSG_COMM_ERROR);
    MysteryGiftDistributor_Stop();

    // CLI_RECV_EREADER_TRAINER.
    script[0].instr = CLI_RECV_EREADER_TRAINER;
    crc = CalcCRC16WithTable((const u8 *)script, sizeof(script[0]) * 2);
    StartRawClient();
    MG_CHECK(FeedClient(MG_LINKID_CLIENT_SCRIPT, crc, sizeof(script[0]) * 2, script, sizeof(script[0]) * 2, &endMsg));
    MG_CHECK(endMsg == CLI_MSG_COMM_ERROR);
    MysteryGiftDistributor_Stop();

    // An instruction the client does not know.
    script[0].instr = 0x7FFFFFFF;
    crc = CalcCRC16WithTable((const u8 *)script, sizeof(script[0]) * 2);
    StartRawClient();
    MG_CHECK(FeedClient(MG_LINKID_CLIENT_SCRIPT, crc, sizeof(script[0]) * 2, script, sizeof(script[0]) * 2, &endMsg));
    MG_CHECK(endMsg == CLI_MSG_COMM_ERROR);
    MysteryGiftDistributor_Stop();

    // A full buffer of CLI_NONE, no CLI_RETURN: the command index stops at the buffer's end.
    memset(script, 0, sizeof(script));
    crc = CalcCRC16WithTable((const u8 *)script, sizeof(script));
    StartRawClient();
    MG_CHECK(MysteryGiftLink_LoopbackInject(0, MG_LINKID_CLIENT_SCRIPT, crc, sizeof(script), script, sizeof(script)));
    MG_CHECK(RunClientToEnd(&endMsg));
    MG_CHECK(endMsg == CLI_MSG_COMM_ERROR);
    MysteryGiftDistributor_Stop();

    Report("client never executes received code", before);
}

s32 MysteryGiftSelftest_Run(void)
{
    sFailures = 0;
    InitHeap(gHeap, HEAP_SIZE);
    ResetGameState();

    TestScriptSizes();
    TestDistribution();
    TestRefusedBlocks();
    TestClientNeverExecutes();

    MysteryGiftDistributor_Stop();
    if (sFailures == 0)
    {
        printf("[mg] selftest OK\n");
        return 0;
    }
    printf("[mg] selftest FAILED (%u)\n", (unsigned int)sFailures);
    return 1;
}
