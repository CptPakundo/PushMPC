"""ArtCNN parity (CI, ubuntu): onnxruntime on the pinned upstream ONNX file against PushMPC's float reference
(apple_nn.h through build/artcnn_ref, from the .pmnn that tools/artcnn_tool.py wrote). Needs numpy and onnxruntime.
  artcnn_parity.py <model.onnx> <model.pmnn> <artcnn_ref executable>
PSNR (8 px border excluded) must be >= 60 dB: both are float32 evaluations of the same network."""
import os
import subprocess
import sys
import tempfile

import numpy as np
import onnxruntime as ort

onnx_path, pmnn, ref = sys.argv[1:4]
rng = np.random.default_rng(3)
h, w = 72, 128
yy, xx = np.mgrid[0:h, 0:w]
img = (0.25 + 0.5 * xx / w + 0.15 * (((xx // 9) + (yy // 9)) % 2) + 0.03 * rng.random((h, w))).clip(0, 1).astype(np.float32)
sess = ort.InferenceSession(onnx_path, providers=["CPUExecutionProvider"])
inp = sess.get_inputs()[0]
shape = inp.shape
# ArtCNN's ONNX takes NCHW or NHWC with one channel
x = img[None, None] if (len(shape) == 4 and shape[1] == 1) else img[None, :, :, None]
y = sess.run(None, {inp.name: x})[0].astype(np.float32).reshape(2 * h, 2 * w)
with tempfile.TemporaryDirectory() as d:
    fi, fo = os.path.join(d, "in.f32"), os.path.join(d, "out.f32")
    img.tofile(fi)
    subprocess.run([ref, pmnn, fi, str(w), str(h), fo], check=True)
    r = np.fromfile(fo, dtype=np.float32).reshape(2 * h, 2 * w)
b = 8
mse = float(np.mean((y[b:-b, b:-b] - r[b:-b, b:-b]) ** 2))
psnr = 200.0 if mse <= 1e-20 else 10 * np.log10(1.0 / mse)
print(f"{os.path.basename(onnx_path)}: input {shape}, onnxruntime vs PushMPC reference PSNR {psnr:.2f} dB, max diff {np.abs(y - r)[b:-b, b:-b].max():.2e}")
sys.exit(0 if psnr >= 60 else 1)
