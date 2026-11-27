#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# glibc-ceiling.sh [--max <version>] <file>...: fails (exit 1) when an ELF file needs a GLIBC_ symbol version above the
# ceiling (default 2.35: Ubuntu 22.04 and Snap's core22, below Debian 12's 2.36), naming the symbols that do. Files
# without glibc symbol versions (not ELF, static) are listed and pass. Needs objdump (binutils).
set -eu
max=2.35
if [ "${1:-}" = --max ]; then max=$2; shift 2; fi
bad=0
for f in "$@"; do
    [ -f "$f" ] || { echo "::error::$f: missing"; bad=1; continue; }
    v=$(objdump -T "$f" 2>/dev/null | grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/^GLIBC_//' | sort -uV | tail -n 1)
    if [ -z "$v" ]; then echo "$f: no glibc symbol versions"; continue; fi
    if [ "$(printf '%s\n%s\n' "$v" "$max" | sort -V | tail -n 1)" = "$max" ]; then
        echo "$f: GLIBC_$v (ceiling $max)"
    else
        syms=$(objdump -T "$f" | awk -v m="$max" '{ for (i = 1; i <= NF; i++) if ($i ~ /^\(?GLIBC_[0-9]/) { s = $i; gsub(/[()]/, "", s); print s " " $NF } }' \
            | while read -r ver sym; do n=${ver#GLIBC_}; [ "$(printf '%s\n%s\n' "$n" "$max" | sort -V | tail -n 1)" = "$max" ] || printf '%s@%s ' "$sym" "$ver"; done)
        echo "::error::$f needs GLIBC_$v, above the ceiling $max (Snap core22 / Ubuntu 22.04 would not load it): $syms"
        bad=1
    fi
done
exit $bad
