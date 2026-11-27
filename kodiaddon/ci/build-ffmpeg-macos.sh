#!/usr/bin/env bash
# Universal (arm64 + x86_64) static FFmpeg for the macOS add-on: one build per architecture with build-ffmpeg.sh (the
# x86_64 one cross-built on the Apple-silicon runner, with nasm), merged with lipo. VideoToolbox decoding on both.
# usage: build-ffmpeg-macos.sh <version> <prefix>
set -euo pipefail
ver="$1"; prefix="$2"
here=$(cd "$(dirname "$0")" && pwd)
host=$(uname -m)
for a in arm64 x86_64; do
  min=11.0; [ "$a" = x86_64 ] && min=10.15   # Kodi 21 / 22 on Intel Macs: macOS 10.15+
  cross=()
  if [ "$a" != "$host" ]; then
    arch=$a; [ "$a" = arm64 ] && arch=aarch64
    cross=(--enable-cross-compile --target-os=darwin --arch="$arch")
  fi
  MACOSX_DEPLOYMENT_TARGET=$min bash "$here/build-ffmpeg.sh" "$ver" "$prefix-$a" --enable-videotoolbox \
    --cc="clang -arch $a" --cxx="clang++ -arch $a" --extra-cflags="-mmacosx-version-min=$min" \
    --extra-ldflags="-arch $a -mmacosx-version-min=$min" ${cross[@]+"${cross[@]}"}   # bash 3.2 + set -u: an empty array is "unbound"
done
rm -rf "$prefix"; mkdir -p "$prefix/lib"
cp -R "$prefix-arm64/include" "$prefix/include"
for l in "$prefix-arm64"/lib/*.a; do
  n=$(basename "$l")
  lipo -create "$l" "$prefix-x86_64/lib/$n" -output "$prefix/lib/$n"
  lipo -info "$prefix/lib/$n"
done
# the per-architecture headers must agree (avconfig.h, the version headers)
diff -r "$prefix-arm64/include" "$prefix-x86_64/include"
