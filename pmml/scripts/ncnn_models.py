#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""PushFlow's ONNX models -> ncnn (param/bin) with pnnx, per network size, checked against ONNX Runtime's CPU EP.

The ncnn backend runs the same models as every other pmml backend (with the grid and mask outputs), so its full-size
rebuild and "flow once" are the same. pnnx needs fixed shapes: one conversion per padded network size.

  python -I ncnn_models.py --models DIR --out DIR --sizes 640x384,1280x768 [--roles fast,quality]

Writes <out>/pushflow-<role>-<W>x<H>.ncnn.param / .bin and <out>/ncnn-models.json: blob names of the input and the
four outputs (picture, grid0, grid1, mask) and the agreement with ONNX Runtime (dB per output).
"""
from __future__ import annotations

import argparse
import json
import math
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np


def psnr(a, b, peak):
    mse = float(np.mean((np.asarray(a, np.float64) - np.asarray(b, np.float64)) ** 2))
    return 99.0 if mse < 1e-12 else 10 * math.log10(peak * peak / mse)


def frames(w, h):
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    a = np.stack([(0.3 + 0.2 * np.sin(x / 7.0) * np.cos(y / 9.0))] * 3)
    b = np.roll(a, 6, axis=2)
    a[:, h // 3:h // 3 + 40, 100:160] = 0.9
    b[:, h // 3:h // 3 + 40, 112:172] = 0.9
    return a.astype(np.float32), b.astype(np.float32)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--models", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--sizes", default="640x384")
    ap.add_argument("--roles", default="fast,quality")
    a = ap.parse_args()
    import ncnn
    import onnxruntime as ort
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    pnnx = shutil.which("pnnx")
    if not pnnx:
        import pnnx as pnnx_mod  # noqa: F401  (the wheel provides the executable inside the package)
        cand = list(Path(pnnx_mod.__file__).parent.glob("pnnx*"))
        pnnx = next((str(c) for c in cand if c.is_file() and c.suffix in ("", ".exe")), None)
    record = {"converter": "pnnx", "models": {}}
    for role in a.roles.split(","):
        src = Path(a.models) / f"pushflow-{role}.onnx"
        sess = ort.InferenceSession(str(src), providers=["CPUExecutionProvider"])
        in_name = sess.get_inputs()[0].name
        out_names = [o.name for o in sess.get_outputs()][:4]
        for size in a.sizes.split(","):
            w, h = (int(v) for v in size.split("x"))
            stem = f"pushflow-{role}-{w}x{h}"
            work = out / f"{stem}.work"
            shutil.rmtree(work, ignore_errors=True)
            work.mkdir()
            shutil.copy2(src, work / f"{stem}.onnx")
            entry = {"file": f"{stem}.ncnn.param", "size": [w, h], "ok": False}
            try:
                r = subprocess.run([pnnx, f"{stem}.onnx", f"inputshape=[1,7,{h},{w}]"], cwd=work, capture_output=True, text=True, timeout=600)
                log = r.stdout + r.stderr
                (out / f"{stem}.pnnx.log").write_text(log, encoding="utf-8")   # the whole log (an artifact): the reason pnnx stops
                log = "".join(ch for ch in log if ch.isascii())
                param, binf = work / f"{stem}.ncnn.param", work / f"{stem}.ncnn.bin"
                if not param.exists():
                    lines = [ln for ln in log.splitlines() if any(k in ln.lower() for k in ("error", "unsupported", "not supported", "fail", "abort", "ignore"))]
                    entry["error"] = f"pnnx exit {r.returncode}, no ncnn param: " + " | ".join(lines[-12:])[-1200:]
                    raise RuntimeError(entry["error"])
                shutil.copy2(param, out / param.name)
                shutil.copy2(binf, out / binf.name)
                text = param.read_text()
                layers = [ln.split()[0] for ln in text.splitlines()[2:] if ln.strip()]
                entry["layers"] = {k: layers.count(k) for k in sorted(set(layers))}
                # blob names: pnnx names the inputs in0.. and the outputs out0..
                fa, fb = frames(w, h)
                x = np.concatenate([fa, fb, np.full((1, h, w), 0.5, np.float32)])[None]
                ref = sess.run(out_names, {in_name: x})
                net = ncnn.Net()
                net.opt.use_vulkan_compute = False
                net.load_param(str(out / param.name))
                net.load_model(str(out / binf.name))
                ex = net.create_extractor()
                ex.input("in0", ncnn.Mat(x[0]).clone())
                agree = {}
                for k, name in enumerate(out_names):
                    ret, m = ex.extract(f"out{k}")
                    got = np.array(m)
                    want = ref[k][0]
                    if got.shape != want.shape:
                        got = got.reshape(want.shape) if got.size == want.size else got
                    agree[name] = round(psnr(got, want, 2.0 if k in (1, 2) else 1.0), 2) if got.shape == want.shape else f"shape {got.shape} vs {want.shape}"
                entry.update({"ok": all(isinstance(v, float) and v > 40 for v in agree.values()), "agreement_db": agree,
                              "blobs": {"input": "in0", "outputs": [f"out{k}" for k in range(4)]}})
            except Exception as e:  # noqa: BLE001 - recorded, the backend is then unavailable for this model
                entry.setdefault("error", f"{type(e).__name__}: {e}"[:1500])
            print(json.dumps({stem: entry}))
            record["models"][stem] = entry
            shutil.rmtree(work, ignore_errors=True)
    (out / "ncnn-models.json").write_text(json.dumps(record, indent=2))
    return 0 if any(v["ok"] for v in record["models"].values()) else 1


if __name__ == "__main__":
    sys.exit(main())
