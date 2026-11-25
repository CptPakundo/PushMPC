#!/usr/bin/env bash
# Runs the VLC spike cases headless and summarises each with analyze.py.
# env: VLC (vlc binary, may contain spaces), PLUGDIR (dir with libpushspike_plugin.*), CLIP, OUT (output dir),
#      SRC_FRAMES (frames in CLIP), CASES (optional space-separated subset), EXTRA (optional extra vlc args),
#      PY (python, default python3), VLC_WRAP (optional command prefix, e.g. "flatpak run ... org.videolan.VLC")
set -u
here="$(cd "$(dirname "$0")" && pwd)"
PY="${PY:-python3}"
mkdir -p "$OUT"
export VLC_PLUGIN_PATH="$PLUGDIR"
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
  {
    echo "### ${LABEL:-VLC spike} ($("$PY" -c 'import platform; print(platform.system())'))"
    echo
    echo "| case | verdict | frames / expected | unique | size | median delta ms | on grid % | non-monotonic | late ms median / p95 | alloc fail | dropped by chain | chain |"
    echo "|---|---|---|---|---|---|---|---|---|---|---|---|"
  } >> "$GITHUB_STEP_SUMMARY"
fi

run() { # name mult expect_size -- vlc args...
  local name="$1" mult="$2" size="$3"; shift 4
  local csv="$OUT/$name.csv" log="$OUT/$name.log"
  rm -f "$csv" "$log"
  local t0=$(date +%s)
  local cmd=()
  # shellcheck disable=SC2206
  [ -n "${VLC_WRAP:-}" ] && cmd+=($VLC_WRAP)
  [ -n "${VLC:-}" ] && cmd+=("$VLC")
  # shellcheck disable=SC2086
  timeout 90 "${cmd[@]}" -I dummy --no-audio --play-and-exit --no-video-title-show --no-osd \
    --file-logging --logfile="$log" --log-verbose=2 \
    --vout=pushdump --pushdump-file="$csv" ${HWARGS---avcodec-hw=none} ${EXTRA:-} "$@" "$CLIP" >"$OUT/$name.stdout" 2>&1
  local rc=$?
  echo "== $name: vlc exit $rc in $(( $(date +%s) - t0 )) s"
  [ -s "$log" ] || cp "$OUT/$name.stdout" "$log" 2>/dev/null
  grep -E "pushspike|pushdump|as static|as interactive|compensate|dropping pictures|Failed to" "$log" | head -15
  (cd "$OUT" && "$PY" "$here/analyze.py" "$name" "$csv" "$log" --src-frames "$SRC_FRAMES" --mult "$mult" ${size:+--expect-size "$size"})
}

if ! command -v timeout >/dev/null 2>&1; then # macOS has no coreutils timeout
  timeout() { local s="$1"; shift; perl -e 'alarm shift; exec @ARGV or die "exec: $!"' "$s" "$@"; }
fi

want() { [ -z "${CASES:-}" ] || [[ " $CASES " == *" $1 "* ]]; }

S="${SRC_SIZE:-320x180}"
want base        && run base        1 "$S" --
want pp_m2       && run pp_m2       2 "$S" -- --video-filter=postproc --pushspike-mult=2
want pp_m3       && run pp_m3       3 "$S" -- --video-filter=postproc --pushspike-mult=3
want pp_m4       && run pp_m4       4 "$S" -- --video-filter=postproc --pushspike-mult=4
want pp_m5       && run pp_m5       5 "$S" -- --video-filter=postproc --pushspike-mult=5
want pp_m8       && run pp_m8       8 "$S" -- --video-filter=postproc --pushspike-mult=8
want cfg_m4      && run cfg_m4      4 "$S" -- "--video-filter=postproc{mult=4}"
want heap_m4     && run heap_m4     4 "$S" -- --video-filter=postproc --pushspike-mult=4 --pushspike-alloc=1
want heap_m8     && run heap_m8     8 "$S" -- --video-filter=postproc --pushspike-mult=8 --pushspike-alloc=1
want plusi_m8    && run plusi_m8    8 "$S" -- "--video-filter=postproc{mult=8}:pushspike{mult=1}"
want named_m2    && run named_m2    2 "$S" -- --video-filter=pushspike --pushspike-mult=2
want scale2      && run scale2      1 ""   -- --video-filter=postproc --pushspike-mult=1 --pushspike-scale=2
want scale2_m2   && run scale2_m2   2 ""   -- --video-filter=postproc --pushspike-mult=2 --pushspike-scale=2
# Windows: the d3d11va decoder hands the static chain D3D11 textures ('DX11'); the filter logs texture + slice and
# outputs mult-1 clones sharing the texture; VLC copies back to the CPU display after the chain.
if [ "${D3D11:-0}" = 1 ]; then
  want d3d11_m2 && run d3d11_m2 2 "$S" -- --avcodec-hw=d3d11va --video-filter=postproc --pushspike-mult=2
  want d3d11_m4 && run d3d11_m4 4 "$S" -- --avcodec-hw=d3d11va --video-filter=postproc --pushspike-mult=4
  want d3d11_m8 && run d3d11_m8 8 "$S" -- --avcodec-hw=d3d11va --video-filter=postproc --pushspike-mult=8
fi
exit 0
