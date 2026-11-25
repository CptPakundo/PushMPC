"""The generated ArtCNN fragment shaders in mpv's OpenGL renderer against PushMPC's float reference (CI, ubuntu, Xvfb +
llvmpipe). Needs mpv, numpy, pillow.
  artcnn_mpv_check.py <ArtCNN_*_fragment.glsl> <model.pmnn> <artcnn_ref executable>

A grey (Y-only) raw picture is shown in a window exactly 2x its size (so the shader's LUMA hook runs and its output
is the final size), full range in and out, and captured with a window screenshot; the red channel is then the luma the shader produced. PSNR
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
    shot = os.path.join(d, "shot.png")
    lua = os.path.join(d, "shot.lua")
    # vo=image saves at the video's own size (no upscale, so the LUMA hook never runs): render into a 2x window under
    # Xvfb instead and take a window screenshot of the first frame
    with open(lua, "w") as f:
        f.write('mp.register_event("playback-restart", function() mp.add_timeout(1.0, function() '
                'mp.commandv("screenshot-to-file", "%s", "window"); mp.command("quit") end) end)\n' % shot)
    cmd = ["mpv", "--no-config", "--msg-level=all=warn", "--demuxer=rawvideo", "--demuxer-rawvideo-mp-format=gray",
           f"--demuxer-rawvideo-w={w}", f"--demuxer-rawvideo-h={h}", "--demuxer-rawvideo-fps=1", "--pause",
           f"--geometry={2 * w}x{2 * h}", "--keepaspect=no", "--keepaspect-window=no", "--border=no",
           "--vo=gpu", "--gpu-api=opengl", "--gpu-context=x11", "--video-output-levels=full", "--dither-depth=no", "--scale=bilinear", "--cscale=bilinear",
           "--osd-level=0", f"--glsl-shaders={glsl}", f"--script={lua}", "--screenshot-format=png", raw]
    subprocess.run(cmd, check=True, timeout=120)
    if not os.path.isfile(shot):
        raise SystemExit("mpv wrote no screenshot")
    pngs = [os.path.basename(shot)]
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
