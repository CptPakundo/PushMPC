#!/bin/sh
# Fetches the VLC 3.0.24 sources the Direct3D 11 output fork is built from (the official release tarball, SHA-256
# checked), puts them below modules/ next to PushMPC's own files and applies pushmpc-vlc-3.0.24.patch. The upstream files
# are not kept in this repository; only PushMPC's changes are (the patch, pm_nis_d3d11.*).
#   fetch-vlc-sources.sh [download-cache-dir]
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
ver=3.0.24
cache="${1:-${TMPDIR:-/tmp}}"
tar="$cache/vlc-$ver.tar.xz"
base="https://download.videolan.org/pub/videolan/vlc/$ver"
mkdir -p "$cache"
[ -s "$tar" ] || curl -fsSL --retry 3 -o "$tar" "$base/vlc-$ver.tar.xz"
want=$(curl -fsSL --retry 3 "$base/vlc-$ver.tar.xz.sha256" | cut -d' ' -f1)
got=$( (sha256sum "$tar" 2>/dev/null || shasum -a 256 "$tar") | cut -d' ' -f1)
[ "$want" = "$got" ] || { echo "SHA-256 mismatch for vlc-$ver.tar.xz: $got" >&2; exit 1; }
files="COPYING.LIB
include/vlc_vout_wrapper.h
modules/codec/avcodec/va_surface.h
modules/video_chroma/copy.c
modules/video_chroma/copy.h
modules/video_chroma/d3d11_fmt.c
modules/video_chroma/d3d11_fmt.h
modules/video_chroma/dxgi_fmt.c
modules/video_chroma/dxgi_fmt.h
modules/video_output/win32/common.c
modules/video_output/win32/common.h
modules/video_output/win32/events.c
modules/video_output/win32/events.h
modules/video_output/win32/sensors.cpp
modules/video_output/win32/win32touch.c
modules/video_output/win32/win32touch.h
modules/video_output/win32/d3d11_quad.c
modules/video_output/win32/d3d11_quad.h
modules/video_output/win32/d3d11_shaders.c
modules/video_output/win32/d3d11_shaders.h
modules/video_output/win32/d3d11_scaler.cpp
modules/video_output/win32/d3d11_scaler.h
modules/video_output/win32/d3d11_tonemap.cpp
modules/video_output/win32/d3d11_tonemap.h
modules/video_output/win32/direct3d11.c"
list=""
for f in $files; do list="$list vlc-$ver/$f"; done
# shellcheck disable=SC2086
tar -xJf "$tar" -C "$here" --strip-components=1 $list
cd "$here"
patch -p1 --forward < pushmpc-vlc-3.0.24.patch
echo "VLC $ver sources in $here/modules, PushMPC's patch applied"
