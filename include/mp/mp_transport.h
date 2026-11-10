#ifndef GUARD_MP_MP_TRANSPORT_H
#define GUARD_MP_MP_TRANSPORT_H

// ENet wrapper (design.md section 2.1, PS-011). Internal to src/mp.
// No ENet types appear here: peers are small integer ids. Only
// src/mp/mp_transport.c includes <enet/enet.h>.
//
// One active ENet host at a time (a listen server OR a client). For the
// --mp-selftest loopback test there is a second, independent instance that
// is selected with MpTransport_SelectInstance(); normal code never calls it.
//
// Nothing here blocks: enet_host_service is always called with timeout 0.

#include "gba/types.h"

#define MP_CHANNEL_CONTROL      0   // reliable
#define MP_CHANNEL_PRESENCE     1   // unreliable sequenced
#define MP_CHANNEL_COUNT        2
#define MP_PEER_NONE            0xFF
#define MP_TRANSPORT_MAX_EVENTS_PER_POLL 64
#define MP_TRANSPORT_INSTANCE_COUNT 8   // 0 = normal, 1..7 = selftest only

enum
{
    MP_TRANSPORT_EV_CONNECT,
    MP_TRANSPORT_EV_DISCONNECT,
    MP_TRANSPORT_EV_RECEIVE,
};

struct MpTransportEvent
{
    u8 type;            // MP_TRANSPORT_EV_*
    u8 peer;            // 0..peerCount-1 (a client's only peer, the host, is 0)
    u8 channel;         // RECEIVE only
    const u8 *data;     // RECEIVE only. Valid only during the callback. Never keep it.
    u32 length;         // RECEIVE only. Raw length (<= MP_MAX_PACKET_SIZE), NOT yet validated.
};

typedef void (*MpTransportCallback)(const struct MpTransportEvent *ev);

// Initialises ENet (on Windows: WSAStartup, timeBeginPeriod). Opens no socket.
// Idempotent; Listen/Connect call it automatically, so single-player never touches ENet.
bool8 MpTransport_Init(void);
// Closes every instance and deinitialises ENet. Idempotent.
void MpTransport_Shutdown(void);
// Creates a listen server on 0.0.0.0:`port` for `maxPeers` peers. FALSE if an ENet host
// already exists in this instance, or the port cannot be bound.
bool8 MpTransport_Listen(u16 port, u8 maxPeers);
// Creates a client host and starts connecting to `ipv4` (strict dotted quad, never DNS)
// on `port`. The result arrives later as a CONNECT or DISCONNECT event for peer 0.
bool8 MpTransport_Connect(const char *ipv4, u16 port);
// PS-054: a client host that lost its peer dials `ipv4`:`port` again on the same ENet host
// (same socket, same statistics): every peer is reset silently, then a new connect starts.
// The result arrives as a CONNECT or DISCONNECT event for peer 0. FALSE if this instance has
// no client host or the address is invalid.
bool8 MpTransport_Reconnect(const char *ipv4, u16 port);
// PS-054: ENet peer timeouts for peers connecting from now on (enet_peer_timeout limit 0 =
// ENet's default retry count, minimum, maximum; enet_peer_ping_interval). 0 = the default.
#define MP_TRANSPORT_TIMEOUT_MIN_MS 4000
#define MP_TRANSPORT_TIMEOUT_MAX_MS 7000
#define MP_TRANSPORT_PING_MS        500     // ENet's own default
void MpTransport_SetPeerTimeouts(u32 minMs, u32 maxMs, u32 pingMs);
// PS-054: simulated network conditions on the selected instance, both directions, kept until
// changed (Close / Listen / Connect do not reset them). Loss: that share of datagrams (0..1000
// per mille) is dropped at random. Blackout: every datagram is dropped, for `durationMs`
// (0 = until switched off). GetSimBlackout also gives the time left (0 if untimed).
void MpTransport_SetSimLoss(u16 permille);
u16 MpTransport_GetSimLoss(void);
void MpTransport_SetSimBlackout(bool8 on, u32 durationMs);
bool8 MpTransport_GetSimBlackout(u32 *leftMs);
u32 MpTransport_GetSimDropped(void);
// Queues `len` bytes on `channel` to `peer`. FALSE if the peer is not connected, or the
// arguments are out of range (len > MP_MAX_PACKET_SIZE, channel >= MP_CHANNEL_COUNT).
bool8 MpTransport_Send(u8 peer, u8 channel, const u8 *data, u16 len, bool8 reliable);
// Services the host: up to MP_TRANSPORT_MAX_EVENTS_PER_POLL events, timeout 0.
// Incoming connections with the wrong connect magic are reset without an event.
// The callback may call MpTransport_Send/Disconnect/Close.
void MpTransport_Poll(MpTransportCallback callback);
// Graceful disconnect (enet_peer_disconnect_later): queued packets go out first and a
// DISCONNECT event follows.
void MpTransport_Disconnect(u8 peer);
// Immediate reset (enet_peer_reset). No event, nothing is sent.
void MpTransport_DisconnectNow(u8 peer);
// Destroys this instance's ENet host (all peers are reset silently).
void MpTransport_Close(void);
// Sends everything queued now (enet_host_flush).
void MpTransport_Flush(void);
// TRUE if this instance has an ENet host.
bool8 MpTransport_IsOpen(void);
// Milliseconds from enet_time_get(). Wraps; compare with (u32)(now - then).
u32 MpTransport_GetTimeMs(void);
// ENet's smoothed round-trip time for `peer` in ms, 0 if unknown.
u16 MpTransport_GetRttMs(u8 peer);
// PS-053: ENet's own counters for this instance (mp_netstats.c, the F3 overlay).
#define MP_TRANSPORT_STATS_PEERS 6  // == MP_HOST_PEER_COUNT (the host's peer count)

struct MpTransportPeerStats
{
    bool8 connected;
    u32 rttMs;              // ENet's smoothed round-trip time
    u32 rttVarMs;           // ENet's round-trip time variance
    u32 lossPermille;       // ENet's mean loss of reliable packets (10 s windows), 0..1000
};

struct MpTransportStats
{
    bool8 open;
    u32 wireSentBytes;      // UDP payload bytes sent (ENet host total: our data + ENet headers/acks/pings)
    u32 wireRecvBytes;
    u32 wireSentDatagrams;
    u32 wireRecvDatagrams;
    struct MpTransportPeerStats peers[MP_TRANSPORT_STATS_PEERS];
};

// Fills `out` for this instance (all zero if no host).
void MpTransport_GetStats(struct MpTransportStats *out);
// The selected instance (0 = the game).
u8 MpTransport_GetInstance(void);
// Selftest only: makes `instance` (0 or 1) the target of every other call.
void MpTransport_SelectInstance(u8 instance);
// Strict IPv4 dotted-quad check: four decimal octets 0..255, 1-3 digits each.
bool8 MpTransport_IsValidIPv4(const char *text);
// Selftest only (AD-9 flood test): sends about `totalBytes` of random UDP datagrams
// (0..1400 bytes each, xorshift PRNG from `seed`) from a fresh plain socket to
// ipv4:port. Not ENet traffic. Returns the number of datagrams sent.
u32 MpTransport_DebugSendRandomUdp(const char *ipv4, u16 port, u32 totalBytes, u32 seed);

#endif // GUARD_MP_MP_TRANSPORT_H
