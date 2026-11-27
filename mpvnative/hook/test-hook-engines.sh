#!/bin/sh
# test-hook-engines.sh <hook dir> <work dir> [--vulkan-frames | --sidecars <pmml runtime>]: which engine the engine
# module opens in the distro's own mpv through pushmpc_hook.so, read from PUSHMPC_ENGINE_LOG. Needs mpv, ffmpeg
# (ffprobe) and python3; a Vulkan driver (lavapipe on the runners) for the Vulkan engine.
#   (default)        software frames: the Vulkan engine on lavapipe (vulkan_allow_cpu=1), x2; Auto without that option
#                    stays on the CPU engine and names the reason; backend=nvidia falls back with its reason
#   --vulkan-frames  FFV1 decoded by Vulkan (FFmpeg 8+), gpu-next on Vulkan under Xvfb: the frames go into the Vulkan
#                    engine with no copy. Best effort: where this mpv / driver decodes no Vulkan frames, a note, no fail
#   --sidecars <rt>  the sidecar engines next to the module (libpushmpc_engine_ml.so / _nvl.so): PushFlow through
#                    pmml on ncnn (x2 by frames), ONNX Runtime's WebGPU EP (best effort), Auto's PushFlow-versus-Vulkan
#                    choice, and the NVIDIA engine refusing with a reason (no NVIDIA GPU on the runners)
set -u
H=$1; T=$2; shift 2
mode=software; RT=''
case ${1:-} in
    --vulkan-frames) mode=vulkan ;;
    --sidecars) mode=sidecars; RT=${2:?--sidecars needs the pmml runtime folder} ;;
    '') ;;
    *) echo "test-hook-engines.sh: unknown option $1" >&2; exit 2 ;;
esac
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$T"; fails=0
[ -f "$H/libpushmpc_engine.so" ] && export PUSHMPC_ENGINE="$H/libpushmpc_engine.so"
echo "distro: $(. /etc/os-release; echo "$PRETTY_NAME"); $(mpv --version | head -1); $(ffmpeg -version | head -1); mode $mode"
command -v vulkaninfo >/dev/null && vulkaninfo --summary 2>/dev/null | grep -E "deviceName|driverName" | head -4

fail() { echo "FAIL: $*"; fails=$((fails + 1)); }
# run <name> <k> <pushmpc-vf options> [more mpv options]: encode mode (software frames), the engine's log in $T/<name>.engine.log
run() {
    name=$1; vf=$3; shift 3   # $2: the multiplier (frames_ok checks it)
    rm -f "$T/$name.engine.log" "$T/$name.mkv"
    PUSHMPC_ENGINE_LOG="$T/$name.engine.log" mpv --no-config --script="$H/pushmpc_hook.so" --script-opts="pushmpc-vf=$vf" "$@" "$clip" \
        --o="$T/$name.mkv" --of=matroska --ovc=ffv1 --no-audio --msg-level=all=v > "$T/$name.log" 2>&1
    echo "-- $name ($vf):"
    grep -m1 "pushmpc_hook\]" "$T/$name.log" | sed 's/^/   /'   # the vf append's own result ("error running command": options mpv could not parse)
    grep -E "^(open|open failed|software frames)" "$T/$name.engine.log" 2>/dev/null | sed 's/^/   /' | head -8
}
frames_ok() {   # frames_ok <name> <k>
    python3 "$HERE/../tests/check_frames.py" "$T/$1.mkv" $((1 + (n - 1) * $2)) "$(rate "$2")" --tolerance 3
}
rate() { echo "$((24000 * $1))/1001"; }
opened() { grep -q "^open: $2:" "$T/$1.engine.log" 2>/dev/null; }

if [ "$mode" = software ]; then
    clip=$T/clip.mkv
    ffmpeg -v error -y -f lavfi -i "testsrc2=size=640x360:rate=24000/1001:duration=3" -pix_fmt yuv420p -c:v ffv1 "$clip"
    n=$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 "$clip")
    # 1. the Vulkan engine on the runner's lavapipe (a CPU device: only with vulkan_allow_cpu=1)
    run vk 2 "smoother=2:extra=[vulkan_allow_cpu=1]"
    opened vk portable-vulkan || fail "Auto with vulkan_allow_cpu=1 did not open the Vulkan engine ($(grep -m1 'portable-vulkan not used' "$T/vk.engine.log" 2>/dev/null))"
    frames_ok vk 2 || fail "Vulkan engine: frames"
    # 2. Auto as users get it: lavapipe is a CPU, so the CPU engine, and the log says why the Vulkan engine was not used
    run auto 2 "smoother=2"
    opened auto portable-cpu || fail "Auto on lavapipe did not stay on the CPU engine"
    grep -q "portable-vulkan not used: " "$T/auto.engine.log" || fail "Auto: no reason for leaving the Vulkan engine out"
    frames_ok auto 2 || fail "Auto (CPU engine): frames"
    # 3. backend=nvidia here (no NVIDIA engine in this folder, or no NVIDIA GPU): a reason, then the next engine
    run nv 2 "smoother=2:backend=nvidia:extra=[vulkan_allow_cpu=1]"
    grep -q "nvidia not used: " "$T/nv.engine.log" || fail "backend=nvidia: no reason logged"
    grep -q "^open: " "$T/nv.engine.log" || fail "backend=nvidia: no engine opened after the fallback"
    frames_ok nv 2 || fail "backend=nvidia fallback: frames"
    # 4. an engine asking for a 10-bit stream (NVIDIA's RTX Video HDR on 8-bit frames; here the test option makes the CPU
    #    engine ask): the filter records it, the hook reads it in-process (not vf-metadata), converts the stream to 10 bits ahead of the
    #    filter and sets it up again with source_depth=8 (real-time playback on vo=null: the hook looks every 0.5 s)
    ffmpeg -v error -y -f lavfi -i "testsrc2=size=640x360:rate=24000/1001:duration=8" -pix_fmt yuv420p -c:v ffv1 "$T/clip8.mkv"
    rm -f "$T/depth.engine.log"
    PUSHMPC_ENGINE_LOG="$T/depth.engine.log" timeout 120 mpv --no-config --script="$H/pushmpc_hook.so" \
        --script-opts="pushmpc-vf=smoother=2:extra=[test_want_out_depth=10]" --vo=null --ao=null --msg-level=all=v "$T/clip8.mkv" > "$T/depth.log" 2>&1
    echo "-- 10-bit request:"
    grep -E "pushmpc_hook|want_depth|would do more|frames.*x2" "$T/depth.log" | head -8 | sed 's/^/   /'
    grep -q "10-bit stream for the engine" "$T/depth.log" || fail "the hook did not answer the engine's 10-bit request"
    grep -q "on yuv420p10le frames" "$T/depth.log" || fail "the filter did not get the 10-bit stream after the request"
    [ "$(grep -c '^open: ' "$T/depth.engine.log" 2>/dev/null)" -ge 2 ] || fail "no second engine open after the request"
fi

if [ "$mode" = vulkan ]; then
    # FFV1 is the codec FFmpeg decodes with Vulkan compute (no video queue): lavapipe can do it
    clip=$T/clip-vk.mkv
    ffmpeg -v error -y -f lavfi -i "testsrc2=size=640x360:rate=24000/1001:duration=3" -pix_fmt yuv420p -c:v ffv1 "$clip"
    rm -f "$T/vkframes.engine.log"
    PUSHMPC_ENGINE_LOG="$T/vkframes.engine.log" timeout 300 xvfb-run -a -s "-screen 0 1280x720x24" mpv --no-config \
        --script="$H/pushmpc_hook.so" --script-opts="pushmpc-vf=smoother=2:extra=[vulkan_allow_cpu=1]" \
        --vo=gpu-next --gpu-api=vulkan --hwdec=vulkan --hwdec-codecs=all --ao=null --untimed --frames=60 \
        --msg-level=all=v "$clip" > "$T/vkframes.log" 2>&1
    echo "-- Vulkan frames:"
    grep -E "Using hardware decoding|Could not create device|hwdec|pushmpc" "$T/vkframes.log" | grep -viE "^\[.*\] *(Option|Setting)" | head -12 | sed 's/^/   /'
    grep -E "^(open|open failed|software frames)" "$T/vkframes.engine.log" 2>/dev/null | sed 's/^/   /' | head -6
    if grep -q "on vulkan frames (zero copy)" "$T/vkframes.log"; then
        echo "Vulkan frames went into the engine with no copy"
        grep -q "^open: portable-vulkan:" "$T/vkframes.engine.log" || fail "zero copy reported, but the engine log has no Vulkan engine"
    elif grep -qE "Using hardware decoding \(vulkan\)" "$T/vkframes.log" && grep -q "on vulkan frames" "$T/vkframes.log"; then
        fail "Vulkan frames reached the filter but took the copy fallback: $(grep -m1 'on vulkan frames' "$T/vkframes.log")"
    else
        echo "NOTE: this mpv / driver decoded no Vulkan frames here (see vkframes.log): zero copy not checked"
    fi
fi

if [ "$mode" = sidecars ]; then
    clip=$T/clip-s.mkv
    # small and short: PushFlow on lavapipe is slow
    ffmpeg -v error -y -f lavfi -i "testsrc2=size=320x180:rate=24000/1001:duration=1" -pix_fmt yuv420p -c:v ffv1 "$clip"
    n=$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 "$clip")
    for f in libpushmpc_engine_ml.so libpushmpc_engine_nvl.so; do [ -f "$H/$f" ] || fail "$f is not in $H"; done
    # 1. PushFlow through pmml on ncnn (Vulkan, lavapipe)
    run ml-ncnn 2 "smoother=2:backend=ml:extra=[ml_runtime=$RT|ml_backend=ncnn]"
    if opened ml-ncnn ml; then frames_ok ml-ncnn 2 || fail "PushFlow (ncnn): frames"
    else fail "backend=ml with ncnn did not open PushFlow: $(grep -m1 'ml not used' "$T/ml-ncnn.engine.log" 2>/dev/null)"; fi
    # 2. ONNX Runtime's WebGPU EP (Dawn on Vulkan): best effort, Dawn may refuse a CPU adapter
    run ml-webgpu 2 "smoother=2:backend=ml:extra=[ml_runtime=$RT|ml_backend=webgpu]"
    if opened ml-webgpu ml; then frames_ok ml-webgpu 2 || fail "PushFlow (WebGPU EP): frames"
    else echo "NOTE: the WebGPU EP did not run here: $(grep -m1 'ml not used' "$T/ml-webgpu.engine.log" 2>/dev/null)"; fi
    # 3. Auto: PushFlow measured against the Vulkan engine (or the reason PushFlow was left out)
    run auto-ml 2 "smoother=2:extra=[ml_runtime=$RT|vulkan_allow_cpu=1]"
    if grep -q "PushFlow reaches" "$T/auto-ml.engine.log"; then grep -m1 "PushFlow reaches" "$T/auto-ml.engine.log"
    elif grep -q "ml not used: " "$T/auto-ml.engine.log"; then echo "Auto left PushFlow out: $(grep -m1 'ml not used: ' "$T/auto-ml.engine.log")"
    else fail "Auto: neither a PushFlow-versus-Vulkan choice nor a reason in the log"; fi
    grep -q "^open: " "$T/auto-ml.engine.log" && frames_ok auto-ml 2 || fail "Auto (sidecars present): no engine or wrong frames"
    # 4. the NVIDIA sidecar without an NVIDIA GPU: refuses with the reason, the next engine runs
    run nv-sc 2 "smoother=2:backend=nvidia:extra=[vulkan_allow_cpu=1]"
    grep -q "nvidia not used: " "$T/nv-sc.engine.log" || fail "backend=nvidia: no reason logged"
    grep -m1 "nvidia not used: " "$T/nv-sc.engine.log"
    frames_ok nv-sc 2 || fail "backend=nvidia fallback: frames"
fi

[ "$fails" = 0 ] && echo "engines ($mode): all checks passed" || { echo "engines ($mode): $fails check(s) failed"; exit 1; }
