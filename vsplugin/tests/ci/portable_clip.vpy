# vspipe test clip for pushmpc.Enhance with the portable backend (CI on every OS, no GPU): 48 frames of 320x240 at
# 24000/1001 with a bright box moving 6 px a frame on a dark background, a hard cut at frame 24 (to a flat grey
# picture), through Enhance(backend="portable") with the multiplier from PUSHMPC_TEST_K.
#   PUSHMPC_VS_PLUGIN  path of pushmpc_vs (.dll / .so / .dylib)
import os
import vapoursynth as vs

core = vs.core
core.std.LoadPlugin(os.environ["PUSHMPC_VS_PLUGIN"])
k = os.environ.get("PUSHMPC_TEST_K", "4")
bits = int(os.environ.get("PUSHMPC_TEST_BITS", "8"))
fmt = vs.YUV420P10 if bits > 8 else vs.YUV420P8
s = 4 if bits > 8 else 1
W, H, B = 320, 240, 48


def blank(w, h, y):
    return core.std.BlankClip(format=fmt, width=w, height=h, length=1, fpsnum=24000, fpsden=1001, color=[y * s, 128 * s, 128 * s])


frames = []
for i in range(48):
    if i < 24:
        x = 20 + 6 * i
        row = core.std.StackHorizontal([blank(x, B, 40), blank(B, B, 220), blank(W - x - B, B, 40)])
        frames.append(core.std.StackVertical([blank(W, 96, 40), row, blank(W, H - 96 - B, 40)]))
    else:
        frames.append(blank(W, H, 128))
clip = core.std.Splice(frames)
clip = core.std.SetFrameProps(clip, _Matrix=1, _Transfer=1, _Primaries=1, _Range=0)  # API 4.2+ range (0 limited)
clip = core.pushmpc.Enhance(clip, backend="portable", smoother=k)
clip.set_output()
