#ifndef GUARD_MP_MP_LINK_H
#define GUARD_MP_MP_LINK_H

// Virtual link cable, session side (PS-046). Internal to src/mp.
//
// mp_link.c (no game state): the host's link groups and row sequencer ("the bus"), and the
// member side (requests, open/close, BUS_START, the received-row queue, the watchdog).
// mp_link_cable.c (game side): MP_LinkMain1, the LinkMain1 replacement, uses the
// MpLink_Cable* functions below. All of them act on the selected session instance
// (gMpSession), so the selftest can drive several members in one process.

#include "gba/types.h"
#include "mp/mp_protocol.h"

#define MP_LINK_MAX_GROUPS      2       // 4 players: at most 2 groups at a time
#define MP_LINK_HOST_FIFO_SIZE  128     // commands queued per member on the host
#define MP_LINK_RX_BLOCK_SLOTS  64      // PS-052: whole blocks queued per member (MP_LINK_BLOCK_MAX bytes each)

// PS-047 bus pre-filter. The host refuses LINK_CMDs that no honest game sends and that
// would hurt the receivers (link.c refuses them too; this ends the link for everyone at
// once). The values mirror link.h / decompress.h, checked at compile time in mp_link_cable.c.
#define MP_LINKCMD_INIT_BLOCK       0xBBBB  // LINKCMD_INIT_BLOCK: word 1 = block size
#define MP_LINKCMD_SEND_BLOCK_REQ   0xCCCC  // LINKCMD_SEND_BLOCK_REQ: word 1 = request type
#define MP_LINK_MAX_BLOCK_SIZE      0x4000  // sizeof(gDecompressionBuffer)
#define MP_LINK_BLOCK_REQ_COUNT     5       // ARRAY_COUNT(sBlockRequests) in link.c
bool8 MpLink_IsCmdAllowed(const u16 *cmd); // cmd: MP_LINK_CMD_WORDS words

// --- Session hooks (mp_session.c, mp_server.c, mp_client.c) ---
void MpLink_ResetInstance(u8 instance);     // selftest + MP_Init: forget everything
void MpLink_OnSessionStart(void);           // MP_Host / MP_Join: no groups
void MpLink_OnSessionEnd(void);             // MP_Leave / MpSession_Fail: group ends (SESSION)
void MpLink_HostOnSlotLeft(u8 slot);        // host: a player left the table
void MpLink_HostUpdate(void);               // host: emit rows (once per MP_Update)
// Host: a LINK_* message from `slot` (the sender's real slot). FALSE = malformed.
bool8 MpLink_OnHostReceive(u8 slot, u8 channel, u8 type, const u8 *payload, u32 len);
// Client: a LINK_* message from the host. FALSE = malformed.
bool8 MpLink_OnClientReceive(u8 channel, u8 type, const u8 *payload, u32 len);

// --- PS-044 trade commit (mp_trade_commit.c) ---
// Host: the link group `slot` is in (its id and member slot mask). FALSE if none.
bool8 MpLink_HostGetGroup(u8 slot, u8 *groupId, u8 *memberMask);
// Member: our link group's id while GROUPED, else 0.
u8 MpLink_MemberGroupId(void);

// --- Cable interface (mp_link_cable.c) ---
enum
{
    MP_LINK_POP_NONE,       // nothing waiting
    MP_LINK_POP_ROW,        // one row (copied out by MpLink_CablePop)
    MP_LINK_POP_BUS_END,    // the bus ended while we were on it (a member closed)
    MP_LINK_POP_BLOCK,      // PS-052: one whole block from a member (MpLink_CableHeadBlock)
};

struct MpLinkCableInfo
{
    bool8 grouped;          // MP_LINK_STATE_GROUPED
    bool8 open;             // we sent LINK_OPEN and have not closed since
    bool8 busUp;            // BUS_START received for our current open
    bool8 lost;             // the group ended (or the watchdog fired) while we were open
    u8 linkId;              // our link id (kept after the group ends, for GetMultiplayerId)
    u8 count;               // members in the group
    u8 openCount;           // members the host has seen open (LINK_STATUS)
    u16 queued;             // rows (and PS-052 blocks) waiting in our receive queue
    u32 outstanding;        // PS-052 stats: our own commands / blocks not yet back from the bus
};

void MpLink_CableGetInfo(struct MpLinkCableInfo *out);  // also runs the watchdog
void MpLink_CableOpen(void);                            // OpenLink reached the handshake
void MpLink_CableClose(void);                           // CloseLink (gLink.state back to START0)
void MpLink_CableRequestStart(void);                    // link id 0: the game advanced the handshake
void MpLink_CableSend(const u16 *cmd);                  // one non-zero 8-word command
// PS-052: one whole block (1..MP_LINK_BLOCK_MAX bytes) as LINK_BLOCK, in order with our commands.
// FALSE (nothing sent) unless we are on the bus.
bool8 MpLink_CableSendBlock(const u8 *data, u16 size);
u8 MpLink_CableHeadType(void);                          // MP_LINK_POP_* of the oldest item, not removed
bool8 MpLink_CableHeadRow(u16 (*cmds)[MP_LINK_CMD_WORDS]); // copies the oldest item if it is a row
// The oldest item if it is a block: its data (valid until it is popped), sender link id, size.
const u8 *MpLink_CableHeadBlock(u8 *linkId, u16 *size);
// Removes the oldest item; fills cmds[0..3] (if not NULL) on MP_LINK_POP_ROW.
u8 MpLink_CablePop(u16 (*cmds)[MP_LINK_CMD_WORDS]);

// --- PS-052: fast cable measurements (game side, mp_link_cable.c), per session instance ---
struct MpLinkCableStats
{
    u32 busTicks;           // game ticks with the cable connected (CONN_ESTABLISHED)
    u32 rowsPlayed;         // rows handed to the game (one per tick, plus catch-up)
    u32 catchUpRows;        // of which extra rows played by the catch-up (row pacing)
    u32 idleTicks;          // ticks on the cable with nothing received
    u32 waitTicks;          // of which our own command or block was still on its way through the bus
    u32 blocksSentFast;     // SendBlock as one LINK_BLOCK
    u32 blocksSentRows;     // SendBlock as INIT_BLOCK + CONT_BLOCK rows
    u32 blocksRecvFast;     // blocks completed for the game (any link id, ours included) from LINK_BLOCK
    u32 blocksRecvRows;     // ... from rows
    u32 ownBlocks;          // our own blocks back from the bus
    u32 ownBlockTicks;      // sum of ticks from SendBlock until our own block came back
    u32 holdTicks;          // ticks a block waited because the game had not taken the previous one
};
void MpLink_GetCableStats(struct MpLinkCableStats *out);    // the current (or last) link activity
// --mp-selftest: called for every row handed to the game (isBlock FALSE, row = the 4 x 8 words)
// and every completed block (isBlock TRUE, who = link id, size), in the order the game sees them.
typedef void (*MpLinkSelftestObserver)(bool8 isBlock, u8 who, const u16 (*row)[MP_LINK_CMD_WORDS], u16 size);
void MpLink_SetSelftestObserver(MpLinkSelftestObserver observer);

// --- PS-051: the agreed end of a link activity (mp_link_cable.c: MP_LinkActivity*) ---
void MpLink_MemberSetAgreed(void);          // both players agreed to stop
bool8 MpLink_MemberIsAgreed(void);
bool8 MpLink_MemberIsFinishing(void);       // agreed, done, a partner done, or the group ended DONE
u8 MpLink_MemberPeerDoneMask(void);         // other members that sent LINK_DONE, by link id
void MpLink_MemberSendDone(void);           // LINK_DONE to the host (once per activity)
u16 MpLink_MemberBumpFinishWait(void);      // +1, returns the frames waited so far
u8 MpLink_InstanceId(void);                 // the selected session instance (trace label)
void MpLink_TraceSelftestEndIncident(void); // mp_link_cable.c, selftest: close the incident window

// --- Selftest / --mp-debug counters for the selected instance ---
struct MpLinkStats
{
    u32 cmdsSent;           // member: LINK_CMDs sent
    u32 rowsReceived;       // member: rows queued
    u16 rxPeak;             // member: largest receive queue seen
    u32 rowsEmitted;        // host: rows sent (per group member counted once)
    u32 groupsFormed;       // host
    u32 cmdsRefused;        // host: LINK_CMDs refused by the PS-047 pre-filter
    u32 blocksSent;         // member: LINK_BLOCKs sent (PS-052)
    u32 blocksReceived;     // member: LINK_BLOCKs queued (PS-052)
    u32 blocksRelayed;      // host: LINK_BLOCKs relayed (per group member counted once) (PS-052)
};
void MpLink_GetStats(struct MpLinkStats *out);

#endif // GUARD_MP_MP_LINK_H
