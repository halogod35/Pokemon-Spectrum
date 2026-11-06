// Presence capture (PS-030): turns the local player state into a struct MpPresence once
// per game tick and hands it to MP_SetLocalPresence(). Called from the "// MP:" hook at
// the end of MainLoop()'s callbacks in src/main.c.
//
// READ ONLY (AD-1): nothing here changes game state. It reads gMain.callback2 / inBattle,
// gSaveBlock1Ptr->location, the player object event, gPlayerAvatar.flags, the start menu
// task and the field-controls lock.
//
// Wire semantics (protocol.md 6.8):
//  - x, y: currentCoords minus MAP_OFFSET. ObjectEvent currentCoords is already the
//    destination tile while a step is in progress.
//  - A new step / turn / jump / warp increments stepId (mod 256).
//  - movement is the kind of the last step that started; it returns to MP_MOVE_NONE once the
//    player has been idle (no held movement in progress) for MP_IDLE_TICKS ticks, so the short
//    gap between two consecutive steps does not flicker NONE.
//  - Map change inside the overworld callback while moving = walking across a map seam:
//    reported as a step (coords already in the new map). Any other map change, or the
//    first field tick after a load at a different tile, is MP_MOVE_WARP (receivers snap).
//  - Outside the overworld callback the last field position is kept and only `busy` changes:
//    BATTLE (gMain.inBattle), WARPING (the save's map differs from the last field map, i.e.
//    WarpIntoMap() has run and the new map is loading), otherwise MENU (bag, party, PC, ...).
//    Before the overworld has been seen once: MP_MAP_NONE / NOT_IN_FIELD.
//  - In the overworld: MENU while the start menu task runs, SCRIPT while field controls are
//    locked (scripts, cutscenes, door / warp animations, battle transitions), else NONE.

#include "global.h"
#include "event_object_movement.h"
#include "fieldmap.h"
#include "main.h"
#include "overworld.h"
#include "script.h"
#include "start_menu.h"
#include "task.h"
#include "constants/event_object_movement.h"
#include "mp/mp.h"

#define MP_IDLE_TICKS 2

static bool8 sSeenField;            // the overworld callback has run at least once
static bool8 sFieldLastTick;        // the previous tick was a captured overworld tick
static struct MpPresence sCapture;  // last value built (and passed on)
static u8 sIdleTicks;
static u8 sLastActionId;

// TRUE if `action` is in the 4-direction block starting at `firstDown` (DOWN, UP, LEFT, RIGHT).
static bool8 MpActionIn(u8 action, u8 firstDown)
{
    return action >= firstDown && action < firstDown + 4;
}

static u8 MpStepSpeedFromAction(u8 action)
{
    if (MpActionIn(action, MOVEMENT_ACTION_WALK_FASTER_DOWN))
        return MP_MOVE_WALK_FASTER;
    if (MpActionIn(action, MOVEMENT_ACTION_WALK_FAST_DOWN)
     || MpActionIn(action, MOVEMENT_ACTION_PLAYER_RUN_DOWN)
     || MpActionIn(action, MOVEMENT_ACTION_RIDE_WATER_CURRENT_DOWN)
     || MpActionIn(action, MOVEMENT_ACTION_SLIDE_DOWN))
        return MP_MOVE_WALK_FAST;
    return MP_MOVE_WALK; // WALK_NORMAL, WALK_SLOW, 1-tile jumps, anything unexpected
}

static bool8 MpIsJumpInPlaceAction(u8 action)
{
    return (action >= MOVEMENT_ACTION_JUMP_IN_PLACE_DOWN && action <= MOVEMENT_ACTION_JUMP_IN_PLACE_RIGHT_LEFT)
        || MpActionIn(action, MOVEMENT_ACTION_ACRO_WHEELIE_HOP_FACE_DOWN);
}

static u8 MpAvatarStateFromFlags(u8 flags)
{
    if (flags & PLAYER_AVATAR_FLAG_UNDERWATER)
        return MP_AVATAR_UNDERWATER;
    if (flags & PLAYER_AVATAR_FLAG_SURFING)
        return MP_AVATAR_SURFING;
    if (flags & PLAYER_AVATAR_FLAG_MACH_BIKE)
        return MP_AVATAR_MACH_BIKE;
    if (flags & PLAYER_AVATAR_FLAG_ACRO_BIKE)
        return MP_AVATAR_ACRO_BIKE;
    return MP_AVATAR_ON_FOOT;
}

static s16 MpAbs16(s16 v)
{
    return (v < 0) ? (s16)-v : v;
}

// Overworld callback is running. Returns FALSE if the player object is not usable yet.
static bool8 MpCaptureField(struct MpPresence *p)
{
    struct ObjectEvent *player;
    u8 mapGroup, mapNum, facing, action;
    s16 x, y, dx, dy;
    bool8 moving;

    if (gSaveBlock1Ptr == NULL || gPlayerAvatar.objectEventId >= OBJECT_EVENTS_COUNT)
        return FALSE;
    player = &gObjectEvents[gPlayerAvatar.objectEventId];
    if (!player->active)
        return FALSE;
    mapGroup = (u8)gSaveBlock1Ptr->location.mapGroup;
    mapNum = (u8)gSaveBlock1Ptr->location.mapNum;
    if (!MP_IsValidMap(mapGroup, mapNum))
        return FALSE;

    x = (s16)(player->currentCoords.x - MAP_OFFSET);
    y = (s16)(player->currentCoords.y - MAP_OFFSET);
    facing = player->facingDirection;
    if (facing < DIR_SOUTH || facing > DIR_EAST)
        facing = DIR_SOUTH;
    action = player->movementActionId;
    moving = player->heldMovementActive && !player->heldMovementFinished;

    if (!sSeenField || !sFieldLastTick || mapGroup != p->mapGroup || mapNum != p->mapNum)
    {
        // First field frame after a load (warp, fly, battle / menu return) or a new map.
        // Walking across a map seam changes the map without leaving the overworld
        // callback: report it as the step it is (coords are already in the new map).
        // Anything else that changes the map or comes back from a load is a WARP (snap).
        if (sSeenField && sFieldLastTick && moving
         && (mapGroup != p->mapGroup || mapNum != p->mapNum))
            p->movement = MpStepSpeedFromAction(action);
        else if (sSeenField && mapGroup == p->mapGroup && mapNum == p->mapNum && x == p->x && y == p->y)
            p->movement = MP_MOVE_NONE; // back from a menu or battle, same tile: not a warp
        else
            p->movement = MP_MOVE_WARP;
        if (p->movement != MP_MOVE_NONE)
            p->stepId++;
        sIdleTicks = 0;
    }
    else if (x != p->x || y != p->y)
    {
        dx = MpAbs16((s16)(x - p->x));
        dy = MpAbs16((s16)(y - p->y));
        if (dx + dy == 1)
            p->movement = MpStepSpeedFromAction(action);
        else if (dx + dy == 2 && (dx == 0 || dy == 0))
            p->movement = MP_MOVE_JUMP; // ledge (JUMP_2_*)
        else
            p->movement = MP_MOVE_WARP; // same-map warp / teleport
        p->stepId++;
        sIdleTicks = 0;
    }
    else if (moving && action != sLastActionId && MpIsJumpInPlaceAction(action))
    {
        p->movement = MP_MOVE_JUMP_IN_PLACE;
        p->stepId++;
        sIdleTicks = 0;
    }
    else if (facing != p->facing)
    {
        p->movement = MP_MOVE_TURN;
        p->stepId++;
        sIdleTicks = 0;
    }
    else if (moving)
    {
        sIdleTicks = 0;
    }
    else if (sIdleTicks < MP_IDLE_TICKS)
    {
        if (++sIdleTicks >= MP_IDLE_TICKS)
            p->movement = MP_MOVE_NONE;
    }

    p->mapGroup = mapGroup;
    p->mapNum = mapNum;
    p->x = x;
    p->y = y;
    p->facing = facing;
    p->avatarState = MpAvatarStateFromFlags(gPlayerAvatar.flags);
    if (FuncIsActiveTask(Task_ShowStartMenu))
        p->busy = MP_BUSY_MENU;
    else if (ArePlayerFieldControlsLocked())
        p->busy = MP_BUSY_SCRIPT;
    else
        p->busy = MP_BUSY_NONE;
    sLastActionId = moving ? action : MOVEMENT_ACTION_NONE;
    return TRUE;
}

// MP: called once per game tick from MainLoop() (src/main.c).
void MP_CaptureLocalPresence(void)
{
    struct MpPresence *p = &sCapture;

    if (gMain.callback2 == CB2_Overworld && MpCaptureField(p))
    {
        sSeenField = TRUE;
        sFieldLastTick = TRUE;
        MP_SetLocalPresence(p);
        return;
    }
    sFieldLastTick = FALSE;
    if (!sSeenField)
    {
        // Title screen, intro, main menu: not in the world yet.
        memset(p, 0, sizeof(*p));
        p->mapGroup = MP_MAP_NONE;
        p->mapNum = MP_MAP_NONE;
        p->facing = DIR_SOUTH;
        p->busy = MP_BUSY_NOT_IN_FIELD;
    }
    else
    {
        // Left the overworld callback: keep the last field position, report why.
        p->movement = MP_MOVE_NONE;
        if (gMain.inBattle)
            p->busy = MP_BUSY_BATTLE;
        else if (gSaveBlock1Ptr != NULL
              && ((u8)gSaveBlock1Ptr->location.mapGroup != p->mapGroup
               || (u8)gSaveBlock1Ptr->location.mapNum != p->mapNum))
            p->busy = MP_BUSY_WARPING;
        else
            p->busy = MP_BUSY_MENU;
        sIdleTicks = MP_IDLE_TICKS;
    }
    MP_SetLocalPresence(p);
}
