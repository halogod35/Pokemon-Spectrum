#ifndef GUARD_MP_MP_NETSTATS_H
#define GUARD_MP_MP_NETSTATS_H

// Network traffic counters (PS-053). Internal to src/mp.
//
// mp_transport.c reports every packet it hands to ENet (MpTransport_Send) and every packet
// ENet delivers (RECEIVE), per transport instance, per peer and per message type (the
// header's type byte). The wire side (ENet's own headers, acks, pings, connects) comes from
// ENet's host totals, which are UDP payload bytes; IP + UDP headers add about
// MP_NETSTATS_IP_UDP_OVERHEAD bytes per datagram on top.
//
// Nothing here changes what is sent. The counters are always on (a few adds per packet);
// --mp-net-stats only turns on the log lines (a summary every MP_NETSTATS_REPORT_MS and a
// full table when the transport closes). The F3 overlay reads MP_GetNetOverlay() (mp.h).

#include "gba/types.h"
#include "mp/mp_transport.h"

#define MP_NETSTATS_PEERS           MP_TRANSPORT_STATS_PEERS
#define MP_NETSTATS_TYPES           256
#define MP_NETSTATS_REPORT_MS       10000   // --mp-net-stats summary interval
#define MP_NETSTATS_RATE_MS         1000    // overlay rate window
#define MP_NETSTATS_IP_UDP_OVERHEAD 28      // IPv4 (20) + UDP (8) per datagram

struct MpNetTypeStats
{
    u32 sentCount;
    u32 sentBytes;      // message bytes (header + payload) handed to ENet
    u32 recvCount;
    u32 recvBytes;
};

// Totals of the selected instance since its transport opened.
struct MpNetTotals
{
    u32 elapsedMs;
    u32 msgSentCount;
    u32 msgSentBytes;
    u32 msgRecvCount;
    u32 msgRecvBytes;
    u32 wireSentBytes;      // ENet host totals: UDP payload (ENet headers, acks, pings included)
    u32 wireRecvBytes;
    u32 wireSentDatagrams;
    u32 wireRecvDatagrams;
};

// mp_transport.c hooks. `instance` is the transport instance (0 = the game).
void MpNetStats_OnOpen(u8 instance, u32 now);
void MpNetStats_OnSend(u8 instance, u8 peer, const u8 *data, u32 len);
void MpNetStats_OnRecv(u8 instance, u8 peer, const u8 *data, u32 len);
// The transport is about to destroy the host: `stats` is its last snapshot. Logs the full
// table when --mp-net-stats is on, then marks the instance closed (the counters stay
// readable until the next open).
void MpNetStats_OnClose(u8 instance, const struct MpTransportStats *stats, u32 now);

// mp_session.c: once per MP_Update while the transport is open (selected instance): the
// overlay rates and, with --mp-net-stats, the periodic summary.
void MpNetStats_Update(u32 now);

// Selected instance.
void MpNetStats_GetTotals(struct MpNetTotals *out);
// Per message type, summed over peers (out has MP_NETSTATS_TYPES entries).
void MpNetStats_GetTypeTotals(struct MpNetTypeStats *out);
// Per message type for one peer; FALSE (out zeroed) if `peer` is out of range.
bool8 MpNetStats_GetPeerTypeTotals(u8 peer, struct MpNetTypeStats *out);

// Short name of a message type ("PRESENCE", "LINK_ROW", ...), "?" for unknown ones.
const char *MpNetStats_TypeName(u8 type);
bool8 MpNetStats_IsLogging(void);

#endif // GUARD_MP_MP_NETSTATS_H
