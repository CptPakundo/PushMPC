// Interlaced input on the VapourSynth routes (pushmpc.Enhance, pushmpc.Interlace for the script's rife preset): measured
// in the picture as the DirectShow filter and the Kodi add-on measure it (content_detect.h CombMeter: field combing on
// moving areas of the native luma, 24 moving frames, half or more combed = interlaced), never from names. The frames'
// _FieldBased (mpv's vf_vapoursynth and the source filters set it) is a prior: it is logged and sizes the look, as the
// stream's field_order does in Kodi, but the picture decides. Interlaced: PushMPC steps aside, the clip passes through
// unchanged at the source rate (frame generation and upscaling on combed pictures would smear the combs).
//
// A VapourSynth filter fixes its length and rate when it is created, so the decision is made there:
//   - a clip of known length (vspipe, an encode script): the first frames are read at once (kProbeFlagged of them
//     when frame 0 says progressive, else kProbe; the Kodi add-on's look) and decide: interlaced, the output is the
//     clip itself, N of N frames;
//   - mpv's source of unknown length (no frame exists while the script loads): the guard that already sits on that
//     source (eof_guard.h) measures the first frames as they pass (kGiveUp frames without enough motion count as
//     progressive, the filter's rule). The verdict is kept for the stream in the process (vs_content.h's memo) and,
//     when it differs from what the running instance was created as, the status file asks pushmpc.lua to add the
//     filter again (interlace_reload=1), as for a measured rate; the next instance starts as the verdict says. Every
//     instance measures again, so a progressive stream after an interlaced one of the same size and rate gets its
//     frame generation back the same way.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include "VapourSynth4.h"
#include "pushmpc/content_detect.h"
#include "eof_guard.h"
#include "vs_content.h"
#include "vslog.h"

namespace pmvs {

using pushmpc::content::CombMeter;

inline int64_t IlArgInt(const VSMap* in, const VSAPI* a, const char* key, int64_t def)
{
    int err = 0;
    const int64_t v = a->mapGetInt(in, key, 0, &err);
    return err ? def : v;
}
// one frame's luma into the meter (YUV or Gray, integer samples)
inline CombMeter::Verdict PushLuma(CombMeter& m, const VSAPI* a, const VSFrame* f)
{
    const VSVideoFormat* vf = a->getVideoFrameFormat(f);
    if (!vf || vf->sampleType != stInteger || (vf->colorFamily != cfYUV && vf->colorFamily != cfGray)) return m.verdict();
    const int w = a->getFrameWidth(f, 0), h = a->getFrameHeight(f, 0);
    if (vf->bytesPerSample == 1) return m.Push(a->getReadPtr(f, 0), a->getStride(f, 0), w, h, 0);
    return m.Push(reinterpret_cast<const uint16_t*>(a->getReadPtr(f, 0)), a->getStride(f, 0) / 2, w, h, std::max(0, vf->bitsPerSample - 8));
}
// _FieldBased: 0 progressive, 1 bottom field first, 2 top field first; -1 not set
inline int FieldBased(const VSAPI* a, const VSFrame* f)
{
    int e = 0;
    const int64_t v = a->mapGetInt(a->getFramePropertiesRO(f), "_FieldBased", 0, &e);
    return e ? -1 : int(v);
}
inline std::string IlWhy(int combed, int measured)
{
    return std::format("interlaced input (field combing measured on {} of {} moving frames)", combed, measured);
}
inline std::string ProgressiveWhy(const CombMeter& c, CombMeter::Verdict v, int flagged, int seen)
{
    return std::format("progressive (measured): {} of {} moving frames combed{}; flags said {} of {} frames interlaced", c.combed(), c.measured(),
                       v == CombMeter::kMeasuring ? " (too little motion to tell: treated as progressive)" : "", flagged, seen);
}
inline void RememberInterlace(const std::string& key, int il, int measured, int combed)
{
    std::lock_guard<std::mutex> lk(MemoLock());
    StreamMemo& m = Memo()[key];
    m.il = il; m.ilMeasured = measured; m.ilCombed = combed;
}

// the measurement while mpv plays (in the source's guard): thread-safe, decides once
struct IlWatch {
    std::string key;
    bool aside = false;      // the instance was created passing the stream through
    double declared = 0;
    std::mutex mx;
    CombMeter comb;
    int seen = 0, flagged = 0;
    bool done = false;
    void Push(const VSAPI* a, const VSFrame* f)
    {
        std::lock_guard<std::mutex> lk(mx);
        if (done) return;
        const int fb = FieldBased(a, f);
        if (seen < 3) Log(std::format("frame {}: _FieldBased {} (a prior; the picture decides)", seen, fb < 0 ? std::string("not set") : std::to_string(fb)));
        ++seen;
        if (fb > 0) ++flagged;
        const CombMeter::Verdict v = PushLuma(comb, a, f);
        if (v == CombMeter::kMeasuring && seen < CombMeter::kGiveUp) return;
        done = true;
        if (v == CombMeter::kMeasuring) {   // a still stretch (a seek into one): nothing measured, the instance stays as it was made
            Log(ProgressiveWhy(comb, v, flagged, seen) + (aside ? "; the stream keeps passing through (the earlier verdict stands)" : ""));
            return;
        }
        const bool il = v == CombMeter::kInterlaced;
        RememberInterlace(key, il ? kIlInterlaced : kIlProgressive, comb.measured(), comb.combed());
        if (il) Log("interlaced: " + IlWhy(comb.combed(), comb.measured()));
        else Log(ProgressiveWhy(comb, v, flagged, seen));
        const bool reload = il != aside;
        if (reload) Log(il ? "interlaced: this instance generates frames; pushmpc.lua adds the filter again, and the next instance passes the stream through"
                           : "this instance passes the stream through (an earlier verdict on a stream of the same size and rate); pushmpc.lua adds the filter again for frame generation");
        const StreamMemo m = Recall(key);
        if (FILE* sf = std::fopen(StatusPath().c_str(), "w")) {
            std::fprintf(sf, "fps=%.4f\ndeclared=%.4f\nclass=%d\nclass_name=%s\ncg=%d\nreload=0\ninterlaced=%d\ninterlace_reload=%d\nwhy=%s\n", m.fps, declared, m.cls,
                         pushmpc::content::ClassName(m.cls), m.cg ? 1 : 0, m.il, reload ? 1 : 0, (il ? IlWhy(comb.combed(), comb.measured()) : ProgressiveWhy(comb, v, flagged, seen)).c_str());
            std::fclose(sf);
        }
    }
};

// the first frames of a clip of known length, read at once
struct IlProbe { CombMeter::Verdict v = CombMeter::kMeasuring; int measured = 0, combed = 0, seen = 0, flagged = 0, firstFb = -1; std::string error; };
inline IlProbe ProbeInterlace(VSNode* node, const VSAPI* a)
{
    IlProbe r;
    CombMeter comb;
    const VSVideoInfo* vi = a->getVideoInfo(node);
    int budget = CombMeter::kProbe;
    for (int n = 0; n < vi->numFrames && n < budget && comb.verdict() == CombMeter::kMeasuring; ++n) {
        char err[256] = "";
        const VSFrame* f = a->getFrame(n, node, err, sizeof err);
        if (!f) { r.error = err; break; }
        const int fb = FieldBased(a, f);
        if (n == 0) { r.firstFb = fb; if (fb == 0) budget = CombMeter::kProbeFlagged; }   // flagged progressive: the shorter look
        if (fb > 0) ++r.flagged;
        ++r.seen;
        PushLuma(comb, a, f);
        a->freeFrame(f);
    }
    r.v = comb.verdict(); r.measured = comb.measured(); r.combed = comb.combed();
    return r;
}

// the decision at create: aside = pass the clip through (node: what to output), else node is what the backend reads
// (an unknown-length source wrapped in its measuring guard: it hands a flagged end frame on only once a PushMPC backend
// takes it, eof_guard.h). The reference to node passes on.
struct IlDecision { bool aside = false; VSNode* node = nullptr; std::string why; };
inline IlDecision DecideInterlace(VSNode* node, VSCore* core, const VSAPI* a)
{
    IlDecision d;
    const VSVideoInfo* vi = a->getVideoInfo(node);
    const std::string key = StreamKey(*vi);
    if (vi->numFrames >= kUnknownLengthFrames) {
        const StreamMemo m = Recall(key);
        auto w = std::make_shared<IlWatch>();
        w->key = key; w->aside = m.il == kIlInterlaced;
        w->declared = vi->fpsNum > 0 && vi->fpsDen > 0 ? double(vi->fpsNum) / double(vi->fpsDen) : 0.0;
        d.aside = w->aside;
        if (d.aside) d.why = IlWhy(m.ilCombed, m.ilMeasured) + ", on this stream before";
        d.node = WatchUnknownLength(node, core, a, [w](const VSAPI* api, const VSFrame* f, int) { w->Push(api, f); });
        if (d.aside) Log("interlaced: " + d.why + ": the clip passes through at the source rate (measured again as it plays)");
        else Log("interlacing: measured on the first frames as they pass (field combing; _FieldBased is only a prior)");
        return d;
    }
    const IlProbe p = ProbeInterlace(node, a);
    d.node = node;
    d.aside = p.v == CombMeter::kInterlaced;
    RememberInterlace(key, d.aside ? kIlInterlaced : kIlProgressive, p.measured, p.combed);
    if (d.aside) {
        d.why = IlWhy(p.combed, p.measured);
        Log(std::format("interlaced: {}: the clip passes through unchanged ({} frames at {}/{} fps); _FieldBased {} on frame 0, {} of {} frames flagged (a prior)", d.why,
                        vi->numFrames, vi->fpsNum, vi->fpsDen, p.firstFb, p.flagged, p.seen));
    } else {
        Log(std::format("progressive (measured): {} of {} moving frames combed{} in the first {} frames; _FieldBased {} on frame 0, {} flagged interlaced (a prior){}", p.combed, p.measured,
                        p.v == CombMeter::kMeasuring ? " (too little motion to tell: treated as progressive)" : "", p.seen, p.firstFb, p.flagged,
                        p.error.empty() ? std::string() : "; reading stopped: " + p.error));
    }
    return d;
}

} // namespace pmvs
