// Network traffic counters, the --mp-net-stats log and the F3 overlay data (PS-053).
// See include/mp/mp_netstats.h. Read only with respect to the session: nothing here sends,
// drops or changes a packet.

#include <string.h>
#include "mp/mp.h"
#include "mp/mp_internal.h"
#include "mp/mp_netstats.h"
#include "mp/mp_protocol.h"
#include "mp/mp_session_internal.h"
#include "mp/mp_transport.h"

#define MP_NETSTATS_TOP_TYPES 3

struct MpNetInstance
{
    bool8 open;
    u32 openMs;
    u32 closeMs;
    struct MpTransportStats wire;       // last ENet snapshot (MpNetStats_Update / close)
    struct MpNetTypeStats byType[MP_NETSTATS_PEERS][MP_NETSTATS_TYPES];
    // --mp-net-stats periodic summary
    u32 reportMs;
    struct MpNetTotals reportBase;
    u32 reportTypeBytes[MP_NETSTATS_TYPES];     // sent + received bytes at reportMs
    // F3 overlay: rates over the last MP_NETSTATS_RATE_MS
    bool8 rateValid;
    u32 rateMs;
    struct MpTransportStats rateBase;
    u32 upBitsPerSec;
    u32 downBitsPerSec;
};

static struct MpNetInstance sNet[MP_TRANSPORT_INSTANCE_COUNT];
static bool8 sLogging = FALSE;

static struct MpNetInstance *MpNetGet(u8 instance)
{
    return (instance < MP_TRANSPORT_INSTANCE_COUNT) ? &sNet[instance] : NULL;
}

static const char *MpNetTag(u8 instance)
{
    static const char *const sTags[] = { "", "[i1] ", "[i2] ", "[i3] ", "[i4] ", "[i5] ", "[i6] ", "[i7] " };

    return (instance < sizeof(sTags) / sizeof(sTags[0])) ? sTags[instance] : "[i?] ";
}

const char *MpNetStats_TypeName(u8 type)
{
    switch (type)
    {
    case MP_MSG_HELLO:              return "HELLO";
    case MP_MSG_WELCOME:            return "WELCOME";
    case MP_MSG_REJECT:             return "REJECT";
    case MP_MSG_PLAYER_JOINED:      return "PLAYER_JOINED";
    case MP_MSG_PLAYER_LEFT:        return "PLAYER_LEFT";
    case MP_MSG_PLAYER_UPDATE:      return "PLAYER_UPDATE";
    case MP_MSG_BYE:                return "BYE";
    case MP_MSG_PING:               return "PING";
    case MP_MSG_PONG:               return "PONG";
    case MP_MSG_PRESENCE:           return "PRESENCE";
    case MP_MSG_INTERACT_REQUEST:   return "INTERACT_REQUEST";
    case MP_MSG_INTERACT_RESPONSE:  return "INTERACT_RESPONSE";
    case MP_MSG_INTERACT_CANCEL:    return "INTERACT_CANCEL";
    case MP_MSG_TRAINER_CARD_DATA:  return "TRAINER_CARD_DATA";
    case MP_MSG_PARTY_DATA:         return "PARTY_DATA";
    case MP_MSG_LINK_JOIN:          return "LINK_JOIN";
    case MP_MSG_LINK_LEAVE:         return "LINK_LEAVE";
    case MP_MSG_LINK_GROUP:         return "LINK_GROUP";
    case MP_MSG_LINK_END:           return "LINK_END";
    case MP_MSG_LINK_OPEN:          return "LINK_OPEN";
    case MP_MSG_LINK_CLOSE:         return "LINK_CLOSE";
    case MP_MSG_LINK_START:         return "LINK_START";
    case MP_MSG_LINK_STATUS:        return "LINK_STATUS";
    case MP_MSG_LINK_BUS_START:     return "LINK_BUS_START";
    case MP_MSG_LINK_BUS_END:       return "LINK_BUS_END";
    case MP_MSG_LINK_CMD:           return "LINK_CMD";
    case MP_MSG_LINK_ROW:           return "LINK_ROW";
    case MP_MSG_TRADE_COMMIT:       return "TRADE_COMMIT";
    case MP_MSG_TRADE_RECOVER:      return "TRADE_RECOVER";
    case MP_MSG_LINK_DONE:          return "LINK_DONE";
    case MP_MSG_DEBUG_ECHO:         return "DEBUG_ECHO";
    }
    return "?";
}

bool8 MpNetStats_IsLogging(void)
{
    return sLogging;
}

void MP_SetNetStatsLogging(bool8 enabled)
{
    sLogging = enabled ? TRUE : FALSE;
    if (sLogging)
        MP_LOG("netstats: on (summary every %u s, full table when a session's transport closes)\n",
               (unsigned)(MP_NETSTATS_REPORT_MS / 1000));
}

// ---------------------------------------------------------------------------
// Counting (mp_transport.c)
// ---------------------------------------------------------------------------

void MpNetStats_OnOpen(u8 instance, u32 now)
{
    struct MpNetInstance *n = MpNetGet(instance);

    if (n == NULL)
        return;
    memset(n, 0, sizeof(*n));
    n->open = TRUE;
    n->openMs = now;
    n->reportMs = now;
}

void MpNetStats_OnSend(u8 instance, u8 peer, const u8 *data, u32 len)
{
    struct MpNetInstance *n = MpNetGet(instance);
    struct MpNetTypeStats *t;

    // data[0] is the header's type byte: only an index here, any value is in range.
    if (n == NULL || peer >= MP_NETSTATS_PEERS || data == NULL || len == 0)
        return;
    t = &n->byType[peer][data[0]];
    t->sentCount++;
    t->sentBytes += len;
}

void MpNetStats_OnRecv(u8 instance, u8 peer, const u8 *data, u32 len)
{
    struct MpNetInstance *n = MpNetGet(instance);
    struct MpNetTypeStats *t;

    // Unvalidated bytes from the network: only data[0] is read, as an array index (0..255).
    if (n == NULL || peer >= MP_NETSTATS_PEERS || data == NULL || len == 0)
        return;
    t = &n->byType[peer][data[0]];
    t->recvCount++;
    t->recvBytes += len;
}

// ---------------------------------------------------------------------------
// Sums
// ---------------------------------------------------------------------------

static void MpNetSumTypes(const struct MpNetInstance *n, struct MpNetTypeStats *out)
{
    u32 p, t;

    memset(out, 0, sizeof(*out) * MP_NETSTATS_TYPES);
    for (p = 0; p < MP_NETSTATS_PEERS; p++)
    {
        for (t = 0; t < MP_NETSTATS_TYPES; t++)
        {
            out[t].sentCount += n->byType[p][t].sentCount;
            out[t].sentBytes += n->byType[p][t].sentBytes;
            out[t].recvCount += n->byType[p][t].recvCount;
            out[t].recvBytes += n->byType[p][t].recvBytes;
        }
    }
}

static void MpNetTotalsOf(const struct MpNetInstance *n, const struct MpTransportStats *wire, u32 now, struct MpNetTotals *out)
{
    u32 p, t;

    memset(out, 0, sizeof(*out));
    for (p = 0; p < MP_NETSTATS_PEERS; p++)
    {
        for (t = 0; t < MP_NETSTATS_TYPES; t++)
        {
            out->msgSentCount += n->byType[p][t].sentCount;
            out->msgSentBytes += n->byType[p][t].sentBytes;
            out->msgRecvCount += n->byType[p][t].recvCount;
            out->msgRecvBytes += n->byType[p][t].recvBytes;
        }
    }
    out->wireSentBytes = wire->wireSentBytes;
    out->wireRecvBytes = wire->wireRecvBytes;
    out->wireSentDatagrams = wire->wireSentDatagrams;
    out->wireRecvDatagrams = wire->wireRecvDatagrams;
    out->elapsedMs = (n->openMs != 0 || n->open) ? (u32)((n->open ? now : n->closeMs) - n->openMs) : 0;
}

void MpNetStats_GetTotals(struct MpNetTotals *out)
{
    u8 instance = MpTransport_GetInstance();
    struct MpNetInstance *n = MpNetGet(instance);
    struct MpTransportStats wire;

    if (out == NULL)
        return;
    memset(out, 0, sizeof(*out));
    if (n == NULL)
        return;
    if (n->open)
        MpTransport_GetStats(&wire);
    else
        wire = n->wire;
    MpNetTotalsOf(n, &wire, MpTransport_GetTimeMs(), out);
}

void MpNetStats_GetTypeTotals(struct MpNetTypeStats *out)
{
    struct MpNetInstance *n = MpNetGet(MpTransport_GetInstance());

    if (out == NULL)
        return;
    if (n == NULL)
    {
        memset(out, 0, sizeof(*out) * MP_NETSTATS_TYPES);
        return;
    }
    MpNetSumTypes(n, out);
}

bool8 MpNetStats_GetPeerTypeTotals(u8 peer, struct MpNetTypeStats *out)
{
    struct MpNetInstance *n = MpNetGet(MpTransport_GetInstance());

    if (out == NULL)
        return FALSE;
    if (n == NULL || peer >= MP_NETSTATS_PEERS)
    {
        memset(out, 0, sizeof(*out) * MP_NETSTATS_TYPES);
        return FALSE;
    }
    memcpy(out, n->byType[peer], sizeof(n->byType[peer]));
    return TRUE;
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

static u32 MpPerSec(u32 amount, u32 ms)
{
    return (ms == 0) ? 0 : (u32)(((u64)amount * 1000) / ms);
}

// Bits per second on the wire: UDP payload plus the IPv4 + UDP header estimate.
static u32 MpWireBitsPerSec(u32 bytes, u32 datagrams, u32 ms)
{
    u64 total = (u64)bytes + (u64)datagrams * MP_NETSTATS_IP_UDP_OVERHEAD;

    return (ms == 0) ? 0 : (u32)((total * 8 * 1000) / ms);
}

// "12.3" for x / 10 (permille -> percent, or tenths)
#define MP_TENTHS(x) (unsigned)((x) / 10), (unsigned)((x) % 10)

static u32 MpPermille(u32 part, u32 whole)
{
    return (whole == 0) ? 0 : (u32)(((u64)part * 1000) / whole);
}

// The `count` biggest entries of bytes[] (sent + received), largest first.
static u8 MpTopTypes(const u32 *bytes, u8 *outTypes, u8 count)
{
    u8 found = 0;
    u32 i, k;

    for (k = 0; k < count; k++)
    {
        u32 best = 0;
        s32 bestType = -1;

        for (i = 0; i < MP_NETSTATS_TYPES; i++)
        {
            bool8 taken = FALSE;
            u32 j;

            for (j = 0; j < found; j++)
                taken = taken || (outTypes[j] == i);
            if (!taken && bytes[i] > best)
            {
                best = bytes[i];
                bestType = (s32)i;
            }
        }
        if (bestType < 0)
            break;
        outTypes[found++] = (u8)bestType;
    }
    return found;
}

static void MpLogPeers(u8 instance, const struct MpTransportStats *wire)
{
    u32 i;

    for (i = 0; i < MP_NETSTATS_PEERS; i++)
    {
        const struct MpTransportPeerStats *p = &wire->peers[i];

        if (!p->connected)
            continue;
        MP_LOG("%snetstats: peer %u: rtt %u +- %u ms, loss %u.%u%%\n", MpNetTag(instance), (unsigned)i,
               (unsigned)p->rttMs, (unsigned)p->rttVarMs, MP_TENTHS(p->lossPermille));
    }
}

static void MpLogSummary(u8 instance, struct MpNetInstance *n, u32 now)
{
    struct MpNetTotals cur;
    struct MpNetTypeStats types[MP_NETSTATS_TYPES];
    u32 bytes[MP_NETSTATS_TYPES];
    u8 top[MP_NETSTATS_TOP_TYPES];
    u32 ms = now - n->reportMs;
    u32 msgUp, msgDown, msgAll, i;
    u8 found;
    char line[160];
    u32 pos = 0;

    MpNetTotalsOf(n, &n->wire, now, &cur);
    MpNetSumTypes(n, types);
    for (i = 0; i < MP_NETSTATS_TYPES; i++)
    {
        u32 all = types[i].sentBytes + types[i].recvBytes;

        bytes[i] = all - n->reportTypeBytes[i];
        n->reportTypeBytes[i] = all;
    }
    msgUp = cur.msgSentBytes - n->reportBase.msgSentBytes;
    msgDown = cur.msgRecvBytes - n->reportBase.msgRecvBytes;
    msgAll = msgUp + msgDown;
    MP_LOG("%snetstats: last %u.%u s: up %u B/s msg (%u msg/s), %u B/s ENet (%u.%u kbit/s with IP/UDP); "
           "down %u B/s msg (%u msg/s), %u B/s ENet (%u.%u kbit/s with IP/UDP)\n",
           MpNetTag(instance), MP_TENTHS(ms / 100),
           (unsigned)MpPerSec(msgUp, ms), (unsigned)MpPerSec(cur.msgSentCount - n->reportBase.msgSentCount, ms),
           (unsigned)MpPerSec(cur.wireSentBytes - n->reportBase.wireSentBytes, ms),
           MP_TENTHS(MpWireBitsPerSec(cur.wireSentBytes - n->reportBase.wireSentBytes,
                                      cur.wireSentDatagrams - n->reportBase.wireSentDatagrams, ms) / 100),
           (unsigned)MpPerSec(msgDown, ms), (unsigned)MpPerSec(cur.msgRecvCount - n->reportBase.msgRecvCount, ms),
           (unsigned)MpPerSec(cur.wireRecvBytes - n->reportBase.wireRecvBytes, ms),
           MP_TENTHS(MpWireBitsPerSec(cur.wireRecvBytes - n->reportBase.wireRecvBytes,
                                      cur.wireRecvDatagrams - n->reportBase.wireRecvDatagrams, ms) / 100));
    found = MpTopTypes(bytes, top, MP_NETSTATS_TOP_TYPES);
    line[0] = '\0';
    for (i = 0; i < found && pos < sizeof(line); i++)
    {
        u32 pm = MpPermille(bytes[top[i]], msgAll);
        int w = snprintf(&line[pos], sizeof(line) - pos, "%s%s %u.%u%%", (i == 0) ? "" : ", ",
                         MpNetStats_TypeName(top[i]), MP_TENTHS(pm));

        if (w < 0)
            break;
        pos += (u32)w;
    }
    if (found != 0)
        MP_LOG("%snetstats: top message types by bytes: %s\n", MpNetTag(instance), line);
    MpLogPeers(instance, &n->wire);
    n->reportBase = cur;
    n->reportMs = now;
}

static void MpLogTable(u8 instance, struct MpNetInstance *n, u32 now)
{
    struct MpNetTotals tot;
    struct MpNetTypeStats types[MP_NETSTATS_TYPES];
    u32 i, p, msgAll;

    MpNetTotalsOf(n, &n->wire, now, &tot);
    MpNetSumTypes(n, types);
    msgAll = tot.msgSentBytes + tot.msgRecvBytes;
    MP_LOG("%snetstats: === session traffic, %u.%u s ===\n", MpNetTag(instance), MP_TENTHS(tot.elapsedMs / 100));
    MP_LOG("%snetstats: %-4s %-18s %8s %10s %8s %10s %6s\n", MpNetTag(instance),
           "type", "name", "sent", "sent B", "recv", "recv B", "share");
    for (i = 0; i < MP_NETSTATS_TYPES; i++)
    {
        const struct MpNetTypeStats *t = &types[i];

        if (t->sentCount == 0 && t->recvCount == 0)
            continue;
        MP_LOG("%snetstats: 0x%02X %-18s %8u %10u %8u %10u %4u.%u%%\n", MpNetTag(instance), (unsigned)i,
               MpNetStats_TypeName((u8)i), (unsigned)t->sentCount, (unsigned)t->sentBytes,
               (unsigned)t->recvCount, (unsigned)t->recvBytes,
               MP_TENTHS(MpPermille(t->sentBytes + t->recvBytes, msgAll)));
    }
    MP_LOG("%snetstats: %-23s %8u %10u %8u %10u\n", MpNetTag(instance), "messages",
           (unsigned)tot.msgSentCount, (unsigned)tot.msgSentBytes, (unsigned)tot.msgRecvCount, (unsigned)tot.msgRecvBytes);
    MP_LOG("%snetstats: %-23s %8u %10u %8u %10u  (UDP payload datagrams / bytes)\n", MpNetTag(instance), "ENet wire",
           (unsigned)tot.wireSentDatagrams, (unsigned)tot.wireSentBytes, (unsigned)tot.wireRecvDatagrams, (unsigned)tot.wireRecvBytes);
    MP_LOG("%snetstats: average up %u B/s msg, %u B/s ENet, %u.%u kbit/s with IP/UDP; down %u B/s msg, %u B/s ENet, %u.%u kbit/s with IP/UDP\n",
           MpNetTag(instance),
           (unsigned)MpPerSec(tot.msgSentBytes, tot.elapsedMs), (unsigned)MpPerSec(tot.wireSentBytes, tot.elapsedMs),
           MP_TENTHS(MpWireBitsPerSec(tot.wireSentBytes, tot.wireSentDatagrams, tot.elapsedMs) / 100),
           (unsigned)MpPerSec(tot.msgRecvBytes, tot.elapsedMs), (unsigned)MpPerSec(tot.wireRecvBytes, tot.elapsedMs),
           MP_TENTHS(MpWireBitsPerSec(tot.wireRecvBytes, tot.wireRecvDatagrams, tot.elapsedMs) / 100));
    for (p = 0; p < MP_NETSTATS_PEERS; p++)
    {
        u32 sc = 0, sb = 0, rc = 0, rb = 0;

        for (i = 0; i < MP_NETSTATS_TYPES; i++)
        {
            sc += n->byType[p][i].sentCount;
            sb += n->byType[p][i].sentBytes;
            rc += n->byType[p][i].recvCount;
            rb += n->byType[p][i].recvBytes;
        }
        if (sc == 0 && rc == 0)
            continue;
        MP_LOG("%snetstats: peer %u: sent %u msg / %u B, received %u msg / %u B\n", MpNetTag(instance),
               (unsigned)p, (unsigned)sc, (unsigned)sb, (unsigned)rc, (unsigned)rb);
    }
    MpLogPeers(instance, &n->wire);
}

void MpNetStats_OnClose(u8 instance, const struct MpTransportStats *stats, u32 now)
{
    struct MpNetInstance *n = MpNetGet(instance);

    if (n == NULL || !n->open)
        return;
    if (stats != NULL)
        n->wire = *stats;
    n->open = FALSE;
    n->closeMs = now;
    if (sLogging)
        MpLogTable(instance, n, now);
}

// ---------------------------------------------------------------------------
// Per frame (mp_session.c)
// ---------------------------------------------------------------------------

void MpNetStats_Update(u32 now)
{
    u8 instance = MpTransport_GetInstance();
    struct MpNetInstance *n = MpNetGet(instance);
    u32 ms;

    if (n == NULL || !n->open)
        return;
    MpTransport_GetStats(&n->wire);

    if (!n->rateValid)
    {
        n->rateValid = TRUE;
        n->rateMs = now;
        n->rateBase = n->wire;
    }
    ms = now - n->rateMs;
    if (ms >= MP_NETSTATS_RATE_MS)
    {
        n->upBitsPerSec = MpWireBitsPerSec(n->wire.wireSentBytes - n->rateBase.wireSentBytes,
                                           n->wire.wireSentDatagrams - n->rateBase.wireSentDatagrams, ms);
        n->downBitsPerSec = MpWireBitsPerSec(n->wire.wireRecvBytes - n->rateBase.wireRecvBytes,
                                             n->wire.wireRecvDatagrams - n->rateBase.wireRecvDatagrams, ms);
        n->rateBase = n->wire;
        n->rateMs = now;
    }

    if (sLogging && (u32)(now - n->reportMs) >= MP_NETSTATS_REPORT_MS)
        MpLogSummary(instance, n, now);
}

void MP_GetNetOverlay(struct MpNetOverlay *out)
{
    const struct MpNetInstance *n = &sNet[0];
    u32 i;

    if (out == NULL)
        return;
    memset(out, 0, sizeof(*out));
    out->open = n->open;
    out->inSession = (MP_GetState() == MP_STATE_IN_SESSION);
    out->isHost = MP_IsHost();
    out->players = MP_GetPlayerCount();
    if (!n->open)
        return;
    out->upBitsPerSec = n->upBitsPerSec;
    out->downBitsPerSec = n->downBitsPerSec;
    for (i = 0; i < MP_NETSTATS_PEERS; i++)
    {
        const struct MpTransportPeerStats *p = &n->wire.peers[i];

        if (!p->connected)
            continue;
        if (p->rttMs >= out->rttMs)
        {
            out->rttMs = p->rttMs;
            out->rttVarMs = p->rttVarMs;
        }
        if (p->lossPermille > out->lossPermille)
            out->lossPermille = p->lossPermille;
    }
}
