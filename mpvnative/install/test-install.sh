#!/bin/sh
# test-install.sh <folder with the hook files> <work dir>: pushmpc-native.sh in a fake HOME against the system's mpv:
# install (with an existing mpv.conf), play through the installed config (x4), status, uninstall (byte-exact restore,
# folder gone). Needs mpv, ffmpeg (ffprobe) and python3.
set -u
H=$1; T=$2; HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$T"; fails=0
export HOME="$T/home" XDG_CONFIG_HOME="$T/home/.config" XDG_DATA_HOME="$T/home/.local/share"
mkdir -p "$XDG_CONFIG_HOME/mpv"
printf 'volume=50\n[myprofile]\nvolume=80\n' > "$XDG_CONFIG_HOME/mpv/mpv.conf"
cp "$XDG_CONFIG_HOME/mpv/mpv.conf" "$T/orig.conf"
ffmpeg -v error -y -f lavfi -i "testsrc2=size=640x360:rate=24000/1001:duration=4" -pix_fmt yuv420p -c:v ffv1 "$T/clip.mkv"
n=$(ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames -of csv=p=0 "$T/clip.mkv")

out=$(sh "$HERE/pushmpc-native.sh" install --from "$H" --target native --opts smoother=4 --json); echo "install: $out"
echo "$out" | grep -q '"state":"installed"' || fails=$((fails + 1))
echo "$out" | grep -q 'rebound avfilter_get_by_name' || fails=$((fails + 1))
head -4 "$XDG_CONFIG_HOME/mpv/mpv.conf"
mpv "$T/clip.mkv" --o="$T/installed.mkv" --of=matroska --ovc=ffv1 --no-audio > "$T/installed.log" 2>&1
python3 "$HERE/../tests/check_frames.py" "$T/installed.mkv" $((1 + (n - 1) * 4)) 96000/1001 --tolerance 3 || { fails=$((fails + 1)); tail -15 "$T/installed.log"; }
st=$(sh "$HERE/pushmpc-native.sh" status --target native --json); echo "status: $st"
echo "$st" | grep -q '"state":"installed"' || fails=$((fails + 1))
un=$(sh "$HERE/pushmpc-native.sh" uninstall --target native --json); echo "uninstall: $un"
cmp -s "$T/orig.conf" "$XDG_CONFIG_HOME/mpv/mpv.conf" || { echo "mpv.conf not restored byte for byte"; fails=$((fails + 1)); }
[ ! -e "$XDG_DATA_HOME/pushmpc/mpvnative" ] || { echo "install folder left behind"; fails=$((fails + 1)); }
ls "$XDG_CONFIG_HOME/mpv/" | grep -q pushmpc-native && { echo "backup files left behind"; fails=$((fails + 1)); }
[ "$fails" = 0 ] && echo "install: all checks passed" || { echo "install: $fails check(s) failed"; exit 1; }
