#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# PushMPC capture mode, CI check (capture.yml): the bright square's horizontal position in every frame of a .y4m dump
# (pushmpc-capture --no-present --dump), then the steps between frames. Frame generation must make the motion even:
#   check_motion.py <with.y4m> [<without.y4m>] [--speed <px per second> --hz <display rate>]
# with.y4m: frame generation on (Smoother Auto); without.y4m: --smoother off. Passes when, on the frames where the
# square moves (wrap-arounds left out), the step's spread with frame generation is below 0.75 px and below half the spread
# without it, the mean step is within 25 % of speed / hz, and no step goes backwards by more than 0.5 px.
# Every result also prints what the steps are made of, for both dumps: stalls (a step under a quarter of the wanted
# one), jumps (over one and a half source frames' worth: a frame the source skipped), backward steps, the extremes and
# the most common step sizes; then which rule failed. A failure then says whether the source (the off dump shows it
# too) or frame generation made it. --fps: the clip's frame rate (24).
# --source-limited yes: the source itself delivered too few pictures (capture.yml's source gate failed, e.g. the macOS
# VM's screen capture at 13-15 a second of 24): the spread rules are printed as information only, and the run must
# still have its mean step within 10 % of speed / hz and no step backwards by more than 0.5 px.
# Python 3, stdlib only.
import collections
import statistics
import sys


def frames(path):
    with open(path, "rb") as f:
        head = f.readline().split()
        w = int(next(t for t in head if t.startswith(b"W"))[1:])
        h = int(next(t for t in head if t.startswith(b"H"))[1:])
        csize = ((w + 1) // 2) * ((h + 1) // 2)
        while True:
            line = f.readline()
            if not line:
                return
            y = f.read(w * h)
            f.read(2 * csize)
            if len(y) < w * h:
                return
            yield w, h, y


def centres(path):
    out = []
    for w, h, y in frames(path):
        sx = n = 0
        # luma-weighted (the edges of a generated frame are blends: sub-pixel positions)
        for row in range(0, h, 2):
            base = row * w
            for x in range(w):
                v = y[base + x] - 100
                if v > 0:
                    sx += x * v
                    n += v
        out.append(sx / n if n > 2000 else None)
    return out


def steps(c):
    s = []
    for a, b in zip(c, c[1:]):
        if a is None or b is None:
            continue
        d = b - a
        if abs(d) > 60:  # the square wrapped around
            continue
        s.append(d)
    return s


def describe(name, s, want, src_step):
    stall = sum(1 for d in s if abs(d) < 0.25 * want)
    jump_at = 1.5 * max(want, src_step)
    jumps = sum(1 for d in s if d > jump_at)
    back = sum(1 for d in s if d < -0.5)
    common = collections.Counter(round(d * 2) / 2 for d in s).most_common(6)
    print(f"  {name}: {stall} stalls (under {0.25 * want:.2f} px), {jumps} jumps (over {jump_at:.2f} px), {back} backward "
          f"(under -0.5 px); largest {max(s):.3f}, lowest {min(s):.3f}; most common (px: count) "
          + ", ".join(f"{k:g}: {n}" for k, n in common))


def main(argv):
    args, opt, i = [], {}, 1
    while i < len(argv):
        if argv[i].startswith("--") and i + 1 < len(argv):
            opt[argv[i]] = argv[i + 1]
            i += 2
        else:
            args.append(argv[i])
            i += 1
    speed = float(opt.get("--speed", "96"))
    hz = float(opt.get("--hz", "60"))
    fps = float(opt.get("--fps", "24"))
    want = speed / hz
    src_step = speed / fps
    on = steps(centres(args[0]))
    if len(on) < 30:
        print(f"FAIL: only {len(on)} usable steps in {args[0]}")
        return 1
    on = on[10:]  # the timeline settles first
    sd_on = statistics.pstdev(on)
    mean_on = statistics.mean(on)
    back = min(on)
    print(f"with frame generation: {len(on)} steps, mean {mean_on:.3f} px (want {want:.3f}), spread {sd_on:.3f} px, lowest {back:.3f}")
    describe("with", on, want, src_step)
    limited = opt.get("--source-limited", "no") == "yes"
    failed, info = [], []
    spread_rules = info if limited else failed
    if not sd_on < 0.75:
        spread_rules.append(f"spread {sd_on:.3f} not under 0.75 px")
    tol = 0.10 if limited else 0.25
    if not abs(mean_on - want) <= tol * want:
        failed.append(f"mean {mean_on:.3f} more than {tol * 100:.0f} % from {want:.3f}")
    if not back > -0.5:
        failed.append(f"a step goes back {back:.3f} px")
    if len(args) > 1:
        off = steps(centres(args[1]))[10:]
        sd_off = statistics.pstdev(off) if off else 0
        print(f"without: {len(off)} steps, spread {sd_off:.3f} px")
        if off:
            describe("without", off, want, src_step)
        if not sd_on < 0.5 * sd_off:
            spread_rules.append(f"spread {sd_on:.3f} not under half the spread without ({0.5 * sd_off:.3f})")
    if limited:
        print("  the source delivered too few pictures: the spread rules are information only (mean within 10 %, no step back)")
    for f in info:
        print(f"  not met (information): {f}")
    for f in failed:
        print(f"  failed: {f}")
    print("FAIL" if failed else "PASS")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
