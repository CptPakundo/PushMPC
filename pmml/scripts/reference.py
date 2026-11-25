#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""pmml's reference and quality check (CPU, numpy + ONNX Runtime's CPU EP).

  1. graph check: the generated pack/synth graphs against an independent numpy implementation, and the synth graph at
     the network's own size against the PushFlow model's own picture (the rebuild must reproduce it: > 45 dB).
  2. quality on a public clip (Y4M 4:2:0, 50 fps): held-out frames.
       2x: frame 2k+1 from 2k and 2k+2;  4x: frames 4k+1..4k+3 from 4k and 4k+4,
     per timestep (the network runs for every t) against flow once (the network runs once at t = 0.5, the other
     timesteps reuse its grids, scaled), plus a blend and a repeat as floors. PSNR-Y in dB.
  3. --dump DIR: writes the reference output planes (raw uint16) that pmml_cli's outputs are compared with.

  python -I reference.py --models DIR --graphs DIR --clip clip.y4m [--frames 9] [--tier fast] [--dump DIR] [--json out.json]
"""
from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

import numpy as np
import onnxruntime as ort

BT709_LIMITED_8 = [0.2126, 0.0722, 16 / 255, 219 / 255, 128 / 255, 224 / 255, 255.0, 0.0]
TIERS = {"fast": ("fast", 540, 0.5), "balanced": ("fast", 720, 1.0), "quality": ("quality", 1080, 1.0)}


def read_y4m(path: Path, limit: int):
    data = path.read_bytes()
    end = data.index(b"\n")
    header = data[:end].decode("ascii").split()
    w = int(next(t[1:] for t in header if t.startswith("W")))
    h = int(next(t[1:] for t in header if t.startswith("H")))
    pos, frames = end + 1, []
    fsize = w * h * 3 // 2
    while pos < len(data) and len(frames) < limit:
        nl = data.index(b"\n", pos)
        pos = nl + 1
        if pos + fsize > len(data):
            break
        buf = np.frombuffer(data, np.uint8, fsize, pos)
        y = buf[:w * h].reshape(h, w)
        u = buf[w * h:w * h * 5 // 4].reshape(h // 2, w // 2)
        v = buf[w * h * 5 // 4:].reshape(h // 2, w // 2)
        frames.append((y.astype(np.uint16), u.astype(np.uint16), v.astype(np.uint16)))
        pos += fsize
    return w, h, frames


def resize_linear(img, oh, ow):
    """ONNX Resize, mode linear, half_pixel, no antialias; img [C, H, W]."""
    c, h, w = img.shape
    ys = np.clip((np.arange(oh) + 0.5) * h / oh - 0.5, 0, h - 1)
    xs = np.clip((np.arange(ow) + 0.5) * w / ow - 0.5, 0, w - 1)
    y0 = np.floor(ys).astype(int); x0 = np.floor(xs).astype(int)
    y1 = np.minimum(y0 + 1, h - 1); x1 = np.minimum(x0 + 1, w - 1)
    fy = (ys - y0)[None, :, None]; fx = (xs - x0)[None, None, :]
    a = img[:, y0][:, :, x0] * (1 - fx) + img[:, y0][:, :, x1] * fx
    b = img[:, y1][:, :, x0] * (1 - fx) + img[:, y1][:, :, x1] * fx
    return (a * (1 - fy) + b * fy).astype(np.float32)


def to_rgb(planes, colour):
    kr, kb, yo, ys, co, cs, mx = colour[:7]
    y, u, v = (p.astype(np.float32) / mx for p in planes)
    h, w = y.shape
    uv = resize_linear(np.stack([u, v]), h, w)
    yl = (y - yo) / ys
    cb, cr = (uv - co) / cs
    r = yl + cr * 2 * (1 - kr)
    b = yl + cb * 2 * (1 - kb)
    g = (yl - kr * r - kb * b) / (1 - kr - kb)
    return np.stack([r, g, b]).astype(np.float32)


def from_rgb(rgb, colour):
    kr, kb, yo, ys, co, cs, mx = colour[:7]
    r, g, b = rgb
    kg = 1 - kr - kb
    yl = kr * r + kg * g + kb * b
    cb = (b - yl) / (2 * (1 - kb))
    cr = (r - yl) / (2 * (1 - kr))
    h, w = yl.shape
    pool = lambda x: x.reshape(h // 2, 2, w // 2, 2).mean(axis=(1, 3))  # noqa: E731
    code = lambda x: np.round(np.clip(x, 0, 1) * mx).astype(np.uint16)  # noqa: E731
    return code(yl * ys + yo), code(pool(cb) * cs + co), code(pool(cr) * cs + co)


def network_size(w, h, tier, pad):
    _, cap, half = TIERS[tier]
    scale = min(half, cap / h, 1.0)
    nh = max(32, int(round(h * scale / 2)) * 2)
    nw = max(32, int(round(w * scale / 2)) * 2)
    ph = (nh + pad - 1) // pad * pad
    pw = (nw + pad - 1) // pad * pad
    return nw, nh, pw, ph


def affine(w, h, nw, nh, pw, ph):
    """network-normalised (align_corners over the padded size) -> full-size-normalised grid coordinate."""
    def axis(full, n, p):
        # xm = (g + 1) / 2 * (p - 1);  xs = (xm + 0.5) * full / n - 0.5;  gf = 2 * xs / (full - 1) - 1
        a = (p - 1) / 2 * full / n * 2 / (full - 1)
        b = (((p - 1) / 2 + 0.5) * full / n - 0.5) * 2 / (full - 1) - 1
        return a, b
    ax, bx = axis(w, nw, pw)
    ay, by = axis(h, nh, ph)
    return [ax, bx, ay, by]


def bilinear(img, gx, gy):
    """GridSample bilinear, align_corners = 1, border; img [C,H,W], g normalised [-1, 1] arrays [h, w]."""
    c, h, w = img.shape
    x = np.clip((gx + 1) * 0.5 * (w - 1), 0, w - 1)
    y = np.clip((gy + 1) * 0.5 * (h - 1), 0, h - 1)
    x0 = np.floor(x).astype(int); y0 = np.floor(y).astype(int)
    x1 = np.minimum(x0 + 1, w - 1); y1 = np.minimum(y0 + 1, h - 1)
    fx = x - x0; fy = y - y0
    a = img[:, y0, x0] * (1 - fx) + img[:, y0, x1] * fx
    b = img[:, y1, x0] * (1 - fx) + img[:, y1, x1] * fx
    return (a * (1 - fy) + b * fy).astype(np.float32)


def synth_numpy(fa, fb, g0, g1, mask, s, aff, nw, nh):
    _, h, w = fa.shape
    ph, pw = g0.shape[1:3]
    ix = 2 * np.arange(pw) / max(pw - 1, 1) - 1
    iy = 2 * np.arange(ph) / max(ph - 1, 1) - 1
    ident = np.stack(np.meshgrid(ix, iy), axis=-1)[None].astype(np.float32)
    out = []
    for k, (grid, frame) in enumerate(((g0, fa), (g1, fb))):
        gs = ident + (grid - ident) * s[k]
        gs = gs * np.array([aff[0], aff[2]], np.float32) + np.array([aff[1], aff[3]], np.float32)
        t = np.moveaxis(gs[0], -1, 0)[:, :nh, :nw]
        t = resize_linear(t, h, w)
        out.append(bilinear(frame, t[0], t[1]))
    m = resize_linear(mask[0][:, :nh, :nw], h, w)[0]
    return out[1] + (out[0] - out[1]) * m


def psnr(a, b, peak=255.0):
    mse = float(np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2))
    return 99.0 if mse < 1e-10 else 10 * math.log10(peak * peak / mse)


class Pipeline:
    def __init__(self, models: Path, graphs: Path, tier: str, w: int, h: int, colour, providers=("CPUExecutionProvider",)):
        role = TIERS[tier][0]
        facts = dict(line.split("=", 1) for line in (models / f"pushflow-{role}.onnx.txt").read_text().split() if "=" in line)
        self.pad = int(facts.get("pad", 64))
        self.mask_for_first = facts.get("mask_for_first", "1") == "1"
        self.w, self.h, self.colour = w, h, np.array(colour, np.float32)
        self.nw, self.nh, self.pw, self.ph = network_size(w, h, tier, self.pad)
        self.aff = affine(w, h, self.nw, self.nh, self.pw, self.ph)
        so = ort.SessionOptions()
        so.log_severity_level = 3
        self.net = ort.InferenceSession(str(models / f"pushflow-{role}.onnx"), so, providers=list(providers))
        self.net_in = self.net.get_inputs()[0].name
        self.net_out = [o.name for o in self.net.get_outputs()]
        self.graphs = graphs
        self._keep = []

        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import make_graphs
        sizes = {"nh": self.nh, "nw": self.nw, "padh": self.ph - self.nh, "padw": self.pw - self.nw}

        def session(name, _unused):
            o = ort.SessionOptions()
            o.log_severity_level = 3
            data = make_graphs.patch_sizes((graphs / name).read_bytes(), sizes)
            return ort.InferenceSession(data, o, providers=list(providers))
        self.pack = session("pack-yuv.onnx", {"net_size": [1, 3, self.nh, self.nw], "pads": [0, 0, 0, 0, 0, 0, self.ph - self.nh, self.pw - self.nw]})
        self.synth = session("synth-yuv.onnx", {"crop_end": [self.nh, self.nw]})
        self.addt = session("addt.onnx", {})

    def network(self, fa, fb, t):
        """numpy: pack + network at t; returns grids, mask, the network's own picture (padded size)."""
        small = [np.pad(resize_linear(f, self.nh, self.nw), ((0, 0), (0, self.ph - self.nh), (0, self.pw - self.nw)), mode="edge") for f in (fa, fb)]
        x = np.concatenate([small[0], small[1], np.full((1, self.ph, self.pw), t, np.float32)])[None]
        rgb, g0, g1, m = self.net.run(None, {self.net_in: x})[:4]
        if not self.mask_for_first:
            m = 1 - m
        return g0, g1, m, rgb

    def numpy_frame(self, a, b, t, t_ref=None, cache=None):
        fa, fb = to_rgb(a, self.colour), to_rgb(b, self.colour)
        if t_ref is None:
            g0, g1, m, _ = self.network(fa, fb, t)
            s = (1.0, 1.0)
        else:
            if cache is None or "net" not in cache:
                net = self.network(fa, fb, t_ref)
                if cache is not None:
                    cache["net"] = net
            else:
                net = cache["net"]
            g0, g1, m, _ = net
            s = (t / t_ref, (1 - t) / (1 - t_ref))
        return from_rgb(synth_numpy(fa, fb, g0, g1, m, s, self.aff, self.nw, self.nh), self.colour)

    def graph_frame(self, a, b, t, t_ref=None):
        feed = {"a_y": a[0][None, None], "a_u": a[1][None, None], "a_v": a[2][None, None],
                "b_y": b[0][None, None], "b_u": b[1][None, None], "b_v": b[2][None, None],
                "colour": self.colour}
        small, fa, fb = self.pack.run(["small_ab", "frame_a", "frame_b"], feed)
        x = self.addt.run(["net_input"], {"small_ab": small, "t": np.array([t if t_ref is None else t_ref], np.float32)})[0]
        _, g0, g1, m = self.net.run(None, {self.net_in: x})[:4]
        if not self.mask_for_first:
            m = 1 - m
        s = [1.0, 1.0] if t_ref is None else [t / t_ref, (1 - t) / (1 - t_ref)]
        y, u, v = self.synth.run(["out_y", "out_u", "out_v"], {"frame_a": fa, "frame_b": fb, "grid0": g0, "grid1": g1, "mask": m,
                                 "scale": np.array(s, np.float32), "affine": np.array(self.aff, np.float32), "colour": self.colour})
        return y[0, 0], u[0, 0], v[0, 0]


def rebuild_check(models: Path, graphs: Path):
    """At the network's own size (no scaling, pad 0), the synth graph must reproduce the model's own picture."""
    w, h = 512, 384
    yy, xx = np.mgrid[0:h, 0:w]
    base = (60 + 40.0 * xx / w + 30.0 * yy / h)
    results = {}
    for tier in ("fast", "quality"):
        p = Pipeline(models, graphs, "balanced" if tier == "fast" else "quality", w, h, [0.2126, 0.0722, 0, 1, 0.5, 1, 255.0, 0])
        def frame(sx):
            img = base.copy()
            img[(xx >= sx) & (xx < sx + 48) & (yy >= 120) & (yy < 168)] = 230
            y = img.astype(np.uint16)
            return (y, np.full((h // 2, w // 2), 128, np.uint16), np.full((h // 2, w // 2), 128, np.uint16))
        a, b = frame(100), frame(124)
        fa, fb = to_rgb(a, p.colour), to_rgb(b, p.colour)
        g0, g1, m, rgb = p.network(fa, fb, 0.5)
        mine = synth_numpy(fa, fb, g0, g1, m, (1.0, 1.0), p.aff, p.nw, p.nh)
        model = np.clip(rgb[0][:, :h, :w], 0, 1)
        graph = p.graph_frame(a, b, 0.5)
        results[tier] = {"rebuild_vs_model_db": round(psnr(np.clip(mine, 0, 1), model, 1.0), 2),
                         "graph_vs_numpy_y_db": round(psnr(graph[0], from_rgb(mine, p.colour)[0]), 2)}
    return results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--models", required=True)
    ap.add_argument("--graphs", required=True)
    ap.add_argument("--clip")
    ap.add_argument("--frames", type=int, default=9)
    ap.add_argument("--tier", default="fast")
    ap.add_argument("--dump")
    ap.add_argument("--json")
    a = ap.parse_args()
    models, graphs = Path(a.models), Path(a.graphs)
    report = {"rebuild": rebuild_check(models, graphs)}
    print("rebuild:", json.dumps(report["rebuild"]))
    ok = all(v["rebuild_vs_model_db"] > 45 and v["graph_vs_numpy_y_db"] > 45 for v in report["rebuild"].values())
    if a.clip:
        w, h, frames = read_y4m(Path(a.clip), a.frames)
        p = Pipeline(models, graphs, a.tier, w, h, BT709_LIMITED_8)
        rows = {"2x_per_t": [], "4x_per_t": [], "4x_flow_once": [], "4x_blend": [], "4x_repeat": [], "graph_vs_numpy": []}
        dump = Path(a.dump) if a.dump else None
        if dump:
            dump.mkdir(parents=True, exist_ok=True)
        for k in range(0, len(frames) - 4, 4):
            a0, a4 = frames[k], frames[k + 4]
            cache = {}
            for j in (1, 2, 3):
                t = j / 4
                truth = frames[k + j][0]
                per_t = p.graph_frame(a0, a4, t)
                once = p.graph_frame(a0, a4, t, t_ref=0.5) if j != 2 else per_t
                rows["4x_per_t"].append(psnr(per_t[0], truth))
                rows["4x_flow_once"].append(psnr(once[0], truth))
                blend = np.round(a0[0] * (1 - t) + a4[0] * t)
                rows["4x_blend"].append(psnr(blend, truth))
                rows["4x_repeat"].append(psnr(a0[0] if t < 0.5 else a4[0], truth))
                if j == 1:
                    rows["graph_vs_numpy"].append(psnr(per_t[0], p.numpy_frame(a0, a4, t)[0]))
                if dump:
                    for name, planes in (("per_t", per_t), ("once", once)):
                        for c, plane in zip("yuv", planes):
                            plane.astype("<u2").tofile(dump / f"f{k:03d}_t{j}_{name}_{c}.u16")
            for j in (2,):
                rows["2x_per_t"].append(psnr(p.graph_frame(frames[k], frames[k + 4], 0.5)[0], frames[k + 2][0]))
        report["clip"] = {"name": Path(a.clip).stem, "size": [w, h], "tier": a.tier, "network": [p.nw, p.nh, p.pw, p.ph],
                          **{k: round(float(np.mean(v)), 2) for k, v in rows.items() if v}}
        print("clip:", json.dumps(report["clip"]))
        ok = ok and report["clip"]["graph_vs_numpy"] > 45 and report["clip"]["4x_per_t"] > report["clip"]["4x_repeat"]   # a blend can win on slow, finely textured content (ducks_take_off); a repeat must not
    if a.json:
        Path(a.json).write_text(json.dumps(report, indent=2))
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
