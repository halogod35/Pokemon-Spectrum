#ifndef GUARD_MYSTERY_GIFT_LINK_H
#define GUARD_MYSTERY_GIFT_LINK_H

#define MG_LINK_BUFFER_SIZE 0x400

// Send/receive ids for the Client/Server to make sure
// they're sending/receiving the same thing
enum {
    MG_LINKID_CLIENT_SCRIPT = 16,
    MG_LINKID_GAME_DATA,
    MG_LINKID_GAME_STAT,
    MG_LINKID_RESPONSE,
    MG_LINKID_READY_END,
    MG_LINKID_DYNAMIC_MSG,
    MG_LINKID_CARD,
    MG_LINKID_NEWS,
    MG_LINKID_STAMP,
    MG_LINKID_RAM_SCRIPT,
    MG_LINKID_EREADER_TRAINER,
    MG_LINKID_UNK_1,
    MG_LINKID_UNK_2,
};

struct MysteryGiftLink
{
    s32 state;
    u8 sendPlayerId;
    u8 recvPlayerId;
    u16 recvIdent;
    u16 recvCounter;
    u16 recvCRC;
    u16 recvSize;
    u16 sendIdent;
    u16 sendCounter;
    u16 sendCRC;
    u16 sendSize;
    void *recvBuffer;
    const void *sendBuffer;
    u32 (*recvFunc)(struct MysteryGiftLink *);
    u32 (*sendFunc)(struct MysteryGiftLink *);
};

void MysteryGiftLink_Init(struct MysteryGiftLink *link, u32 sendPlayerId, u32 recvPlayerId);
void MysteryGiftLink_InitSend(struct MysteryGiftLink *link, u32 ident, const void *src, u32 size);
u32 MysteryGiftLink_Recv(struct MysteryGiftLink *link);
u32 MysteryGiftLink_Send(struct MysteryGiftLink *link);
void MysteryGiftLink_InitRecv(struct MysteryGiftLink *link, u32 ident, void *dest);

// PS-080: in-process loopback transport, used by the built-in distributor (no wireless adapter,
// no network). While it is enabled, MysteryGiftLink_Init installs the loopback send/recv
// functions on every link created, so the client and the server in the same process talk
// through two one-block pipes in memory instead of the RFU block queues.
void MysteryGiftLink_StartLoopback(void);
void MysteryGiftLink_StopLoopback(void);
bool32 MysteryGiftLink_IsLoopback(void);
// TRUE once a block was refused (bad size, wrong ident, bad CRC). The wireless link calls
// LinkRfu_FatalError there, which the PC cannot run; the menu ends the session instead.
bool32 MysteryGiftLink_LoopbackFailed(void);
// TRUE when neither pipe holds an unread block.
bool32 MysteryGiftLink_LoopbackIdle(void);
// Test hook (--mg-selftest): puts a raw block, header fields as given, into the pipe that the
// link with recvPlayerId `recvPlayerId` reads from. `size` may be deliberately wrong; `dataSize`
// (<= MG_LINK_BUFFER_SIZE) is how much of `data` is copied.
bool32 MysteryGiftLink_LoopbackInject(u32 recvPlayerId, u16 ident, u16 crc, u16 size, const void *data, u32 dataSize);

#endif //GUARD_MYSTERY_GIFT_LINK_H
