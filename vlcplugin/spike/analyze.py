#!/usr/bin/env python3
"""Summarise a pushdump CSV (one line per displayed picture) from a VLC spike run.

usage: analyze.py CASE CSV LOG [--src-frames N] [--src-fps F] [--mult M] [--expect-size WxH]
Prints a one-line verdict, appends a Markdown row to $GITHUB_STEP_SUMMARY when set, and writes CASE.json.
Exit code is always 0 unless --strict is given (the spike collects evidence; the workflow decides).
"""
import csv, json, os, statistics, sys, argparse

ap = argparse.ArgumentParser()
ap.add_argument("case"); ap.add_argument("csv"); ap.add_argument("log")
ap.add_argument("--src-frames", type=int, default=0)
ap.add_argument("--src-fps", type=float, default=24.0)
ap.add_argument("--mult", type=int, default=1)
ap.add_argument("--expect-size", default="")
ap.add_argument("--strict", action="store_true")
a = ap.parse_args()

rows = []
try:
    with open(a.csv, newline="") as f:
        for r in csv.DictReader(f):
            rows.append(r)
except FileNotFoundError:
    pass

log = ""
try:
    with open(a.log, errors="replace") as f:
        log = f.read()
except FileNotFoundError:
    pass

n = len(rows)
uniq = len({r["sum"] for r in rows})
sizes = sorted({f'{r["w"]}x{r["h"]}' for r in rows})
dates = [int(r["date"]) for r in rows]
shown = [int(r["shown"]) for r in rows]
deltas = [(b - x) / 1000.0 for x, b in zip(dates, dates[1:])]
# VLC itself shows the first picture once before playback starts (the baseline without any filter has the same step
# back), so the first 2 * mult + 2 pictures are start-up and not judged
skip = 2 * a.mult + 2
startup_steps_back = sum(1 for d in deltas[:skip] if d <= 0)
deltas = deltas[skip:]
nonmono = sum(1 for d in deltas if d <= 0)
exp = 1000.0 / (a.src_fps * a.mult)
on_grid = sum(1 for d in deltas if abs(d - exp) <= 0.25 * exp)
late = [(s - d) / 1000.0 for s, d in zip(shown, dates)]

def q(v, p):
    if not v: return float("nan")
    v = sorted(v); return v[min(len(v) - 1, int(p * (len(v) - 1) + 0.5))]

expected = a.src_frames * a.mult if a.src_frames else 0
ratio = (n / a.src_frames) if a.src_frames else float("nan")
markers = {
    "active": "pushspike: active" in log or (" -> x" in log and "PushMPC " in log),
    "static": "as static" in log,
    "interactive": "as interactive" in log,
    "dropping": log.count("dropping pictures"),
    "compensate": "compensate for format changes" in log,
    "too_late": log.count("too late to be displayed"),
    "d3d11_opaque": "D3D11 opaque picture" in log,
}
alloc_fail = 0
for line in log.splitlines():
    if "pushspike: close" in line and "alloc_fail=" in line:
        try: alloc_fail = int(line.rsplit("alloc_fail=", 1)[1].split()[0])
        except ValueError: pass
    if "PushMPC: close" in line and ", failed " in line:
        try: alloc_fail = int(line.split(", failed ", 1)[1].split(")")[0])
        except ValueError: pass

res = dict(case=a.case, frames=n, unique=uniq, src_frames=a.src_frames, mult=a.mult, expected=expected,
           ratio=round(ratio, 3) if ratio == ratio else None, sizes=sizes,
           delta_ms_median=round(q(deltas, .5), 3) if deltas else None, delta_ms_min=round(min(deltas), 3) if deltas else None,
           delta_ms_max=round(max(deltas), 3) if deltas else None, expected_delta_ms=round(exp, 3),
           on_grid_pct=round(100.0 * on_grid / len(deltas), 1) if deltas else None, non_monotonic=nonmono,
           startup_steps_back=startup_steps_back,
           late_ms_median=round(q(late, .5), 2) if late else None, late_ms_p95=round(q(late, .95), 2) if late else None,
           alloc_fail=alloc_fail, **markers)
ok = True
if expected:
    ok = n >= 0.9 * expected and nonmono == 0
if a.expect_size:
    ok = ok and sizes == [a.expect_size]
res["pass"] = ok
with open(a.case + ".json", "w") as f:
    json.dump(res, f, indent=1)
print(("PASS " if ok else "FAIL ") + json.dumps(res))
s = os.environ.get("GITHUB_STEP_SUMMARY")
if s:
    with open(s, "a") as f:
        f.write(f"| {a.case} | {'pass' if ok else '**fail**'} | {n} / {expected or '-'} | {uniq} | {', '.join(sizes)} | "
                f"{res['delta_ms_median']} (exp {res['expected_delta_ms']}) | {res['on_grid_pct']} | {nonmono} | "
                f"{res['late_ms_median']} / {res['late_ms_p95']} | {alloc_fail} | {markers['dropping']} | "
                f"{'static' if markers['static'] else ('interactive' if markers['interactive'] else '-')}"
                f"{' +conv' if markers['compensate'] else ''}{' d3d11' if markers['d3d11_opaque'] else ''} |\n")
sys.exit(0 if (ok or not a.strict) else 1)
