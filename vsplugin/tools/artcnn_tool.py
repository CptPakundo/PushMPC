#!/usr/bin/env python3
"""artcnn_tool: ArtCNN (MIT, github.com/Artoriuz/ArtCNN) for PushMPC. Python 3 standard library only.

  artcnn_tool.py fetch --out DIR [--lock vsplugin/models/artcnn.lock.json] [--only NAME ...]
        fetch the pinned upstream files (immutable commit URLs) into DIR and check each SHA-256; a file already in DIR
        with the right hash is kept (CI cache)
  artcnn_tool.py pmnn MODEL.onnx OUT.pmnn
        the weights for PushMPC's own engines (Metal in the apple backend, the C++ reference in apple_nn.h)
  artcnn_tool.py glsl MODEL.onnx OUT.glsl [--name "ArtCNN C4F16"]
        mpv user shader made of FRAGMENT passes only (no //!COMPUTE): runs on OpenGL 4.1 (IINA) and every mpv renderer.
        Each 3x3 convolution of F channels is ceil(F/4) passes of 4 channels (rgba16f), each reading every input
        texture at 9 taps; the last pass does the depth-to-space (x2) and the clip.
  artcnn_tool.py describe MODEL.onnx

Supported graphs: ArtCNN's "C" family (C4F16, C4F32 and their DS / DN variants): Conv(1->F), (Conv + Relu) x N,
Conv, Add(long skip from the first Conv), Conv(F->4), DepthToSpace(2, DCR), Clip. Anything else is refused.

.pmnn layout (little endian): b"PMNN", u32 version 1, u32 layer count, then per layer: u32 in, u32 out, u32 flags
(1 relu, 2 add the saved skip after the bias, 4 save this layer's output as the skip, 8 final: depth-to-space x2 and
clip to [lo, hi]), f32 lo, f32 hi, f32 weights[out][in][3][3], f32 bias[out]. Convolutions pad with zeros (ONNX
SAME_UPPER)."""
import argparse
import hashlib
import json
import os
import struct
import sys
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import onnx_lite  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_LOCK = os.path.join(os.path.dirname(HERE), "models", "artcnn.lock.json")
RELU, ADD_SKIP, SAVE_SKIP, FINAL = 1, 2, 4, 8


class Layer:
    def __init__(self, w, b, cin, cout):
        self.w, self.b, self.cin, self.cout = w, b, cin, cout
        self.flags, self.lo, self.hi = 0, 0.0, 1.0


def layers_from_onnx(path):
    m = onnx_lite.read(sys.stdin.buffer.read() if path == "-" else path)
    producer = {o: n for n in m.nodes for o in n.outputs}
    layers, by_output = [], {}
    for n in m.nodes:
        if n.op == "Conv":
            w = m.inits[n.inputs[1]]
            b = m.inits[n.inputs[2]] if len(n.inputs) > 2 else onnx_lite.Tensor("", [w.dims[0]], [0.0] * w.dims[0])
            if w.dims[2:] != [3, 3] or n.attrs.get("group", 1) != 1 or n.attrs.get("strides", [1, 1]) != [1, 1] or n.attrs.get("dilations", [1, 1]) != [1, 1]:
                raise SystemExit(f"unsupported Conv {n.outputs[0]}: {w.dims} {n.attrs}")
            pad = n.attrs.get("auto_pad", "")
            if pad not in ("SAME_UPPER", "SAME_LOWER") and n.attrs.get("pads", [1, 1, 1, 1]) != [1, 1, 1, 1]:
                raise SystemExit(f"unsupported padding on {n.outputs[0]}: {n.attrs}")
            L = Layer(w.data, b.data, w.dims[1], w.dims[0])
            if layers and layers[-1].cout != L.cin:
                raise SystemExit("layers do not chain")
            src = n.inputs[0]
            if src not in m.inputs and src not in by_output:
                raise SystemExit(f"Conv {n.outputs[0]} reads {src}, which is not the previous layer")
            layers.append(L)
            by_output[n.outputs[0]] = L
        elif n.op == "Relu":
            L = by_output.get(n.inputs[0])
            if L is None or L is not layers[-1]:
                raise SystemExit("Relu not right after a Conv")
            L.flags |= RELU
            by_output[n.outputs[0]] = L
        elif n.op == "Add":
            a, b = (by_output.get(x) for x in n.inputs)
            last = layers[-1]
            skip = b if a is last else a if b is last else None
            if skip is None or skip is last or (a is not last and b is not last):
                raise SystemExit("Add is not a long skip onto the last layer")
            if skip.flags & RELU:
                raise SystemExit("the skip source has a Relu (not an ArtCNN C graph)")
            skip.flags |= SAVE_SKIP
            last.flags |= ADD_SKIP
            by_output[n.outputs[0]] = last
        elif n.op == "DepthToSpace":
            if n.attrs.get("blocksize") != 2 or n.attrs.get("mode", "DCR") != "DCR" or layers[-1].cout != 4:
                raise SystemExit(f"unsupported DepthToSpace {n.attrs}")
            layers[-1].flags |= FINAL
            by_output[n.outputs[0]] = layers[-1]
        elif n.op == "Clip":
            lo = m.inits[n.inputs[1]].data[0] if len(n.inputs) > 1 and n.inputs[1] else n.attrs.get("min", 0.0)
            hi = m.inits[n.inputs[2]].data[0] if len(n.inputs) > 2 and n.inputs[2] else n.attrs.get("max", 1.0)
            layers[-1].lo, layers[-1].hi = float(lo), float(hi)
            by_output[n.outputs[0]] = layers[-1]
        else:
            raise SystemExit(f"unsupported op {n.op}")
    if not layers or not layers[-1].flags & FINAL or layers[0].cin != 1:
        raise SystemExit("not an ArtCNN C luma doubler")
    if sum(1 for L in layers if L.flags & SAVE_SKIP) > 1:
        raise SystemExit("more than one skip")
    _ = producer
    return layers


def write_pmnn(layers, out):
    with open(out, "wb") as f:
        f.write(b"PMNN" + struct.pack("<II", 1, len(layers)))
        for L in layers:
            f.write(struct.pack("<IIIff", L.cin, L.cout, L.flags, L.lo, L.hi))
            f.write(struct.pack(f"<{len(L.w)}f", *L.w))
            f.write(struct.pack(f"<{len(L.b)}f", *L.b))


def g(x):
    s = f"{x:.9g}"
    return s if any(c in s for c in ".en") else s + ".0"


TAPS = [(dx, dy) for dy in (-1, 0, 1) for dx in (-1, 0, 1)]


def write_glsl(layers, out, name):
    """fragment passes; tensor i part p is texture t{i}_{p} (4 channels)"""
    when = "//!WHEN OUTPUT.w LUMA.w / 1.3 > OUTPUT.h LUMA.h / 1.3 > *"
    lic = ("// " + name + " as mpv FRAGMENT passes (no compute shaders: works on OpenGL 4.1, e.g. IINA), generated by PushMPC's\n"
           "// vsplugin/tools/artcnn_tool.py from the upstream ONNX weights. ArtCNN: https://github.com/Artoriuz/ArtCNN\n"
           "// MIT License. Copyright (c) 2024 Joao Chrisostomo, Kacper Michajlow.\n"
           "// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated\n"
           "// documentation files (the \"Software\"), to deal in the Software without restriction, including without limitation\n"
           "// the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and\n"
           "// to permit persons to whom the Software is furnished to do so, subject to the following conditions: The above\n"
           "// copyright notice and this permission notice shall be included in all copies or substantial portions of the\n"
           "// Software. THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT\n"
           "// LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT\n"
           "// SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION\n"
           "// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER\n"
           "// DEALINGS IN THE SOFTWARE.\n\n")
    o = [lic]
    skip_idx = next((i for i, L in enumerate(layers) if L.flags & SAVE_SKIP), -1)
    npasses = 0
    for i, L in enumerate(layers):
        parts_in = (L.cin + 3) // 4
        if L.flags & FINAL:
            # 4 channels at the source size (ONE pass), then depth-to-space at 2x
            pass
        for p in range((L.cout + 3) // 4):
            npasses += 1
            o.append(f"//!DESC {name} (fragment) conv {i} part {p}\n//!HOOK LUMA\n")
            if i == 0:
                o.append("//!BIND LUMA\n")
            else:
                for q in range(parts_in):
                    o.append(f"//!BIND t{i - 1}_{q}\n")
            if L.flags & ADD_SKIP:
                o.append(f"//!BIND t{skip_idx}_{p}\n")
            o.append(f"//!SAVE t{i}_{p}\n//!WIDTH LUMA.w\n//!HEIGHT LUMA.h\n//!COMPONENTS 4\n{when}\n")
            o.append("vec4 hook() {\n")
            bias = [L.b[4 * p + r] if 4 * p + r < L.cout else 0.0 for r in range(4)]
            o.append(f"    vec4 r = vec4({', '.join(g(x) for x in bias)});\n")
            for dx, dy in TAPS:
                kx, ky = dx + 1, dy + 1
                if i == 0:
                    w = [L.w[((4 * p + r) * L.cin + 0) * 9 + ky * 3 + kx] if 4 * p + r < L.cout else 0.0 for r in range(4)]
                    o.append(f"    r += vec4({', '.join(g(x) for x in w)}) * (LUMA_mul * LUMA_texOff(vec2({dx}.0, {dy}.0)).x);\n")
                    continue
                for q in range(parts_in):
                    # GLSL mat4 is column-major: column c = input channel 4q+c, row r = output channel 4p+r
                    vals = []
                    for c in range(4):
                        for r in range(4):
                            oc, ic = 4 * p + r, 4 * q + c
                            vals.append(L.w[(oc * L.cin + ic) * 9 + ky * 3 + kx] if oc < L.cout and ic < L.cin else 0.0)
                    o.append(f"    r += mat4({', '.join(g(x) for x in vals)}) * t{i - 1}_{q}_texOff(vec2({dx}.0, {dy}.0));\n")
            if L.flags & ADD_SKIP:
                o.append(f"    r += t{skip_idx}_{p}_tex(t{skip_idx}_{p}_pos);\n")
            if L.flags & RELU:
                o.append("    r = max(r, vec4(0.0));\n")
            o.append("    return r;\n}\n\n")
    last = len(layers) - 1
    L = layers[-1]
    o.append(f"//!DESC {name} (fragment) depth-to-space x2\n//!HOOK LUMA\n//!BIND t{last}_0\n//!WIDTH LUMA.w 2.0 *\n//!HEIGHT LUMA.h 2.0 *\n"
             f"//!COMPONENTS 1\n{when}\n"
             "vec4 hook() {\n"
             "    // DCR: channel (row parity) * 2 + (column parity)\n"
             f"    vec2 src = floor(HOOKED_pos * t{last}_0_size);\n"
             f"    vec4 c = t{last}_0_tex((src + vec2(0.5)) * t{last}_0_pt);\n"
             f"    ivec2 par = ivec2(floor(HOOKED_pos * t{last}_0_size * 2.0)) - ivec2(src) * 2;\n"
             "    float v = par.y == 0 ? (par.x == 0 ? c.x : c.y) : (par.x == 0 ? c.z : c.w);\n"
             f"    return vec4(clamp(v, {g(L.lo)}, {g(L.hi)}), 0.0, 0.0, 1.0);\n"
             "}\n")
    with open(out, "w", newline="\n") as f:
        f.write("".join(o))
    return npasses + 1


def fetch(lock, out, only):
    j = json.load(open(lock, encoding="utf-8"))
    os.makedirs(out, exist_ok=True)
    for e in j["files"]:
        if only and e["name"] not in only:
            continue
        dst = os.path.join(out, e["name"])
        if os.path.isfile(dst) and hashlib.sha256(open(dst, "rb").read()).hexdigest().upper() == e["sha256"].upper():
            print(f"{e['name']}: cached, SHA-256 ok")
            continue
        url = f"{j['repository'].replace('github.com', 'raw.githubusercontent.com')}/{j['commit']}/{e['path']}"
        data = urllib.request.urlopen(url, timeout=60).read()
        got = hashlib.sha256(data).hexdigest().upper()
        if got != e["sha256"].upper():
            raise SystemExit(f"{e['name']}: SHA-256 {got} != pinned {e['sha256']}")
        blob = hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()
        if blob != e["gitBlobSha1"]:
            raise SystemExit(f"{e['name']}: git blob {blob} != pinned {e['gitBlobSha1']}")
        open(dst, "wb").write(data)
        print(f"{e['name']}: fetched {len(data)} bytes, SHA-256 ok")


def main():
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest="cmd", required=True)
    f = sp.add_parser("fetch")
    f.add_argument("--out", required=True)
    f.add_argument("--lock", default=DEFAULT_LOCK)
    f.add_argument("--only", nargs="*")
    p = sp.add_parser("pmnn")
    p.add_argument("onnx")
    p.add_argument("out")
    gl = sp.add_parser("glsl")
    gl.add_argument("onnx")
    gl.add_argument("out")
    gl.add_argument("--name", default="")
    d = sp.add_parser("describe")
    d.add_argument("onnx")
    a = ap.parse_args()
    if a.cmd == "fetch":
        fetch(a.lock, a.out, a.only)
        return
    layers = layers_from_onnx(a.onnx)
    if a.cmd == "pmnn":
        write_pmnn(layers, a.out)
        print(f"{a.out}: {len(layers)} layers, {sum(len(L.w) + len(L.b) for L in layers)} parameters")
    elif a.cmd == "glsl":
        name = a.name or os.path.splitext(os.path.basename(a.onnx))[0].replace("_", " ")
        n = write_glsl(layers, a.out, name)
        src = open(a.out).read()
        if "//!COMPUTE" in src:
            raise SystemExit("internal: a compute pass was generated")
        print(f"{a.out}: {n} fragment passes")
    else:
        for i, L in enumerate(layers):
            print(f"layer {i}: {L.cin} -> {L.cout}, flags {L.flags}, clip [{L.lo}, {L.hi}]")


if __name__ == "__main__":
    main()
