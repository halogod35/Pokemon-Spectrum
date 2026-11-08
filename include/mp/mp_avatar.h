#ifndef GUARD_MP_MP_AVATAR_H
#define GUARD_MP_MP_AVATAR_H

// Remote avatars (PS-031): the pure decision logic, internal to src/mp. No game state
// is read here, so --mp-selftest can test it directly. The game-side code that applies
// these decisions to object events is src/mp/mp_avatars.c.

#include "gba/types.h"
#include "mp/mp.h"

#define MP_AVATAR_SNAP_TILES        2   // snap when the avatar is more than this behind
#define MP_AVATAR_RESERVE_SLOTS     2   // object slots always left free (scripts' addobject etc.)
#define MP_AVATAR_NPC_MARGIN        6   // tiles around the view in which unspawned NPCs are counted

// What the avatar should do next to follow one presence update.
enum
{
    MP_AVATAR_PLAN_DONE,    // already there and facing the right way: consume the update
    MP_AVATAR_PLAN_ACTION,  // start `action` (a MOVEMENT_ACTION_*); the update stays pending
                            // until the avatar's tile equals its x/y
    MP_AVATAR_PLAN_SNAP,    // teleport to x/y and face `facing` (warp, or too far / not reachable)
};

// localId <-> slot. MpAvatar_SlotForLocalId returns MP_SLOT_NONE if it is not an avatar id.
u8 MpAvatar_LocalIdForSlot(u8 slot);
u8 MpAvatar_SlotForLocalId(u8 localId);

// How many remote avatars may exist now. `activeOthers` = active object events that are
// not remote avatars (player, NPCs, camera...), `pendingNpcs` = unhidden map NPC templates
// near the view that are not spawned yet (they may need a slot soon). Never more than
// MP_MAX_REMOTE_AVATARS; always leaves MP_AVATAR_RESERVE_SLOTS free.
u8 MpAvatar_Budget(u8 activeOthers, u8 pendingNpcs);

// OBJ_EVENT_GFX_* for a remote player: Brendan / May by `gender` (anything but FEMALE = male)
// in the sprite of `avatarState` (MP_AVATAR_*; out of range = on foot).
u8 MpAvatar_GraphicsId(u8 gender, u8 avatarState);

// DIR_* of a 1-tile step from (0,0) to (dx,dy); the dominant axis wins, x on a tie.
// DIR_NONE for (0,0).
u8 MpAvatar_DirectionOf(s16 dx, s16 dy);

// MOVEMENT_ACTION_* for a 1-tile step in `dir` at the speed of `movement` (MP_MOVE_*).
// WALK_FAST is the running animation on foot, and fast walking on a bike / surfing.
u8 MpAvatar_StepAction(u8 movement, u8 avatarState, u8 dir);

// Plans the next action of an avatar standing (not mid-step) on map-local tile
// (fromX, fromY) facing `facing`, towards `target`. `newStep` = target->stepId differs from
// the last update the avatar finished (an in-place hop is only replayed once).
u8 MpAvatar_Plan(s16 fromX, s16 fromY, u8 facing, const struct MpPresence *target,
                 bool8 newStep, u8 *action);

// TRUE if the avatar on (x, y) is so far from the latest presence that it should snap.
bool8 MpAvatar_IsTooFarBehind(s16 x, s16 y, const struct MpPresence *latest);

// BUG-008. TRUE if `p` says the player is standing still: no step in flight (MP_MOVE_NONE,
// a turn, or an in-place hop). Keep-alives repeat the last value, so they are idle too
// once the player has stopped.
bool8 MpAvatar_IsIdlePresence(const struct MpPresence *p);

// BUG-008. Convergence plan for an avatar standing on (x, y) with nothing left to replay
// while its player is idle at `latest`: DONE if on the tile and facing the right way,
// ACTION with a FACE_* (same tile) or a 1-tile WALK_NORMAL (adjacent tile), SNAP otherwise.
u8 MpAvatar_PlanIdle(s16 x, s16 y, u8 facing, const struct MpPresence *latest, u8 *action);

// BUG-008. Bounded catch-up for a 1-tile step `action` started while the avatar is `behind`
// tiles (Manhattan) from the player's latest position. A normal walk 2 or more behind is
// a whole 16-frame step late (a burst after jitter), so it becomes WALK_FAST (same walking
// animation, twice the speed). Everything else is unchanged: 8-frame and faster steps are
// 2 behind from ordinary latency alone, and speeding them up would stutter. Further than
// MP_AVATAR_SNAP_TILES is a snap, not a catch-up.
u8 MpAvatar_CatchUpAction(u8 action, s16 behind);

// BUG-008 (selftest): game frames a MOVEMENT_ACTION_* used by the replay takes.
u8 MpAvatar_ActionFrames(u8 action);

// PS-032. Map-local (x, y) on a map connected to the current one in `direction`
// (CONNECTION_SOUTH/NORTH/WEST/EAST) with the connection's `offset`, translated into
// current-map coordinates, exactly as fieldmap.c lays connected maps out. FALSE for any
// other direction (dive / emerge are not side by side).
bool8 MpAvatar_TranslateConnection(u8 direction, s32 offset, s32 curWidth, s32 curHeight,
                                   s32 connWidth, s32 connHeight, s16 x, s16 y, s16 *outX, s16 *outY);

// PS-032. TRUE for maps where remote avatars are never shown (link rooms, Frontier / Tent
// challenge rooms, per-save generated layouts, contest stages, the intro truck).
bool8 MpAvatar_IsMapDenied(u8 mapGroup, u8 mapNum);

// PS-036. (x, y) moved `tiles` tiles in DIR_* `dir` (other directions: no move).
void MpAvatar_StepCoords(u8 dir, s16 x, s16 y, s16 tiles, s16 *outX, s16 *outY);

// PS-036. Door effect for an avatar step in `dir` from a tile to the next one. ENTER: the
// destination is a warp door and the step goes north into it (open the door, then hide the
// avatar on it, then close). EXIT: the step leaves a warp door southwards (show the avatar,
// open the door, close after the step). Mirrors field_control_avatar.c TryDoorWarp and
// field_screen_effect.c's door warp / exit sequences.
enum
{
    MP_AVATAR_DOOR_NONE,
    MP_AVATAR_DOOR_ENTER,
    MP_AVATAR_DOOR_EXIT,
};
u8 MpAvatar_DoorTransition(u8 dir, bool8 fromIsDoor, bool8 destIsDoor);
u32 MpAvatar_DeniedMapCount(void);      // selftest
u16 MpAvatar_DeniedMapAt(u32 index);    // selftest: MAP_* id, 0xFFFF past the end

#endif // GUARD_MP_MP_AVATAR_H
