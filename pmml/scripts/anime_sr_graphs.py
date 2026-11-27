#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""pmml's anime upscalers (PushMPC's "Anime upscaler" Auto candidates): the graphs around the models and the model
graphs themselves, written without the weights.

Models (pinned in scripts/anime-models-pin.json: URL, size, SHA-256; Setup fetches them, never this script):
  ArtCNN (MIT)           upstream ONNX as released (luma in [1,1,H,W] 0..1 -> [1,1,2H,2W]); used unmodified
  CuNNy (LGPL-3.0)       the released pretrained weights (a pickle of numpy arrays) -> sr-cunny-<variant>.onnx
  AnimeSR (Apache-2.0)   the released checkpoint AnimeSR_v2.pth (a PyTorch zip) -> sr-animesr-v2.onnx (one step of the
                         recurrent cell: prev / cur / next frame, the previous output and the hidden state)

The CuNNy and AnimeSR graphs keep their weights in the upstream files themselves: every initializer is ONNX external
data that points at the tensor's bytes inside the pinned file (offset, length from the pin file's tensor table). So
the model file stays a separate, replaceable component (CuNNy is LGPL-3.0): replace the weights file, rerun `index` and
`graphs`, and the graph follows. Nothing is converted on the user's PC, and no weights are copied into PushMPC's files.

Wrappers (one per I/O format, any model of the kind; written by make_graphs.py):
  sr_in_luma-<fmt>    picture -> x [1,1,h,w] (luma 0..1) + cbcr [1,2,..] (chroma -0.5..0.5, kept for the output)
  sr_in_rgb-<fmt>     picture -> x [1,3,h,w] RGB 0..1
  sr_out_luma-<fmt>   y [1,1,H,W] (the model's luma) + cbcr (bilinear to the new size) -> picture
  sr_out_rgb-<fmt>    y [1,3,H,W] -> picture
fmt: yuv    16-bit planes, Y full size, U/V half size (any bit depth, BT.601/709/2020 by `colour`, as make_graphs.py)
     rgba8  [1,h,w,4] uint8 (pushcore's encoded RGBA8 picture, BT.709 full range); the output is resized to the
            caller's size (sentinels outh / outw, replaced in the model bytes per session as make_graphs.py's sizes)

Usage:
  python -I anime_sr_graphs.py index --pins scripts/anime-models-pin.json --models DIR   (refresh the tensor tables)
  python -I anime_sr_graphs.py graphs --pins scripts/anime-models-pin.json --out DIR    (model graphs + wrappers)
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pickle
import struct
import zipfile
from pathlib import Path

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper

OPSET = 17
SENTINEL_OUT = {"outh": 0x504D4D4C00000005, "outw": 0x504D4D4C00000006}
# RGBA8 rows wider than the picture (D3D12 copies want 256-byte row pitches): the input is cropped to INW pixels, the
# output padded by OPAD pixels at the right
SENTINEL_PITCH = {"inw": 0x504D4D4C00000007, "opad": 0x504D4D4C00000008}
F32 = TensorProto.FLOAT


# ---------------------------------------------------------------------------------------------------------------
# weights: read safely (no code from the files runs), and locate every tensor's bytes inside the file

class _SafeUnpickler(pickle.Unpickler):
    """Only plain containers and numpy array reconstruction (CuNNy), or torch's tensor rebuild as a tuple (AnimeSR)."""
    _ok = {("numpy.core.multiarray", "_reconstruct"), ("numpy", "ndarray"), ("numpy", "dtype"),
           ("numpy.core.numeric", "_frombuffer"), ("collections", "OrderedDict")}

    def find_class(self, module, name):
        module = module.replace("numpy._core", "numpy.core")
        if module == "torch._utils" and name == "_rebuild_tensor_v2":
            return lambda storage, offset, size, stride, *a: ("tensor", storage, offset, tuple(size), tuple(stride))
        if module == "torch" and name == "FloatStorage":
            return "float32"
        if (module, name) not in self._ok:
            raise pickle.UnpicklingError(f"refused: {module}.{name}")
        return super().find_class(module, name)

    def persistent_load(self, pid):
        return pid   # ('storage', dtype, key, location, numel)


def _contiguous_stride(shape):
    s, out = 1, []
    for d in reversed(shape):
        out.append(s)
        s *= d
    return tuple(reversed(out))


def index_tensors(path: Path):
    """-> (meta, [(name, dtype, shape, offset, length)]) with offset/length = the tensor's raw little-endian bytes in
    the file (checked against the decoded values)."""
    data = path.read_bytes()
    table = []
    if zipfile.is_zipfile(path):   # PyTorch checkpoint: data.pkl + one stored (uncompressed) entry per storage
        z = zipfile.ZipFile(path)
        pkl = next(n for n in z.namelist() if n.endswith("/data.pkl"))
        root = pkl[: -len("data.pkl")]
        sd = _SafeUnpickler(z.open(pkl)).load()
        for name, t in sd.items():
            _, storage, off, shape, stride = t
            _, dtype, key, _, _ = storage
            if dtype != "float32" or stride != _contiguous_stride(shape):
                raise ValueError(f"{name}: only contiguous float32 tensors")
            info = z.getinfo(root + "data/" + key)
            if info.compress_type != zipfile.ZIP_STORED:
                raise ValueError(f"{name}: storage {key} is compressed")
            n, e = struct.unpack("<HH", data[info.header_offset + 26: info.header_offset + 30])
            start = info.header_offset + 30 + n + e + off * 4
            length = int(np.prod(shape)) * 4
            if data[start: start + length] != z.read(info)[off * 4: off * 4 + length]:
                raise ValueError(f"{name}: offset check failed")
            table.append((name, "float32", list(shape), start, length))
        return {}, table
    sd = _SafeUnpickler(open(path, "rb")).load()   # CuNNy: a dict of numpy arrays and settings
    meta = {k: v for k, v in sd.items() if not isinstance(v, np.ndarray)}
    for name, v in sd.items():
        if not isinstance(v, np.ndarray):
            continue
        if v.dtype != np.float32 or not v.flags["C_CONTIGUOUS"]:
            raise ValueError(f"{name}: only contiguous float32 arrays")
        raw = v.astype("<f4").tobytes()
        start = data.find(raw)
        if start < 0 or data.find(raw, start + 1) >= 0:
            raise ValueError(f"{name}: bytes not found exactly once")
        table.append((name, "float32", list(v.shape), start, len(raw)))
    return meta, table


def load_tensors(path: Path, table):
    """The tensors of a table from the weights file (offline checks)."""
    data = path.read_bytes()
    return {n: np.frombuffer(data, "<f4", int(np.prod(s)), off).reshape(s) for n, _, s, off, _ in table}


# ---------------------------------------------------------------------------------------------------------------
# graph helpers

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

    def external(self, name, shape, location, offset, length):
        """An initializer whose bytes stay in the upstream weights file."""
        t = TensorProto()
        t.name, t.data_type = name, F32
        t.dims.extend(shape)
        t.data_location = TensorProto.EXTERNAL
        for k, v in (("location", location), ("offset", str(offset)), ("length", str(length))):
            e = t.external_data.add()
            e.key, e.value = k, v
        self.inits.append(t)
        return name

    def i64(self, v):
        return self.const(v, np.int64)

    def model(self, check=True):
        g = helper.make_graph(self.nodes, self.name, self.inputs, self.outputs, self.inits)
        m = helper.make_model(g, opset_imports=[helper.make_opsetid("", OPSET)], producer_name="pmml")
        m.ir_version = 8
        if check:
            onnx.checker.check_model(m)
        return m


def _hw(g, x, a=2, b=4):
    return g.op("Slice", [g.op("Shape", [x]), g.i64([a]), g.i64([b])])


def _resize_to(g, x, hw, mode="linear"):
    """x [1,C,h,w] -> [1,C,hw] (hw = int64 [2])."""
    c = g.op("Slice", [g.op("Shape", [x]), g.i64([0]), g.i64([2])])
    return g.op("Resize", [x, "", "", g.op("Concat", [c, hw], axis=0)], mode=mode, coordinate_transformation_mode="half_pixel")


def _scale_hw(g, x, s):
    return g.op("Mul", [_hw(g, x), g.i64([s, s])])


# ---------------------------------------------------------------------------------------------------------------
# wrappers

# The YUV wrappers take and give the 16-bit codes as float32 (exact: integers below 2^24): TensorRT(-RTX) takes no
# uint16 at all, not even as network I/O ("Unsupported data type UINT16 for input node"), so uint16 planes sent the
# whole wrapper to the CPU EP. pmml converts the planes on the host (OrtUpscaler::run binds host memory there anyway).
def _yuv_inputs(g):
    for n, sub in (("in_y", False), ("in_u", True), ("in_v", True)):
        g.inputs.append(helper.make_tensor_value_info(n, F32, [1, 1, "pmml_ch" if sub else "pmml_h", "pmml_cw" if sub else "pmml_w"]))
    g.inputs.append(helper.make_tensor_value_info("colour", F32, [8]))


def _k(g, i):
    return g.op("Gather", ["colour", g.i64(i)])


def _yuv_norm(g):
    """16-bit codes (as float) -> y (0..1), cbcr (-0.5..0.5) at their own sizes."""
    inv = g.op("Reciprocal", [_k(g, 6)])
    y, u, v = (g.op("Mul", [n, inv]) for n in ("in_y", "in_u", "in_v"))
    y = g.op("Div", [g.op("Sub", [y, _k(g, 2)]), _k(g, 3)])
    uv = g.op("Div", [g.op("Sub", [g.op("Concat", [u, v], axis=1), _k(g, 4)]), _k(g, 5)])
    return y, uv


def _ycc_to_rgb(g, y, cbcr, kr, kb):
    one, two = g.const(1.0), g.const(2.0)
    cb = g.op("Slice", [cbcr, g.i64([0]), g.i64([1]), g.i64([1])])
    cr = g.op("Slice", [cbcr, g.i64([1]), g.i64([2]), g.i64([1])])
    r = g.op("Add", [y, g.op("Mul", [cr, g.op("Mul", [two, g.op("Sub", [one, kr])])])])
    b = g.op("Add", [y, g.op("Mul", [cb, g.op("Mul", [two, g.op("Sub", [one, kb])])])])
    kg = g.op("Sub", [g.op("Sub", [one, kr]), kb])
    gg = g.op("Div", [g.op("Sub", [g.op("Sub", [y, g.op("Mul", [kr, r])]), g.op("Mul", [kb, b])]), kg])
    return g.op("Concat", [r, gg, b], axis=1)


def _rgb_to_ycc(g, rgb, kr, kb):
    one, two = g.const(1.0), g.const(2.0)
    r, gg, b = (g.op("Slice", [rgb, g.i64([i]), g.i64([i + 1]), g.i64([1])]) for i in range(3))
    kg = g.op("Sub", [g.op("Sub", [one, kr]), kb])
    y = g.op("Add", [g.op("Add", [g.op("Mul", [kr, r]), g.op("Mul", [kg, gg])]), g.op("Mul", [kb, b])])
    cb = g.op("Div", [g.op("Sub", [b, y]), g.op("Mul", [two, g.op("Sub", [one, kb])])])
    cr = g.op("Div", [g.op("Sub", [r, y]), g.op("Mul", [two, g.op("Sub", [one, kr])])])
    return y, g.op("Concat", [cb, cr], axis=1)


def _code(g, x, mx, name):
    """0..1 -> the code 0..max, rounded, as float (pmml writes the 16-bit planes on the host)."""
    return g.op("Round", [g.op("Mul", [g.op("Clip", [x, g.const(0.0), g.const(1.0)]), mx])], out=name)


# The RGBA8 wrappers keep uint8 at the graph's edges only: one Cast right after in_rgba, one Cast that writes out_rgba.
# TensorRT(-RTX) takes uint8 only as network I/O ("does not support UINT8 types for intermediate tensors"), so any
# other uint8 node (a Slice, Transpose or Pad on the bytes) splits the graph and sends that part to the CPU EP, with
# a device <-> host copy of the picture each way. check_wrapper() enforces this.
def _rgba_in(g):
    g.inputs.append(helper.make_tensor_value_info("in_rgba", TensorProto.UINT8, [1, "pmml_h", "pmml_ipw", 4]))
    x = g.op("Cast", ["in_rgba"], to=F32)
    rows = g.op("Slice", [x, g.i64([0]), g.const([SENTINEL_PITCH["inw"]], np.int64, name="in_width"), g.i64([2])])
    x = g.op("Mul", [g.op("Transpose", [rows], perm=[0, 3, 1, 2]), g.const(1.0 / 255.0)])
    return g.op("Slice", [x, g.i64([0]), g.i64([3]), g.i64([1])])


CUBIC_A = -0.5


def _cubic_axis(g, x, axis, out_n, name):
    """Bicubic resampling of x [1,C,H,W] along `axis` (2 or 3) to out_n samples, as ONNX Resize mode cubic with
    half_pixel, cubic_coeff_a -0.5, exclude_outside 1 computes it, built from Gather / Mul / Add only. Resize's cubic
    mode ran on the CPU EP under TensorRT-RTX (and DirectML has no cubic resampling): ONNX Runtime's CPU kernel takes
    over 20 s for 1440x960 -> 3240x2160, while an exact 2x (1080p -> 2160p: scale 1) takes a fraction of a second.
    Taps and weights depend on the sizes only, so ONNX Runtime folds them when the session is created."""
    n_in = g.op("Slice", [g.op("Shape", [x]), g.i64([axis]), g.i64([axis + 1])])                   # int64 [1]
    m = g.const([out_n], np.int64, name=name)                                                       # sentinel
    pos = g.op("Cast", [g.op("Range", [g.i64(0), g.op("Squeeze", [m, g.i64([0])]), g.i64(1)])], to=F32)
    nf = g.op("Cast", [n_in], to=F32)
    ratio = g.op("Div", [nf, g.op("Cast", [m], to=F32)])
    src = g.op("Sub", [g.op("Mul", [g.op("Add", [pos, g.const(0.5)]), ratio]), g.const(0.5)])
    base = g.op("Floor", [src])
    t = g.op("Sub", [src, base])
    last = g.op("Squeeze", [g.op("Sub", [nf, g.const(1.0)]), g.i64([0])])
    a = CUBIC_A
    taps, weights = [], []
    for k in (-1, 0, 1, 2):
        d = g.op("Abs", [g.op("Sub", [t, g.const(float(k))])])                                     # 0..2
        d2 = g.op("Mul", [d, d])
        d3 = g.op("Mul", [d2, d])
        near = g.op("Add", [g.op("Sub", [g.op("Mul", [d3, g.const(a + 2)]), g.op("Mul", [d2, g.const(a + 3)])]), g.const(1.0)])
        far = g.op("Sub", [g.op("Add", [g.op("Sub", [g.op("Mul", [d3, g.const(a)]), g.op("Mul", [d2, g.const(5 * a)])]),
                                        g.op("Mul", [d, g.const(8 * a)])]), g.const(4 * a)])
        w = g.op("Where", [g.op("LessOrEqual", [d, g.const(1.0)]), near, far])
        idx = g.op("Add", [base, g.const(float(k))])
        inside = g.op("And", [g.op("GreaterOrEqual", [idx, g.const(0.0)]), g.op("LessOrEqual", [idx, last])])
        weights.append(g.op("Where", [inside, w, g.const(0.0)]))                                      # exclude_outside
        taps.append(g.op("Cast", [g.op("Clip", [idx, g.const(0.0), last])], to=TensorProto.INT64))
    total = g.op("Add", [g.op("Add", [weights[0], weights[1]]), g.op("Add", [weights[2], weights[3]])])
    shape = [-1] if axis == 3 else [-1, 1]
    acc = None
    for idx, w in zip(taps, weights):
        wn = g.op("Reshape", [g.op("Div", [w, total]), g.i64(shape)])
        term = g.op("Mul", [g.op("Gather", [x, idx], axis=axis), wn])
        acc = term if acc is None else g.op("Add", [acc, term])
    return acc


def _rgba_out(g, rgb):
    """RGB [1,3,H,W] -> bicubic to the caller's size -> out_rgba [1,oh,ow,4] uint8 (alpha 255)."""
    rgb = _cubic_axis(g, _cubic_axis(g, rgb, 3, SENTINEL_OUT["outw"], "out_w"), 2, SENTINEL_OUT["outh"], "out_h")
    shp = g.op("Shape", [rgb])
    alpha = g.op("Expand", [g.const(1.0), g.op("Concat", [g.i64([1, 1]), g.op("Slice", [shp, g.i64([2]), g.i64([4])])], axis=0)])
    rgba = g.op("Concat", [rgb, alpha], axis=1)
    q = g.op("Round", [g.op("Mul", [g.op("Clip", [rgba, g.const(0.0), g.const(1.0)]), g.const(255.0)])])
    pads = g.const([0, 0, 0, 0, 0, 0, SENTINEL_PITCH["opad"], 0], np.int64, name="out_pads")
    g.op("Cast", [g.op("Pad", [g.op("Transpose", [q], perm=[0, 2, 3, 1]), pads], mode="constant")], out="out_rgba", to=TensorProto.UINT8)
    g.outputs.append(helper.make_tensor_value_info("out_rgba", TensorProto.UINT8, [1, "pmml_oh", "pmml_opw", 4]))


def check_wrapper(m):
    """The rules the wrappers keep for TensorRT-RTX / DirectML (no GPU needed; write_wrappers runs it): no Resize in
    cubic mode, no 16-bit integer tensors anywhere (TensorRT-RTX refuses them even as I/O), and uint8 only at the edges
    (a graph input read by Casts only, a graph output written by a Cast)."""
    gr = m.graph
    for v in list(gr.input) + list(gr.output):
        if v.type.tensor_type.elem_type in (TensorProto.UINT16, TensorProto.INT16):
            raise ValueError(f"{gr.name}: {v.name} is a 16-bit integer tensor (TensorRT-RTX refuses it)")
    u8_in = {i.name for i in gr.input if i.type.tensor_type.elem_type == TensorProto.UINT8}
    u8_out = {o.name for o in gr.output if o.type.tensor_type.elem_type == TensorProto.UINT8}
    for n in gr.node:
        attrs = {a.name: helper.get_attribute_value(a) for a in n.attribute}
        if n.op_type == "Resize" and attrs.get("mode", b"nearest") in (b"cubic", "cubic"):
            raise ValueError(f"{gr.name}: {n.name} is a cubic Resize (CPU EP under TensorRT-RTX / DirectML)")
        if any(i in u8_in for i in n.input) and n.op_type != "Cast":
            raise ValueError(f"{gr.name}: {n.name} ({n.op_type}) reads a uint8 input; only a Cast may")
        if n.op_type == "Cast" and attrs.get("to") == TensorProto.UINT8 and not all(o in u8_out for o in n.output):
            raise ValueError(f"{gr.name}: {n.name} makes an intermediate uint8 tensor")
        if n.op_type == "Cast" and attrs.get("to") in (TensorProto.UINT16, TensorProto.INT16):
            raise ValueError(f"{gr.name}: {n.name} makes a 16-bit integer tensor")
    return m


def check_cubic():
    """_cubic_axis against ONNX Runtime's own Resize (cubic, half_pixel, a -0.5, exclude_outside 1) on the CPU EP, up
    and down, at small sizes (skipped without onnxruntime). Returns the largest difference."""
    try:
        import onnxruntime as ort
    except ImportError:
        return None
    worst = 0.0
    rng = np.random.default_rng(7)
    for h, w, oh, ow in ((20, 36, 45, 81), (48, 30, 21, 13), (16, 16, 16, 16), (9, 11, 40, 7)):
        g = G("pmml_check_cubic")
        g.inputs.append(helper.make_tensor_value_info("x", F32, [1, 3, h, w]))
        g.op("Identity", [_cubic_axis(g, _cubic_axis(g, "x", 3, ow, "cw"), 2, oh, "ch")], out="mine")
        g.op("Resize", ["x", "", "", g.i64([1, 3, oh, ow])], out="ref", mode="cubic", coordinate_transformation_mode="half_pixel",
             cubic_coeff_a=CUBIC_A, exclude_outside=1)
        g.outputs += [helper.make_tensor_value_info(n, F32, [1, 3, oh, ow]) for n in ("mine", "ref")]
        s = ort.InferenceSession(g.model().SerializeToString(), providers=["CPUExecutionProvider"])
        mine, ref = s.run(None, {"x": rng.random((1, 3, h, w), dtype=np.float32)})
        if mine.shape != ref.shape:
            raise ValueError(f"cubic check: shape {mine.shape} vs {ref.shape}")
        worst = max(worst, float(np.abs(mine - ref).max()))
    if worst > 1e-4:
        raise ValueError(f"cubic check: differs from ONNX Resize by {worst}")
    return worst


BT709 =(0.2126, 0.0722)


def sr_in(kind, fmt):
    g = G(f"pmml_sr_in_{kind}_{fmt}")
    if fmt == "yuv":
        _yuv_inputs(g)
        y, cbcr = _yuv_norm(g)
        if kind == "luma":
            g.op("Identity", [y], out="x")
            g.op("Identity", [cbcr], out="cbcr")
        else:
            g.op("Identity", [_ycc_to_rgb(g, y, _resize_to(g, cbcr, _hw(g, y)), _k(g, 0), _k(g, 1))], out="x")
    else:
        rgb = _rgba_in(g)
        if kind == "luma":
            y, cbcr = _rgb_to_ycc(g, rgb, g.const(BT709[0]), g.const(BT709[1]))
            g.op("Identity", [y], out="x")
            g.op("Identity", [cbcr], out="cbcr")
        else:
            g.op("Identity", [rgb], out="x")
    g.outputs.append(helper.make_tensor_value_info("x", F32, [1, 1 if kind == "luma" else 3, "pmml_h", "pmml_w"]))
    if kind == "luma":
        g.outputs.append(helper.make_tensor_value_info("cbcr", F32, [1, 2, "pmml_ch" if fmt == "yuv" else "pmml_h", "pmml_cw" if fmt == "yuv" else "pmml_w"]))
    return g.model()


def sr_out(kind, fmt):
    g = G(f"pmml_sr_out_{kind}_{fmt}")
    g.inputs.append(helper.make_tensor_value_info("y", F32, [1, 1 if kind == "luma" else 3, "pmml_oh2", "pmml_ow2"]))
    if kind == "luma":
        g.inputs.append(helper.make_tensor_value_info("cbcr", F32, [1, 2, "pmml_ch", "pmml_cw"]))
    if fmt == "yuv":
        g.inputs.append(helper.make_tensor_value_info("colour", F32, [8]))
        mx = _k(g, 6)
        if kind == "luma":
            y = "y"
            cbcr = _resize_to(g, "cbcr", g.op("Div", [_hw(g, "y"), g.i64([2, 2])]))
        else:
            y, cbcr = _rgb_to_ycc(g, "y", _k(g, 0), _k(g, 1))
            cbcr = g.op("AveragePool", [cbcr], kernel_shape=[2, 2], strides=[2, 2], pads=[0, 0, 0, 0])
        _code(g, g.op("Add", [g.op("Mul", [y, _k(g, 3)]), _k(g, 2)]), mx, "out_y")
        uv = g.op("Add", [g.op("Mul", [cbcr, _k(g, 5)]), _k(g, 4)])
        _code(g, g.op("Slice", [uv, g.i64([0]), g.i64([1]), g.i64([1])]), mx, "out_u")
        _code(g, g.op("Slice", [uv, g.i64([1]), g.i64([2]), g.i64([1])]), mx, "out_v")
        for n, sub in (("out_y", False), ("out_u", True), ("out_v", True)):
            g.outputs.append(helper.make_tensor_value_info(n, F32, [1, 1, "pmml_oh2c" if sub else "pmml_oh2", "pmml_ow2c" if sub else "pmml_ow2"]))
    else:
        if kind == "luma":
            rgb = _ycc_to_rgb(g, "y", _resize_to(g, "cbcr", _hw(g, "y")), g.const(BT709[0]), g.const(BT709[1]))
        else:
            rgb = "y"
        _rgba_out(g, rgb)
    return g.model()


# ---------------------------------------------------------------------------------------------------------------
# model graphs (weights as external data in the upstream file)

def _conv(g, x, w, b=None, stride=1):
    ins = [x, w] + ([b] if b else [])
    return g.op("Conv", ins, kernel_shape=[3, 3], pads=[1, 1, 1, 1], strides=[stride, stride])


def cunny_graph(model):
    """CuNNy (train.py Net): cin (+bias) -> act -> conv... (no bias) -> act -> cout (+bias) -> pixel shuffle 2x ->
    + bilinear 2x of the input. act = clamp(0, 1) + 0.01 relu(x - 1) for the quantised models, else relu."""
    loc, t = model["file"], {n: (s, off, ln) for n, _, s, off, ln in model["tensors"]}
    ch = 3 if model["arch"]["rgb"] else 1
    g = G(f"pmml_{model['id']}")
    g.inputs.append(helper.make_tensor_value_info("x", F32, [1, ch, "H", "W"]))
    ext = lambda n: g.external(n, t[n][0], loc, t[n][1], t[n][2])  # noqa: E731

    def act(x):
        if not model["arch"]["quant"]:
            return g.op("Relu", [x])
        return g.op("Add", [g.op("Clip", [x, g.const(0.0), g.const(1.0)]),
                            g.op("Mul", [g.op("Relu", [g.op("Sub", [x, g.const(1.0)])]), g.const(0.01)])])
    h = act(_conv(g, "x", ext("cin.weight"), ext("cin.bias")))
    i = 0
    while f"conv.{i}.weight" in t:
        h = act(_conv(g, h, ext(f"conv.{i}.weight")))
        i += 1
    h = _conv(g, h, ext("cout.weight"), ext("cout.bias"))
    h = g.op("DepthToSpace", [h], blocksize=2, mode="CRD")
    base = g.op("Resize", ["x", "", g.const([1, 1, 2, 2], np.float32)], mode="linear", coordinate_transformation_mode="half_pixel")
    g.op("Add", [h, base], out="y")
    g.outputs.append(helper.make_tensor_value_info("y", F32, [1, ch, "H2", "W2"]))
    return g.model(check=False)


def animesr_graph(model):
    """One step of AnimeSR's MSRSWVSR (vsr_arch.py, num_feat 64, blocks (5, 3, 2), netscale 4). Inputs prev / cur /
    next [1,3,H,W] RGB 0..1, fb = the previous output [1,3,4H,4W], state [1,64,H,W]; outputs y = this frame 4x and
    state_out. Any H, W: the upsampled s2 / s4 branches are cropped to the s1 size (the reference crops the input to
    a multiple of 4 instead)."""
    loc, t = model["file"], {n: (s, off, ln) for n, _, s, off, ln in model["tensors"]}
    g = G(f"pmml_{model['id']}")
    for n in ("prev", "cur", "next"):
        g.inputs.append(helper.make_tensor_value_info(n, F32, [1, 3, "H", "W"]))
    g.inputs += [helper.make_tensor_value_info("fb", F32, [1, 3, "H4", "W4"]),
                 helper.make_tensor_value_info("state", F32, [1, 64, "H", "W"])]
    p = "recurrent_cell."
    ext = lambda n: g.external(n, t[p + n][0], loc, t[p + n][1], t[p + n][2])  # noqa: E731
    lrelu = lambda x: g.op("LeakyRelu", [x], alpha=0.1)  # noqa: E731
    hw = _hw(g, "cur")
    # pixel_unshuffle(fb, 4): [1,3,4H,4W] -> [1,48,H,W], channel = c * 16 + i * 4 + j (torch order)
    shp = g.op("Concat", [g.i64([1, 3]), g.op("Slice", [hw, g.i64([0]), g.i64([1])]), g.i64([4]),
                          g.op("Slice", [hw, g.i64([1]), g.i64([2])]), g.i64([4])], axis=0)
    u = g.op("Transpose", [g.op("Reshape", ["fb", shp])], perm=[0, 1, 3, 5, 2, 4])
    u = g.op("Reshape", [u, g.op("Concat", [g.i64([1, 48]), hw], axis=0)])
    x = g.op("Concat", ["prev", "cur", "next", u, "state"], axis=1)

    def rb(x, n):
        return g.op("Add", [x, _conv(g, g.op("Relu", [_conv(g, x, ext(n + ".conv1.weight"), ext(n + ".conv1.bias"))]),
                                     ext(n + ".conv2.weight"), ext(n + ".conv2.bias"))])

    def up(x, s, like):
        r = g.op("Resize", [x, "", g.const([1, 1, s, s], np.float32)], mode="linear", coordinate_transformation_mode="half_pixel")
        return g.op("Slice", [r, g.i64([0, 0]), _hw(g, like), g.i64([2, 3])])
    s1 = lrelu(_conv(g, x, ext("conv_s1_first.0.weight"), ext("conv_s1_first.0.bias")))
    s2 = lrelu(_conv(g, s1, ext("conv_s2_first.0.weight"), ext("conv_s2_first.0.bias"), stride=2))
    s4 = lrelu(_conv(g, s2, ext("conv_s4_first.0.weight"), ext("conv_s4_first.0.bias"), stride=2))
    nb = (5, 3, 2)
    f2 = f4 = False
    for i in range(nb[0]):
        a = s1
        if f2:
            a = g.op("Add", [a, up(s2, 2, s1)])
        if f4:
            a = g.op("Add", [a, up(s4, 4, s1)])
        s1 = rb(a, f"body_s1_first.{i}")
        if i >= nb[0] - nb[1]:
            s2 = rb(g.op("Add", [s2, up(s4, 2, s2)]) if f4 else s2, f"body_s2_first.{i - nb[0] + nb[1]}")
            f2 = True
        if i >= nb[0] - nb[2]:
            s4 = rb(s4, f"body_s4_first.{i - nb[0] + nb[2]}")
            f4 = True
    fu = g.op("Concat", [s1, up(s2, 2, s1), up(s4, 4, s1)], axis=1)
    fu = lrelu(_conv(g, fu, ext("fusion.0.weight"), ext("fusion.0.bias")))
    out = _conv(g, fu, ext("fusion.2.weight"), ext("fusion.2.bias"))
    img = g.op("DepthToSpace", [g.op("Slice", [out, g.i64([0]), g.i64([48]), g.i64([1])])], blocksize=4, mode="CRD")
    base = g.op("Resize", ["cur", "", g.const([1, 1, 4, 4], np.float32)], mode="linear", coordinate_transformation_mode="half_pixel")
    g.op("Add", [img, base], out="y")
    g.op("LeakyRelu", [g.op("Slice", [out, g.i64([48]), g.i64([112]), g.i64([1])])], out="state_out", alpha=0.1)
    g.outputs += [helper.make_tensor_value_info("y", F32, [1, 3, "H4", "W4"]),
                  helper.make_tensor_value_info("state_out", F32, [1, 64, "H", "W"])]
    return g.model(check=False)


# ---------------------------------------------------------------------------------------------------------------

def write_wrappers(out: Path):
    worst = check_cubic()
    print("cubic check: skipped (no onnxruntime)" if worst is None else f"cubic check: max difference to ONNX Resize {worst:.2e}")
    for kind in ("luma", "rgb"):
        for fmt in ("yuv", "rgba8"):
            for name, make in ((f"sr_in_{kind}-{fmt}.onnx", sr_in), (f"sr_out_{kind}-{fmt}.onnx", sr_out)):
                onnx.save(check_wrapper(make(kind, fmt)), str(out / name))
                print(f"wrote {out / name}")


def write_models(pins: dict, out: Path):
    out.mkdir(parents=True, exist_ok=True)
    for m in pins["models"]:
        make = {"cunny": cunny_graph, "animesr": animesr_graph}.get(m["family"])
        if not make or m["graph"] == m["file"]:
            continue
        onnx.save(make(m), str(out / m["graph"]))
        print(f"wrote {out / m['graph']}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=("index", "graphs"))
    ap.add_argument("--pins", required=True)
    ap.add_argument("--models", help="index: the folder with the fetched weights")
    ap.add_argument("--out", help="graphs: the graphs folder (model graphs go to <out>/anime-sr)")
    a = ap.parse_args()
    pins_path = Path(a.pins)
    pins = json.loads(pins_path.read_text(encoding="utf-8"))
    if a.cmd == "index":
        files = {f["name"]: f for f in pins["files"]}
        for m in pins["models"]:
            p = Path(a.models) / m["file"]
            f = files[m["file"]]
            if p.stat().st_size != f["size"] or hashlib.sha256(p.read_bytes()).hexdigest() != f["sha256"]:
                raise SystemExit(f"{p}: size or SHA-256 differs from the pin")
            if m["family"] in ("cunny", "animesr"):
                meta, table = index_tensors(p)
                m["tensors"] = [list(r) for r in table]
                if m["family"] == "cunny":
                    m["arch"] = {"rgb": bool(meta["rgb"]), "quant": bool(meta["quant"]), "layers": list(meta["layers"])}
                    m["kind"] = "rgb" if meta["rgb"] else "luma"
        text = json.dumps(pins, indent=1)
        pins_path.write_text(text + "\n", encoding="utf-8", newline="\n")
        print(f"updated {pins_path}")
    else:
        out = Path(a.out)
        out.mkdir(parents=True, exist_ok=True)
        write_wrappers(out)
        write_models(pins, out / "anime-sr")


if __name__ == "__main__":
    main()
