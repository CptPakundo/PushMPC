#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes pmml's small ONNX graphs that run around a PushFlow (RIFE) model on the same execution provider, so
frames go to the GPU once per pair and nothing but the finished picture comes back:

  pack-<fmt>.onnx   full-size frames (16-bit planes) -> both frames at the network's padded size [1,6,ph,pw]
                    (RGB 0..1), plus the two frames as RGB float on the device for synth (once per pair).
  addt.onnx         [1,6,ph,pw] + t -> the network's input [1,7,ph,pw] (once per network run).
  synth-<fmt>.onnx  the picture at full size from the network's grids and mask (what PushFPS's
                    PushFlowReconstruct.hlsl does: sampled from the full-size real frames), with the grids'
                    displacement scaled by s0 / s1 ("flow once, synthesize many": the network runs once at t_ref and
                    further timesteps reuse its motion; s = 1 is the plain rebuild).
  synth_map-<fmt>.onnx  the same with s0 / s1 per pixel (scale0_map / scale1_map [1,ph,pw,1]): a phase per pixel.

fmt: rgb   three full-size planes R, G, B
     yuv   Y full size, U and V at half size in both directions (4:2:0), BT.601/709/2020 by the `colour` input

Every size is a named free dimension (fixed per session with AddFreeDimensionOverrideByName) and the network size
and padding are sentinel constants that pmml replaces in the model bytes per session (patch_sizes), so every shape is
known when the session is created (DirectML / TensorRT-RTX plan the whole graph). Opset 16 (GridSample).

  colour = [Kr, Kb, y_off, y_scale, c_off, c_scale, max_value, 0]  (inputs scaled to 0..1 by 1/max_value first;
           y = (code/max - y_off) / y_scale; full range: y_off 0, y_scale 1, c_off 0.5, c_scale 1)
  scale  = [s0, s1]          the grids' displacement factors (t / t_ref, (1 - t) / (1 - t_ref))
  affine = [ax, bx, ay, by]  network-normalised grid -> full-size-normalised grid (computed by pmml)

Usage: python -I make_graphs.py --out DIR
"""
from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper

OPSET = 16
# Size constants are written as unique 64-bit sentinels; pmml replaces them in the model bytes before it creates a
# session (patch_sizes below; pmml's ort.cpp does the same), so they are true constants for every EP. (Overriding
# initializers with AddInitializer works on the CPU EP but DirectML folds the file's own values.)
SENTINEL = {"nh": 0x504D4D4C00000001, "nw": 0x504D4D4C00000002, "padh": 0x504D4D4C00000003, "padw": 0x504D4D4C00000004}


def patch_sizes(data: bytes, sizes: dict) -> bytes:
    """sizes: {"nh", "nw", "padh", "padw"} -> the model bytes with every sentinel replaced (little-endian int64)."""
    for k, v in sizes.items():
        data = data.replace(SENTINEL[k].to_bytes(8, "little"), int(v).to_bytes(8, "little", signed=True))
    return data


class G:
    def __init__(self, name):
        self.name, self.nodes, self.inits, self.inputs, self.outputs, self.n = name, [], [], [], [], 0

    def tmp(self, base="t"):
        self.n += 1
        return f"{base}_{self.n}"

    def op(self, op_type, inputs, out=None, **attrs):
        out = out or self.tmp(op_type.lower())
        self.nodes.append(helper.make_node(op_type, inputs, [out], name=f"{self.name}_{self.n}_{op_type}", **attrs))
        self.n += 1
        return out

    def const(self, value, dtype=np.float32, name=None):
        name = name or self.tmp("c")
        self.inits.append(numpy_helper.from_array(np.asarray(value, dtype=dtype), name))
        return name

    def model(self):
        g = helper.make_graph(self.nodes, self.name, self.inputs, self.outputs, self.inits)
        m = helper.make_model(g, opset_imports=[helper.make_opsetid("", OPSET)], producer_name="pmml")
        m.ir_version = 8
        onnx.checker.check_model(m)
        return m


def to_rgb(g: G, planes, fmt, colour):
    """16-bit planes -> RGB float [1,3,H,W] 0..1 (BT matrix from `colour`)."""
    inv_max = g.op("Reciprocal", [g.op("Gather", [colour, g.const(6, np.int64)])])
    if fmt == "rgb":
        p = [g.op("Mul", [g.op("Cast", [x], to=TensorProto.FLOAT), inv_max]) for x in planes]
        return g.op("Concat", p, axis=1)
    y, u, v = (g.op("Mul", [g.op("Cast", [x], to=TensorProto.FLOAT), inv_max]) for x in planes)
    k = lambda i: g.op("Gather", [colour, g.const(i, np.int64)])  # noqa: E731
    kr, kb, yo, ys, co, cs = (k(i) for i in range(6))
    shape_y = g.op("Shape", [y])
    uv = g.op("Concat", [u, v], axis=1)
    uv = g.op("Resize", [uv, "", "", g.op("Concat", [g.const([1, 2], np.int64), g.op("Slice", [shape_y, g.const([2], np.int64), g.const([4], np.int64)])], axis=0)],
              mode="linear", coordinate_transformation_mode="half_pixel")
    yl = g.op("Div", [g.op("Sub", [y, yo]), ys])
    uvc = g.op("Div", [g.op("Sub", [uv, co]), cs])
    cb = g.op("Slice", [uvc, g.const([0], np.int64), g.const([1], np.int64), g.const([1], np.int64)])
    cr = g.op("Slice", [uvc, g.const([1], np.int64), g.const([2], np.int64), g.const([1], np.int64)])
    one = g.const(1.0)
    two = g.const(2.0)
    kg = g.op("Sub", [g.op("Sub", [one, kr]), kb])
    r = g.op("Add", [yl, g.op("Mul", [cr, g.op("Mul", [two, g.op("Sub", [one, kr])])])])
    b = g.op("Add", [yl, g.op("Mul", [cb, g.op("Mul", [two, g.op("Sub", [one, kb])])])])
    gg = g.op("Div", [g.op("Sub", [g.op("Sub", [yl, g.op("Mul", [kr, r])]), g.op("Mul", [kb, b])]), kg])
    return g.op("Concat", [r, gg, b], axis=1)


def from_rgb(g: G, rgb, fmt, colour, outs):
    """RGB float [1,3,H,W] -> 16-bit planes (rounded, clamped)."""
    mx = g.op("Gather", [colour, g.const(6, np.int64)])

    def code(x, name):
        q = g.op("Round", [g.op("Mul", [g.op("Clip", [x, g.const(0.0), g.const(1.0)]), mx])])
        g.op("Cast", [q], out=name, to=TensorProto.UINT16)

    ch = [g.op("Slice", [rgb, g.const([i], np.int64), g.const([i + 1], np.int64), g.const([1], np.int64)]) for i in range(3)]
    if fmt == "rgb":
        for c, name in zip(ch, outs):
            code(c, name)
        return
    k = lambda i: g.op("Gather", [colour, g.const(i, np.int64)])  # noqa: E731
    kr, kb, yo, ys, co, cs = (k(i) for i in range(6))
    one, two = g.const(1.0), g.const(2.0)
    r, gg, b = ch
    kg = g.op("Sub", [g.op("Sub", [one, kr]), kb])
    yl = g.op("Add", [g.op("Add", [g.op("Mul", [kr, r]), g.op("Mul", [kg, gg])]), g.op("Mul", [kb, b])])
    cb = g.op("Div", [g.op("Sub", [b, yl]), g.op("Mul", [two, g.op("Sub", [one, kb])])])
    cr = g.op("Div", [g.op("Sub", [r, yl]), g.op("Mul", [two, g.op("Sub", [one, kr])])])
    code(g.op("Add", [g.op("Mul", [yl, ys]), yo]), outs[0])
    uv = g.op("Concat", [cb, cr], axis=1)
    uv = g.op("AveragePool", [uv], kernel_shape=[2, 2], strides=[2, 2], pads=[0, 0, 0, 0])   # explicit pads: CoreML's ML Program needs them
    uv = g.op("Add", [g.op("Mul", [uv, cs]), co])
    code(g.op("Slice", [uv, g.const([0], np.int64), g.const([1], np.int64), g.const([1], np.int64)]), outs[1])
    code(g.op("Slice", [uv, g.const([1], np.int64), g.const([2], np.int64), g.const([1], np.int64)]), outs[2])


def plane_inputs(g: G, prefix, fmt):
    names = [f"{prefix}_{c}" for c in ("r", "g", "b")] if fmt == "rgb" else [f"{prefix}_{c}" for c in ("y", "u", "v")]
    for i, n in enumerate(names):
        sub = fmt == "yuv" and i > 0
        g.inputs.append(helper.make_tensor_value_info(n, TensorProto.UINT16, [1, 1, "pmml_ch" if sub else "pmml_h", "pmml_cw" if sub else "pmml_w"]))
    return names


def pack(fmt):
    g = G(f"pmml_pack_{fmt}")
    a = plane_inputs(g, "a", fmt)
    b = plane_inputs(g, "b", fmt)
    g.inputs += [helper.make_tensor_value_info("colour", TensorProto.FLOAT, [8])]
    net_size = g.const([1, 3, SENTINEL["nh"], SENTINEL["nw"]], np.int64, name="net_size")               # [1, 3, nh, nw]
    pads = g.const([0, 0, 0, 0, 0, 0, SENTINEL["padh"], SENTINEL["padw"]], np.int64, name="pads")     # [0,0,0,0, 0,0,ph-nh,pw-nw]
    rgb_a = to_rgb(g, a, fmt, "colour")
    rgb_b = to_rgb(g, b, fmt, "colour")
    g.op("Identity", [rgb_a], out="frame_a")
    g.op("Identity", [rgb_b], out="frame_b")
    small = []
    for x in (rgb_a, rgb_b):
        r = g.op("Resize", [x, "", "", net_size], mode="linear", coordinate_transformation_mode="half_pixel")
        small.append(g.op("Pad", [r, pads], mode="edge"))
    g.op("Concat", [small[0], small[1]], out="small_ab", axis=1)
    g.outputs += [helper.make_tensor_value_info("small_ab", TensorProto.FLOAT, [1, 6, "pmml_ph", "pmml_pw"]),
                  helper.make_tensor_value_info("frame_a", TensorProto.FLOAT, [1, 3, "pmml_h", "pmml_w"]),
                  helper.make_tensor_value_info("frame_b", TensorProto.FLOAT, [1, 3, "pmml_h", "pmml_w"])]
    return g.model()


def addt():
    """small_ab [1,6,ph,pw] + t -> the network's input [1,7,ph,pw] (the t plane): a pair is packed once, every
    network run only adds its timestep."""
    g = G("pmml_addt")
    g.inputs += [helper.make_tensor_value_info("small_ab", TensorProto.FLOAT, [1, 6, "pmml_ph", "pmml_pw"]),
                 helper.make_tensor_value_info("t", TensorProto.FLOAT, [1])]
    plane = g.op("Slice", ["small_ab", g.const([0], np.int64), g.const([1], np.int64), g.const([1], np.int64)])
    tplane = g.op("Add", [g.op("Mul", [plane, g.const(0.0)]), g.op("Reshape", ["t", g.const([1, 1, 1, 1], np.int64)])])
    g.op("Concat", ["small_ab", tplane], out="net_input", axis=1)
    g.outputs.append(helper.make_tensor_value_info("net_input", TensorProto.FLOAT, [1, 7, "pmml_ph", "pmml_pw"]))
    return g.model()


def synth(fmt, per_pixel=False):
    """per_pixel (synth_map-<fmt>.onnx): the displacement factors are maps scale0_map / scale1_map [1, ph, pw, 1] at
    the network's padded size instead of the scalar pair `scale` (s0 = t / t_ref, s1 = (1 - t) / (1 - t_ref) per pixel):
    the held-drawing repair gives the parts of one frame that share a pair their own phases in one network run."""
    g = G(f"pmml_synth_map_{fmt}" if per_pixel else f"pmml_synth_{fmt}")
    scale_inputs = ([helper.make_tensor_value_info("scale0_map", TensorProto.FLOAT, [1, "pmml_ph", "pmml_pw", 1]),
                     helper.make_tensor_value_info("scale1_map", TensorProto.FLOAT, [1, "pmml_ph", "pmml_pw", 1])]
                    if per_pixel else [helper.make_tensor_value_info("scale", TensorProto.FLOAT, [2])])
    g.inputs += [helper.make_tensor_value_info("frame_a", TensorProto.FLOAT, [1, 3, "pmml_h", "pmml_w"]),
                 helper.make_tensor_value_info("frame_b", TensorProto.FLOAT, [1, 3, "pmml_h", "pmml_w"]),
                 helper.make_tensor_value_info("grid0", TensorProto.FLOAT, [1, "pmml_ph", "pmml_pw", 2]),
                 helper.make_tensor_value_info("grid1", TensorProto.FLOAT, [1, "pmml_ph", "pmml_pw", 2]),
                 helper.make_tensor_value_info("mask", TensorProto.FLOAT, [1, 1, "pmml_ph", "pmml_pw"])] + scale_inputs + [
                 helper.make_tensor_value_info("affine", TensorProto.FLOAT, [4]),
                 helper.make_tensor_value_info("colour", TensorProto.FLOAT, [8])]
    crop = g.const([SENTINEL["nh"], SENTINEL["nw"]], np.int64, name="crop_end")   # [nh, nw]
    i64 = lambda v: g.const(v, np.int64)  # noqa: E731
    # identity grid at the padded network size, align_corners = 1: 2 * x / (n - 1) - 1
    shp = g.op("Shape", ["grid0"])
    ph = g.op("Cast", [g.op("Gather", [shp, i64(1)])], to=TensorProto.FLOAT)
    pw = g.op("Cast", [g.op("Gather", [shp, i64(2)])], to=TensorProto.FLOAT)
    one, two = g.const(1.0), g.const(2.0)
    xs = g.op("Range", [g.const(0.0), pw, one])
    ys = g.op("Range", [g.const(0.0), ph, one])
    gx = g.op("Sub", [g.op("Div", [g.op("Mul", [xs, two]), g.op("Max", [g.op("Sub", [pw, one]), one])]), one])
    gy = g.op("Sub", [g.op("Div", [g.op("Mul", [ys, two]), g.op("Max", [g.op("Sub", [ph, one]), one])]), one])
    full = g.op("Concat", [i64([1]), g.op("Slice", [shp, i64([1]), i64([3])]), i64([1])], axis=0)   # [1, ph, pw, 1]
    gxe = g.op("Expand", [g.op("Reshape", [gx, i64([1, 1, -1, 1])]), full])
    gye = g.op("Expand", [g.op("Reshape", [gy, i64([1, -1, 1, 1])]), full])
    ident = g.op("Concat", [gxe, gye], axis=3)                                                         # [1, ph, pw, 2]
    aff = "affine"
    a_vec = g.op("Concat", [g.op("Gather", [aff, i64([0])]), g.op("Gather", [aff, i64([2])])], axis=0)  # [ax, ay]
    b_vec = g.op("Concat", [g.op("Gather", [aff, i64([1])]), g.op("Gather", [aff, i64([3])])], axis=0)  # [bx, by]
    shape_img = g.op("Shape", ["frame_a"])
    hw = g.op("Slice", [shape_img, i64([2]), i64([4])])
    up2 = g.op("Concat", [i64([1, 2]), hw], axis=0)
    up1 = g.op("Concat", [i64([1, 1]), hw], axis=0)
    warped = []
    for k, (grid, frame) in enumerate((("grid0", "frame_a"), ("grid1", "frame_b"))):
        s = ("scale0_map", "scale1_map")[k] if per_pixel else g.op("Gather", ["scale", i64([k])])   # [1,ph,pw,1] broadcasts over x/y
        gs = g.op("Add", [ident, g.op("Mul", [g.op("Sub", [grid, ident]), s])])
        gs = g.op("Add", [g.op("Mul", [gs, a_vec]), b_vec])
        t = g.op("Transpose", [gs], perm=[0, 3, 1, 2])
        t = g.op("Slice", [t, i64([0, 0]), crop, i64([2, 3])])
        t = g.op("Resize", [t, "", "", up2], mode="linear", coordinate_transformation_mode="half_pixel")
        f = g.op("Transpose", [t], perm=[0, 2, 3, 1])
        warped.append(g.op("GridSample", [frame, f], align_corners=1, mode="bilinear", padding_mode="border"))
    m = g.op("Slice", ["mask", i64([0, 0]), crop, i64([2, 3])])
    m = g.op("Resize", [m, "", "", up1], mode="linear", coordinate_transformation_mode="half_pixel")
    rgb = g.op("Add", [warped[1], g.op("Mul", [g.op("Sub", [warped[0], warped[1]]), m])])
    names = ["out_r", "out_g", "out_b"] if fmt == "rgb" else ["out_y", "out_u", "out_v"]
    from_rgb(g, rgb, fmt, "colour", names)
    for i, n in enumerate(names):
        sub = fmt == "yuv" and i > 0
        g.outputs.append(helper.make_tensor_value_info(n, TensorProto.UINT16, [1, 1, "pmml_ch" if sub else "pmml_h", "pmml_cw" if sub else "pmml_w"]))
    return g.model()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    onnx.save(addt(), str(out / "addt.onnx"))
    print(f"wrote {out / 'addt.onnx'}")
    for fmt in ("rgb", "yuv"):
        for kind, make in (("pack", pack), ("synth", synth)):
            path = out / f"{kind}-{fmt}.onnx"
            onnx.save(make(fmt), str(path))
            print(f"wrote {path}")
        path = out / f"synth_map-{fmt}.onnx"
        onnx.save(synth(fmt, per_pixel=True), str(path))
        print(f"wrote {path}")


if __name__ == "__main__":
    main()
