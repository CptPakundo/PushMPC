// ContentDetect in the engine module (pmeng): what the picture is (drawn animation, rendered 3D CG among the drawn,
// live action, game footage), measured on the frames the filter submits, never from names or paths.
//   - frames in host memory: the cell grid and, about once a second, the image model's thumbnail straight from the planes
//     (Submit's thread, ~2 ms at 1080p for the cells);
//   - frames on the GPU (D3D11, Vulkan zero copy): the backend's compute pass makes the same cells and thumbnail as packed
//     words and reads back only those (content_detect.h, "GPU taps"); never the frame itself.
// The detector itself (~4 ms mean, ~25 ms worst at a window) runs on this tap's own thread; the image model on its own
// (content_model.h, when the module links pmml). The class reaches the backend through Poll() on Submit's thread.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "engine.h"
#include "pushmpc/content_detect.h"
#include "pushmpc/content_model.h"

namespace pme {

class ContentTap {
public:
    struct Snapshot { int cls = 0; bool cg = false; float conf = 0.f; uint32_t serial = 0; };
    ContentTap() = default;
    ~ContentTap()
    {
        { std::lock_guard<std::mutex> lk(m_mx); m_stop = true; }
        m_cv.notify_all();
        if (m_thread.joinable()) m_thread.join();
    }
    ContentTap(const ContentTap&) = delete;
    ContentTap& operator=(const ContentTap&) = delete;

    // the image model wants a thumbnail now (about once a second of stream time)
    bool alwaysClip = false;   // tests: a thumbnail with every frame
    bool WantClip(double t)
    {
        if (alwaysClip) return true;
#if defined(PUSHMPC_CONTENT_MODEL) && PUSHMPC_CONTENT_MODEL
        if (!m_model) m_model = std::make_unique<pushmpc::content::ContentModel>([](const std::string& m) { Log(m); });
        if (m_model->Failed()) return false;
        if (m_clipNext < 0 || t >= m_clipNext || t < m_clipNext - 5.0) { m_clipNext = t + 1.0; return true; }
#else
        (void)t;
#endif
        return false;
    }
    void OfferClip(std::vector<float> in, double t)
    {
#if defined(PUSHMPC_CONTENT_MODEL) && PUSHMPC_CONTENT_MODEL
        if (m_model && !in.empty()) m_model->Offer(std::move(in), t);
#else
        (void)in; (void)t;
#endif
    }
    // one source frame's cells, in presentation order (t: stream seconds); disc: a seek or gap before it
    void Push(pushmpc::content::Cells c, double t, bool disc)
    {
        if (!c.ok()) return;
        std::lock_guard<std::mutex> lk(m_mx);
        if (!m_thread.joinable()) m_thread = std::thread([this] { Loop(); });
        if (m_q.size() >= 8) { m_q.clear(); disc = true; ++m_dropped; }   // the detector fell behind: a gap
        m_q.push_back(Job{ std::move(c), t, disc || m_discNext });
        m_discNext = false;
        m_cv.notify_all();
    }
    void Discontinuity() { std::lock_guard<std::mutex> lk(m_mx); m_discNext = true; }
    // Submit's thread: the class now (serial changes with every decided change)
    Snapshot Poll() const { std::lock_guard<std::mutex> lk(m_mx); return m_snap; }
    std::string Describe() const
    {
        std::lock_guard<std::mutex> lk(m_mx);
        char b[256];
        std::snprintf(b, sizeof(b), "content: %s%s, %llu frames measured (%llu dropped), %u class changes", pushmpc::content::ClassName(m_snap.cls),
                      m_snap.cg ? " (rendered 3D CG)" : "", (unsigned long long)m_frames, (unsigned long long)m_dropped, m_changes);
        return b;
    }

private:
    struct Job { pushmpc::content::Cells c; double t; bool disc; };
    void Loop()
    {
        using namespace pushmpc::content;
        while (true) {
            Job j;
            {
                std::unique_lock<std::mutex> lk(m_mx);
                m_cv.wait(lk, [&] { return m_stop || !m_q.empty(); });
                if (m_stop) return;
                j = std::move(m_q.front()); m_q.pop_front();
            }
            if (j.disc) m_det.Discontinuity();
#if defined(PUSHMPC_CONTENT_MODEL) && PUSHMPC_CONTENT_MODEL
            double p, pt, pc;
            if (m_model && m_model->Take(p, pt, pc)) m_det.SetImageEvidence(p, pt, pc);
#endif
            const bool ev = m_det.Push(j.c, j.t);
            std::lock_guard<std::mutex> lk(m_mx);
            ++m_frames;
            if (!ev) continue;
            const State& s = m_det.Current();
            if (s.cls != m_snap.cls || s.cg != m_snap.cg) {
                m_snap.cls = s.cls; m_snap.cg = s.cg; ++m_snap.serial; m_changes = s.changes;
                char b[320];
                std::snprintf(b, sizeof(b), "content: %s%s (%.0f %%), measured in the picture at %.1f s; drawn probability %.2f, evidence %.1f%s",
                              ClassName(s.cls), s.cg ? " (rendered 3D CG)" : "", 100.0 * s.confidence, j.t, s.drawnShare, s.logOdds,
                              s.imageEvidence ? " (with the image model)" : " (hand-built cues only)");
                Log(b);
            }
            m_snap.conf = s.confidence;
        }
    }
    mutable std::mutex m_mx;
    std::condition_variable m_cv;
    std::thread m_thread;
    std::deque<Job> m_q;
    bool m_stop = false, m_discNext = false;
    Snapshot m_snap;
    uint64_t m_frames = 0, m_dropped = 0;
    uint32_t m_changes = 0;
    pushmpc::content::ContentDetector m_det;   // the tap's thread only
#if defined(PUSHMPC_CONTENT_MODEL) && PUSHMPC_CONTENT_MODEL
    std::unique_ptr<pushmpc::content::ContentModel> m_model;
    double m_clipNext = -1;
#endif
};

// Frames in host memory (pmeng_image, PMENG_MEM_CPU): cells + thumbnail from the planes. Range from the frame's tag
// (FFmpeg AVCOL_RANGE_JPEG = 2: full), else video range.
inline void TapCpuFrame(ContentTap& tap, const pmeng_image& src, double t, bool disc)
{
    using namespace pushmpc::content;
    const int w = int(src.width), h = int(src.height);
    if (src.mem != PMENG_MEM_CPU || !src.data[0] || !src.data[1] || w < 16 || h < 16) return;
    const bool limited = src.range != 2;
    const bool two = src.fmt == PMENG_FMT_NV12 || src.fmt == PMENG_FMT_P010;
    const bool deep = src.fmt == PMENG_FMT_P010 || src.fmt == PMENG_FMT_YUV420P10;
    if (!two && src.fmt != PMENG_FMT_YUV420P && src.fmt != PMENG_FMT_YUV420P10) return;
    if (!two && !src.data[2]) return;
    const int bits = src.fmt == PMENG_FMT_P010 ? 16 : src.fmt == PMENG_FMT_YUV420P10 ? 10 : 8;
    if (disc) tap.Discontinuity();
    if (deep) {
        auto* y = reinterpret_cast<const uint16_t*>(src.data[0]); auto* u = reinterpret_cast<const uint16_t*>(src.data[1]);
        auto* v = two ? nullptr : reinterpret_cast<const uint16_t*>(src.data[2]);
        const ptrdiff_t ys = src.linesize[0] / 2, cs = src.linesize[1] / 2;
        if (tap.WantClip(t)) tap.OfferClip(ClipInput(y, ys, u, v, cs, w, h, bits, limited), t);
        tap.Push(CellsFromYuv(y, ys, u, v, cs, w, h, bits, limited), t, false);
    } else {
        const uint8_t* v = two ? nullptr : src.data[2];
        if (tap.WantClip(t)) tap.OfferClip(ClipInput(src.data[0], src.linesize[0], src.data[1], v, src.linesize[1], w, h, 8, limited), t);
        tap.Push(CellsFromYuv(src.data[0], src.linesize[0], src.data[1], v, src.linesize[1], w, h, 8, limited), t, false);
    }
}

} // namespace pme
