#!/usr/bin/env python3
"""Verdict for one hardware-format case (run-hw.sh): VLC decoded through the null VA-API / VDPAU driver, PushMPC took
the path the case expects, and the displayed frames show the moving box where it belongs.

usage: hwcheck.py CASE CSV LOG --expect base|hw|pass --mult K [--src-frames N]
  base  no PushMPC: VLC's own hardware decoding and download (k = 1)
  hw    PushMPC's hardware path ("hw path ok"): K frames per source frame, each generated frame half-way (etc.)
  pass  the driver fails PushMPC's round trip: "hardware path unavailable", plain playback (k = 1)
The null drivers draw a 32 px box at x = 16 + 4 n for source picture n, so consecutive displayed frames should move by
4 / k px; a repeated frame (no interpolation) moves by 0, a skipped one by 2 * 4 / k. The pushdump display's cx column
is the box's x centroid. Exit 1 on FAIL; a Markdown row goes to $GITHUB_STEP_SUMMARY.
"""
import argparse, csv, json, os, re, statistics

ap = argparse.ArgumentParser()
ap.add_argument("case"); ap.add_argument("csv"); ap.add_argument("log")
ap.add_argument("--expect", required=True, choices=["base", "hw", "pass"])
ap.add_argument("--mult", type=int, default=1)
ap.add_argument("--src-frames", type=int, default=0)
a = ap.parse_args()

rows = []
try:
    with open(a.csv, newline="") as f:
        rows = list(csv.DictReader(f))
except FileNotFoundError:
    pass
try:
    with open(a.log, errors="replace") as f:
        log = f.read()
except FileNotFoundError:
    log = ""

k = a.mult if a.expect == "hw" else 1
cx = [float(r.get("cx", "-1") or -1) for r in rows]
n = len(rows)
skip = 2 * k + 2
mid = cx[skip:-skip] if n > 2 * skip + 4 else []
steps = [b - x for x, b in zip(mid, mid[1:]) if x >= 0 and b >= 0]
step = 4.0 / k
# the transport is right when every shown frame lies between its neighbours (a lost upload shows a stale surface: a
# jump back) and most frames are new positions (without interpolation only 1 in k would be); how evenly the engine
# spaces them on this synthetic box is the engine's business, reported, not judged
good = sum(1 for d in steps if 0.5 * step <= d <= 1.5 * step)
repeats = sum(1 for d in steps if abs(d) < 0.05)
back = sum(1 for d in steps if d < -0.5)
moving = (len(steps) - repeats) / len(steps) if steps else 0.0
mean_step = sum(steps) / len(steps) if steps else 0.0
want = a.src_frames * k if a.src_frames else 0

problems = []
hwdec = re.search(r"Using (.*) for hardware decoding", log)
if not hwdec or "null" not in hwdec.group(1):
    problems.append("VLC did not decode through the null driver")
if a.expect == "hw":
    if "PushMPC: hw path ok" not in log: problems.append("no 'hw path ok'")
    if "hardware path unavailable" in log: problems.append("PushMPC fell back to pass-through")
elif a.expect == "pass":
    if "hardware path unavailable" not in log: problems.append("PushMPC did not report the failed round trip")
    if "PushMPC: hw path ok" in log: problems.append("PushMPC used a driver that loses uploads")
if want and n < 0.9 * want: problems.append(f"{n} frames shown, {want} expected")
if len(steps) < 20: problems.append(f"too few measurable frames ({len(steps)})")
elif back > 3: problems.append(f"{back} jumps back (stale or misordered frames)")
elif moving < (0.7 if k > 1 else 0.9): problems.append(f"only {len(steps) - repeats}/{len(steps)} frames show a new box position")
elif abs(mean_step - step) > 0.25 * step: problems.append(f"mean step {mean_step:.2f} px, {step:.2f} expected")
m = re.search(r"PushMPC: hw path ok, x(\d+) \((.*)\)\s*$", log, re.M)
how = m.group(2) if m else ""
gen = re.findall(r"PushMPC: close, in=\d+ out=\d+ generated=(\d+)", log)
hwline = re.findall(r"PushMPC: hardware path [^\n]*", log)
res = dict(case=a.case, expect=a.expect, k=k, frames=n, expected=want, steps=len(steps), good=good, repeats=repeats, back=back, mean_step=round(mean_step, 3),
           median_step=round(statistics.median(steps), 3) if steps else None, decoder=hwdec.group(1) if hwdec else "",
           path=how, generated=int(gen[-1]) if gen else 0, closing=hwline[-1] if hwline else "", problems=problems)
verdict = "FAIL" if problems else "PASS"
print(verdict, json.dumps(res))
with open(os.path.splitext(a.csv)[0] + ".hw.json", "w") as f:
    json.dump(res, f, indent=1)
if os.environ.get("GITHUB_STEP_SUMMARY"):
    with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as f:
        f.write(f"| {a.case} | {verdict} | {a.expect} x{k} | {n} / {want} | {good}/{len(steps)} ({repeats} repeats) | "
                f"{res['median_step']} / {step:.2f} | {res['generated']} | {how or '; '.join(problems) or '-'} |\n")
raise SystemExit(1 if problems else 0)
