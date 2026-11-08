// --mp-selftest, PS-031 remote-avatar group (pure logic, no game state):
//  - localId <-> slot mapping and MP_IsRemoteAvatarLocalId;
//  - the object-slot budget (never more than 3, always 2 slots left for NPCs / scripts);
//  - graphics ids per gender and avatar state;
//  - presence movement -> MOVEMENT_ACTION_* translation;
//  - the per-update plan (turn / step / run / ledge jump / hop / snap) and the snap rule;
//  - a replay simulation: a remote path fed through the planner like mp_avatars.c does,
//    with a clean stream, merged steps (stepId gaps) and a 3-tile gap.

#include <string.h>
#include "mp/mp.h"
#include "mp/mp_avatar.h"
#include "mp/mp_internal.h"
#include "mp/mp_selftest.h"
#include "constants/global.h"
#include "constants/event_objects.h"
#include "constants/event_object_movement.h"
#include "constants/maps.h"

static u32 sFail;

#define A_CHECK(cond, what)                                                         \
    do                                                                              \
    {                                                                               \
        if (!(cond))                                                                \
        {                                                                           \
            MP_LOG("selftest: FAIL: %s (%s:%d)\n", what, __FILE__, __LINE__);       \
            sFail++;                                                                \
        }                                                                           \
    } while (0)

static void Report(const char *what, u32 failuresBefore)
{
    if (sFail == failuresBefore)
        MP_LOG("selftest: avatar: %s OK\n", what);
}

static void P(struct MpPresence *p, s16 x, s16 y, u8 facing, u8 movement, u8 avatarState, u8 stepId)
{
    memset(p, 0, sizeof(*p));
    p->mapGroup = 0;
    p->mapNum = 9;
    p->x = x;
    p->y = y;
    p->facing = facing;
    p->movement = movement;
    p->avatarState = avatarState;
    p->stepId = stepId;
}

static void TestIdsAndBudget(void)
{
    u32 failuresBefore = sFail;
    u8 slot;

    for (slot = 0; slot < MP_MAX_PLAYERS; slot++)
    {
        A_CHECK(MpAvatar_LocalIdForSlot(slot) == MP_AVATAR_LOCAL_ID_BASE + slot, "localId = 0xE0 + slot");
        A_CHECK(MpAvatar_SlotForLocalId(MpAvatar_LocalIdForSlot(slot)) == slot, "localId -> slot round trip");
        A_CHECK(MP_IsRemoteAvatarLocalId(MpAvatar_LocalIdForSlot(slot)), "avatar localId recognised");
    }
    A_CHECK(MP_AVATAR_LOCAL_ID_BASE == 0xE0, "MP_AVATAR_LOCAL_ID_BASE is 0xE0 (PS-014)");
    A_CHECK(!MP_IsRemoteAvatarLocalId(0xDF) && !MP_IsRemoteAvatarLocalId(0xE0 + MP_MAX_PLAYERS), "range ends");
    A_CHECK(!MP_IsRemoteAvatarLocalId(LOCALID_PLAYER) && !MP_IsRemoteAvatarLocalId(127) && !MP_IsRemoteAvatarLocalId(0) && !MP_IsRemoteAvatarLocalId(1),
            "player, camera and map localIds are not avatars");
    A_CHECK(!MP_IsRemoteAvatarLocalId(236) && !MP_IsRemoteAvatarLocalId(240), "Berry Blender localIds are not avatars");
    A_CHECK(MpAvatar_SlotForLocalId(5) == MP_SLOT_NONE && MpAvatar_SlotForLocalId(0xFF) == MP_SLOT_NONE, "non-avatar localId -> no slot");
    A_CHECK(MpAvatar_LocalIdForSlot(200) < MP_AVATAR_LOCAL_ID_BASE + MP_MAX_PLAYERS, "bad slot stays inside the range");

    // 16 object slots: others + pending NPCs + 2 reserved, at most 3 avatars.
    A_CHECK(MpAvatar_Budget(1, 0) == MP_MAX_REMOTE_AVATARS, "budget: empty map allows 3");
    A_CHECK(MpAvatar_Budget(11, 0) == 3, "budget: 11 others -> 3");
    A_CHECK(MpAvatar_Budget(12, 0) == 2, "budget: 12 others -> 2");
    A_CHECK(MpAvatar_Budget(13, 0) == 1, "budget: 13 others -> 1");
    A_CHECK(MpAvatar_Budget(14, 0) == 0, "budget: 14 others -> 0 (2 reserved)");
    A_CHECK(MpAvatar_Budget(16, 0) == 0 && MpAvatar_Budget(200, 200) == 0, "budget: full / absurd -> 0");
    A_CHECK(MpAvatar_Budget(6, 7) == 1, "budget: pending NPCs count like spawned ones");
    A_CHECK(MpAvatar_Budget(0, 0) == MP_MAX_REMOTE_AVATARS, "budget never exceeds MP_MAX_REMOTE_AVATARS");
    Report("localId mapping and object-slot budget", failuresBefore);
}

static void TestGraphicsAndActions(void)
{
    u32 failuresBefore = sFail;
    u8 d;

    A_CHECK(MpAvatar_GraphicsId(MALE, MP_AVATAR_ON_FOOT) == OBJ_EVENT_GFX_BRENDAN_NORMAL, "Brendan on foot");
    A_CHECK(MpAvatar_GraphicsId(FEMALE, MP_AVATAR_ON_FOOT) == OBJ_EVENT_GFX_MAY_NORMAL, "May on foot");
    A_CHECK(MpAvatar_GraphicsId(MALE, MP_AVATAR_MACH_BIKE) == OBJ_EVENT_GFX_BRENDAN_MACH_BIKE, "Brendan mach bike");
    A_CHECK(MpAvatar_GraphicsId(FEMALE, MP_AVATAR_ACRO_BIKE) == OBJ_EVENT_GFX_MAY_ACRO_BIKE, "May acro bike");
    A_CHECK(MpAvatar_GraphicsId(MALE, MP_AVATAR_SURFING) == OBJ_EVENT_GFX_BRENDAN_SURFING, "Brendan surfing");
    A_CHECK(MpAvatar_GraphicsId(FEMALE, MP_AVATAR_UNDERWATER) == OBJ_EVENT_GFX_MAY_UNDERWATER, "May underwater");
    A_CHECK(MpAvatar_GraphicsId(2, MP_AVATAR_STATE_COUNT) == OBJ_EVENT_GFX_BRENDAN_NORMAL, "bad gender / state -> Brendan on foot");

    A_CHECK(MpAvatar_DirectionOf(1, 0) == DIR_EAST && MpAvatar_DirectionOf(-1, 0) == DIR_WEST, "direction x");
    A_CHECK(MpAvatar_DirectionOf(0, 1) == DIR_SOUTH && MpAvatar_DirectionOf(0, -2) == DIR_NORTH, "direction y");
    A_CHECK(MpAvatar_DirectionOf(1, 1) == DIR_EAST && MpAvatar_DirectionOf(1, -3) == DIR_NORTH, "direction dominant axis, x on a tie");
    A_CHECK(MpAvatar_DirectionOf(0, 0) == DIR_NONE, "direction none");

    for (d = DIR_SOUTH; d <= DIR_EAST; d++)
    {
        A_CHECK(MpAvatar_StepAction(MP_MOVE_WALK, MP_AVATAR_ON_FOOT, d) == MOVEMENT_ACTION_WALK_NORMAL_DOWN + d - 1, "walk -> WALK_NORMAL_<dir>");
        A_CHECK(MpAvatar_StepAction(MP_MOVE_WALK_FAST, MP_AVATAR_ON_FOOT, d) == MOVEMENT_ACTION_PLAYER_RUN_DOWN + d - 1, "running on foot -> PLAYER_RUN_<dir>");
        A_CHECK(MpAvatar_StepAction(MP_MOVE_WALK_FAST, MP_AVATAR_SURFING, d) == MOVEMENT_ACTION_WALK_FAST_DOWN + d - 1, "surfing -> WALK_FAST_<dir>");
        A_CHECK(MpAvatar_StepAction(MP_MOVE_WALK_FAST, MP_AVATAR_ACRO_BIKE, d) == MOVEMENT_ACTION_WALK_FAST_DOWN + d - 1, "acro bike -> WALK_FAST_<dir>");
        A_CHECK(MpAvatar_StepAction(MP_MOVE_WALK_FASTER, MP_AVATAR_MACH_BIKE, d) == MOVEMENT_ACTION_WALK_FASTER_DOWN + d - 1, "mach bike -> WALK_FASTER_<dir>");
        A_CHECK(MpAvatar_StepAction(MP_MOVE_NONE, MP_AVATAR_ON_FOOT, d) == MOVEMENT_ACTION_WALK_NORMAL_DOWN + d - 1, "missed step -> walk");
    }
    A_CHECK(MOVEMENT_ACTION_WALK_NORMAL_UP == MOVEMENT_ACTION_WALK_NORMAL_DOWN + DIR_NORTH - 1
         && MOVEMENT_ACTION_PLAYER_RUN_RIGHT == MOVEMENT_ACTION_PLAYER_RUN_DOWN + DIR_EAST - 1
         && MOVEMENT_ACTION_JUMP_2_LEFT == MOVEMENT_ACTION_JUMP_2_DOWN + DIR_WEST - 1
         && MOVEMENT_ACTION_FACE_RIGHT == MOVEMENT_ACTION_FACE_DOWN + DIR_EAST - 1, "action blocks are DOWN, UP, LEFT, RIGHT");
    A_CHECK(MpAvatar_StepAction(MP_MOVE_WALK, MP_AVATAR_ON_FOOT, 0) == MOVEMENT_ACTION_WALK_NORMAL_DOWN
         && MpAvatar_StepAction(MP_MOVE_WALK, MP_AVATAR_ON_FOOT, 9) == MOVEMENT_ACTION_WALK_NORMAL_DOWN, "bad direction -> DOWN");
    Report("graphics ids and movement-action translation", failuresBefore);
}

static void TestPlan(void)
{
    struct MpPresence t;
    u32 failuresBefore = sFail;
    u8 action;

    P(&t, 5, 5, DIR_SOUTH, MP_MOVE_NONE, MP_AVATAR_ON_FOOT, 1);
    A_CHECK(MpAvatar_Plan(5, 5, DIR_SOUTH, &t, TRUE, &action) == MP_AVATAR_PLAN_DONE, "same tile, same facing: done");
    t.facing = DIR_WEST;
    A_CHECK(MpAvatar_Plan(5, 5, DIR_SOUTH, &t, FALSE, &action) == MP_AVATAR_PLAN_ACTION && action == MOVEMENT_ACTION_FACE_LEFT, "turn in place");
    t.facing = 0;
    A_CHECK(MpAvatar_Plan(5, 5, DIR_NORTH, &t, FALSE, &action) == MP_AVATAR_PLAN_ACTION && action == MOVEMENT_ACTION_FACE_DOWN, "bad facing treated as south");

    P(&t, 6, 5, DIR_EAST, MP_MOVE_WALK, MP_AVATAR_ON_FOOT, 2);
    A_CHECK(MpAvatar_Plan(5, 5, DIR_SOUTH, &t, TRUE, &action) == MP_AVATAR_PLAN_ACTION && action == MOVEMENT_ACTION_WALK_NORMAL_RIGHT, "walk east");
    t.movement = MP_MOVE_WALK_FAST;
    A_CHECK(MpAvatar_Plan(5, 5, DIR_EAST, &t, TRUE, &action) == MP_AVATAR_PLAN_ACTION && action == MOVEMENT_ACTION_PLAYER_RUN_RIGHT, "run east");
    t.avatarState = MP_AVATAR_MACH_BIKE;
    t.movement = MP_MOVE_WALK_FASTER;
    A_CHECK(MpAvatar_Plan(5, 5, DIR_EAST, &t, TRUE, &action) == MP_AVATAR_PLAN_ACTION && action == MOVEMENT_ACTION_WALK_FASTER_RIGHT, "mach bike east");

    P(&t, 5, 7, DIR_SOUTH, MP_MOVE_JUMP, MP_AVATAR_ON_FOOT, 3);
    A_CHECK(MpAvatar_Plan(5, 5, DIR_SOUTH, &t, TRUE, &action) == MP_AVATAR_PLAN_ACTION && action == MOVEMENT_ACTION_JUMP_2_DOWN, "ledge jump south (2 tiles)");
    t.movement = MP_MOVE_WALK;
    A_CHECK(MpAvatar_Plan(5, 5, DIR_SOUTH, &t, TRUE, &action) == MP_AVATAR_PLAN_ACTION && action == MOVEMENT_ACTION_WALK_NORMAL_DOWN, "2 tiles behind (merged steps): walk the first");
    P(&t, 6, 6, DIR_SOUTH, MP_MOVE_WALK, MP_AVATAR_ON_FOOT, 3);
    A_CHECK(MpAvatar_Plan(5, 5, DIR_SOUTH, &t, TRUE, &action) == MP_AVATAR_PLAN_ACTION && action == MOVEMENT_ACTION_WALK_NORMAL_RIGHT, "diagonal gap: x first");
    P(&t, 8, 5, DIR_EAST, MP_MOVE_WALK, MP_AVATAR_ON_FOOT, 4);
    A_CHECK(MpAvatar_Plan(5, 5, DIR_EAST, &t, TRUE, &action) == MP_AVATAR_PLAN_SNAP, "3 tiles: snap");
    P(&t, 6, 5, DIR_EAST, MP_MOVE_WARP, MP_AVATAR_ON_FOOT, 5);
    A_CHECK(MpAvatar_Plan(5, 5, DIR_EAST, &t, TRUE, &action) == MP_AVATAR_PLAN_SNAP, "warp: snap even when adjacent");
    P(&t, 5, 5, DIR_EAST, MP_MOVE_WARP, MP_AVATAR_ON_FOOT, 5);
    A_CHECK(MpAvatar_Plan(5, 5, DIR_EAST, &t, TRUE, &action) == MP_AVATAR_PLAN_DONE, "warp onto the same tile: nothing to do");

    P(&t, 5, 5, DIR_NORTH, MP_MOVE_JUMP_IN_PLACE, MP_AVATAR_ACRO_BIKE, 6);
    A_CHECK(MpAvatar_Plan(5, 5, DIR_NORTH, &t, TRUE, &action) == MP_AVATAR_PLAN_ACTION && action == MOVEMENT_ACTION_ACRO_WHEELIE_HOP_FACE_UP, "acro hop in place");
    A_CHECK(MpAvatar_Plan(5, 5, DIR_NORTH, &t, FALSE, &action) == MP_AVATAR_PLAN_DONE, "the same hop is not replayed twice (keep-alive)");
    t.avatarState = MP_AVATAR_ON_FOOT;
    A_CHECK(MpAvatar_Plan(5, 5, DIR_NORTH, &t, TRUE, &action) == MP_AVATAR_PLAN_ACTION && action == MOVEMENT_ACTION_JUMP_IN_PLACE_UP, "on-foot jump in place");

    P(&t, 7, 5, DIR_EAST, MP_MOVE_WALK, MP_AVATAR_ON_FOOT, 7);
    A_CHECK(!MpAvatar_IsTooFarBehind(5, 5, &t), "2 tiles behind: no snap");
    t.x = 8;
    A_CHECK(MpAvatar_IsTooFarBehind(5, 5, &t), "3 tiles behind: snap");
    t.movement = MP_MOVE_JUMP;
    A_CHECK(!MpAvatar_IsTooFarBehind(5, 5, &t), "a ledge jump in flight allows one extra tile");
    t.x = 9;
    A_CHECK(MpAvatar_IsTooFarBehind(5, 5, &t), "jump + 2 more: snap");
    P(&t, 5, 3, DIR_NORTH, MP_MOVE_WALK, MP_AVATAR_ON_FOOT, 7);
    t.x = 4;
    A_CHECK(MpAvatar_IsTooFarBehind(5, 5, &t), "Manhattan distance (1 + 2)");
    Report("per-update plan and snap rule", failuresBefore);
}

// --- replay simulation -------------------------------------------------------

struct SimAvatar
{
    s16 x, y;
    u8 facing;
    u8 lastStepId;
    u32 snaps;
    u32 actions;
};

static bool8 InBlock(u8 action, u8 firstDown)
{
    return action >= firstDown && action < firstDown + 4;
}

// Applies a MOVEMENT_ACTION_* to the simulated avatar, the way the game would.
static void SimApply(struct SimAvatar *a, u8 action)
{
    static const s8 sDx[5] = { 0, 0, 0, -1, 1 };
    static const s8 sDy[5] = { 0, 1, -1, 0, 0 };
    u8 dir = DIR_NONE;
    u8 tiles = 0;

    if (InBlock(action, MOVEMENT_ACTION_FACE_DOWN))
        dir = (u8)(action - MOVEMENT_ACTION_FACE_DOWN + 1);
    else if (InBlock(action, MOVEMENT_ACTION_WALK_NORMAL_DOWN))
        dir = (u8)(action - MOVEMENT_ACTION_WALK_NORMAL_DOWN + 1), tiles = 1;
    else if (InBlock(action, MOVEMENT_ACTION_PLAYER_RUN_DOWN))
        dir = (u8)(action - MOVEMENT_ACTION_PLAYER_RUN_DOWN + 1), tiles = 1;
    else if (InBlock(action, MOVEMENT_ACTION_WALK_FAST_DOWN))
        dir = (u8)(action - MOVEMENT_ACTION_WALK_FAST_DOWN + 1), tiles = 1;
    else if (InBlock(action, MOVEMENT_ACTION_WALK_FASTER_DOWN))
        dir = (u8)(action - MOVEMENT_ACTION_WALK_FASTER_DOWN + 1), tiles = 1;
    else if (InBlock(action, MOVEMENT_ACTION_JUMP_2_DOWN))
        dir = (u8)(action - MOVEMENT_ACTION_JUMP_2_DOWN + 1), tiles = 2;
    else if (InBlock(action, MOVEMENT_ACTION_JUMP_IN_PLACE_DOWN))
        dir = (u8)(action - MOVEMENT_ACTION_JUMP_IN_PLACE_DOWN + 1);
    else if (InBlock(action, MOVEMENT_ACTION_ACRO_WHEELIE_HOP_FACE_DOWN))
        dir = (u8)(action - MOVEMENT_ACTION_ACRO_WHEELIE_HOP_FACE_DOWN + 1);
    if (dir < DIR_SOUTH || dir > DIR_EAST)
    {
        sFail++;
        MP_LOG("selftest: FAIL: unknown movement action 0x%02X in the replay\n", (unsigned)action);
        return;
    }
    a->facing = dir;
    a->x = (s16)(a->x + sDx[dir] * tiles);
    a->y = (s16)(a->y + sDy[dir] * tiles);
    a->actions++;
}

// Feeds `count` updates through the planner exactly like MpReplay (one action per "tick",
// DONE consumes and continues, SNAP teleports). Returns TRUE if every update was reached.
static bool8 SimReplay(struct SimAvatar *a, const struct MpPresence *updates, u32 count)
{
    u32 i, guard;
    u8 plan, action;

    for (i = 0; i < count; i++)
    {
        for (guard = 0; guard < 8; guard++)
        {
            plan = MpAvatar_Plan(a->x, a->y, a->facing, &updates[i], updates[i].stepId != a->lastStepId, &action);
            if (plan == MP_AVATAR_PLAN_SNAP)
            {
                a->x = updates[i].x;
                a->y = updates[i].y;
                a->facing = updates[i].facing;
                a->lastStepId = updates[i].stepId;
                a->snaps++;
                break;
            }
            a->lastStepId = updates[i].stepId;
            if (plan == MP_AVATAR_PLAN_DONE)
                break;
            SimApply(a, action);
        }
        if (guard == 8 || a->x != updates[i].x || a->y != updates[i].y)
            return FALSE;
    }
    return TRUE;
}

static void TestReplay(void)
{
    struct MpPresence path[16];
    struct SimAvatar a;
    u32 failuresBefore = sFail;
    u32 n = 0;

    // Clean stream: walk east 3, turn north, run north 2, ledge jump south, hop, mach bike west 2.
    P(&path[n++], 11, 10, DIR_EAST, MP_MOVE_WALK, MP_AVATAR_ON_FOOT, 1);
    P(&path[n++], 12, 10, DIR_EAST, MP_MOVE_WALK, MP_AVATAR_ON_FOOT, 2);
    P(&path[n++], 13, 10, DIR_EAST, MP_MOVE_WALK, MP_AVATAR_ON_FOOT, 3);
    P(&path[n++], 13, 10, DIR_NORTH, MP_MOVE_TURN, MP_AVATAR_ON_FOOT, 4);
    P(&path[n++], 13, 9, DIR_NORTH, MP_MOVE_WALK_FAST, MP_AVATAR_ON_FOOT, 5);
    P(&path[n++], 13, 8, DIR_NORTH, MP_MOVE_WALK_FAST, MP_AVATAR_ON_FOOT, 6);
    P(&path[n++], 13, 8, DIR_NORTH, MP_MOVE_NONE, MP_AVATAR_ON_FOOT, 6);   // stopped (keep-alive)
    P(&path[n++], 13, 10, DIR_SOUTH, MP_MOVE_JUMP, MP_AVATAR_ON_FOOT, 7);  // ledge
    P(&path[n++], 13, 10, DIR_SOUTH, MP_MOVE_JUMP_IN_PLACE, MP_AVATAR_ON_FOOT, 8);
    P(&path[n++], 13, 10, DIR_SOUTH, MP_MOVE_JUMP_IN_PLACE, MP_AVATAR_ON_FOOT, 8); // keep-alive repeat
    P(&path[n++], 12, 10, DIR_WEST, MP_MOVE_WALK_FASTER, MP_AVATAR_MACH_BIKE, 9);
    P(&path[n++], 11, 10, DIR_WEST, MP_MOVE_WALK_FASTER, MP_AVATAR_MACH_BIKE, 10);
    memset(&a, 0, sizeof(a));
    a.x = 10;
    a.y = 10;
    a.facing = DIR_EAST;
    A_CHECK(SimReplay(&a, path, n), "clean stream: every update reached");
    A_CHECK(a.snaps == 0, "clean stream: no snap");
    A_CHECK(a.x == 11 && a.y == 10 && a.facing == DIR_WEST, "clean stream: ends on the last update");
    A_CHECK(a.actions == 3 + 1 + 2 + 1 + 1 + 2, "clean stream: one action per step, turn, jump and hop; keep-alives replay nothing");

    // Merged steps: only every second update arrives (stepId gaps of 2).
    n = 0;
    P(&path[n++], 12, 10, DIR_EAST, MP_MOVE_WALK, MP_AVATAR_ON_FOOT, 2);
    P(&path[n++], 14, 10, DIR_EAST, MP_MOVE_WALK, MP_AVATAR_ON_FOOT, 4);
    P(&path[n++], 14, 12, DIR_SOUTH, MP_MOVE_WALK, MP_AVATAR_ON_FOOT, 6);
    memset(&a, 0, sizeof(a));
    a.x = 10;
    a.y = 10;
    a.facing = DIR_EAST;
    A_CHECK(SimReplay(&a, path, n), "merged steps: every update reached by walking");
    A_CHECK(a.snaps == 0 && a.x == 14 && a.y == 12 && a.facing == DIR_SOUTH, "merged steps: no snap, right end");

    // A 3-tile gap (lost packets) and a warp both snap.
    n = 0;
    P(&path[n++], 13, 10, DIR_EAST, MP_MOVE_WALK, MP_AVATAR_ON_FOOT, 3);
    P(&path[n++], 30, 2, DIR_NORTH, MP_MOVE_WARP, MP_AVATAR_ON_FOOT, 4);
    P(&path[n++], 30, 1, DIR_NORTH, MP_MOVE_WALK, MP_AVATAR_ON_FOOT, 5);
    memset(&a, 0, sizeof(a));
    a.x = 10;
    a.y = 10;
    a.facing = DIR_EAST;
    A_CHECK(SimReplay(&a, path, n), "gap + warp: every update reached");
    A_CHECK(a.snaps == 2 && a.x == 30 && a.y == 1, "gap + warp: two snaps, then walking resumes");
    Report("replay simulation (clean, merged steps, gap + warp)", failuresBefore);
}

// --- PS-032: connected maps and the deny table ---------------------------------

static void TestConnections(void)
{
    struct MpPresence t;
    u32 failuresBefore = sFail;
    s16 x, y, x2, y2;
    u8 action;

    // Current map 20 x 20. Connected maps: 30 x 10 (north / south), 12 x 25 (west / east).
    A_CHECK(MpAvatar_TranslateConnection(CONNECTION_SOUTH, 5, 20, 20, 30, 10, 3, 0, &x, &y) && x == 8 && y == 20, "south: (3,0) -> (8,20)");
    A_CHECK(MpAvatar_TranslateConnection(CONNECTION_NORTH, 5, 20, 20, 30, 10, 3, 9, &x, &y) && x == 8 && y == -1, "north: bottom row -> y -1");
    A_CHECK(MpAvatar_TranslateConnection(CONNECTION_NORTH, -4, 20, 20, 30, 10, 0, 0, &x, &y) && x == -4 && y == -10, "north, negative offset");
    A_CHECK(MpAvatar_TranslateConnection(CONNECTION_WEST, -4, 20, 20, 12, 25, 11, 2, &x, &y) && x == -1 && y == -2, "west: right column -> x -1");
    A_CHECK(MpAvatar_TranslateConnection(CONNECTION_EAST, 3, 20, 20, 12, 25, 0, 0, &x, &y) && x == 20 && y == 3, "east: (0,0) -> (20,3)");
    A_CHECK(!MpAvatar_TranslateConnection(CONNECTION_DIVE, 0, 20, 20, 20, 20, 1, 1, &x, &y), "dive is not side by side");
    A_CHECK(!MpAvatar_TranslateConnection(CONNECTION_EMERGE, 0, 20, 20, 20, 20, 1, 1, &x, &y), "emerge is not side by side");
    A_CHECK(!MpAvatar_TranslateConnection(CONNECTION_NONE, 0, 20, 20, 20, 20, 1, 1, &x, &y), "no connection");
    A_CHECK(!MpAvatar_TranslateConnection(CONNECTION_EAST, 0x100000, 20, 20, 20, 20, 1, 1, &x, &y), "absurd offset rejected");

    // Seam continuity: the last tile of the north map and the first tile of the current map
    // are one step apart, so a player walking across is replayed as one walk, not a snap.
    MpAvatar_TranslateConnection(CONNECTION_NORTH, 5, 20, 20, 30, 10, 3, 9, &x, &y);
    P(&t, 8, 0, DIR_SOUTH, MP_MOVE_WALK, MP_AVATAR_ON_FOOT, 2);
    A_CHECK(MpAvatar_Plan(x, y, DIR_SOUTH, &t, TRUE, &action) == MP_AVATAR_PLAN_ACTION && action == MOVEMENT_ACTION_WALK_NORMAL_DOWN,
            "walking south across a seam is one step");
    // East seam the other way round.
    MpAvatar_TranslateConnection(CONNECTION_EAST, 3, 20, 20, 12, 25, 0, 4, &x2, &y2);
    P(&t, 19, 7, DIR_WEST, MP_MOVE_WALK_FAST, MP_AVATAR_ON_FOOT, 3);
    A_CHECK(x2 == 20 && y2 == 7 && MpAvatar_Plan(x2, y2, DIR_WEST, &t, TRUE, &action) == MP_AVATAR_PLAN_ACTION
         && action == MOVEMENT_ACTION_PLAYER_RUN_LEFT, "running west out of the east map is one step");
    Report("connected-map coordinate translation", failuresBefore);
}

static void TestDenyTable(void)
{
    u32 failuresBefore = sFail;
    u32 i, j, n = MpAvatar_DeniedMapCount();
    bool8 allValid = TRUE, unique = TRUE;

    A_CHECK(n >= 40, "deny table is populated");
    for (i = 0; i < n; i++)
    {
        u16 id = MpAvatar_DeniedMapAt(i);

        if (!MP_IsValidMap((u8)(id >> 8), (u8)(id & 0xFF)))
            allValid = FALSE;
        for (j = 0; j < i; j++)
        {
            if (MpAvatar_DeniedMapAt(j) == id)
                unique = FALSE;
        }
    }
    A_CHECK(allValid, "every denied map is a valid map of this build");
    A_CHECK(unique, "no duplicate in the deny table");
    A_CHECK(MpAvatar_DeniedMapAt(n) == 0xFFFF, "deny table index past the end");

#define DENIED(map) MpAvatar_IsMapDenied(MAP_GROUP(map), MAP_NUM(map))
    A_CHECK(DENIED(MAP_UNION_ROOM) && DENIED(MAP_TRADE_CENTER) && DENIED(MAP_RECORD_CORNER)
         && DENIED(MAP_BATTLE_COLOSSEUM_2P) && DENIED(MAP_BATTLE_COLOSSEUM_4P), "link rooms denied");
    A_CHECK(DENIED(MAP_BATTLE_FRONTIER_BATTLE_TOWER_BATTLE_ROOM) && DENIED(MAP_BATTLE_FRONTIER_BATTLE_PIKE_THREE_PATH_ROOM)
         && DENIED(MAP_BATTLE_FRONTIER_BATTLE_PYRAMID_FLOOR) && DENIED(MAP_SLATEPORT_CITY_BATTLE_TENT_BATTLE_ROOM), "challenge rooms denied");
    A_CHECK(DENIED(MAP_TRAINER_HILL_1F) && DENIED(MAP_SECRET_BASE_RED_CAVE1) && DENIED(MAP_SECRET_BASE_SHRUB4), "per-save layouts denied");
    A_CHECK(DENIED(MAP_CONTEST_HALL) && DENIED(MAP_LILYCOVE_CITY_CONTEST_HALL) && DENIED(MAP_INSIDE_OF_TRUCK), "contest stages and the truck denied");
    A_CHECK(!DENIED(MAP_LITTLEROOT_TOWN) && !DENIED(MAP_OLDALE_TOWN_POKEMON_CENTER_1F) && !DENIED(MAP_LILYCOVE_CITY),
            "ordinary maps allowed");
    A_CHECK(!DENIED(MAP_SAFARI_ZONE_SOUTH) && !DENIED(MAP_SAFARI_ZONE_NORTHEAST), "Safari Zone allowed (decision)");
    A_CHECK(!DENIED(MAP_BATTLE_FRONTIER_OUTSIDE_WEST) && !DENIED(MAP_BATTLE_FRONTIER_BATTLE_TOWER_LOBBY)
         && !DENIED(MAP_LILYCOVE_CITY_CONTEST_LOBBY) && !DENIED(MAP_ROUTE112_CABLE_CAR_STATION)
         && !DENIED(MAP_TRAINER_HILL_ENTRANCE), "lobbies, outside areas and stations allowed");
#undef DENIED
    A_CHECK(!MpAvatar_IsMapDenied(MP_MAP_NONE, MP_MAP_NONE), "MP_MAP_NONE is not in the table");
    Report("deny table", failuresBefore);
}

// --- PS-036: doors and ledges ------------------------------------------------------

static void TestDoorsAndLedges(void)
{
    struct MpPresence t;
    u32 failuresBefore = sFail;
    s16 x, y;
    u8 action;

    MpAvatar_StepCoords(DIR_SOUTH, 5, 5, 1, &x, &y);
    A_CHECK(x == 5 && y == 6, "step south");
    MpAvatar_StepCoords(DIR_NORTH, 5, 5, 2, &x, &y);
    A_CHECK(x == 5 && y == 3, "two north");
    MpAvatar_StepCoords(DIR_WEST, 5, 5, 1, &x, &y);
    A_CHECK(x == 4 && y == 5, "step west");
    MpAvatar_StepCoords(DIR_EAST, 5, 5, 1, &x, &y);
    A_CHECK(x == 6 && y == 5, "step east");
    MpAvatar_StepCoords(DIR_NONE, 5, 5, 1, &x, &y);
    A_CHECK(x == 5 && y == 5, "no direction, no move");

    A_CHECK(MpAvatar_DoorTransition(DIR_NORTH, FALSE, TRUE) == MP_AVATAR_DOOR_ENTER, "north into a door: enter");
    A_CHECK(MpAvatar_DoorTransition(DIR_SOUTH, TRUE, FALSE) == MP_AVATAR_DOOR_EXIT, "south out of a door: exit");
    A_CHECK(MpAvatar_DoorTransition(DIR_SOUTH, FALSE, TRUE) == MP_AVATAR_DOOR_NONE, "south onto a door: not a door warp");
    A_CHECK(MpAvatar_DoorTransition(DIR_EAST, FALSE, TRUE) == MP_AVATAR_DOOR_NONE, "sideways onto a door: nothing");
    A_CHECK(MpAvatar_DoorTransition(DIR_NORTH, TRUE, FALSE) == MP_AVATAR_DOOR_NONE, "north off a door: nothing");
    A_CHECK(MpAvatar_DoorTransition(DIR_NORTH, FALSE, FALSE) == MP_AVATAR_DOOR_NONE, "ordinary step: nothing");
    A_CHECK(MpAvatar_DoorTransition(DIR_SOUTH, TRUE, TRUE) == MP_AVATAR_DOOR_NONE, "door to door: nothing");

    // The capture now reports a ledge jump as ONE update with the landing tile (PS-036):
    // replayed as JUMP_2 (arc + shadow), never as two walks or a snap.
    P(&t, 5, 7, DIR_SOUTH, MP_MOVE_JUMP, MP_AVATAR_ON_FOOT, 9);
    A_CHECK(MpAvatar_Plan(5, 5, DIR_SOUTH, &t, TRUE, &action) == MP_AVATAR_PLAN_ACTION && action == MOVEMENT_ACTION_JUMP_2_DOWN,
            "ledge jump down -> JUMP_2_DOWN");
    P(&t, 7, 5, DIR_EAST, MP_MOVE_JUMP, MP_AVATAR_ACRO_BIKE, 9);
    A_CHECK(MpAvatar_Plan(5, 5, DIR_EAST, &t, TRUE, &action) == MP_AVATAR_PLAN_ACTION && action == MOVEMENT_ACTION_JUMP_2_RIGHT,
            "ledge jump east on a bike -> JUMP_2_RIGHT");
    A_CHECK(!MpAvatar_IsTooFarBehind(5, 5, &t), "a jump's landing tile never snaps");
    Report("doors and ledge jumps", failuresBefore);
}

// --- BUG-008: jitter, loss and reordering end exactly on the reported tile -------------
//
// Frame-by-frame model of both ends. Sender: a scripted walk, captured every frame (x/y =
// destination during a step), sent on change at most every 3 frames (50 ms) plus a keep-alive
// every 60 frames (1 s), seq + 1 each send. Network: fixed seed, loss, base delay + random
// jitter (so packets reorder). Receiver: the PS-030 newer-seq filter, latest + an 8-entry
// FIFO that drops the oldest on overflow, and a replay that mirrors MpReplay: nothing starts
// while a step is still running, then the overflow / too-far snap, then queued updates through
// MpAvatar_Plan (+ catch-up), then MpAvatar_PlanIdle once the queue is empty.

#define NET_FRAMES      1800
#define NET_INFLIGHT    256

struct NetScriptItem
{
    u8 movement;    // MP_MOVE_WALK / WALK_FAST / TURN / NONE (wait)
    u8 dir;
    u8 count;       // steps, or frames for a wait
};

struct NetSim
{
    u32 rng;
    // sender
    struct MpPresence local;
    const struct NetScriptItem *script;
    u32 scriptLen, item, itemDone, busy;
    struct MpPresence lastSent;
    bool8 sentAny;
    u32 lastSentFrame;
    u16 nextSeq;
    // network
    struct { u32 at; struct MpPresence p; bool8 used; } air[NET_INFLIGHT];
    // receiver
    bool8 seqValid;
    u16 lastSeq;
    struct MpPresence latest;
    bool8 hasLatest;
    struct MpPresence queue[MP_PRESENCE_QUEUE_SIZE];
    u32 qHead, qCount;
    bool8 overflow;
    // avatar
    struct SimAvatar av;
    bool8 spawned, hasPending;
    struct MpPresence pending;
    u32 avBusy;
    u32 midStepSnaps;   // must stay 0
};

static u32 NetRand(struct NetSim *s)
{
    s->rng = s->rng * 1103515245u + 12345u;
    return (s->rng >> 16) & 0x7FFF;
}

static void NetSenderTick(struct NetSim *s)
{
    static const s8 sDx[5] = { 0, 0, 0, -1, 1 };
    static const s8 sDy[5] = { 0, 1, -1, 0, 0 };

    if (s->busy > 0 && --s->busy > 0)
        return;
    while (s->item < s->scriptLen)
    {
        const struct NetScriptItem *it = &s->script[s->item];

        if (s->itemDone >= it->count)
        {
            s->item++;
            s->itemDone = 0;
            continue;
        }
        s->itemDone++;
        if (it->movement == MP_MOVE_NONE)
        {
            s->local.movement = MP_MOVE_NONE;
            s->busy = 1;
            return;
        }
        s->local.facing = it->dir;
        s->local.stepId++;
        s->local.movement = it->movement;
        if (it->movement == MP_MOVE_TURN)
        {
            s->busy = 8;
            return;
        }
        s->local.x = (s16)(s->local.x + sDx[it->dir]);
        s->local.y = (s16)(s->local.y + sDy[it->dir]);
        s->busy = (it->movement == MP_MOVE_WALK_FAST) ? 8 : 16;
        return;
    }
    s->local.movement = MP_MOVE_NONE; // script over: standing still
}

static bool8 NetSamePresence(const struct MpPresence *a, const struct MpPresence *b)
{
    return a->x == b->x && a->y == b->y && a->facing == b->facing && a->movement == b->movement
        && a->stepId == b->stepId && a->avatarState == b->avatarState;
}

static void NetSend(struct NetSim *s, u32 frame, u32 lossPct, u32 baseDelay, u32 jitter, bool8 loseFinal)
{
    bool8 changed = !s->sentAny || !NetSamePresence(&s->local, &s->lastSent);
    u32 i;

    if (s->sentAny && (frame - s->lastSentFrame < 3 || (!changed && frame - s->lastSentFrame < 60)))
        return;
    s->lastSent = s->local;
    s->lastSent.seq = s->nextSeq++;
    s->sentAny = TRUE;
    s->lastSentFrame = frame;
    if (NetRand(s) % 100 < lossPct)
        return;
    // "Final step lost": every update that changes something from the last step on is lost;
    // only the keep-alives after it get through.
    if (loseFinal && changed && s->item >= s->scriptLen - 1)
        return;
    for (i = 0; i < NET_INFLIGHT; i++)
    {
        if (!s->air[i].used)
        {
            s->air[i].used = TRUE;
            s->air[i].p = s->lastSent;
            s->air[i].at = frame + baseDelay + (jitter ? NetRand(s) % (jitter + 1) : 0);
            return;
        }
    }
}

static void NetDeliver(struct NetSim *s, u32 frame)
{
    u32 i;

    for (i = 0; i < NET_INFLIGHT; i++)
    {
        const struct MpPresence *p = &s->air[i].p;

        if (!s->air[i].used || s->air[i].at > frame)
            continue;
        s->air[i].used = FALSE;
        if (s->seqValid && p->seq <= s->lastSeq)
            continue; // stale (reordered or duplicate): dropped like MpSeqAcceptable
        s->seqValid = TRUE;
        s->lastSeq = p->seq;
        s->latest = *p;
        s->hasLatest = TRUE;
        if (s->qCount >= MP_PRESENCE_QUEUE_SIZE)
        {
            s->qHead = (s->qHead + 1) % MP_PRESENCE_QUEUE_SIZE;
            s->qCount--;
            s->overflow = TRUE;
        }
        s->queue[(s->qHead + s->qCount) % MP_PRESENCE_QUEUE_SIZE] = *p;
        s->qCount++;
    }
}

static bool8 NetPop(struct NetSim *s, struct MpPresence *out)
{
    if (s->qCount == 0)
        return FALSE;
    *out = s->queue[s->qHead];
    s->qHead = (s->qHead + 1) % MP_PRESENCE_QUEUE_SIZE;
    s->qCount--;
    return TRUE;
}

static void NetSnap(struct NetSim *s, const struct MpPresence *to)
{
    if (s->avBusy > 0)
        s->midStepSnaps++;
    s->av.x = to->x;
    s->av.y = to->y;
    s->av.facing = to->facing;
    s->av.lastStepId = to->stepId;
    s->av.snaps++;
    s->hasPending = FALSE;
}

static void NetStart(struct NetSim *s, u8 action)
{
    SimApply(&s->av, action);
    s->avBusy = MpAvatar_ActionFrames(action);
}

static s16 NetBehind(const struct NetSim *s)
{
    s16 dx = (s16)(s->latest.x - s->av.x), dy = (s16)(s->latest.y - s->av.y);

    return (s16)((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy));
}

// One MpReplay tick.
static void NetReplayTick(struct NetSim *s)
{
    struct MpPresence raw;
    u8 plan, action;
    u32 guard;

    if (!s->hasLatest)
        return;
    if (!s->spawned)
    {
        s->spawned = TRUE;
        s->av.x = s->latest.x;
        s->av.y = s->latest.y;
        s->av.facing = s->latest.facing;
        s->av.lastStepId = s->latest.stepId;
        s->qCount = 0;
        s->overflow = FALSE;
        return;
    }
    if (s->avBusy > 0 && --s->avBusy > 0)
        return; // still stepping: nothing starts, not even a snap
    if (s->overflow || MpAvatar_IsTooFarBehind(s->av.x, s->av.y, &s->latest))
    {
        s->qCount = 0;
        s->overflow = FALSE;
        NetSnap(s, &s->latest);
        return;
    }
    for (guard = 0; guard < MP_PRESENCE_QUEUE_SIZE + 1; guard++)
    {
        if (!s->hasPending)
        {
            if (!NetPop(s, &raw))
            {
                if (!MpAvatar_IsIdlePresence(&s->latest))
                    return;
                plan = MpAvatar_PlanIdle(s->av.x, s->av.y, s->av.facing, &s->latest, &action);
                if (plan == MP_AVATAR_PLAN_SNAP)
                    NetSnap(s, &s->latest);
                else if (plan == MP_AVATAR_PLAN_ACTION)
                {
                    s->av.lastStepId = s->latest.stepId;
                    NetStart(s, action);
                }
                return;
            }
            s->pending = raw;
            s->hasPending = TRUE;
        }
        plan = MpAvatar_Plan(s->av.x, s->av.y, s->av.facing, &s->pending, s->pending.stepId != s->av.lastStepId, &action);
        if (plan == MP_AVATAR_PLAN_SNAP)
        {
            NetSnap(s, &s->pending);
            return;
        }
        s->av.lastStepId = s->pending.stepId;
        if (plan == MP_AVATAR_PLAN_ACTION)
        {
            if (s->pending.x != s->av.x || s->pending.y != s->av.y)
                action = MpAvatar_CatchUpAction(action, NetBehind(s));
            NetStart(s, action);
            return;
        }
        s->hasPending = FALSE;
    }
}

// Runs the script through the network; TRUE if the avatar ends exactly on the sender's
// tile and facing, idle (no step running, nothing pending or queued), never snapped mid-step.
static bool8 NetRun(const struct NetScriptItem *script, u32 len, u32 seed, u32 lossPct,
                    u32 baseDelay, u32 jitter, bool8 loseFinal, bool8 clearQueueAtEnd, u32 *snapsOut)
{
    static struct NetSim s;
    u32 f;

    memset(&s, 0, sizeof(s));
    s.rng = seed;
    s.script = script;
    s.scriptLen = len;
    P(&s.local, 10, 10, DIR_SOUTH, MP_MOVE_NONE, MP_AVATAR_ON_FOOT, 0);
    for (f = 0; f < NET_FRAMES; f++)
    {
        NetSenderTick(&s);
        NetSend(&s, f, (f < 30) ? 0 : lossPct, baseDelay, jitter, loseFinal);
        NetDeliver(&s, f);
        // A drained queue with the latest presence one tile ahead (e.g. dropped untranslatable
        // entries): only MpAvatar_PlanIdle can bring the avatar home.
        if (clearQueueAtEnd && f == NET_FRAMES - 400)
        {
            s.qCount = 0;
            s.hasPending = FALSE;
            if (s.avBusy == 0)
            {
                s.av.x = (s16)(s.local.x - 1);
                s.av.y = s.local.y;
            }
        }
        NetReplayTick(&s);
    }
    *snapsOut = s.av.snaps;
    return s.av.x == s.local.x && s.av.y == s.local.y && s.av.facing == s.local.facing
        && s.avBusy == 0 && !s.hasPending && s.qCount == 0 && s.midStepSnaps == 0
        && s.local.movement == MP_MOVE_NONE;
}

static void TestNetworkConditions(void)
{
    static const struct NetScriptItem sScript[] =
    {
        { MP_MOVE_NONE, 0, 40 },
        { MP_MOVE_WALK, DIR_EAST, 6 },
        { MP_MOVE_NONE, 0, 20 },
        { MP_MOVE_WALK_FAST, DIR_NORTH, 7 },
        { MP_MOVE_TURN, DIR_WEST, 1 },
        { MP_MOVE_WALK, DIR_WEST, 3 },
        { MP_MOVE_NONE, 0, 5 },
        { MP_MOVE_WALK_FAST, DIR_SOUTH, 4 },
        { MP_MOVE_WALK, DIR_EAST, 1 },  // the final step (lost in the loseFinal runs)
    };
    static const struct { const char *name; u32 loss, base, jitter; } sNets[] =
    {
        { "LAN",                       0,  1,  1 },
        { "delayed 200 ms",            0, 12,  0 },
        { "jitter 0-250 ms (reorder)", 0,  4, 15 },
        { "5% loss",                   5,  6,  3 },
        { "20% loss + jitter",        20,  6, 12 },
    };
    u32 failuresBefore = sFail;
    u32 n, seed, mode, snaps;
    u8 action;
    struct MpPresence t;

    // Pure pieces.
    P(&t, 5, 5, DIR_NORTH, MP_MOVE_NONE, MP_AVATAR_ON_FOOT, 3);
    A_CHECK(MpAvatar_PlanIdle(5, 5, DIR_NORTH, &t, &action) == MP_AVATAR_PLAN_DONE, "idle on the tile: done");
    A_CHECK(MpAvatar_PlanIdle(5, 5, DIR_EAST, &t, &action) == MP_AVATAR_PLAN_ACTION && action == MOVEMENT_ACTION_FACE_UP,
            "idle on the tile, wrong facing: face");
    A_CHECK(MpAvatar_PlanIdle(5, 6, DIR_NORTH, &t, &action) == MP_AVATAR_PLAN_ACTION && action == MOVEMENT_ACTION_WALK_NORMAL_UP,
            "idle one tile off: short walk");
    A_CHECK(MpAvatar_PlanIdle(6, 6, DIR_NORTH, &t, &action) == MP_AVATAR_PLAN_SNAP, "idle two tiles off (diagonal): snap");
    A_CHECK(MpAvatar_PlanIdle(5, 9, DIR_NORTH, &t, &action) == MP_AVATAR_PLAN_SNAP, "idle far off: snap");
    A_CHECK(MpAvatar_IsIdlePresence(&t), "NONE is idle");
    t.movement = MP_MOVE_WALK;
    A_CHECK(!MpAvatar_IsIdlePresence(&t), "a walk is not idle");
    A_CHECK(MpAvatar_CatchUpAction(MOVEMENT_ACTION_WALK_NORMAL_LEFT, 1) == MOVEMENT_ACTION_WALK_NORMAL_LEFT, "catch-up: 1 behind, same speed");
    A_CHECK(MpAvatar_CatchUpAction(MOVEMENT_ACTION_WALK_NORMAL_LEFT, 2) == MOVEMENT_ACTION_WALK_FAST_LEFT, "catch-up: walk -> fast");
    A_CHECK(MpAvatar_CatchUpAction(MOVEMENT_ACTION_WALK_FAST_UP, 2) == MOVEMENT_ACTION_WALK_FAST_UP, "catch-up: fast steps unchanged");
    A_CHECK(MpAvatar_CatchUpAction(MOVEMENT_ACTION_WALK_FAST_DOWN, 1) == MOVEMENT_ACTION_WALK_FAST_DOWN, "catch-up: fast step 1 behind unchanged");
    A_CHECK(MpAvatar_CatchUpAction(MOVEMENT_ACTION_PLAYER_RUN_DOWN, 2) == MOVEMENT_ACTION_PLAYER_RUN_DOWN, "catch-up: running unchanged");
    A_CHECK(MpAvatar_CatchUpAction(MOVEMENT_ACTION_JUMP_2_DOWN, 3) == MOVEMENT_ACTION_JUMP_2_DOWN, "catch-up: jumps unchanged");

    // Every network x final step delivered / lost / queue drained, 3 fixed seeds each.
    for (n = 0; n < sizeof(sNets) / sizeof(sNets[0]); n++)
    {
        for (mode = 0; mode < 3; mode++)
        {
            for (seed = 0; seed < 3; seed++)
            {
                bool8 ok = NetRun(sScript, sizeof(sScript) / sizeof(sScript[0]), 0xB008u + seed * 7919u,
                                  sNets[n].loss, sNets[n].base, sNets[n].jitter, mode == 1, mode == 2, &snaps);

                if (!ok)
                {
                    sFail++;
                    MP_LOG("selftest: FAIL: net '%s' %s seed %u: avatar not idle on the reported tile\n",
                           sNets[n].name, (mode == 0) ? "all steps" : (mode == 1) ? "final step lost" : "queue drained",
                           (unsigned)seed);
                }
                if (n == 0 && mode == 0 && snaps != 0)
                {
                    sFail++;
                    MP_LOG("selftest: FAIL: LAN replay snapped %u times\n", (unsigned)snaps);
                }
            }
        }
    }
    Report("BUG-008 idle convergence under delay, jitter, reordering and loss", failuresBefore);
}

u32 MpSelftest_Avatar(void)
{
    sFail = 0;
    TestIdsAndBudget();
    TestGraphicsAndActions();
    TestPlan();
    TestReplay();
    TestConnections();
    TestDenyTable();
    TestDoorsAndLedges();
    TestNetworkConditions();
    return sFail;
}
