#!/usr/bin/env bash
# Static, minimal FFmpeg for inputstream.pushmpc (Linux/macOS): demuxers, video decoders, parsers, swscale.
# The add-on links it privately (-Bsymbolic, --exclude-libs), so it never clashes with the FFmpeg Kodi itself uses.
# usage: build-ffmpeg.sh <version> <prefix> [extra configure args]
set -euo pipefail
ver="$1"; prefix="$2"; shift 2
work="$(mktemp -d)"
curl -fsSL --retry 3 -o "$work/ff.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$ver.tar.xz"
tar -xf "$work/ff.tar.xz" -C "$work"
cd "$work/ffmpeg-$ver"
./configure --prefix="$prefix" --enable-static --disable-shared --enable-pic --disable-programs --disable-doc \
  --disable-autodetect --disable-everything --disable-avdevice --disable-avfilter --disable-swresample \
  --enable-avformat --enable-avcodec --enable-swscale --enable-protocol=file \
  --enable-demuxer=matroska,mov,mpegts,mpegps,avi,flv,ogg,asf,h264,hevc,m4v,ivf,mpegvideo,webm_dash_manifest \
  --enable-parser=h264,hevc,vp8,vp9,av1,mpeg4video,mpegvideo,vc1,aac,ac3,mpegaudio,dca,opus,flac,vorbis \
  --enable-decoder=h264,hevc,vp8,vp9,av1,mpeg1video,mpeg2video,mpeg4,msmpeg4v3,vc1,wmv3,theora \
  --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,extract_extradata,vp9_superframe_split,av1_frame_split \
  "$@"
make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu)"
make install
