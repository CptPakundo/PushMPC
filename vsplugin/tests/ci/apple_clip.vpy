# vspipe test clip for pushmpc.Enhance with the apple backend (macOS CI): 48 frames of 1280x720 at 24000/1001 with a
# bright box moving 12 px a frame on a dark background and a hard cut at frame 24 (to a flat grey picture).
#   PUSHMPC_VS_PLUGIN  path of pushmpc_vs.dylib (skipped when the plugin is autoloaded, e.g. Homebrew)
#   PUSHMPC_TEST_K     multiplier (default 2)       PUSHMPC_TEST_BITS  8 or 10      PUSHMPC_TEST_SR  apple_sr (default off)
#   PUSHMPC_TEST_BACKEND  backend (default apple)
import os
import vapoursynth as vs

core = vs.core
if not hasattr(core, "pushmpc"):
    core.std.LoadPlugin(os.environ["PUSHMPC_VS_PLUGIN"])
k = os.environ.get("PUSHMPC_TEST_K", "2")
bits = int(os.environ.get("PUSHMPC_TEST_BITS", "8"))
fmt = vs.YUV420P10 if bits > 8 else vs.YUV420P8
s = 4 if bits > 8 else 1
W, H, B = 1280, 720, 96


def blank(w, h, y):
    return core.std.BlankClip(format=fmt, width=w, height=h, length=1, fpsnum=24000, fpsden=1001, color=[y * s, 128 * s, 128 * s])


frames = []
for i in range(48):
    if i < 24:
        x = 64 + 12 * i
        row = core.std.StackHorizontal([blank(x, B, 40), blank(B, B, 220), blank(W - x - B, B, 40)])
        frames.append(core.std.StackVertical([blank(W, 304, 40), row, blank(W, H - 304 - B, 40)]))
    else:
        frames.append(blank(W, H, 128))
clip = core.std.Splice(frames)
clip = core.std.SetFrameProps(clip, _Matrix=1, _Transfer=1, _Primaries=1, _Range=0)  # API 4.2+ range (0 limited)
clip = core.pushmpc.Enhance(clip, backend=os.environ.get("PUSHMPC_TEST_BACKEND", "apple"), smoother=k,
                            apple_sr=os.environ.get("PUSHMPC_TEST_SR", "off"))
clip.set_output()
