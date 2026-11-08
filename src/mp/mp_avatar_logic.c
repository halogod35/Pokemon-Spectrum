// Remote avatars (PS-031): pure decision logic (see include/mp/mp_avatar.h).
// Only constant headers are included; nothing here reads or writes game state.

#include "gba/types.h"
#include "gba/defines.h"
#include "mp/mp.h"
#include "mp/mp_avatar.h"
#include "constants/global.h"
#include "constants/event_objects.h"
#include "constants/event_object_movement.h"
#include "constants/maps.h"

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

// ---------------------------------------------------------------------------
// BUG-008: idle convergence and bounded catch-up
// ---------------------------------------------------------------------------

bool8 MpAvatar_IsIdlePresence(const struct MpPresence *p)
{
    return p->movement == MP_MOVE_NONE || p->movement == MP_MOVE_TURN || p->movement == MP_MOVE_JUMP_IN_PLACE;
}

u8 MpAvatar_PlanIdle(s16 x, s16 y, u8 facing, const struct MpPresence *latest, u8 *action)
{
    s16 dx = (s16)(latest->x - x);
    s16 dy = (s16)(latest->y - y);
    s16 dist = (s16)(MpAbs(dx) + MpAbs(dy));
    u8 targetFacing = (latest->facing >= DIR_SOUTH && latest->facing <= DIR_EAST) ? latest->facing : DIR_SOUTH;

    *action = MOVEMENT_ACTION_NONE;
    if (dist == 0)
    {
        if (facing == targetFacing)
            return MP_AVATAR_PLAN_DONE;
        *action = MpDirAction(MOVEMENT_ACTION_FACE_DOWN, targetFacing);
        return MP_AVATAR_PLAN_ACTION;
    }
    if (dist == 1)
    {
        *action = MpDirAction(MOVEMENT_ACTION_WALK_NORMAL_DOWN, MpAvatar_DirectionOf(dx, dy));
        return MP_AVATAR_PLAN_ACTION;
    }
    return MP_AVATAR_PLAN_SNAP;
}

static bool8 MpInBlock(u8 action, u8 firstDown)
{
    return action >= firstDown && action < firstDown + 4;
}

u8 MpAvatar_CatchUpAction(u8 action, s16 behind)
{
    if (behind < 2)
        return action;
    if (MpInBlock(action, MOVEMENT_ACTION_WALK_NORMAL_DOWN))
        return (u8)(MOVEMENT_ACTION_WALK_FAST_DOWN + (action - MOVEMENT_ACTION_WALK_NORMAL_DOWN));
    return action;
}

u8 MpAvatar_ActionFrames(u8 action)
{
    if (MpInBlock(action, MOVEMENT_ACTION_WALK_NORMAL_DOWN))
        return 16;
    if (MpInBlock(action, MOVEMENT_ACTION_WALK_FAST_DOWN) || MpInBlock(action, MOVEMENT_ACTION_PLAYER_RUN_DOWN))
        return 8;
    if (MpInBlock(action, MOVEMENT_ACTION_WALK_FASTER_DOWN))
        return 4;
    if (MpInBlock(action, MOVEMENT_ACTION_JUMP_2_DOWN))
        return 32;
    if (MpInBlock(action, MOVEMENT_ACTION_JUMP_IN_PLACE_DOWN)
     || MpInBlock(action, MOVEMENT_ACTION_ACRO_WHEELIE_HOP_FACE_DOWN))
        return 16;
    return 1; // FACE_*
}

// ---------------------------------------------------------------------------
// PS-032: connected maps
// ---------------------------------------------------------------------------

bool8 MpAvatar_TranslateConnection(u8 direction, s32 offset, s32 curWidth, s32 curHeight,
                                   s32 connWidth, s32 connHeight, s16 x, s16 y, s16 *outX, s16 *outY)
{
    s32 tx, ty;

    // Same layout as fieldmap.c Fill*Connection: the connected map's (0,0) sits at
    // south (offset, curHeight), north (offset, -connHeight), west (-connWidth, offset)
    // and east (curWidth, offset) in current-map coordinates.
    switch (direction)
    {
    case CONNECTION_SOUTH:
        tx = x + offset;
        ty = y + curHeight;
        break;
    case CONNECTION_NORTH:
        tx = x + offset;
        ty = y - connHeight;
        break;
    case CONNECTION_WEST:
        tx = x - connWidth;
        ty = y + offset;
        break;
    case CONNECTION_EAST:
        tx = x + curWidth;
        ty = y + offset;
        break;
    default:
        return FALSE; // CONNECTION_DIVE / EMERGE / NONE / INVALID: not side by side
    }
    if (tx < -0x4000 || tx > 0x4000 || ty < -0x4000 || ty > 0x4000)
        return FALSE;
    *outX = (s16)tx;
    *outY = (s16)ty;
    return TRUE;
}

// ---------------------------------------------------------------------------
// PS-032: maps where remote avatars are never shown (and from which a player is never
// shown on a connected map). Each block says why.
// ---------------------------------------------------------------------------

static const u16 sDeniedMaps[] =
{
    // Link rooms (Cable Club / Union Room). They use the GBA link player objects, which the
    // PC port stubs, and their scripts assume only link partners stand there.
    MAP_UNION_ROOM,
    MAP_TRADE_CENTER,
    MAP_RECORD_CORNER,
    MAP_BATTLE_COLOSSEUM_2P,
    MAP_BATTLE_COLOSSEUM_4P,

    // Battle Frontier facility interiors and Battle Tent challenge rooms. Fixed-camera,
    // fully scripted challenge sequences (the player is walked from room to room, with warps
    // between battles), so another player's position means nothing to you. The lobbies,
    // lounges, Pokemon Center, mart and the outside areas stay allowed.
    MAP_BATTLE_FRONTIER_BATTLE_TOWER_ELEVATOR,
    MAP_BATTLE_FRONTIER_BATTLE_TOWER_CORRIDOR,
    MAP_BATTLE_FRONTIER_BATTLE_TOWER_BATTLE_ROOM,
    MAP_BATTLE_FRONTIER_BATTLE_TOWER_MULTI_PARTNER_ROOM,
    MAP_BATTLE_FRONTIER_BATTLE_TOWER_MULTI_CORRIDOR,
    MAP_BATTLE_FRONTIER_BATTLE_TOWER_MULTI_BATTLE_ROOM,
    MAP_BATTLE_FRONTIER_BATTLE_DOME_CORRIDOR,
    MAP_BATTLE_FRONTIER_BATTLE_DOME_PRE_BATTLE_ROOM,
    MAP_BATTLE_FRONTIER_BATTLE_DOME_BATTLE_ROOM,
    MAP_BATTLE_FRONTIER_BATTLE_PALACE_CORRIDOR,
    MAP_BATTLE_FRONTIER_BATTLE_PALACE_BATTLE_ROOM,
    MAP_BATTLE_FRONTIER_BATTLE_ARENA_CORRIDOR,
    MAP_BATTLE_FRONTIER_BATTLE_ARENA_BATTLE_ROOM,
    MAP_BATTLE_FRONTIER_BATTLE_FACTORY_PRE_BATTLE_ROOM,
    MAP_BATTLE_FRONTIER_BATTLE_FACTORY_BATTLE_ROOM,
    MAP_BATTLE_FRONTIER_BATTLE_PIKE_CORRIDOR,
    MAP_BATTLE_FRONTIER_BATTLE_PIKE_THREE_PATH_ROOM,
    MAP_BATTLE_FRONTIER_BATTLE_PIKE_ROOM_NORMAL,
    MAP_BATTLE_FRONTIER_BATTLE_PIKE_ROOM_FINAL,
    MAP_BATTLE_FRONTIER_BATTLE_PIKE_ROOM_WILD_MONS,
    MAP_SLATEPORT_CITY_BATTLE_TENT_CORRIDOR,
    MAP_SLATEPORT_CITY_BATTLE_TENT_BATTLE_ROOM,
    MAP_VERDANTURF_TOWN_BATTLE_TENT_CORRIDOR,
    MAP_VERDANTURF_TOWN_BATTLE_TENT_BATTLE_ROOM,
    MAP_FALLARBOR_TOWN_BATTLE_TENT_CORRIDOR,
    MAP_FALLARBOR_TOWN_BATTLE_TENT_BATTLE_ROOM,

    // Per-save generated layouts: the Battle Pyramid floors and Trainer Hill floors are built
    // from each save's own challenge data, and secret bases from each save's own base and
    // decorations. The same map id shows different rooms in different games.
    MAP_BATTLE_FRONTIER_BATTLE_PYRAMID_FLOOR,
    MAP_BATTLE_FRONTIER_BATTLE_PYRAMID_TOP,
    MAP_TRAINER_HILL_1F,
    MAP_TRAINER_HILL_2F,
    MAP_TRAINER_HILL_3F,
    MAP_TRAINER_HILL_4F,
    MAP_TRAINER_HILL_ROOF,
    MAP_TRAINER_HILL_ELEVATOR,
    MAP_SECRET_BASE_RED_CAVE1, MAP_SECRET_BASE_BROWN_CAVE1, MAP_SECRET_BASE_BLUE_CAVE1,
    MAP_SECRET_BASE_YELLOW_CAVE1, MAP_SECRET_BASE_TREE1, MAP_SECRET_BASE_SHRUB1,
    MAP_SECRET_BASE_RED_CAVE2, MAP_SECRET_BASE_BROWN_CAVE2, MAP_SECRET_BASE_BLUE_CAVE2,
    MAP_SECRET_BASE_YELLOW_CAVE2, MAP_SECRET_BASE_TREE2, MAP_SECRET_BASE_SHRUB2,
    MAP_SECRET_BASE_RED_CAVE3, MAP_SECRET_BASE_BROWN_CAVE3, MAP_SECRET_BASE_BLUE_CAVE3,
    MAP_SECRET_BASE_YELLOW_CAVE3, MAP_SECRET_BASE_TREE3, MAP_SECRET_BASE_SHRUB3,
    MAP_SECRET_BASE_RED_CAVE4, MAP_SECRET_BASE_BROWN_CAVE4, MAP_SECRET_BASE_BLUE_CAVE4,
    MAP_SECRET_BASE_YELLOW_CAVE4, MAP_SECRET_BASE_TREE4, MAP_SECRET_BASE_SHRUB4,

    // Contest stages: the contest runs as a fixed-camera cutscene with scripted
    // contestants on the stage. The Lilycove contest lobby stays allowed.
    MAP_LILYCOVE_CITY_CONTEST_HALL,
    MAP_CONTEST_HALL,
    MAP_CONTEST_HALL_BEAUTY,
    MAP_CONTEST_HALL_COOL,
    MAP_CONTEST_HALL_CUTE,
    MAP_CONTEST_HALL_SMART,
    MAP_CONTEST_HALL_TOUGH,
    MAP_UNUSED_CONTEST_HALL1,
    MAP_UNUSED_CONTEST_HALL2,
    MAP_UNUSED_CONTEST_HALL3,
    MAP_UNUSED_CONTEST_HALL4,
    MAP_UNUSED_CONTEST_HALL5,
    MAP_UNUSED_CONTEST_HALL6,

    // The intro truck: fixed camera, scripted, before the player has a real position.
    MAP_INSIDE_OF_TRUCK,

    // Not denied, on purpose: the Safari Zone (ordinary walking areas; seeing each other
    // there is fun and harmless), the cable car stations (the ride itself is a separate
    // screen, not the overworld), the Battle Frontier outside areas and lobbies.
};

bool8 MpAvatar_IsMapDenied(u8 mapGroup, u8 mapNum)
{
    u16 id = (u16)(mapNum | (mapGroup << 8));
    u32 i;

    for (i = 0; i < sizeof(sDeniedMaps) / sizeof(sDeniedMaps[0]); i++)
    {
        if (sDeniedMaps[i] == id)
            return TRUE;
    }
    return FALSE;
}

void MpAvatar_StepCoords(u8 dir, s16 x, s16 y, s16 tiles, s16 *outX, s16 *outY)
{
    switch (dir)
    {
    case DIR_SOUTH: y = (s16)(y + tiles); break;
    case DIR_NORTH: y = (s16)(y - tiles); break;
    case DIR_WEST:  x = (s16)(x - tiles); break;
    case DIR_EAST:  x = (s16)(x + tiles); break;
    }
    *outX = x;
    *outY = y;
}

u8 MpAvatar_DoorTransition(u8 dir, bool8 fromIsDoor, bool8 destIsDoor)
{
    if (dir == DIR_NORTH && destIsDoor && !fromIsDoor)
        return MP_AVATAR_DOOR_ENTER;
    if (dir == DIR_SOUTH && fromIsDoor && !destIsDoor)
        return MP_AVATAR_DOOR_EXIT;
    return MP_AVATAR_DOOR_NONE;
}

u32 MpAvatar_DeniedMapCount(void)
{
    return sizeof(sDeniedMaps) / sizeof(sDeniedMaps[0]);
}

u16 MpAvatar_DeniedMapAt(u32 index)
{
    return (index < MpAvatar_DeniedMapCount()) ? sDeniedMaps[index] : 0xFFFF;
}
