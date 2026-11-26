#!/bin/bash
# build-plex-mac.sh <src dir> <out dir>  (macOS, Xcode clang): the pieces for Plex for Mac (desktop, HTPC), whose
# libmpv.2.dylib links FFmpeg 5.x as separate universal dylibs (libavfilter.8.dylib is 8.29.100, a FFmpeg master
# snapshot): pushmpc-avfilter-<ver>.dylib for every pins.json snapshotCommits entry (arm64 + x86_64, only vf_pushmpc,
# binds @rpath/libavutil.57.dylib = Plex's), the proxy libavfilter.8.dylib (re-exports the renamed original
# libavfilter.8-plex.dylib, defines avfilter_get_by_name), libpushmpc_engine.dylib (CPU engine) and the self-test host.
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
                --disable-postproc --disable-asm --install-name-dir='@rpath' \
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

# the engine module (portable CPU engine), universal
cmake -S "$here/../engine" -B "$work/engine" -DCMAKE_BUILD_TYPE=Release -DPME_VULKAN=OFF \
    -DCMAKE_OSX_ARCHITECTURES='arm64;x86_64' -DCMAKE_OSX_DEPLOYMENT_TARGET=$min > "$work/engine.log" 2>&1 \
    && cmake --build "$work/engine" --parallel --target pushmpc_engine >> "$work/engine.log" 2>&1 || { tail -30 "$work/engine.log"; exit 1; }
cp "$work/engine/out/libpushmpc_engine.dylib" "$out/"
codesign -f -s - "$out/libpushmpc_engine.dylib"

# the self-test host (loads Plex's libmpv in its own process)
clang -O2 -Wall -arch arm64 -arch x86_64 -mmacosx-version-min=$min "$here/../hook/libmpv_host.c" -o "$out/pushmpc-plex-selftest"
codesign -f -s - "$out/pushmpc-plex-selftest"
ls -la "$out"
