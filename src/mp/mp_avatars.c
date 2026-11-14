// Remote player avatars (PS-031, edge cases PS-032): other session players drawn as object
// events on the local player's map, animated from their PRESENCE stream (PS-030).
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
//    map NPCs near the view and scripts always find a free slot. Never evict an NPC; if the
//    game still runs out of slots, MP_EvictRemoteAvatarForGame() gives one avatar's slot back
//    (hook in InitObjectEventStateFromTemplate, PS-032).
//  - Not solid, not interactive, no reflection, never repaints the player's palette: hooks in
//    event_object_movement.c. Avatars whose palette differs from the local player's (Brendan
//    seen by May, underwater...) get their own sprite palette (MpEnsurePalette).
//  - Never saved: SaveObjectEvents() leaves avatar object events out of the copy (PS-075);
//    MP_RemoveRemoteAvatars() runs when the object list is reloaded from the save (LoadObjectEvents).
//  - Lifetimes: object events are wiped by every map load (ResetObjectEvents) and keep their
//    data across battles / menus (SpawnObjectEventsOnReturnToField re-creates their sprites).
//    So on every field entry each tracked avatar is re-validated by localId + map, then
//    re-spawned or snapped. The game may also remove an avatar that walked out of view
//    (RemoveObjectEventsOutsideView); MP_OnObjectEventRemoved() then drops its surf blob and
//    palette in the same frame, and it is re-spawned when its player is back in view.
//  - PS-032 connected maps: a player on the current map or on a map directly connected to it
//    (N/S/E/W) is shown, with coordinates translated into the current map's frame; when the
//    local player crosses a seam the game shifts all object coordinates into the new map's
//    frame, so the translated targets stay continuous. Denied maps (MpAvatar_IsMapDenied)
//    show nobody and are never shown from.
//  - AD-1: nothing here changes world state; only our own object events, sprites, palettes.

#include "global.h"
#include "battle_pyramid.h"
#include "event_data.h"
#include "event_object_movement.h"
#include "field_effect.h"
#include "field_effect_helpers.h"
#include "field_player_avatar.h"
#include "field_weather.h"
#include "field_camera.h" // BUG-008: gFieldCamera (sprite alignment)
#include "field_door.h"
#include "fieldmap.h"
#include "metatile_behavior.h"
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

#define MP_BLOB_NONE            MAX_SPRITES
#define MP_PAL_NONE             0xFFFF                      // no palette tag loaded
#define MP_AVATAR_PAL_TAG(slot) (0x7AE0 + (slot))           // our sprite-palette tags
#define MP_EVICT_COOLDOWN_TICKS 120                         // no re-spawn for 2 s after an eviction

struct MpAvatar
{
    bool8 spawned;
    u8 objectEventId;
    u8 epoch;               // epoch of the player it was spawned for
    u8 mapGroup;            // map the object event belongs to (the local map at spawn time)
    u8 mapNum;
    u8 avatarState;         // MP_AVATAR_* currently shown
    u8 gender;
    u8 blobSpriteId;        // surf blob, MP_BLOB_NONE if none
    u8 lastStepId;          // stepId of the last update the avatar finished
    bool8 hasPending;
    u16 palTag;             // object palette tag loaded into our own sprite palette, MP_PAL_NONE if none
    u8 doorState;           // PS-036: MP_AVATAR_DOOR_* of the step in progress
    struct MpPresence pending;  // already translated into the current map's frame
};

// PS-036: a door this module opened for an avatar (local cosmetics only: the door animation
// draws BG tiles, it never changes the map). Kept apart from MpAvatar so a door still gets
// closed after its avatar is gone.
struct MpDoor
{
    bool8 open;
    bool8 closeRequested;
    u8 waitTicks;
    s16 x;                  // map coordinates with MAP_OFFSET (what FieldAnimateDoor* take)
    s16 y;
};

#define MP_DOOR_CLOSE_TIMEOUT 60    // ticks to wait for another door animation before forcing closed

static struct MpAvatar sAvatars[MP_MAX_PLAYERS];
static struct MpDoor sDoors[MP_MAX_PLAYERS];
static bool8 sAvatarsInit;
static bool8 sFieldLastTick;
static u8 sEvictCooldown;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void MpAvatarResetState(struct MpAvatar *av)
{
    memset(av, 0, sizeof(*av));
    av->objectEventId = OBJECT_EVENTS_COUNT;
    av->blobSpriteId = MP_BLOB_NONE;
    av->palTag = MP_PAL_NONE;
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

// Avatars are never shown while the local player is on one of these maps.
static bool8 MpAvatarsAllowedHere(void)
{
    if (gMapHeader.events == NULL || gMapHeader.mapLayout == NULL)
        return FALSE;
    if (IsOverworldLinkActive())
        return FALSE;
    if (CurrentBattlePyramidLocation() != PYRAMID_LOCATION_NONE || InTrainerHill())
        return FALSE;
    return !MpAvatar_IsMapDenied((u8)gSaveBlock1Ptr->location.mapGroup, (u8)gSaveBlock1Ptr->location.mapNum);
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

static void MpFreePalette(u8 slot, struct MpAvatar *av)
{
    if (IndexOfSpritePaletteTag(MP_AVATAR_PAL_TAG(slot)) != 0xFF)
        FreeSpritePaletteByTag(MP_AVATAR_PAL_TAG(slot));
    av->palTag = MP_PAL_NONE;
}

// PS-032: an avatar shares the player palette slot only if it needs the same colours as the
// local player; otherwise it gets its own sprite palette (the hooks in event_object_movement.c
// make sure the game never repaints the player slot for an avatar).
static void MpEnsurePalette(u8 slot, struct MpAvatar *av)
{
    struct ObjectEvent *obj = &gObjectEvents[av->objectEventId];
    struct Sprite *sprite = &gSprites[obj->spriteId];
    const struct ObjectEventGraphicsInfo *info = GetObjectEventGraphicsInfo(obj->graphicsId);
    u16 localTag = MP_PAL_NONE;
    u8 index;

    if (info->paletteSlot != PALSLOT_PLAYER)
        return; // not a player sprite: the game's own palette handling applies
    if (gPlayerAvatar.objectEventId < OBJECT_EVENTS_COUNT && gObjectEvents[gPlayerAvatar.objectEventId].active)
        localTag = GetObjectEventGraphicsInfo(gObjectEvents[gPlayerAvatar.objectEventId].graphicsId)->paletteTag;
    if (info->paletteTag == localTag)
    {
        sprite->oam.paletteNum = PALSLOT_PLAYER;
        if (av->palTag != MP_PAL_NONE)
            MpFreePalette(slot, av);
        return;
    }
    index = IndexOfSpritePaletteTag(MP_AVATAR_PAL_TAG(slot));
    if (index == 0xFF)
    {
        index = AllocSpritePalette(MP_AVATAR_PAL_TAG(slot));
        av->palTag = MP_PAL_NONE; // fresh slot: must be filled
    }
    if (index == 0xFF)
    {
        // No free sprite palette: wrong colours, but never anyone else's palette touched.
        sprite->oam.paletteNum = PALSLOT_PLAYER;
        return;
    }
    if (av->palTag != info->paletteTag)
    {
        PatchObjectPalette(info->paletteTag, index);
        UpdateSpritePaletteWithWeather(index);
        av->palTag = info->paletteTag;
    }
    sprite->oam.paletteNum = index;
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

static u8 MpElevationAt(s16 x, s16 y)
{
    return MapGridGetElevationAt(x + MAP_OFFSET, y + MAP_OFFSET);
}

// PS-032: `in` translated into the current map's frame in `out` (map set to the current map).
// FALSE if the player is on a denied map or on a map that is neither the current one nor
// directly connected to it.
static bool8 MpTranslate(const struct MpPresence *in, struct MpPresence *out)
{
    u8 curGroup = (u8)gSaveBlock1Ptr->location.mapGroup;
    u8 curNum = (u8)gSaveBlock1Ptr->location.mapNum;
    const struct MapConnections *connections = gMapHeader.connections;
    s32 i;

    if (in->busy == MP_BUSY_NOT_IN_FIELD || !MP_IsValidMap(in->mapGroup, in->mapNum)
     || MpAvatar_IsMapDenied(in->mapGroup, in->mapNum))
        return FALSE;
    *out = *in;
    out->mapGroup = curGroup;
    out->mapNum = curNum;
    if (in->mapGroup == curGroup && in->mapNum == curNum)
        return TRUE;
    if (connections == NULL || connections->connections == NULL)
        return FALSE;
    for (i = 0; i < connections->count && i < 16; i++)
    {
        const struct MapConnection *c = &connections->connections[i];
        const struct MapHeader *header;

        if (c->mapGroup != in->mapGroup || c->mapNum != in->mapNum)
            continue;
        header = GetMapHeaderFromConnection(c);
        if (header == NULL || header->mapLayout == NULL)
            continue;
        if (MpAvatar_TranslateConnection(c->direction, (s32)c->offset,
                                         gMapHeader.mapLayout->width, gMapHeader.mapLayout->height,
                                         header->mapLayout->width, header->mapLayout->height,
                                         in->x, in->y, &out->x, &out->y))
            return TRUE;
    }
    return FALSE;
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

static u8 MpFacingOf(const struct MpPresence *p)
{
    return (p->facing >= DIR_SOUTH && p->facing <= DIR_EAST) ? p->facing : DIR_SOUTH;
}

// ---------------------------------------------------------------------------
// PS-036: doors (local cosmetics)
// ---------------------------------------------------------------------------

// Map-local tile (x, y) is an animated warp door.
static bool8 MpIsWarpDoorAt(s16 x, s16 y)
{
    return MetatileBehavior_IsWarpDoor(MapGridGetMetatileBehaviorAt(x + MAP_OFFSET, y + MAP_OFFSET));
}

// The local player stands on the door tile or right below it: they may be using that door
// themselves (their own door warp / exit animates it), so leave it alone.
static bool8 MpLocalPlayerAtDoor(s16 doorX, s16 doorY)
{
    const struct ObjectEvent *player;

    if (gPlayerAvatar.objectEventId >= OBJECT_EVENTS_COUNT)
        return TRUE;
    player = &gObjectEvents[gPlayerAvatar.objectEventId];
    return player->currentCoords.x == doorX && (player->currentCoords.y == doorY || player->currentCoords.y == doorY + 1);
}

static void MpDoorOpen(u8 slot, s16 x, s16 y)
{
    struct MpDoor *door = &sDoors[slot];
    s16 mx = x + MAP_OFFSET, my = y + MAP_OFFSET;

    if (door->open && door->x == mx && door->y == my)
    {
        door->closeRequested = FALSE; // reuse the open door
        return;
    }
    // Only one door animation can run at a time (field_door.c). Never start one while
    // another runs or at a door the local player is at, so the local player's own door
    // warp always gets its animation and is never delayed by ours.
    if (door->open || FieldIsDoorAnimationRunning() || MpLocalPlayerAtDoor(mx, my))
        return;
    if (FieldAnimateDoorOpen(mx, my) < 0)
        return;
    door->open = TRUE;
    door->closeRequested = FALSE;
    door->waitTicks = 0;
    door->x = mx;
    door->y = my;
}

static void MpDoorRequestClose(u8 slot)
{
    if (sDoors[slot].open)
    {
        sDoors[slot].closeRequested = TRUE;
        sDoors[slot].waitTicks = 0;
    }
}

static void MpDoorsUpdate(void)
{
    u32 i;

    for (i = 0; i < MP_MAX_PLAYERS; i++)
    {
        struct MpDoor *door = &sDoors[i];

        if (!door->open || !door->closeRequested)
            continue;
        if (MpLocalPlayerAtDoor(door->x, door->y))
        {
            door->open = FALSE; // the local player's own door sequence draws it from here
            continue;
        }
        if (!FieldIsDoorAnimationRunning())
        {
            if (FieldAnimateDoorClose(door->x, door->y) < 0)
                FieldSetDoorClosed(door->x, door->y);
            door->open = FALSE;
        }
        else if (++door->waitTicks >= MP_DOOR_CLOSE_TIMEOUT)
        {
            FieldSetDoorClosed(door->x, door->y);
            door->open = FALSE;
        }
    }
}

// Called before an avatar starts a 1-tile step from (x, y) to (destX, destY).
static void MpDoorBeforeStep(u8 slot, struct MpAvatar *av, s16 x, s16 y, s16 destX, s16 destY)
{
    struct ObjectEvent *obj = &gObjectEvents[av->objectEventId];
    u8 dir = MpAvatar_DirectionOf((s16)(destX - x), (s16)(destY - y));

    av->doorState = MpAvatar_DoorTransition(dir, MpIsWarpDoorAt(x, y), MpIsWarpDoorAt(destX, destY));
    if (av->doorState == MP_AVATAR_DOOR_ENTER)
    {
        MpDoorOpen(slot, destX, destY);
    }
    else if (av->doorState == MP_AVATAR_DOOR_EXIT)
    {
        obj->invisible = FALSE;
        MpDoorOpen(slot, x, y);
    }
    else if (obj->invisible && !MpIsWarpDoorAt(destX, destY))
    {
        obj->invisible = FALSE; // left a door some other way
    }
}

// Called when the avatar's step has finished.
static void MpDoorAfterStep(u8 slot, struct MpAvatar *av)
{
    if (av->doorState == MP_AVATAR_DOOR_ENTER)
    {
        gObjectEvents[av->objectEventId].invisible = TRUE; // went inside; the warp follows
        MpDoorRequestClose(slot);
    }
    else if (av->doorState == MP_AVATAR_DOOR_EXIT)
    {
        MpDoorRequestClose(slot);
    }
    av->doorState = MP_AVATAR_DOOR_NONE;
}

// ---------------------------------------------------------------------------
// Spawn / despawn / snap
// ---------------------------------------------------------------------------

static void MpDespawn(u8 slot, struct MpAvatar *av, const char *why)
{
    if (av->spawned && MpAvatarObjectValid(slot, av))
    {
        MpDestroyBlob(av);
        MpFreePalette(slot, av);
        RemoveObjectEventById(av->objectEventId);
        MP_DLOG("avatar slot %u removed (%s)\n", (unsigned)slot, why);
    }
    MpDoorRequestClose(slot); // a door opened for it must not stay drawn open
    MpAvatarResetState(av);
}

static void MpSnap(struct MpAvatar *av, const struct MpPresence *to)
{
    struct ObjectEvent *obj = &gObjectEvents[av->objectEventId];
    u8 elevation = MpElevationAt(to->x, to->y);

    // BUG-008: MoveObjectEventToMapCoords sets the coords and the sprite position but leaves
    // a held movement running. A step that kept running after the snap moved the sprite by
    // its remaining pixels and left it off the grid (half a tile) for good, because the
    // coords already said "arrived". Stop it first, and clear the jump / hop offset.
    ObjectEventClearHeldMovementIfActive(obj);
    MoveObjectEventToMapCoords(obj, to->x + MAP_OFFSET, to->y + MAP_OFFSET);
    gSprites[obj->spriteId].x2 = 0;
    gSprites[obj->spriteId].y2 = 0;
    obj->currentElevation = elevation;
    obj->previousElevation = elevation;
    ObjectEventTurn(obj, MpFacingOf(to));
    // PS-036: a player standing on a door tile is mid door-transition (just arrived, or just
    // walked in): hidden on their own screen, so hidden here too until they step off.
    obj->invisible = MpIsWarpDoorAt(to->x, to->y);
    av->doorState = MP_AVATAR_DOOR_NONE;
    av->hasPending = FALSE;
    av->lastStepId = to->stepId;
}

static void MpSetAvatarState(u8 slot, struct MpAvatar *av, u8 avatarState)
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
    MpEnsurePalette(slot, av);
}

// `p` is the player's latest presence, already translated into the current map's frame.
static bool8 MpSpawn(u8 slot, struct MpAvatar *av, const struct MpPlayer *player, const struct MpPresence *p)
{
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
    ObjectEventTurn(&gObjectEvents[id], MpFacingOf(p));
    gObjectEvents[id].invisible = MpIsWarpDoorAt(p->x, p->y); // PS-036: arriving through a door
    MpSetAvatarState(slot, av, avatarState);
    MpDrainQueue(slot); // it starts at the latest position
    MP_DLOG("avatar slot %u spawned as object %u at (%d,%d) on map %u.%u\n", (unsigned)slot, (unsigned)id,
            (int)p->x, (int)p->y, (unsigned)av->mapGroup, (unsigned)av->mapNum);
    return TRUE;
}

// ---------------------------------------------------------------------------
// Replay
// ---------------------------------------------------------------------------

static s16 MpTilesBetween(s16 x, s16 y, const struct MpPresence *p)
{
    s16 dx = (s16)(p->x - x);
    s16 dy = (s16)(p->y - y);

    return (s16)(((dx < 0) ? -dx : dx) + ((dy < 0) ? -dy : dy));
}

// BUG-008: TRUE if the (standing) avatar's sprite sits exactly where
// MoveObjectEventToMapCoords puts an object on its currentCoords (x2/y2 are not judged: a
// surfing rider bobs with its blob), and
// previousCoords == currentCoords. Only judged while the local camera is at rest (between
// tiles the camera's sub-tile offset is still being applied), otherwise assumed aligned.
static bool8 MpSpriteAligned(const struct ObjectEvent *obj)
{
    const struct Sprite *sprite = &gSprites[obj->spriteId];
    const struct ObjectEventGraphicsInfo *info = GetObjectEventGraphicsInfo(obj->graphicsId);
    s16 ex, ey;

    if (obj->previousCoords.x != obj->currentCoords.x || obj->previousCoords.y != obj->currentCoords.y)
        return FALSE;
    if (gFieldCamera.x != 0 || gFieldCamera.y != 0)
        return TRUE;
    SetSpritePosToMapCoords(obj->currentCoords.x, obj->currentCoords.y, &ex, &ey);
    ex += 8;
    ey += 16 - (info->height >> 1);
    return sprite->x == ex && sprite->y == ey;
}

// BUG-008: the avatar is standing and its queue is empty. If its player is idle, converge on
// exactly the reported tile: turn, or a short walk from an adjacent tile, otherwise a snap;
// on the tile, make sure the sprite is on the grid. This also recovers a lost final step
// (the next keep-alive, or the latest presence itself, carries the tile).
static void MpConverge(u8 slot, struct MpAvatar *av, const struct MpPresence *latest, s16 x, s16 y)
{
    struct ObjectEvent *obj = &gObjectEvents[av->objectEventId];
    u8 plan, action;

    if (!MpAvatar_IsIdlePresence(latest))
        return;
    MpSetAvatarState(slot, av, latest->avatarState);
    plan = MpAvatar_PlanIdle(x, y, obj->facingDirection, latest, &action);
    if (plan == MP_AVATAR_PLAN_SNAP)
    {
        MpSnap(av, latest);
        MP_DLOG("avatar slot %u idle off its tile, snapped to (%d,%d)\n", (unsigned)slot, (int)latest->x, (int)latest->y);
    }
    else if (plan == MP_AVATAR_PLAN_ACTION)
    {
        av->lastStepId = latest->stepId;
        if (latest->x != x || latest->y != y)
        {
            MpDoorBeforeStep(slot, av, x, y, latest->x, latest->y);
            MP_DLOG("avatar slot %u idle one tile off, walking to (%d,%d)\n", (unsigned)slot, (int)latest->x, (int)latest->y);
        }
        ObjectEventSetHeldMovement(obj, action);
    }
    else if (!MpSpriteAligned(obj))
    {
        MpSnap(av, latest);
        MP_DLOG("avatar slot %u sprite off the grid, re-aligned on (%d,%d)\n", (unsigned)slot, (int)latest->x, (int)latest->y);
    }
}

// `latest` is the player's latest presence translated into the current map's frame.
static void MpReplay(u8 slot, struct MpAvatar *av, const struct MpPlayer *player, const struct MpPresence *latest)
{
    struct ObjectEvent *obj = &gObjectEvents[av->objectEventId];
    s16 x, y;
    struct MpPresence raw;
    u8 plan, action;
    u32 guard;

    if (obj->frozen)
        return; // defensive: FreezeObjectEvent never freezes avatars since PS-035
    // BUG-008: nothing (not even a snap) starts before the current step has finished. With
    // internet jitter, updates arrive in bursts and the avatar is often more than 2 tiles
    // behind while mid-step; snapping then left the sprite half a tile off. A step is at
    // most 32 frames, so a deferred snap is at most that late.
    if (obj->heldMovementActive)
    {
        if (!obj->heldMovementFinished)
            return; // still stepping
        ObjectEventClearHeldMovement(obj);
        MpDoorAfterStep(slot, av); // PS-036: hide inside a door / close it behind
    }
    x = obj->currentCoords.x - MAP_OFFSET;
    y = obj->currentCoords.y - MAP_OFFSET;

    if (player->presenceOverflow || MpAvatar_IsTooFarBehind(x, y, latest))
    {
        MpDrainQueue(slot);
        MpSnap(av, latest);
        MpSetAvatarState(slot, av, latest->avatarState);
        MP_DLOG("avatar slot %u snapped to (%d,%d)\n", (unsigned)slot, (int)latest->x, (int)latest->y);
        return;
    }

    for (guard = 0; guard < MP_PRESENCE_QUEUE_SIZE + 1; guard++)
    {
        if (!av->hasPending)
        {
            if (!MP_PopRemotePresence(slot, &raw))
            {
                MpConverge(slot, av, latest, x, y); // BUG-008: nothing left to replay
                return;
            }
            // Queued on a map that is not visible from here (before / after a visit
            // elsewhere): not replayable, skip it.
            if (!MpTranslate(&raw, &av->pending))
                continue;
            av->hasPending = TRUE;
        }
        MpSetAvatarState(slot, av, av->pending.avatarState);
        plan = MpAvatar_Plan(x, y, obj->facingDirection, &av->pending, av->pending.stepId != av->lastStepId, &action);
        if (plan == MP_AVATAR_PLAN_SNAP)
        {
            MpSnap(av, &av->pending);
            return;
        }
        if (plan == MP_AVATAR_PLAN_ACTION)
        {
            av->lastStepId = av->pending.stepId;
            // PS-036: a 1-tile step (not a turn, hop or ledge jump) may go through a door.
            if ((av->pending.x != x || av->pending.y != y)
             && !(action >= MOVEMENT_ACTION_JUMP_2_DOWN && action < MOVEMENT_ACTION_JUMP_2_DOWN + 4))
            {
                s16 destX, destY;

                MpAvatar_StepCoords(MpAvatar_DirectionOf((s16)(av->pending.x - x), (s16)(av->pending.y - y)),
                                    x, y, 1, &destX, &destY);
                MpDoorBeforeStep(slot, av, x, y, destX, destY);
                // BUG-008: bounded catch-up after a burst (a late normal walk goes fast).
                action = MpAvatar_CatchUpAction(action, MpTilesBetween(x, y, latest));
            }
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

    // PS-036: every field entry redraws the map view from the map grid (doors closed), so
    // no door of ours is still drawn open.
    memset(sDoors, 0, sizeof(sDoors));
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
        // Return from battle / menu: the sprite was re-created (with the player palette
        // slot), the surf blob and our sprite palette were not, unless the sprite system
        // survived, which MpBlobValid / IndexOfSpritePaletteTag detect.
        if (!MpBlobValid(av->blobSpriteId, av->objectEventId))
            av->blobSpriteId = MP_BLOB_NONE;
        if (IndexOfSpritePaletteTag(MP_AVATAR_PAL_TAG(i)) == 0xFF)
            av->palTag = MP_PAL_NONE;
        av->hasPending = FALSE;
        MpSetAvatarState((u8)i, av, av->avatarState);
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
    struct MpPresence latest[MP_MAX_PLAYERS];
    bool8 visible[MP_MAX_PLAYERS];
    u8 budget, spawned, slot;
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
    MpDoorsUpdate(); // PS-036: pending door closes, also after the avatars are gone
    if (sEvictCooldown != 0)
        sEvictCooldown--;

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
    scriptRunning = ArePlayerFieldControlsLocked();

    // Where every remote player is, seen from the current map (PS-032: same or connected).
    for (slot = 0; slot < MP_MAX_PLAYERS; slot++)
    {
        const struct MpPlayer *player = MP_GetPlayer(slot);

        visible[slot] = player != NULL && !player->isLocal && player->hasPresence
                     && MpTranslate(&player->presence, &latest[slot]);
    }

    // 1. Drop avatars whose player is gone, re-joined (new epoch) or no longer visible from
    //    here, and forget the ones the game removed (walked out of view).
    for (slot = 0; slot < MP_MAX_PLAYERS; slot++)
    {
        struct MpAvatar *av = &sAvatars[slot];
        const struct MpPlayer *player = MP_GetPlayer(slot);

        if (!av->spawned)
            continue;
        if (!MpAvatarObjectValid(slot, av))
        {
            MpDestroyBlob(av); // normally already done by MP_OnObjectEventRemoved
            MpFreePalette(slot, av);
            MpAvatarResetState(av);
            MP_DLOG("avatar slot %u was removed by the game (out of view)\n", (unsigned)slot);
            continue;
        }
        if (player == NULL || player->isLocal)
            MpDespawn(slot, av, "player left");
        else if (player->epoch != av->epoch)
            MpDespawn(slot, av, "slot reassigned");
        else if (!visible[slot])
            MpDespawn(slot, av, "player not on this or a connected map");
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

        if (player == NULL || player->isLocal)
            continue;
        if (!visible[slot])
        {
            MpDrainQueue(slot); // not shown here: keep the queue from overflowing
            continue;
        }
        if (!av->spawned)
        {
            if (spawned < budget && !scriptRunning && sEvictCooldown == 0
             && MpInView(latest[slot].x + MAP_OFFSET, latest[slot].y + MAP_OFFSET)
             && MpSpawn(slot, av, player, &latest[slot]))
                spawned++;
            else
                MpDrainQueue(slot);
            continue;
        }
        MpReplay(slot, av, player, &latest[slot]);
        MpEnsurePalette(slot, av); // the local player may have changed palette (e.g. underwater)
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
        {
            MpDestroyBlob(av);
            MpFreePalette((u8)i, av);
        }
        if (inField)
            MpDoorRequestClose((u8)i); // closed on the next field tick
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

bool8 MP_EvictRemoteAvatarForGame(void)
{
    s32 i;

    MpAvatarsInit();
    for (i = MP_MAX_PLAYERS - 1; i >= 0; i--)
    {
        struct MpAvatar *av = &sAvatars[i];

        if (!av->spawned || !MpAvatarObjectValid((u8)i, av))
            continue;
        if (MpInField())
        {
            MpDespawn((u8)i, av, "the game needed the object slot");
        }
        else
        {
            gObjectEvents[av->objectEventId].active = FALSE;
            MpAvatarResetState(av);
        }
        sEvictCooldown = MP_EVICT_COOLDOWN_TICKS;
        return TRUE;
    }
    return FALSE;
}

// MP: called from RemoveObjectEvent (event_object_movement.c) for avatar localIds, before the
// object's sprite is destroyed: whoever removes an avatar (us, or RemoveObjectEventsOutsideView),
// its surf blob and palette go in the same frame, so the blob never follows a reused slot.
void MP_OnObjectEventRemoved(u8 objectEventId)
{
    u32 i;

    MpAvatarsInit();
    for (i = 0; i < MP_MAX_PLAYERS; i++)
    {
        struct MpAvatar *av = &sAvatars[i];

        if (!av->spawned || av->objectEventId != objectEventId)
            continue;
        MpDestroyBlob(av);
        if (MpInField())
        {
            MpFreePalette((u8)i, av);
            MpDoorRequestClose((u8)i);
        }
        MpAvatarResetState(av);
    }
}

// PS-041 (Agent D): A-button interaction. The only lookup of avatars by position; normal NPC
// interaction (GetObjectEventIdByPosition) still never sees them. Read only.
u8 MP_GetRemoteAvatarSlotAt(s16 x, s16 y)
{
    u32 i;

    if (!sAvatarsInit || !MpInField())
        return MP_SLOT_NONE;
    for (i = 0; i < MP_MAX_PLAYERS; i++)
    {
        const struct MpAvatar *av = &sAvatars[i];
        const struct ObjectEvent *obj;

        if (!av->spawned || !MpAvatarObjectValid((u8)i, av))
            continue;
        obj = &gObjectEvents[av->objectEventId];
        if (obj->currentCoords.x == x && obj->currentCoords.y == y && !obj->invisible)
            return (u8)i;
    }
    return MP_SLOT_NONE;
}
