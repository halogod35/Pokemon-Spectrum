// Client role (protocol.md sections 6 and 7): connect timer, HELLO,
// WELCOME/REJECT, the mirrored player table, BYE, ping, malformed budget.

#include <string.h>
#include "mp/mp_session_internal.h"
#include "mp/mp_summary.h"

#define MP_HOST_PEER_ID 0   // a client's only transport peer

static u8 sClientBuf[MP_MAX_PACKET_SIZE];

static void MpClientSendHello(void)
{
    struct MpSession *s = gMpSession;
    struct MpMsgHello hello;
    u16 len;

    memset(&hello, 0, sizeof(hello));
    hello.protocolVersion = s->protocolVersion;
    hello.buildHash = s->buildHash;
    hello.buildFlags = s->buildFlags;
    hello.summary = s->localSummary;
    len = MpProto_WriteHello(sClientBuf, sizeof(sClientBuf), &hello);
    MpSession_SendTo(MP_HOST_PEER_ID, sClientBuf, len, MP_CHANNEL_CONTROL, TRUE);
    MP_SDLOG("HELLO sent (protocol %u, build hash 0x%08X)\n", (unsigned)hello.protocolVersion, (unsigned)hello.buildHash);
}

void MpClient_SendBye(u8 byeReason)
{
    struct MpMsgBye bye;
    u16 len;

    bye.reason = byeReason;
    len = MpProto_WriteBye(sClientBuf, sizeof(sClientBuf), &bye);
    MpSession_SendTo(MP_HOST_PEER_ID, sClientBuf, len, MP_CHANNEL_CONTROL, TRUE);
    MpTransport_Disconnect(MP_HOST_PEER_ID);
}

void MpClient_SendLocalUpdate(void)
{
    struct MpSession *s = gMpSession;
    struct MpMsgPlayerEntry entry;
    u16 len;

    if (s->localSlot >= MP_MAX_PLAYERS)
        return;
    memset(&entry, 0, sizeof(entry));
    entry.slot = s->localSlot;
    entry.epoch = s->players[s->localSlot].epoch;
    entry.summary = s->localSummary;
    len = MpProto_WritePlayerEntry(sClientBuf, sizeof(sClientBuf), MP_MSG_PLAYER_UPDATE, &entry);
    MpSession_SendTo(MP_HOST_PEER_ID, sClientBuf, len, MP_CHANNEL_CONTROL, TRUE);
}

static void MpClientMalformed(const char *why)
{
    struct MpSession *s = gMpSession;

    s->client.malformed++;
    MP_SDLOG("dropped message from host (%s), malformed %u/%u\n", why, (unsigned)s->client.malformed, (unsigned)MP_MALFORMED_LIMIT);
    if (s->client.malformed >= MP_MALFORMED_LIMIT)
    {
        MpClient_SendBye(MP_BYE_PROTOCOL_ERROR);
        MpSession_Fail(s->state == MP_STATE_CONNECTING ? MP_EVENT_CONNECT_FAILED : MP_EVENT_DISCONNECTED, MP_REASON_PROTOCOL_ERROR);
    }
}

static void MpClientHandleWelcome(const u8 *payload, u32 len)
{
    struct MpSession *s = gMpSession;
    struct MpMsgWelcome welcome;
    u32 i;

    if (MpProto_ParseWelcome(payload, len, &welcome) != MP_PARSE_OK)
    {
        MP_SLOG("malformed WELCOME\n");
        MpClient_SendBye(MP_BYE_PROTOCOL_ERROR);
        MpSession_Fail(MP_EVENT_CONNECT_FAILED, MP_REASON_PROTOCOL_ERROR);
        return;
    }
    for (i = 0; i < MP_MAX_PLAYERS; i++)
        MpSession_ClearSlot((u8)i);
    for (i = 0; i < welcome.playerCount; i++)
    {
        const struct MpMsgPlayerEntry *e = &welcome.players[i];
        bool8 isLocal = (e->slot == welcome.yourSlot);

        MpSession_SetSlot(e->slot, e->epoch, isLocal ? &s->localSummary : &e->summary, isLocal);
    }
    s->localSlot = welcome.yourSlot;
    s->state = MP_STATE_IN_SESSION;
    s->client.lastPingSentMs = MpTransport_GetTimeMs();
    MP_SLOG("joined the session as slot %u (epoch %u)\n", (unsigned)welcome.yourSlot, (unsigned)welcome.yourEpoch);
    MpSession_LogTable();
    MpSession_QueueEvent(MP_EVENT_SESSION_STARTED, welcome.yourSlot, MP_REASON_NONE);
    // protocol.md 6.2: send the current local presence immediately.
    MpPresence_Update(MpTransport_GetTimeMs());
}

static void MpClientHandleReject(const u8 *payload, u32 len)
{
    struct MpMsgReject reject;
    u8 reason;

    if (MpProto_ParseReject(payload, len, &reject) != MP_PARSE_OK)
    {
        MpClientMalformed("REJECT");
        return;
    }
    reason = (reject.reason >= 1 && reject.reason <= 31) ? reject.reason : MP_REASON_BAD_DATA;
    MP_SLOG("rejected by host: %s (host protocol %u, build hash 0x%08X)\n", MP_GetReasonName(reason),
            (unsigned)reject.hostProtocolVersion, (unsigned)reject.hostBuildHash);
    MpSession_Fail(MP_EVENT_CONNECT_FAILED, reason);
}

// TRUE if `slot` is a remote slot that this client may change.
static bool8 MpClientIsRemoteSlot(u8 slot)
{
    return slot != 0 && slot < MP_MAX_PLAYERS && slot != gMpSession->localSlot;
}

static void MpClientHandleSessionMessage(u8 channel, const struct MpMsgHeader *hdr, const u8 *payload, u32 len, u32 now)
{
    struct MpSession *s = gMpSession;
    struct MpMsgPlayerEntry entry;
    struct MpMsgPlayerLeft left;
    struct MpMsgBye bye;
    struct MpMsgPing ping;
    u32 rtt;
    u16 outLen;
    u8 reason;

    if (hdr->version != MP_PROTOCOL_VERSION)
    {
        MpClientMalformed("header version");
        return;
    }

    switch (hdr->type)
    {
    case MP_MSG_PLAYER_JOINED:
        if (channel != MP_CHANNEL_CONTROL || MpProto_ParsePlayerEntry(payload, len, &entry) != MP_PARSE_OK
         || !MpClientIsRemoteSlot(entry.slot))
        {
            MpClientMalformed("PLAYER_JOINED");
            break;
        }
        if (s->slotUsed[entry.slot])
            MP_SLOG("WARNING: PLAYER_JOINED for occupied slot %u, replacing\n", (unsigned)entry.slot);
        MpSession_SetSlot(entry.slot, entry.epoch, &entry.summary, FALSE);
        MpSession_QueueEvent(MP_EVENT_PLAYER_JOINED, entry.slot, MP_REASON_NONE);
        MpSession_LogTable();
        break;
    case MP_MSG_PLAYER_LEFT:
        if (channel != MP_CHANNEL_CONTROL || MpProto_ParsePlayerLeft(payload, len, &left) != MP_PARSE_OK
         || !MpClientIsRemoteSlot(left.slot))
        {
            MpClientMalformed("PLAYER_LEFT");
            break;
        }
        if (!s->slotUsed[left.slot] || s->players[left.slot].epoch != left.epoch)
            break; // stale
        reason = left.reason;
        if (reason != MP_REASON_LEFT && reason != MP_REASON_CONNECTION_LOST
         && reason != MP_REASON_KICKED && reason != MP_REASON_PROTOCOL_ERROR)
            reason = MP_REASON_CONNECTION_LOST;
        MpSession_QueueEvent(MP_EVENT_PLAYER_LEFT, left.slot, reason);
        MpSession_ClearSlot(left.slot);
        MP_SLOG("slot %u left: %s\n", (unsigned)left.slot, MP_GetReasonName(reason));
        MpSession_LogTable();
        break;
    case MP_MSG_PLAYER_UPDATE:
        if (channel != MP_CHANNEL_CONTROL || MpProto_ParsePlayerEntry(payload, len, &entry) != MP_PARSE_OK
         || entry.slot == s->localSlot)
        {
            MpClientMalformed("PLAYER_UPDATE");
            break;
        }
        if (!s->slotUsed[entry.slot] || s->players[entry.slot].epoch != entry.epoch)
            break; // stale
        s->players[entry.slot].summary = entry.summary;
        MpSession_QueueEvent(MP_EVENT_PLAYER_UPDATED, entry.slot, MP_REASON_NONE);
        MP_SDLOG("slot %u summary updated\n", (unsigned)entry.slot);
        break;
    case MP_MSG_BYE:
        if (channel != MP_CHANNEL_CONTROL || MpProto_ParseBye(payload, len, &bye) != MP_PARSE_OK)
        {
            MpClientMalformed("BYE");
            break;
        }
        MpSession_Fail(MP_EVENT_DISCONNECTED, (bye.reason == MP_BYE_PROTOCOL_ERROR) ? MP_REASON_KICKED : MP_REASON_HOST_CLOSED);
        break;
    case MP_MSG_PING:
        if (channel != MP_CHANNEL_PRESENCE || MpProto_ParsePing(payload, len, &ping) != MP_PARSE_OK)
        {
            MpClientMalformed("PING");
            break;
        }
        if (s->client.lastPingRecvMs != 0 && (u32)(now - s->client.lastPingRecvMs) < MP_PING_MIN_GAP_MS)
            break;
        s->client.lastPingRecvMs = now;
        outLen = MpProto_WritePing(sClientBuf, sizeof(sClientBuf), MP_MSG_PONG, &ping);
        MpSession_SendTo(MP_HOST_PEER_ID, sClientBuf, outLen, MP_CHANNEL_PRESENCE, FALSE);
        break;
    case MP_MSG_PONG:
        if (channel != MP_CHANNEL_PRESENCE || MpProto_ParsePing(payload, len, &ping) != MP_PARSE_OK)
        {
            MpClientMalformed("PONG");
            break;
        }
        rtt = now - ping.timeMs;
        if (rtt <= 60000 && s->slotUsed[MP_HOST_SLOT])
            s->players[MP_HOST_SLOT].pingMs = (u16)rtt;
        break;
    case MP_MSG_PRESENCE:
        if (channel != MP_CHANNEL_PRESENCE)
            MpClientMalformed("PRESENCE wrong channel");
        else if (MpPresence_OnClientReceive(payload, len) == MP_PRESENCE_INVALID)
            MpClientMalformed("PRESENCE");
        break;
    default:
        MpClientMalformed("unexpected message type");
        break;
    }
}

void MpClient_OnTransportEvent(const struct MpTransportEvent *ev)
{
    struct MpSession *s = gMpSession;
    struct MpMsgHeader hdr;
    u32 now = MpTransport_GetTimeMs();

    if (ev->peer != MP_HOST_PEER_ID)
        return;

    switch (ev->type)
    {
    case MP_TRANSPORT_EV_CONNECT:
        s->client.transportConnected = TRUE;
        s->client.lastRecvMs = now;
        if (s->state == MP_STATE_CONNECTING)
            MpClientSendHello();
        break;
    case MP_TRANSPORT_EV_DISCONNECT:
        s->client.transportConnected = FALSE;
        if (s->state == MP_STATE_CONNECTING)
            MpSession_Fail(MP_EVENT_CONNECT_FAILED, MP_REASON_CONNECTION_LOST);
        else if (s->state == MP_STATE_IN_SESSION)
            MpSession_Fail(MP_EVENT_DISCONNECTED, MP_REASON_CONNECTION_LOST);
        break;
    case MP_TRANSPORT_EV_RECEIVE:
        s->client.lastRecvMs = now;
        if (!MpProto_ParseHeader(ev->data, ev->length, &hdr))
        {
            MpClientMalformed("bad header");
            break;
        }
        if (s->state == MP_STATE_CONNECTING)
        {
            if (hdr.type == MP_MSG_REJECT && ev->channel == MP_CHANNEL_CONTROL)
                MpClientHandleReject(ev->data + MP_HEADER_SIZE, hdr.length); // any header version (frozen)
            else if (hdr.type == MP_MSG_WELCOME && ev->channel == MP_CHANNEL_CONTROL && hdr.version == MP_PROTOCOL_VERSION)
                MpClientHandleWelcome(ev->data + MP_HEADER_SIZE, hdr.length);
            else if (hdr.type == MP_MSG_PRESENCE && ev->channel == MP_CHANNEL_PRESENCE)
                break; // relayed presence overtook a delayed WELCOME (expected race, protocol.md 6.8)
            else
                MpClientMalformed("unexpected message while connecting");
        }
        else if (s->state == MP_STATE_IN_SESSION)
        {
            MpClientHandleSessionMessage(ev->channel, &hdr, ev->data + MP_HEADER_SIZE, hdr.length, now);
        }
        break;
    }
}

void MpClient_Update(u32 now)
{
    struct MpSession *s = gMpSession;
    struct MpMsgPing ping;
    u16 len;

    if (s->state == MP_STATE_CONNECTING)
    {
        if ((u32)(now - s->client.joinMs) >= MP_CONNECT_TIMEOUT_MS)
            MpSession_Fail(MP_EVENT_CONNECT_FAILED, MP_REASON_TIMEOUT);
        return;
    }
    if (s->state != MP_STATE_IN_SESSION)
        return;

    if ((u32)(now - s->client.lastRecvMs) >= MP_IDLE_TIMEOUT_MS)
    {
        MpSession_Fail(MP_EVENT_DISCONNECTED, MP_REASON_CONNECTION_LOST);
        return;
    }
    if ((u32)(now - s->client.lastPingSentMs) >= MP_PING_INTERVAL_MS)
    {
        s->client.lastPingSentMs = now;
        ping.timeMs = now;
        len = MpProto_WritePing(sClientBuf, sizeof(sClientBuf), MP_MSG_PING, &ping);
        MpSession_SendTo(MP_HOST_PEER_ID, sClientBuf, len, MP_CHANNEL_PRESENCE, FALSE);
    }
    MpSession_TrySendLocalUpdate(now);
}
