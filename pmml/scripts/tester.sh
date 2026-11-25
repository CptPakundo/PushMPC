#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# pmml tester script: run by a volunteer (or the maintainer) on a real GPU, never by CI. It only reads what is given and
# writes a report folder; nothing is installed. Needs a pmml runtime folder (fetch_runtime.py output plus models/ and
# graphs/) and pmml_cli.
#   sh tester.sh <pmml_cli> <runtime dir> [clip.y4m] [report dir]
# What it measures: every device and EP, Auto's probes at 1920x1080 and 3840x2160 (each in its own process), the
# choice for 23.976 fps at x5 and x2, and (with a clip) quality per t / flow once against the held-out frames.
set -eu
cli=$1; rt=$2; clip=${3:-}; out=${4:-pmml-report}
mkdir -p "$out"
{ uname -a; command -v nvidia-smi >/dev/null && nvidia-smi --query-gpu=name,driver_version,utilization.gpu,memory.used,clocks.gr,power.draw --format=csv; command -v vulkaninfo >/dev/null && vulkaninfo --summary 2>/dev/null | head -40; } > "$out/system.txt" 2>&1 || true
for fl in core directml migraphx; do [ -d "$rt/ort/$fl" ] && "$cli" list --runtime "$rt" --flavor "$fl" > "$out/devices-$fl.txt" 2>&1 || true; done
for size in 1920x1080 3840x2160; do
  for mult in 5 2; do
    "$cli" auto --runtime "$rt" --size "$size" --fps 23.976 --mult "$mult" --pairs 8 --cache "$out" --refresh > "$out/auto-$size-x$mult.txt" 2>&1 || true
  done
done
if [ -n "$clip" ]; then
  for b in $(grep -ho '^ort:[^ ]*\|^ncnn:[^ ]*' "$out"/devices-*.txt | sort -u); do
    for tier in fast balanced quality; do
      for once in "" --flow-once; do
        "$cli" run --runtime "$rt" --backend "$b" --tier "$tier" --clip "$clip" --frames 17 --mult 4 $once >> "$out/quality.txt" 2>&1 || true
      done
    done
  done
fi
echo "report in $out (send the whole folder)"
