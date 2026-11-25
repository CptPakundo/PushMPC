# vspipe test clip for pushmpc.Enhance with the ml backend (pmml; CI on the CPU EP): a public Y4M clip (PMML_TEST_CLIP)
# through Enhance(backend="ml") at x4 (PUSHMPC_TEST_K), with ml_backend / ml_tier from PMML_TEST_BACKEND / PMML_TEST_TIER.
#   PUSHMPC_VS_PLUGIN  path of pushmpc_vs;  PMML_RUNTIME  the pmml runtime folder (ort/, models/, graphs/)
import os
import vapoursynth as vs

core = vs.core
core.std.LoadPlugin(os.environ["PUSHMPC_VS_PLUGIN"])
path = os.environ["PMML_TEST_CLIP"]
w, h, n = int(os.environ.get("PMML_TEST_W", "1280")), int(os.environ.get("PMML_TEST_H", "720")), int(os.environ.get("PMML_TEST_FRAMES", "9"))
with open(path, "rb") as f:
    data = f.read()
start = data.index(b"\n") + 1
size = w * h * 3 // 2
frames = []
blank = core.std.BlankClip(format=vs.YUV420P8, width=w, height=h, length=1, fpsnum=25, fpsden=1)
for i in range(n):
    off = start + i * (size + 6) + 6
    buf = data[off:off + size]
    def fill(fr, buf=buf):   # noqa: E306
        out = fr.copy()
        for p, (o, s) in enumerate(((0, w * h), (w * h, w * h // 4), (w * h * 5 // 4, w * h // 4))):
            pw = w if p == 0 else w // 2
            arr = memoryview(out[p]).cast("B")
            stride = out.get_stride(p)
            for y in range(h if p == 0 else h // 2):
                arr[y * stride:y * stride + pw] = buf[o + y * pw:o + y * pw + pw]
        return out
    frames.append(core.std.ModifyFrame(blank, blank, fill))
clip = core.std.Splice(frames)
clip = core.std.SetFrameProps(clip, _Matrix=1, _Transfer=1, _Primaries=1, _Range=0)
clip = core.pushmpc.Enhance(clip, backend="ml", smoother=os.environ.get("PUSHMPC_TEST_K", "4"),
                            ml_backend=os.environ.get("PMML_TEST_BACKEND", "cpu"), ml_tier=os.environ.get("PMML_TEST_TIER", "fast"))
clip.set_output()
