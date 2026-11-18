#!/usr/bin/env bash
# PS-090: specials must not return 8-bit types.
#
# `specialvar VAR, Special` calls the special through `u16 (*)(void)` (src/scrcmd.c) and stores 16 bits.
# On x86 (32- and 64-bit) a function that returns bool8/u8/s8 only defines the low 8 bits of eax;
# bits 8-15 are whatever the callee left there, so the var gets garbage in its upper byte
# (the Day Care lady said "You don't have enough money" with 31,171). The GBA (ARM) zero-extended
# for free, which is why vanilla never saw it. Every special therefore returns bool16/u16/s16 or
# a 32-bit type, and this script fails CI when one does not.
#
# What it does: takes every `def_special Name` from data/specials.inc, finds the definition line
# `[static] type [UNUSED] Name(` in src/**/*.c, and fails with the offending names when the type is
# bool8/u8/s8 (or char), or when a special has no definition the script understands (so the regex
# cannot rot silently).
#
# Usage: tools/check_specials.sh [repo-root]        (also `make checkspecials`)
# Needs only bash, grep, sed, awk, sort, mktemp. Exit 0 clean, 1 offending or missing specials, 2 usage.

set -u

root=${1:-$(cd "$(dirname "$0")/.." && pwd)}
cd "$root" || { echo "::error::check_specials: cannot cd to $root"; exit 2; }

specials=data/specials.inc
if [ ! -r "$specials" ]; then
    echo "::error::check_specials: $specials not found (run from the repo root)"
    exit 2
fi

tmp_names=$(mktemp) || exit 2
tmp_defs=$(mktemp) || exit 2
trap 'rm -f "$tmp_names" "$tmp_defs"' EXIT

# 1. Special names. The table lines are "def_special Name" or "def_special Name, waitstate=1";
#    the macro's own definition line starts with ".macro" and is skipped by the anchor.
grep -E '^[[:space:]]*def_special[[:space:]]' "$specials" \
    | sed -E 's/^[[:space:]]*def_special[[:space:]]+([A-Za-z_][A-Za-z0-9_]*).*/\1/' \
    | sort -u > "$tmp_names"

if [ ! -s "$tmp_names" ]; then
    echo "::error::check_specials: no def_special lines found in $specials"
    exit 1
fi

# 2. One pass over src/: candidate definition lines "type Name(" at the start of a line, with an
#    optional "static" and an optional UNUSED attribute after the type, and no ';' on the line
#    (that excludes prototypes and calls). Output "Name type", one per line.
grep -rhE '^[[:space:]]*(static[[:space:]]+)?[A-Za-z_][A-Za-z0-9_]*([[:space:]]+UNUSED)?[[:space:]]+\*?[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\([^;]*$' src --include='*.c' \
    | sed -E 's/^[[:space:]]*(static[[:space:]]+)?//; s/[[:space:]]+UNUSED[[:space:]]+/ /; s/[[:space:]]+\*/ ptr_/; s/[[:space:]]*\(.*$//' \
    | awk 'NF == 2 && $1 != "return" && $1 != "else" && $1 != "case" && $1 != "goto" && $1 != "sizeof" { print $2, $1 }' \
    | sort -u > "$tmp_defs"

# 3. Match them up.
report=$(awk '
    NR == FNR { types[$1] = types[$1] " " $2; next }
    {
        name = $1
        total++
        if (!(name in types))
        {
            printf "MISSING %s\n", name
            next
        }
        n = split(types[name], t, " ")
        for (i = 1; i <= n; i++)
            if (t[i] ~ /^(bool8|u8|s8|char)$/)
                printf "NARROW %s %s\n", name, t[i]
    }
    END { printf "TOTAL %d\n", total }' "$tmp_defs" "$tmp_names")

fail=0
total=0
while read -r kind name type; do
    case $kind in
        TOTAL)
            total=$name
            ;;
        MISSING)
            echo "::error::check_specials: no definition of special $name found in src/ (unusual definition shape? fix the regex in tools/check_specials.sh)"
            fail=1
            ;;
        NARROW)
            echo "::error::check_specials: special $name returns $type; specialvar reads 16 bits, so it must return bool16/u16/s16 (PS-090, see src/scrcmd.c SpecialFunc)"
            fail=1
            ;;
    esac
done <<EOF
$report
EOF

if [ "$fail" -ne 0 ]; then
    echo "check_specials: FAILED ($total specials checked)"
    exit 1
fi

echo "check_specials: OK, $total specials all return 16- or 32-bit types"
exit 0
