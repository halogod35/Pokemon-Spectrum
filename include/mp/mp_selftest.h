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

#endif // GUARD_MP_MP_SELFTEST_H
