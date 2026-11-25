#!/bin/bash
# build-plex-libs.sh <src dir> <out dir>  (MSYS2 UCRT64): the Windows pieces for players whose libmpv loads FFmpeg 5.x
# as separate DLLs (Plex): pushmpc-avfilter-8.<min>.<micro>.dll for every pinned FFmpeg 5.0 / 5.1 tag (libavfilter with
# only vf_pushmpc, D3D11 frames enabled; it imports avutil-57.dll by name) and the proxy avfilter-8.dll whose exports
# forward to the renamed original. Tags of one libavfilter version give one file.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
src=$1; out=$2
mkdir -p "$out"
cc=gcc; command -v ccache >/dev/null && cc="ccache gcc"
tags=$(python3 -c "import json,sys,re; print(' '.join(t['tag'] for t in json.load(open(sys.argv[1]))['hookTags'] if re.fullmatch(r'n5\.[01](\.\d+)?', t['tag'])))" "$here/../pins.json")
ref=''
for tag in $tags; do
    commit=$(python3 -c "import json,sys; print([t['commit'] for t in json.load(open(sys.argv[1]))['hookTags'] if t['tag']==sys.argv[2]][0])" "$here/../pins.json" "$tag")
    if [ ! -d "$src/.git" ]; then git clone -q --depth 1 -b "$tag" https://git.ffmpeg.org/ffmpeg.git "$src"
    else git -C "$src" fetch -q --depth 1 origin tag "$tag" && git -C "$src" checkout -q -f "$tag"; fi
    [ "$(git -C "$src" rev-parse HEAD)" = "$commit" ] || { echo "::error::$tag: commit mismatch"; exit 1; }
    sh "$here/../ffmpeg/add-to-ffmpeg.sh" "$src" >/dev/null
    (
        cd "$src"
        rm -f config.h config_components.h
        ./configure --cc="$cc" --disable-everything --enable-filter=pushmpc --enable-shared --disable-static --disable-programs \
            --disable-doc --disable-avdevice --disable-avformat --disable-avcodec --disable-swresample --disable-swscale \
            --disable-postproc --disable-asm --enable-d3d11va --extra-ldflags=-static-libgcc > configure.out 2>&1 \
            || { tail -30 configure.out; exit 1; }
        grep -qs 'CONFIG_PUSHMPC_FILTER 1' config_components.h config.h
        grep -q 'CONFIG_D3D11VA 1' config.h || echo "::warning::$tag: D3D11VA is not enabled (no D3D11 frames for pushmpc)"
        make -j"$(nproc)" >/dev/null
    )
    vdir="$src/libavfilter/"
    maj=$( (cat "${vdir}version_major.h" "${vdir}version.h" 2>/dev/null || true) | sed -n 's/^#define LIBAVFILTER_VERSION_MAJOR *\([0-9]*\).*/\1/p' | head -1)
    min=$(sed -n 's/^#define LIBAVFILTER_VERSION_MINOR *\([0-9]*\).*/\1/p' "$src"/libavfilter/version.h | head -1)
    mic=$(sed -n 's/^#define LIBAVFILTER_VERSION_MICRO *\([0-9]*\).*/\1/p' "$src"/libavfilter/version.h | head -1)
    name="pushmpc-avfilter-$maj.$min.$mic.dll"
    cp "$src/libavfilter/avfilter-$maj.dll" "$out/$name"
    echo "$tag -> $name (imports: $(objdump -p "$out/$name" | sed -n 's/^\s*DLL Name: //p' | tr '\n' ' '))"
    ref="$out/$name"
done
# the proxy: libavfilter 8's exports, forwarded
sh "$here/make-def.sh" "$ref" "$out/avfilter_proxy.def"
gcc -O2 -Wall -shared -static-libgcc "$here/avfilter_proxy.c" "$out/avfilter_proxy.def" -o "$out/avfilter-8.dll"
rm -f "$out/avfilter_proxy.def"
echo "proxy avfilter-8.dll imports: $(objdump -p "$out/avfilter-8.dll" | sed -n 's/^\s*DLL Name: //p' | tr '\n' ' ')"
ls -la "$out"
