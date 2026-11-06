#ifndef GUARD_MP_MP_INTERNAL_H
#define GUARD_MP_MP_INTERNAL_H

// Shared internals of src/mp/*.c. Game code must not include this (use mp/mp.h).
// Deliberately does not include global.h, so it can sit next to <enet/enet.h>
// and the Windows headers in mp_transport.c.

#include <stdio.h>
#include "gba/types.h"
#include "gba/defines.h" // TRUE, FALSE, NULL

// [MP] logging (design.md section 7).
// MP_LOG: always printed (session timeline, errors, warnings).
// MP_DLOG: only with --mp-debug (MP_SetDebugLogging(TRUE)).
extern bool8 gMpDebugLogging;

#define MP_LOG(...)                 \
    do                              \
    {                               \
        printf("[MP] " __VA_ARGS__); \
        fflush(stdout);             \
    } while (0)

#define MP_DLOG(...)                \
    do                              \
    {                               \
        if (gMpDebugLogging)        \
            MP_LOG(__VA_ARGS__);    \
    } while (0)

// mp_build.c: the protocol version this instance advertises and expects
// (MP_PROTOCOL_VERSION unless --mp-fake-protocol is in use).
u16 MpBuild_GetProtocolVersion(void);
// mp_build.c: 1 if the build was made from a tree with uncommitted changes.
u8 MpBuild_IsDirty(void);
// mp_build.c: computes and logs the build identity. Called by MP_Init.
void MpBuild_Init(void);

#endif // GUARD_MP_MP_INTERNAL_H
