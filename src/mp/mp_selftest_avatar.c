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

u32 MpSelftest_Avatar(void)
{
    sFail = 0;
    TestIdsAndBudget();
    TestGraphicsAndActions();
    TestPlan();
    TestReplay();
    return sFail;
}
