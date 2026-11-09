// Pokemon Spectrum multiplayer: the [MP] log (PS-051 part A).
//
// Every MP_LOG line goes to stdout (as before) and, once MpLog_Open() ran (MP_Init), to a log
// file in the working directory, flushed per line, so a play-test always leaves a file behind
// even if the console is closed or the game is killed:
//   mp_log.txt            normally (truncated at every start);
//   mp_log_<pid>.txt      if another running instance already holds mp_log.txt (two instances
//                         on one PC must not clobber each other's log).
// The path is printed at startup ("[MP] log file: ...").
// The link flight recorder (mp_link_cable.c) writes its dumps here with MpLog_FilePrintf.
//
// No game state here (does not include global.h), so the Windows headers can be used.

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#include <share.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
#include "mp/mp_internal.h"

#define MP_LOG_NAME         "mp_log.txt"
#define MP_LOG_PATH_SIZE    1024

static FILE *sLogFile;
static char sLogPath[MP_LOG_PATH_SIZE];

static int MpLogPid(void)
{
#ifdef _WIN32
    return (int)_getpid();
#else
    return (int)getpid();
#endif
}

// Opens `name` for writing, truncated, unless another process holds it. NULL if held / failed.
static FILE *MpLogTryOpen(const char *name)
{
#ifdef _WIN32
    // Deny writers: a second instance's open fails instead of truncating our file.
    return _fsopen(name, "w", _SH_DENYWR);
#else
    int fd = open(name, O_WRONLY | O_CREAT, 0644);
    FILE *f;

    if (fd < 0)
        return NULL;
    // An advisory lock: a second instance sees it held and uses its own file. Only truncate
    // once we own the lock, so the other instance's log survives.
    if (flock(fd, LOCK_EX | LOCK_NB) != 0 || ftruncate(fd, 0) != 0)
    {
        close(fd);
        return NULL;
    }
    f = fdopen(fd, "w");
    if (f == NULL)
        close(fd);
    return f;
#endif
}

static void MpLogMakeAbsolute(const char *name)
{
    char cwd[MP_LOG_PATH_SIZE - 64];
    const char *dir = NULL;

#ifdef _WIN32
    if (_getcwd(cwd, (int)sizeof(cwd)) != NULL)
        dir = cwd;
#else
    if (getcwd(cwd, sizeof(cwd)) != NULL)
        dir = cwd;
#endif
    if (dir != NULL)
    {
#ifdef _WIN32
        snprintf(sLogPath, sizeof(sLogPath), "%s\\%s", dir, name);
#else
        snprintf(sLogPath, sizeof(sLogPath), "%s/%s", dir, name);
#endif
    }
    else
    {
        snprintf(sLogPath, sizeof(sLogPath), "%s", name);
    }
}

void MpLog_Open(void)
{
    char name[64];

    if (sLogFile != NULL)
        return;
    snprintf(name, sizeof(name), "%s", MP_LOG_NAME);
    sLogFile = MpLogTryOpen(name);
    if (sLogFile == NULL)
    {
        snprintf(name, sizeof(name), "mp_log_%d.txt", MpLogPid());
        sLogFile = MpLogTryOpen(name);
    }
    if (sLogFile == NULL)
    {
        sLogPath[0] = '\0';
        MP_LOG("log file: could not create %s or %s in the working directory; logging to the console only\n", MP_LOG_NAME, name);
        return;
    }
    MpLogMakeAbsolute(name);
    {
        time_t now = time(NULL);
        char when[64];

        if (strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime(&now)) == 0)
            snprintf(when, sizeof(when), "?");
        MP_LOG("log file: %s (started %s, pid %d; send this file with any multiplayer bug report)\n", sLogPath, when, MpLogPid());
    }
}

void MpLog_Close(void)
{
    if (sLogFile != NULL)
    {
        fclose(sLogFile);
        sLogFile = NULL;
    }
}

bool8 MpLog_FileIsOpen(void)
{
    return sLogFile != NULL;
}

const char *MpLog_GetPath(void)
{
    return sLogPath;
}

void MpLog_Printf(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
    if (sLogFile != NULL)
    {
        va_start(args, fmt);
        vfprintf(sLogFile, fmt, args);
        va_end(args);
        fflush(sLogFile);
    }
}

void MpLog_FilePrintf(const char *fmt, ...)
{
    va_list args;

    if (sLogFile == NULL)
        return;
    va_start(args, fmt);
    vfprintf(sLogFile, fmt, args);
    va_end(args);
}

void MpLog_FileFlush(void)
{
    if (sLogFile != NULL)
        fflush(sLogFile);
}

// A code address as an offset from the exe's load address, so the shipped pokeemerald.map (or
// nm on Linux) resolves it: Windows offset + 0x140000000 (64-bit) / + 0x400000 (32-bit) is the
// map address, as for the crash reporter (BUG-003). 0 stays 0 (a NULL callback).
#ifndef _WIN32
extern char __executable_start; // GNU ld: the first byte of the executable's image
#endif

uintptr_t MpLog_ExeBase(void)
{
#ifdef _WIN32
    return (uintptr_t)GetModuleHandleA(NULL);
#else
    return (uintptr_t)&__executable_start;
#endif
}

u32 MpLog_ExeOffset(const void *addr)
{
    uintptr_t a = (uintptr_t)addr;
    uintptr_t base = MpLog_ExeBase();

    if (a == 0)
        return 0;
    if (a < base)
        return 0xFFFFFFFFu; // not in the image (should not happen for a game function)
    return (u32)(a - base);
}
