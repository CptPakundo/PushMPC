#!/usr/bin/env bash
# Headless tests of the real module (libpushmpc_plugin) with the CSV display (libpushdump_plugin, built from
# vlcplugin/spike/pushspike.c with -DPUSHSPIKE_DUMP_ONLY), both from one VLC_PLUGIN_PATH folder.
# env: VLC, PLUGDIR, CLIP, CLIP10 (10-bit clip, optional), OUT, SRC_FRAMES, PY, LABEL, CASES, HWARGS, VLC_WRAP,
#      DISPLAYS (optional: real vouts for the crash/rate check, e.g. "gl xcb_x11"), NORC=1 (no rc interface: Windows)
set -u
here="$(cd "$(dirname "$0")" && pwd)"
an="$here/../spike/analyze.py"
PY="${PY:-python3}"
mkdir -p "$OUT"
export VLC_PLUGIN_PATH="$PLUGDIR"
if ! command -v timeout >/dev/null 2>&1; then
  timeout() { local s="$1"; shift; perl -e 'alarm shift; exec @ARGV or die "exec: $!"' "$s" "$@"; }
fi
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
  {
    echo "### ${LABEL:-PushMPC VLC module}"
    echo
    echo "| case | verdict | frames / expected | unique | size | median delta ms | on grid % | non-monotonic | late ms median / p95 | alloc fail | dropped by chain | chain |"
    echo "|---|---|---|---|---|---|---|---|---|---|---|---|"
  } >> "$GITHUB_STEP_SUMMARY"
fi
BASECMD=()
# shellcheck disable=SC2206
[ -n "${VLC_WRAP:-}" ] && BASECMD+=($VLC_WRAP)
[ -n "${VLC:-}" ] && BASECMD+=("$VLC")

run() { # name mult clip -- args
  local name="$1" mult="$2" clip="$3"; shift 4
  local log="$OUT/$name.log" csv="$OUT/$name.csv"
  rm -f "$log" "$csv"
  timeout 90 "${BASECMD[@]}" -I dummy --no-audio --play-and-exit --no-video-title-show --no-osd \
    --file-logging --logfile="$log" --log-verbose=2 --vout=pushdump --pushdump-file="$csv" \
    ${HWARGS---avcodec-hw=none} "$@" "$clip" >"$OUT/$name.stdout" 2>&1
  local rc=$?
  [ -s "$log" ] || cp "$OUT/$name.stdout" "$log" 2>/dev/null
  echo "== $name: exit $rc"
  grep -hE "PushMPC|as static|as interactive|compensate|Failed to" "$log" | sed 's/^[^:]*: //' | sort | uniq -c | head -8
  local abs; abs="$(cd "$OUT" && pwd)"
  (cd "$OUT" && "$PY" "$an" "$name" "$abs/$name.csv" "$abs/$name.log" --src-frames "$SRC_FRAMES" --mult "$mult")
}
want() { [ -z "${CASES:-}" ] || [[ " $CASES " == *" $1 "* ]]; }

PP="--video-filter=postproc:pushmpc_out"
want base      && run base      1 "$CLIP" --
want off       && run off       1 "$CLIP" -- $PP --pushmpc-smoother=1
want x2        && run x2        2 "$CLIP" -- $PP --pushmpc-smoother=2
want x4        && run x4        4 "$CLIP" -- $PP --pushmpc-smoother=4
want x8        && run x8        8 "$CLIP" -- $PP --pushmpc-smoother=8
want x8_noout  && run x8_noout  4 "$CLIP" -- --video-filter=postproc --pushmpc-smoother=8   # expected: capped at x4
# Auto cases pass mult 0: the analyzer takes the multiplier the module picked (display x5 / x6, minus what the
# runner's CPU cannot generate in time) from its log, and checks that VLC showed that many frames on time
want auto120   && PUSHMPC_DISPLAY_HZ=120 run auto120 0 "$CLIP" -- $PP
if want vlcrc144; then  # the installer's route: everything in vlcrc, Auto, a 144 Hz display -> x6
  printf '[core]\nvideo-filter=postproc:pushmpc_out\n[pushmpc]\npushmpc-smoother=0\npushmpc-display-hz=144\n' > "$OUT/vlcrc"
  run vlcrc144 0 "$CLIP" -- --config="$OUT/vlcrc"
fi
# after the installer ran: no arguments at all, everything comes from its vlcrc and VLC_PLUGIN_PATH (only when asked)
if [[ " ${CASES:-} " == *" installed "* ]]; then
  run installed "${INSTALLED_MULT:-0}" "$CLIP" --
fi
# the platform's default decoding (hardware where VLC finds it: VideoToolbox, VA-API, D3D11VA); VLC converts the
# hardware pictures in front of the filter and back after it, or drops the filters when it cannot
want x4_hwdec  && HWARGS=" " run x4_hwdec 4 "$CLIP" -- $PP --pushmpc-smoother=4
if [ -n "${CLIP10:-}" ] && want x4_10bit; then
  run x4_10bit 4 "$CLIP10" -- $PP --pushmpc-smoother=4
fi

# real displays: no crash, and VLC's own count of displayed frames (rc interface) where available
if [ -n "${DISPLAYS:-}" ]; then
  [ -n "${GITHUB_STEP_SUMMARY:-}" ] && printf '\n| display | case | displayed | lost | exit |\n|---|---|---|---|---|\n' >> "$GITHUB_STEP_SUMMARY"
  for vout in $DISPLAYS; do
    for c in "none" "x4:$PP --pushmpc-smoother=4" "x8:$PP --pushmpc-smoother=8"; do
      tag="${c%%:*}"; args="${c#*:}"; [ "$c" = none ] && args=""
      log="$OUT/display-$vout-$tag.log"; rcout="$OUT/display-$vout-$tag.rc"
      echo "+ $vout $tag: ${BASECMD[*]} --vout=$vout ${HWARGS---avcodec-hw=none} $args" > "$rcout"
      if [ "${NORC:-0}" = 1 ]; then
        # shellcheck disable=SC2086
        timeout 60 "${BASECMD[@]}" -I dummy --no-audio --play-and-exit --file-logging --logfile="$log" --log-verbose=2 \
          --vout="$vout" ${HWARGS---avcodec-hw=none} $args "$CLIP" >> "$rcout" 2>&1
      else
        # shellcheck disable=SC2086
        { sleep 5; echo stats; sleep 1; echo quit; } | timeout 60 "${BASECMD[@]}" -I rc --rc-fake-tty --no-audio --play-and-exit \
          --file-logging --logfile="$log" --log-verbose=2 --vout="$vout" ${HWARGS---avcodec-hw=none} $args "$CLIP" >> "$rcout" 2>&1
      fi
      rc=$?
      disp=$(grep -i "frames displayed" "$rcout" | tail -1 | grep -o '[0-9]\+' | tail -1)
      lost=$(grep -i "frames lost" "$rcout" | tail -1 | grep -o '[0-9]\+' | tail -1)
      gen=$(grep -ho "generated=[0-9]*" "$log" 2>/dev/null | tail -1)
      echo "== display $vout $tag: displayed=${disp:-n/a} lost=${lost:-n/a} exit=$rc $gen"
      [ -n "${GITHUB_STEP_SUMMARY:-}" ] && echo "| $vout | $tag | ${disp:-n/a} | ${lost:-n/a} | $rc ($gen) |" >> "$GITHUB_STEP_SUMMARY"
    done
  done
fi
exit 0
