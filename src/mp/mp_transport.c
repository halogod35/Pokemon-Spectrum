// ENet transport wrapper (design.md section 2.1, protocol.md section 1).
// This is the only file in the project that includes <enet/enet.h>.
// Main thread only; never blocks (enet_host_service timeout is always 0).

#include <enet/enet.h> // first, so the Windows headers come before ours
#include <string.h>

#include "mp/mp_internal.h"
#include "mp/mp_netstats.h"
#include "mp/mp_protocol.h"
#include "mp/mp_transport.h"

#define MP_ENET_MAX_WAITING_DATA    16384

struct MpTransportContext
{
    ENetHost *host;
    bool8 isServer;
    // PS-054 simulated network conditions (kept across Close / Listen / Connect)
    u16 simLossPermille;
    bool8 simBlackout;
    bool8 simBlackoutTimed;
    u32 simBlackoutUntilMs;
    u32 simRng;
    u32 simDropped;         // datagrams dropped (both directions) since the instance was set up
};

static struct MpTransportContext sContexts[MP_TRANSPORT_INSTANCE_COUNT];
static struct MpTransportContext *sCtx = &sContexts[0];
static bool8 sEnetInitialized = FALSE;

// PS-054: ENet peer timeouts (MpTransport_SetPeerTimeouts). A peer whose reliable data stays
// unacknowledged for timeoutMin (after ENet's retries) or timeoutMax is dropped.
static u32 sPeerTimeoutMinMs = MP_TRANSPORT_TIMEOUT_MIN_MS;
static u32 sPeerTimeoutMaxMs = MP_TRANSPORT_TIMEOUT_MAX_MS;
static u32 sPeerPingMs = MP_TRANSPORT_PING_MS;

void MpTransport_SetPeerTimeouts(u32 minMs, u32 maxMs, u32 pingMs)
{
    sPeerTimeoutMinMs = minMs ? minMs : MP_TRANSPORT_TIMEOUT_MIN_MS;
    sPeerTimeoutMaxMs = (maxMs >= sPeerTimeoutMinMs) ? maxMs : sPeerTimeoutMinMs;
    sPeerPingMs = pingMs ? pingMs : MP_TRANSPORT_PING_MS;
}

static void MpApplyPeerTimeouts(ENetPeer *peer)
{
    enet_peer_timeout(peer, 0, sPeerTimeoutMinMs, sPeerTimeoutMaxMs);
    enet_peer_ping_interval(peer, sPeerPingMs);
}

// ---------------------------------------------------------------------------
// PS-054 simulated network conditions (ENet intercept + the sendFilter hook)
// ---------------------------------------------------------------------------

static struct MpTransportContext *MpContextOfHost(const ENetHost *host)
{
    u32 i;

    for (i = 0; i < MP_TRANSPORT_INSTANCE_COUNT; i++)
    {
        if (sContexts[i].host == host)
            return &sContexts[i];
    }
    return NULL;
}

static void MpSimExpire(struct MpTransportContext *ctx, u32 now)
{
    if (ctx->simBlackout && ctx->simBlackoutTimed && (s32)(now - ctx->simBlackoutUntilMs) >= 0)
    {
        ctx->simBlackout = FALSE;
        MP_LOG("net sim: instance %u: simulated dropout over, network back\n", (unsigned)(ctx - sContexts));
    }
}

// TRUE if this datagram is lost (blackout, or the random loss share).
static bool8 MpSimDrops(struct MpTransportContext *ctx)
{
    if (ctx == NULL)
        return FALSE;
    MpSimExpire(ctx, (u32)enet_time_get());
    if (ctx->simBlackout)
    {
        ctx->simDropped++;
        return TRUE;
    }
    if (ctx->simLossPermille == 0)
        return FALSE;
    ctx->simRng ^= ctx->simRng << 13;
    ctx->simRng ^= ctx->simRng >> 17;
    ctx->simRng ^= ctx->simRng << 5;
    if ((ctx->simRng % 1000) < ctx->simLossPermille)
    {
        ctx->simDropped++;
        return TRUE;
    }
    return FALSE;
}

static int ENET_CALLBACK MpSimIntercept(ENetHost *host, ENetEvent *event)
{
    (void)event;
    return MpSimDrops(MpContextOfHost(host)) ? 1 : 0;
}

static int ENET_CALLBACK MpSimSendFilter(ENetHost *host, const ENetAddress *address)
{
    (void)address;
    return MpSimDrops(MpContextOfHost(host)) ? 1 : 0;
}

void MpTransport_SetSimLoss(u16 permille)
{
    sCtx->simLossPermille = (permille > 1000) ? 1000 : permille;
    if (sCtx->simRng == 0)
        sCtx->simRng = 0x9E3779B9u ^ (u32)(sCtx - sContexts);
}

u16 MpTransport_GetSimLoss(void)
{
    return sCtx->simLossPermille;
}

void MpTransport_SetSimBlackout(bool8 on, u32 durationMs)
{
    u32 now = (u32)enet_time_get();

    if (on)
    {
        sCtx->simBlackout = TRUE;
        sCtx->simBlackoutTimed = (durationMs != 0);
        sCtx->simBlackoutUntilMs = now + durationMs;
        if (durationMs != 0)
            MP_LOG("net sim: instance %u: simulated dropout for %u ms (nothing sent or received)\n", (unsigned)(sCtx - sContexts), (unsigned)durationMs);
        else
            MP_LOG("net sim: instance %u: simulated dropout until it is switched off\n", (unsigned)(sCtx - sContexts));
    }
    else if (sCtx->simBlackout)
    {
        sCtx->simBlackout = FALSE;
        MP_LOG("net sim: instance %u: simulated dropout switched off, network back\n", (unsigned)(sCtx - sContexts));
    }
}

bool8 MpTransport_GetSimBlackout(u32 *leftMs)
{
    u32 now = (u32)enet_time_get();

    MpSimExpire(sCtx, now);
    if (leftMs != NULL)
        *leftMs = (sCtx->simBlackout && sCtx->simBlackoutTimed) ? (u32)(sCtx->simBlackoutUntilMs - now) : 0;
    return sCtx->simBlackout;
}

u32 MpTransport_GetSimDropped(void)
{
    return sCtx->simDropped;
}

bool8 MpTransport_Init(void)
{
    if (sEnetInitialized)
        return TRUE;
    if (enet_initialize() != 0)
    {
        MP_LOG("ERROR: enet_initialize failed\n");
        return FALSE;
    }
    sEnetInitialized = TRUE;
    MP_DLOG("transport: ENet %d.%d.%d initialized\n", ENET_VERSION_MAJOR, ENET_VERSION_MINOR, ENET_VERSION_PATCH);
    return TRUE;
}

static u8 MpContextIndex(const struct MpTransportContext *ctx)
{
    return (u8)(ctx - sContexts);
}

// PS-053: ENet's counters for `ctx` (all zero if it has no host).
static void MpFillStats(const struct MpTransportContext *ctx, struct MpTransportStats *out)
{
    ENetHost *host = ctx->host;
    size_t i;

    memset(out, 0, sizeof(*out));
    if (host == NULL)
        return;
    out->open = TRUE;
    out->wireSentBytes = host->totalSentData;
    out->wireRecvBytes = host->totalReceivedData;
    out->wireSentDatagrams = host->totalSentPackets;
    out->wireRecvDatagrams = host->totalReceivedPackets;
    for (i = 0; i < host->peerCount && i < MP_TRANSPORT_STATS_PEERS; i++)
    {
        const ENetPeer *peer = &host->peers[i];
        struct MpTransportPeerStats *p = &out->peers[i];

        p->connected = (peer->state == ENET_PEER_STATE_CONNECTED);
        if (!p->connected)
            continue;
        p->rttMs = peer->roundTripTime;
        p->rttVarMs = peer->roundTripTimeVariance;
        p->lossPermille = (u32)(((u64)peer->packetLoss * 1000) / ENET_PEER_PACKET_LOSS_SCALE);
        if (p->lossPermille > 1000)
            p->lossPermille = 1000;
    }
}

void MpTransport_GetStats(struct MpTransportStats *out)
{
    if (out != NULL)
        MpFillStats(sCtx, out);
}

u8 MpTransport_GetInstance(void)
{
    return MpContextIndex(sCtx);
}

static void MpCloseContext(struct MpTransportContext *ctx)
{
    if (ctx->host != NULL)
    {
        struct MpTransportStats stats;

        MpFillStats(ctx, &stats);
        MpNetStats_OnClose(MpContextIndex(ctx), &stats, (u32)enet_time_get()); // PS-053: session-end table
        enet_host_destroy(ctx->host);
        ctx->host = NULL;
    }
    ctx->isServer = FALSE;
}

void MpTransport_Shutdown(void)
{
    u32 i;

    for (i = 0; i < MP_TRANSPORT_INSTANCE_COUNT; i++)
        MpCloseContext(&sContexts[i]);
    sCtx = &sContexts[0];
    if (sEnetInitialized)
    {
        enet_deinitialize();
        sEnetInitialized = FALSE;
    }
}

void MpTransport_SelectInstance(u8 instance)
{
    if (instance < MP_TRANSPORT_INSTANCE_COUNT)
        sCtx = &sContexts[instance];
}

static void MpApplyHostLimits(ENetHost *host)
{
    // ENet's default maximumPacketSize is 32 MB: a memory DoS (protocol.md section 1).
    host->maximumPacketSize = MP_MAX_PACKET_SIZE;
    host->maximumWaitingData = MP_ENET_MAX_WAITING_DATA;
    // PS-054: simulated network conditions. Both hooks do nothing unless a test or the
    // --mp-net-loss / F4 dev tools switched them on for this instance.
    host->intercept = MpSimIntercept;
    host->sendFilter = MpSimSendFilter;
}

bool8 MpTransport_Listen(u16 port, u8 maxPeers)
{
    ENetAddress address;
    ENetHost *host;

    if (sCtx->host != NULL || maxPeers == 0 || port == 0)
        return FALSE;
    if (!MpTransport_Init())
        return FALSE;

    memset(&address, 0, sizeof(address));
    address.host = ENET_HOST_ANY;
    address.port = port;
    host = enet_host_create(&address, maxPeers, MP_CHANNEL_COUNT, 0, 0);
    if (host == NULL)
    {
        MP_LOG("ERROR: cannot listen on UDP port %u (in use?)\n", (unsigned)port);
        return FALSE;
    }
    MpApplyHostLimits(host);
    sCtx->host = host;
    sCtx->isServer = TRUE;
    MpNetStats_OnOpen(MpContextIndex(sCtx), (u32)enet_time_get());
    MP_DLOG("transport: listening on 0.0.0.0:%u, %u peers\n", (unsigned)port, (unsigned)maxPeers);
    return TRUE;
}

bool8 MpTransport_IsValidIPv4(const char *text)
{
    u32 octets = 0;
    u32 digits = 0;
    u32 value = 0;
    const char *c;

    if (text == NULL)
        return FALSE;

    for (c = text; ; c++)
    {
        if (*c >= '0' && *c <= '9')
        {
            if (++digits > 3)
                return FALSE;
            value = value * 10 + (u32)(*c - '0');
            if (value > 255)
                return FALSE;
        }
        else if (*c == '.' || *c == '\0')
        {
            if (digits == 0)
                return FALSE;
            octets++;
            if (*c == '\0')
                break;
            if (octets >= 4)
                return FALSE;
            digits = 0;
            value = 0;
        }
        else
        {
            return FALSE;
        }
    }
    return octets == 4;
}

bool8 MpTransport_Connect(const char *ipv4, u16 port)
{
    ENetAddress address;
    ENetHost *host;
    ENetPeer *peer;

    if (sCtx->host != NULL || port == 0)
        return FALSE;
    if (!MpTransport_IsValidIPv4(ipv4))
    {
        MP_LOG("ERROR: '%s' is not an IPv4 address (a.b.c.d)\n", ipv4 != NULL ? ipv4 : "(null)");
        return FALSE;
    }
    if (!MpTransport_Init())
        return FALSE;

    memset(&address, 0, sizeof(address));
    // Never enet_address_set_host: that resolves DNS and blocks (AD-4).
    if (enet_address_set_host_ip(&address, ipv4) != 0)
        return FALSE;
    address.port = port;

    host = enet_host_create(NULL, 1, MP_CHANNEL_COUNT, 0, 0);
    if (host == NULL)
    {
        MP_LOG("ERROR: cannot create the client socket\n");
        return FALSE;
    }
    MpApplyHostLimits(host);

    peer = enet_host_connect(host, &address, MP_CHANNEL_COUNT, MP_CONNECT_MAGIC);
    if (peer == NULL)
    {
        MP_LOG("ERROR: enet_host_connect failed\n");
        enet_host_destroy(host);
        return FALSE;
    }
    sCtx->host = host;
    sCtx->isServer = FALSE;
    MpNetStats_OnOpen(MpContextIndex(sCtx), (u32)enet_time_get());
    MP_DLOG("transport: connecting to %s:%u\n", ipv4, (unsigned)port);
    return TRUE;
}

bool8 MpTransport_Reconnect(const char *ipv4, u16 port)
{
    ENetAddress address;
    ENetPeer *peer;
    size_t i;

    if (sCtx->host == NULL || sCtx->isServer || port == 0 || !MpTransport_IsValidIPv4(ipv4))
        return FALSE;
    memset(&address, 0, sizeof(address));
    if (enet_address_set_host_ip(&address, ipv4) != 0)
        return FALSE;
    address.port = port;
    // Forget the old connection without a word (the host has dropped it, or will).
    for (i = 0; i < sCtx->host->peerCount; i++)
    {
        if (sCtx->host->peers[i].state != ENET_PEER_STATE_DISCONNECTED)
            enet_peer_reset(&sCtx->host->peers[i]);
    }
    peer = enet_host_connect(sCtx->host, &address, MP_CHANNEL_COUNT, MP_CONNECT_MAGIC);
    if (peer == NULL)
    {
        MP_LOG("ERROR: enet_host_connect failed (reconnect)\n");
        return FALSE;
    }
    MpApplyPeerTimeouts(peer);
    MP_DLOG("transport: reconnecting to %s:%u\n", ipv4, (unsigned)port);
    return TRUE;
}

// Returns the peer for `id`, or NULL if there is no host or `id` is out of range.
static ENetPeer *MpGetPeer(u8 id)
{
    if (sCtx->host == NULL || id >= sCtx->host->peerCount)
        return NULL;
    return &sCtx->host->peers[id];
}

static u8 MpGetPeerId(const ENetPeer *peer)
{
    size_t index;

    if (sCtx->host == NULL || peer < sCtx->host->peers)
        return MP_PEER_NONE;
    index = (size_t)(peer - sCtx->host->peers);
    if (index >= sCtx->host->peerCount || index >= MP_PEER_NONE)
        return MP_PEER_NONE;
    return (u8)index;
}

bool8 MpTransport_Send(u8 peerId, u8 channel, const u8 *data, u16 len, bool8 reliable)
{
    ENetPeer *peer = MpGetPeer(peerId);
    ENetPacket *packet;

    if (peer == NULL || channel >= MP_CHANNEL_COUNT || len > MP_MAX_PACKET_SIZE)
        return FALSE;
    if (data == NULL && len != 0)
        return FALSE;
    if (peer->state != ENET_PEER_STATE_CONNECTED)
        return FALSE;

    packet = enet_packet_create(data, len, reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    if (packet == NULL)
        return FALSE;
    if (enet_peer_send(peer, channel, packet) < 0)
    {
        // enet_peer_send only takes a reference on success.
        if (packet->referenceCount == 0)
            enet_packet_destroy(packet);
        return FALSE;
    }
    MpNetStats_OnSend(MpContextIndex(sCtx), peerId, data, len);
    return TRUE;
}

static void MpDispatchEvent(ENetEvent *ev, MpTransportCallback callback)
{
    struct MpTransportEvent out;
    u8 peerId = MpGetPeerId(ev->peer);

    memset(&out, 0, sizeof(out));
    out.peer = peerId;

    switch (ev->type)
    {
    case ENET_EVENT_TYPE_CONNECT:
        if (peerId == MP_PEER_NONE)
        {
            enet_peer_reset(ev->peer);
            return;
        }
        if (sCtx->isServer && ev->data != MP_CONNECT_MAGIC)
        {
            // Not one of ours (protocol.md section 1): drop it without a word.
            MP_DLOG("transport: peer %u: wrong connect magic 0x%08X, reset\n", (unsigned)peerId, (unsigned)ev->data);
            enet_peer_reset(ev->peer);
            return;
        }
        MpApplyPeerTimeouts(ev->peer);
        MP_DLOG("transport: peer %u connected\n", (unsigned)peerId);
        out.type = MP_TRANSPORT_EV_CONNECT;
        if (callback != NULL)
            callback(&out);
        break;
    case ENET_EVENT_TYPE_DISCONNECT:
        MP_DLOG("transport: peer %u disconnected\n", (unsigned)peerId);
        if (peerId == MP_PEER_NONE)
            return;
        out.type = MP_TRANSPORT_EV_DISCONNECT;
        if (callback != NULL)
            callback(&out);
        break;
    case ENET_EVENT_TYPE_RECEIVE:
        if (ev->packet == NULL)
            return;
        if (peerId != MP_PEER_NONE && ev->packet->dataLength <= MP_MAX_PACKET_SIZE && callback != NULL)
        {
            // Before the callback: it may close this host (MpCloseContext logs the table).
            MpNetStats_OnRecv(MpContextIndex(sCtx), peerId, ev->packet->data, (u32)ev->packet->dataLength);
            out.type = MP_TRANSPORT_EV_RECEIVE;
            out.channel = ev->channelID;
            out.data = ev->packet->data;
            out.length = (u32)ev->packet->dataLength;
            callback(&out);
        }
        else
        {
            MP_DLOG("transport: peer %u: dropped packet of %u bytes\n", (unsigned)peerId, (unsigned)ev->packet->dataLength);
        }
        enet_packet_destroy(ev->packet);
        break;
    default:
        break;
    }
}

void MpTransport_Poll(MpTransportCallback callback)
{
    struct MpTransportContext *ctx = sCtx;
    ENetEvent ev;
    u32 n;

    for (n = 0; n < MP_TRANSPORT_MAX_EVENTS_PER_POLL; n++)
    {
        int result;

        // The callback may have closed the host or switched instance.
        if (sCtx != ctx || ctx->host == NULL)
            break;
        result = enet_host_service(ctx->host, &ev, 0);
        if (result < 0)
        {
            MP_LOG("ERROR: enet_host_service failed\n");
            break;
        }
        if (result == 0)
            break;
        MpDispatchEvent(&ev, callback);
    }
}

void MpTransport_Disconnect(u8 peerId)
{
    ENetPeer *peer = MpGetPeer(peerId);

    if (peer != NULL && peer->state != ENET_PEER_STATE_DISCONNECTED)
        enet_peer_disconnect_later(peer, 0);
}

void MpTransport_DisconnectNow(u8 peerId)
{
    ENetPeer *peer = MpGetPeer(peerId);

    if (peer != NULL && peer->state != ENET_PEER_STATE_DISCONNECTED)
        enet_peer_reset(peer);
}

void MpTransport_Close(void)
{
    MpCloseContext(sCtx);
}

void MpTransport_Flush(void)
{
    if (sCtx->host != NULL)
        enet_host_flush(sCtx->host);
}

bool8 MpTransport_IsOpen(void)
{
    return sCtx->host != NULL;
}

u32 MpTransport_GetTimeMs(void)
{
    return (u32)enet_time_get();
}

u32 MpTransport_DebugSendRandomUdp(const char *ipv4, u16 port, u32 totalBytes, u32 seed)
{
    ENetAddress address;
    ENetSocket sock;
    ENetBuffer buffer;
    static u8 sData[1400];
    u32 state = (seed != 0) ? seed : 0x12345678u;
    u32 sent = 0;
    u32 bytes = 0;
    u32 len, i;

    if (!MpTransport_IsValidIPv4(ipv4) || !MpTransport_Init())
        return 0;
    memset(&address, 0, sizeof(address));
    if (enet_address_set_host_ip(&address, ipv4) != 0)
        return 0;
    address.port = port;
    sock = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    if (sock == ENET_SOCKET_NULL)
        return 0;

    while (bytes < totalBytes)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        len = state % (sizeof(sData) + 1);
        for (i = 0; i < len; i++)
        {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            sData[i] = (u8)(state >> 24);
        }
        buffer.data = sData;
        buffer.dataLength = len;
        if (enet_socket_send(sock, &address, &buffer, 1) >= 0)
            sent++;
        bytes += (len != 0) ? len : 1;
    }
    enet_socket_destroy(sock);
    return sent;
}

u16 MpTransport_GetRttMs(u8 peerId)
{
    ENetPeer *peer = MpGetPeer(peerId);

    if (peer == NULL || peer->state != ENET_PEER_STATE_CONNECTED)
        return 0;
    return (peer->roundTripTime > 0xFFFF) ? 0xFFFF : (u16)peer->roundTripTime;
}
