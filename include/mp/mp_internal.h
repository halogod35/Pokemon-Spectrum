#ifndef GUARD_MP_MP_INTERNAL_H
#define GUARD_MP_MP_INTERNAL_H

// Shared internals of src/mp/*.c. Game code must not include this (use mp/mp.h).
// Deliberately does not include global.h, so it can sit next to <enet/enet.h>
// and the Windows headers in mp_transport.c.

#include <stdint.h>
#include <stdio.h>
#include "gba/types.h"
#include "gba/defines.h" // TRUE, FALSE, NULL

#define MP_EOS 0xFF // game charset string terminator (EOS), without pulling in global.h

// [MP] logging (design.md section 7).
// MP_LOG: always printed (session timeline, errors, warnings), to stdout and (PS-051) to the
// log file MpLog_Open() picked (mp_log.txt, or mp_log_<pid>.txt if another instance holds it).
// MP_DLOG: only with --mp-debug (MP_SetDebugLogging(TRUE)).
extern bool8 gMpDebugLogging;

// mp_log.c (PS-051)
void MpLog_Open(void);                  // MP_Init: create the log file, print its path
void MpLog_Close(void);                 // MP_Shutdown
bool8 MpLog_FileIsOpen(void);
const char *MpLog_GetPath(void);        // "" if no file
void MpLog_Printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));     // stdout + file
void MpLog_FilePrintf(const char *fmt, ...) __attribute__((format(printf, 1, 2))); // file only, not flushed
void MpLog_FileFlush(void);
uintptr_t MpLog_ExeBase(void);
u32 MpLog_ExeOffset(const void *addr);  // a code address as exe+offset (pokemon-spectrum.map / nm)

#define MP_LOG(...) MpLog_Printf("[MP] " __VA_ARGS__)

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
// mp_build.c: selftest only: drops --mp-fake-* overrides (sessions keep what they captured).
void MpBuild_ClearOverrides(void);

#endif // GUARD_MP_MP_INTERNAL_H
