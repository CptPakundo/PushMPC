#!/bin/bash
# build-mac-extras.sh <bundle dir>  (macOS): the Mac player pieces next to the Plex for Mac / engine bundle that
# build-plex-mac.sh made in <bundle dir> (one Mac bundle: the Homebrew formula's plex-mac resource, pushmpc-setup):
#   vlc/   libpushmpc_plugin.dylib (frame generation: VideoToolbox through libpushmpc_engine_apple.dylib, else the GPU
#          engine on MoltenVK, else the CPU engine),
#          libpushmpc_macosx_plugin.dylib (PushMPC's macOS OpenGL output with FSR 1 upscaling), libMoltenVK.dylib,
#          pushmpc-vlc.sh: universal arm64 + x86_64, built against VLC 3.0.24 (the official SDK headers and sources)
#   iina/  PushMPC-<version>.iinaplgz (the IINA plugin; integrations/iina/build.py, pinned shaders)
#   pushmpc-mac-players.sh (IINA and VLC install / uninstall / self-test, called by pushmpc-setup)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); root=$(cd "$here/../.." && pwd)
out=$1; mkdir -p "$out/vlc" "$out/iina"
work=$(mktemp -d)
vlcver=3.0.24
min=11.0

# VLC 3 plugin headers: the official Windows SDK carries them (VLC.app has none); the sources for the output fork
command -v 7z >/dev/null || brew install p7zip >/dev/null
command -v ninja >/dev/null || brew install ninja >/dev/null
f="$work/vlc-$vlcver-win64.7z"
curl -fsSL --retry 3 -o "$f" "https://download.videolan.org/pub/videolan/vlc/$vlcver/win64/vlc-$vlcver-win64.7z"
got=$(shasum -a 256 "$f" | cut -d' ' -f1)
if want=$(curl -fsSL --retry 3 "https://download.videolan.org/pub/videolan/vlc/$vlcver/win64/vlc-$vlcver-win64.7z.sha256" 2>/dev/null | cut -d' ' -f1) && [ -n "$want" ]; then
    [ "$want" = "$got" ] || { echo "::error::vlc-$vlcver-win64.7z: SHA-256 mismatch"; exit 1; }
else echo "::warning::no published SHA-256 for vlc-$vlcver-win64.7z (got $got)"; fi
7z x -y -o"$work/vlcwin" "$f" "vlc-$vlcver/sdk/include" >/dev/null
inc="$work/vlcwin/vlc-$vlcver/sdk/include/vlc/plugins"
sh "$root/vlcplugin/display/vlc3/fetch-vlc-sources.sh" "$work/vlc-src" >/dev/null
# the CVPX zero copy mirrors VLC's private struct cvpxpic_ctx (vlcplugin/src/pm_cvpx.c): it must be the same text in
# vt_utils.c at every tag the module allows (pm_cvpx.c PM_CVPX_MIN_REV..MAX_REV), and picture_context_t the same in
# vlc_picture.h; otherwise the build stops (the allow-list or the mirror needs updating)
norm() { tr -s ' \t\n' ' ' | sed 's/ *$//'; }
ours=$(sed -n '/PM_CVPX_LAYOUT_BEGIN/,/PM_CVPX_LAYOUT_END/p' "$root/vlcplugin/src/pm_cvpx.c" | sed -n '/^struct cvpxpic_ctx/,/^};/p' | norm)
[ -n "$ours" ] || { echo "::error::pm_cvpx.c: no mirrored layout"; exit 1; }
for rev in 20 21 22 23 24; do
    tag=3.0.$rev
    vt=$(curl -fsSL --retry 3 "https://raw.githubusercontent.com/videolan/vlc/$tag/modules/codec/vt_utils.c") || { echo "::error::VLC $tag: vt_utils.c not fetched"; exit 1; }
    theirs=$(printf '%s\n' "$vt" | sed -n '/^struct cvpxpic_ctx/,/^};/p' | norm)
    [ "$theirs" = "$ours" ] || { echo "::error::VLC $tag: struct cvpxpic_ctx differs from pm_cvpx.c's mirror"; printf '%s\n' "$theirs"; exit 1; }
    pc=$(curl -fsSL --retry 3 "https://raw.githubusercontent.com/videolan/vlc/$tag/include/vlc_picture.h" | sed -n '/^typedef struct picture_context_t/,/} picture_context_t;/p' | norm)
    case $pc in *"void (*destroy)(struct picture_context_t *); struct picture_context_t *(*copy)(struct picture_context_t *); } picture_context_t;"*) ;;
        *) echo "::error::VLC $tag: picture_context_t differs"; printf '%s\n' "$pc"; exit 1 ;; esac
    echo "VLC $tag: the CVPX picture layout matches pm_cvpx.c"
done
cmake -S "$root/vlcplugin" -B "$work/vlc" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES='arm64;x86_64' \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=$min -DVLC_PLUGIN_INCLUDE="$inc" -DPM_FETCH_GPU_DEPS=ON > "$work/vlc.log" 2>&1 \
    && cmake --build "$work/vlc" --target pushmpc_plugin pushmpc_macosx_plugin >> "$work/vlc.log" 2>&1 || { tail -40 "$work/vlc.log"; exit 1; }
grep -E "GPU engine|no GPU engine|Vulkan" "$work/vlc.log" | head -3 || true
for p in libpushmpc_plugin.dylib libpushmpc_macosx_plugin.dylib; do
    cp "$work/vlc/$p" "$out/vlc/"; codesign -f -s - "$out/vlc/$p"
    echo "vlc/$p ($(lipo -archs "$out/vlc/$p"))"
done
cp "$out/libMoltenVK.dylib" "$out/vlc/"   # the GPU engine finds it next to the plugin
cp "$out/libpushmpc_engine_apple.dylib" "$out/vlc/"   # VideoToolbox interpolation, tried first (engine_portable.cpp)
cp "$root/vlcplugin/install/pushmpc-vlc.sh" "$out/vlc/"

# the IINA plugin (pinned shaders, fetched and hash-checked)
python3 -I "$root/integrations/iina/build.py" build --cache "$work/iina-cache" --out "$out/iina" > "$work/iina.log" 2>&1 || { tail -30 "$work/iina.log"; exit 1; }
ls "$out/iina"

cp "$root/mpvnative/install/pushmpc-mac-players.sh" "$out/"
rm -rf "$work"
