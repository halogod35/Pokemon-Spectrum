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
// PS-046: LINK_* parser fuzzing and the virtual link cable on the real link stack (link.c):
// two game instances in one process (MP_LinkSelftestState), the host as sequencer only and as
// a member; player-data exchange, SendBlock, stall + catch-up, re-open, disconnect.
u32 MpSelftest_Link(void);

#endif // GUARD_MP_MP_SELFTEST_H
