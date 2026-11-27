# Can a filter fetch its source synchronously (vsapi->getFrame) from inside its getFrame, without deadlocking a small
# thread pool? The candidate fix for mpv's unknown-length source (a request past the end fails the output that asked):
# a guard that fetches each source frame itself and turns a failure into a flagged frame. Emulated without the plugin:
# std.FrameEval's eval runs in a worker thread and calls src.get_frame (a synchronous getFrame); a consumer asks for
# frames n .. n + 3 of the guard (as a lookahead of 3), and the outputs are read with 2 requests in flight (mpv's
# concurrent-frames=2). Run with the thread count as the argument, under a short timeout: a deadlock shows as the
# timeout. Prints: sync <threads>: <outputs> outputs, <flagged> flagged past the end, <s> s
import sys
import time
import vapoursynth as vs

core = vs.core
core.num_threads = int(sys.argv[1])
N, UNKNOWN, LOOK, INFLIGHT = 60, 134217727, 3, 2
base = core.std.BlankClip(format=vs.YUV420P8, width=64, height=64, length=48, fpsnum=24, fpsden=1)
looped = core.std.Loop(base, times=0)[:UNKNOWN]


def past_end(n, f):
    if n >= N:
        raise vs.Error("EOF (as mpv returns it)")
    return f


src = core.std.ModifyFrame(looped, looped, past_end)
end = core.std.SetFrameProps(base[0] * UNKNOWN, PushMPCEnd=1)


def guard(n):
    try:
        src.get_frame(n)   # synchronous, inside a worker thread
        return src
    except vs.Error:
        return end


guarded = core.std.FrameEval(src, guard)
shifted = [guarded] + [guarded[i:] + guarded[-1] * i for i in range(1, LOOK + 1)]


def consumer(n, f):
    return f[0]


out = core.std.ModifyFrame(guarded, shifted, consumer)

t0 = time.time()
pending, results = [], {}
n = 0
total = N + 10
while n < total or pending:
    while n < total and len(pending) < INFLIGHT:
        pending.append((n, out.get_frame_async(n)))
        n += 1
    i, fut = pending.pop(0)
    results[i] = fut.result()
flagged = sum(1 for i, f in results.items() if f.props.get("PushMPCEnd", 0) == 1)
print(f"sync {core.num_threads}: {len(results)} outputs, {flagged} flagged past the end (want {total - N}), {time.time() - t0:.1f} s")
sys.exit(0 if len(results) == total and flagged == total - N else 1)
