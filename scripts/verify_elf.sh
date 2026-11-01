#!/usr/bin/env bash
set -euo pipefail

library=${1:?usage: verify_elf.sh library.so [--android]}
test -s "$library"
header=$(readelf --wide -h "$library")
grep -Eq 'Type:[[:space:]]+DYN' <<< "$header"
if [[ ${2:-} == --android ]]; then
    grep -Eq 'Machine:[[:space:]]+AArch64' <<< "$header"
fi
symbols=$(readelf --wide --dyn-syms "$library")
awk '
    $1 ~ /^[0-9]+:$/ && $7 != "UND" && ($5 == "GLOBAL" || $5 == "WEAK") {
        if ($8 == "zygisk_module_entry" && $4 == "FUNC") entry = 1
        else { print "unexpected exported symbol: " $8 > "/dev/stderr"; invalid = 1 }
    }
    END {
        if (!entry) print "missing Zygisk entry point" > "/dev/stderr"
        exit invalid || !entry
    }
' <<< "$symbols"
echo 'ELF export checks passed'
