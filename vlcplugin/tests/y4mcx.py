#!/usr/bin/env python3
"""The moving box's x centroid in every frame of a YUV4MPEG2 file (VLC's yuv output), as the CSV that hwcheck.py reads
(columns n, cx), for players whose output cannot take PushMPC's CSV display (the snap). 8-bit 4:2:0 only.
usage: y4mcx.py IN.y4m OUT.csv"""
import re, sys

BRIGHT = re.compile(rb"[\x81-\xff]+")

src, dst = sys.argv[1], sys.argv[2]
with open(src, "rb") as f:
    data = f.read()
nl = data.index(b"\n")
hdr = data[:nl].split()
w = int([t for t in hdr if t.startswith(b"W")][0][1:])
h = int([t for t in hdr if t.startswith(b"H")][0][1:])
ysz, fsz = w * h, w * h * 3 // 2
pos, n, rows = nl + 1, 0, []
while pos < len(data):
    e = data.index(b"\n", pos)          # "FRAME[ params]\n"
    y = data[e + 1:e + 1 + ysz]
    if len(y) < ysz:
        break
    sx = cnt = 0
    for r in range(h):
        for m in BRIGHT.finditer(y, r * w, (r + 1) * w):   # runs of bright pixels: sum of x by the run's ends
            a, b = m.start() - r * w, m.end() - r * w
            sx += (a + b - 1) * (b - a) / 2
            cnt += b - a
    rows.append((n, sx / cnt if cnt else -1.0))
    n += 1
    pos = e + 1 + fsz
with open(dst, "w") as f:
    f.write("n,cx\n")
    for i, c in rows:
        f.write(f"{i},{c:.2f}\n")
print(f"{src}: {n} frames {w}x{h}")
