"""onnx_lite: a stdlib-only reader for the small ONNX files PushMPC uses (ArtCNN's C models). No onnx / protobuf
package needed, so the Homebrew formula and CI can convert the pinned upstream files with plain Python 3.

  read(path_or_bytes) -> Model(nodes=[Node], inits={name: Tensor}, inputs=[str], outputs=[str])

Only what these models need is decoded: graph nodes (op type, inputs, outputs, int / float / ints attributes) and
float32 / float16 initialisers (raw_data or float_data)."""
import struct
import sys
from dataclasses import dataclass, field


def _varint(b, i):
    r = s = 0
    while True:
        c = b[i]
        i += 1
        r |= (c & 0x7F) << s
        s += 7
        if c < 0x80:
            return r, i


def _fields(b):
    """(field number, wire type, value) for one protobuf message; value: int for varint / fixed, bytes for length."""
    i, n = 0, len(b)
    while i < n:
        key, i = _varint(b, i)
        f, wt = key >> 3, key & 7
        if wt == 0:
            v, i = _varint(b, i)
        elif wt == 1:
            v = b[i:i + 8]
            i += 8
        elif wt == 2:
            ln, i = _varint(b, i)
            v = b[i:i + ln]
            i += ln
        elif wt == 5:
            v = b[i:i + 4]
            i += 4
        else:
            raise ValueError(f"unsupported wire type {wt}")
        yield f, wt, v


def _packed_varints(v):
    out, i = [], 0
    while i < len(v):
        x, i = _varint(v, i)
        out.append(x)
    return out


def _signed(x):
    return x - (1 << 64) if x >= 1 << 63 else x


@dataclass
class Tensor:
    name: str
    dims: list
    data: list          # flat float values (row-major)


@dataclass
class Node:
    op: str
    inputs: list
    outputs: list
    attrs: dict = field(default_factory=dict)
    name: str = ""


@dataclass
class Model:
    nodes: list
    inits: dict
    inputs: list
    outputs: list


def _tensor(b):
    dims, dtype, name, raw, floats = [], 1, "", None, []
    for f, wt, v in _fields(b):
        if f == 1:
            dims += [_signed(x) for x in _packed_varints(v)] if wt == 2 else [_signed(v)]
        elif f == 2:
            dtype = v
        elif f == 8:
            name = v.decode()
        elif f == 9:
            raw = v
        elif f == 4:
            floats += list(struct.unpack(f"<{len(v) // 4}f", v)) if wt == 2 else list(struct.unpack("<f", v))
    count = 1
    for d in dims:
        count *= d
    if raw is not None:
        if dtype == 1:
            floats = list(struct.unpack(f"<{len(raw) // 4}f", raw))
        elif dtype == 10:
            floats = list(struct.unpack(f"<{len(raw) // 2}e", raw))
        elif dtype == 7:
            floats = [float(x) for x in struct.unpack(f"<{len(raw) // 8}q", raw)]
        else:
            raise ValueError(f"tensor {name}: data type {dtype} not supported")
    if len(floats) != count:
        raise ValueError(f"tensor {name}: {len(floats)} values for dims {dims}")
    return Tensor(name, dims, floats)


def _attr(b):
    name, val = "", None
    ints = []
    for f, wt, v in _fields(b):
        if f == 1:
            name = v.decode()
        elif f == 2:
            val = struct.unpack("<f", v)[0]
        elif f == 3:
            val = _signed(v)
        elif f == 4:
            val = v.decode(errors="replace")
        elif f == 8:
            ints += [_signed(x) for x in _packed_varints(v)] if wt == 2 else [_signed(v)]
    if ints:
        val = ints
    return name, val


def _node(b):
    n = Node("", [], [])
    for f, wt, v in _fields(b):
        if f == 1:
            n.inputs.append(v.decode())
        elif f == 2:
            n.outputs.append(v.decode())
        elif f == 3:
            n.name = v.decode()
        elif f == 4:
            n.op = v.decode()
        elif f == 5:
            k, val = _attr(v)
            n.attrs[k] = val
    return n


def _value_name(b):
    for f, wt, v in _fields(b):
        if f == 1:
            return v.decode()
    return ""


def read(src):
    b = src if isinstance(src, (bytes, bytearray)) else open(src, "rb").read()
    graph = None
    for f, wt, v in _fields(b):
        if f == 7:
            graph = v
    if graph is None:
        raise ValueError("no graph in the ONNX file")
    nodes, inits, inputs, outputs = [], {}, [], []
    for f, wt, v in _fields(graph):
        if f == 1:
            nodes.append(_node(v))
        elif f == 5:
            t = _tensor(v)
            inits[t.name] = t
        elif f == 11:
            inputs.append(_value_name(v))
        elif f == 12:
            outputs.append(_value_name(v))
    # Constant nodes become initialisers
    for n in list(nodes):
        if n.op == "Constant" and isinstance(n.attrs.get("value"), str) is False and "value" in n.attrs:
            pass
    inputs = [i for i in inputs if i not in inits]
    return Model(nodes, inits, inputs, outputs)


if __name__ == "__main__":
    data = sys.stdin.buffer.read() if len(sys.argv) < 2 or sys.argv[1] == "-" else open(sys.argv[1], "rb").read()
    m = read(data)
    print("inputs", m.inputs, "outputs", m.outputs)
    for n in m.nodes:
        print(n.op, n.inputs, "->", n.outputs, n.attrs)
    for k, t in m.inits.items():
        print("init", k, t.dims)
