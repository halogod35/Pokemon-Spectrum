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

// --- Cable interface (mp_link_cable.c) ---
enum
{
    MP_LINK_POP_NONE,       // no row waiting
    MP_LINK_POP_ROW,        // one row copied out
    MP_LINK_POP_BUS_END,    // the bus ended while we were on it (a member closed)
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
    u16 queued;             // rows waiting in our receive queue
};

void MpLink_CableGetInfo(struct MpLinkCableInfo *out);  // also runs the watchdog
void MpLink_CableOpen(void);                            // OpenLink reached the handshake
void MpLink_CableClose(void);                           // CloseLink (gLink.state back to START0)
void MpLink_CableRequestStart(void);                    // link id 0: the game advanced the handshake
void MpLink_CableSend(const u16 *cmd);                  // one non-zero 8-word command
u8 MpLink_CablePop(u16 (*cmds)[MP_LINK_CMD_WORDS]);     // fills cmds[0..3] on MP_LINK_POP_ROW

// --- Selftest / --mp-debug counters for the selected instance ---
struct MpLinkStats
{
    u32 cmdsSent;           // member: LINK_CMDs sent
    u32 rowsReceived;       // member: rows queued
    u16 rxPeak;             // member: largest receive queue seen
    u32 rowsEmitted;        // host: rows sent (per group member counted once)
    u32 groupsFormed;       // host
};
void MpLink_GetStats(struct MpLinkStats *out);

#endif // GUARD_MP_MP_LINK_H
