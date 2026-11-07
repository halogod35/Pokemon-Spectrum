#ifndef GUARD_MP_MP_SELFTEST_H
#define GUARD_MP_MP_SELFTEST_H

// --mp-selftest groups (internal to src/mp). Each returns the number of failed checks.

#include "gba/types.h"

// PS-012: protocol.md 8.3 parser fuzzing (round trip, truncated, oversized, bit flips,
// garbage, targeted cases) for every message parser.
u32 MpSelftest_Parsers(void);
// PS-012: in-process session scenario (host + 3 clients, rejects, kick, leave, timeouts,
// UDP flood, host BYE, host loss).
u32 MpSelftest_Session(void);
// PS-030: PRESENCE parser fuzzing, MP_IsValidMap, seq wraparound, the receive paths
// (stale / duplicate / foreign epoch / overflow), and an in-process host + clients scenario
// with relay order, cached presence for a late joiner and the < 2 KB/s walking budget.
u32 MpSelftest_Presence(void);
// PS-031: remote-avatar logic (localId mapping, object budget, graphics ids, movement-action
// translation, the replay plan and snap rule, a replay simulation).
u32 MpSelftest_Avatar(void);
// PS-041: INTERACT_* parser fuzzing and an in-process host + 2 clients scenario (request ->
// accept / decline, busy, timeout, cancel, rate limit, other map, malformed, requester leaving).
u32 MpSelftest_Interact(void);
// PS-042: TRAINER_CARD_DATA parser fuzzing, the game-side card checks
// (MpPlayerData_SelfTest), and an in-process host + 2 clients scenario (client <-> client,
// client <-> host, card while the target has a prompt, tampered card, requester leaving).
u32 MpSelftest_View(void);
// PS-042 (src/mp/mp_playerdata.c): builds a card from in-memory test save blocks (the save is
// left untouched, the result passes the parser) and checks the conversion to struct TrainerCard.
u32 MpPlayerData_SelfTest(void);

#endif // GUARD_MP_MP_SELFTEST_H
