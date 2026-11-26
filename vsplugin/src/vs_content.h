// ContentDetect on the VapourSynth routes (pushmpc.Enhance: pushcore, portable, ml): what the picture is and the real
// frame rate, measured on the frames the filter reads (planes + _DurationNum/_DurationDen), never from names or the
// container's rate.
//
// A VapourSynth filter declares its rate and frame count when it is created, before any frame exists, and mpv reloads the
// script on every seek and on a filter change. So the measurement is kept per stream in the process (the plugin stays
// loaded across reloads) and used by the next instance; and it is published in a small status file
// (<temp>/pushmpc-vs-<pid>.txt) that pushmpc.lua reads: when the measured rate or class differs from what the running
// instance was created with, the script re-adds the filter once (as a seek would), and the new instance starts with them.
#pragma once
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include "VapourSynth4.h"
#include "pushmpc/content_detect.h"
#include "pushmpc/content_model.h"     // the image model (only with PUSHMPC_CONTENT_MODEL: pmml linked)
#include <memory>
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace pmvs {

struct StreamMemo { double fps = 0; int cls = 0; bool cg = false; };   // cg: drawn, rendered 3D CG
inline std::mutex& MemoLock() { static std::mutex m; return m; }
inline std::map<std::string, StreamMemo>& Memo() { static std::map<std::string, StreamMemo> m; return m; }
inline std::string StreamKey(const VSVideoInfo& vi)
{
    return std::to_string(vi.width) + "x" + std::to_string(vi.height) + ":" + std::to_string(vi.format.bitsPerSample) + ":" + std::to_string(vi.fpsNum) + "/" + std::to_string(vi.fpsDen);
}
inline StreamMemo Recall(const std::string& key) { std::lock_guard<std::mutex> lk(MemoLock()); auto it = Memo().find(key); return it == Memo().end() ? StreamMemo{} : it->second; }

// the declared rate replaced by the measured one of the same stream (kept from an earlier instance), when they differ by
// more than 5 % (a "29.97" tag on a 24p body); NTSC fractions kept exact. Returns the stream key.
inline std::string ApplyMeasuredRate(VSVideoInfo& vi, std::string* note = nullptr)
{
    const std::string key = StreamKey(vi);
    const StreamMemo m = Recall(key);
    const double declared = vi.fpsNum > 0 && vi.fpsDen > 0 ? double(vi.fpsNum) / double(vi.fpsDen) : 0.0;
    if (m.fps > 1.0 && (declared <= 0 || std::fabs(m.fps - declared) / m.fps > 0.05)) {
        int64_t num = int64_t(std::llround(m.fps * 1000.0)), den = 1000;
        double bestErr = 0.004;   // the closest common rate within 0.4 %
        for (const auto& r : { std::pair<int64_t, int64_t>{ 24000, 1001 }, { 24, 1 }, { 25, 1 }, { 30000, 1001 }, { 30, 1 }, { 50, 1 }, { 60000, 1001 }, { 60, 1 }, { 120000, 1001 }, { 120, 1 } }) {
            const double e = std::fabs(m.fps - double(r.first) / double(r.second)) / m.fps;
            if (e < bestErr) { bestErr = e; num = r.first; den = r.second; }
        }
        if (note) *note = "the frame rate measured from the frames' durations (" + std::to_string(m.fps).substr(0, 7) + " fps) replaces the declared " + std::to_string(declared).substr(0, 7);
        vi.fpsNum = num; vi.fpsDen = den;
    }
    return key;
}

class VsContent {
public:
    void Init(const std::string& key, const VSVideoInfo& declared, int createdClass, bool createdCg = false)
    {
        m_createdCg = createdCg;
        m_key = key; m_declared = declared.fpsNum > 0 && declared.fpsDen > 0 ? double(declared.fpsNum) / double(declared.fpsDen) : 0.0;
        m_createdClass = createdClass; m_det.Reset(); m_iv.Reset(); m_t = 0; m_last = -2;
    }
    // one source frame, in the order the filter reads them (s = source index)
    void Push(const VSAPI* a, const VSFrame* f, int64_t s)
    {
        if (!f || s == m_last) return;
        const bool disc = s != m_last + 1;
        m_last = s;
        const VSMap* p = a->getFramePropertiesRO(f);
        int e1 = 0, e2 = 0;
        const int64_t dn = a->mapGetInt(p, "_DurationNum", 0, &e1), dd = a->mapGetInt(p, "_DurationDen", 0, &e2);
        const double dur = !e1 && !e2 && dn > 0 && dd > 0 ? double(dn) / double(dd) : (m_declared > 0 ? 1.0 / m_declared : 1.0 / 23.976);
        if (disc) m_det.Discontinuity();
        bool changed = m_iv.Push(m_t, disc);
        const VSVideoFormat* vf = a->getVideoFrameFormat(f);
        const int w = a->getFrameWidth(f, 0), h = a->getFrameHeight(f, 0);
        if (vf && vf->colorFamily == cfYUV && vf->subSamplingW == 1 && vf->subSamplingH == 1 && vf->sampleType == stInteger) {
            using namespace pushmpc::content;
            const Cells c = vf->bytesPerSample == 1
                ? CellsFromYuv(a->getReadPtr(f, 0), a->getStride(f, 0), a->getReadPtr(f, 1), a->getReadPtr(f, 2), a->getStride(f, 1), w, h, 8, true)
                : CellsFromYuv(reinterpret_cast<const uint16_t*>(a->getReadPtr(f, 0)), a->getStride(f, 0) / 2, reinterpret_cast<const uint16_t*>(a->getReadPtr(f, 1)),
                               reinterpret_cast<const uint16_t*>(a->getReadPtr(f, 2)), a->getStride(f, 1) / 2, w, h, vf->bitsPerSample, true);
#if defined(PUSHMPC_CONTENT_MODEL) && PUSHMPC_CONTENT_MODEL
            {   // the image model: a thumbnail about once a second on its own thread, the latest result into the detector
                if (!m_model) m_model = std::make_unique<ContentModel>([](const std::string&) {});
                double p, pt;
                double pc; if (m_model->Take(p, pt, pc)) m_det.SetImageEvidence(p, pt, pc);
                if (!m_model->Failed() && (m_modelNext < 0 || m_t >= m_modelNext || m_t < m_modelNext - 5.0)) {
                    m_modelNext = m_t + 1.0;
                    auto in = vf->bytesPerSample == 1
                        ? ClipInput(a->getReadPtr(f, 0), a->getStride(f, 0), a->getReadPtr(f, 1), a->getReadPtr(f, 2), a->getStride(f, 1), w, h, 8, true)
                        : ClipInput(reinterpret_cast<const uint16_t*>(a->getReadPtr(f, 0)), a->getStride(f, 0) / 2, reinterpret_cast<const uint16_t*>(a->getReadPtr(f, 1)),
                                    reinterpret_cast<const uint16_t*>(a->getReadPtr(f, 2)), a->getStride(f, 1) / 2, w, h, vf->bitsPerSample, true);
                    if (!in.empty()) m_model->Offer(std::move(in), m_t);
                }
            }
#endif
            if (m_det.Push(c, m_t) && (m_det.Current().cls != m_cls || m_det.Current().cg != m_cg)) { m_cls = m_det.Current().cls; m_cg = m_det.Current().cg; changed = true; }
        }
        m_t += dur;
        if (changed) Publish();
    }
    int Class() const { return m_cls; }
    bool Cg() const { return m_cg; }
    double Fps() const { return m_iv.fps(); }
private:
    void Publish()
    {
        StreamMemo m; m.fps = m_iv.fps(); m.cls = m_cls; m.cg = m_cg;
        { std::lock_guard<std::mutex> lk(MemoLock()); Memo()[m_key] = m; }
#if defined(_WIN32)
        char tmp[MAX_PATH] = {}; GetTempPathA(MAX_PATH, tmp);
        const std::string path = std::string(tmp) + "pushmpc-vs-" + std::to_string(GetCurrentProcessId()) + ".txt";
#else
        const char* t = std::getenv("TMPDIR");
        const std::string path = std::string(t && *t ? t : "/tmp") + "/pushmpc-vs-" + std::to_string(getpid()) + ".txt";
#endif
        if (FILE* f = std::fopen(path.c_str(), "w")) {
            // reload = 1 when the running instance was created with another rate or class (pushmpc.lua re-adds the filter)
            const bool rate = m.fps > 1.0 && m_declared > 0 && std::fabs(m.fps - m_declared) / m.fps > 0.05;
            const bool cls = m.cls != 0 && ((m.cls == pushmpc::content::kDrawn) != (m_createdClass == pushmpc::content::kDrawn) ||
                                            (m.cls == pushmpc::content::kDrawn && m.cg != m_createdCg));   // only what changes Auto
            std::fprintf(f, "fps=%.4f\ndeclared=%.4f\nclass=%d\nclass_name=%s\ncg=%d\nreload=%d\n", m.fps, m_declared, m.cls, pushmpc::content::ClassName(m.cls), m.cg ? 1 : 0, (rate || cls) ? 1 : 0);
            std::fclose(f);
        }
    }
    pushmpc::content::ContentDetector m_det;
    pushmpc::content::IntervalMeter m_iv;
    std::string m_key;
    double m_declared = 0, m_t = 0;
    int64_t m_last = -2;
    int m_cls = 0, m_createdClass = 0;
    bool m_cg = false, m_createdCg = false;
#if defined(PUSHMPC_CONTENT_MODEL) && PUSHMPC_CONTENT_MODEL
    std::unique_ptr<pushmpc::content::ContentModel> m_model;
    double m_modelNext = -1;
#endif
};

} // namespace pmvs
