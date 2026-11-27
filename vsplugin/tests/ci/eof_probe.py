# The end of a stream whose length the source does not know (mpv's vapoursynth filter: numFrames 134217727, and a
# request past the last decoded frame fails with an error). A 48-frame moving-box clip looped, cut to 134217727 frames,
# whose frames from N on raise, as mpv's do. It goes through pushmpc.Enhance (portable: the GPU engine on lavapipe with
# PUSHMPC_VK_ALLOW_CPU=1, and the CPU engine), and the outputs are read in order until the first error, as mpv does.
# The same N frames with a known length are the reference. Prints one line a route:
#   eof <route> dedup=<on|off> threads <t>: known <count> unknown <count>, frames identical|DIFFER
# With --require: fails unless the unknown-length stream gives the known-length output, frame for frame (the end
# guard, eof_guard.h). --threads N sets core.num_threads (1: its synchronous fetch must not deadlock one thread).
#   PUSHMPC_VS_PLUGIN  path of pushmpc_vs (.dll / .so / .dylib)
import hashlib
import os
import sys
import vapoursynth as vs

core = vs.core
for _i, _a in enumerate(sys.argv):
    if _a == "--threads" and _i + 1 < len(sys.argv):
        core.num_threads = int(sys.argv[_i + 1])   # 1: the end guard's synchronous fetch must not deadlock a single thread
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
        raise vs.Error("EOF or filter EOF/reinit")   # mpv 0.42 vf_vapoursynth.c, word for word
    return f


unknown = core.std.ModifyFrame(looped[:UNKNOWN], looped[:UNKNOWN], past_end)
known = looped[:N]


# any other failure stays an error (the guard ends the stream only on mpv's EOF): a decoder error at frame 30 must fail
# the outputs from there, not end the stream quietly
def broken(n, f):
    if n >= 30:
        raise vs.Error("a decoder error")
    return f


midfail = core.std.ModifyFrame(looped[:UNKNOWN], looped[:UNKNOWN], broken)


def count(clip, cap):
    """outputs read in order until the first error, and a digest of each"""
    sums = []
    while len(sums) < cap:
        try:
            f = clip.get_frame(len(sums))
        except (vs.Error, IndexError, ValueError):   # a failed frame, or past a known length
            break
        h = hashlib.md5()
        for p in range(f.format.num_planes):
            h.update(bytes(memoryview(f[p])))
        sums.append(h.hexdigest())
    return sums


bad = 0
for route, device in (("gpu", ""), ("cpu", "cpu")):
    for dedup in ("1", "0"):
        args = dict(backend="portable", smoother=str(K), dedup=dedup)
        if device:
            args["portable_device"] = device
        kn = count(core.pushmpc.Enhance(known, **args), N * K + 8)
        un = count(core.pushmpc.Enhance(unknown, **args), N * K + 8)
        same = kn == un
        print(f"eof {route} dedup={'on' if dedup == '1' else 'off'} threads {core.num_threads}: known {len(kn)} unknown {len(un)}, "
              f"frames {'identical' if same else 'DIFFER'} (N {N}, x{K})")
        if not same:
            bad += 1
if "--require" in sys.argv and bad:
    print(f"::error::{bad} case(s): the unknown-length stream does not give the known-length output")
    sys.exit(1)
for route, device in (("gpu", ""), ("cpu", "cpu")):
    args = dict(backend="portable", smoother=str(K))
    if device:
        args["portable_device"] = device
    clip, n, msg = core.pushmpc.Enhance(midfail, **args), 0, ""
    while n < N * K:
        try:
            clip.get_frame(n)
        except vs.Error as e:
            msg = str(e)
            break
        n += 1
    ok = "a decoder error" in msg and n < 30 * K + 8
    print(f"eof {route} decoder error at source frame 30: output {n} failed ({'the error reached it' if ok else 'WRONG: ' + (msg or 'no error')})")
    if not ok and "--require" in sys.argv:
        print("::error::a source error other than mpv's EOF was not passed on as an error")
        sys.exit(1)
