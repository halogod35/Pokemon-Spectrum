// Remote avatars (PS-031): pure decision logic (see include/mp/mp_avatar.h).
// Only constant headers are included; nothing here reads or writes game state.

#include "gba/types.h"
#include "gba/defines.h"
#include "mp/mp.h"
#include "mp/mp_avatar.h"
#include "constants/global.h"
#include "constants/event_objects.h"
#include "constants/event_object_movement.h"

static s16 MpAbs(s16 v)
{
    return (v < 0) ? (s16)-v : v;
}

bool8 MP_IsRemoteAvatarLocalId(u8 localId)
{
    return localId >= MP_AVATAR_LOCAL_ID_BASE && localId < MP_AVATAR_LOCAL_ID_BASE + MP_MAX_PLAYERS;
}

u8 MpAvatar_LocalIdForSlot(u8 slot)
{
    return (u8)(MP_AVATAR_LOCAL_ID_BASE + (slot % MP_MAX_PLAYERS));
}

u8 MpAvatar_SlotForLocalId(u8 localId)
{
    return MP_IsRemoteAvatarLocalId(localId) ? (u8)(localId - MP_AVATAR_LOCAL_ID_BASE) : MP_SLOT_NONE;
}

u8 MpAvatar_Budget(u8 activeOthers, u8 pendingNpcs)
{
    s32 freeSlots = OBJECT_EVENTS_COUNT - (s32)activeOthers - (s32)pendingNpcs - MP_AVATAR_RESERVE_SLOTS;

    if (freeSlots <= 0)
        return 0;
    if (freeSlots > MP_MAX_REMOTE_AVATARS)
        return MP_MAX_REMOTE_AVATARS;
    return (u8)freeSlots;
}

u8 MpAvatar_GraphicsId(u8 gender, u8 avatarState)
{
    static const u8 sGfx[MP_AVATAR_STATE_COUNT][2] =
    {
        [MP_AVATAR_ON_FOOT]    = { OBJ_EVENT_GFX_BRENDAN_NORMAL,     OBJ_EVENT_GFX_MAY_NORMAL },
        [MP_AVATAR_MACH_BIKE]  = { OBJ_EVENT_GFX_BRENDAN_MACH_BIKE,  OBJ_EVENT_GFX_MAY_MACH_BIKE },
        [MP_AVATAR_ACRO_BIKE]  = { OBJ_EVENT_GFX_BRENDAN_ACRO_BIKE,  OBJ_EVENT_GFX_MAY_ACRO_BIKE },
        [MP_AVATAR_SURFING]    = { OBJ_EVENT_GFX_BRENDAN_SURFING,    OBJ_EVENT_GFX_MAY_SURFING },
        [MP_AVATAR_UNDERWATER] = { OBJ_EVENT_GFX_BRENDAN_UNDERWATER, OBJ_EVENT_GFX_MAY_UNDERWATER },
    };

    if (avatarState >= MP_AVATAR_STATE_COUNT)
        avatarState = MP_AVATAR_ON_FOOT;
    return sGfx[avatarState][(gender == FEMALE) ? 1 : 0];
}

u8 MpAvatar_DirectionOf(s16 dx, s16 dy)
{
    if (dx == 0 && dy == 0)
        return DIR_NONE;
    if (MpAbs(dx) >= MpAbs(dy))
        return (dx > 0) ? DIR_EAST : DIR_WEST;
    return (dy > 0) ? DIR_SOUTH : DIR_NORTH;
}

// The MOVEMENT_ACTION_* blocks are ordered DOWN, UP, LEFT, RIGHT = DIR_SOUTH..DIR_EAST.
static u8 MpDirAction(u8 firstDown, u8 dir)
{
    if (dir < DIR_SOUTH || dir > DIR_EAST)
        dir = DIR_SOUTH;
    return (u8)(firstDown + (dir - DIR_SOUTH));
}

u8 MpAvatar_StepAction(u8 movement, u8 avatarState, u8 dir)
{
    switch (movement)
    {
    case MP_MOVE_WALK_FAST:
        if (avatarState == MP_AVATAR_ON_FOOT)
            return MpDirAction(MOVEMENT_ACTION_PLAYER_RUN_DOWN, dir);
        return MpDirAction(MOVEMENT_ACTION_WALK_FAST_DOWN, dir);
    case MP_MOVE_WALK_FASTER:
        return MpDirAction(MOVEMENT_ACTION_WALK_FASTER_DOWN, dir);
    default:
        return MpDirAction(MOVEMENT_ACTION_WALK_NORMAL_DOWN, dir);
    }
}

u8 MpAvatar_Plan(s16 fromX, s16 fromY, u8 facing, const struct MpPresence *target,
                 bool8 newStep, u8 *action)
{
    s16 dx = (s16)(target->x - fromX);
    s16 dy = (s16)(target->y - fromY);
    s16 dist = (s16)(MpAbs(dx) + MpAbs(dy));
    u8 targetFacing = (target->facing >= DIR_SOUTH && target->facing <= DIR_EAST) ? target->facing : DIR_SOUTH;
    u8 dir;

    *action = MOVEMENT_ACTION_NONE;
    if (dist == 0)
    {
        if (newStep && target->movement == MP_MOVE_JUMP_IN_PLACE)
        {
            *action = (target->avatarState == MP_AVATAR_ACRO_BIKE)
                    ? MpDirAction(MOVEMENT_ACTION_ACRO_WHEELIE_HOP_FACE_DOWN, targetFacing)
                    : MpDirAction(MOVEMENT_ACTION_JUMP_IN_PLACE_DOWN, targetFacing);
            return MP_AVATAR_PLAN_ACTION;
        }
        if (facing != targetFacing)
        {
            *action = MpDirAction(MOVEMENT_ACTION_FACE_DOWN, targetFacing);
            return MP_AVATAR_PLAN_ACTION;
        }
        return MP_AVATAR_PLAN_DONE;
    }
    if (target->movement == MP_MOVE_WARP || dist > MP_AVATAR_SNAP_TILES)
        return MP_AVATAR_PLAN_SNAP;
    dir = MpAvatar_DirectionOf(dx, dy);
    if (dist == 2 && (dx == 0 || dy == 0) && target->movement == MP_MOVE_JUMP)
    {
        *action = MpDirAction(MOVEMENT_ACTION_JUMP_2_DOWN, dir);
        return MP_AVATAR_PLAN_ACTION;
    }
    // One tile (or the first tile of a missed step): walk at the update's speed.
    *action = MpAvatar_StepAction(target->movement, target->avatarState, dir);
    return MP_AVATAR_PLAN_ACTION;
}

bool8 MpAvatar_IsTooFarBehind(s16 x, s16 y, const struct MpPresence *latest)
{
    s16 dist = (s16)(MpAbs((s16)(latest->x - x)) + MpAbs((s16)(latest->y - y)));

    // A ledge jump legitimately puts the destination 2 tiles away; one more step on top of
    // it is still fine. Anything further is a snap (the card's "more than 2 tiles behind").
    if (latest->movement == MP_MOVE_JUMP)
        return dist > MP_AVATAR_SNAP_TILES + 1;
    return dist > MP_AVATAR_SNAP_TILES;
}
