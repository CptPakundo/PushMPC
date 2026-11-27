#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""O4: PushFlow's network as int8 (QDQ) for Snapdragon's Hexagon NPU, with an accuracy gate.

The QNN EP's HTP backend runs quantised models. ONNX Runtime's quantisation for it (qnn_preprocess_model ->
get_qnn_qdq_config -> quantize, with a calibration data reader) runs on x86_64 only, on the CPU: it never needs a
Snapdragon. pmml then runs the int8 network on the NPU ("qnn") and its graphs (pack, the warp in synth) on the Adreno
GPU ("qnn-gpu"): pmml backend "qnn-split".

  1. Calibration: network inputs (pmml's own pack + addt graphs, exactly what pmml feeds the network) from pairs of the
     public Xiph derf clips CI already uses (never anyone's own media): the first --calib-frames of each clip, pairs 2
     and 4 frames apart, timesteps 0.25 / 0.5 / 0.75, at the network sizes of the tiers that use the model (fast: 540
     and 720 lines; quality: the clip's own size).
  2. Quantisation, every variant, in the HTP's speed order: A8W8 (uint8 activations and weights: the fastest), the same
     with per-channel weights, A16W8 (uint16 activations, uint8 weights: ONNX Runtime's recommendation when 8-bit
     activations lose accuracy), A16W8 with per-channel weights, the same calibrated by percentile (99.999) instead of
     min / max; A16W16 per-channel is measured for the record only (never written).
  3. The gate, on frames calibration never saw (the rest of each clip): the whole pmml pipeline (pack, network, synth)
     on the CPU EP with the fp16 network (what the NPU runs otherwise) and with each int8 network: PSNR-Y against the
     true middle frames (2x at t = 0.5, 4x at t = 0.25 / 0.5 / 0.75). A variant passes when its mean PSNR is at most
     --gate-db (0.3) below the fp16 network's on every clip's mean and overall, and above a frame repeat. The first
     passing variant in the order above is written; none passing writes the report with "pass": false (pmml then
     never offers qnn-split for that model: it keeps its other paths).

  python -I quantize_qnn.py --models DIR --graphs DIR --clips a.y4m b.y4m ... --role fast|quality --out DIR
                            [--calib-frames 24] [--frames 48] [--gate-db 0.3]
  Writes DIR/pushflow-<role>-qdq.onnx (when a variant passed) and DIR/pushflow-<role>-qdq.json (always). Exit 0 =
  a variant passed, 1 = none passed, 2 = usage / environment.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
import tempfile
from pathlib import Path

import numpy as np
import onnxruntime as ort

sys.path.insert(0, str(Path(__file__).resolve().parent))
import reference  # noqa: E402  (pmml's CPU reference: the same pack / synth graphs and the y4m reader)

TIERS_OF_ROLE = {"fast": ("fast", "balanced"), "quality": ("quality",)}


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


class Net:
    """One network session; float16 inputs / outputs converted (the -fp16 model may keep fp16 I/O)."""

    def __init__(self, path: Path):
        so = ort.SessionOptions()
        so.log_severity_level = 3
        self.s = ort.InferenceSession(str(path), so, providers=["CPUExecutionProvider"])
        i = self.s.get_inputs()[0]
        self.name, self.half = i.name, i.type == "tensor(float16)"

    def run(self, _outputs, feed):
        x = next(iter(feed.values()))
        out = self.s.run(None, {self.name: x.astype(np.float16) if self.half else x})
        return [o.astype(np.float32) for o in out]


def pipeline(models: Path, graphs: Path, tier: str, w: int, h: int, net: Path | None):
    p = reference.Pipeline(models, graphs, tier, w, h, reference.BT709_LIMITED_8)
    if net is not None:
        p.net = Net(net)
        p.net_in = p.net.name
    return p


def net_inputs(p, a, b, t):
    feed = {"a_y": a[0][None, None], "a_u": a[1][None, None], "a_v": a[2][None, None],
            "b_y": b[0][None, None], "b_u": b[1][None, None], "b_v": b[2][None, None], "colour": p.colour}
    small = p.pack.run(["small_ab"], feed)[0]
    return p.addt.run(["net_input"], {"small_ab": small, "t": np.array([t], np.float32)})[0]


def make_reader(base, inputs, name):
    """a CalibrationDataReader over the network inputs (base: onnxruntime.quantization.CalibrationDataReader)"""
    class Reader(base):
        def __init__(self):
            self.items = [{name: x} for x in inputs]
            self.i = 0

        def get_next(self):
            if self.i >= len(self.items):
                return None
            self.i += 1
            return self.items[self.i - 1]

        def rewind(self):
            self.i = 0
    return Reader()


def constants_to_initializers(src: Path, dst: Path) -> int:
    """Constant nodes -> initializers. The quantiser treats a Constant node's output as an activation and puts a
    QuantizeLinear/DequantizeLinear pair on it. For RIFE's Resize scales that breaks the model: /block0/Resize_1's scales
    [1, 1, 32, 32] go through uint8 with the range [0, 32] (step 32/255), so 1.0 comes back as 1.0039 (uint16: 1.000015),
    and ONNX Runtime's CPU Resize rejects 'linear' with outer scales != 1 (upsamplebase.h ScalesValidation); smaller
    scales ([1, 1, 1/32, 1/32]) survive the round trip only by luck and are still shifted. As initializers the Resize
    parameters stay exact (the QDQ Resize handler quantises only the data input), and the QNN HTP needs them static."""
    import onnx
    from onnx import helper, numpy_helper
    m = onnx.load(str(src))
    g = m.graph
    keep, n = [], 0
    for node in g.node:
        if node.op_type != "Constant" or node.domain not in ("", "ai.onnx"):
            keep.append(node)
            continue
        a = node.attribute[0]
        v = helper.get_attribute_value(a)
        if a.name == "value":
            arr = numpy_helper.to_array(v)
        elif a.name in ("value_float", "value_floats"):
            arr = np.array(v, np.float32)
        elif a.name in ("value_int", "value_ints"):
            arr = np.array(v, np.int64)
        else:  # sparse / string constants: leave the node
            keep.append(node)
            continue
        g.initializer.append(numpy_helper.from_array(arr, node.output[0]))
        n += 1
    del g.node[:]
    g.node.extend(keep)
    onnx.save(m, str(dst))
    return n


def check_static_params(path: Path) -> None:
    """the quantised model keeps every Resize roi / scales / sizes exact: no DequantizeLinear produces them"""
    import onnx
    m = onnx.load(str(path))
    dq = {o for nd in m.graph.node if nd.op_type == "DequantizeLinear" for o in nd.output}
    bad = [nd.name for nd in m.graph.node if nd.op_type == "Resize" and any(i in dq for i in nd.input[1:] if i)]
    if bad:   # exit 2 (environment / tool error), not 1 (gate not passed): CI fails the job on it
        print(f"{path.name}: quantised Resize parameters (roi/scales/sizes) in {', '.join(bad)}", file=sys.stderr)
        raise SystemExit(2)


def evaluate(models, graphs, clips, tier, net):
    """mean PSNR-Y per clip (2x and 4x per t) of the pipeline with this network on the held-out frames, and a repeat"""
    out = {}
    for clip in clips:
        w, h, frames = clip["w"], clip["h"], clip["held"]
        p = pipeline(models, graphs, tier, w, h, net)
        vals, rep = [], []
        for k in range(0, len(frames) - 4, 4):
            a0, a4 = frames[k], frames[k + 4]
            for j in (1, 2, 3):
                y = p.graph_frame(a0, a4, j / 4)[0]
                vals.append(reference.psnr(y, frames[k + j][0]))
                rep.append(reference.psnr(a0[0] if j < 2 else a4[0], frames[k + j][0]))
            y = p.graph_frame(frames[k], frames[k + 2], 0.5)[0]
            vals.append(reference.psnr(y, frames[k + 1][0]))
        out[clip["name"]] = {"psnr": float(np.mean(vals)), "repeat": float(np.mean(rep)), "n": len(vals)}
        if not vals:
            raise SystemExit(f"{clip['name']}: no held-out frames (raise --frames)")
    return out


RIFE_MIT = """RIFE (Real-Time Intermediate Flow Estimation), Practical-RIFE v4 weights: https://github.com/hzwer/Practical-RIFE
ONNX conversions: https://github.com/AmusementClub/vs-mlrt (external-models)

MIT License

Copyright (c) 2021 hzwer

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
"""


def write_licence(models: Path, out: Path, report: dict) -> None:
    """RIFE_LICENSE.txt (PushFPS's copy when the models folder has it, else the same MIT text) and NOTICE.txt (what the
    int8 networks are made from, how, and under which terms) next to the networks"""
    src = next((p for p in (models / "licenses" / "RIFE_LICENSE.txt", models.parent / "licenses" / "RIFE_LICENSE.txt") if p.exists()), None)
    (out / "RIFE_LICENSE.txt").write_text(src.read_text(encoding="utf-8") if src else RIFE_MIT, encoding="utf-8")
    notice = out / "NOTICE.txt"
    lines = notice.read_text(encoding="utf-8").splitlines() if notice.exists() else [
        "int8 (QDQ) versions of PushFlow's RIFE networks, for the Snapdragon NPU (PushMPC, pmml backend qnn-split).",
        "",
        "Made from: the RIFE models PushFPS ships (Practical-RIFE v4.25 lite and v4.26 weights by hzwer, MIT, see",
        "RIFE_LICENSE.txt; ONNX conversions by vs-mlrt, https://github.com/AmusementClub/vs-mlrt, GPL-3.0; prepared by",
        "PushFPS's scripts/pushflow/prepare_runtime.py, GPL-3.0, https://github.com/DosukaSOL/PushFPS, at the tag pinned",
        "in PushMPC's scripts/pushfps-pin.json).",
        "Made by: PushMPC's pmml/scripts/quantize_qnn.py with ONNX Runtime's QNN quantisation tools, calibrated on public",
        "Xiph test clips; built by .github/workflows/pmml-qdq.yml.",
        "These files are distributed under the GPL-3.0-or-later with PushMPC (the conversions they derive from are",
        "GPL-3.0); the RIFE weights' MIT notice in RIFE_LICENSE.txt applies to them as well. The source to rebuild them",
        "is PushMPC's repository at the release tag together with the pinned PushFPS tag.",
        "",
        "Files:"]
    if report.get("pass"):
        lines.append(f"  {report['file']}  sha256 {report['sha256']}  from {report['source']} (sha256 {report['source_sha256']}), {report['variant']}")
    notice.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--models", required=True)
    ap.add_argument("--graphs", required=True)
    ap.add_argument("--clips", nargs="+", required=True)
    ap.add_argument("--role", choices=("fast", "quality"), required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--calib-frames", type=int, default=24)
    ap.add_argument("--frames", type=int, default=48)
    ap.add_argument("--gate-db", type=float, default=0.3)
    ap.add_argument("--variants", default="", help="comma-separated variant names (default: all)")
    a = ap.parse_args()
    sys.stdout.reconfigure(line_buffering=True)  # CI tees this: keep the progress lines in order with ORT's stderr
    try:
        from onnxruntime.quantization import CalibrationDataReader, QuantType, quantize
        from onnxruntime.quantization.execution_providers.qnn import get_qnn_qdq_config, qnn_preprocess_model
    except ImportError as e:
        print(f"ONNX Runtime's QNN quantisation tools are missing ({e}): onnxruntime >= 1.17 on x86_64", file=sys.stderr)
        return 2
    models, graphs, out = Path(a.models), Path(a.graphs), Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    fp32 = models / f"pushflow-{a.role}.onnx"
    fp16 = models / f"pushflow-{a.role}-fp16.onnx"
    ref = fp16 if fp16.exists() else fp32
    if a.frames < a.calib_frames + 9:
        print("--frames must leave at least 9 held-out frames after --calib-frames", file=sys.stderr)
        return 2

    clips = []
    for c in a.clips:
        w, h, frames = reference.read_y4m(Path(c), a.frames)
        if len(frames) < a.calib_frames + 9:
            print(f"{c}: {len(frames)} frames, {a.calib_frames + 9} needed", file=sys.stderr)
            return 2
        clips.append({"name": Path(c).stem, "w": w, "h": h, "calib": frames[:a.calib_frames], "held": frames[a.calib_frames:]})

    # 1. calibration inputs
    inputs, name = [], None
    for tier in TIERS_OF_ROLE[a.role]:
        for clip in clips:
            p = pipeline(models, graphs, tier, clip["w"], clip["h"], None)
            name = p.net_in
            fr = clip["calib"]
            for gap in (2, 4):
                for k in range(0, len(fr) - gap, gap * 2):
                    for t in (0.25, 0.5, 0.75):
                        inputs.append(net_inputs(p, fr[k], fr[k + gap], t))
    print(f"calibration: {len(inputs)} network inputs from {len(clips)} public clips, tiers {', '.join(TIERS_OF_ROLE[a.role])}")

    # 2. quantise (A8W8, then A16W8)
    work = Path(tempfile.mkdtemp(prefix="pmml-qdq-"))
    pre = work / "pre.onnx"
    src = work / "static.onnx"
    folded = constants_to_initializers(pre if qnn_preprocess_model(str(fp32), str(pre)) else fp32, src)
    print(f"{folded} Constant nodes made initializers (Resize scales and other parameters stay exact)", flush=True)
    # every variant is quantised and measured (the report has each one's numbers); the first that passes in this order
    # (the HTP's fastest first) is written. A16W16 is measured for the record only: whether the HTP runs 16-bit weights
    # in every op of this network is not confirmed on a device, so it is never written.
    from onnxruntime.quantization import CalibrationMethod
    U8, U16 = QuantType.QUInt8, QuantType.QUInt16
    mm, pct = CalibrationMethod.MinMax, CalibrationMethod.Percentile
    variants = [
        {"name": "A8W8", "act": U8, "wt": U8, "per_channel": False, "method": mm, "ship": True},
        {"name": "A8W8-pc", "act": U8, "wt": U8, "per_channel": True, "method": mm, "ship": True},
        {"name": "A16W8", "act": U16, "wt": U8, "per_channel": False, "method": mm, "ship": True},
        {"name": "A16W8-pc", "act": U16, "wt": U8, "per_channel": True, "method": mm, "ship": True},
        {"name": "A16W8-pc-p99999", "act": U16, "wt": U8, "per_channel": True, "method": pct, "ship": True},
        {"name": "A16W16-pc", "act": U16, "wt": U16, "per_channel": True, "method": mm, "ship": False},
    ]
    if a.variants:
        variants = [v for v in variants if v["name"] in a.variants.split(",")]

    # 3. the gate
    gate_tier = TIERS_OF_ROLE[a.role][0]
    base = evaluate(models, graphs, clips, gate_tier, ref)
    report = {"role": a.role, "source": fp32.name, "source_sha256": sha256(fp32), "reference": ref.name, "tier": gate_tier,
              "onnxruntime": ort.__version__, "calibration": {"clips": [c["name"] for c in clips], "frames_per_clip": a.calib_frames,
              "inputs": len(inputs)}, "held_out_frames_per_clip": a.frames - a.calib_frames, "gate_db": a.gate_db,
              "fp16": base, "variants": {}, "pass": False}
    print("fp16:", json.dumps(base))
    for v in variants:
        vname = v["name"]
        qdq = work / f"{vname}.onnx"
        cfg = get_qnn_qdq_config(str(src), make_reader(CalibrationDataReader, inputs, name), calibrate_method=v["method"],
                                 activation_type=v["act"], weight_type=v["wt"], per_channel=v["per_channel"])
        if v["method"] == CalibrationMethod.Percentile:
            cfg.extra_options["CalibPercentile"] = 99.999
        quantize(str(src), str(qdq), cfg)
        check_static_params(qdq)
        res = evaluate(models, graphs, clips, gate_tier, qdq)
        losses = {c: base[c]["psnr"] - res[c]["psnr"] for c in res}
        overall = float(np.mean([base[c]["psnr"] for c in res]) - np.mean([res[c]["psnr"] for c in res]))
        ok = all(l <= a.gate_db for l in losses.values()) and overall <= a.gate_db and all(res[c]["psnr"] > res[c]["repeat"] for c in res)
        report["variants"][vname] = {"result": res, "loss_db": {c: round(l, 3) for c, l in losses.items()}, "loss_overall_db": round(overall, 3),
                                     "pass": ok, "shippable": v["ship"], "per_channel": v["per_channel"], "calibration": v["method"].name}
        print(f"{vname}: loss {overall:.3f} dB overall, per clip {json.dumps({c: round(l, 3) for c, l in losses.items()})}: "
              f"{'PASS' if ok else 'FAIL'}{'' if v['ship'] else ' (measured only, never written)'}")
        if ok and v["ship"] and not report["pass"]:
            dst = out / f"pushflow-{a.role}-qdq.onnx"
            dst.write_bytes(qdq.read_bytes())
            report.update({"pass": True, "variant": vname, "file": dst.name, "sha256": sha256(dst)})
        qdq.unlink(missing_ok=True)
    (out / f"pushflow-{a.role}-qdq.json").write_text(json.dumps(report, indent=2))
    write_licence(models, out, report)
    print("PASS" if report["pass"] else "FAIL: no int8 variant within the gate; qnn-split stays off for this model")
    return 0 if report["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
