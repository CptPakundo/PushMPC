#!/bin/sh
# make-zips.sh <artifacts> <out> <version>: Kodi's "Install from zip file" (Settings > Add-ons) archives, one per platform and
# Kodi major, the add-on folder at its root: inputstream.pushmpc-<version>-<platform>-kodi<major>.zip from every
# <artifacts>/addon-<os>-kodi<major>/ folder (kodi-addon.yml's build artifacts). Used by kodi-addon.yml's zips job and by
# release.yml's extras job (the release attaches them).
set -eu
art=$1; out=$2; ver=$3
mkdir -p "$out"
out=$(cd "$out" && pwd)
plat() {
    case $1 in
        Windows) echo windows-x64 ;; Linux) echo linux-x86_64 ;; Linux-aarch64) echo linux-aarch64 ;; Linux-armv7) echo linux-armv7 ;;
        Android-arm64) echo android-arm64 ;; Android-armv7) echo android-armv7 ;; Android-x86) echo android-x86 ;; macOS) echo macos ;;
        *) echo "" ;;
    esac
}
for d in "$art"/addon-*-kodi*; do
    [ -d "$d" ] || continue
    n=${d##*/addon-}; os=${n%-kodi*}; k=${n##*-kodi}; p=$(plat "$os")
    if [ -z "$p" ] || [ ! -f "$d/inputstream.pushmpc/addon.xml" ]; then echo "::warning title=Kodi zips::skipped $d"; continue; fi
    chmod +x "$d/inputstream.pushmpc/pushmpc-nvvfx-selftest" "$d/inputstream.pushmpc/pushmpc-vulkan-probe" 2>/dev/null || true   # artifacts drop the exec bits
    z="$out/inputstream.pushmpc-$ver-$p-kodi$k.zip"
    (cd "$d" && zip -qr -X "$z" inputstream.pushmpc)
    echo "$(basename "$z"): $(unzip -l "$z" | tail -1)"
done
ls -la "$out"
[ "$(ls "$out" | grep -c '^inputstream\.pushmpc-')" -gt 0 ]
