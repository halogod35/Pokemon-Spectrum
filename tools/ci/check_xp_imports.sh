#!/usr/bin/env bash
# PS-083: Windows XP import audit for the CI Windows jobs.
#
# Prints every DLL and every symbol the exe imports (from `objdump -p`) and fails when
#   - the exe imports a DLL outside the allowlist below (Windows XP does not have it), or
#   - the exe imports a symbol from the Vista-or-later list below.
# SDL2.dll, when given, is only listed for the log (SDL 2.0.16 loads its newer APIs dynamically).
#
# Usage: tools/ci/check_xp_imports.sh <objdump> <exe> [SDL2.dll]
#   e.g. tools/ci/check_xp_imports.sh i686-w64-mingw32-objdump pokemon-spectrum32.exe SDL2/i686-w64-mingw32/bin/SDL2.dll

set -u

objdump=${1:?usage: check_xp_imports.sh <objdump> <exe> [SDL2.dll]}
exe=${2:?usage: check_xp_imports.sh <objdump> <exe> [SDL2.dll]}
sdl2=${3:-}

# DLLs that exist on Windows XP (compared case-insensitively). Extend only with a comment saying why the
# DLL exists on XP.
allowed_dlls="kernel32.dll user32.dll gdi32.dll advapi32.dll shell32.dll ws2_32.dll winmm.dll msvcrt.dll sdl2.dll"

# Symbols that Windows Vista or later introduced. Importing one makes the exe fail to load on XP.
vista_symbols="inet_pton inet_ntop WSAPoll GetTickCount64 SetProcessDPIAware InitializeSRWLock
AcquireSRWLockExclusive InitializeConditionVariable SleepConditionVariableCS GetSystemTimePreciseAsFileTime
QueryUnbiasedInterruptTime SHGetKnownFolderPath K32GetProcessMemoryInfo SetThreadDescription
CreateWaitableTimerExW GetFinalPathNameByHandleA GetFinalPathNameByHandleW RtlGenRandom BCryptGenRandom"

# Prints "DLL symbol" pairs for every named import in a PE file. objdump -p lists, per DLL, either
#     DLL Name: KERNEL32.dll
#     vma:  Hint/Ord Member-Name Bound-To
#     a2d0       85  CloseHandle
# (older binutils) or, with an Ordinal column (binutils 2.39 and later),
#     vma:     Ordinal  Hint  Member-Name  Bound-To
#     a2d0       <none>    85  CloseHandle
# so the name is the first field after the vma that looks like an identifier (ordinal-only imports
# have no name and are skipped).
imports_of()
{
    "$objdump" -p "$1" | awk '
        /DLL Name:/ { dll = $3; next }
        dll != "" && $1 ~ /^[0-9a-f]+$/ {
            for (i = 2; i <= NF; i++)
                if ($i ~ /^[A-Za-z_?@$][A-Za-z0-9_?@$]*$/) { print dll, $i; break }
        }
    '
}

# The raw import tables, for the log, so a parser mismatch can be seen and fixed.
raw_imports_of()
{
    "$objdump" -p "$1" | awk '/The Import Tables/ { p = 1 } /The Function Table|The Export Tables/ { p = 0 } p'
}

dlls_of()
{
    "$objdump" -p "$1" | awk '/DLL Name:/ { print $3 }'
}

fail=0

echo "=== XP import audit: $exe ==="
if [ ! -f "$exe" ]; then
    echo "::error::check_xp_imports: $exe not found"
    exit 1
fi

dlls=$(dlls_of "$exe")
imports=$(imports_of "$exe")

echo "--- imported DLLs"
echo "$dlls" | sed 's/^/    /'

echo "--- imported symbols"
echo "$imports" | awk 'NF { print "    " $1 "  " $2 }'
nsyms=$(echo "$imports" | grep -c .)
echo "    ($nsyms named imports from $(echo "$dlls" | grep -c .) DLLs)"

echo "--- raw import tables (objdump -p)"
raw_imports_of "$exe" | sed 's/^/    /'

echo "--- verdict"
if [ "$nsyms" = 0 ]; then
    echo "::error::check_xp_imports: parsed no imported symbols from $exe; the objdump -p layout changed, fix imports_of()"
    fail=1
fi
for dll in $dlls; do
    lower=$(echo "$dll" | tr '[:upper:]' '[:lower:]')
    ok=0
    for a in $allowed_dlls; do
        [ "$lower" = "$a" ] && ok=1
    done
    if [ "$ok" = 0 ]; then
        echo "::error::$exe imports $dll, which Windows XP does not have (allowlist: $allowed_dlls)"
        fail=1
    fi
done

for sym in $vista_symbols; do
    hit=$(echo "$imports" | awk -v s="$sym" '$2 == s { print $1 }')
    if [ -n "$hit" ]; then
        echo "::error::$exe imports $sym from $hit, which needs Windows Vista or later"
        fail=1
    fi
done

if [ "$fail" = 0 ]; then
    echo "    OK: every imported DLL is on the XP allowlist and no Vista+ symbol is imported"
fi

if [ -n "$sdl2" ]; then
    echo
    echo "=== for the log only: DLLs imported by $sdl2 (no fail rule) ==="
    if [ -f "$sdl2" ]; then
        dlls_of "$sdl2" | sed 's/^/    /'
    else
        echo "    (not found)"
    fi
fi

exit $fail
