#!/usr/bin/env bash
# Real video outputs (not the CSV dump): does the static-chain multiplier survive VLC's own displays, including output
# pictures that come from the heap instead of the display pool (needed above x4)? Reads VLC's input statistics
# ("displayed frames", "lost frames") from the rc interface near the end of the clip, and checks VLC exits cleanly.
# env: VLC, PLUGDIR, CLIP, OUT, VOUTS (space-separated vout modules), HWARGS (optional), DUR (clip seconds, default 6)
set -u
mkdir -p "$OUT"
export VLC_PLUGIN_PATH="$PLUGDIR"
DUR="${DUR:-6}"
if ! command -v timeout >/dev/null 2>&1; then
  timeout() { local s="$1"; shift; perl -e 'alarm shift; exec @ARGV or die "exec: $!"' "$s" "$@"; }
fi
[ -n "${GITHUB_STEP_SUMMARY:-}" ] && printf '\n### %s: real displays\n\n| vout | filter | displayed | lost | exit | notes |\n|---|---|---|---|---|---|\n' "${LABEL:-}" >> "$GITHUB_STEP_SUMMARY"
for vout in $VOUTS; do
  for f in none "postproc --pushspike-mult=2" "postproc --pushspike-mult=4" "postproc --pushspike-mult=8 --pushspike-alloc=1"; do
    tag="$vout-$(echo "$f" | tr -dc 'a-z0-9=' | sed 's/pushspike//g' | cut -c1-40)"
    log="$OUT/$tag.log"; rcout="$OUT/$tag.rc"
    fargs=(); [ "$f" != none ] && fargs=(--video-filter=${f%% *} ${f#* })
    [ "$f" = none ] && fargs=()
    # shellcheck disable=SC2086
    { sleep $((DUR - 1)); echo stats; sleep 1; echo quit; } | timeout 60 "$VLC" -I rc --rc-fake-tty ${RCARGS:-} --no-audio --play-and-exit \
        --file-logging --logfile="$log" --log-verbose=2 --vout="$vout" ${HWARGS:-} "${fargs[@]}" "$CLIP" > "$rcout" 2>&1
    rc=$?
    disp=$(grep -i "displayed frames" "$rcout" | tail -1 | grep -o '[0-9]\+' | tail -1)
    lost=$(grep -i "lost frames" "$rcout" | tail -1 | grep -o '[0-9]\+' | tail -1)
    notes=$(grep -hoE "pushspike: active[^,]*|using vout display module \"[a-z0-9_]+\"|Failed to [a-z ]+|alloc_fail=[0-9]+" "$log" 2>/dev/null | sort -u | tr '\n' ';' | cut -c1-200)
    echo "== $tag: displayed=${disp:-?} lost=${lost:-?} exit=$rc :: $notes"
    [ -n "${GITHUB_STEP_SUMMARY:-}" ] && echo "| $vout | $f | ${disp:-?} | ${lost:-?} | $rc | $notes |" >> "$GITHUB_STEP_SUMMARY"
  done
done
exit 0
