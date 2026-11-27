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
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
};

inline const VSFrame* VS_CC EofGuardGetFrame(int n, int reason, void* inst, void**, VSFrameContext* fc, VSCore* core, const VSAPI* a)
{
    auto* g = static_cast<EofGuard*>(inst);
    if (reason != arInitial) return nullptr;
    char err[512] = "";
    if (const VSFrame* f = a->getFrame(n, g->up, err, sizeof(err))) return f;
    if (n == 0) { a->setFilterError(("pushmpc.Enhance: the source's first frame failed: " + std::string(err)).c_str(), fc); return nullptr; }
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
    a->freeNode(g->up);
    delete g;
}

// node wrapped when its length is the unknown one (the reference to node passes to the guard), else node itself
inline VSNode* GuardUnknownLength(VSNode* node, VSCore* core, const VSAPI* a)
{
    const VSVideoInfo* vi = a->getVideoInfo(node);
    if (vi->numFrames < kUnknownLengthFrames) return node;
    auto* g = new EofGuard();
    g->up = node; g->vi = *vi;
    VSFilterDependency deps[] = { { node, rpGeneral } };
    VSNode* out = a->createVideoFilter2("PushMPCEndGuard", vi, EofGuardGetFrame, EofGuardFree, fmParallel, deps, 1, g, core);
    if (!out) { delete g; return node; }
    return out;
}

// arInitial's request range kept in frameData[0] (from, and the count after it: both well under 2^31)
static_assert(sizeof(void*) == 8, "PackRange keeps two numbers in a pointer");
inline void* PackRange(int64_t from, int64_t to) { return reinterpret_cast<void*>(uintptr_t(from) | (uintptr_t(to - from) << 32)); }
inline void UnpackRange(void* p, int64_t& from, int64_t& to)
{
    const uintptr_t v = reinterpret_cast<uintptr_t>(p);
    from = int64_t(v & 0xffffffffu); to = from + int64_t(v >> 32);
}
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
