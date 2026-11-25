#!/usr/bin/env bash
# The module's hardware-format path (VA-API through VLC's DRM decoder or its OpenGL interop, VDPAU under X) against
# PushMPC's null drivers (tests/fakehw: no GPU needed), headless, with the CSV display (libpushdump_plugin) in the same
# VLC_PLUGIN_PATH folder. Each case is judged by hwcheck.py (decoder, PushMPC's path, frame count, the moving box's steps).
# env: VLC, PLUGDIR, FAKEHW (pmnull_drv_video.so, libvdpau_pmnull.so.1), CLIP (H.264, no B-frames), SRC_FRAMES,
#      CLIP10 + SRC_FRAMES10 (HEVC Main 10, optional), OUT, PY, LABEL, CASES (optional subset), VDPAU=1 (needs X),
#      ZC=1 (zero-copy cases: the GPU engine on lavapipe, /dev/udmabuf), GL=1 (OpenGL interop cases: X with EGL dma-buf
#      import, i.e. Mesa >= 24.1)
set -u
here="$(cd "$(dirname "$0")" && pwd)"
PY="${PY:-python3}"
mkdir -p "$OUT"
OUTABS="$(cd "$OUT" && pwd)"
export VLC_PLUGIN_PATH="$PLUGDIR"
export LIBVA_DRIVERS_PATH="$FAKEHW" LIBVA_DRIVER_NAME=pmnull VDPAU_DRIVER=pmnull VDPAU_DRIVER_PATH="$FAKEHW"
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
  { echo "### ${LABEL:-PushMPC hardware-format path}"; echo
    echo "| case | verdict | expect | frames / expected | box steps on target | median step / target px | generated | path |"
    echo "|---|---|---|---|---|---|---|---|"; } >> "$GITHUB_STEP_SUMMARY"
fi
fails=0
want() { [ -z "${CASES:-}" ] || [[ " $CASES " == *" $1 "* ]]; }
# run NAME API(va|vdpau|vagl) EXPECT(base|hw|pass) MULT CLIP SRCN "QUIRK=1 ..." -- vlc arguments
#   vagl: VLC's VA-API decoder with its OpenGL output and GL interop (X needed); the null driver logs what the display
#         read (PMNULL_SHOWLOG), and hwcheck judges that log in place of the CSV display
#   CHECK="...": more hwcheck.py arguments for the case (--need, --zc)
run() {
  local name="$1" api="$2" expect="$3" mult="$4" clip="$5" srcn="$6" quirks="$7"; shift 8
  want "$name" || return 0
  local log="$OUT/$name.log" csv="$OUTABS/$name.csv" hw=vaapi_drm
  [ "$api" = vdpau ] && hw=vdpau
  rm -f "$log" "$csv"
  local vout=(--vout=pushdump --pushdump-file="$csv")
  if [ "$api" = vagl ]; then
    hw=vaapi
    vout=(--vout=gl --no-drop-late-frames --no-skip-frames --width=640 --height=360)   # X11 (Xvfb): VLC's xcb window
    quirks="$quirks PMNULL_SHOWLOG=$csv"
  fi
  # shellcheck disable=SC2086
  env $quirks timeout 90 "$VLC" -I dummy --no-audio --play-and-exit --no-video-title-show --no-osd --file-logging \
    --logfile="$log" --log-verbose=2 "${vout[@]}" --avcodec-hw=$hw "$@" "$clip" > "$OUT/$name.stdout" 2>&1
  local rc=$?
  [ -s "$log" ] || cp "$OUT/$name.stdout" "$log" 2>/dev/null
  echo "== $name ($api, $quirks): exit $rc"
  grep -hE "for hardware decoding|PushMPC|pmnull|using hw decoder|using glconv|zero copy|Failed to|compensate" "$log" | sed 's/^[^:]*: //' | sort | uniq -c | head -12
  # shellcheck disable=SC2086
  "$PY" "$here/hwcheck.py" "$name" "$csv" "$OUTABS/$name.log" --expect "$expect" --mult "$mult" --src-frames "$srcn" ${CHECK:-} || fails=$((fails + 1))
  [ "$rc" = 0 ] || [ "$rc" = 124 ] || { echo "::error::$name: VLC exit $rc"; fails=$((fails + 1)); }
}
PP="--video-filter=postproc:pushmpc_out"
S="$SRC_FRAMES"
run va_base        va base 1 "$CLIP" "$S" "" --
run va_x2          va hw   2 "$CLIP" "$S" "" -- $PP --pushmpc-smoother=2
run va_x4          va hw   4 "$CLIP" "$S" "" -- $PP --pushmpc-smoother=4
run va_x8          va hw   8 "$CLIP" "$S" "" -- $PP --pushmpc-smoother=8
run va_x4_sync     va hw   4 "$CLIP" "$S" "" -- $PP --pushmpc-smoother=4 --no-pushmpc-worker
# without pushmpc_out: PushMPC's own surface pool, so no x4 cap from VLC's pool
run va_x8_noout    va hw   8 "$CLIP" "$S" "" -- --video-filter=postproc --pushmpc-smoother=8
# what real drivers differ in (the null driver's quirk switches)
run va_noderive_x4 va hw   4 "$CLIP" "$S" "PMNULL_DERIVE=0" -- $PP --pushmpc-smoother=4
run va_noput_x4    va hw   4 "$CLIP" "$S" "PMNULL_PUTIMAGE=0" -- $PP --pushmpc-smoother=4
run va_noget_x4    va hw   4 "$CLIP" "$S" "PMNULL_GETIMAGE=0" -- $PP --pushmpc-smoother=4
run va_oddpitch_x4 va hw   4 "$CLIP" "$S" "PMNULL_PITCH=odd" -- $PP --pushmpc-smoother=4
# drivers that cannot take uploads: the round trip fails, PushMPC passes frames through, playback goes on
run va_broken_x4   va pass 4 "$CLIP" "$S" "PMNULL_BROKEN=1" -- $PP --pushmpc-smoother=4
run va_noupload_x4 va pass 4 "$CLIP" "$S" "PMNULL_DERIVE=0 PMNULL_PUTIMAGE=0" -- $PP --pushmpc-smoother=4
if [ -n "${CLIP10:-}" ]; then
  run va10_base    va base 1 "$CLIP10" "${SRC_FRAMES10:-$S}" "" --
  run va10_x4      va hw   4 "$CLIP10" "${SRC_FRAMES10:-$S}" "" -- $PP --pushmpc-smoother=4
fi
# zero copy: VA-API surfaces as dmabufs into the GPU engine (lavapipe here) and back; and the fallbacks when the
# driver does not export or the Vulkan driver cannot import
if [ "${ZC:-0}" = 1 ]; then
  # lavapipe needs ~200 ms a source frame: x2, late frames kept, and no frame-count check (src 0); the transport checks
  # (no jumps back, new positions, zero-copy counters) stay
  GPU="--pushmpc-engine=3 --no-drop-late-frames --no-skip-frames"
  CHECK="--zc on" run va_zc_x2      va hw 2 "$CLIP" 0 "PUSHMPC_VK_ALLOW_CPU=1" -- $PP --pushmpc-smoother=2 $GPU
  CHECK="--zc on" run va_zc_x2_sync va hw 2 "$CLIP" 0 "PUSHMPC_VK_ALLOW_CPU=1" -- $PP --pushmpc-smoother=2 --no-pushmpc-worker $GPU
  if [ -n "${CLIP10:-}" ]; then
    CHECK="--zc on" run va10_zc_x2  va hw 2 "$CLIP10" 0 "PUSHMPC_VK_ALLOW_CPU=1" -- $PP --pushmpc-smoother=2 $GPU
  fi
  CHECK="--zc off --need zero.copy.not.used"    run va_zc_noexport_x2 va hw 2 "$CLIP" 0 "PUSHMPC_VK_ALLOW_CPU=1 PMNULL_EXPORT=0" -- $PP --pushmpc-smoother=2 $GPU
  CHECK="--zc off --need zero.copy.unavailable" run va_zc_novk_x2     va hw 2 "$CLIP" 0 "PUSHMPC_VK_ALLOW_CPU=1 PUSHMPC_VK_NO_DMABUF=1" -- $PP --pushmpc-smoother=2 $GPU
fi
# VLC's own desktop path: the VA-API decoder on the OpenGL output's surfaces (glconv_vaapi_x11, EGL dma-buf import), with
# PushMPC's pictures shown by the GL interop directly
if [ "${GL:-0}" = 1 ]; then
  NEED='--need using.hw.decoder.module."vaapi" --need using.glconv.module."glconv_vaapi_x11"'
  CHECK="$NEED"          run vagl_base      vagl base 1 "$CLIP" "$S" "" --
  CHECK="$NEED"          run vagl_x2        vagl hw   2 "$CLIP" "$S" "" -- $PP --pushmpc-smoother=2
  CHECK="$NEED --zc off" run vagl_broken_x2 vagl pass 2 "$CLIP" "$S" "PMNULL_BROKEN=1" -- $PP --pushmpc-smoother=2
  if [ "${ZC:-0}" = 1 ]; then
    CHECK="$NEED --zc on" run vagl_zc_x2    vagl hw   2 "$CLIP" 0 "PUSHMPC_VK_ALLOW_CPU=1" -- $PP --pushmpc-smoother=2 --pushmpc-engine=3
  fi
fi
if [ "${VDPAU:-0}" = 1 ]; then
  run vdp_base       vdpau base 1 "$CLIP" "$S" "" --
  run vdp_x4         vdpau hw   4 "$CLIP" "$S" "" -- $PP --pushmpc-smoother=4
  run vdp_x4_sync    vdpau hw   4 "$CLIP" "$S" "" -- $PP --pushmpc-smoother=4 --no-pushmpc-worker
  run vdp_noput_x4   vdpau pass 4 "$CLIP" "$S" "PMNULL_PUTBITS=0" -- $PP --pushmpc-smoother=4
  run vdp_broken_x4  vdpau pass 4 "$CLIP" "$S" "PMNULL_BROKEN=1" -- $PP --pushmpc-smoother=4
fi
echo "hardware-format cases: $fails failed"
exit $((fails > 0))
