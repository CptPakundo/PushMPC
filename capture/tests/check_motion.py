#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# PushMPC capture mode, CI check (capture.yml): the bright square's horizontal position in every frame of a .y4m dump
# (pushmpc-capture --no-present --dump), then the steps between frames. Frame generation must make the motion even:
#   check_motion.py <with.y4m> [<without.y4m>] [--speed <px per second> --hz <display rate>]
# with.y4m: frame generation on (Smoother Auto); without.y4m: --smoother off. Passes when, on the frames where the
# square moves (wrap-arounds left out), the step's spread with frame generation is below 0.75 px and below half the spread
# without it, the mean step is within 25 % of speed / hz, and no step goes backwards by more than 0.5 px.
# Python 3, stdlib only.
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
    want = speed / hz
    on = steps(centres(args[0]))
    if len(on) < 30:
        print(f"FAIL: only {len(on)} usable steps in {args[0]}")
        return 1
    on = on[10:]  # the timeline settles first
    sd_on = statistics.pstdev(on)
    mean_on = statistics.mean(on)
    back = min(on)
    print(f"with frame generation: {len(on)} steps, mean {mean_on:.3f} px (want {want:.3f}), spread {sd_on:.3f} px, lowest {back:.3f}")
    ok = sd_on < 0.75 and abs(mean_on - want) <= 0.25 * want and back > -0.5
    if len(args) > 1:
        off = steps(centres(args[1]))[10:]
        sd_off = statistics.pstdev(off) if off else 0
        print(f"without: {len(off)} steps, spread {sd_off:.3f} px")
        ok = ok and sd_on < 0.5 * sd_off
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
