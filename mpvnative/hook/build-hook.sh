#!/bin/bash
# build-hook.sh <ffmpeg tree of one release branch> <out dir> [--hook]: the runtime hook's pieces for an existing mpv.
#   libpushmpc-avfilter-<maj>.<min>.<micro>.so  that branch's libavfilter with only vf_pushmpc and the parse2 glue
#                       (pushmpc_hook_glue.c); it links libavutil by soname, so it uses the copy mpv already loaded
#   pushmpc_hook.so     (--hook) the mpv C plugin; independent of the FFmpeg version, built once per architecture
# The hook picks the library matching the system's avfilter_version() at run time.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
tree=$1; out=$2; hook=${3:-}
mkdir -p "$out"
sh "$here/../ffmpeg/add-to-ffmpeg.sh" "$tree"
cp "$here/pushmpc_hook_glue.c" "$tree/libavfilter/pushmpc_hook_glue.c"
grep -q 'pushmpc_hook_glue.o' "$tree/libavfilter/Makefile" || \
    perl -pi -e 's/^(OBJS-\$\(CONFIG_PUSHMPC_FILTER\)\s+\+= vf_pushmpc\.o)\s*$/$1 pushmpc_hook_glue.o\n/' "$tree/libavfilter/Makefile"
grep -q 'pushmpc_hook_glue.o' "$tree/libavfilter/Makefile"
cd "$tree"
cc=cc; command -v ccache >/dev/null && cc="ccache cc"
if [ ! -f config_components.h ] || ! grep -q 'CONFIG_PUSHMPC_FILTER 1' config_components.h; then
    ./configure --cc="$cc" --disable-everything --enable-filter=pushmpc --enable-shared --disable-static --disable-programs \
        --disable-doc --disable-avdevice --disable-avformat --disable-avcodec --disable-swresample --disable-swscale \
        $(./configure --help | grep -q disable-postproc && echo --disable-postproc) --disable-asm > configure.out 2>&1 || { tail -30 configure.out; exit 1; }
fi
grep -q 'CONFIG_PUSHMPC_FILTER 1' config_components.h
make -j"$(nproc)" >/dev/null
maj=$(sed -n 's/^#define LIBAVFILTER_VERSION_MAJOR *\([0-9]*\).*/\1/p' libavfilter/version_major.h libavfilter/version.h | head -1)
min=$(sed -n 's/^#define LIBAVFILTER_VERSION_MINOR *\([0-9]*\).*/\1/p' libavfilter/version.h | head -1)
mic=$(sed -n 's/^#define LIBAVFILTER_VERSION_MICRO *\([0-9]*\).*/\1/p' libavfilter/version.h | head -1)
so=$(ls libavfilter/libavfilter.so.* | grep -E '\.so\.[0-9]+$' | head -1)
name="libpushmpc-avfilter-$maj.$min.$mic.so"
cp "$so" "$out/$name"
patchelf --set-soname "$name" "$out/$name"
nm -D --defined-only "$out/$name" | grep -q avfilter_pushmpc_parse2 || { echo "::error::the parse2 glue is not exported"; exit 1; }
echo "$name: NEEDED $(readelf -d "$out/$name" | grep NEEDED | grep -o '\[.*\]' | tr '\n' ' ')"
cd - >/dev/null
if [ "$hook" = --hook ]; then
    $cc -shared -fPIC -O2 -Wall "$here/pushmpc_hook.c" -o "$out/pushmpc_hook.so" $(pkg-config --cflags mpv 2>/dev/null || echo "-I$here/../third_party") -ldl
fi
ls -la "$out"
