#!/bin/bash
# make-appimage.sh <prefix> <engine .so> <src dir> <out dir>: PushMPC-mpv-x86_64.AppImage from a build-unix.sh prefix.
# AppDir: mpv + our FFmpeg (vf_pushmpc) + libplacebo + the engine module + the libraries ldd finds, minus the base
# system and the graphics / audio stack (glibc, libGL/EGL/Vulkan loaders and drivers, X11/xcb/Wayland, ALSA/Pulse/
# PipeWire), which must come from the host. The runtime is the pinned AppImage type2-runtime release, checked against
# the digest GitHub publishes for it. mksquashfs: squashfs-tools.
set -euo pipefail
P=$1; ENGINE=$2; S=$3; OUT=$4
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
pin() { python3 -c "import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]][sys.argv[3]])" "$ROOT/mpvnative/pins.json" "$1" "$2"; }
A=$S/AppDir; rm -rf "$A"; mkdir -p "$A/usr/bin" "$A/usr/lib" "$A/usr/share/applications" "$OUT"
cp "$P/bin/mpv" "$A/usr/bin/"
cp -P "$P"/lib/lib{avcodec,avdevice,avfilter,avformat,avutil,swresample,swscale,postproc,placebo}*.so* "$A/usr/lib/" 2>/dev/null || true
cp "$ENGINE" "$A/usr/lib/"
exclude='^(libc|libm|libdl|libpthread|librt|libresolv|libutil|ld-linux.*|libGL.*|libEGL.*|libGLX.*|libGLdispatch|libOpenGL|libvulkan|libdrm.*|libgbm|libX.*|libxcb.*|libxkbcommon.*|libwayland.*|libasound|libpulse.*|libpipewire.*|libjack.*|libgcc_s|libstdc\+\+|libdbus.*|libsystemd|libudev|libexpat|libz)\.so'
for f in "$A/usr/bin/mpv" "$A"/usr/lib/*.so*; do
    LD_LIBRARY_PATH="$P/lib" ldd "$f" 2>/dev/null | awk '/=> \// {print $3}'
done | sort -u | while read -r lib; do
    b=$(basename "$lib"); echo "$b" | grep -Eq "$exclude" && continue
    [ -e "$A/usr/lib/$b" ] || cp -L "$lib" "$A/usr/lib/$b"
done
patchelf --set-rpath '$ORIGIN/../lib' "$A/usr/bin/mpv"
for f in "$A"/usr/lib/*.so*; do [ -L "$f" ] || patchelf --set-rpath '$ORIGIN' "$f" 2>/dev/null || true; done
cat > "$A/AppRun" <<'RUN'
#!/bin/sh
HERE=$(dirname "$(readlink -f "$0")")
export PUSHMPC_ENGINE="${PUSHMPC_ENGINE:-$HERE/usr/lib/libpushmpc_engine.so}"
exec "$HERE/usr/bin/mpv" "$@"
RUN
chmod +x "$A/AppRun"
cat > "$A/pushmpc-mpv.desktop" <<'DESK'
[Desktop Entry]
Type=Application
Name=mpv (PushMPC)
GenericName=Video player
Comment=mpv with PushMPC frame generation and upscaling (vf_pushmpc)
Exec=mpv --player-operation-mode=pseudo-gui -- %U
Icon=mpv
Terminal=false
Categories=AudioVideo;Video;Player;
MimeType=video/mp4;video/x-matroska;video/webm;video/quicktime;video/x-msvideo;video/mpeg;video/mp2t;
DESK
cp "$A/pushmpc-mpv.desktop" "$A/usr/share/applications/"
icon=$(ls "$S"/mpv/etc/mpv-icon-8bit-128x128.png 2>/dev/null || ls "$S"/mpv/etc/*.png 2>/dev/null | head -1 || true)
if [ -n "$icon" ]; then cp "$icon" "$A/mpv.png"; else printf '' > "$A/mpv.png"; fi
cp "$ROOT/mpvnative/ci/SOURCES.txt" "$A/"
[ -f "$ROOT/LICENSE" ] && cp "$ROOT/LICENSE" "$A/"
# the runtime, checked against its pinned SHA-256 (the digest GitHub publishes for the release asset, recorded once)
repo=$(pin appimageRuntime repo); tag=$(pin appimageRuntime tag); asset=$(pin appimageRuntime asset); want=$(pin appimageRuntime sha256)
rt="$S/runtime-$tag"
if [ ! -f "$rt" ]; then
    curl -fsSL --retry 3 -o "$rt" "https://github.com/$repo/releases/download/$tag/$asset"
    got=$(sha256sum "$rt" | cut -d' ' -f1)
    echo "runtime $tag: sha256 $got"
    [ "$got" = "$want" ] || { rm -f "$rt"; echo "::error::AppImage runtime SHA-256 mismatch ($got, pinned $want)"; exit 1; }
fi
rm -f "$S/AppDir.squashfs"
mksquashfs "$A" "$S/AppDir.squashfs" -root-owned -noappend -comp zstd -quiet
cat "$rt" "$S/AppDir.squashfs" > "$OUT/PushMPC-mpv-x86_64.AppImage"
chmod +x "$OUT/PushMPC-mpv-x86_64.AppImage"
cp "$ROOT/mpvnative/ci/SOURCES.txt" "$OUT/"
ls -la "$OUT"; du -sh "$A"
