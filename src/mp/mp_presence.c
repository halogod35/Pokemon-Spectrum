// Presence (PS-030, protocol.md 6.8, design.md 3.3): local presence storage and
// sanitising, send-on-change at most 20 Hz plus the 1 s keep-alive, the host relay
// and cache, the client receive path with the newer-than check, the per-slot FIFO,
// MP_IsValidMap, and the --mp-debug bytes/s report.
//
// No game-state access here (the capture hook is mp_presence_capture.c), so a headless
// dedicated server can reuse it (AD-2).

#include <string.h>
#include "mp/mp.h"
#include "mp/mp_internal.h"
#include "mp/mp_protocol.h"
#include "mp/mp_session_internal.h"
#include "mp/mp_transport.h"
#include "constants/maps.h" // generated map ids (map_groups.h); enums only

#define MP_HOST_PEER_ID 0   // a client's only transport peer

static u8 sPresenceBuf[MP_MAX_PACKET_SIZE];

// Local presence survives MpBeginSession (which clears the session), so whatever the
// capture hook stored before MP_Host / MP_Join is sent the moment the session starts.
static struct MpPresence sLocalPresence[MP_SESSION_INSTANCE_COUNT];
static bool8 sHasLocalPresence[MP_SESSION_INSTANCE_COUNT];

// ---------------------------------------------------------------------------
// Map table (protocol.md 6.8 map rule)
// ---------------------------------------------------------------------------

// The last map of every group, in data/maps/map_groups.json group order. Each entry is
// checked at compile time to belong to its group, and the table to have exactly
// MAP_GROUPS_COUNT entries, so a map added at the end of a group is the only edit that
// can go unnoticed (it would only make that one new map "invalid" on the wire).
#define MP_GROUP_SIZE(lastMap) (MAP_NUM(lastMap) + 1)
#define MP_CHECK_GROUP(index, lastMap) \
    typedef char sMpMapGroupCheck##index[(MAP_GROUP(lastMap) == (index)) ? 1 : -1]

static const u8 sMapGroupSizes[] =
{
    MP_GROUP_SIZE(MAP_UNDERWATER_ROUTE125),
    MP_GROUP_SIZE(MAP_LITTLEROOT_TOWN_PROFESSOR_BIRCHS_LAB),
    MP_GROUP_SIZE(MAP_OLDALE_TOWN_MART),
    MP_GROUP_SIZE(MAP_DEWFORD_TOWN_HOUSE2),
    MP_GROUP_SIZE(MAP_LAVARIDGE_TOWN_POKEMON_CENTER_2F),
    MP_GROUP_SIZE(MAP_FALLARBOR_TOWN_MOVE_RELEARNERS_HOUSE),
    MP_GROUP_SIZE(MAP_VERDANTURF_TOWN_HOUSE),
    MP_GROUP_SIZE(MAP_PACIFIDLOG_TOWN_HOUSE5),
    MP_GROUP_SIZE(MAP_PETALBURG_CITY_MART),
    MP_GROUP_SIZE(MAP_SLATEPORT_CITY_MART),
    MP_GROUP_SIZE(MAP_MAUVILLE_CITY_MART),
    MP_GROUP_SIZE(MAP_RUSTBORO_CITY_HOUSE3),
    MP_GROUP_SIZE(MAP_FORTREE_CITY_DECORATION_SHOP),
    MP_GROUP_SIZE(MAP_LILYCOVE_CITY_DEPARTMENT_STORE_ELEVATOR),
    MP_GROUP_SIZE(MAP_MOSSDEEP_CITY_GAME_CORNER_B1F),
    MP_GROUP_SIZE(MAP_SOOTOPOLIS_CITY_MYSTERY_EVENTS_HOUSE_B1F),
    MP_GROUP_SIZE(MAP_EVER_GRANDE_CITY_POKEMON_LEAGUE_2F),
    MP_GROUP_SIZE(MAP_ROUTE104_PRETTY_PETAL_FLOWER_SHOP),
    MP_GROUP_SIZE(MAP_ROUTE111_OLD_LADYS_REST_STOP),
    MP_GROUP_SIZE(MAP_MT_CHIMNEY_CABLE_CAR_STATION),
    MP_GROUP_SIZE(MAP_ROUTE114_LANETTES_HOUSE),
    MP_GROUP_SIZE(MAP_ROUTE116_TUNNELERS_REST_HOUSE),
    MP_GROUP_SIZE(MAP_ROUTE117_POKEMON_DAY_CARE),
    MP_GROUP_SIZE(MAP_ROUTE121_SAFARI_ZONE_ENTRANCE),
    MP_GROUP_SIZE(MAP_METEOR_FALLS_STEVENS_CAVE),
    MP_GROUP_SIZE(MAP_UNION_ROOM),
    MP_GROUP_SIZE(MAP_TRAINER_HILL_ELEVATOR),
    MP_GROUP_SIZE(MAP_ROUTE104_PROTOTYPE_PRETTY_PETAL_FLOWER_SHOP),
    MP_GROUP_SIZE(MAP_ROUTE109_SEASHORE_HOUSE),
    MP_GROUP_SIZE(MAP_ROUTE110_SEASIDE_CYCLING_ROAD_NORTH_ENTRANCE),
    MP_GROUP_SIZE(MAP_ROUTE113_GLASS_WORKSHOP),
    MP_GROUP_SIZE(MAP_ROUTE123_BERRY_MASTERS_HOUSE),
    MP_GROUP_SIZE(MAP_ROUTE119_HOUSE),
    MP_GROUP_SIZE(MAP_ROUTE124_DIVING_TREASURE_HUNTERS_HOUSE),
    MP_GROUP_SIZE(MAP_TEMPLATE_ROUTE), // MP: PS-071 gMapGroup_TemplateRegion (placeholder region)
};

typedef char sMpMapGroupCountCheck[(sizeof(sMapGroupSizes) == MAP_GROUPS_COUNT) ? 1 : -1];
MP_CHECK_GROUP(0, MAP_UNDERWATER_ROUTE125);
MP_CHECK_GROUP(1, MAP_LITTLEROOT_TOWN_PROFESSOR_BIRCHS_LAB);
MP_CHECK_GROUP(2, MAP_OLDALE_TOWN_MART);
MP_CHECK_GROUP(3, MAP_DEWFORD_TOWN_HOUSE2);
MP_CHECK_GROUP(4, MAP_LAVARIDGE_TOWN_POKEMON_CENTER_2F);
MP_CHECK_GROUP(5, MAP_FALLARBOR_TOWN_MOVE_RELEARNERS_HOUSE);
MP_CHECK_GROUP(6, MAP_VERDANTURF_TOWN_HOUSE);
MP_CHECK_GROUP(7, MAP_PACIFIDLOG_TOWN_HOUSE5);
MP_CHECK_GROUP(8, MAP_PETALBURG_CITY_MART);
MP_CHECK_GROUP(9, MAP_SLATEPORT_CITY_MART);
MP_CHECK_GROUP(10, MAP_MAUVILLE_CITY_MART);
MP_CHECK_GROUP(11, MAP_RUSTBORO_CITY_HOUSE3);
MP_CHECK_GROUP(12, MAP_FORTREE_CITY_DECORATION_SHOP);
MP_CHECK_GROUP(13, MAP_LILYCOVE_CITY_DEPARTMENT_STORE_ELEVATOR);
MP_CHECK_GROUP(14, MAP_MOSSDEEP_CITY_GAME_CORNER_B1F);
MP_CHECK_GROUP(15, MAP_SOOTOPOLIS_CITY_MYSTERY_EVENTS_HOUSE_B1F);
MP_CHECK_GROUP(16, MAP_EVER_GRANDE_CITY_POKEMON_LEAGUE_2F);
MP_CHECK_GROUP(17, MAP_ROUTE104_PRETTY_PETAL_FLOWER_SHOP);
MP_CHECK_GROUP(18, MAP_ROUTE111_OLD_LADYS_REST_STOP);
MP_CHECK_GROUP(19, MAP_MT_CHIMNEY_CABLE_CAR_STATION);
MP_CHECK_GROUP(20, MAP_ROUTE114_LANETTES_HOUSE);
MP_CHECK_GROUP(21, MAP_ROUTE116_TUNNELERS_REST_HOUSE);
MP_CHECK_GROUP(22, MAP_ROUTE117_POKEMON_DAY_CARE);
MP_CHECK_GROUP(23, MAP_ROUTE121_SAFARI_ZONE_ENTRANCE);
MP_CHECK_GROUP(24, MAP_METEOR_FALLS_STEVENS_CAVE);
MP_CHECK_GROUP(25, MAP_UNION_ROOM);
MP_CHECK_GROUP(26, MAP_TRAINER_HILL_ELEVATOR);
MP_CHECK_GROUP(27, MAP_ROUTE104_PROTOTYPE_PRETTY_PETAL_FLOWER_SHOP);
MP_CHECK_GROUP(28, MAP_ROUTE109_SEASHORE_HOUSE);
MP_CHECK_GROUP(29, MAP_ROUTE110_SEASIDE_CYCLING_ROAD_NORTH_ENTRANCE);
MP_CHECK_GROUP(30, MAP_ROUTE113_GLASS_WORKSHOP);
MP_CHECK_GROUP(31, MAP_ROUTE123_BERRY_MASTERS_HOUSE);
MP_CHECK_GROUP(32, MAP_ROUTE119_HOUSE);
MP_CHECK_GROUP(33, MAP_ROUTE124_DIVING_TREASURE_HUNTERS_HOUSE);
MP_CHECK_GROUP(34, MAP_TEMPLATE_ROUTE); // MP: PS-071

bool8 MP_IsValidMap(u8 mapGroup, u8 mapNum)
{
    if (mapGroup >= MAP_GROUPS_COUNT || mapGroup >= sizeof(sMapGroupSizes))
        return FALSE;
    return mapNum < sMapGroupSizes[mapGroup];
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void MpDefaultPresence(struct MpPresence *p)
{
    memset(p, 0, sizeof(*p));
    p->mapGroup = MP_MAP_NONE;
    p->mapNum = MP_MAP_NONE;
    p->facing = DIR_SOUTH;
    p->movement = MP_MOVE_NONE;
    p->avatarState = MP_AVATAR_ON_FOOT;
    p->busy = MP_BUSY_NOT_IN_FIELD;
}

static s16 MpClampCoord(s16 v)
{
    if (v < MP_PRESENCE_COORD_MIN)
        return MP_PRESENCE_COORD_MIN;
    if (v > MP_PRESENCE_COORD_MAX)
        return MP_PRESENCE_COORD_MAX;
    return v;
}

// Makes `p` pass MpProto_ParsePresence (the sender never sends what a receiver rejects).
static void MpSanitizePresence(struct MpPresence *p)
{
    p->presenceFlags = 0;
    if (p->avatarState >= MP_AVATAR_STATE_COUNT)
        p->avatarState = MP_AVATAR_ON_FOOT;
    if (p->busy >= MP_BUSY_COUNT)
        p->busy = MP_BUSY_NONE;
    if (!MP_IsValidMap(p->mapGroup, p->mapNum))
    {
        p->mapGroup = MP_MAP_NONE;
        p->mapNum = MP_MAP_NONE;
        p->busy = MP_BUSY_NOT_IN_FIELD;
    }
    if (p->mapGroup == MP_MAP_NONE)
    {
        p->x = 0;
        p->y = 0;
        p->facing = DIR_SOUTH;
        p->movement = MP_MOVE_NONE;
        return;
    }
    p->x = MpClampCoord(p->x);
    p->y = MpClampCoord(p->y);
    if (p->facing < DIR_SOUTH || p->facing > DIR_EAST)
        p->facing = DIR_SOUTH;
    if (p->movement >= MP_MOVE_COUNT)
        p->movement = MP_MOVE_NONE;
}

// Everything but seq.
static bool8 MpPresenceEqual(const struct MpPresence *a, const struct MpPresence *b)
{
    return a->mapGroup == b->mapGroup && a->mapNum == b->mapNum
        && a->x == b->x && a->y == b->y
        && a->facing == b->facing && a->movement == b->movement
        && a->avatarState == b->avatarState && a->busy == b->busy
        && a->stepId == b->stepId && a->presenceFlags == b->presenceFlags;
}

static const struct MpPresence *MpLocalPresence(void)
{
    static struct MpPresence sDefault;
    u8 instance = gMpSession->instance;

    if (instance < MP_SESSION_INSTANCE_COUNT && sHasLocalPresence[instance])
        return &sLocalPresence[instance];
    MpDefaultPresence(&sDefault);
    return &sDefault;
}

static const char *MpMoveName(u8 movement)
{
    static const char *const sNames[MP_MOVE_COUNT] =
    {
        "NONE", "TURN", "WALK", "WALK_FAST", "WALK_FASTER", "JUMP", "JUMP_IN_PLACE", "WARP",
    };

    return movement < MP_MOVE_COUNT ? sNames[movement] : "?";
}

static const char *MpBusyName(u8 busy)
{
    static const char *const sNames[MP_BUSY_COUNT] =
    {
        "NONE", "BATTLE", "MENU", "SCRIPT", "WARPING", "NOT_IN_FIELD",
    };

    return busy < MP_BUSY_COUNT ? sNames[busy] : "?";
}

static void MpLogPresence(const char *what, u8 slot, const struct MpPresence *p)
{
    MP_SDLOG("presence %s slot %u seq %u map %u.%u (%d,%d) facing %u %s state %u busy %s step %u\n",
             what, (unsigned)slot, (unsigned)p->seq, (unsigned)p->mapGroup, (unsigned)p->mapNum,
             (int)p->x, (int)p->y, (unsigned)p->facing, MpMoveName(p->movement),
             (unsigned)p->avatarState, MpBusyName(p->busy), (unsigned)p->stepId);
}

static void MpCountOut(u8 peer, u16 len)
{
    if (peer < MP_HOST_PEER_COUNT)
    {
        gMpSession->presence.traffic[peer].bytesOut += len;
        gMpSession->presence.traffic[peer].packetsOut++;
    }
}

static void MpCountIn(u8 peer, u32 len)
{
    if (peer < MP_HOST_PEER_COUNT)
    {
        gMpSession->presence.traffic[peer].bytesIn += len + MP_HEADER_SIZE;
        gMpSession->presence.traffic[peer].packetsIn++;
    }
}

static void MpSendPresenceTo(u8 peer, const u8 *data, u16 len)
{
    if (MpSession_SendTo(peer, data, len, MP_CHANNEL_PRESENCE, FALSE))
        MpCountOut(peer, len);
}

// Host: to every player peer except `exceptPeer`.
static void MpBroadcastPresence(const u8 *data, u16 len, u8 exceptPeer)
{
    u32 i;

    for (i = 0; i < MP_HOST_PEER_COUNT; i++)
    {
        if (i != exceptPeer && gMpSession->peers[i].state == MP_HOST_PEER_PLAYER)
            MpSendPresenceTo((u8)i, data, len);
    }
}

// Stores an accepted remote update: latest value + FIFO (drop oldest on overflow).
static void MpStoreRemote(u8 slot, const struct MpPresence *p)
{
    struct MpSession *s = gMpSession;
    struct MpPresenceSlot *q = &s->presence.slots[slot];

    q->seqValid = TRUE;
    q->lastSeq = p->seq;
    s->players[slot].presence = *p;
    s->players[slot].hasPresence = TRUE;
    if (q->count >= MP_PRESENCE_QUEUE_SIZE)
    {
        q->head = (u8)((q->head + 1) % MP_PRESENCE_QUEUE_SIZE);
        q->count--;
        s->players[slot].presenceOverflow = 1;
        MP_SDLOG("presence queue of slot %u overflowed (consumer should snap)\n", (unsigned)slot);
    }
    q->queue[(q->head + q->count) % MP_PRESENCE_QUEUE_SIZE] = *p;
    q->count++;
    s->presence.accepted++;
}

// Newer-than check per slot (protocol.md 6.8). TRUE if `seq` may be accepted.
static bool8 MpSeqAcceptable(u8 slot, u16 seq)
{
    const struct MpPresenceSlot *q = &gMpSession->presence.slots[slot];

    return !q->seqValid || MpSeqIsNewer(seq, q->lastSeq);
}

// ---------------------------------------------------------------------------
// Local presence and sending
// ---------------------------------------------------------------------------

void MpPresence_ResetLocal(u8 instance)
{
    if (instance >= MP_SESSION_INSTANCE_COUNT)
        return;
    MpDefaultPresence(&sLocalPresence[instance]);
    sHasLocalPresence[instance] = FALSE;
}

void MP_SetLocalPresence(const struct MpPresence *presence)
{
    struct MpSession *s = gMpSession;
    struct MpPresence p;
    u8 instance = s->instance;

    if (presence == NULL || instance >= MP_SESSION_INSTANCE_COUNT)
        return;
    p = *presence;
    p.seq = 0;
    MpSanitizePresence(&p);
    sLocalPresence[instance] = p;
    sHasLocalPresence[instance] = TRUE;
    if (s->state == MP_STATE_IN_SESSION && s->localSlot < MP_MAX_PLAYERS && s->slotUsed[s->localSlot])
    {
        p.seq = s->presence.lastSent.seq;
        s->players[s->localSlot].presence = p;
        s->players[s->localSlot].hasPresence = TRUE;
    }
}

void MpPresence_Update(u32 now)
{
    struct MpSession *s = gMpSession;
    struct MpPresenceState *ps = &s->presence;
    const struct MpPresence *local = MpLocalPresence();
    struct MpMsgPresence msg;
    u32 since;
    u16 len;

    if (s->state != MP_STATE_IN_SESSION || s->localSlot >= MP_MAX_PLAYERS || !s->slotUsed[s->localSlot])
        return;
    since = now - ps->lastSentMs;
    if (ps->sentAny)
    {
        if (since < MP_PRESENCE_MIN_GAP_MS)
            return;
        if (MpPresenceEqual(local, &ps->lastSent) && since < MP_PRESENCE_KEEPALIVE_MS)
            return;
    }

    memset(&msg, 0, sizeof(msg));
    msg.slot = s->localSlot;
    msg.epoch = s->players[s->localSlot].epoch;
    msg.presence = *local;
    msg.presence.seq = ps->nextSeq;
    len = MpProto_WritePresence(sPresenceBuf, sizeof(sPresenceBuf), &msg);
    if (len == 0)
        return;
    if (s->isHost)
        MpBroadcastPresence(sPresenceBuf, len, MP_PEER_NONE);
    else
        MpSendPresenceTo(MP_HOST_PEER_ID, sPresenceBuf, len);

    if (!ps->sentAny || !MpPresenceEqual(local, &ps->lastSent))
        MpLogPresence("sent", msg.slot, &msg.presence);
    ps->sentAny = TRUE;
    ps->lastSentMs = now;
    ps->lastSent = msg.presence;
    ps->nextSeq++;
    ps->sent++;
    s->players[s->localSlot].presence = msg.presence;
    s->players[s->localSlot].hasPresence = TRUE;
}

// ---------------------------------------------------------------------------
// Receiving
// ---------------------------------------------------------------------------

u8 MpPresence_OnHostReceive(u8 peer, const u8 *payload, u32 len, u32 now)
{
    struct MpSession *s = gMpSession;
    struct MpHostPeer *hp;
    struct MpMsgPresence msg;
    u8 copy[MP_PRESENCE_SIZE];
    u16 outLen;
    u8 slot;

    if (peer >= MP_HOST_PEER_COUNT)
        return MP_PRESENCE_IGNORED;
    hp = &s->peers[peer];
    if (hp->state != MP_HOST_PEER_PLAYER || hp->slot >= MP_MAX_PLAYERS || !s->slotUsed[hp->slot])
        return MP_PRESENCE_IGNORED;
    slot = hp->slot;
    MpCountIn(peer, len);
    if (len != MP_PRESENCE_SIZE || payload == NULL)
    {
        s->presence.invalid++;
        return MP_PRESENCE_INVALID;
    }
    if (hp->lastPresenceRecvMs != 0 && (u32)(now - hp->lastPresenceRecvMs) < MP_PRESENCE_HOST_GAP_MS)
    {
        s->presence.ignored++;
        MP_SDLOG("peer %u: PRESENCE rate-limited\n", (unsigned)peer);
        return MP_PRESENCE_IGNORED;
    }
    // The host is authoritative for slot and epoch: overwrite them before parsing.
    memcpy(copy, payload, sizeof(copy));
    copy[0] = slot;
    copy[1] = s->players[slot].epoch;
    if (MpProto_ParsePresence(copy, sizeof(copy), &msg) != MP_PARSE_OK)
    {
        s->presence.invalid++;
        return MP_PRESENCE_INVALID;
    }
    if (!MpSeqAcceptable(slot, msg.presence.seq))
    {
        s->presence.stale++;
        MP_SDLOG("slot %u: stale PRESENCE seq %u (last %u) dropped\n", (unsigned)slot,
                 (unsigned)msg.presence.seq, (unsigned)s->presence.slots[slot].lastSeq);
        return MP_PRESENCE_STALE;
    }
    hp->lastPresenceRecvMs = (now != 0) ? now : 1;
    if (!s->players[slot].hasPresence || !MpPresenceEqual(&msg.presence, &s->players[slot].presence))
        MpLogPresence("recv", slot, &msg.presence);
    MpStoreRemote(slot, &msg.presence);

    // Relay unchanged (same seq) to every other client.
    outLen = MpProto_WritePresence(sPresenceBuf, sizeof(sPresenceBuf), &msg);
    if (outLen != 0)
        MpBroadcastPresence(sPresenceBuf, outLen, peer);
    return MP_PRESENCE_ACCEPTED;
}

u8 MpPresence_OnClientReceive(const u8 *payload, u32 len)
{
    struct MpSession *s = gMpSession;
    struct MpMsgPresence msg;

    MpCountIn(MP_HOST_PEER_ID, len);
    if (MpProto_ParsePresence(payload, len, &msg) != MP_PARSE_OK)
    {
        s->presence.invalid++;
        return MP_PRESENCE_INVALID;
    }
    // Expected races (slot just freed / reused, our own echo): silent drop.
    if (!s->slotUsed[msg.slot] || msg.slot == s->localSlot || s->players[msg.slot].epoch != msg.epoch)
    {
        s->presence.ignored++;
        return MP_PRESENCE_IGNORED;
    }
    if (!MpSeqAcceptable(msg.slot, msg.presence.seq))
    {
        s->presence.stale++;
        MP_SDLOG("slot %u: stale PRESENCE seq %u (last %u) dropped\n", (unsigned)msg.slot,
                 (unsigned)msg.presence.seq, (unsigned)s->presence.slots[msg.slot].lastSeq);
        return MP_PRESENCE_STALE;
    }
    if (!s->players[msg.slot].hasPresence || !MpPresenceEqual(&msg.presence, &s->players[msg.slot].presence))
        MpLogPresence("recv", msg.slot, &msg.presence);
    MpStoreRemote(msg.slot, &msg.presence);
    return MP_PRESENCE_ACCEPTED;
}

void MpPresence_SendCachedTo(u8 peer, u8 newSlot)
{
    struct MpSession *s = gMpSession;
    struct MpMsgPresence msg;
    u16 len;
    u32 i;

    for (i = 0; i < MP_MAX_PLAYERS; i++)
    {
        if (i == newSlot || !s->slotUsed[i])
            continue;
        memset(&msg, 0, sizeof(msg));
        msg.slot = (u8)i;
        msg.epoch = s->players[i].epoch;
        if (i == s->localSlot)
        {
            if (!s->presence.sentAny)
                continue; // nothing sent yet: the first send reaches the new client too
            msg.presence = s->presence.lastSent;
        }
        else
        {
            if (!s->players[i].hasPresence)
                continue;
            msg.presence = s->players[i].presence;
        }
        len = MpProto_WritePresence(sPresenceBuf, sizeof(sPresenceBuf), &msg);
        if (len != 0)
            MpSendPresenceTo(peer, sPresenceBuf, len);
    }
}

void MpPresence_ResetSlot(u8 slot)
{
    if (slot >= MP_MAX_PLAYERS)
        return;
    memset(&gMpSession->presence.slots[slot], 0, sizeof(gMpSession->presence.slots[slot]));
}

// ---------------------------------------------------------------------------
// Consumer API (PS-031)
// ---------------------------------------------------------------------------

bool8 MP_PopRemotePresence(u8 slot, struct MpPresence *out)
{
    struct MpSession *s = gMpSession;
    struct MpPresenceSlot *q;

    if (out != NULL)
        memset(out, 0, sizeof(*out));
    if (out == NULL || s->state != MP_STATE_IN_SESSION || slot >= MP_MAX_PLAYERS
     || !s->slotUsed[slot] || slot == s->localSlot)
        return FALSE;
    q = &s->presence.slots[slot];
    if (q->count == 0)
        return FALSE;
    *out = q->queue[q->head];
    q->head = (u8)((q->head + 1) % MP_PRESENCE_QUEUE_SIZE);
    q->count--;
    return TRUE;
}

void MP_ClearPresenceOverflow(u8 slot)
{
    if (slot < MP_MAX_PLAYERS)
        gMpSession->players[slot].presenceOverflow = 0;
}

// ---------------------------------------------------------------------------
// --mp-debug report
// ---------------------------------------------------------------------------

void MpPresence_DebugReport(u32 now)
{
    struct MpSession *s = gMpSession;
    struct MpPresenceState *ps = &s->presence;
    u32 elapsed, i, peerCount;

    if (ps->trafficStartMs == 0)
    {
        ps->trafficStartMs = (now != 0) ? now : 1;
        return;
    }
    elapsed = now - ps->trafficStartMs;
    if (elapsed < MP_PRESENCE_STATS_MS)
        return;
    if (gMpDebugLogging)
    {
        peerCount = s->isHost ? MP_HOST_PEER_COUNT : 1;
        for (i = 0; i < peerCount; i++)
        {
            const struct MpPresenceTraffic *t = &ps->traffic[i];
            u8 slot = s->isHost ? s->peers[i].slot : MP_HOST_SLOT;

            if (s->isHost && s->peers[i].state != MP_HOST_PEER_PLAYER)
                continue;
            MP_SLOG("presence peer %u (slot %u): out %u B/s %u pkt/s (~%u B/s on the wire), in %u B/s %u pkt/s (~%u B/s on the wire)\n",
                    (unsigned)i, (unsigned)slot,
                    (unsigned)(t->bytesOut * 1000 / elapsed), (unsigned)(t->packetsOut * 1000 / elapsed),
                    (unsigned)((t->bytesOut + t->packetsOut * MP_PRESENCE_WIRE_OVERHEAD) * 1000 / elapsed),
                    (unsigned)(t->bytesIn * 1000 / elapsed), (unsigned)(t->packetsIn * 1000 / elapsed),
                    (unsigned)((t->bytesIn + t->packetsIn * MP_PRESENCE_WIRE_OVERHEAD) * 1000 / elapsed));
        }
        MP_SLOG("presence totals: sent %u, accepted %u, stale %u, ignored %u, invalid %u\n",
                (unsigned)ps->sent, (unsigned)ps->accepted, (unsigned)ps->stale,
                (unsigned)ps->ignored, (unsigned)ps->invalid);
    }
    memset(ps->traffic, 0, sizeof(ps->traffic));
    ps->trafficStartMs = (now != 0) ? now : 1;
}
