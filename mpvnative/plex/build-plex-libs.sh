#!/bin/bash
# build-plex-libs.sh <src dir> <out dir>  (MSYS2 UCRT64): the Windows pieces for players whose libmpv loads FFmpeg 5.x
# as separate DLLs (Plex): pushmpc-avfilter-8.<min>.<micro>.dll for every pinned FFmpeg 5.0 / 5.1 tag and every pinned
# master snapshot (pins.json snapshotCommits: Plex ships libavfilter 8.29.100, a master version between 5.0 and 5.1),
# each a libavfilter with only vf_pushmpc and D3D11 frames that imports avutil-57.dll by name; and the proxy
# avfilter-8.dll whose exports forward to the renamed original. Builds of one libavfilter version give one file.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
src=$1; out=$2
mkdir -p "$out"
cc=gcc; command -v ccache >/dev/null && cc="ccache gcc"
pins="$here/../pins.json"
ref=''

build_tree() {   # build_tree <label>: the tree in $src is checked out
    local label=$1
    sh "$here/../ffmpeg/add-to-ffmpeg.sh" "$src" >/dev/null
    (
        cd "$src"
        rm -f config.h config_components.h
        ./configure --cc="$cc" --disable-everything --enable-filter=pushmpc --enable-shared --disable-static --disable-programs \
            --disable-doc --disable-avdevice --disable-avformat --disable-avcodec --disable-swresample --disable-swscale \
            --disable-postproc --disable-asm --enable-d3d11va --extra-ldflags=-static-libgcc > configure.out 2>&1 \
            || { tail -30 configure.out; exit 1; }
        grep -qs 'CONFIG_PUSHMPC_FILTER 1' config_components.h config.h
        grep -q 'CONFIG_D3D11VA 1' config.h || echo "::warning::$label: D3D11VA is not enabled (no D3D11 frames for pushmpc)"
        make -j"$(nproc)" >/dev/null
    )
    local vdir="$src/libavfilter/" maj min mic name
    maj=$( (cat "${vdir}version_major.h" "${vdir}version.h" 2>/dev/null || true) | sed -n 's/^#define LIBAVFILTER_VERSION_MAJOR *\([0-9]*\).*/\1/p' | head -1)
    min=$(sed -n 's/^#define LIBAVFILTER_VERSION_MINOR *\([0-9]*\).*/\1/p' "${vdir}version.h" | head -1)
    mic=$(sed -n 's/^#define LIBAVFILTER_VERSION_MICRO *\([0-9]*\).*/\1/p' "${vdir}version.h" | head -1)
    name="pushmpc-avfilter-$maj.$min.$mic.dll"
    cp "$src/libavfilter/avfilter-$maj.dll" "$out/$name"
    echo "$label -> $name (imports: $(objdump -p "$out/$name" | sed -n 's/^\s*DLL Name: //p' | grep -v '^api-ms-win-crt' | tr '\n' ' '))"
    ref="$out/$name"
}

# release tags
tags=$(python3 -c "import json,sys,re; print(' '.join(t['tag'] for t in json.load(open(sys.argv[1]))['hookTags'] if re.fullmatch(r'n5\.[01](\.\d+)?', t['tag'])))" "$pins")
for tag in $tags; do
    commit=$(python3 -c "import json,sys; print([t['commit'] for t in json.load(open(sys.argv[1]))['hookTags'] if t['tag']==sys.argv[2]][0])" "$pins" "$tag")
    # git.ffmpeg.org, else the GitHub mirror (same tags; the commit check below holds either way)
    [ -d "$src/.git" ] || git init -q "$src"
    for url in https://git.ffmpeg.org/ffmpeg.git https://github.com/FFmpeg/FFmpeg.git; do
        git -C "$src" fetch -q --depth 1 "$url" tag "$tag" 2>/dev/null && break
        echo "$tag: $url unreachable, trying the next source"
    done
    git -C "$src" checkout -q -f "$tag"
    [ "$(git -C "$src" rev-parse HEAD)" = "$commit" ] || { echo "::error::$tag: commit mismatch"; exit 1; }
    build_tree "$tag"
done
# master snapshots (by commit, from the GitHub mirror, which serves single commits)
for commit in $(python3 -c "import json,sys; print(' '.join(c['commit'] for c in json.load(open(sys.argv[1])).get('snapshotCommits', [])))" "$pins"); do
    git -C "$src" fetch -q --depth 1 https://github.com/FFmpeg/FFmpeg.git "$commit" && git -C "$src" checkout -q -f "$commit"
    [ "$(git -C "$src" rev-parse HEAD)" = "$commit" ] || { echo "::error::snapshot $commit: checkout failed"; exit 1; }
    build_tree "master@${commit:0:12}"
done
# the proxy: libavfilter 8's exports, forwarded
sh "$here/make-def.sh" "$ref" "$out/avfilter_proxy.def"
gcc -O2 -Wall -shared -static-libgcc "$here/avfilter_proxy.c" "$out/avfilter_proxy.def" -o "$out/avfilter-8.dll"
rm -f "$out/avfilter_proxy.def"
echo "proxy avfilter-8.dll imports: $(objdump -p "$out/avfilter-8.dll" | sed -n 's/^\s*DLL Name: //p' | tr '\n' ' ')"
ls -la "$out"
