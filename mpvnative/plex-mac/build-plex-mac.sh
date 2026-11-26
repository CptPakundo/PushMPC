#!/bin/bash
# build-plex-mac.sh <src dir> <out dir>  (macOS, Xcode clang): the pieces for Plex for Mac (desktop, HTPC), whose
# libmpv.2.dylib links FFmpeg 5.x as separate universal dylibs (libavfilter.8.dylib is 8.29.100, a FFmpeg master
# snapshot): pushmpc-avfilter-<ver>.dylib for every pins.json snapshotCommits entry (arm64 + x86_64, only vf_pushmpc,
# binds @rpath/libavutil.57.dylib = Plex's), the proxy libavfilter.8.dylib (re-exports the renamed original
# libavfilter.8-plex.dylib, defines avfilter_get_by_name), libpushmpc_engine.dylib + libpushmpc_engine_apple.dylib + libMoltenVK.dylib (Auto: VideoToolbox,
# then Vulkan on MoltenVK, then CPU) and the self-test host.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
src=$1; out=$2
mkdir -p "$out"
pins="$here/../pins.json"
min=11.0
work=$(mktemp -d)
stub=''

for commit in $(python3 -c "import json,sys; print(' '.join(c['commit'] for c in json.load(open(sys.argv[1])).get('snapshotCommits', [])))" "$pins"); do
    [ -d "$src/.git" ] || git init -q "$src"
    git -C "$src" fetch -q --depth 1 https://github.com/FFmpeg/FFmpeg.git "$commit" && git -C "$src" checkout -q -f "$commit"
    [ "$(git -C "$src" rev-parse HEAD)" = "$commit" ] || { echo "::error::snapshot $commit: checkout failed"; exit 1; }
    sh "$here/../ffmpeg/add-to-ffmpeg.sh" "$src" >/dev/null
    libs=()
    for arch in arm64 x86_64; do
        (
            b="$work/build-$arch"; rm -rf "$b"; mkdir -p "$b"; cd "$b"   # out of tree: one build folder per architecture
            cross=''; [ "$arch" = "$(uname -m)" ] || cross='--enable-cross-compile'
            "$src/configure" --cc="clang -arch $arch" --arch="$arch" --target-os=darwin $cross \
                --disable-everything --enable-filter=pushmpc --enable-shared --disable-static --disable-programs \
                --disable-doc --disable-avdevice --disable-avformat --disable-avcodec --disable-swresample --disable-swscale \
                --disable-postproc --disable-asm --disable-autodetect --install-name-dir='@rpath' \
                --extra-cflags="-mmacosx-version-min=$min" --extra-ldflags="-arch $arch -mmacosx-version-min=$min" \
                > configure.out 2>&1 || { tail -30 configure.out; exit 1; }
            grep -qs 'CONFIG_PUSHMPC_FILTER 1' config_components.h config.h
            make -j"$(sysctl -n hw.ncpu)" >/dev/null
        )
        cp "$work/build-$arch/libavfilter/libavfilter.8.dylib" "$work/avfilter-$arch.dylib"
        libs+=("$work/avfilter-$arch.dylib")
    done
    vdir="$src/libavfilter/"
    maj=$( (cat "${vdir}version_major.h" "${vdir}version.h" 2>/dev/null || true) | sed -n 's/^#define LIBAVFILTER_VERSION_MAJOR *\([0-9]*\).*/\1/p' | head -1)
    mi=$(sed -n 's/^#define LIBAVFILTER_VERSION_MINOR *\([0-9]*\).*/\1/p' "${vdir}version.h" | head -1)
    mic=$(sed -n 's/^#define LIBAVFILTER_VERSION_MICRO *\([0-9]*\).*/\1/p' "${vdir}version.h" | head -1)
    name="pushmpc-avfilter-$maj.$mi.$mic.dylib"
    lipo -create "${libs[@]}" -output "$out/$name"
    install_name_tool -id "@rpath/$name" "$out/$name" 2>/dev/null
    codesign -f -s - "$out/$name"
    bad=$(otool -L "$out/$name" | sed 1d | awk '{print $1}' | grep -v -e '^@rpath/' -e '^/System/' -e '^/usr/lib/' | grep -v "^$out/" || true)
    [ -z "$bad" ] || { echo "::error::$name links outside the app and the system: $bad"; exit 1; }
    echo "master@${commit:0:12} -> $name ($(lipo -archs "$out/$name")); links: $(otool -L "$out/$name" | sed 1,2d | awk '{print $1}' | grep -v '^/System\|^/usr/lib' | tr '\n' ' ')"
    # link-time stand-in for Plex's original under its new name (same exports: libavfilter 8 of the same release)
    stub="$work/libavfilter.8-plex.dylib"
    lipo -create "${libs[@]}" -output "$stub"
    install_name_tool -id '@rpath/libavfilter.8-plex.dylib' "$stub" 2>/dev/null
done
[ -n "$stub" ] || { echo "::error::no snapshotCommits in pins.json"; exit 1; }

# the proxy: Plex's libmpv links @rpath/libavfilter.8.dylib
clang -dynamiclib -arch arm64 -arch x86_64 -O2 -Wall -mmacosx-version-min=$min "$here/avfilter_proxy_mac.c" \
    -install_name '@rpath/libavfilter.8.dylib' -compatibility_version 8.0.0 -current_version 8.29.100 \
    -Wl,-reexport_library,"$stub" -o "$out/libavfilter.8.dylib"
codesign -f -s - "$out/libavfilter.8.dylib"
echo "proxy libavfilter.8.dylib ($(lipo -archs "$out/libavfilter.8.dylib")): $(otool -L "$out/libavfilter.8.dylib" | sed 1,2d | awk '{print $1}' | tr '\n' ' ')"
nm -gU "$out/libavfilter.8.dylib" | grep -E ' _(avfilter_get_by_name|pushmpc_proxy_status)$'

# the engine module, universal: the CPU engine, the portable GPU engine on its own Vulkan device (MoltenVK, below) and
# libpushmpc_engine_apple.dylib (VideoToolbox, macOS 13.3+); Auto: apple, then Vulkan, then CPU. Needs glslang (brew).
command -v glslangValidator >/dev/null || brew install glslang >/dev/null
cmake -S "$here/../engine" -B "$work/engine" -DCMAKE_BUILD_TYPE=Release -DPME_FETCH_GPU_DEPS=ON \
    -DCMAKE_OSX_ARCHITECTURES='arm64;x86_64' -DCMAKE_OSX_DEPLOYMENT_TARGET=$min > "$work/engine.log" 2>&1 \
    && cmake --build "$work/engine" --parallel --target pushmpc_engine pushmpc_engine_apple >> "$work/engine.log" 2>&1 || { tail -40 "$work/engine.log"; exit 1; }
grep -q 'no Vulkan engine' "$work/engine.log" && { grep 'no Vulkan engine' "$work/engine.log"; echo "::error::the engine was built without Vulkan"; exit 1; }
for m in libpushmpc_engine.dylib libpushmpc_engine_apple.dylib; do
    cp "$work/engine/out/$m" "$out/"; codesign -f -s - "$out/$m"
    echo "$m ($(lipo -archs "$out/$m")), minimum macOS $(otool -l "$out/$m" | awk '/LC_BUILD_VERSION/{f=1} f&&/minos/{print $2; exit}')"
done
# MoltenVK (Apache-2.0), the release pinned in gpu-pins.json: the Vulkan driver for the GPU engine, loaded from here
mvk=$(python3 -c "import json,sys; m=json.load(open(sys.argv[1]))['moltenvk']; print(m['url'], m['sha256'], m['version'])" "$here/../../vsplugin/engines/gpu/gpu-pins.json")
set -- $mvk
curl -fsSL -o "$work/mvk.tar" "$1"
echo "$2  $work/mvk.tar" | shasum -a 256 -c - >/dev/null || { echo "::error::MoltenVK $3: SHA-256 mismatch"; exit 1; }
mkdir -p "$work/mvk" && tar -xf "$work/mvk.tar" -C "$work/mvk"
lib=$(find "$work/mvk" -path '*dylib/macOS/libMoltenVK.dylib' | head -1)
[ -n "$lib" ] || { echo "::error::no macOS libMoltenVK.dylib in MoltenVK $3"; find "$work/mvk" -name '*.dylib' | head; exit 1; }
cp "$lib" "$out/libMoltenVK.dylib"
codesign -v "$out/libMoltenVK.dylib" 2>/dev/null || codesign -f -s - "$out/libMoltenVK.dylib"
lic=$(find "$work/mvk" -maxdepth 3 -name 'LICENSE*' | head -1); [ -z "$lic" ] || cp "$lic" "$out/MoltenVK-LICENSE.txt"
echo "libMoltenVK.dylib $3 ($(lipo -archs "$out/libMoltenVK.dylib"))"

# the self-test host (loads Plex's libmpv in its own process)
clang -O2 -Wall -arch arm64 -arch x86_64 -mmacosx-version-min=$min "$here/../hook/libmpv_host.c" -o "$out/pushmpc-plex-selftest"
codesign -f -s - "$out/pushmpc-plex-selftest"
# the libmpv stand-in for Stremio / Jellyfin Desktop (pushmpc-apps-mac.sh): re-exports the app's original libmpv
# (renamed pushmpc-orig-libmpv.dylib; the link-time stub has only that install name) and loads PushMPC's mpv bridge
# from its own mpv_initialize. current_version is the highest there is, so no app's recorded version can refuse it.
mkdir -p "$out/apps"
: > "$work/empty.c"
clang -dynamiclib -arch arm64 -arch x86_64 -mmacosx-version-min=$min "$work/empty.c" \
    -install_name '@rpath/pushmpc-orig-libmpv.dylib' -o "$work/pushmpc-orig-libmpv.dylib"
clang -dynamiclib -arch arm64 -arch x86_64 -O2 -Wall -mmacosx-version-min=$min "$here/mpv_proxy_mac.c" \
    -install_name '@rpath/libmpv.2.dylib' -compatibility_version 1.0.0 -current_version 65535.255.255 \
    -Wl,-rpath,@loader_path -Wl,-reexport_library,"$work/pushmpc-orig-libmpv.dylib" -o "$out/apps/libmpv-proxy.dylib"
codesign -f -s - "$out/apps/libmpv-proxy.dylib"
nm -gU "$out/apps/libmpv-proxy.dylib" | grep -E ' _(mpv_initialize|pushmpc_mpv_proxy_status)$'
echo "apps/libmpv-proxy.dylib ($(lipo -archs "$out/apps/libmpv-proxy.dylib")): $(otool -L "$out/apps/libmpv-proxy.dylib" | sed 1,2d | awk '{print $1}' | tr '\n' ' ')"
# the installers next to their files: this folder is the whole Mac bundle (Setup / Homebrew run them from here)
cp "$here/../install/pushmpc-plex-mac.sh" "$here/../install/pushmpc-apps-mac.sh" "$here/../ci/SOURCES.txt" "$out/"
ls -la "$out"
