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
