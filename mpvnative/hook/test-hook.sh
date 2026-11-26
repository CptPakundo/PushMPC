#!/bin/sh
# test-hook.sh <hook dir> <engine .so> <work dir>: the distro's own mpv through pushmpc_hook.so (run inside the distro:
# runner or container). Checks: the direct filter (x4, encode mode), a lavfi graph string naming pushmpc (x2), and
# display-resample on vo=null. Needs mpv, ffmpeg (ffprobe) and python3.
set -u
H=$1; E=$2; T=$3; HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$T"; fails=0
export PUSHMPC_ENGINE="$E"
echo "distro: $(. /etc/os-release; echo "$PRETTY_NAME"); $(mpv --version | head -1); $(ffmpeg -version | head -1)"
ffmpeg -v error -y -f lavfi -i "testsrc2=size=640x360:rate=24000/1001:duration=4" -pix_fmt yuv420p -c:v ffv1 "$T/clip.mkv"
ffmpeg -v error -y -f lavfi -i "testsrc2=size=640x360:rate=24000/1001:duration=12" -pix_fmt yuv420p -c:v ffv1 "$T/clip12.mkv"
n=$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 "$T/clip.mkv")
mpv --no-config --script="$H/pushmpc_hook.so" --script-opts=pushmpc-vf=smoother=4 "$T/clip.mkv" \
    --o="$T/hook.mkv" --of=matroska --ovc=ffv1 --no-audio > "$T/hook.log" 2>&1
grep -E "pushmpc_hook|filter:" "$T/hook.log" | head -4
if [ ! -s "$T/hook.mkv" ] && command -v gdb >/dev/null; then   # a crash: where
    gdb -batch -ex run -ex bt -ex "info sharedlibrary" --args mpv --no-config --script="$H/pushmpc_hook.so" --script-opts=pushmpc-vf=smoother=4 "$T/clip.mkv" \n        --o="$T/hook-gdb.mkv" --of=matroska --ovc=ffv1 --no-audio 2>&1 | grep -E "^#|SIGSEGV|pushmpc|avfilter|avutil" | head -40
fi
python3 "$HERE/../tests/check_frames.py" "$T/hook.mkv" $((1 + (n - 1) * 4)) 96000/1001 --tolerance 3 || fails=$((fails + 1))
if grep -q "graph strings: unavailable" "$T/hook.log"; then echo "graph strings: not hooked with this FFmpeg (as designed below 6.0)"; else
mpv --no-config --script="$H/pushmpc_hook.so" --script-opts=pushmpc-append=no "--vf=lavfi=[format=yuv420p,pushmpc=smoother=2]" "$T/clip.mkv" \
    --o="$T/hook-graph.mkv" --of=matroska --ovc=ffv1 --no-audio > "$T/hook-graph.log" 2>&1
grep -E "pushmpc_hook|filter:|Parsed_pushmpc" "$T/hook-graph.log" | head -4
python3 "$HERE/../tests/check_frames.py" "$T/hook-graph.mkv" $((1 + (n - 1) * 2)) 48000/1001 --tolerance 3 || fails=$((fails + 1))
fi
mpv --no-config --script="$H/pushmpc_hook.so" --script-opts=pushmpc-vf=smoother=4 --vo=null --vo-null-fps=95.904 \
    --video-sync=display-resample --ao=null --script="$HERE/../tests/sync.lua" "$T/clip12.mkv" > "$T/hook-sync.log" 2>&1
grep "pushmpc-ci" "$T/hook-sync.log" | tee "$T/sync.txt"
grep -q "after-seek: estimated-vf-fps=9[56]\." "$T/sync.txt" || fails=$((fails + 1))
[ "$fails" = 0 ] && echo "hook: all checks passed" || { echo "hook: $fails check(s) failed"; tail -20 "$T/hook.log"; exit 1; }
