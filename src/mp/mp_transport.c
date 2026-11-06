// ENet transport wrapper (design.md section 2.1, protocol.md section 1).
// This is the only file in the project that includes <enet/enet.h>.
// Main thread only; never blocks (enet_host_service timeout is always 0).

#include <enet/enet.h> // first, so the Windows headers come before ours
#include <string.h>

#include "mp/mp_internal.h"
#include "mp/mp_protocol.h"
#include "mp/mp_transport.h"

#define MP_ENET_MAX_WAITING_DATA    16384
#define MP_ENET_TIMEOUT_MIN_MS      5000
#define MP_ENET_TIMEOUT_MAX_MS      10000

struct MpTransportContext
{
    ENetHost *host;
    bool8 isServer;
};

static struct MpTransportContext sContexts[MP_TRANSPORT_INSTANCE_COUNT];
static struct MpTransportContext *sCtx = &sContexts[0];
static bool8 sEnetInitialized = FALSE;

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

static void MpCloseContext(struct MpTransportContext *ctx)
{
    if (ctx->host != NULL)
    {
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
    MP_DLOG("transport: connecting to %s:%u\n", ipv4, (unsigned)port);
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
        enet_peer_timeout(ev->peer, 0, MP_ENET_TIMEOUT_MIN_MS, MP_ENET_TIMEOUT_MAX_MS);
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

u16 MpTransport_GetRttMs(u8 peerId)
{
    ENetPeer *peer = MpGetPeer(peerId);

    if (peer == NULL || peer->state != ENET_PEER_STATE_CONNECTED)
        return 0;
    return (peer->roundTripTime > 0xFFFF) ? 0xFFFF : (u16)peer->roundTripTime;
}
