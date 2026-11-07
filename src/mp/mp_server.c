// Host role (protocol.md sections 6 and 7): pending peers, HELLO validation,
// slot/epoch assignment, relay, malformed budget, timeouts.
// No game-state access here, so a headless dedicated server can reuse it (AD-2).

#include <string.h>
#include "mp/mp_session_internal.h"
#include "mp/mp_summary.h"

static u8 sServerBuf[MP_MAX_PACKET_SIZE];

static struct MpHostPeer *MpServerPeer(u8 peer)
{
    return (peer < MP_HOST_PEER_COUNT) ? &gMpSession->peers[peer] : NULL;
}

static void MpServerEntryForSlot(u8 slot, struct MpMsgPlayerEntry *entry)
{
    memset(entry, 0, sizeof(*entry));
    entry->slot = slot;
    entry->epoch = gMpSession->players[slot].epoch;
    entry->entryFlags = (slot == MP_HOST_SLOT) ? MP_ENTRY_FLAG_HOST : 0;
    entry->summary = gMpSession->players[slot].summary;
}

// Sends `len` bytes to every peer in the player table except `exceptPeer`.
static void MpServerBroadcast(const u8 *data, u16 len, u8 channel, bool8 reliable, u8 exceptPeer)
{
    u32 i;

    for (i = 0; i < MP_HOST_PEER_COUNT; i++)
    {
        if (i != exceptPeer && gMpSession->peers[i].state == MP_HOST_PEER_PLAYER)
            MpSession_SendTo((u8)i, data, len, channel, reliable);
    }
}

static void MpServerClosePeer(u8 peer)
{
    struct MpHostPeer *p = MpServerPeer(peer);

    if (p == NULL)
        return;
    p->state = MP_HOST_PEER_CLOSING;
    p->sinceMs = MpTransport_GetTimeMs();
    MpTransport_Disconnect(peer);
}

static void MpServerResetPeer(u8 peer)
{
    struct MpHostPeer *p = MpServerPeer(peer);

    if (p == NULL)
        return;
    MpTransport_DisconnectNow(peer);
    memset(p, 0, sizeof(*p));
}

// Removes the peer's player from the table and tells everyone else.
static void MpServerRemovePlayer(u8 peer, u8 reason)
{
    struct MpHostPeer *p = MpServerPeer(peer);
    struct MpMsgPlayerLeft left;
    char name[MP_PLAYER_NAME_SIZE + 1];
    u16 len;
    u8 slot;

    if (p == NULL || p->state != MP_HOST_PEER_PLAYER || p->slot >= MP_MAX_PLAYERS)
        return;
    slot = p->slot;
    MP_NameToAscii(gMpSession->players[slot].summary.name, name, sizeof(name));
    MP_SLOG("player %s (slot %u) left: %s\n", name, (unsigned)slot, MP_GetReasonName(reason));

    left.slot = slot;
    left.epoch = gMpSession->players[slot].epoch;
    left.reason = reason;
    MpSession_QueueEvent(MP_EVENT_PLAYER_LEFT, slot, reason); // copies the name first
    MpSession_ClearSlot(slot);
    p->slot = MP_SLOT_NONE;
    p->state = MP_HOST_PEER_NONE;

    len = MpProto_WritePlayerLeft(sServerBuf, sizeof(sServerBuf), &left);
    MpServerBroadcast(sServerBuf, len, MP_CHANNEL_CONTROL, TRUE, peer);
    MpSession_LogTable();
}

static void MpServerReject(u8 peer, u8 reason)
{
    struct MpMsgReject reject;
    u16 len;

    MP_SLOG("peer %u rejected: %s\n", (unsigned)peer, MP_GetReasonName(reason));
    reject.reason = reason;
    reject.hostProtocolVersion = gMpSession->protocolVersion;
    reject.hostBuildHash = gMpSession->buildHash;
    len = MpProto_WriteReject(sServerBuf, sizeof(sServerBuf), &reject);
    MpSession_SendTo(peer, sServerBuf, len, MP_CHANNEL_CONTROL, TRUE);
    MpServerClosePeer(peer);
}

static void MpServerKick(u8 peer, u8 reason)
{
    struct MpMsgBye bye;
    u16 len;

    MP_SLOG("kicking peer %u: %s\n", (unsigned)peer, MP_GetReasonName(reason));
    bye.reason = MP_BYE_PROTOCOL_ERROR;
    len = MpProto_WriteBye(sServerBuf, sizeof(sServerBuf), &bye);
    MpSession_SendTo(peer, sServerBuf, len, MP_CHANNEL_CONTROL, TRUE);
    MpServerRemovePlayer(peer, reason);
    MpServerClosePeer(peer);
}

static void MpServerMalformed(u8 peer, const char *why)
{
    struct MpHostPeer *p = MpServerPeer(peer);

    if (p == NULL)
        return;
    if (p->state == MP_HOST_PEER_PENDING)
    {
        // Before the handshake anything but a good HELLO ends the connection at once.
        MP_SLOG("peer %u: %s before HELLO, reset\n", (unsigned)peer, why);
        MpServerResetPeer(peer);
        return;
    }
    if (p->state != MP_HOST_PEER_PLAYER)
        return;
    p->malformed++;
    MP_SDLOG("peer %u: dropped message (%s), malformed %u/%u\n", (unsigned)peer, why, (unsigned)p->malformed, (unsigned)MP_MALFORMED_LIMIT);
    if (p->malformed >= MP_MALFORMED_LIMIT)
        MpServerKick(peer, MP_REASON_KICKED);
}

static u8 MpServerNextEpoch(void)
{
    u8 epoch = gMpSession->nextEpoch;

    if (epoch == 0)
        epoch = 1;
    gMpSession->nextEpoch = (u8)(epoch + 1);
    if (gMpSession->nextEpoch == 0)
        gMpSession->nextEpoch = 1;
    return epoch;
}

static void MpServerHandleHello(u8 peer, const struct MpMsgHeader *hdr, const u8 *payload, u32 len)
{
    struct MpSession *s = gMpSession;
    struct MpHostPeer *p = MpServerPeer(peer);
    struct MpMsgHello hello;
    struct MpMsgWelcome welcome;
    struct MpMsgPlayerEntry entry;
    char name[MP_PLAYER_NAME_SIZE + 1];
    u16 protocolVersion;
    u32 buildHash;
    u16 outLen;
    u32 i;
    u8 slot;

    // 2. frozen prefix
    if (MpProto_ParseHelloPrefix(payload, len, &protocolVersion, &buildHash) != MP_PARSE_OK)
    {
        MpServerReject(peer, MP_REASON_BAD_DATA);
        return;
    }
    // 3. protocol version (field and header byte)
    if (protocolVersion != s->protocolVersion || hdr->version != (u8)(s->protocolVersion & 0xFF))
    {
        MP_SLOG("peer %u: protocol %u, ours %u\n", (unsigned)peer, (unsigned)protocolVersion, (unsigned)s->protocolVersion);
        MpServerReject(peer, MP_REASON_VERSION);
        return;
    }
    // 4. build
    if (buildHash != s->buildHash)
    {
        MP_SLOG("peer %u: build hash 0x%08X, ours 0x%08X\n", (unsigned)peer, (unsigned)buildHash, (unsigned)s->buildHash);
        MpServerReject(peer, MP_REASON_BUILD);
        return;
    }
    // 5./6. exact size and summary
    if (MpProto_ParseHello(payload, len, &hello) != MP_PARSE_OK)
    {
        MpServerReject(peer, MP_REASON_BAD_DATA);
        return;
    }
    // 7. free slot
    slot = MP_SLOT_NONE;
    for (i = 1; i < MP_MAX_PLAYERS; i++)
    {
        if (!s->slotUsed[i])
        {
            slot = (u8)i;
            break;
        }
    }
    if (slot == MP_SLOT_NONE)
    {
        MpServerReject(peer, MP_REASON_FULL);
        return;
    }

    // 8. accept
    MpSession_SetSlot(slot, MpServerNextEpoch(), &hello.summary, FALSE);
    s->slotPeer[slot] = peer;
    p->state = MP_HOST_PEER_PLAYER;
    p->slot = slot;
    p->malformed = 0;
    p->lastPingSentMs = MpTransport_GetTimeMs();

    memset(&welcome, 0, sizeof(welcome));
    welcome.yourSlot = slot;
    welcome.yourEpoch = s->players[slot].epoch;
    for (i = 0; i < MP_MAX_PLAYERS; i++)
    {
        if (s->slotUsed[i])
            MpServerEntryForSlot((u8)i, &welcome.players[welcome.playerCount++]);
    }
    outLen = MpProto_WriteWelcome(sServerBuf, sizeof(sServerBuf), &welcome);
    MpSession_SendTo(peer, sServerBuf, outLen, MP_CHANNEL_CONTROL, TRUE);

    MpServerEntryForSlot(slot, &entry);
    outLen = MpProto_WritePlayerEntry(sServerBuf, sizeof(sServerBuf), MP_MSG_PLAYER_JOINED, &entry);
    MpServerBroadcast(sServerBuf, outLen, MP_CHANNEL_CONTROL, TRUE, peer);
    // protocol.md 6.1 step 8.3: the cached PRESENCE of every other player (host included).
    MpPresence_SendCachedTo(peer, slot);

    MP_NameToAscii(hello.summary.name, name, sizeof(name));
    MP_SLOG("player %s joined: slot %u, epoch %u, peer %u%s\n", name, (unsigned)slot,
            (unsigned)s->players[slot].epoch, (unsigned)peer, (hello.buildFlags & MP_BUILD_FLAG_DIRTY) ? " (dirty build)" : "");
    MpSession_QueueEvent(MP_EVENT_PLAYER_JOINED, slot, MP_REASON_NONE);
    MpSession_LogTable();
}

static void MpServerHandleUpdate(u8 peer, const u8 *payload, u32 len, u32 now)
{
    struct MpSession *s = gMpSession;
    struct MpHostPeer *p = MpServerPeer(peer);
    struct MpMsgPlayerEntry entry;
    u8 copy[MP_PLAYER_ENTRY_SIZE];
    u16 outLen;
    u8 slot = p->slot;

    if (p->lastUpdateRecvMs != 0 && (u32)(now - p->lastUpdateRecvMs) < MP_UPDATE_MIN_GAP_MS)
    {
        MP_SDLOG("peer %u: PLAYER_UPDATE rate-limited\n", (unsigned)peer);
        return;
    }
    if (len != MP_PLAYER_ENTRY_SIZE)
    {
        MpServerMalformed(peer, "PLAYER_UPDATE size");
        return;
    }
    // The host is authoritative for slot, epoch and flags: overwrite them before parsing.
    memcpy(copy, payload, sizeof(copy));
    copy[0] = slot;
    copy[1] = s->players[slot].epoch;
    copy[2] = 0;
    if (MpProto_ParsePlayerEntry(copy, sizeof(copy), &entry) != MP_PARSE_OK)
    {
        MpServerMalformed(peer, "PLAYER_UPDATE summary");
        return;
    }
    p->lastUpdateRecvMs = now;
    s->players[slot].summary = entry.summary;
    outLen = MpProto_WritePlayerEntry(sServerBuf, sizeof(sServerBuf), MP_MSG_PLAYER_UPDATE, &entry);
    MpServerBroadcast(sServerBuf, outLen, MP_CHANNEL_CONTROL, TRUE, peer);
    MpSession_QueueEvent(MP_EVENT_PLAYER_UPDATED, slot, MP_REASON_NONE);
    MP_SDLOG("slot %u summary updated\n", (unsigned)slot);
}

static void MpServerHandlePlayerMessage(u8 peer, u8 channel, const struct MpMsgHeader *hdr, const u8 *payload, u32 len, u32 now)
{
    struct MpHostPeer *p = MpServerPeer(peer);
    struct MpMsgPing ping;
    u32 rtt;
    u16 outLen;

    if (hdr->type == MP_MSG_HELLO)
    {
        MpServerKick(peer, MP_REASON_PROTOCOL_ERROR); // second HELLO
        return;
    }
    if (hdr->version != MP_PROTOCOL_VERSION)
    {
        MpServerMalformed(peer, "header version");
        return;
    }

    switch (hdr->type)
    {
    case MP_MSG_PLAYER_UPDATE:
        if (channel != MP_CHANNEL_CONTROL)
            MpServerMalformed(peer, "wrong channel");
        else
            MpServerHandleUpdate(peer, payload, len, now);
        break;
    case MP_MSG_BYE:
        if (channel != MP_CHANNEL_CONTROL || len != MP_BYE_SIZE)
        {
            MpServerMalformed(peer, "BYE");
            break;
        }
        MpServerRemovePlayer(peer, MP_REASON_LEFT);
        MpServerClosePeer(peer);
        break;
    case MP_MSG_PING:
        if (channel != MP_CHANNEL_PRESENCE || MpProto_ParsePing(payload, len, &ping) != MP_PARSE_OK)
        {
            MpServerMalformed(peer, "PING");
            break;
        }
        if (p->lastPingRecvMs != 0 && (u32)(now - p->lastPingRecvMs) < MP_PING_MIN_GAP_MS)
            break;
        p->lastPingRecvMs = now;
        outLen = MpProto_WritePing(sServerBuf, sizeof(sServerBuf), MP_MSG_PONG, &ping);
        MpSession_SendTo(peer, sServerBuf, outLen, MP_CHANNEL_PRESENCE, FALSE);
        break;
    case MP_MSG_PONG:
        if (channel != MP_CHANNEL_PRESENCE || MpProto_ParsePing(payload, len, &ping) != MP_PARSE_OK)
        {
            MpServerMalformed(peer, "PONG");
            break;
        }
        rtt = now - ping.timeMs;
        if (rtt <= 60000 && p->slot < MP_MAX_PLAYERS)
            gMpSession->players[p->slot].pingMs = (u16)rtt;
        break;
    case MP_MSG_PRESENCE:
        if (channel != MP_CHANNEL_PRESENCE)
            MpServerMalformed(peer, "PRESENCE wrong channel");
        else if (MpPresence_OnHostReceive(peer, payload, len, now) == MP_PRESENCE_INVALID)
            MpServerMalformed(peer, "PRESENCE");
        break;
    default:
        MpServerMalformed(peer, "unexpected message type");
        break;
    }
}

void MpServer_OnTransportEvent(const struct MpTransportEvent *ev)
{
    struct MpHostPeer *p = MpServerPeer(ev->peer);
    struct MpMsgHeader hdr;
    u32 now = MpTransport_GetTimeMs();

    if (p == NULL)
        return;

    switch (ev->type)
    {
    case MP_TRANSPORT_EV_CONNECT:
        memset(p, 0, sizeof(*p));
        p->state = MP_HOST_PEER_PENDING;
        p->slot = MP_SLOT_NONE;
        p->sinceMs = now;
        p->lastRecvMs = now;
        MP_SDLOG("peer %u connected, waiting for HELLO\n", (unsigned)ev->peer);
        break;
    case MP_TRANSPORT_EV_DISCONNECT:
        if (p->state == MP_HOST_PEER_PLAYER)
            MpServerRemovePlayer(ev->peer, MP_REASON_CONNECTION_LOST);
        memset(p, 0, sizeof(*p));
        MP_SDLOG("peer %u disconnected\n", (unsigned)ev->peer);
        break;
    case MP_TRANSPORT_EV_RECEIVE:
        if (p->state != MP_HOST_PEER_PENDING && p->state != MP_HOST_PEER_PLAYER)
            break;
        p->lastRecvMs = now;
        if (!MpProto_ParseHeader(ev->data, ev->length, &hdr))
        {
            MpServerMalformed(ev->peer, "bad header");
            break;
        }
        if (p->state == MP_HOST_PEER_PENDING)
        {
            if (hdr.type != MP_MSG_HELLO || ev->channel != MP_CHANNEL_CONTROL)
            {
                if (hdr.type == MP_MSG_HELLO)
                    MpServerReject(ev->peer, MP_REASON_BAD_DATA);
                else
                    MpServerMalformed(ev->peer, "message");
                break;
            }
            MpServerHandleHello(ev->peer, &hdr, ev->data + MP_HEADER_SIZE, hdr.length);
        }
        else
        {
            MpServerHandlePlayerMessage(ev->peer, ev->channel, &hdr, ev->data + MP_HEADER_SIZE, hdr.length, now);
        }
        break;
    }
}

void MpServer_Update(u32 now)
{
    struct MpMsgPing ping;
    u16 len;
    u32 i;

    for (i = 0; i < MP_HOST_PEER_COUNT; i++)
    {
        struct MpHostPeer *p = &gMpSession->peers[i];

        switch (p->state)
        {
        case MP_HOST_PEER_PENDING:
            if ((u32)(now - p->sinceMs) >= MP_CONNECT_TIMEOUT_MS)
            {
                MP_SLOG("peer %u: no HELLO within %u ms, reset\n", (unsigned)i, (unsigned)MP_CONNECT_TIMEOUT_MS);
                MpServerResetPeer((u8)i);
            }
            break;
        case MP_HOST_PEER_CLOSING:
            if ((u32)(now - p->sinceMs) >= MP_CLOSING_PEER_MS)
                MpServerResetPeer((u8)i);
            break;
        case MP_HOST_PEER_PLAYER:
            if ((u32)(now - p->lastRecvMs) >= MP_IDLE_TIMEOUT_MS)
            {
                MpServerRemovePlayer((u8)i, MP_REASON_CONNECTION_LOST);
                MpServerResetPeer((u8)i);
                break;
            }
            if ((u32)(now - p->lastPingSentMs) >= MP_PING_INTERVAL_MS)
            {
                p->lastPingSentMs = now;
                ping.timeMs = now;
                len = MpProto_WritePing(sServerBuf, sizeof(sServerBuf), MP_MSG_PING, &ping);
                MpSession_SendTo((u8)i, sServerBuf, len, MP_CHANNEL_PRESENCE, FALSE);
            }
            break;
        }
    }
    MpSession_TrySendLocalUpdate(now);
}

void MpServer_BroadcastBye(u8 byeReason)
{
    struct MpMsgBye bye;
    u16 len;
    u32 i;

    bye.reason = byeReason;
    len = MpProto_WriteBye(sServerBuf, sizeof(sServerBuf), &bye);
    for (i = 0; i < MP_HOST_PEER_COUNT; i++)
    {
        struct MpHostPeer *p = &gMpSession->peers[i];

        if (p->state == MP_HOST_PEER_PLAYER)
            MpSession_SendTo((u8)i, sServerBuf, len, MP_CHANNEL_CONTROL, TRUE);
        if (p->state != MP_HOST_PEER_NONE)
        {
            MpTransport_Disconnect((u8)i);
            p->state = MP_HOST_PEER_CLOSING;
            p->sinceMs = MpTransport_GetTimeMs();
        }
    }
}

void MpServer_SendLocalUpdate(void)
{
    struct MpMsgPlayerEntry entry;
    u16 len;

    MpServerEntryForSlot(MP_HOST_SLOT, &entry);
    len = MpProto_WritePlayerEntry(sServerBuf, sizeof(sServerBuf), MP_MSG_PLAYER_UPDATE, &entry);
    MpServerBroadcast(sServerBuf, len, MP_CHANNEL_CONTROL, TRUE, MP_PEER_NONE);
}
