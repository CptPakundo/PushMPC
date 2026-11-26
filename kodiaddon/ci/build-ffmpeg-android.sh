#!/usr/bin/env bash
# Static, minimal FFmpeg for the Android add-on (arm64-v8a), cross-compiled with the NDK's clang, with FFmpeg's
# MediaCodec decoders: their NDK backend (AMediaCodec, libmediandk) needs no Java VM, so they work in Kodi's add-on.
# usage: build-ffmpeg-android.sh <version> <prefix> <ndk> [api]
set -euo pipefail
ver="$1"; prefix="$2"; ndk="$3"; api="${4:-24}"
tc="$ndk/toolchains/llvm/prebuilt/linux-x86_64"
bash "$(dirname "$0")/build-ffmpeg.sh" "$ver" "$prefix" \
  --enable-cross-compile --target-os=android --arch=aarch64 --cpu=armv8-a \
  --cc="$tc/bin/aarch64-linux-android$api-clang" --cxx="$tc/bin/aarch64-linux-android$api-clang++" \
  --ar="$tc/bin/llvm-ar" --ranlib="$tc/bin/llvm-ranlib" --strip="$tc/bin/llvm-strip" --nm="$tc/bin/llvm-nm" \
  --enable-jni --enable-mediacodec --enable-decoder=h264_mediacodec,hevc_mediacodec,vp9_mediacodec,av1_mediacodec
