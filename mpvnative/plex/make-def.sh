#!/bin/sh
# make-def.sh <a libavfilter-8 DLL of the same branch> <out.def>: the proxy's export table: every export of libavfilter
# forwarded to avfilter-8-plex.dll, except avfilter_get_by_name (implemented in avfilter_proxy.c). Needs gendef (MSYS2).
set -eu
dll=$1; out=$2; t=$(mktemp -d)
(cd "$t" && gendef - "$dll" > exports.def)
{
    echo "LIBRARY avfilter-8.dll"
    echo "EXPORTS"
    sed -n '/^EXPORTS/,$p' "$t/exports.def" | tail -n +2 | sed 's/[[:space:]].*//; s/@.*//' | grep -v '^$' | sort -u | while read -r s; do
        case $s in
            avfilter_get_by_name) echo "    avfilter_get_by_name" ;;
            *) echo "    $s=avfilter-8-plex.$s" ;;
        esac
    done
    echo "    pushmpc_proxy_status"
} > "$out"
rm -rf "$t"
echo "$(grep -c '=avfilter-8-plex' "$out") exports forwarded"
