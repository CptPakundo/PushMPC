# The end of a stream whose length the source does not know, on the Windows nvidia backend (pushmpc_vs.cpp
# NvidiaCreate / EnhanceGetFrame) against the stub core on WARP: mpv's vapoursynth filter gives its source 134217727
# frames and fails a request past the last decoded one ("EOF or filter EOF/reinit"). With the end guard (eof_guard.h)
# the unknown-length stream must give the known-length output, frame for frame: K x N, the last frame's K - 1 slots
# repeating it (PushMPCFrameKind 2). A decoder error mid-stream must still reach the output as an error.
# Prints one line a case:
#   nv eof threads <t>: known <count> unknown <count> (want <K x N>), frames identical|DIFFER, frame 0 identical|DIFFERS, tail kinds <...>
#   nv eof decoder error at source frame 30: output <n> failed (the error reached it | WRONG ...)
# Exits 1 when a case fails.
#   PUSHMPC_VS_PLUGIN  path of pushmpc_vs.dll     PUSHMPC_STUB_DIR  folder of the stub pushcore.dll
#   PUSHMPC_TEST_K     the multiplier (default 2)  --threads N  core.num_threads (1: the guard's synchronous fetch must not deadlock)
import hashlib
import os
import sys
import vapoursynth as vs

core = vs.core
for _i, _a in enumerate(sys.argv):
    if _a == "--threads" and _i + 1 < len(sys.argv):
        core.num_threads = int(sys.argv[_i + 1])
core.std.LoadPlugin(os.environ["PUSHMPC_VS_PLUGIN"])
N, UNKNOWN = 60, 134217727
K = int(os.environ.get("PUSHMPC_TEST_K", "2"))
W, H, B = 320, 240, 48
ARGS = dict(smoother=str(K), sharper="off", adapter="warp", core_dir=os.environ["PUSHMPC_STUB_DIR"], chroma="420",
            matrix=1, transfer=1, primaries=1, range=1)


def blank(w, h, y):
    return core.std.BlankClip(format=vs.YUV420P8, width=w, height=h, length=1, fpsnum=24000, fpsden=1001, color=[y, 128, 128])


frames = []
for i in range(48):
    x = 20 + 4 * i
    row = core.std.StackHorizontal([blank(x, B, 40), blank(B, B, 220), blank(W - x - B, B, 40)])
    frames.append(core.std.StackVertical([blank(W, 96, 40), row, blank(W, H - 96 - B, 40)]))
base = core.std.SetFrameProps(core.std.Splice(frames), _Matrix=1, _Transfer=1, _Primaries=1, _Range=0)
looped = core.std.Loop(base, times=0)


def past_end(n, f):
    if n >= N:
        raise vs.Error("EOF or filter EOF/reinit")   # mpv 0.42 vf_vapoursynth.c, word for word
    return f


def broken(n, f):
    if n >= 30:
        raise vs.Error("a decoder error")
    return f


unknown = core.std.ModifyFrame(looped[:UNKNOWN], looped[:UNKNOWN], past_end)
known = looped[:N]
midfail = core.std.ModifyFrame(looped[:UNKNOWN], looped[:UNKNOWN], broken)


def count(clip, cap):
    """outputs read in order until the first error: their digests and frame kinds"""
    sums, kinds = [], []
    while len(sums) < cap:
        try:
            f = clip.get_frame(len(sums))
        except (vs.Error, IndexError, ValueError):
            break
        h = hashlib.md5()
        for p in range(f.format.num_planes):
            h.update(bytes(memoryview(f[p])))
        sums.append(h.hexdigest())
        kinds.append(int(f.props.get("PushMPCFrameKind", -1)))
    return sums, kinds


bad = 0
kn, kk = count(core.pushmpc.Enhance(known, **ARGS), N * K + 8)
un, uk = count(core.pushmpc.Enhance(unknown, **ARGS), N * K + 8)
same = kn == un and len(un) == N * K and uk[-(K - 1):] == [2] * (K - 1) if K > 1 else kn == un and len(un) == N
print(f"nv eof threads {core.num_threads}: known {len(kn)} unknown {len(un)} (want {N * K}), frames {'identical' if same else 'DIFFER'}, "
      f"frame 0 {'identical' if kn[:1] == un[:1] and kn else 'DIFFERS'}, tail kinds known {kk[-K:]} unknown {uk[-K:]} (x{K})")
bad += not same
clip, n, msg = core.pushmpc.Enhance(midfail, **ARGS), 0, ""
while n < N * K:
    try:
        clip.get_frame(n)
    except vs.Error as e:
        msg = str(e)
        break
    n += 1
ok = "a decoder error" in msg and n < 30 * K + 8
print(f"nv eof decoder error at source frame 30: output {n} failed ({'the error reached it' if ok else 'WRONG: ' + (msg or 'no error')})")
bad += not ok
if bad:
    print(f"::error::{bad} nvidia end-of-stream case(s) failed")
    sys.exit(1)
