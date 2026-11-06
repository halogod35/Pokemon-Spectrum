// Build identity for the handshake (protocol.md section 9).
// The only file compiled with -DMP_BUILD_ID / -DMP_BUILD_DIRTY (see Makefile),
// so a new commit rebuilds just this file.

#include <string.h>
#include "mp/mp.h"
#include "mp/mp_internal.h"

#ifndef MP_BUILD_ID
#define MP_BUILD_ID "nogit"
#endif
#ifndef MP_BUILD_DIRTY
#define MP_BUILD_DIRTY 0
#endif

static char sBuildId[MP_BUILD_ID_MAX_LEN + 1];
static u16 sProtocolVersion = MP_PROTOCOL_VERSION;
static u32 sBuildHash;
static bool8 sInitialized = FALSE;

// Copies at most MP_BUILD_ID_MAX_LEN printable ASCII chars; anything else ends the id.
static void MpSetBuildIdString(const char *id)
{
    u32 i;

    for (i = 0; i < MP_BUILD_ID_MAX_LEN && id != NULL && id[i] > ' ' && id[i] < 0x7F; i++)
        sBuildId[i] = id[i];
    sBuildId[i] = '\0';
}

static u32 MpFnv1aByte(u32 hash, u8 byte)
{
    return (hash ^ byte) * 0x01000193u;
}

static void MpComputeBuildHash(void)
{
    u32 hash = 0x811C9DC5u;
    u32 i;

    hash = MpFnv1aByte(hash, (u8)(sProtocolVersion & 0xFF));
    hash = MpFnv1aByte(hash, (u8)(sProtocolVersion >> 8));
    for (i = 0; sBuildId[i] != '\0'; i++)
        hash = MpFnv1aByte(hash, (u8)sBuildId[i]);
    sBuildHash = hash;
}

void MpBuild_Init(void)
{
    if (sInitialized)
        return;
    sInitialized = TRUE;
    MpSetBuildIdString(MP_BUILD_ID);
    MpComputeBuildHash();
    MP_DLOG("build id %s%s, protocol %u, hash 0x%08X\n", sBuildId, MP_BUILD_DIRTY ? " (dirty)" : "",
            (unsigned)sProtocolVersion, (unsigned)sBuildHash);
}

const char *MP_GetBuildId(void)
{
    MpBuild_Init();
    return sBuildId;
}

u32 MP_GetBuildHash(void)
{
    MpBuild_Init();
    return sBuildHash;
}

u16 MpBuild_GetProtocolVersion(void)
{
    return sProtocolVersion;
}

u8 MpBuild_IsDirty(void)
{
    return MP_BUILD_DIRTY ? 1 : 0;
}

void MpBuild_ClearOverrides(void)
{
    MpBuild_Init();
    sProtocolVersion = MP_PROTOCOL_VERSION;
    MpSetBuildIdString(MP_BUILD_ID);
    MpComputeBuildHash();
}

void MP_SetFakeProtocolVersion(u16 version)
{
    MpBuild_Init();
    sProtocolVersion = version;
    MpComputeBuildHash();
    MP_LOG("WARNING: fake protocol version %u in use (build hash now 0x%08X)\n", (unsigned)version, (unsigned)sBuildHash);
}

void MP_SetFakeBuildId(const char *buildId)
{
    MpBuild_Init();
    MpSetBuildIdString(buildId);
    MpComputeBuildHash();
    MP_LOG("WARNING: fake build id '%s' in use (build hash now 0x%08X)\n", sBuildId, (unsigned)sBuildHash);
}
