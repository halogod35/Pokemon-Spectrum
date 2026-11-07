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

#endif // GUARD_MP_MP_AVATAR_H
