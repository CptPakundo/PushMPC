#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""PSNR (RGB, 8-bit) of raw RGBA frames against a reference frame in the same folder.

  python -I compare_rgba.py DIR REFERENCE NAME [NAME ...] [--min-db 35]   (files DIR/<name>.rgba)
"""
import argparse
import math
import sys
from pathlib import Path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("reference")
    ap.add_argument("names", nargs="+")
    ap.add_argument("--min-db", type=float, default=35.0)
    a = ap.parse_args()
    d = Path(a.dir)
    ref = (d / f"{a.reference}.rgba").read_bytes()
    worst = 99.0
    for n in a.names:
        x = (d / f"{n}.rgba").read_bytes()
        if len(x) != len(ref):
            print(f"{n}: size {len(x)} differs from {len(ref)}")
            return 1
        se, count = 0, 0
        for i in range(0, len(ref), 4):
            for c in range(3):
                e = x[i + c] - ref[i + c]
                se += e * e
            count += 3
        mse = se / count
        db = 99.0 if mse < 1e-9 else 10 * math.log10(255 * 255 / mse)
        print(f"{n} against {a.reference}: {db:.2f} dB")
        worst = min(worst, db)
    return 0 if worst >= a.min_db else 1


if __name__ == "__main__":
    sys.exit(main())
