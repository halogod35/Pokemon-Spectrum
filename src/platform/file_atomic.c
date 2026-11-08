// MP: PS-050. Small crash-safe files next to the save (the "<save>.trade" journal), with the
// same method as PS-014's save write: write "<path>.tmp", flush, sync to disk, then atomically
// replace <path>. Plain C stdio, so it builds for every platform backend.
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include "global.h"
#include "platform.h"

#define FILE_ATOMIC_TMP_SUFFIX  ".tmp"

bool8 Platform_WriteFileAtomic(const char *path, const void *data, u32 size)
{
    char tmpPath[PLATFORM_ARG_PATH_SIZE + 32];
    FILE *file;
    bool8 ok = TRUE;
    int len;

    len = snprintf(tmpPath, sizeof(tmpPath), "%s%s", path, FILE_ATOMIC_TMP_SUFFIX);
    if (len < 0 || (size_t)len >= sizeof(tmpPath))
        return FALSE;
    file = fopen(tmpPath, "wb");
    if (file == NULL)
        return FALSE;
    if (size != 0 && fwrite(data, 1, size, file) != size)
        ok = FALSE;
    if (fflush(file) != 0)
        ok = FALSE;
#ifdef _WIN32
    if (ok && _commit(_fileno(file)) != 0)
        ok = FALSE;
#else
    if (ok && fsync(fileno(file)) != 0)
        ok = FALSE;
#endif
    if (fclose(file) != 0)
        ok = FALSE;
    if (!ok)
    {
        remove(tmpPath);
        return FALSE;
    }
#ifdef _WIN32
    if (!MoveFileExA(tmpPath, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        remove(tmpPath);
        return FALSE;
    }
#else
    if (rename(tmpPath, path) != 0)
    {
        remove(tmpPath);
        return FALSE;
    }
#endif
    return TRUE;
}

s32 Platform_ReadSmallFile(const char *path, void *buf, u32 cap)
{
    FILE *file = fopen(path, "rb");
    size_t got;
    u8 extra;

    if (file == NULL)
        return -1;
    got = fread(buf, 1, cap, file);
    // One byte more than `cap` means the file is too big: report cap + 1.
    if (got == cap && fread(&extra, 1, 1, file) == 1)
        got = (size_t)cap + 1;
    fclose(file);
    return (s32)got;
}

bool8 Platform_RemoveFile(const char *path)
{
    FILE *file = fopen(path, "rb");

    if (file == NULL)
        return TRUE; // already gone
    fclose(file);
    return remove(path) == 0;
}
