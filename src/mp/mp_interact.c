// Player interaction requests (PS-041, protocol.md 6.11): TRADE / BATTLE requests through the
// host. Session layer only: no game state, no UI (the menus are src/mp/mp_menu_interact.c).
//
// Roles
//  - Every instance is an "endpoint": it may have one OUTGOING request (we asked someone) and
//    one INCOMING request (someone asked us), in gMpSession->interact.
//  - The host additionally ROUTES every request, its own included: routes[] is indexed by the
//    requester's slot, so a player can have only one request in flight. The host checks the
//    target (in the table, current epoch, on the requester's map, not already asked or asking),
//    rate-limits requests (MP_INTERACT_MIN_GAP_MS per requester), forwards, and times out
//    requests the target never answers (MP_INTERACT_TIMEOUT_MS + MP_INTERACT_HOST_GRACE_MS).
//  - The target's own session answers TIMEOUT after MP_INTERACT_TIMEOUT_MS, so normally the
//    requester hears TIMEOUT from the target; the host timer is the backstop for a target that
//    vanished silently, and the requester's own timer (+ MP_INTERACT_CLIENT_GRACE_MS) the
//    backstop for a host that did.
//  - Anything stale (unknown request id, old epoch, slot gone) is dropped silently. Routing
//    refusals and rate limits are answered (UNAVAILABLE / BUSY) and are not "malformed"; only
//    messages that fail to parse count towards the malformed budget (protocol.md 8.2).
//
// PS-042 (protocol.md 6.13): kind TRAINER_CARD. The target's session answers by itself, with
// no prompt: TRAINER_CARD_DATA (the card from gMpLocalTrainerCardFunc), then RESPONSE
// ACCEPTED, both on the reliable ordered channel, so the data always arrives first. The host
// forwards the data only along a live route (sender = the route's target, one data message
// per TRAINER_CARD route) and re-serialises it from the validated parse. The requester keeps
// it in gMpSession->interact.rx; the RESPONSE then completes it (ACCEPTED) or turns into the
// local result BAD_DATA if the data was missing or invalid. TRAINER_CARD requests do not
// make the target "busy" for trade / battle requests and are not refused when it is.
//
// PS-043 (protocol.md 6.14): kind VIEW_PARTY is prompted like a trade. When the target answers
// ACCEPTED, its session sends the party as 1..2 PARTY_DATA parts (up to 3 raw 100-byte mons
// each; a whole party does not fit in one 512-byte packet), then the RESPONSE. The host
// forwards the parts in order along the route; the requester checks every mon with
// MP_ValidateMon as it arrives. Any bad mon, a missing / extra / out-of-order part, or a count
// that disagrees rejects the whole transfer (BAD_DATA).

#include <string.h>
#include "mp/mp_session_internal.h"

#define MP_INTERACT_CLIENT_HOST_PEER 0  // a client's only transport peer (as in mp_client.c)

u32 gMpInteractTimeoutMs = MP_INTERACT_TIMEOUT_MS;
void (*gMpLocalTrainerCardFunc)(struct MpTrainerCard *out) = MP_BuildLocalTrainerCard;
u8 (*gMpLocalPartyFunc)(u8 mons[PARTY_SIZE][MP_MON_WIRE_SIZE]) = MP_BuildLocalParty;

static u8 sInteractBuf[MP_MAX_PACKET_SIZE];

static const char *const sKindNames[MP_INTERACT_KIND_COUNT] = { "none", "trade", "battle", "trainer card", "party", "double battle" };
static const char *const sResultNames[MP_INTERACT_RESULT_COUNT] =
{
    "none", "accepted", "declined", "busy", "timeout", "unavailable", "cancelled", "bad data",
};

// Kinds answered by the target's session without a prompt; they never make anyone busy.
static bool8 MpKindIsSilent(u8 kind)
{
    return kind == MP_INTERACT_TRAINER_CARD;
}

static const char *MpKindName(u8 kind)
{
    return (kind < MP_INTERACT_KIND_COUNT) ? sKindNames[kind] : "?";
}

static const char *MpResultName(u8 result)
{
    return (result < MP_INTERACT_RESULT_COUNT) ? sResultNames[result] : "?";
}

static bool8 MpSlotLive(u8 slot, u8 epoch)
{
    return slot < MP_MAX_PLAYERS && gMpSession->slotUsed[slot] && gMpSession->players[slot].epoch == epoch;
}

// Both players' latest presence is on the same (real) map. Avatars on a connected map are
// visible across the seam (PS-032), but a request needs both on one map (card: "a request for
// a slot that isn't on your map").
static bool8 MpSameMap(u8 a, u8 b)
{
    const struct MpPlayer *pa = &gMpSession->players[a];
    const struct MpPlayer *pb = &gMpSession->players[b];

    if (!pa->hasPresence || !pb->hasPresence)
        return FALSE;
    if (pa->presence.mapGroup == MP_MAP_NONE || pb->presence.mapGroup == MP_MAP_NONE)
        return FALSE;
    return pa->presence.mapGroup == pb->presence.mapGroup && pa->presence.mapNum == pb->presence.mapNum;
}

static void MpSendInteract(u8 peer, u8 type, u8 slot, u8 epoch, u8 kind, u8 requestId, u8 result)
{
    struct MpMsgInteract msg;
    u16 len;

    if (peer == MP_PEER_NONE)
        return;
    msg.slot = slot;
    msg.epoch = epoch;
    msg.kind = kind;
    msg.requestId = requestId;
    msg.result = result;
    len = MpProto_WriteInteract(sInteractBuf, sizeof(sInteractBuf), type, &msg);
    MpSession_SendTo(peer, sInteractBuf, len, MP_CHANNEL_CONTROL, TRUE);
}

// Host: peer of a slot in the table, MP_PEER_NONE for the host itself / empty slots.
static u8 MpPeerOfSlot(u8 slot)
{
    if (slot >= MP_MAX_PLAYERS || !gMpSession->slotUsed[slot] || slot == gMpSession->localSlot)
        return MP_PEER_NONE;
    return gMpSession->slotPeer[slot];
}

// ---------------------------------------------------------------------------
// Endpoint (we are the requester or the target)
// ---------------------------------------------------------------------------

// A request to us arrived (from the network, or from the host role on this instance).
static void MpEndpointRespond(u8 result);
static void MpHostRouteResponse(u8 target, u8 requester, u8 requestId, u8 result);

static void MpSendCardData(u8 peer, u8 slot, u8 epoch, u8 requestId, const struct MpTrainerCard *card)
{
    struct MpMsgTrainerCardData msg;
    u16 len;

    if (peer == MP_PEER_NONE)
        return;
    memset(&msg, 0, sizeof(msg));
    msg.slot = slot;
    msg.epoch = epoch;
    msg.requestId = requestId;
    msg.card = *card;
    len = MpProto_WriteTrainerCardData(sInteractBuf, sizeof(sInteractBuf), &msg);
    MpSession_SendTo(peer, sInteractBuf, len, MP_CHANNEL_CONTROL, TRUE);
}

// Target, kind TRAINER_CARD: no prompt. Send the card, then ACCEPTED.
static void MpEndpointSendTrainerCard(u8 requester, u8 requesterEpoch, u8 requestId)
{
    struct MpSession *s = gMpSession;
    struct MpTrainerCard card;

    memset(&card, 0, sizeof(card));
    gMpLocalTrainerCardFunc(&card);
    MP_SLOG("interact: sending our trainer card to slot %u (request %u)\n", (unsigned)requester, (unsigned)requestId);
    if (s->isHost)
    {
        // We are the target and the router: straight to the requester (never ourselves).
        if (requester < MP_MAX_PLAYERS)
            s->interact.routes[requester].dataCount++;
        MpSendCardData(MpPeerOfSlot(requester), s->localSlot, s->players[s->localSlot].epoch, requestId, &card);
        MpHostRouteResponse(s->localSlot, requester, requestId, MP_INTERACT_RESULT_ACCEPTED);
    }
    else
    {
        MpSendCardData(MP_INTERACT_CLIENT_HOST_PEER, requester, requesterEpoch, requestId, &card);
        MpSendInteract(MP_INTERACT_CLIENT_HOST_PEER, MP_MSG_INTERACT_RESPONSE, requester, requesterEpoch,
                       MP_INTERACT_TRAINER_CARD, requestId, MP_INTERACT_RESULT_ACCEPTED);
    }
}

// PS-043: the party in parts of up to 3 mons, in order, to `peer`.
static void MpSendPartyParts(u8 peer, u8 slot, u8 epoch, u8 requestId, u8 count, u8 mons[PARTY_SIZE][MP_MON_WIRE_SIZE])
{
    struct MpMsgPartyData msg;
    u8 parts = MP_PARTY_PART_COUNT(count);
    u8 p, i;
    u16 len;

    if (peer == MP_PEER_NONE)
        return;
    for (p = 0; p < parts; p++)
    {
        memset(&msg, 0, sizeof(msg));
        msg.slot = slot;
        msg.epoch = epoch;
        msg.requestId = requestId;
        msg.partyCount = count;
        msg.partIndex = p;
        msg.monCount = count - p * MP_PARTY_MONS_PER_PART;
        if (msg.monCount > MP_PARTY_MONS_PER_PART)
            msg.monCount = MP_PARTY_MONS_PER_PART;
        for (i = 0; i < msg.monCount; i++)
            memcpy(msg.mons[i], mons[p * MP_PARTY_MONS_PER_PART + i], MP_MON_WIRE_SIZE);
        len = MpProto_WritePartyData(sInteractBuf, sizeof(sInteractBuf), &msg);
        MpSession_SendTo(peer, sInteractBuf, len, MP_CHANNEL_CONTROL, TRUE);
    }
}

// Target, kind VIEW_PARTY, the player said YES: send the party (before the RESPONSE).
static void MpEndpointSendParty(u8 requester, u8 requesterEpoch, u8 requestId)
{
    struct MpSession *s = gMpSession;
    u8 mons[PARTY_SIZE][MP_MON_WIRE_SIZE];
    u8 count;

    memset(mons, 0, sizeof(mons));
    count = gMpLocalPartyFunc(mons);
    if (count > PARTY_SIZE)
        count = PARTY_SIZE;
    MP_SLOG("interact: showing our party (%u) to slot %u (request %u)\n", (unsigned)count, (unsigned)requester, (unsigned)requestId);
    if (s->isHost)
    {
        if (requester < MP_MAX_PLAYERS)
            s->interact.routes[requester].dataCount += MP_PARTY_PART_COUNT(count);
        MpSendPartyParts(MpPeerOfSlot(requester), s->localSlot, s->players[s->localSlot].epoch, requestId, count, mons);
    }
    else
    {
        MpSendPartyParts(MP_INTERACT_CLIENT_HOST_PEER, requester, requesterEpoch, requestId, count, mons);
    }
}

static void MpDeliverRequest(u8 fromSlot, u8 fromEpoch, u8 kind, u8 requestId, u32 now)
{
    struct MpInteractRequest *in = &gMpSession->interact.incoming;

    if (!MpSlotLive(fromSlot, fromEpoch))
        return; // the requester is already gone: the host cancels / times it out
    if (kind == MP_INTERACT_TRAINER_CARD)
    {
        // Public info (PS-042): answered at once, whatever we are doing, without touching
        // the incoming-request state that the trade / battle prompt uses.
        MpEndpointSendTrainerCard(fromSlot, fromEpoch, requestId);
        return;
    }
    if (in->active)
    {
        // The host never routes a second request to a target, but answer BUSY rather than
        // trust that.
        if (gMpSession->isHost)
            MpHostRouteResponse(gMpSession->localSlot, fromSlot, requestId, MP_INTERACT_RESULT_BUSY);
        else
            MpSendInteract(MP_INTERACT_CLIENT_HOST_PEER, MP_MSG_INTERACT_RESPONSE, fromSlot, fromEpoch, kind, requestId, MP_INTERACT_RESULT_BUSY);
        return;
    }
    in->active = TRUE;
    in->slot = fromSlot;
    in->epoch = fromEpoch;
    in->kind = kind;
    in->requestId = requestId;
    in->startMs = now;
    MP_SLOG("interact: slot %u asks us to %s (request %u)\n", (unsigned)fromSlot, MpKindName(kind), (unsigned)requestId);
    MpSession_QueueInteractEvent(MP_EVENT_INTERACT_REQUEST, fromSlot, kind, MP_INTERACT_RESULT_NONE);
}

// Requester: the request ended with `result`. For an ACCEPTED request that carries data,
// check that the data arrived and was valid; returns the result to report.
static u8 MpFinishRx(u8 kind, u8 result)
{
    struct MpInteractRx *rx = &gMpSession->interact.rx;
    bool8 ok;

    if (kind != MP_INTERACT_TRAINER_CARD && kind != MP_INTERACT_VIEW_PARTY)
        return result;
    if (result != MP_INTERACT_RESULT_ACCEPTED)
    {
        memset(rx, 0, sizeof(*rx));
        return result;
    }
    if (kind == MP_INTERACT_TRAINER_CARD)
        ok = !rx->bad && rx->kind == kind && rx->hasCard;
    else // PS-043: every part, in order, every mon valid, and the counts agree
        ok = !rx->bad && rx->kind == kind && rx->partsReceived > 0
          && rx->partsReceived == MP_PARTY_PART_COUNT(rx->partyCount) && rx->monsReceived == rx->partyCount;
    if (!ok)
    {
        MP_SLOG("interact: the %s from slot %u was missing or invalid; rejected\n", MpKindName(kind), (unsigned)rx->slot);
        memset(rx, 0, sizeof(*rx));
        return MP_INTERACT_RESULT_BAD_DATA;
    }
    rx->complete = TRUE;
    return result;
}

// Requester: a TRAINER_CARD_DATA for our request (already parsed and clamped).
static void MpDeliverCardData(u8 targetSlot, u8 requestId, const struct MpTrainerCard *card)
{
    struct MpInteractRequest *out = &gMpSession->interact.outgoing;
    struct MpInteractRx *rx = &gMpSession->interact.rx;

    if (!out->active || out->slot != targetSlot || out->requestId != requestId)
        return; // stale
    if (out->kind != MP_INTERACT_TRAINER_CARD || rx->hasCard)
    {
        rx->bad = TRUE; // not what we asked for, or a second copy: reject the transfer
        return;
    }
    rx->card = *card;
    rx->hasCard = TRUE;
}

// Requester (PS-043): a PARTY_DATA part for our request (layout already parsed). Checks the
// order and the count, and validates every mon now; anything wrong rejects the transfer.
static void MpDeliverPartyData(u8 targetSlot, const struct MpMsgPartyData *msg)
{
    struct MpInteractRequest *out = &gMpSession->interact.outgoing;
    struct MpInteractRx *rx = &gMpSession->interact.rx;
    u32 i, first;

    if (!out->active || out->slot != targetSlot || out->requestId != msg->requestId)
        return; // stale
    if (rx->bad)
        return;
    if (out->kind != MP_INTERACT_VIEW_PARTY || msg->partIndex != rx->partsReceived
     || (rx->partsReceived > 0 && msg->partyCount != rx->partyCount))
    {
        rx->bad = TRUE; // not asked for, out of order / duplicated, or the count changed
        return;
    }
    first = (u32)msg->partIndex * MP_PARTY_MONS_PER_PART;
    if (first + msg->monCount > PARTY_SIZE)
    {
        rx->bad = TRUE; // cannot happen after the parser; never index past the buffer
        return;
    }
    for (i = 0; i < msg->monCount; i++)
    {
        if (!MP_ValidateMon(msg->mons[i]))
        {
            MP_SLOG("interact: mon %u from slot %u failed validation; the party is rejected\n",
                    (unsigned)(first + i), (unsigned)targetSlot);
            rx->bad = TRUE;
            return;
        }
    }
    for (i = 0; i < msg->monCount; i++)
        memcpy(rx->mons[first + i], msg->mons[i], MP_MON_WIRE_SIZE);
    rx->partyCount = msg->partyCount;
    rx->partsReceived++;
    rx->monsReceived += msg->monCount;
}

// The answer to our request arrived.
static void MpDeliverResponse(u8 targetSlot, u8 requestId, u8 result)
{
    struct MpInteractRequest *out = &gMpSession->interact.outgoing;

    if (!out->active || out->slot != targetSlot || out->requestId != requestId)
        return; // stale (cancelled, timed out locally, or an old request)
    out->active = FALSE;
    result = MpFinishRx(out->kind, result);
    MP_SLOG("interact: our %s request to slot %u: %s\n", MpKindName(out->kind), (unsigned)targetSlot, MpResultName(result));
    MpSession_QueueInteractEvent(MP_EVENT_INTERACT_RESULT, targetSlot, out->kind, result);
}

// The request to us is gone.
static void MpDeliverCancel(u8 fromSlot, u8 requestId, u8 why)
{
    struct MpInteractRequest *in = &gMpSession->interact.incoming;

    if (!in->active || in->slot != fromSlot || in->requestId != requestId)
        return;
    in->active = FALSE;
    MP_SLOG("interact: request from slot %u withdrawn (%s)\n", (unsigned)fromSlot, MpResultName(why));
    MpSession_QueueInteractEvent(MP_EVENT_INTERACT_CANCELLED, fromSlot, in->kind, why);
}

// ---------------------------------------------------------------------------
// Host routing
// ---------------------------------------------------------------------------

// Sends `result` for a request that was never routed (or no longer is) back to `requester`.
static void MpHostRefuse(u8 requester, u8 target, u8 targetEpoch, u8 kind, u8 requestId, u8 result)
{
    MP_SLOG("interact: %s request from slot %u to slot %u refused: %s\n", MpKindName(kind),
            (unsigned)requester, (unsigned)target, MpResultName(result));
    if (requester == gMpSession->localSlot)
        MpDeliverResponse(target, requestId, result);
    else
        MpSendInteract(MpPeerOfSlot(requester), MP_MSG_INTERACT_RESPONSE, target, targetEpoch ? targetEpoch : 1, kind, requestId, result);
}

// Returns MP_INTERACT_RESULT_NONE if routed, else the refusal (already sent back).
static u8 MpHostRouteRequest(u8 from, u8 to, u8 toEpoch, u8 kind, u8 requestId, u32 now)
{
    struct MpInteractState *st = &gMpSession->interact;
    struct MpInteractRoute *route;
    u8 result = MP_INTERACT_RESULT_NONE;
    u32 i;

    if (from >= MP_MAX_PLAYERS || !gMpSession->slotUsed[from])
        return MP_INTERACT_RESULT_UNAVAILABLE;
    route = &st->routes[from];

    if (route->active)
    {
        result = MP_INTERACT_RESULT_UNAVAILABLE;            // one request in flight per requester
    }
    else if (st->lastRequestMs[from] != 0 && (u32)(now - st->lastRequestMs[from]) < MP_INTERACT_MIN_GAP_MS)
    {
        result = MP_INTERACT_RESULT_UNAVAILABLE;            // rate limit (1 per MP_INTERACT_MIN_GAP_MS)
    }
    else
    {
        st->lastRequestMs[from] = now ? now : 1;            // every request past the limits counts
        if (to == from || !MpSlotLive(to, toEpoch) || gMpSession->players[to].connectionLost)
            result = MP_INTERACT_RESULT_UNAVAILABLE;        // gone / stale / ourselves / reconnecting (PS-054)
        else if (!MpSameMap(from, to))
            result = MP_INTERACT_RESULT_UNAVAILABLE;        // not on the requester's map
        else if (!MpKindIsSilent(kind))                     // PS-042: a TRAINER_CARD request is never busy
        {
            if (st->routes[to].active && !MpKindIsSilent(st->routes[to].kind))
                result = MP_INTERACT_RESULT_BUSY;           // the target is waiting on someone itself
            for (i = 0; i < MP_MAX_PLAYERS; i++)
            {
                if (st->routes[i].active && st->routes[i].to == to && !MpKindIsSilent(st->routes[i].kind))
                    result = MP_INTERACT_RESULT_BUSY;       // someone else is already asking
            }
        }
    }
    if (result != MP_INTERACT_RESULT_NONE)
    {
        MpHostRefuse(from, to, toEpoch, kind, requestId, result);
        return result;
    }

    route->active = TRUE;
    route->fromEpoch = gMpSession->players[from].epoch;
    route->to = to;
    route->toEpoch = toEpoch;
    route->kind = kind;
    route->requestId = requestId;
    route->dataCount = 0;
    route->startMs = now;
    MP_SLOG("interact: routing %s request %u from slot %u to slot %u\n", MpKindName(kind), (unsigned)requestId, (unsigned)from, (unsigned)to);
    if (to == gMpSession->localSlot)
        MpDeliverRequest(from, route->fromEpoch, kind, requestId, now);
    else
        MpSendInteract(MpPeerOfSlot(to), MP_MSG_INTERACT_REQUEST, from, route->fromEpoch, kind, requestId, 0);
    return MP_INTERACT_RESULT_NONE;
}

// The target answered (or the host answers for it).
static void MpHostRouteResponse(u8 target, u8 requester, u8 requestId, u8 result)
{
    struct MpInteractRoute *route;

    if (requester >= MP_MAX_PLAYERS)
        return;
    route = &gMpSession->interact.routes[requester];
    if (!route->active || route->to != target || route->requestId != requestId)
        return; // stale
    route->active = FALSE;
    if (requester == gMpSession->localSlot)
        MpDeliverResponse(target, requestId, result);
    else if (MpSlotLive(requester, route->fromEpoch))
        MpSendInteract(MpPeerOfSlot(requester), MP_MSG_INTERACT_RESPONSE, target, route->toEpoch, route->kind, requestId, result);
}

// The requester withdrew (or left, or the host timed the request out): tell the target.
static void MpHostRouteCancel(u8 requester, u8 requestId, u8 why)
{
    struct MpInteractRoute *route;

    if (requester >= MP_MAX_PLAYERS)
        return;
    route = &gMpSession->interact.routes[requester];
    if (!route->active || route->requestId != requestId)
        return;
    route->active = FALSE;
    if (route->to == gMpSession->localSlot)
        MpDeliverCancel(requester, requestId, why);
    else if (MpSlotLive(route->to, route->toEpoch))
        MpSendInteract(MpPeerOfSlot(route->to), MP_MSG_INTERACT_CANCEL, requester, route->fromEpoch, route->kind, requestId, 0);
}

// ---------------------------------------------------------------------------
// Endpoint answer (target side)
// ---------------------------------------------------------------------------

static void MpEndpointRespond(u8 result)
{
    struct MpInteractRequest *in = &gMpSession->interact.incoming;

    if (!in->active)
        return;
    in->active = FALSE;
    MP_SLOG("interact: we answer slot %u's %s request: %s\n", (unsigned)in->slot, MpKindName(in->kind), MpResultName(result));
    // PS-043: the party goes out first, on the same ordered channel as the RESPONSE.
    if (result == MP_INTERACT_RESULT_ACCEPTED && in->kind == MP_INTERACT_VIEW_PARTY)
        MpEndpointSendParty(in->slot, in->epoch, in->requestId);
    if (gMpSession->isHost)
        MpHostRouteResponse(gMpSession->localSlot, in->slot, in->requestId, result);
    else
        MpSendInteract(MP_INTERACT_CLIENT_HOST_PEER, MP_MSG_INTERACT_RESPONSE, in->slot, in->epoch, in->kind, in->requestId, result);
}

// ---------------------------------------------------------------------------
// Receive paths
// ---------------------------------------------------------------------------

// Host: TRAINER_CARD_DATA from the target `from`; msg->slot names the requester. Forwarded only
// along the matching live route. Returns FALSE (malformed) for data the route does not allow.
static bool8 MpHostRouteCardData(u8 from, const struct MpMsgTrainerCardData *msg)
{
    struct MpInteractRoute *route = &gMpSession->interact.routes[msg->slot]; // slot < 4 (parser)
    u8 requester = msg->slot;

    if (!route->active || route->to != from || route->requestId != msg->requestId)
        return TRUE; // stale (cancelled, timed out, requester gone): drop silently
    if (route->kind != MP_INTERACT_TRAINER_CARD || route->dataCount >= 1)
        return FALSE; // data the request did not ask for, or a second copy
    route->dataCount++;
    if (requester == gMpSession->localSlot)
        MpDeliverCardData(from, msg->requestId, &msg->card);
    else if (MpSlotLive(requester, route->fromEpoch))
        MpSendCardData(MpPeerOfSlot(requester), from, route->toEpoch, msg->requestId, &msg->card);
    return TRUE;
}

// Host (PS-043): a PARTY_DATA part from the target `from`; msg->slot names the requester. Parts
// must come in order along a live VIEW_PARTY route, at most MP_PARTY_PART_COUNT of them.
static bool8 MpHostRoutePartyData(u8 from, struct MpMsgPartyData *msg)
{
    struct MpInteractRoute *route = &gMpSession->interact.routes[msg->slot]; // slot < 4 (parser)
    u8 requester = msg->slot;

    if (!route->active || route->to != from || route->requestId != msg->requestId)
        return TRUE; // stale: drop silently
    if (route->kind != MP_INTERACT_VIEW_PARTY || msg->partIndex != route->dataCount
     || route->dataCount >= MP_PARTY_MAX_PARTS)
        return FALSE; // not asked for, out of order, or too many
    route->dataCount++;
    if (requester == gMpSession->localSlot)
    {
        MpDeliverPartyData(from, msg);
    }
    else if (MpSlotLive(requester, route->fromEpoch))
    {
        u16 len;

        msg->slot = from;
        msg->epoch = route->toEpoch;
        len = MpProto_WritePartyData(sInteractBuf, sizeof(sInteractBuf), msg);
        MpSession_SendTo(MpPeerOfSlot(requester), sInteractBuf, len, MP_CHANNEL_CONTROL, TRUE);
    }
    return TRUE;
}

bool8 MpInteract_OnHostReceive(u8 peer, u8 type, const u8 *payload, u32 len, u32 now)
{
    struct MpMsgInteract msg;
    struct MpMsgTrainerCardData card;
    struct MpMsgPartyData party;
    u8 from;

    if (peer >= MP_HOST_PEER_COUNT || gMpSession->peers[peer].state != MP_HOST_PEER_PLAYER)
        return TRUE;
    from = gMpSession->peers[peer].slot;
    if (from >= MP_MAX_PLAYERS)
        return TRUE;
    if (type == MP_MSG_TRAINER_CARD_DATA)
    {
        if (MpProto_ParseTrainerCardData(payload, len, &card) != MP_PARSE_OK)
            return FALSE;
        return MpHostRouteCardData(from, &card);
    }
    if (type == MP_MSG_PARTY_DATA)
    {
        if (MpProto_ParsePartyData(payload, len, &party) != MP_PARSE_OK)
            return FALSE;
        return MpHostRoutePartyData(from, &party);
    }
    if (MpProto_ParseInteract(type, payload, len, &msg) != MP_PARSE_OK)
        return FALSE;

    switch (type)
    {
    case MP_MSG_INTERACT_REQUEST:
        // msg.slot = target. The requester is the sender's real slot (never trusted from the wire).
        MpHostRouteRequest(from, msg.slot, msg.epoch, msg.kind, msg.requestId, now);
        break;
    case MP_MSG_INTERACT_RESPONSE:
        // msg.slot = requester; the target is the sender.
        MpHostRouteResponse(from, msg.slot, msg.requestId, msg.result);
        break;
    case MP_MSG_INTERACT_CANCEL:
        MpHostRouteCancel(from, msg.requestId, MP_INTERACT_RESULT_CANCELLED);
        break;
    }
    return TRUE;
}

bool8 MpInteract_OnClientReceive(u8 type, const u8 *payload, u32 len)
{
    struct MpMsgInteract msg;
    struct MpMsgTrainerCardData card;

    if (type == MP_MSG_TRAINER_CARD_DATA)
    {
        if (MpProto_ParseTrainerCardData(payload, len, &card) != MP_PARSE_OK)
        {
            gMpSession->interact.rx.bad = TRUE; // the RESPONSE that follows becomes BAD_DATA
            return FALSE;
        }
        if (card.slot == gMpSession->localSlot)
            return FALSE; // the host never names us as the other party
        MpDeliverCardData(card.slot, card.requestId, &card.card);
        return TRUE;
    }
    if (type == MP_MSG_PARTY_DATA)
    {
        struct MpMsgPartyData party;

        if (MpProto_ParsePartyData(payload, len, &party) != MP_PARSE_OK)
        {
            gMpSession->interact.rx.bad = TRUE;
            return FALSE;
        }
        if (party.slot == gMpSession->localSlot)
            return FALSE;
        MpDeliverPartyData(party.slot, &party);
        return TRUE;
    }
    if (MpProto_ParseInteract(type, payload, len, &msg) != MP_PARSE_OK)
        return FALSE;
    if (msg.slot == gMpSession->localSlot)
        return FALSE; // the host never names us as the other party

    switch (type)
    {
    case MP_MSG_INTERACT_REQUEST:
        MpDeliverRequest(msg.slot, msg.epoch, msg.kind, msg.requestId, MpTransport_GetTimeMs());
        break;
    case MP_MSG_INTERACT_RESPONSE:
        MpDeliverResponse(msg.slot, msg.requestId, msg.result);
        break;
    case MP_MSG_INTERACT_CANCEL:
        MpDeliverCancel(msg.slot, msg.requestId, MP_INTERACT_RESULT_CANCELLED);
        break;
    }
    return TRUE;
}

void MpInteract_OnSlotLeaving(u8 slot)
{
    struct MpInteractState *st = &gMpSession->interact;
    u32 i;

    if (slot >= MP_MAX_PLAYERS)
        return;
    if (gMpSession->isHost)
    {
        // Its own request: tell its target.
        if (st->routes[slot].active)
            MpHostRouteCancel(slot, st->routes[slot].requestId, MP_INTERACT_RESULT_CANCELLED);
        // Requests to it: tell the requesters.
        for (i = 0; i < MP_MAX_PLAYERS; i++)
        {
            if (st->routes[i].active && st->routes[i].to == slot)
                MpHostRouteResponse(slot, (u8)i, st->routes[i].requestId, MP_INTERACT_RESULT_UNAVAILABLE);
        }
        st->lastRequestMs[slot] = 0;
    }
    // Endpoint state (both roles; on the host the routes above usually cleared it already).
    if (st->outgoing.active && st->outgoing.slot == slot)
        MpDeliverResponse(slot, st->outgoing.requestId, MP_INTERACT_RESULT_UNAVAILABLE);
    if (st->incoming.active && st->incoming.slot == slot)
        MpDeliverCancel(slot, st->incoming.requestId, MP_INTERACT_RESULT_CANCELLED);
}

void MpInteract_Update(u32 now)
{
    struct MpInteractState *st = &gMpSession->interact;
    u32 i;

    if (gMpSession->state != MP_STATE_IN_SESSION)
        return;

    // Target: no answer in time -> TIMEOUT to the requester, and the prompt goes away.
    if (st->incoming.active && (u32)(now - st->incoming.startMs) >= gMpInteractTimeoutMs)
    {
        u8 from = st->incoming.slot;
        u8 kind = st->incoming.kind;

        MpEndpointRespond(MP_INTERACT_RESULT_TIMEOUT);
        MpSession_QueueInteractEvent(MP_EVENT_INTERACT_CANCELLED, from, kind, MP_INTERACT_RESULT_TIMEOUT);
    }

    // Host backstop: the target never answered (gone without a trace, or misbehaving).
    if (gMpSession->isHost)
    {
        for (i = 0; i < MP_MAX_PLAYERS; i++)
        {
            struct MpInteractRoute *route = &st->routes[i];

            if (route->active && (u32)(now - route->startMs) >= gMpInteractTimeoutMs + MP_INTERACT_HOST_GRACE_MS)
            {
                struct MpInteractRoute r = *route;

                MP_SLOG("interact: request %u from slot %u to slot %u timed out at the host\n",
                        (unsigned)r.requestId, (unsigned)i, (unsigned)r.to);
                route->active = FALSE;
                // Tell the target the request is gone...
                if (r.to == gMpSession->localSlot)
                    MpDeliverCancel((u8)i, r.requestId, MP_INTERACT_RESULT_TIMEOUT);
                else if (MpSlotLive(r.to, r.toEpoch))
                    MpSendInteract(MpPeerOfSlot(r.to), MP_MSG_INTERACT_CANCEL, (u8)i, r.fromEpoch, r.kind, r.requestId, 0);
                // ...and the requester that it timed out.
                if (i == gMpSession->localSlot)
                    MpDeliverResponse(r.to, r.requestId, MP_INTERACT_RESULT_TIMEOUT);
                else if (MpSlotLive((u8)i, r.fromEpoch))
                    MpSendInteract(MpPeerOfSlot((u8)i), MP_MSG_INTERACT_RESPONSE, r.to, r.toEpoch, r.kind, r.requestId, MP_INTERACT_RESULT_TIMEOUT);
            }
        }
    }

    // Requester backstop: the host never answered.
    if (st->outgoing.active && (u32)(now - st->outgoing.startMs) >= gMpInteractTimeoutMs + MP_INTERACT_CLIENT_GRACE_MS)
        MpDeliverResponse(st->outgoing.slot, st->outgoing.requestId, MP_INTERACT_RESULT_TIMEOUT);
}

// ---------------------------------------------------------------------------
// Public API (mp.h)
// ---------------------------------------------------------------------------

bool8 MP_SendInteractRequest(u8 targetSlot, u8 kind)
{
    struct MpSession *s = gMpSession;
    struct MpInteractRequest *out = &s->interact.outgoing;
    u32 now;

    if (s->state != MP_STATE_IN_SESSION || s->localSlot >= MP_MAX_PLAYERS || s->client.reconnecting) // PS-054
        return FALSE;
    if (kind == MP_INTERACT_NONE || kind >= MP_INTERACT_KIND_COUNT)
        return FALSE;
    if (targetSlot >= MP_MAX_PLAYERS || targetSlot == s->localSlot || !s->slotUsed[targetSlot])
        return FALSE;
    if (s->players[targetSlot].connectionLost) // PS-054: that player is reconnecting
        return FALSE;
    if (out->active)
        return FALSE;

    now = MpTransport_GetTimeMs();
    out->active = TRUE;
    out->slot = targetSlot;
    out->epoch = s->players[targetSlot].epoch;
    out->kind = kind;
    out->requestId = ++s->interact.nextRequestId;
    out->startMs = now;
    // PS-042: a new request forgets whatever was viewed before.
    memset(&s->interact.rx, 0, sizeof(s->interact.rx));
    s->interact.rx.kind = kind;
    s->interact.rx.slot = targetSlot;
    s->interact.rx.requestId = out->requestId;
    MP_SLOG("interact: asking slot %u to %s (request %u)\n", (unsigned)targetSlot, MpKindName(kind), (unsigned)out->requestId);
    if (s->isHost)
        MpHostRouteRequest(s->localSlot, targetSlot, out->epoch, kind, out->requestId, now); // a refusal arrives as an event
    else
        MpSendInteract(MP_INTERACT_CLIENT_HOST_PEER, MP_MSG_INTERACT_REQUEST, targetSlot, out->epoch, kind, out->requestId, 0);
    return TRUE;
}

void MP_CancelInteractRequest(void)
{
    struct MpSession *s = gMpSession;
    struct MpInteractRequest *out = &s->interact.outgoing;

    if (!out->active)
        return;
    out->active = FALSE;
    memset(&s->interact.rx, 0, sizeof(s->interact.rx));
    if (s->state != MP_STATE_IN_SESSION)
        return;
    MP_SLOG("interact: we withdraw request %u to slot %u\n", (unsigned)out->requestId, (unsigned)out->slot);
    if (s->isHost)
        MpHostRouteCancel(s->localSlot, out->requestId, MP_INTERACT_RESULT_CANCELLED);
    else
        MpSendInteract(MP_INTERACT_CLIENT_HOST_PEER, MP_MSG_INTERACT_CANCEL, out->slot, out->epoch ? out->epoch : 1, out->kind, out->requestId, 0);
}

bool8 MP_IsInteractRequestPending(void)
{
    return gMpSession->state == MP_STATE_IN_SESSION && gMpSession->interact.outgoing.active;
}

void MP_RespondInteract(u8 result)
{
    if (gMpSession->state != MP_STATE_IN_SESSION)
        return;
    if (result != MP_INTERACT_RESULT_ACCEPTED && result != MP_INTERACT_RESULT_DECLINED && result != MP_INTERACT_RESULT_BUSY)
        return;
    MpEndpointRespond(result);
}

bool8 MP_HasIncomingInteract(void)
{
    return gMpSession->state == MP_STATE_IN_SESSION && gMpSession->interact.incoming.active;
}

bool8 MP_GetViewedTrainerCard(u8 slot, struct MpTrainerCard *out)
{
    const struct MpInteractRx *rx = &gMpSession->interact.rx;

    if (out == NULL)
        return FALSE;
    if (gMpSession->state != MP_STATE_IN_SESSION || !rx->complete || rx->kind != MP_INTERACT_TRAINER_CARD
     || !rx->hasCard || rx->slot != slot)
        return FALSE;
    *out = rx->card;
    return TRUE;
}

bool8 MP_GetViewedParty(u8 slot, u8 *countOut, u8 monsOut[PARTY_SIZE][MP_MON_WIRE_SIZE])
{
    const struct MpInteractRx *rx = &gMpSession->interact.rx;
    u32 i;

    if (countOut == NULL || monsOut == NULL)
        return FALSE;
    *countOut = 0;
    if (gMpSession->state != MP_STATE_IN_SESSION || !rx->complete || rx->kind != MP_INTERACT_VIEW_PARTY
     || rx->slot != slot || rx->partyCount > PARTY_SIZE)
        return FALSE;
    for (i = 0; i < rx->partyCount; i++)
        memcpy(monsOut[i], rx->mons[i], MP_MON_WIRE_SIZE);
    *countOut = rx->partyCount;
    return TRUE;
}

void MP_ClearViewedData(void)
{
    memset(&gMpSession->interact.rx, 0, sizeof(gMpSession->interact.rx));
}
