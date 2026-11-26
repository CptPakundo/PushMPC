#!/usr/bin/env bash
# Static, minimal FFmpeg for the Android add-on, cross-compiled with the NDK's clang, with FFmpeg's MediaCodec decoders:
# their NDK backend (AMediaCodec, libmediandk) needs no Java VM, so they work in Kodi's add-on.
# usage: build-ffmpeg-android.sh <version> <prefix> <ndk> [abi] [api]
#   abi: arm64-v8a (default: Android TV, Shield, Fire TV 4K Max / Cube), armeabi-v7a (32-bit Kodi: most Fire TV sticks
#        and older boxes), x86 (Kodi's x86 build: Android x86 devices, Chromebooks, the emulator test), x86_64
set -euo pipefail
ver="$1"; prefix="$2"; ndk="$3"; abi="${4:-arm64-v8a}"; api="${5:-24}"
tc="$ndk/toolchains/llvm/prebuilt/linux-x86_64"
case "$abi" in
  arm64-v8a)   triple=aarch64-linux-android; arch=(--arch=aarch64 --cpu=armv8-a) ;;
  armeabi-v7a) triple=armv7a-linux-androideabi; arch=(--arch=arm --cpu=armv7-a --enable-neon) ;;
  # 32-bit x86: FFmpeg's x86 assembly is not position independent there (text relocations, which Android refuses)
  x86)         triple=i686-linux-android; arch=(--arch=x86 --cpu=i686 --disable-asm) ;;
  x86_64)      triple=x86_64-linux-android; arch=(--arch=x86_64) ;;
  *) echo "build-ffmpeg-android.sh: unknown ABI $abi" >&2; exit 1 ;;
esac
bash "$(dirname "$0")/build-ffmpeg.sh" "$ver" "$prefix" \
  --enable-cross-compile --target-os=android "${arch[@]}" \
  --cc="$tc/bin/$triple$api-clang" --cxx="$tc/bin/$triple$api-clang++" \
  --ar="$tc/bin/llvm-ar" --ranlib="$tc/bin/llvm-ranlib" --strip="$tc/bin/llvm-strip" --nm="$tc/bin/llvm-nm" \
  --enable-jni --enable-mediacodec --enable-decoder=h264_mediacodec,hevc_mediacodec,vp9_mediacodec,av1_mediacodec
