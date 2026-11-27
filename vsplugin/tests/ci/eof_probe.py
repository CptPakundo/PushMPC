# The end of a stream whose length the source does not know (mpv's vapoursynth filter: numFrames 134217727, and a
# request past the last decoded frame fails with an error). A 48-frame moving-box clip looped, cut to 134217727 frames,
# whose frames from N on raise, as mpv's do. It goes through pushmpc.Enhance (portable: the GPU engine on lavapipe with
# PUSHMPC_VK_ALLOW_CPU=1, and the CPU engine), and the outputs are read in order until the first error, as mpv does.
# The same N frames with a known length are the reference. Prints one line a route:
#   eof <route> dedup=<on|off>: known <count> unknown <count>
# With --require: fails when the unknown-length count is below the known-length one minus allow (k - 1: the last
# source's generated frames, which need the frame after it).
#   PUSHMPC_VS_PLUGIN  path of pushmpc_vs (.dll / .so / .dylib)
import os
import sys
import vapoursynth as vs

core = vs.core
core.std.LoadPlugin(os.environ["PUSHMPC_VS_PLUGIN"])
N, K, UNKNOWN = 60, 2, 134217727
W, H, B = 320, 240, 48


def blank(w, h, y):
    return core.std.BlankClip(format=vs.YUV420P8, width=w, height=h, length=1, fpsnum=24, fpsden=1, color=[y, 128, 128])


frames = []
for i in range(48):
    x = 20 + 4 * i   # 20 .. 208: the right strip stays at least 64 px wide (and even, for 4:2:0)
    row = core.std.StackHorizontal([blank(x, B, 40), blank(B, B, 220), blank(W - x - B, B, 40)])
    frames.append(core.std.StackVertical([blank(W, 96, 40), row, blank(W, H - 96 - B, 40)]))
base = core.std.SetFrameProps(core.std.Splice(frames), _Matrix=1, _Transfer=1, _Primaries=1, _Range=0)
looped = core.std.Loop(base, times=0)


def past_end(n, f):
    if n >= N:
        raise vs.Error("EOF (as mpv returns it)")
    return f


unknown = core.std.ModifyFrame(looped[:UNKNOWN], looped[:UNKNOWN], past_end)
known = looped[:N]


def count(clip, cap):
    n = 0
    while n < cap:
        try:
            clip.get_frame(n)
        except (vs.Error, IndexError, ValueError):   # a failed frame, or past a known length
            break
        n += 1
    return n


bad = 0
for route, device in (("gpu", ""), ("cpu", "cpu")):
    for dedup in ("1", "0"):
        args = dict(backend="portable", smoother=str(K), dedup=dedup)
        if device:
            args["portable_device"] = device
        kn = count(core.pushmpc.Enhance(known, **args), N * K + 8)
        un = count(core.pushmpc.Enhance(unknown, **args), N * K + 8)
        print(f"eof {route} dedup={'on' if dedup == '1' else 'off'}: known {kn} unknown {un} (N {N}, x{K})")
        if un < kn - (K - 1):
            bad += 1
if "--require" in sys.argv and bad:
    print(f"::error::{bad} route(s) lose frames at the end of an unknown-length stream")
    sys.exit(1)
