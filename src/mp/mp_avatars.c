// Remote player avatars (PS-031): other session players drawn as object events on the
// local player's map, animated from their PRESENCE stream (PS-030).
//
// Called once per game tick from MainLoop() (src/main.c, "// MP:"), after the capture hook.
// It only touches object events while the overworld callback (CB2_Overworld) runs; outside
// the field the sprite system belongs to another screen.
//
// Rules (PS-031 card + PS-014 requirements):
//  - localId MP_AVATAR_LOCAL_ID_BASE + slot, no object template, flagId 0 (no template to
//    look one up in), movement type NONE (held movements only), trainer type NONE. Spawned
//    with SpawnSpecialObjectEvent, removed with RemoveObjectEventById (= the static
//    RemoveObjectEvent). Never Overworld_SetObjEventTemplate*, RemoveObjectEventByLocalIdAndMap
//    or FlagSet.
//  - Budget: at most MP_MAX_REMOTE_AVATARS, and only while MpAvatar_Budget() allows it, so
//    map NPCs near the view and scripts always find a free slot. Never evict an NPC.
//  - Not solid, not interactive: hooks in event_object_movement.c skip avatars in the object
//    collision check and in GetObjectEventIdByPosition (A button, counters).
//  - Never saved: MP_RemoveRemoteAvatars() runs at the start of SaveObjectEvents().
//  - Lifetimes: object events are wiped by every map load (ResetObjectEvents) and keep their
//    data across battles / menus (SpawnObjectEventsOnReturnToField re-creates their sprites).
//    So on every field entry each tracked avatar is re-validated by localId + map, then
//    re-spawned or snapped. The game may also remove an avatar that walked out of view
//    (RemoveObjectEventsOutsideView); it is re-spawned when its player is back in view.
//  - AD-1: nothing here changes world state; only our own object events and sprites.

#include "global.h"
#include "battle_pyramid.h"
#include "event_data.h"
#include "event_object_movement.h"
#include "field_effect.h"
#include "field_effect_helpers.h"
#include "fieldmap.h"
#include "main.h"
#include "overworld.h"
#include "script.h"
#include "sprite.h"
#include "task.h"
#include "trainer_hill.h"
#include "constants/battle_pyramid.h"
#include "constants/event_objects.h"
#include "constants/event_object_movement.h"
#include "constants/field_effects.h"
#include "constants/maps.h"
#include "constants/trainer_types.h"
#include "mp/mp.h"
#include "mp/mp_avatar.h"
#include "mp/mp_internal.h"

#define MP_BLOB_NONE MAX_SPRITES

struct MpAvatar
{
    bool8 spawned;
    u8 objectEventId;
    u8 epoch;               // epoch of the player it was spawned for
    u8 mapGroup;
    u8 mapNum;
    u8 avatarState;         // MP_AVATAR_* currently shown
    u8 gender;
    u8 blobSpriteId;        // surf blob, MP_BLOB_NONE if none
    u8 lastStepId;          // stepId of the last update the avatar finished
    bool8 hasPending;
    struct MpPresence pending;
};

static struct MpAvatar sAvatars[MP_MAX_PLAYERS];
static bool8 sAvatarsInit;
static bool8 sFieldLastTick;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void MpAvatarResetState(struct MpAvatar *av)
{
    memset(av, 0, sizeof(*av));
    av->objectEventId = OBJECT_EVENTS_COUNT;
    av->blobSpriteId = MP_BLOB_NONE;
}

static void MpAvatarsInit(void)
{
    u32 i;

    if (sAvatarsInit)
        return;
    sAvatarsInit = TRUE;
    for (i = 0; i < MP_MAX_PLAYERS; i++)
        MpAvatarResetState(&sAvatars[i]);
}

static bool8 MpInField(void)
{
    return gMain.callback2 == CB2_Overworld && gSaveBlock1Ptr != NULL;
}

// Maps where avatars are never shown in M1 (PS-032 owns the full deny table).
static bool8 MpAvatarsAllowedHere(void)
{
    if (gMapHeader.events == NULL || gMapHeader.mapLayout == NULL)
        return FALSE;
    if (IsOverworldLinkActive())
        return FALSE;
    if (CurrentBattlePyramidLocation() != PYRAMID_LOCATION_NONE || InTrainerHill())
        return FALSE;
    return TRUE;
}

// TRUE if the object event is still the avatar of slot `slot` on the tracked map.
static bool8 MpAvatarObjectValid(u8 slot, const struct MpAvatar *av)
{
    const struct ObjectEvent *obj;

    if (av->objectEventId >= OBJECT_EVENTS_COUNT)
        return FALSE;
    obj = &gObjectEvents[av->objectEventId];
    return obj->active && !obj->isPlayer && obj->localId == MpAvatar_LocalIdForSlot(slot)
        && obj->mapGroup == av->mapGroup && obj->mapNum == av->mapNum;
}

// TRUE if `spriteId` is still the surf blob following object event `objectEventId`.
static bool8 MpBlobValid(u8 spriteId, u8 objectEventId)
{
    if (spriteId >= MAX_SPRITES)
        return FALSE;
    return gSprites[spriteId].inUse && gSprites[spriteId].callback == UpdateSurfBlobFieldEffect
        && gSprites[spriteId].data[2] == objectEventId; // data[2] = sPlayerObjId (field_effect_helpers.c)
}

static void MpDestroyBlob(struct MpAvatar *av)
{
    if (MpBlobValid(av->blobSpriteId, av->objectEventId))
        DestroySprite(&gSprites[av->blobSpriteId]);
    av->blobSpriteId = MP_BLOB_NONE;
}

static void MpCreateBlob(struct MpAvatar *av)
{
    struct ObjectEvent *obj = &gObjectEvents[av->objectEventId];
    u32 spriteId;

    if (MpBlobValid(av->blobSpriteId, av->objectEventId))
        return;
    gFieldEffectArguments[0] = obj->currentCoords.x;
    gFieldEffectArguments[1] = obj->currentCoords.y;
    gFieldEffectArguments[2] = av->objectEventId;
    spriteId = FieldEffectStart(FLDEFF_SURF_BLOB);
    if (spriteId < MAX_SPRITES)
    {
        SetSurfBlob_BobState((u8)spriteId, BOB_PLAYER_AND_MON);
        av->blobSpriteId = (u8)spriteId;
    }
    else
    {
        av->blobSpriteId = MP_BLOB_NONE;
    }
}

// In view = inside the rectangle RemoveObjectEventsOutsideView keeps (map coords with offset).
static bool8 MpInView(s16 x, s16 y)
{
    s16 left = gSaveBlock1Ptr->pos.x - 2;
    s16 right = gSaveBlock1Ptr->pos.x + 17;
    s16 top = gSaveBlock1Ptr->pos.y;
    s16 bottom = gSaveBlock1Ptr->pos.y + 16;

    return x >= left && x <= right && y >= top && y <= bottom;
}

static bool8 MpOnLayout(s16 x, s16 y)
{
    return x >= 0 && y >= 0 && x < gMapHeader.mapLayout->width && y < gMapHeader.mapLayout->height;
}

static u8 MpElevationAt(s16 x, s16 y)
{
    return MapGridGetElevationAt(x + MAP_OFFSET, y + MAP_OFFSET);
}

// Unhidden NPC templates near the view that are not spawned yet: they may claim a slot soon.
static u8 MpCountPendingNpcs(void)
{
    u32 i, j, count;
    u8 n = 0;
    s16 left = gSaveBlock1Ptr->pos.x - 2 - MP_AVATAR_NPC_MARGIN;
    s16 right = gSaveBlock1Ptr->pos.x + MAP_OFFSET_W + 2 + MP_AVATAR_NPC_MARGIN;
    s16 top = gSaveBlock1Ptr->pos.y - MP_AVATAR_NPC_MARGIN;
    s16 bottom = gSaveBlock1Ptr->pos.y + MAP_OFFSET_H + 2 + MP_AVATAR_NPC_MARGIN;

    count = gMapHeader.events->objectEventCount;
    if (count > OBJECT_EVENT_TEMPLATES_COUNT)
        count = OBJECT_EVENT_TEMPLATES_COUNT;
    for (i = 0; i < count; i++)
    {
        const struct ObjectEventTemplate *t = &gSaveBlock1Ptr->objectEventTemplates[i];
        s16 x = t->x + MAP_OFFSET;
        s16 y = t->y + MAP_OFFSET;
        bool8 spawned = FALSE;

        if (x < left || x > right || y < top || y > bottom || FlagGet(t->flagId))
            continue;
        for (j = 0; j < OBJECT_EVENTS_COUNT; j++)
        {
            if (gObjectEvents[j].active && gObjectEvents[j].localId == t->localId
             && gObjectEvents[j].mapNum == gSaveBlock1Ptr->location.mapNum
             && gObjectEvents[j].mapGroup == gSaveBlock1Ptr->location.mapGroup)
            {
                spawned = TRUE;
                break;
            }
        }
        if (!spawned && n < 0xFF)
            n++;
    }
    return n;
}

static u8 MpCountActiveOthers(void)
{
    u32 i;
    u8 n = 0;

    for (i = 0; i < OBJECT_EVENTS_COUNT; i++)
    {
        if (gObjectEvents[i].active && !MP_IsRemoteAvatarLocalId(gObjectEvents[i].localId))
            n++;
    }
    return n;
}

static u8 MpCountSpawned(void)
{
    u32 i;
    u8 n = 0;

    for (i = 0; i < MP_MAX_PLAYERS; i++)
    {
        if (sAvatars[i].spawned)
            n++;
    }
    return n;
}

static void MpDrainQueue(u8 slot)
{
    struct MpPresence p;
    u32 guard = 0;

    while (MP_PopRemotePresence(slot, &p) && guard++ < 64)
        ;
    MP_ClearPresenceOverflow(slot);
}

// ---------------------------------------------------------------------------
// Spawn / despawn / snap
// ---------------------------------------------------------------------------

static void MpDespawn(u8 slot, struct MpAvatar *av, const char *why)
{
    if (av->spawned && MpAvatarObjectValid(slot, av))
    {
        MpDestroyBlob(av);
        RemoveObjectEventById(av->objectEventId);
        MP_DLOG("avatar slot %u removed (%s)\n", (unsigned)slot, why);
    }
    MpAvatarResetState(av);
}

static void MpSnap(struct MpAvatar *av, const struct MpPresence *to)
{
    struct ObjectEvent *obj = &gObjectEvents[av->objectEventId];
    u8 elevation = MpElevationAt(to->x, to->y);

    MoveObjectEventToMapCoords(obj, to->x + MAP_OFFSET, to->y + MAP_OFFSET);
    obj->currentElevation = elevation;
    obj->previousElevation = elevation;
    ObjectEventTurn(obj, (to->facing >= DIR_SOUTH && to->facing <= DIR_EAST) ? to->facing : DIR_SOUTH);
    av->hasPending = FALSE;
    av->lastStepId = to->stepId;
}

static void MpSetAvatarState(struct MpAvatar *av, u8 avatarState)
{
    struct ObjectEvent *obj = &gObjectEvents[av->objectEventId];

    if (avatarState >= MP_AVATAR_STATE_COUNT)
        avatarState = MP_AVATAR_ON_FOOT;
    if (avatarState != av->avatarState)
    {
        ObjectEventSetGraphicsId(obj, MpAvatar_GraphicsId(av->gender, avatarState));
        ObjectEventTurn(obj, obj->facingDirection);
        av->avatarState = avatarState;
    }
    if (avatarState == MP_AVATAR_SURFING)
        MpCreateBlob(av);
    else
        MpDestroyBlob(av);
}

static bool8 MpSpawn(u8 slot, struct MpAvatar *av, const struct MpPlayer *player)
{
    const struct MpPresence *p = &player->presence;
    struct ObjectEventTemplate template;
    u8 avatarState = (p->avatarState < MP_AVATAR_STATE_COUNT) ? p->avatarState : MP_AVATAR_ON_FOOT;
    u8 id;

    memset(&template, 0, sizeof(template)); // flagId 0, script NULL, trainer type NONE
    template.localId = MpAvatar_LocalIdForSlot(slot);
    template.graphicsId = MpAvatar_GraphicsId(player->summary.gender, avatarState);
    template.kind = OBJ_KIND_NORMAL;
    template.x = p->x;
    template.y = p->y;
    template.elevation = MpElevationAt(p->x, p->y);
    template.movementType = MOVEMENT_TYPE_NONE;
    template.trainerType = TRAINER_TYPE_NONE;
    id = SpawnSpecialObjectEvent(&template);
    if (id >= OBJECT_EVENTS_COUNT)
    {
        MP_DLOG("avatar slot %u: no free object event / sprite, skipped\n", (unsigned)slot);
        return FALSE;
    }
    MpAvatarResetState(av);
    av->spawned = TRUE;
    av->objectEventId = id;
    av->epoch = player->epoch;
    av->mapGroup = (u8)gSaveBlock1Ptr->location.mapGroup;
    av->mapNum = (u8)gSaveBlock1Ptr->location.mapNum;
    av->gender = player->summary.gender;
    av->avatarState = avatarState;
    av->lastStepId = p->stepId;
    ObjectEventTurn(&gObjectEvents[id], (p->facing >= DIR_SOUTH && p->facing <= DIR_EAST) ? p->facing : DIR_SOUTH);
    MpSetAvatarState(av, avatarState);
    MpDrainQueue(slot); // it starts at the latest position
    MP_DLOG("avatar slot %u spawned as object %u at (%d,%d) map %u.%u\n", (unsigned)slot, (unsigned)id,
            (int)p->x, (int)p->y, (unsigned)av->mapGroup, (unsigned)av->mapNum);
    return TRUE;
}

// ---------------------------------------------------------------------------
// Replay
// ---------------------------------------------------------------------------

static void MpReplay(u8 slot, struct MpAvatar *av, const struct MpPlayer *player)
{
    struct ObjectEvent *obj = &gObjectEvents[av->objectEventId];
    s16 x = obj->currentCoords.x - MAP_OFFSET;
    s16 y = obj->currentCoords.y - MAP_OFFSET;
    u8 plan, action;
    u32 guard;

    if (player->presenceOverflow || MpAvatar_IsTooFarBehind(x, y, &player->presence))
    {
        MpDrainQueue(slot);
        MpSnap(av, &player->presence);
        MpSetAvatarState(av, player->presence.avatarState);
        MP_DLOG("avatar slot %u snapped to (%d,%d)\n", (unsigned)slot, (int)player->presence.x, (int)player->presence.y);
        return;
    }
    if (obj->frozen)
        return; // a script froze all objects; continue when it is over
    if (obj->heldMovementActive)
    {
        if (!obj->heldMovementFinished)
            return; // still stepping
        ObjectEventClearHeldMovement(obj);
    }

    for (guard = 0; guard < MP_PRESENCE_QUEUE_SIZE + 1; guard++)
    {
        if (!av->hasPending)
        {
            if (!MP_PopRemotePresence(slot, &av->pending))
                return;
            av->hasPending = TRUE;
        }
        if (av->pending.mapGroup != av->mapGroup || av->pending.mapNum != av->mapNum)
        {
            av->hasPending = FALSE; // queued before / after a visit elsewhere: not replayable here
            continue;
        }
        MpSetAvatarState(av, av->pending.avatarState);
        plan = MpAvatar_Plan(x, y, obj->facingDirection, &av->pending, av->pending.stepId != av->lastStepId, &action);
        if (plan == MP_AVATAR_PLAN_SNAP)
        {
            MpSnap(av, &av->pending);
            return;
        }
        if (plan == MP_AVATAR_PLAN_ACTION)
        {
            av->lastStepId = av->pending.stepId;
            ObjectEventSetHeldMovement(obj, action);
            return;
        }
        // DONE: consume and look at the next update in the same tick.
        av->lastStepId = av->pending.stepId;
        av->hasPending = FALSE;
    }
}

// ---------------------------------------------------------------------------
// Field entry (map load, return from battle / menu)
// ---------------------------------------------------------------------------

static void MpOnFieldEntry(void)
{
    u32 i;
    u8 objects = 0, avatars = 0, tasks = 0, sprites = 0;

    for (i = 0; i < MP_MAX_PLAYERS; i++)
    {
        struct MpAvatar *av = &sAvatars[i];

        if (!av->spawned)
            continue;
        if (!MpAvatarObjectValid((u8)i, av))
        {
            // Map load wiped it (or something reused the slot): forget, never touch.
            MpAvatarResetState(av);
            continue;
        }
        // Return from battle / menu: the sprite was re-created, the surf blob was not
        // (unless the sprite system survived, which MpBlobValid detects).
        if (!MpBlobValid(av->blobSpriteId, av->objectEventId))
            av->blobSpriteId = MP_BLOB_NONE;
        av->hasPending = FALSE;
        if (av->avatarState == MP_AVATAR_SURFING)
            MpCreateBlob(av);
    }

    if (!gMpDebugLogging)
        return;
    for (i = 0; i < OBJECT_EVENTS_COUNT; i++)
    {
        if (gObjectEvents[i].active)
        {
            objects++;
            if (MP_IsRemoteAvatarLocalId(gObjectEvents[i].localId))
                avatars++;
        }
    }
    for (i = 0; i < NUM_TASKS; i++)
    {
        if (gTasks[i].isActive)
            tasks++;
    }
    for (i = 0; i < MAX_SPRITES; i++)
    {
        if (gSprites[i].inUse)
            sprites++;
    }
    MP_LOG("field: map %u.%u object events %u/%u (remote avatars %u), tasks %u/%u, sprites %u/%u\n",
           (unsigned)(u8)gSaveBlock1Ptr->location.mapGroup, (unsigned)(u8)gSaveBlock1Ptr->location.mapNum,
           (unsigned)objects, (unsigned)OBJECT_EVENTS_COUNT, (unsigned)avatars,
           (unsigned)tasks, (unsigned)NUM_TASKS, (unsigned)sprites, (unsigned)MAX_SPRITES);
}

// ---------------------------------------------------------------------------
// Public hooks
// ---------------------------------------------------------------------------

void MP_UpdateRemoteAvatars(void)
{
    u8 mapGroup, mapNum, budget, spawned, slot;
    bool8 allowed, scriptRunning;
    s32 i;

    MpAvatarsInit();
    if (!MpInField())
    {
        sFieldLastTick = FALSE;
        return;
    }
    if (!sFieldLastTick)
    {
        sFieldLastTick = TRUE;
        MpOnFieldEntry();
    }

    allowed = MP_IsActive() && MpAvatarsAllowedHere();
    if (!allowed)
    {
        for (slot = 0; slot < MP_MAX_PLAYERS; slot++)
        {
            if (sAvatars[slot].spawned)
                MpDespawn(slot, &sAvatars[slot], MP_IsActive() ? "map not allowed" : "not in a session");
            if (MP_IsActive())
                MpDrainQueue(slot);
        }
        return;
    }

    mapGroup = (u8)gSaveBlock1Ptr->location.mapGroup;
    mapNum = (u8)gSaveBlock1Ptr->location.mapNum;
    scriptRunning = ArePlayerFieldControlsLocked();

    // 1. Drop avatars whose player is gone, re-joined (new epoch), elsewhere or unplaceable,
    //    and forget the ones the game removed (walked out of view).
    for (slot = 0; slot < MP_MAX_PLAYERS; slot++)
    {
        struct MpAvatar *av = &sAvatars[slot];
        const struct MpPlayer *player = MP_GetPlayer(slot);
        const struct MpPresence *p;

        if (!av->spawned)
            continue;
        if (!MpAvatarObjectValid(slot, av))
        {
            MpDestroyBlob(av); // the blob would otherwise follow whatever reuses the slot
            MpAvatarResetState(av);
            MP_DLOG("avatar slot %u was removed by the game (out of view)\n", (unsigned)slot);
            continue;
        }
        if (player == NULL || player->isLocal)
        {
            MpDespawn(slot, av, "player left");
            continue;
        }
        p = &player->presence;
        if (player->epoch != av->epoch)
            MpDespawn(slot, av, "slot reassigned");
        else if (!player->hasPresence || p->mapGroup != mapGroup || p->mapNum != mapNum || p->busy == MP_BUSY_NOT_IN_FIELD)
            MpDespawn(slot, av, "player left this map");
        else if (!MpOnLayout(p->x, p->y))
            MpDespawn(slot, av, "outside the map layout");
    }

    // 2. Budget: shrink first (highest slots go first), never touch NPCs.
    budget = MpAvatar_Budget(MpCountActiveOthers(), MpCountPendingNpcs());
    spawned = MpCountSpawned();
    for (i = MP_MAX_PLAYERS - 1; i >= 0 && spawned > budget; i--)
    {
        if (sAvatars[i].spawned)
        {
            MpDespawn((u8)i, &sAvatars[i], "object budget");
            spawned--;
        }
    }

    // 3. Spawn missing avatars, replay the others.
    for (slot = 0; slot < MP_MAX_PLAYERS; slot++)
    {
        struct MpAvatar *av = &sAvatars[slot];
        const struct MpPlayer *player = MP_GetPlayer(slot);
        const struct MpPresence *p;

        if (player == NULL || player->isLocal)
            continue;
        p = &player->presence;
        if (!player->hasPresence || p->mapGroup != mapGroup || p->mapNum != mapNum
         || p->busy == MP_BUSY_NOT_IN_FIELD || !MpOnLayout(p->x, p->y))
        {
            MpDrainQueue(slot); // not shown here: keep the queue from overflowing
            continue;
        }
        if (!av->spawned)
        {
            if (spawned < budget && !scriptRunning && MpInView(p->x + MAP_OFFSET, p->y + MAP_OFFSET)
             && MpSpawn(slot, av, player))
                spawned++;
            else
                MpDrainQueue(slot);
            continue;
        }
        MpReplay(slot, av, player);
    }
}

void MP_RemoveRemoteAvatars(void)
{
    bool8 inField = MpInField();
    u32 i;

    MpAvatarsInit();
    for (i = 0; i < MP_MAX_PLAYERS; i++)
    {
        struct MpAvatar *av = &sAvatars[i];

        // Outside the field another screen owns the sprites: never destroy one there.
        if (inField && av->spawned && MpAvatarObjectValid((u8)i, av))
            MpDestroyBlob(av);
        MpAvatarResetState(av);
    }
    // Every object event with an avatar localId, tracked or not.
    for (i = 0; i < OBJECT_EVENTS_COUNT; i++)
    {
        struct ObjectEvent *obj = &gObjectEvents[i];

        if (obj->isPlayer || !MP_IsRemoteAvatarLocalId(obj->localId))
            continue;
        if (inField && obj->active)
            RemoveObjectEventById((u8)i); // destroys its sprite
        // Leave nothing of it behind, not even inactive data (like ClearObjectEvent).
        memset(obj, 0, sizeof(*obj));
        obj->localId = LOCALID_PLAYER;
        obj->mapNum = MAP_NUM(MAP_UNDEFINED);
        obj->mapGroup = MAP_GROUP(MAP_UNDEFINED);
        obj->movementActionId = MOVEMENT_ACTION_NONE;
    }
}
