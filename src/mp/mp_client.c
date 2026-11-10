// Client role (protocol.md sections 6 and 7): connect timer, HELLO,
// WELCOME/REJECT, the mirrored player table, BYE, ping, malformed budget.

#include <string.h>
#include "mp/mp_session_internal.h"
#include "mp/mp_summary.h"
#include "mp/mp_link.h"

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

// PS-054: the first message on a reconnect, in place of HELLO.
static void MpClientSendResume(void)
{
    struct MpSession *s = gMpSession;
    struct MpMsgResume resume;
    u16 len;

    memset(&resume, 0, sizeof(resume));
    resume.protocolVersion = s->protocolVersion;
    resume.buildHash = s->buildHash;
    resume.buildFlags = s->buildFlags;
    resume.slot = s->localSlot;
    resume.epoch = (s->localSlot < MP_MAX_PLAYERS) ? s->players[s->localSlot].epoch : 0;
    resume.summary = s->localSummary;
    memcpy(resume.token, s->client.token, MP_SESSION_TOKEN_SIZE);
    len = MpProto_WriteResume(sClientBuf, sizeof(sClientBuf), &resume);
    MpSession_SendTo(MP_HOST_PEER_ID, sClientBuf, len, MP_CHANNEL_CONTROL, TRUE);
    MP_SDLOG("RESUME sent (slot %u, epoch %u, attempt %u)\n", (unsigned)resume.slot, (unsigned)resume.epoch, (unsigned)s->client.attempts);
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

// PS-054: the connection to the host is gone (ENet disconnect or idle timeout) while IN_SESSION.
// Keep the session and the table, and retry the host with RESUME (MpClient_Update). What needs
// the host now ends exactly as if the session had ended: a trade or battle on the cable (PS-048
// recovery; a trade waiting for its commit presumes ABORT and its PS-050 journal stays for the
// recovery after the reconnect), and interaction requests (UNAVAILABLE / CANCELLED).
static void MpClientConnectionLost(u32 now, const char *why)
{
    struct MpSession *s = gMpSession;
    u32 i;

    if (MpProto_TokenIsZero(s->client.token) || s->localSlot >= MP_MAX_PLAYERS || gMpNetTiming.retryWindowMs == 0)
    {
        MpSession_Fail(MP_EVENT_DISCONNECTED, MP_REASON_CONNECTION_LOST);
        return;
    }
    MP_SLOG("connection to the host lost (%s); reconnecting for up to %u ms\n", why, (unsigned)gMpNetTiming.retryWindowMs);
    MpLink_OnSessionEnd();
    for (i = 0; i < MP_MAX_PLAYERS; i++)
    {
        if (i != s->localSlot)
            MpInteract_OnSlotLeaving((u8)i);
        s->players[i].pingMs = 0;
    }
    MpTransport_DisconnectNow(MP_HOST_PEER_ID);
    s->client.transportConnected = FALSE;
    s->client.reconnecting = TRUE;
    s->client.attemptActive = FALSE;
    s->client.attempts = 0;
    s->client.lostMs = now;
    s->client.nextAttemptMs = now + gMpNetTiming.retryFirstMs;
    MpSession_QueueEvent(MP_EVENT_RECONNECTING, s->localSlot, MP_REASON_CONNECTION_LOST);
}

// PS-054: the current RESUME attempt failed (no connection, no answer): wait, then try again.
static void MpClientAttemptFailed(u32 now)
{
    struct MpSession *s = gMpSession;
    u32 wait = gMpNetTiming.retryFirstMs;
    u32 i;

    MpTransport_DisconnectNow(MP_HOST_PEER_ID);
    s->client.transportConnected = FALSE;
    s->client.attemptActive = FALSE;
    for (i = 1; i < s->client.attempts && wait < gMpNetTiming.retryMaxMs; i++)
        wait *= 2;
    if (wait > gMpNetTiming.retryMaxMs)
        wait = gMpNetTiming.retryMaxMs;
    s->client.nextAttemptMs = now + wait;
    MP_SDLOG("reconnect attempt %u failed; next in %u ms\n", (unsigned)s->client.attempts, (unsigned)wait);
}

static void MpClientGiveUp(u8 reason)
{
    struct MpSession *s = gMpSession;

    MP_SLOG("reconnect: giving up after %u ms and %u attempts\n", (unsigned)(MpTransport_GetTimeMs() - s->client.lostMs), (unsigned)s->client.attempts);
    MpSession_Fail(MP_EVENT_DISCONNECTED, reason);
}

// PS-054: the WELCOME that answers our RESUME. Same slot and epoch; the rest of the table may
// have changed while we were away: report the differences as the usual events.
static void MpClientHandleResumed(const struct MpMsgWelcome *welcome)
{
    struct MpSession *s = gMpSession;
    bool8 listed[MP_MAX_PLAYERS];
    u32 i, now = MpTransport_GetTimeMs();

    if (!(welcome->flags & MP_WELCOME_FLAG_RESUMED) || s->localSlot >= MP_MAX_PLAYERS
     || welcome->yourSlot != s->localSlot || welcome->yourEpoch != s->players[s->localSlot].epoch)
    {
        MP_SLOG("reconnect: WELCOME for slot %u epoch %u, not ours\n", (unsigned)welcome->yourSlot, (unsigned)welcome->yourEpoch);
        MpClient_SendBye(MP_BYE_PROTOCOL_ERROR);
        MpClientGiveUp(MP_REASON_PROTOCOL_ERROR);
        return;
    }
    memset(listed, 0, sizeof(listed));
    for (i = 0; i < welcome->playerCount; i++)
    {
        const struct MpMsgPlayerEntry *e = &welcome->players[i];
        bool8 lost = (e->entryFlags & MP_ENTRY_FLAG_LOST) != 0;

        listed[e->slot] = TRUE;
        if (e->slot == s->localSlot)
            continue;
        if (s->slotUsed[e->slot] && s->players[e->slot].epoch == e->epoch)
        {
            // Still the same player: keep its presence (its avatar stays where it is).
            if (memcmp(&s->players[e->slot].summary, &e->summary, sizeof(e->summary)) != 0)
            {
                s->players[e->slot].summary = e->summary;
                MpSession_QueueEvent(MP_EVENT_PLAYER_UPDATED, e->slot, MP_REASON_NONE);
            }
            if (s->players[e->slot].connectionLost != lost)
            {
                s->players[e->slot].connectionLost = lost;
                MpSession_QueueEvent(lost ? MP_EVENT_PLAYER_RECONNECTING : MP_EVENT_PLAYER_RECONNECTED, e->slot, MP_REASON_NONE);
            }
            continue;
        }
        if (s->slotUsed[e->slot])
        {
            MpInteract_OnSlotLeaving(e->slot);
            MpSession_QueueEvent(MP_EVENT_PLAYER_LEFT, e->slot, MP_REASON_CONNECTION_LOST);
            MpSession_ClearSlot(e->slot);
        }
        MpSession_SetSlot(e->slot, e->epoch, &e->summary, FALSE);
        s->players[e->slot].connectionLost = lost;
        MpSession_QueueEvent(MP_EVENT_PLAYER_JOINED, e->slot, MP_REASON_NONE);
    }
    for (i = 0; i < MP_MAX_PLAYERS; i++)
    {
        if (i == s->localSlot || listed[i] || !s->slotUsed[i])
            continue;
        MpInteract_OnSlotLeaving((u8)i);
        MpSession_QueueEvent(MP_EVENT_PLAYER_LEFT, (u8)i, MP_REASON_CONNECTION_LOST);
        MpSession_ClearSlot((u8)i);
    }
    memcpy(s->client.token, welcome->token, MP_SESSION_TOKEN_SIZE);
    s->client.reconnecting = FALSE;
    s->client.attemptActive = FALSE;
    s->client.malformed = 0;
    s->client.lastRecvMs = now;
    s->client.lastPingSentMs = now;
    s->client.lastPingRecvMs = 0;
    MP_SLOG("reconnected as slot %u (epoch %u) after %u ms, %u attempt(s)\n", (unsigned)s->localSlot,
            (unsigned)welcome->yourEpoch, (unsigned)(now - s->client.lostMs), (unsigned)s->client.attempts);
    MpSession_LogTable();
    MpSession_QueueEvent(MP_EVENT_RECONNECTED, s->localSlot, MP_REASON_NONE);
    MpPresence_Update(now); // as after the first WELCOME (protocol.md 6.2)
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
        if (s->client.reconnecting) // PS-054
            MpClientGiveUp(MP_REASON_PROTOCOL_ERROR);
        else
            MpSession_Fail(MP_EVENT_CONNECT_FAILED, MP_REASON_PROTOCOL_ERROR);
        return;
    }
    if (s->client.reconnecting) // PS-054
    {
        MpClientHandleResumed(&welcome);
        return;
    }
    for (i = 0; i < MP_MAX_PLAYERS; i++)
        MpSession_ClearSlot((u8)i);
    for (i = 0; i < welcome.playerCount; i++)
    {
        const struct MpMsgPlayerEntry *e = &welcome.players[i];
        bool8 isLocal = (e->slot == welcome.yourSlot);

        MpSession_SetSlot(e->slot, e->epoch, isLocal ? &s->localSummary : &e->summary, isLocal);
        if (!isLocal)
            s->players[e->slot].connectionLost = (e->entryFlags & MP_ENTRY_FLAG_LOST) != 0; // PS-054
    }
    memcpy(s->client.token, welcome.token, MP_SESSION_TOKEN_SIZE); // PS-054
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
    if (gMpSession->client.reconnecting) // PS-054: the host no longer holds our slot
        MpClientGiveUp(MP_REASON_CONNECTION_LOST);
    else
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
    struct MpMsgPlayerStatus status;
    struct MpMsgBye bye;
    struct MpMsgPing ping;
    u32 rtt;
    u16 outLen;
    u8 reason;
    bool8 lost;

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
        MpInteract_OnSlotLeaving(left.slot); // PS-041: end our requests with that player
        MpSession_QueueEvent(MP_EVENT_PLAYER_LEFT, left.slot, reason);
        MpSession_ClearSlot(left.slot);
        MP_SLOG("slot %u left: %s\n", (unsigned)left.slot, MP_GetReasonName(reason));
        MpSession_LogTable();
        break;
    case MP_MSG_PLAYER_STATUS: // PS-054
        if (channel != MP_CHANNEL_CONTROL || MpProto_ParsePlayerStatus(payload, len, &status) != MP_PARSE_OK
         || !MpClientIsRemoteSlot(status.slot))
        {
            MpClientMalformed("PLAYER_STATUS");
            break;
        }
        if (!s->slotUsed[status.slot] || s->players[status.slot].epoch != status.epoch)
            break; // stale
        lost = (status.status == MP_PLAYER_STATUS_LOST);
        if (s->players[status.slot].connectionLost == lost)
            break;
        s->players[status.slot].connectionLost = lost;
        if (lost)
            MpInteract_OnSlotLeaving(status.slot); // our requests with that player end (the host ended them too)
        MpSession_QueueEvent(lost ? MP_EVENT_PLAYER_RECONNECTING : MP_EVENT_PLAYER_RECONNECTED, status.slot,
                             lost ? MP_REASON_CONNECTION_LOST : MP_REASON_NONE);
        MP_SLOG("slot %u %s\n", (unsigned)status.slot, lost ? "lost its connection (slot held)" : "reconnected");
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
    case MP_MSG_INTERACT_REQUEST:   // PS-041 (protocol.md 6.11)
    case MP_MSG_INTERACT_RESPONSE:
    case MP_MSG_INTERACT_CANCEL:
    case MP_MSG_TRAINER_CARD_DATA:  // PS-042 (protocol.md 6.13)
    case MP_MSG_PARTY_DATA:         // PS-043 (protocol.md 6.14)
        if (channel != MP_CHANNEL_CONTROL)
            MpClientMalformed("INTERACT wrong channel");
        else if (!MpInteract_OnClientReceive(hdr->type, payload, len))
            MpClientMalformed("INTERACT");
        break;
    default:
        // PS-046: virtual link cable (protocol.md 6.12)
        if (hdr->type >= MP_MSG_LINK_FIRST && hdr->type <= MP_MSG_LINK_LAST)
        {
            if (!MpLink_OnClientReceive(channel, hdr->type, payload, len))
                MpClientMalformed("LINK message");
            break;
        }
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
        else if (s->state == MP_STATE_IN_SESSION && s->client.reconnecting) // PS-054
            MpClientSendResume();
        break;
    case MP_TRANSPORT_EV_DISCONNECT:
        s->client.transportConnected = FALSE;
        if (s->state == MP_STATE_CONNECTING)
            MpSession_Fail(MP_EVENT_CONNECT_FAILED, MP_REASON_CONNECTION_LOST);
        else if (s->state == MP_STATE_IN_SESSION && s->client.reconnecting)
            MpClientAttemptFailed(now); // PS-054
        else if (s->state == MP_STATE_IN_SESSION)
            MpClientConnectionLost(now, "transport disconnect"); // PS-054 (was: DISCONNECTED)
        break;
    case MP_TRANSPORT_EV_RECEIVE:
        s->client.lastRecvMs = now;
        if (!MpProto_ParseHeader(ev->data, ev->length, &hdr))
        {
            MpClientMalformed("bad header");
            break;
        }
        if (s->state == MP_STATE_CONNECTING || (s->state == MP_STATE_IN_SESSION && s->client.reconnecting)) // PS-054: a RESUME is answered like a HELLO
        {
            if (hdr.type == MP_MSG_REJECT && ev->channel == MP_CHANNEL_CONTROL)
                MpClientHandleReject(ev->data + MP_HEADER_SIZE, hdr.length); // any header version (frozen)
            else if (hdr.type == MP_MSG_WELCOME && ev->channel == MP_CHANNEL_CONTROL && hdr.version == MP_PROTOCOL_VERSION)
                MpClientHandleWelcome(ev->data + MP_HEADER_SIZE, hdr.length);
            else if ((hdr.type == MP_MSG_PRESENCE || hdr.type == MP_MSG_PING || hdr.type == MP_MSG_PONG)
                  && ev->channel == MP_CHANNEL_PRESENCE)
                break; // channel-1 traffic overtook a delayed/retransmitted WELCOME on channel 0:
                       // an expected race, ignored silently (BUG-001, protocol.md 6.8). Channel-0
                       // messages are ordered after WELCOME, so they cannot race it.
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

    if (s->client.reconnecting) // PS-054: retry with backoff until the window closes
    {
        if ((u32)(now - s->client.lostMs) >= gMpNetTiming.retryWindowMs)
        {
            MpClientGiveUp(MP_REASON_CONNECTION_LOST);
            return;
        }
        if (s->client.attemptActive && (u32)(now - s->client.attemptStartMs) >= gMpNetTiming.attemptMs)
            MpClientAttemptFailed(now);
        if (!s->client.attemptActive && (s32)(now - s->client.nextAttemptMs) >= 0)
        {
            s->client.attempts++;
            s->client.attemptActive = TRUE;
            s->client.attemptStartMs = now;
            if (!MpTransport_Reconnect(s->hostIp, s->port))
                MpClientAttemptFailed(now);
            else
                MP_SDLOG("reconnect attempt %u\n", (unsigned)s->client.attempts);
        }
        return;
    }
    if ((u32)(now - s->client.lastRecvMs) >= gMpNetTiming.idleTimeoutMs)
    {
        MpClientConnectionLost(now, "nothing received"); // PS-054 (was: DISCONNECTED)
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
