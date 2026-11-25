#!/bin/sh
# nvidia-vfx-fetch.sh: installs NVIDIA's Video Effects runtime (RTX VSR, RTX Video HDR, VFG) for PushMPC's Linux NVIDIA
# backend, user-level, from NVIDIA's own PyPI index. PushMPC never redistributes these files: this downloads the exact
# wheel pinned in nvidia-vfx-pin.json, checks it against the pin's SHA-256 (and the index's published hash when the
# index is reachable), extracts only the pinned files and checks each one's SHA-256 and size before anything goes live.
#
#   sh nvidia-vfx-fetch.sh [--dest DIR] [--pin FILE] [--accept-nvidia-license] [--force] [--keep-wheel] [--quiet]
#   sh nvidia-vfx-fetch.sh --check [--dest DIR]        verify an installed runtime (exit 0 good, 1 bad or missing)
#   sh nvidia-vfx-fetch.sh --uninstall [--dest DIR]
#
# DIR defaults to $XDG_DATA_HOME/pushmpc/nvidia-vfx, else ~/.local/share/pushmpc/nvidia-vfx (where the plugin looks;
# PUSHMPC_NVVFX_DIR overrides both here and in the plugin). The layout is the wheel's (nvvfx/libs, nvidia_vfx.libs:
# the libraries' RPATH needs it) plus licenses/. Needs: curl or wget, sha256sum (or shasum), and unzip, bsdtar or
# python3 to extract. Without an NVIDIA driver it does nothing (exit 0) unless --force.
# NVIDIA's licence (NVIDIA Software License Agreement + Product-Specific Terms for NVIDIA AI Products) must be accepted:
# --accept-nvidia-license, or interactively. Exit codes: 0 done / skipped, 1 failed, 3 no wheel for this machine,
# 4 licence not accepted. The installer (T4's install.sh) calls it as a separate step.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
pin="$here/nvidia-vfx-pin.json"
dest="${PUSHMPC_NVVFX_DIR:-}"
[ -n "$dest" ] || { if [ -n "${XDG_DATA_HOME:-}" ]; then dest="$XDG_DATA_HOME/pushmpc/nvidia-vfx"; else dest="$HOME/.local/share/pushmpc/nvidia-vfx"; fi; }
accept=0; force=0; keep=0; quiet=0; mode=install
while [ $# -gt 0 ]; do
    case "$1" in
        --dest) dest=$2; shift ;;
        --pin) pin=$2; shift ;;
        --accept-nvidia-license) accept=1 ;;
        --force) force=1 ;;
        --keep-wheel) keep=1 ;;
        --quiet) quiet=1 ;;
        --check) mode=check ;;
        --uninstall) mode=uninstall ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "nvidia-vfx-fetch: unknown option $1" >&2; exit 1 ;;
    esac
    shift
done
say() { [ "$quiet" = 1 ] || echo "nvidia-vfx-fetch: $*"; }
die() { echo "nvidia-vfx-fetch: $*" >&2; exit 1; }

case "$(uname -m)" in
    x86_64|amd64) plat=linux_x86_64 ;;
    *) echo "nvidia-vfx-fetch: NVIDIA publishes no Linux $(uname -m) wheel of the Video Effects runtime" >&2; exit 3 ;;
esac
[ -f "$pin" ] || die "pin file $pin not found"

# the pin's block for this platform (one JSON object per line, written that way on purpose)
block=$(sed -n "/\"$plat\": {/,/^    }/p" "$pin")
field() { printf '%s\n' "$block" | sed -n "s/^      \"$1\": \"\\([^\"]*\\)\".*/\\1/p" | head -n 1; }
wheel=$(field wheel); url=$(field url); wsha=$(field sha256 | tr 'a-f' 'A-F')
version=$(sed -n 's/^  "version": "\([^"]*\)".*/\1/p' "$pin")
[ -n "$wheel" ] && [ -n "$url" ] && [ -n "$wsha" ] || die "no $plat wheel in $pin"
files=$(printf '%s\n' "$block" | sed -n 's/^ *{ "name": "\([^"]*\)", "to": "\([^"]*\)", "size": \([0-9]*\), "sha256": "\([0-9A-Fa-f]*\)" }.*/\1|\2|\3|\4/p')
[ -n "$files" ] || die "the pin lists no files for $plat"

sha256() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi | tr 'a-f' 'A-F'; }
fsize() { wc -c < "$1" | tr -d ' '; }
verify_tree() {   # $1 = root; prints the first problem
    printf '%s\n' "$files" | while IFS='|' read -r name to size hash; do
        f="$1/$to"
        [ -f "$f" ] || { echo "missing $to"; return 1; }
        [ "$(fsize "$f")" = "$size" ] || { echo "size of $to"; return 1; }
        [ "$(sha256 "$f")" = "$(echo "$hash" | tr 'a-f' 'A-F')" ] || { echo "SHA-256 of $to"; return 1; }
    done
}

if [ "$mode" = uninstall ]; then rm -rf "$dest"; say "removed $dest"; exit 0; fi
if [ "$mode" = check ]; then
    [ -d "$dest" ] || { echo "nvidia-vfx-fetch: not installed in $dest"; exit 1; }
    if problem=$(verify_tree "$dest"); then say "runtime $version in $dest: all $(printf '%s\n' "$files" | wc -l | tr -d ' ') files verified"; exit 0; fi
    echo "nvidia-vfx-fetch: $dest: $problem"; exit 1
fi

# an NVIDIA driver (the proprietary one: libcuda.so.1) or nothing to do
if [ "$force" != 1 ]; then
    if ! { [ -e /proc/driver/nvidia/version ] || command -v nvidia-smi >/dev/null 2>&1; }; then
        say "no NVIDIA proprietary driver found: skipped (PushMPC uses its other backends)"; exit 0
    fi
fi
# already there and intact
if [ -d "$dest" ] && verify_tree "$dest" >/dev/null 2>&1 && [ "$(cat "$dest/.version" 2>/dev/null || true)" = "$version" ]; then
    say "runtime $version already installed and verified in $dest"; exit 0
fi

if [ "$accept" != 1 ]; then
    echo "PushMPC's NVIDIA backend uses NVIDIA's Video Effects runtime (nvidia-vfx $version, about 450 MB download)."
    echo "It is downloaded from NVIDIA (pypi.nvidia.com) under NVIDIA's licence: the NVIDIA Software License Agreement and"
    echo "the Product-Specific Terms for NVIDIA AI Products (copies go to $dest/licenses)."
    if [ -t 0 ]; then
        printf 'Accept NVIDIA'"'"'s licence and download? [y/N] '
        read -r answer
        case "$answer" in y|Y|yes|YES) ;; *) echo "nvidia-vfx-fetch: not accepted: skipped"; exit 4 ;; esac
    else
        echo "nvidia-vfx-fetch: run with --accept-nvidia-license to accept it" >&2; exit 4
    fi
fi

cache="${XDG_CACHE_HOME:-$HOME/.cache}/pushmpc"
mkdir -p "$cache" "$(dirname "$dest")"
whl="$cache/$wheel"
fetch() {   # url out
    if command -v curl >/dev/null 2>&1; then
        if [ "$quiet" = 1 ]; then curl -fsL --retry 3 --connect-timeout 30 -o "$2" "$1"; else curl -fL --retry 3 --connect-timeout 30 -o "$2" "$1"; fi
    else wget -q -O "$2" "$1"; fi
}
# the index's own hash for this wheel must match the pin (skipped with a note when the index cannot be read)
idx="$cache/nvidia-vfx-index.html"
if fetch "https://pypi.nvidia.com/nvidia-vfx/" "$idx" 2>/dev/null; then
    ihash=$(sed -n "s/.*$wheel#sha256=\([0-9a-fA-F]\{64\}\).*/\1/p" "$idx" | head -n 1 | tr 'a-f' 'A-F')
    [ -z "$ihash" ] && die "the index no longer lists $wheel (yanked?): not installed"
    [ "$ihash" = "$wsha" ] || die "the index's SHA-256 for $wheel ($ihash) differs from the pin ($wsha): not installed"
    rm -f "$idx"
else
    say "note: the index page could not be read; checking against the pin's SHA-256 only"
fi
if [ ! -f "$whl" ] || [ "$(sha256 "$whl")" != "$wsha" ]; then
    say "downloading $wheel"
    fetch "$url" "$whl.part" || die "download failed: $url"
    mv -f "$whl.part" "$whl"
fi
got=$(sha256 "$whl")
[ "$got" = "$wsha" ] || { rm -f "$whl"; die "SHA-256 mismatch for $wheel: $got (pin $wsha)"; }
say "wheel verified ($wsha)"

stage="$dest.new.$$"
rm -rf "$stage"; mkdir -p "$stage"
trap 'rm -rf "$stage"' EXIT
names=$(printf '%s\n' "$files" | cut -d'|' -f1)
x="$stage/.x"; mkdir -p "$x"
if command -v unzip >/dev/null 2>&1; then
    # shellcheck disable=SC2086
    unzip -q -o "$whl" $names -d "$x" || die "unzip failed"
elif command -v bsdtar >/dev/null 2>&1; then
    # shellcheck disable=SC2086
    bsdtar -xf "$whl" -C "$x" $names || die "bsdtar failed"
elif command -v python3 >/dev/null 2>&1; then
    printf '%s\n' "$names" | python3 -I -c 'import sys, zipfile; z = zipfile.ZipFile(sys.argv[1]); [z.extract(n.strip(), sys.argv[2]) for n in sys.stdin if n.strip()]' "$whl" "$x" || die "python3 extraction failed"
else
    die "need unzip, bsdtar or python3 to extract the wheel"
fi
printf '%s\n' "$files" | while IFS='|' read -r name to size hash; do
    mkdir -p "$(dirname "$stage/$to")"
    mv -f "$x/$name" "$stage/$to"
done
rm -rf "$x"
problem=$(verify_tree "$stage") || die "extracted files do not match the pin: $problem"
chmod -R u+rwX,go+rX "$stage"
echo "$version" > "$stage/.version"
rm -rf "$dest.old"
[ -d "$dest" ] && mv "$dest" "$dest.old"
mv "$stage" "$dest"
rm -rf "$dest.old"
trap - EXIT
[ "$keep" = 1 ] || rm -f "$whl"
say "installed nvidia-vfx $version ($(printf '%s\n' "$files" | wc -l | tr -d ' ') files, verified) in $dest"
