#!/bin/bash
# build-hook.sh <ffmpeg tree of the system's release> <out dir>: the runtime hook for an existing mpv (pushmpc_hook.c).
#   libpushmpc-avfilter.so  libavfilter of that release with only vf_pushmpc (links libavutil by soname, so it uses the
#                           copy mpv already loaded); soname renamed
#   pushmpc_hook.so         the mpv C plugin
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
tree=$1; out=$2
mkdir -p "$out"
sh "$here/../ffmpeg/add-to-ffmpeg.sh" "$tree"
cd "$tree"
cc=cc; command -v ccache >/dev/null && cc="ccache cc"
if [ ! -f config.h ] || ! grep -q 'CONFIG_PUSHMPC_FILTER 1' config_components.h; then
    ./configure --cc="$cc" --disable-everything --enable-filter=pushmpc --enable-shared --disable-static --disable-programs \
        --disable-doc --disable-avdevice --disable-avformat --disable-avcodec --disable-swresample --disable-swscale \
        --disable-postproc > configure.out 2>&1 || { tail -30 configure.out; exit 1; }
fi
grep -q 'CONFIG_PUSHMPC_FILTER 1' config_components.h
make -j"$(nproc)" >/dev/null
so=$(ls libavfilter/libavfilter.so.* | grep -E '\.so\.[0-9]+$' | head -1)
cp "$so" "$out/libpushmpc-avfilter.so"
patchelf --set-soname libpushmpc-avfilter.so "$out/libpushmpc-avfilter.so"
echo "NEEDED of libpushmpc-avfilter.so:"; readelf -d "$out/libpushmpc-avfilter.so" | grep -E 'NEEDED|SONAME'
$cc -shared -fPIC -O2 -Wall "$here/pushmpc_hook.c" -o "$out/pushmpc_hook.so" $(pkg-config --cflags mpv) -ldl
ls -la "$out"
