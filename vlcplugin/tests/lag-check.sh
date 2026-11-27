#!/usr/bin/env bash
# Checks run-module.sh's held-drawing repair cases in one results folder (exit 1 on a failure):
#   x2_lag, x4_lag  (PUSHMPC_VLC_DRAWN=1): every frame (the analyzer's pass), the repair set ("held-drawing repair on"),
#                   and the pairs advanced on the worker (the close line's "pairs on the worker N", N > 0)
#   x2_live         (a clip the content library measures as not drawn): every frame, and the repair never set; with
#                   content=1 (the library next to the module) also "content not drawn" in the log
# usage: lag-check.sh <results folder> [content=0|1]
set -u
dir="$1"; content="${2:-0}"
bad=0
fail() { echo "::error::$1"; bad=1; }
passed() { grep -q '"pass": true' "$dir/$1.json" 2>/dev/null; }
for c in x2_lag x4_lag; do
  [ -f "$dir/$c.log" ] || { fail "$c did not run"; continue; }
  grep -h "PushMPC: \(held-drawing repair\|close\)" "$dir/$c.log" | sed 's/^[^:]*: //' | head -4
  passed "$c" || fail "$c: fewer frames than expected in the repair's lag mode"
  grep -q "PushMPC: held-drawing repair on (" "$dir/$c.log" || fail "$c: the repair was not set"
  grep -qE "pairs on the worker [1-9]" "$dir/$c.log" || fail "$c: no pair ran on the worker"
done
if [ -f "$dir/x2_live.log" ]; then
  grep -h "PushMPC: \(content\|held-drawing repair\)" "$dir/x2_live.log" | sed 's/^[^:]*: //' | head -4
  passed x2_live || fail "x2_live: fewer frames than expected"
  grep -q "PushMPC: held-drawing repair on (" "$dir/x2_live.log" && fail "x2_live: the repair was set on a picture not measured as drawn"
  if [ "$content" = 1 ]; then
    grep -q "PushMPC: content not drawn" "$dir/x2_live.log" || fail "x2_live: the content library did not measure it as not drawn"
  fi
else
  fail "x2_live did not run"
fi
exit $bad
