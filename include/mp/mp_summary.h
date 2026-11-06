#ifndef GUARD_MP_MP_SUMMARY_H
#define GUARD_MP_MP_SUMMARY_H

// Pokemon Spectrum multiplayer: player summary (PS-013).
//
// INTERNAL header: for src/mp/*.c only. Game code includes "mp/mp.h" only.
// Contract: docs/multiplayer/design.md section 2.1, docs/multiplayer/protocol.md section 5.1.
//
// Nothing declared here ever writes to a save block (AD-10). MP_BuildPlayerSummary only
// reads gSaveBlock2Ptr (name, gender, trainer id) and the badge flags.
// MP_SetNameOverride (declared in mp.h) is also implemented in mp_summary.c.

#include "mp/mp.h"

// Size of a PlayerSummary on the wire (protocol.md 5.1).
#define MP_PLAYER_SUMMARY_WIRE_SIZE 16

// Builds the local player's summary from the loaded save (read only), applying the
// --name override if one is set. The result always passes MP_ReadPlayerSummary's rules:
// a save name that would fail them is sanitized (see mp_summary.c). If no save is loaded
// (gSaveBlock2Ptr NULL), a placeholder summary is produced and a warning is logged.
void MP_BuildPlayerSummary(struct MpPlayerSummary *out);

// Writes the 16-byte wire form of *in to buf (little-endian, field by field). Names are
// written EOS-padded after their first EOS; reserved fields are written as 0.
// Returns MP_PLAYER_SUMMARY_WIRE_SIZE, or 0 (and writes nothing) if cap < 16 or a pointer is NULL.
u16 MP_WritePlayerSummary(u8 *buf, u16 cap, const struct MpPlayerSummary *in);

// Parses and validates a 16-byte wire summary (protocol.md 5.1 / 5.1.1). Pure: no global
// state, no I/O. Reads exactly 16 bytes; `len` is the number of bytes available at `buf`
// and must be >= 16 (exact message sizes are checked by the message parsers).
// Returns TRUE and fills *out on success. Returns FALSE on any error, and then *out is
// zeroed and must not be used.
bool8 MP_ReadPlayerSummary(const u8 *buf, u32 len, struct MpPlayerSummary *out);

// TRUE if `name` (MP_PLAYER_NAME_SIZE bytes, game charset) satisfies protocol.md 5.1.1.
bool8 MP_IsValidPlayerName(const u8 *name);

// Converts a game-charset name (reads at most MP_PLAYER_NAME_SIZE bytes, stops at EOS) to
// NUL-terminated ASCII for logs only. Characters without an ASCII equivalent become '?'.
// Always NUL-terminates when outSize > 0 (truncating if needed). outSize 0 writes nothing.
// A NULL gameName gives "".
void MP_NameToAscii(const u8 *gameName, char *out, u32 outSize);

// Self-test for this module (round trip, truncated / malformed buffers, name rules, fuzzing).
// Called by the --mp-selftest runner (PS-011, mp_selftest.c). Prints one "[MP] selftest
// summary: ..." line per group. Leaves every global it touches as it found it.
// Returns TRUE if every check passed.
bool8 MpSummary_SelfTest(void);

#endif // GUARD_MP_MP_SUMMARY_H
