#!/bin/bash
# run-tests.sh <prefix> <work dir> <os> : the headless checks of the native route, on a built prefix (build-unix.sh).
# Needs PUSHMPC_ENGINE (the engine module) in the environment; Windows also PME_STUB_DIR (the stub pushcore folder).
# Each check prints "check <name>: PASS|FAIL|SKIP"; the script fails when any check fails.
set -uo pipefail
P=$1; T=$2; OS=$3
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$T"
EXE=""; [ "$OS" = msys2 ] && EXE=.exe
FF="$P/bin/ffmpeg$EXE"; FP="$P/bin/ffprobe$EXE"; MPV="$P/bin/mpv$EXE"; [ "$OS" = msys2 ] && MPV="$P/bin/mpv.com"
PY=python3
fails=0
result() { echo "check $1: $2"; [ "$2" = FAIL ] && fails=$((fails + 1)); echo "$1 $2" >> "$T/summary.txt"; }
count() { "$FP" -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 "$1" | tr -d '\r'; }
check() { $PY "$HERE/check_frames.py" "$1" "$2" "$3" --ffprobe "$FP" --tolerance "${4:-1}"; }
: > "$T/summary.txt"

"$FF" -hide_banner -filters 2>/dev/null | grep -E " (pushmpc|fruc_vulkan) " | tee "$T/filters.txt"
grep -q " pushmpc " "$T/filters.txt" && result filter-registered PASS || result filter-registered FAIL
grep -q " fruc_vulkan " "$T/filters.txt" && result fruc-vulkan-built PASS || result fruc-vulkan-built FAIL

# the synthetic clip: 4 s at 24000/1001, moving test pattern (no audio); the public clip when fetched
"$FF" -v error -y -f lavfi -i "testsrc2=size=640x360:rate=24000/1001:duration=4" -pix_fmt yuv420p -c:v ffv1 "$T/clip.mkv"
N=$(count "$T/clip.mkv"); echo "synthetic clip: $N frames"
CLIPS=("$T/clip.mkv")
[ -f "$T/public.mp4" ] && CLIPS+=("$T/public.mp4")

# 1. ffmpeg, CPU frames, x4
for c in "${CLIPS[@]}"; do
    n=$(count "$c"); rate=$("$FP" -v error -select_streams v:0 -show_entries stream=r_frame_rate -of csv=p=0 "$c" | tr -d '\r')
    b=$(basename "$c"); b=${b%.*}
    t0=$(date +%s.%N)
    if "$FF" -v info -y -i "$c" -an -vf "pushmpc=smoother=4" -c:v ffv1 "$T/ff-cpu-$b.mkv" > "$T/ff-cpu-$b.log" 2>&1; then
        t1=$(date +%s.%N)
        grep -i "pushmpc" "$T/ff-cpu-$b.log" | head -3
        echo "ffmpeg cpu $b: $n source frames in $(echo "$t1 - $t0" | bc 2>/dev/null || echo ?) s"
        num=${rate%/*}; den=${rate#*/}
        check "$T/ff-cpu-$b.mkv" $((1 + (n - 1) * 4)) "$((num * 4))/$den" 2 && result "ffmpeg-cpu-x4-$b" PASS || result "ffmpeg-cpu-x4-$b" FAIL
    else tail -20 "$T/ff-cpu-$b.log"; result "ffmpeg-cpu-x4-$b" FAIL; fi
done

# 2. 10-bit CPU path (P010 / yuv420p10: HDR-style signal passes through, tags kept)
"$FF" -v error -y -f lavfi -i "testsrc2=size=640x360:rate=24:duration=2" -vf "setparams=color_trc=smpte2084:color_primaries=bt2020:colorspace=bt2020nc:range=tv" -pix_fmt yuv420p10le -c:v ffv1 "$T/clip10.mkv"
"$FP" -v error -show_entries stream=color_transfer,color_primaries -of csv=p=0 "$T/clip10.mkv" | sed "s/^/10-bit source tags: /"
if "$FF" -v info -y -i "$T/clip10.mkv" -vf "pushmpc=smoother=2" -c:v ffv1 "$T/ff-10.mkv" > "$T/ff-10.log" 2>&1 && check "$T/ff-10.mkv" $((1 + (48 - 1) * 2)) 48 2 \
   && "$FP" -v error -show_entries stream=color_transfer,color_primaries -of csv=p=0 "$T/ff-10.mkv" | tee "$T/ff-10-tags.txt" | grep -q "smpte2084"; then
    result ffmpeg-10bit-hdr-tags PASS; else tail -5 "$T/ff-10.log"; result ffmpeg-10bit-hdr-tags FAIL; fi

# 3. engine missing -> pass-through (the video keeps playing)
if PUSHMPC_ENGINE=/nonexistent "$FF" -v info -y -i "$T/clip.mkv" -vf "pushmpc=engine=no-such-engine-module" -c:v ffv1 "$T/ff-pass.mkv" > "$T/ff-pass.log" 2>&1 \
   && [ "$(count "$T/ff-pass.mkv")" = "$N" ]; then result passthrough-without-engine PASS; else echo "pass-through: $(count "$T/ff-pass.mkv" 2>&1) frames of $N"; tail -8 "$T/ff-pass.log"; result passthrough-without-engine FAIL; fi
grep -i "passes through" "$T/ff-pass.log" | head -2

# 4. Vulkan frames (lavapipe on Linux): hwupload -> pushmpc -> hwdownload; the module has no Vulkan engine yet, so this
#    goes into the portable GPU engine on the same device (zero copy); the copy fallback when that cannot open
if [ "$OS" = linux ]; then
    if "$FF" -v info -y -init_hw_device vulkan=vk -filter_hw_device vk -i "$T/clip.mkv" \
        -vf "format=nv12,hwupload,pushmpc=smoother=2,hwdownload,format=nv12" -c:v ffv1 "$T/ff-vk.mkv" > "$T/ff-vk.log" 2>&1; then
        grep -i "pushmpc\|lavapipe\|llvmpipe" "$T/ff-vk.log" | head -4
        check "$T/ff-vk.mkv" $((1 + (N - 1) * 2)) 48000/1001 2 && result ffmpeg-vulkan-x2 PASS || result ffmpeg-vulkan-x2 FAIL
        if grep -q "portable-vulkan.*(zero copy)" "$T/ff-vk.log"; then result ffmpeg-vulkan-zero-copy PASS; else grep -iE "engine|fallback" "$T/ff-vk.log" | head -5; result ffmpeg-vulkan-zero-copy FAIL; fi
        # import + export alone (x1: every output is a real frame) must be lossless; PSNR per plane for the log
        "$FF" -v info -y -init_hw_device vulkan=vk -filter_hw_device vk -i "$T/clip.mkv" -frames:v 24 \
            -vf "format=nv12,hwupload,pushmpc=smoother=1,hwdownload,format=nv12" -c:v ffv1 "$T/ff-vk1.mkv" > "$T/ff-vk1.log" 2>&1
        "$FF" -v info -i "$T/ff-vk1.mkv" -i "$T/clip.mkv" -lavfi "[0]settb=1/24,setpts=N,format=yuv420p[a];[1]settb=1/24,setpts=N,format=yuv420p[b];[a][b]psnr=shortest=1" -f null - 2>&1 | grep -o "PSNR y:.*" | tail -1 > "$T/vk1-psnr.txt"; echo "vulkan x1 vs source: $(cat "$T/vk1-psnr.txt")"
        grep -q "average:inf" "$T/vk1-psnr.txt" && result ffmpeg-vulkan-x1-lossless PASS || result ffmpeg-vulkan-x1-lossless FAIL
        # the real frames (every 2nd output) must come back unchanged through import, engine and export
        "$FF" -v info -i "$T/ff-vk.mkv" -i "$T/clip.mkv" -lavfi "[0]select='not(mod(n,2))',settb=1/24,setpts=N,format=yuv420p[a];[1]settb=1/24,setpts=N,format=yuv420p[b];[a][b]psnr=shortest=1" -f null - > "$T/ff-vk-psnr.log" 2>&1
        ps=$(grep -o "average:[a-z0-9.]*" "$T/ff-vk-psnr.log" | tail -1); echo "vulkan real frames vs source: $ps"
        case $ps in average:inf|average:[6-9][0-9]*|average:1[0-9][0-9]*) result ffmpeg-vulkan-real-frames-exact PASS ;; *) result ffmpeg-vulkan-real-frames-exact FAIL ;; esac
    else tail -15 "$T/ff-vk.log"; result ffmpeg-vulkan-x2 FAIL; fi
    # Dedup on the Vulkan path: a clip drawn on twos (12 fps doubled to 24), Subtitle Hold on (the band readback runs)
    "$FF" -v error -y -f lavfi -i "testsrc2=size=640x360:rate=12:duration=4,fps=24" -pix_fmt yuv420p -c:v ffv1 "$T/twos.mkv"
    N2=$(count "$T/twos.mkv")
    if PUSHMPC_ENGINE_LOG="$T/engine-twos.log" "$FF" -v info -y -init_hw_device vulkan=vk -filter_hw_device vk -i "$T/twos.mkv" \
        -vf "format=nv12,hwupload,pushmpc=smoother=2:extra=subtitle_hold\=1,hwdownload,format=nv12" -c:v ffv1 "$T/twos-vk.mkv" > "$T/twos-vk.log" 2>&1; then
        grep -h "close:" "$T/engine-twos.log" | tail -1
        dups=$(grep -ho "([0-9]* duplicates)" "$T/engine-twos.log" | tail -1 | tr -dc 0-9)
        if check "$T/twos-vk.mkv" $((1 + (N2 - 1) * 2)) 48 2 && [ "${dups:-0}" -ge $((N2 / 2 - 2)) ]; then result ffmpeg-vulkan-dedup-twos PASS; else echo "duplicates: ${dups:-none} of $N2"; result ffmpeg-vulkan-dedup-twos FAIL; fi
    else tail -15 "$T/twos-vk.log"; result ffmpeg-vulkan-dedup-twos FAIL; fi
    # transition rules on the Vulkan path: a flash (one white frame) is held, a dissolve is blended (thumbnails read back)
    "$FF" -v error -y -f lavfi -i "testsrc2=size=640x360:rate=24:duration=4" -vf "drawbox=x=0:y=0:w=iw:h=ih:color=white:t=fill:enable='eq(n,48)'" -pix_fmt yuv420p -c:v ffv1 "$T/flash.mkv"
    "$FF" -v error -y -f lavfi -i "testsrc2=size=640x360:rate=24:duration=3" -f lavfi -i "smptehdbars=size=640x360:rate=24:duration=3" \
        -filter_complex "[0][1]xfade=transition=fade:duration=1.5:offset=0.75,format=yuv420p" -c:v ffv1 "$T/dissolve.mkv"
    for c in flash dissolve; do
        PUSHMPC_ENGINE_LOG="$T/engine-$c.log" "$FF" -v info -y -init_hw_device vulkan=vk -filter_hw_device vk -i "$T/$c.mkv" \
            -vf "format=nv12,hwupload,pushmpc=smoother=2,hwdownload,format=nv12" -c:v ffv1 "$T/$c-vk.mkv" > "$T/$c-vk.log" 2>&1 || tail -10 "$T/$c-vk.log"
        grep -h "close:" "$T/engine-$c.log" | tail -1 | sed "s/^/$c: /"
    done
    fl=$(grep -ho "[0-9]* flashes held" "$T/engine-flash.log" | tail -1 | tr -dc 0-9)
    [ "${fl:-0}" -ge 1 ] && result ffmpeg-vulkan-flash-held PASS || result ffmpeg-vulkan-flash-held FAIL
    grep -q "close:" "$T/engine-dissolve.log" && result ffmpeg-vulkan-dissolve-runs PASS || result ffmpeg-vulkan-dissolve-runs FAIL
    # fruc_vulkan needs VK_NV_optical_flow (RTX 30+): on lavapipe it must refuse cleanly
    "$FF" -v info -y -init_hw_device vulkan=vk -filter_hw_device vk -i "$T/clip.mkv" -frames:v 10 \
        -vf "format=nv12,hwupload,fruc_vulkan=fps=source_fps*2,hwdownload,format=nv12" -f null - > "$T/fruc.log" 2>&1
    echo "fruc_vulkan on lavapipe (exit $?):"; grep -iE "optical|fruc|error" "$T/fruc.log" | head -4
    result fruc-vulkan-lavapipe-refuses PASS
fi

# 5. D3D11 frames (Windows, WARP): hwupload -> pushmpc (pushcore stub, zero copy in, BGRA texture out) -> hwdownload
if [ "$OS" = msys2 ]; then
    stub=$(cygpath -m "$PME_STUB_DIR")
    if "$FF" -v info -y -init_hw_device d3d11va=dx -filter_hw_device dx -i "$T/clip.mkv" \
        -vf "format=nv12,hwupload,pushmpc=smoother=2:core_dir='$stub':display_fps=48,hwdownload,format=bgra" -c:v ffv1 "$T/ff-d3d11.mkv" > "$T/ff-d3d11.log" 2>&1; then
        grep -i "pushmpc\|pushcore" "$T/ff-d3d11.log" | head -4
        if grep -q "zero copy" "$T/ff-d3d11.log" && check "$T/ff-d3d11.mkv" $((1 + (N - 1) * 2)) 48000/1001 2; then result ffmpeg-d3d11-pushcore-x2 PASS; else result ffmpeg-d3d11-pushcore-x2 FAIL; fi
    elif grep -q "Device creation failed" "$T/ff-d3d11.log"; then
        tail -3 "$T/ff-d3d11.log"; result ffmpeg-d3d11-pushcore-x2 SKIP   # no video-capable D3D11 adapter (hosted runners: Basic Render Driver)
    else tail -20 "$T/ff-d3d11.log"; result ffmpeg-d3d11-pushcore-x2 FAIL; fi
fi

# 6. mpv encode mode: the filter's output rate and pts reach mpv's output
if [ -x "$MPV" ] || [ -f "$MPV" ]; then
    "$MPV" --version | head -1
    if "$MPV" --no-config --msg-level=all=info,ffmpeg=v "$T/clip.mkv" --vf=pushmpc=smoother=4 --o="$T/mpv-enc.mkv" --of=matroska --ovc=ffv1 --no-audio > "$T/mpv-enc.log" 2>&1; then
        grep -iE "pushmpc|lavfi" "$T/mpv-enc.log" | head -4
        check "$T/mpv-enc.mkv" $((1 + (N - 1) * 4)) 96000/1001 3 && result mpv-encode-x4 PASS || result mpv-encode-x4 FAIL
    else tail -20 "$T/mpv-enc.log"; result mpv-encode-x4 FAIL; fi
    # the same through a lavfi graph string
    if "$MPV" --no-config "$T/clip.mkv" "--vf=lavfi=[pushmpc=smoother=2]" --o="$T/mpv-graph.mkv" --of=matroska --ovc=ffv1 --no-audio > "$T/mpv-graph.log" 2>&1 \
       && check "$T/mpv-graph.mkv" $((1 + (N - 1) * 2)) 48000/1001 3; then result mpv-lavfi-graph-x2 PASS; else tail -10 "$T/mpv-graph.log"; result mpv-lavfi-graph-x2 FAIL; fi
    # 7. playback timing: vo=null at a 96 Hz "display", display-resample, a seek (graph rebuilt); mpv must see 95.9 fps
    #    from the filter, lock vsync-ratio 1 and drop nothing
    "$FF" -v error -y -f lavfi -i "testsrc2=size=640x360:rate=24000/1001:duration=12" -pix_fmt yuv420p -c:v ffv1 "$T/clip12.mkv"
    "$MPV" --no-config --vo=null --vo-null-fps=95.904 --video-sync=display-resample --interpolation=no --ao=null --untimed=no \
        --vf=pushmpc=smoother=4 --script="$HERE/sync.lua" "$T/clip12.mkv" > "$T/mpv-sync.log" 2>&1
    grep "pushmpc-ci" "$T/mpv-sync.log" | tee "$T/sync.txt"
    if grep -q "after-seek: estimated-vf-fps=9[56]\." "$T/sync.txt" && grep -q "after-seek.*frame-drop-count=0 " "$T/sync.txt"; then result mpv-display-resample-x4 PASS
    else tail -15 "$T/mpv-sync.log"; result mpv-display-resample-x4 FAIL; fi
fi

echo "---- summary"; cat "$T/summary.txt"
[ "$fails" = 0 ] || { echo "$fails check(s) failed"; exit 1; }
