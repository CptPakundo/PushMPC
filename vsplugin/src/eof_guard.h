// pushmpc.Enhance: the end of a stream whose source does not know its length.
// mpv's vapoursynth filter gives its source INT_MAX / 16 = 134217727 frames (the length is not known) and fails a request
// past the last frame. VapourSynth then fails the output that requested it (arError). The portable and ml backends request
// lookahead frames (Dedup, the held-drawing repair), so the last outputs were lost, and mpv ends at the first failed output
// (vsplugin-gpu 46000778180: 113 of 120 frames with Dedup, 119 without; Homebrew: 593 of 600, ml 589).
// The guard sits between such a source and those backends. It fetches each frame itself (a synchronous getFrame, which
// R82 runs as an external request that reserves a thread: vsplugin-gpu 46005098758's probe, 1 and 2 threads), and in
// place of a failed one hands on a frame flagged PushMPCEnd. The backends take the first flagged frame as the end
// of a known length (ScanEnd, then Pipeline::SetEnd and their End). Outputs at or past that end still fail, so mpv
// finishes as before. A source with a real length is not wrapped.
// The same node also carries the interlace measurement of an unknown-length stream (vs_interlace.h): watch sees each
// source frame as it passes. Without a backend behind it (the pass-through of an interlaced stream, the rife preset)
// substitute is off: a failed frame stays a failure, as from the source itself.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <map>
#include <string>
#include "VapourSynth4.h"
#include "vslog.h"

namespace pmvs {

inline constexpr int kUnknownLengthFrames = 134217727;   // mpv's INT_MAX / 16
inline constexpr const char* kEndProp = "PushMPCEnd";

struct EofGuard {
    VSNode* up = nullptr;
    VSVideoInfo vi{};
    std::atomic<bool> logged{ false };
    std::atomic<bool> substitute{ true };                          // a failed frame at the end becomes a flagged one
    std::function<void(const VSAPI*, const VSFrame*, int)> watch;  // each frame passed on (thread-safe itself)
    VSNode* self = nullptr;
};
// the guards alive: pushmpc.Enhance wraps mpv's source in a measuring guard (WatchUnknownLength) before it picks a
// backend, without substitution: a pass-through (Enable = 0, x1, a fallback) hands it to mpv as it is, and mpv ends at
// the source's own failure. A backend's GuardUnknownLength finds that guard and turns substitution on instead of
// wrapping it again (the backends call it only after their pass-through paths returned).
inline std::mutex& GuardLock() { static std::mutex m; return m; }
inline std::map<const VSNode*, EofGuard*>& Guards() { static std::map<const VSNode*, EofGuard*> s; return s; }

inline const VSFrame* VS_CC EofGuardGetFrame(int n, int reason, void* inst, void**, VSFrameContext* fc, VSCore* core, const VSAPI* a)
{
    auto* g = static_cast<EofGuard*>(inst);
    if (reason != arInitial) return nullptr;
    char err[512] = "";
    if (const VSFrame* f = a->getFrame(n, g->up, err, sizeof(err))) {
        if (g->watch) g->watch(a, f, n);
        return f;
    }
    // only mpv's end ("EOF or filter EOF/reinit", vf_vapoursynth.c: no frame will come) is an end. Any other failure (a
    // frame mpv no longer holds, a decoder error) stays an error, as without the guard: it must not cut the stream short
    if (!g->substitute.load() || n == 0 || !std::strstr(err, "EOF")) {
        a->setFilterError(("pushmpc.Enhance: source frame " + std::to_string(n) + " failed: " + std::string(err)).c_str(), fc);
        return nullptr;
    }
    if (!g->logged.exchange(true)) Log("source: frame " + std::to_string(n) + " failed (" + std::string(err) + "): the end of the stream");
    VSFrame* e = a->newVideoFrame(&g->vi.format, g->vi.width, g->vi.height, nullptr, core);
    if (!e) { a->setFilterError("pushmpc.Enhance: out of memory at the stream's end", fc); return nullptr; }
    for (int p = 0; p < g->vi.format.numPlanes; ++p)
        std::memset(a->getWritePtr(e, p), 0, size_t(a->getStride(e, p)) * size_t(a->getFrameHeight(e, p)));
    a->mapSetInt(a->getFramePropertiesRW(e), kEndProp, 1, maReplace);
    return e;
}
inline void VS_CC EofGuardFree(void* inst, VSCore*, const VSAPI* a)
{
    auto* g = static_cast<EofGuard*>(inst);
    { std::lock_guard<std::mutex> lk(GuardLock()); Guards().erase(g->self); }
    a->freeNode(g->up);
    delete g;
}

// the guard on an unknown-length node (the reference to node passes to it), else node itself. substitute: a PushMPC
// backend reads it (its flagged end frame); a guard already there gets substitution turned on
inline VSNode* MakeGuard(VSNode* node, VSCore* core, const VSAPI* a, std::function<void(const VSAPI*, const VSFrame*, int)> watch, bool substitute)
{
    const VSVideoInfo* vi = a->getVideoInfo(node);
    if (vi->numFrames < kUnknownLengthFrames) return node;
    {
        std::lock_guard<std::mutex> lk(GuardLock());
        auto it = Guards().find(node);
        if (it != Guards().end()) { if (substitute) it->second->substitute.store(true); return node; }
    }
    auto* g = new EofGuard();
    g->up = node; g->vi = *vi; g->watch = std::move(watch); g->substitute.store(substitute);
    VSFilterDependency deps[] = { { node, rpGeneral } };
    VSNode* out = a->createVideoFilter2("PushMPCEndGuard", vi, EofGuardGetFrame, EofGuardFree, fmParallel, deps, 1, g, core);
    if (!out) { delete g; return node; }
    std::lock_guard<std::mutex> lk(GuardLock());
    g->self = out; Guards()[out] = g;
    return out;
}
// a backend's input: mpv's source gets its end guard (or the measuring guard already on it hands on the end frame)
inline VSNode* GuardUnknownLength(VSNode* node, VSCore* core, const VSAPI* a) { return MakeGuard(node, core, a, {}, true); }
// the interlace measurement on mpv's source (vs_interlace.h): no substitution until a backend takes it
inline VSNode* WatchUnknownLength(VSNode* node, VSCore* core, const VSAPI* a, std::function<void(const VSAPI*, const VSFrame*, int)> watch)
{
    return MakeGuard(node, core, a, std::move(watch), false);
}

// arInitial's request range kept in frameData[0] (from, and the count after it: both well under 2^31)
static_assert(sizeof(void*) == 8, "PackRange keeps two numbers in a pointer");
inline void* PackRange(int64_t from, int64_t to) { return reinterpret_cast<void*>(uintptr_t(from) | (uintptr_t(to - from) << 32)); }
inline void UnpackRange(void* p, int64_t& from, int64_t& to)
{
    const uintptr_t v = reinterpret_cast<uintptr_t>(p);
    from = int64_t(v & 0xffffffffu); to = from + int64_t(v >> 32);
}
// the error of an output at or past the end found while playing (VapourSynth needs one with every output not returned)
inline std::string PastEnd(int n) { return "pushmpc.Enhance: the stream ended before output " + std::to_string(n); }
// the first flagged frame among those requested (-1: none)
inline int64_t ScanEnd(const VSAPI* a, VSNode* node, VSFrameContext* fc, int64_t from, int64_t to)
{
    for (int64_t i = from; i <= to; ++i) {
        const VSFrame* f = a->getFrameFilter(int(i), node, fc);
        if (!f) continue;
        int e = 0;
        const bool end = a->mapGetInt(a->getFramePropertiesRO(f), kEndProp, 0, &e) == 1 && !e;
        a->freeFrame(f);
        if (end) return i;
    }
    return -1;
}

} // namespace pmvs
