#include "global.h"
#include "malloc.h"
#include "decompress.h"
#include "util.h"
#include "link.h"
#include "link_rfu.h"
#include "overworld.h"
#include "script.h"
#include "battle_tower.h"
#include "mystery_event_script.h"
#include "mystery_gift.h"
#include "mystery_gift_link.h"

/*
    Handles the link connection functions used by the Mystery Gift client/server.
    Note: MysteryGiftLink is shortened to MGL for internal functions.
*/

struct SendRecvHeader
{
    u16 ident;
    u16 crc;
    u16 size;
};

static u32 MGL_Receive(struct MysteryGiftLink *);
static u32 MGL_Send(struct MysteryGiftLink *);
static u32 MGL_LoopbackReceive(struct MysteryGiftLink *); // PS-080
static u32 MGL_LoopbackSend(struct MysteryGiftLink *);    // PS-080

// PS-080: the in-process loopback. One block per direction, with the same header fields as the
// wireless transport (ident, CRC16, size) so the receiver checks exactly what it would check
// over the air. The client link is created with (send 1, recv 0) and the server link with
// (send 0, recv 1): a link writes the pipe of its sendPlayerId and reads the pipe of its
// recvPlayerId, so pipe 1 carries client -> server and pipe 0 carries server -> client.
struct MGLoopbackPipe
{
    bool8 full;
    u16 ident;
    u16 crc;
    u16 size;
    u8 data[MG_LINK_BUFFER_SIZE];
};

static struct MGLoopbackPipe sLoopbackPipes[2];
static bool8 sLoopbackEnabled = FALSE;
static bool8 sLoopbackFailed = FALSE;

u32 MysteryGiftLink_Recv(struct MysteryGiftLink *link)
{
    return link->recvFunc(link);
}

u32 MysteryGiftLink_Send(struct MysteryGiftLink *link)
{
    return link->sendFunc(link);
}

void MysteryGiftLink_Init(struct MysteryGiftLink *link, u32 sendPlayerId, u32 recvPlayerId)
{
    link->sendPlayerId = sendPlayerId;
    link->recvPlayerId = recvPlayerId;
    link->state = 0;
    link->sendCRC = 0;
    link->sendSize = 0;
    link->sendCounter = 0;
    link->recvCRC = 0;
    link->recvSize = 0;
    link->recvCounter = 0;
    link->sendBuffer = NULL;
    link->recvBuffer = NULL;
    if (sLoopbackEnabled) // PS-080: the built-in distributor's in-process link
    {
        link->sendFunc = MGL_LoopbackSend;
        link->recvFunc = MGL_LoopbackReceive;
    }
    else
    {
        link->sendFunc = MGL_Send;
        link->recvFunc = MGL_Receive;
    }
}

void MysteryGiftLink_StartLoopback(void)
{
    memset(sLoopbackPipes, 0, sizeof(sLoopbackPipes));
    sLoopbackEnabled = TRUE;
    sLoopbackFailed = FALSE;
}

void MysteryGiftLink_StopLoopback(void)
{
    memset(sLoopbackPipes, 0, sizeof(sLoopbackPipes));
    sLoopbackEnabled = FALSE;
    sLoopbackFailed = FALSE;
}

bool32 MysteryGiftLink_IsLoopback(void)
{
    return sLoopbackEnabled;
}

bool32 MysteryGiftLink_LoopbackFailed(void)
{
    return sLoopbackFailed;
}

bool32 MysteryGiftLink_LoopbackIdle(void)
{
    return !sLoopbackPipes[0].full && !sLoopbackPipes[1].full;
}

bool32 MysteryGiftLink_LoopbackInject(u32 recvPlayerId, u16 ident, u16 crc, u16 size, const void *data, u32 dataSize)
{
    struct MGLoopbackPipe *pipe;

    if (!sLoopbackEnabled || recvPlayerId >= ARRAY_COUNT(sLoopbackPipes) || dataSize > MG_LINK_BUFFER_SIZE)
        return FALSE;
    pipe = &sLoopbackPipes[recvPlayerId];
    memset(pipe->data, 0, sizeof(pipe->data));
    if (data != NULL && dataSize != 0)
        memcpy(pipe->data, data, dataSize);
    pipe->ident = ident;
    pipe->crc = crc;
    pipe->size = size;
    pipe->full = TRUE;
    return TRUE;
}

// Sends the whole block at once. Returns FALSE (try again next frame) while the other side has
// not read the previous block, which is all the flow control the one-slot pipe needs.
static u32 MGL_LoopbackSend(struct MysteryGiftLink *link)
{
    struct MGLoopbackPipe *pipe = &sLoopbackPipes[link->sendPlayerId & 1];

    if (sLoopbackFailed || pipe->full)
        return FALSE;
    if (link->sendBuffer == NULL || link->sendSize == 0 || link->sendSize > MG_LINK_BUFFER_SIZE)
    {
        sLoopbackFailed = TRUE;
        return FALSE;
    }
    memcpy(pipe->data, link->sendBuffer, link->sendSize);
    pipe->ident = link->sendIdent;
    pipe->size = link->sendSize;
    pipe->crc = CalcCRC16WithTable(pipe->data, pipe->size);
    pipe->full = TRUE;
    link->sendCRC = pipe->crc;
    link->sendCounter = 0;
    link->state = 0;
    return TRUE;
}

// Receives the whole block at once, with the wireless transport's checks: the size must fit the
// buffer, the ident must be the one the script is waiting for, and the CRC must match. The
// wireless code calls LinkRfu_FatalError on a failure; that dereferences RFU state the PC never
// initialises, so the failure is flagged and the menu ends the session.
static u32 MGL_LoopbackReceive(struct MysteryGiftLink *link)
{
    struct MGLoopbackPipe *pipe = &sLoopbackPipes[link->recvPlayerId & 1];

    if (sLoopbackFailed || !pipe->full)
        return FALSE;
    pipe->full = FALSE;
    if (link->recvBuffer == NULL
     || pipe->size == 0
     || pipe->size > MG_LINK_BUFFER_SIZE
     || pipe->ident != link->recvIdent
     || CalcCRC16WithTable(pipe->data, pipe->size) != pipe->crc)
    {
        sLoopbackFailed = TRUE;
        return FALSE;
    }
    memcpy(link->recvBuffer, pipe->data, pipe->size);
    // Both the client and the server receive into MG_LINK_BUFFER_SIZE buffers (mystery_gift_client.c,
    // mystery_gift_server.c). Clear the rest, so a short block (a gift script) is not followed by
    // the previous block's bytes when it is saved as the RAM script.
    memset((u8 *)link->recvBuffer + pipe->size, 0, MG_LINK_BUFFER_SIZE - pipe->size);
    link->recvSize = pipe->size;
    link->recvCRC = pipe->crc;
    link->recvCounter = 0;
    link->state = 0;
    return TRUE;
}

void MysteryGiftLink_InitSend(struct MysteryGiftLink *link, u32 ident, const void *src, u32 size)
{
    link->state = 0;
    link->sendIdent = ident;
    link->sendCounter = 0;
    link->sendCRC = 0;
    if (size != 0)
        link->sendSize = size;
    else
        link->sendSize = MG_LINK_BUFFER_SIZE;
    link->sendBuffer = src;
}

void MysteryGiftLink_InitRecv(struct MysteryGiftLink *link, u32 ident, void *dest)
{
    link->state = 0;
    link->recvIdent = ident;
    link->recvCounter = 0;
    link->recvCRC = 0;
    link->recvSize = 0;
    link->recvBuffer = dest;
}

static void MGL_ReceiveBlock(u32 playerId, void *dest, size_t size)
{
    memcpy(dest, gBlockRecvBuffer[playerId], size);
}

static bool32 MGL_HasReceived(u32 playerId)
{
    if ((GetBlockReceivedStatus() >> playerId) & 1)
        return TRUE;
    else
        return FALSE;
}

static void MGL_ResetReceived(u32 playerId)
{
    ResetBlockReceivedFlag(playerId);
}

static bool32 MGL_Receive(struct MysteryGiftLink *link)
{
    struct SendRecvHeader header;

    switch (link->state)
    {
    case 0:
        if (MGL_HasReceived(link->recvPlayerId))
        {
            MGL_ReceiveBlock(link->recvPlayerId, &header, sizeof(header));
            link->recvSize = header.size;
            link->recvCRC = header.crc;
            if (link->recvSize > MG_LINK_BUFFER_SIZE)
            {
                LinkRfu_FatalError();
                return FALSE;
            }
            else if (link->recvIdent != header.ident)
            {
                LinkRfu_FatalError();
                return FALSE;
            }
            else
            {
                link->recvCounter = 0;
                MGL_ResetReceived(link->recvPlayerId);
                link->state++;
            }
        }
        break;
    case 1:
        if (MGL_HasReceived(link->recvPlayerId))
        {
            size_t blocksize = link->recvCounter * 252;
            if (link->recvSize - blocksize <= 252)
            {
                MGL_ReceiveBlock(link->recvPlayerId, link->recvBuffer + blocksize, link->recvSize - blocksize);
                link->recvCounter++;
                link->state++;
            }
            else
            {
                MGL_ReceiveBlock(link->recvPlayerId, link->recvBuffer + blocksize, 252);
                link->recvCounter++;
            }
            MGL_ResetReceived(link->recvPlayerId);
        }
        break;
    case 2:
        if (CalcCRC16WithTable(link->recvBuffer, link->recvSize) != link->recvCRC)
        {
            LinkRfu_FatalError();
            return FALSE;
        }
        else
        {
            link->state = 0;
            return TRUE;
        }
        break;
    }

    return FALSE;
}

static bool32 MGL_Send(struct MysteryGiftLink *link)
{
    struct SendRecvHeader header;

    switch (link->state)
    {
    case 0:
        if (IsLinkTaskFinished())
        {
            header.ident = link->sendIdent;
            header.size = link->sendSize;
            header.crc = CalcCRC16WithTable(link->sendBuffer, link->sendSize);
            link->sendCRC = header.crc;
            link->sendCounter = 0;
            SendBlock(0, &header, sizeof(header));
            link->state++;
        }
        break;
    case 1:
        if (IsLinkTaskFinished())
        {
            if (MGL_HasReceived(link->sendPlayerId))
            {
                size_t blocksize;
                MGL_ResetReceived(link->sendPlayerId);
                blocksize = 252 * link->sendCounter;
                if (link->sendSize - blocksize <= 252)
                {
                    SendBlock(0, link->sendBuffer + blocksize, link->sendSize - blocksize);
                    link->sendCounter++;
                    link->state++;
                }
                else
                {
                    SendBlock(0, link->sendBuffer + blocksize, 252);
                    link->sendCounter++;
                }
            }
        }
        break;
    case 2:
        if (IsLinkTaskFinished())
        {
            if (CalcCRC16WithTable(link->sendBuffer, link->sendSize) != link->sendCRC)
                LinkRfu_FatalError();
            else
                link->state++;
        }
        break;
    case 3:
        if (MGL_HasReceived(link->sendPlayerId))
        {
            MGL_ResetReceived(link->sendPlayerId);
            link->state = 0;
            return TRUE;
        }
        break;
    }

    return FALSE;
}
