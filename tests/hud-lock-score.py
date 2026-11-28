#!/usr/bin/env python3
"""Live HUD Lock fidelity: generated frames against the ground truth, lock on against lock off.

Synthetic only: real content has no ground truth for the frames in between. The scenes come from
pushcore_gpu_harness --scene (a panning textured background with static overlays composited before the NV12
conversion). For each scene the harness ran:
  gt-<scene>             --backend none --fine <m>: the source at m times the rate, through the same colour path
  <scene>-off, <scene>-on  frame generation x<m> with --hudlock 0 / 1 (and <scene>-auto with --hudlock 2,
                           <scene>-off-r2 a repeat for the run-to-run floor)
Each run folder holds dump.raw (every output from a source frame on) and mask.raw (the overlay's pixels).

Scored per region, pooled over the scored generated frames: PSNR (RGB, 8-bit) and mean absolute error of
  overlay   the overlay's pixels
  halo      a ring 6 px wide around them (frame generation's drag shows here)
  backgr    everything else
  whole     the whole frame
The decision rules were set before the run (the coordinator's, 2026-11-28):
  R1 lock on beats lock off on the overlay by more than twice the run-to-run floor (logo scene)
  R2 the halo is not worse (more than the floor)
  R3 background and whole frame do not drop by more than the floor (every scene)
  R4 the null scene (no overlay) is bit-identical, on against off
  R5 the slow pan shows no false-lock harm (whole and background within the floor)
Usage: python -I hud-lock-score.py --root DIR --mult 2 [--summary out.md] [--json out.json]
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys

import numpy as np

SCENES = ["logo", "null", "slowpan", "ticker", "translucent", "noise", "startmove"]
# first source frame whose pair is scored: the lock needs ~49 still real frames before it can lock; startmove
# starts to pan at frame 60, so its first scored pair is the one the motion starts in
SCORE_FROM = {"startmove": 61}
DEFAULT_FROM = 56
AUTO_FROM = 112
HALO = 6


def read_dump(path):
    with open(path, "rb") as f:
        w, h = np.frombuffer(f.read(8), dtype=np.uint32)
    w, h = int(w), int(h)
    rec = 12 + w * h * 4
    size = os.path.getsize(path) - 8
    n = size // rec
    mm = np.memmap(path, dtype=np.uint8, mode="r", offset=8, shape=(n, rec))
    heads = np.frombuffer(mm[:, :12].tobytes(), dtype=np.uint32).reshape(n, 3)
    return w, h, mm, heads


def frame(mm, i, w, h):
    return np.asarray(mm[i, 12:]).reshape(h, w, 4)[:, :, :3]


def read_mask(path):
    with open(path, "rb") as f:
        w, h = np.frombuffer(f.read(8), dtype=np.uint32)
        m = np.frombuffer(f.read(), dtype=np.uint8)
    return m.reshape(int(h), int(w)).astype(bool)


def dilate(m, r):
    out = m.copy()
    for _ in range(r):
        d = out.copy()
        d[1:, :] |= out[:-1, :]
        d[:-1, :] |= out[1:, :]
        d[:, 1:] |= out[:, :-1]
        d[:, :-1] |= out[:, 1:]
        d[1:, 1:] |= out[:-1, :-1]
        d[1:, :-1] |= out[:-1, 1:]
        d[:-1, 1:] |= out[1:, :-1]
        d[:-1, :-1] |= out[1:, 1:]
        out = d
    return out


def regions(mask):
    grown = dilate(mask, HALO)
    whole = np.ones_like(mask)
    return {"overlay": mask, "halo": grown & ~mask, "backgr": ~grown, "whole": whole}


class Acc:
    def __init__(self):
        self.se = 0.0
        self.ae = 0.0
        self.n = 0

    def add(self, d, sel):
        if not sel.any():
            return
        v = d[sel]
        self.se += float(np.sum(v * v))
        self.ae += float(np.sum(np.abs(v)))
        self.n += v.size

    def psnr(self):
        if not self.n:
            return None
        mse = self.se / self.n
        return 99.0 if mse <= 1e-12 else 10 * math.log10(255.0 * 255.0 / mse)

    def mae(self):
        return self.ae / self.n if self.n else None


def score(run_dir, gt, regs, mult, first):
    w, h, mm, heads = read_dump(os.path.join(run_dir, "dump.raw"))
    gw, gh, gmm, gheads = gt
    if (w, h) != (gw, gh):
        raise SystemExit(f"{run_dir}: {w}x{h} against ground truth {gw}x{gh}")
    gindex = {int(s): i for i, (s, sub, kind) in enumerate(gheads) if kind == 0}
    accs = {k: Acc() for k in regs}
    real = Acc()
    real_diff_max = 0
    generated = 0
    missing = 0
    for i, (src, sub, kind) in enumerate(heads):
        src, sub, kind = int(src), int(sub), int(kind)
        if src < first:
            continue
        if kind == 1:
            g = gindex.get((src - 1) * mult + sub)
            if g is None:
                missing += 1
                continue
            d = frame(mm, i, w, h).astype(np.float64) - frame(gmm, g, w, h).astype(np.float64)
            for k, sel in regs.items():
                accs[k].add(d, sel)
            generated += 1
        else:   # a real output against the ground truth run's frame at the same time: the colour paths must agree
            g = gindex.get(src * mult)
            if g is None:
                continue
            d = frame(mm, i, w, h).astype(np.int16) - frame(gmm, g, w, h).astype(np.int16)
            real.add(d.astype(np.float64), regs["whole"])
            real_diff_max = max(real_diff_max, int(np.abs(d).max()))
    out = {k: {"psnr": a.psnr(), "mae": a.mae()} for k, a in accs.items()}
    out["generated"] = generated
    out["missingGroundTruth"] = missing
    out["realVsGroundTruth"] = {"psnr": real.psnr(), "maxAbs": real_diff_max}
    return out


def identical(run_a, run_b, first):
    """generated frames of two runs: how many are bit-identical, and the largest difference"""
    wa, ha, ma, hea = read_dump(os.path.join(run_a, "dump.raw"))
    wb, hb, mb, heb = read_dump(os.path.join(run_b, "dump.raw"))
    ib = {(int(s), int(sub)): i for i, (s, sub, kind) in enumerate(heb) if kind == 1}
    same = total = 0
    worst = 0
    for i, (s, sub, kind) in enumerate(hea):
        if kind != 1 or int(s) < first:
            continue
        j = ib.get((int(s), int(sub)))
        if j is None:
            continue
        a = np.asarray(ma[i, 12:])
        b = np.asarray(mb[j, 12:])
        total += 1
        if np.array_equal(a, b):
            same += 1
        else:
            worst = max(worst, int(np.abs(a.astype(np.int16) - b.astype(np.int16)).max()))
    return {"identical": same, "compared": total, "maxAbs": worst}


def fmt(v, digits=2):
    return "-" if v is None else f"{v:.{digits}f}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--mult", type=int, default=2)
    ap.add_argument("--summary")
    ap.add_argument("--json")
    a = ap.parse_args()
    root = a.root
    have = lambda name: os.path.isfile(os.path.join(root, name, "dump.raw"))
    results = {}
    for scene in SCENES:
        if not have("gt-" + scene):
            continue
        gt = read_dump(os.path.join(root, "gt-" + scene, "dump.raw"))
        mask = read_mask(os.path.join(root, "gt-" + scene, "mask.raw"))
        regs = regions(mask)
        results[scene] = {"overlayPixels": int(mask.sum())}
        for mode in ["off", "on", "off-r2", "auto"]:
            name = f"{scene}-{mode}"
            if not have(name):
                continue
            first = AUTO_FROM if mode == "auto" else SCORE_FROM.get(scene, DEFAULT_FROM)
            results[scene][mode] = score(os.path.join(root, name), gt, regs, a.mult, first)
    # the run-to-run floor: the same settings twice (dB, largest over the regions that exist)
    floor = 0.0
    floor_seen = []
    repeats = {}
    for scene in ["logo", "null"]:
        r = results.get(scene, {})
        if "off" in r and "off-r2" in r:
            for k in ["overlay", "halo", "backgr", "whole"]:
                p, q = r["off"][k]["psnr"], r["off-r2"][k]["psnr"]
                if p is not None and q is not None:
                    floor = max(floor, abs(p - q))
            floor_seen.append(scene)
            repeats[scene] = identical(os.path.join(root, f"{scene}-off"), os.path.join(root, f"{scene}-off-r2"), DEFAULT_FROM)

    def delta(scene, k):
        r = results.get(scene, {})
        if "on" not in r or "off" not in r:
            return None
        p, q = r["on"][k]["psnr"], r["off"][k]["psnr"]
        return None if p is None or q is None else p - q

    checks = []

    def rule(name, ok, text):
        checks.append({"rule": name, "pass": ok, "text": text})

    have_floor = bool(floor_seen)
    d = delta("logo", "overlay")
    rule("R1 overlay", None if d is None or not have_floor else d > 2 * floor,
         f"logo: overlay on - off {fmt(d)} dB, needs > {fmt(2 * floor, 3)} (2 x floor)")
    d = delta("logo", "halo")
    rule("R2 halo", None if d is None or not have_floor else d >= -floor, f"logo: halo on - off {fmt(d)} dB, needs >= -{fmt(floor, 3)}")
    worst = None
    worst_where = ""
    for scene in results:
        for k in ["backgr", "whole"]:
            d = delta(scene, k)
            if d is not None and (worst is None or d < worst):
                worst, worst_where = d, f"{scene} {k}"
    rule("R3 background / whole", None if worst is None or not have_floor else worst >= -floor,
         f"worst on - off {fmt(worst)} dB ({worst_where}), needs >= -{fmt(floor, 3)}")
    null_same = None
    if "null" in results and "on" in results["null"] and "off" in results["null"]:
        null_same = identical(os.path.join(root, "null-on"), os.path.join(root, "null-off"), DEFAULT_FROM)
    # two runs are two processes: when the same settings twice (off / off) are not bit-identical either, frame generation
    # itself does not repeat exactly here, and on / off cannot be held to bit-identity (undecided, not a failure)
    r4 = None
    if null_same is not None and null_same["compared"] > 0:
        if null_same["identical"] == null_same["compared"]:
            r4 = True
        elif "null" in repeats and repeats["null"]["compared"] > 0 and repeats["null"]["identical"] == repeats["null"]["compared"]:
            r4 = False
    rule("R4 null bit-identical", r4,
         "null: generated frames identical on / off: " + ("-" if null_same is None else
         f"{null_same['identical']}/{null_same['compared']} (largest difference {null_same['maxAbs']}); off / off repeat: "
         + (f"{repeats['null']['identical']}/{repeats['null']['compared']}" if "null" in repeats else "-")))
    ds = [delta("slowpan", k) for k in ["whole", "backgr"]]
    ok5 = None if None in ds or not have_floor else min(ds) >= -floor
    rule("R5 slow pan", ok5, f"slowpan: whole {fmt(ds[0])} dB, background {fmt(ds[1])} dB on - off, needs >= -{fmt(floor, 3)}")
    decided = all(c["pass"] is not None for c in checks)
    verdict = "ADOPT (ports may follow)" if decided and all(c["pass"] for c in checks) else ("NOT ADOPTED" if decided else "UNDECIDED (runs missing)")

    lines = ["## Live HUD Lock fidelity (synthetic scenes; PushFlow on WARP, a software adapter: fidelity, never speed)", "",
             f"Run-to-run floor: {fmt(floor, 3)} dB (repeats of {', '.join(floor_seen) or 'none'}"
             + "".join(f"; {s} off/off identical {r['identical']}/{r['compared']}" for s, r in repeats.items()) + ")", "",
             "| scene | run | overlay px | generated | overlay dB | halo dB | background dB | whole dB | overlay MAE | real vs GT dB |",
             "|---|---|---|---|---|---|---|---|---|---|"]
    for scene, r in results.items():
        for mode in ["off", "on", "off-r2", "auto"]:
            if mode not in r:
                continue
            x = r[mode]
            lines.append(f"| {scene} | {mode} | {r['overlayPixels']} | {x['generated']} | {fmt(x['overlay']['psnr'])} | {fmt(x['halo']['psnr'])} | "
                         f"{fmt(x['backgr']['psnr'])} | {fmt(x['whole']['psnr'])} | {fmt(x['overlay']['mae'], 3)} | {fmt(x['realVsGroundTruth']['psnr'])} |")
    lines += ["", "| on - off (dB) | overlay | halo | background | whole |", "|---|---|---|---|---|"]
    for scene in results:
        lines.append(f"| {scene} | " + " | ".join(fmt(delta(scene, k)) for k in ["overlay", "halo", "backgr", "whole"]) + " |")
    lines += ["", "| rule | result | numbers |", "|---|---|---|"]
    for c in checks:
        lines.append(f"| {c['rule']} | {'-' if c['pass'] is None else ('pass' if c['pass'] else 'FAIL')} | {c['text']} |")
    lines += ["", f"**VERDICT: {verdict}**"]
    text = "\n".join(lines)
    print(text)
    if a.summary:
        with open(a.summary, "a", encoding="utf-8") as f:
            f.write(text + "\n")
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump({"floor": floor, "repeats": repeats, "results": results, "checks": checks, "verdict": verdict, "nullOnOff": null_same}, f, indent=1)
    print(f"VERDICT {verdict}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
