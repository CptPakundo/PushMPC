"""The generated ArtCNN fragment shaders in mpv's OpenGL renderer against PushMPC's float reference (CI, ubuntu, Xvfb +
llvmpipe). Needs mpv, numpy, pillow.
  artcnn_mpv_check.py <ArtCNN_*_fragment.glsl> <model.pmnn> <artcnn_ref executable>

A grey (Y-only) raw picture is shown at 2x (video-zoom=1, so the shader's LUMA hook runs and its output is the final
size), full range in and out, saved with vo=image as PNG; the red channel is then the luma the shader produced. PSNR
against the reference, 8 px border excluded (mpv's texture edges clamp, the network pads with zeros): >= 35 dB."""
import os
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

glsl, pmnn, ref = sys.argv[1:4]
h, w = 72, 128
yy, xx = np.mgrid[0:h, 0:w]
img = (0.25 + 0.5 * xx / w + 0.15 * (((xx // 9) + (yy // 9)) % 2)).clip(0, 1)
y8 = np.round(img * 255).astype(np.uint8)
with tempfile.TemporaryDirectory() as d:
    raw = os.path.join(d, "in.gray")
    y8.tofile(raw)
    cmd = ["mpv", "--no-config", "--msg-level=all=warn", "--frames=1", "--demuxer=rawvideo", "--demuxer-rawvideo-mp-format=gray",
           f"--demuxer-rawvideo-w={w}", f"--demuxer-rawvideo-h={h}", "--demuxer-rawvideo-fps=1", "--video-zoom=1", "--keepaspect=no",
           "--vo=image", "--vo-image-format=png", f"--vo-image-outdir={d}", "--gpu-api=opengl", "--video-output-levels=full",
           "--video-sync=audio", "--dither-depth=no", f"--glsl-shaders={glsl}", "--scale=bilinear", raw]
    subprocess.run(cmd, check=True, timeout=120)
    pngs = sorted(p for p in os.listdir(d) if p.endswith(".png"))
    if not pngs:
        raise SystemExit("mpv wrote no image")
    out = np.asarray(Image.open(os.path.join(d, pngs[0])).convert("RGB")).astype(np.float64)[:, :, 0] / 255.0
    fi, fo = os.path.join(d, "in.f32"), os.path.join(d, "out.f32")
    (y8.astype(np.float32) / 255.0).tofile(fi)
    subprocess.run([ref, pmnn, fi, str(w), str(h), fo], check=True)
    r = np.fromfile(fo, dtype=np.float32).reshape(2 * h, 2 * w).astype(np.float64)
print(f"mpv output {out.shape[1]}x{out.shape[0]}, reference {r.shape[1]}x{r.shape[0]}")
if out.shape != r.shape:
    raise SystemExit("size mismatch: mpv did not show the picture at exactly 2x")
b = 8
mse = float(np.mean((out[b:-b, b:-b] - r[b:-b, b:-b]) ** 2))
psnr = 200.0 if mse <= 1e-20 else 10 * np.log10(1.0 / mse)
print(f"{os.path.basename(glsl)} in mpv (OpenGL) vs the float reference: PSNR {psnr:.2f} dB")
sys.exit(0 if psnr >= 35 else 1)
