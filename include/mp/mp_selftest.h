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
// PS-054: dropouts. RESUME / PLAYER_STATUS parsers and tokens; host + 2 clients with a simulated
// signal loss on one client: 1 s and 5 s at real timing, refused tokens (wrong, replayed, other
// save), then "20 s", mid-trade and "40 s" with every PS-054 timer scaled down 10x.
u32 MpSelftest_Reconnect(void);
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
// PS-042 / PS-043: TRAINER_CARD_DATA and PARTY_DATA parser fuzzing, the game-side checks
// (MpPlayerData_SelfTest), and an in-process host + 2 clients scenario (cards and parties
// client <-> client and client <-> host, consent / deny, tampered card and mon, empty party,
// requester leaving mid-transfer).
u32 MpSelftest_View(void);
// PS-042 / PS-043 (src/mp/mp_playerdata.c): builds a card from in-memory test save blocks (the
// save is left untouched, the result passes the parser), checks the conversion to struct
// TrainerCard, MP_ValidateMon (targeted fields, every bit flip, garbage) and MP_BuildLocalParty.
u32 MpPlayerData_SelfTest(void);
// PS-043 selftest helper (mp_playerdata.c): a valid raw mon (MP_MON_WIRE_SIZE bytes) made by
// CreateMon with a fixed personality, IVs and OT, without touching the real save.
void MpPlayerData_MakeTestMon(u8 *raw, u16 species, u8 level, u32 personality);
// PS-046: LINK_* parser fuzzing and the virtual link cable on the real link stack (link.c):
// two game instances in one process (MP_LinkSelftestState), the host as sequencer only and as
// a member; player-data exchange, SendBlock, stall + catch-up, re-open, disconnect.
u32 MpSelftest_Link(void);
// PS-047: the hardened link receive path: link.c ProcessRecvCmds, the battle link receive
// path and controller filter, MP_LinkValidateMon and friends, text buffers, the bus pre-filter.
u32 MpSelftest_LinkHardening(void);
// PS-044: TRADE_COMMIT parser fuzzing, the host-arbitrated commit logic (pure + random-event
// fuzz) and host + 2 clients in process: both ready -> both commit; a member that drops before
// commit -> neither commits; in doubt -> presumed abort.
u32 MpSelftest_Trade(void);
// PS-045 direct link battle: party rules, INTERACT kind BATTLE_DOUBLE (every kind byte, fuzz),
// the linkup state machine (pure + random-input fuzz), and in process a link group that ends
// (or a partner that leaves the session) during the start.
u32 MpSelftest_Battle(void);
// PS-053: the netstats counters, and the traffic benchmark (host + 1 and host + 3 clients over
// loopback at 60 Hz: idle, walk, bike, view, trade, battle). Prints the "[MP] netbench:" tables.
u32 MpSelftest_NetBench(void);

#endif // GUARD_MP_MP_SELFTEST_H
