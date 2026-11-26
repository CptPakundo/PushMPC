#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline quality check of the anime upscalers on the CPU (ONNX Runtime CPU EP), against bicubic.

For each clip and time: ffmpeg decodes one frame (RGB), a crop of it is the reference (HR); the HR is reduced by box
averaging (2x, or 4x for the 4x task) and upscaled again by bicubic and by every model. Scores on luma (BT.709):
  psnr       PSNR over the crop (8 px border excluded)
  edge_psnr  PSNR over the HR's line pixels only (Sobel magnitude in the top 10 %): line art
  sharp      mean Sobel magnitude on those pixels, output / HR (1.0 = as sharp as the reference; < 1 soft, > 1 halo)
4x task: AnimeSR v2 (4x) against bicubic 4x and the 2x models applied twice.
The recurrent AnimeSR runs on the three frames before the scored one first (its carry settles), causally.

Needs numpy, onnx, onnxruntime and ffmpeg on PATH; the graphs folder from make_graphs.py with the fetched weights in
<graphs>/anime-sr. ArtCNN's files are ONNX IR 11: on an ONNX Runtime older than 1.21 they are loaded with the IR
version field lowered in memory (the graph is unchanged).

Usage: python -I anime_sr_quality.py --graphs DIR --out results.csv --clip name=path@t1,t2 [--clip ...] [--crop WxH]
"""
from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
from pathlib import Path

import numpy as np
import onnx
import onnxruntime as ort

sys.path.insert(0, str(Path(__file__).resolve().parent))
import anime_sr_graphs  # noqa: E402

KR, KB = 0.2126, 0.0722


def decode(path, t, step=0):
    """RGB float [3,h,w] 0..1 of the frame at t (+ step frames)."""
    probe = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height,r_frame_rate",
                            "-of", "json", path], capture_output=True, check=True, text=True)
    s = json.loads(probe.stdout)["streams"][0]
    num, den = (int(x) for x in s["r_frame_rate"].split("/"))
    w, h = s["width"], s["height"]
    tt = t + step * den / num
    raw = subprocess.run(["ffmpeg", "-v", "error", "-ss", f"{tt:.4f}", "-i", path, "-frames:v", "1", "-vf", "scale=in_range=auto:out_range=pc:flags=accurate_rnd",
                          "-f", "rawvideo", "-pix_fmt", "rgb48le", "-"], capture_output=True, check=True).stdout
    return np.frombuffer(raw, "<u2").reshape(h, w, 3).transpose(2, 0, 1).astype(np.float32) / 65535.0


def luma(rgb):
    return KR * rgb[0] + (1 - KR - KB) * rgb[1] + KB * rgb[2]


def box(x, f):
    c, h, w = x.shape
    return x[:, : h // f * f, : w // f * f].reshape(c, h // f, f, w // f, f).mean(axis=(2, 4))


def bicubic(x, s, a=-0.5):
    def weights(n):
        src = (np.arange(n * s) + 0.5) / s - 0.5
        i0 = np.floor(src).astype(int)
        f = src - i0
        idx = np.stack([np.clip(i0 + k, 0, n - 1) for k in (-1, 0, 1, 2)])
        d = np.stack([1 + f, f, 1 - f, 2 - f])
        wgt = np.where(d <= 1, (a + 2) * d**3 - (a + 3) * d**2 + 1, np.where(d < 2, a * d**3 - 5 * a * d**2 + 8 * a * d - 4 * a, 0))
        return idx, wgt
    c, h, w = x.shape
    iy, wy = weights(h)
    ix, wx = weights(w)
    rows = sum(x[:, iy[k], :] * wy[k][None, :, None] for k in range(4))
    return sum(rows[:, :, ix[k]] * wx[k][None, None, :] for k in range(4)).astype(np.float32)


def sobel(y):
    p = np.pad(y, 1, mode="edge")
    gx = (p[:-2, 2:] + 2 * p[1:-1, 2:] + p[2:, 2:]) - (p[:-2, :-2] + 2 * p[1:-1, :-2] + p[2:, :-2])
    gy = (p[2:, :-2] + 2 * p[2:, 1:-1] + p[2:, 2:]) - (p[:-2, :-2] + 2 * p[:-2, 1:-1] + p[:-2, 2:])
    return np.hypot(gx, gy)


def scores(out_y, ref_y, b=8):
    o, r = np.clip(out_y, 0, 1)[b:-b, b:-b], ref_y[b:-b, b:-b]
    g = sobel(ref_y)[b:-b, b:-b]
    lines = g >= np.quantile(g, 0.9)
    psnr = lambda m: 10 * np.log10(1.0 / max(float(np.mean((o[m] - r[m]) ** 2)), 1e-12))  # noqa: E731
    go = sobel(np.clip(out_y, 0, 1))[b:-b, b:-b]
    return psnr(np.ones_like(lines)), psnr(lines), float(go[lines].mean() / max(g[lines].mean(), 1e-9))


class Models:
    def __init__(self, graphs: Path, pins: dict):
        self.dir = graphs / "anime-sr"
        self.pins = {m["id"]: m for m in pins["models"]}
        self.sessions = {}

    def session(self, mid):
        if mid not in self.sessions:
            m = self.pins[mid]
            path = self.dir / m["graph"]
            if m["family"] == "artcnn":
                model = onnx.load(str(path))
                if model.ir_version > 10 and tuple(int(v) for v in ort.__version__.split(".")[:2]) < (1, 21):
                    model.ir_version = 10
                self.sessions[mid] = ort.InferenceSession(model.SerializeToString(), providers=["CPUExecutionProvider"])
            else:
                self.sessions[mid] = ort.InferenceSession(str(path), providers=["CPUExecutionProvider"])
        return self.sessions[mid]

    def up2(self, mid, rgb):
        """2x of an RGB picture: luma models on Y (chroma bicubic), RGB models on RGB."""
        m, s = self.pins[mid], self.session(mid)
        if m["kind"] == "luma":
            y = luma(rgb)[None, None]
            y2 = s.run(None, {s.get_inputs()[0].name: y})[0][0, 0]
            rgb2 = bicubic(rgb, 2)
            return np.clip(rgb2 + (y2 - luma(rgb2))[None], 0, 1)
        return np.clip(s.run(None, {"x": rgb[None]})[0][0], 0, 1)

    def animesr(self, frames):
        """4x of the last of `frames` (RGB), the carry run over all of them (causal: next = cur)."""
        s = self.session("animesr-v2")
        _, h, w = frames[0].shape
        fb = np.zeros((1, 3, 4 * h, 4 * w), np.float32)
        st = np.zeros((1, 64, h, w), np.float32)
        prev = frames[0][None]
        for f in frames:
            cur = f[None]
            fb, st = s.run(None, {"prev": prev, "cur": cur, "next": cur, "fb": fb, "state": st})
            prev = cur
        return np.clip(fb[0], 0, 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--graphs", required=True)
    ap.add_argument("--pins", default=str(Path(__file__).resolve().parents[2] / "scripts" / "anime-models-pin.json"))
    ap.add_argument("--clip", action="append", required=True, help="name=path@t1,t2,...")
    ap.add_argument("--crop", default="384x216", help="HR crop at the frame centre (WxH, multiples of 4)")
    ap.add_argument("--models", default="artcnn-c4f16,artcnn-c4f16-ds,artcnn-c4f32,artcnn-c4f32-ds,artcnn-r8f64,cunny-fast,cunny-4x16,cunny-8x32")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    pins = json.loads(Path(a.pins).read_text(encoding="utf-8"))
    models = Models(Path(a.graphs), pins)
    cw, ch = (int(v) for v in a.crop.split("x"))
    rows = []
    for spec in a.clip:
        name, rest = spec.split("=", 1)
        path, times = rest.rsplit("@", 1)
        for t in (float(v) for v in times.split(",")):
            seq = [decode(path, t, k) for k in range(4)]   # the scored frame is seq[3]
            _, h, w = seq[3].shape
            y0, x0 = (h - ch) // 2 // 4 * 4, (w - cw) // 2 // 4 * 4
            crops = [f[:, y0: y0 + ch, x0: x0 + cw] for f in seq]
            hr = crops[3]
            ref = luma(hr)
            lr2 = box(hr, 2)
            res = {"bicubic": bicubic(lr2, 2)}
            for mid in a.models.split(","):
                res[mid] = models.up2(mid, lr2)
            for method, img in res.items():
                rows.append([name, t, "2x", method, *scores(luma(img), ref)])
            lr4 = [box(c, 4) for c in crops]
            res4 = {"bicubic": bicubic(lr4[3], 4), "animesr-v2": models.animesr(lr4),
                    "artcnn-c4f32 twice": models.up2("artcnn-c4f32", models.up2("artcnn-c4f32", lr4[3])),
                    "artcnn-r8f64 twice": models.up2("artcnn-r8f64", models.up2("artcnn-r8f64", lr4[3])),
                    "cunny-8x32 twice": models.up2("cunny-8x32", models.up2("cunny-8x32", lr4[3]))}
            for method, img in res4.items():
                rows.append([name, t, "4x", method, *scores(luma(img), ref)])
            print(f"{name} t={t}: done", flush=True)
    with open(a.out, "w", newline="") as f:
        wr = csv.writer(f)
        wr.writerow(["clip", "t", "task", "method", "psnr", "edge_psnr", "sharp"])
        wr.writerows(rows)
    # summary: mean gain over bicubic per task and method
    import collections
    acc = collections.defaultdict(list)
    base = {(r[0], r[1], r[2]): r for r in rows if r[3] == "bicubic"}
    for r in rows:
        b = base[(r[0], r[1], r[2])]
        acc[(r[2], r[3])].append((r[4] - b[4], r[5] - b[5], r[6]))
    for (task, method), v in sorted(acc.items()):
        v = np.array(v)
        print(f"{task} {method:20s} dPSNR {v[:, 0].mean():+.2f} dB  dEdgePSNR {v[:, 1].mean():+.2f} dB  sharp {v[:, 2].mean():.2f}  (n={len(v)})")


if __name__ == "__main__":
    main()
