// pushmpc_content: ContentDetect and its image model behind a C interface (see include/pushmpc_content.h).
//
// The caller's thread takes the cell grid of each frame (CellsFromYuv) and, about once a second of stream time, the
// image model's thumbnail (ClipInput, handed to ContentModel's own thread); the detector (ContentDetector::Push) runs on
// a worker thread of this library, as the MPC-HC filter's ContentLoop does, because one window costs a few ms. A queue of
// at most 8 frames: when the detector falls behind, the queue is dropped and the next frame counts as a gap.
#define PMC_BUILD 1
#include "pushmpc_content.h"
#include "pushmpc/content_detect.h"
#include "pushmpc/content_model.h"   // the image model (only with PUSHMPC_CONTENT_MODEL: pmml linked)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>

namespace {
using namespace pushmpc::content;
using Clock = std::chrono::steady_clock;

#if defined(PUSHMPC_CONTENT_MODEL) && PUSHMPC_CONTENT_MODEL
// the model's result: with the CG probability where this ContentModel returns one, else the drawn probability alone
template <class M>
auto TakeEvidence(M& m, double& p, double& pt, double& cg, int) -> decltype(m.Take(p, pt, cg)) { return m.Take(p, pt, cg); }
template <class M>
bool TakeEvidence(M& m, double& p, double& pt, double& cg, long) { cg = -1.0; return m.Take(p, pt); }
#endif

uint32_t PolicyBits(int cls, bool cg)
{
    const DrawnPolicy& p = PolicyFor(cls, cg);
    return (p.holdRepair ? PMC_POLICY_HOLD_REPAIR : 0u) | (p.animeUpscaler ? PMC_POLICY_ANIME_UPSCALER : 0u) |
           (p.animeUpscalerDs ? PMC_POLICY_ANIME_UPSCALER_DS : 0u) | (p.signHold ? PMC_POLICY_SIGN_HOLD : 0u) |
           (p.lineArt ? PMC_POLICY_LINE_ART : 0u) | (p.animeDeband ? PMC_POLICY_ANIME_DEBAND : 0u);
}

struct Job { Cells cells; double t = 0; bool disc = false; };
} // namespace

struct pmc_ctx {
    pmc_log_fn logFn = nullptr;
    void* logUser = nullptr;
    uint32_t flags = 0;

    // worker
    std::mutex mx;
    std::condition_variable cv;
    std::deque<Job> queue;
    std::thread worker;
    bool stop = false;
    // the caller's evidence (pmc_set_image_evidence), applied by the detector's thread before its next frame
    bool evPending = false;
    double evP = 0.5, evT = 0, evCg = -1;

    ContentDetector det;   // detector thread only (the caller's thread with PMC_FLAG_SYNC)
    int loggedCls = kUnknown, loggedCg = 0, loggedModel = -1;   // the first class line is the first decision

    // published state
    std::mutex stMx;
    pmc_state st{};
    double callerSum = 0, detectSum = 0;

#if defined(PUSHMPC_CONTENT_MODEL) && PUSHMPC_CONTENT_MODEL
    std::unique_ptr<ContentModel> model;   // created with the context (it loads on its own thread); fixed for its life
    double modelNext = -1;
    std::atomic<bool> modelRan{ false };
#endif

    void Log(const char* fmt, ...)
    {
        if (!logFn) return;
        char line[512];
        va_list a; va_start(a, fmt); std::vsnprintf(line, sizeof(line), fmt, a); va_end(a);
        logFn(logUser, line);
    }
    int ModelStatus()
    {
        if (flags & PMC_FLAG_NO_MODEL) return PMC_MODEL_OFF;
#if defined(PUSHMPC_CONTENT_MODEL) && PUSHMPC_CONTENT_MODEL
        if (!model) return PMC_MODEL_FAILED;
        if (model->Failed()) return PMC_MODEL_FAILED;
        return modelRan.load() ? PMC_MODEL_RUNNING : PMC_MODEL_STARTING;
#else
        return PMC_MODEL_NOT_BUILT;
#endif
    }
    // one frame through the detector (detector thread)
    void Detect(Job& j)
    {
        if (j.disc) det.Discontinuity();
        {
            std::lock_guard<std::mutex> lk(mx);
            if (evPending) { det.SetImageEvidence(evP, evT, evCg); evPending = false; }
        }
#if defined(PUSHMPC_CONTENT_MODEL) && PUSHMPC_CONTENT_MODEL
        double p, pt, cg;
        if (model && TakeEvidence(*model, p, pt, cg, 0)) { det.SetImageEvidence(p, pt, cg); modelRan = true; }
#endif
        const auto t0 = Clock::now();
        const bool evaluated = det.Push(j.cells, j.t);
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        const int ms_model = ModelStatus();
        const State& s = det.Current();
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(stMx);
            ++st.processed; detectSum += ms;
            st.detect_ms = detectSum / double(st.processed);
            st.detect_ms_max = std::max(st.detect_ms_max, ms);
            if (ms_model != st.model) { st.model = ms_model; changed = true; }
#if defined(PUSHMPC_CONTENT_MODEL) && PUSHMPC_CONTENT_MODEL
            if (model) st.model_ms = float(model->LastMs());
#endif
            if (evaluated) {
                if (s.cls != st.cls || int(s.cg) != st.cg) changed = true;
                st.cls = s.cls; st.cg = s.cg ? 1 : 0; st.policy = PolicyBits(s.cls, s.cg);
                st.confidence = s.confidence; st.drawn_share = s.drawnShare; st.cg_share = s.cgShare;
                st.image_evidence = s.imageEvidence ? 1 : 0; st.changes = s.changes; st.windows = s.windows;
                st.first_decision_sec = s.firstDecisionSec;
            }
            if (changed) ++st.serial;
        }
        if (ms_model != loggedModel) {
            loggedModel = ms_model;
            static const char* kModel[] = { "not in this build: the hand-built cues decide", "off: the hand-built cues decide",
                                            "starting (the hand-built cues decide meanwhile)", "running", "not available: the hand-built cues decide" };
            Log("content: image model %s", kModel[std::clamp(ms_model, 0, 4)]);
        }
        if (evaluated && (s.cls != loggedCls || int(s.cg) != loggedCg)) {
            loggedCls = s.cls; loggedCg = s.cg ? 1 : 0;
            Log("content: %s%s (%.0f %%), measured in the picture at %.1f s; drawn probability %.2f, evidence %.1f%s; class changes so far %u",
                ClassName(s.cls), s.cls == kDrawn ? (s.cg ? ", rendered 3D CG" : ", hand-drawn") : "", 100.0 * s.confidence, j.t,
                s.drawnShare, s.logOdds, s.imageEvidence ? " (with the image model)" : " (hand-built cues only)", s.changes);
        }
    }
    void Loop()
    {
        while (true) {
            Job j;
            {
                std::unique_lock<std::mutex> lk(mx);
                cv.wait(lk, [&] { return stop || !queue.empty(); });
                if (stop) return;
                j = std::move(queue.front()); queue.pop_front();
            }
            try { Detect(j); } catch (...) { Log("content: the detector failed on a frame (out of memory?)"); }
        }
    }

    template <class T>
    int Push(const T* y, ptrdiff_t ys, const T* u, const T* v, ptrdiff_t cs, int w, int h, int bits, bool limited, double pts, bool disc)
    {
        const auto t0 = Clock::now();
        Job j;
        j.t = pts; j.disc = disc;
        j.cells = CellsFromYuv(y, ys, u, v, cs, w, h, bits, limited);
        if (!j.cells.ok()) return -1;
#if defined(PUSHMPC_CONTENT_MODEL) && PUSHMPC_CONTENT_MODEL
        if (model) {   // a 224x224 thumbnail about once a second, run on the model's own thread
            if (!model->Failed() && (modelNext < 0 || pts >= modelNext || pts < modelNext - 5.0)) {
                modelNext = pts + 1.0;
                auto in = ClipInput(y, ys, u, v, cs, w, h, bits, limited);
                if (!in.empty()) model->Offer(std::move(in), pts);
            }
        }
#endif
        if (flags & PMC_FLAG_SYNC) Detect(j);
        else {
            std::lock_guard<std::mutex> lk(mx);
            if (!worker.joinable()) worker = std::thread([this] { Loop(); });
            if (queue.size() >= 8) {   // the detector fell behind: a gap
                std::lock_guard<std::mutex> sl(stMx);
                st.dropped += queue.size();
                queue.clear(); j.disc = true;
            }
            queue.push_back(std::move(j));
            cv.notify_all();
        }
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        std::lock_guard<std::mutex> sl(stMx);
        ++st.frames; callerSum += ms;
        st.caller_ms = callerSum / double(st.frames);
        return 0;
    }
};

extern "C" {

PMC_API uint32_t pmc_abi(void) { return PMC_ABI; }

PMC_API pmc_ctx* pmc_create(const pmc_config* cfg)
{
    if (!cfg || cfg->struct_size < sizeof(pmc_config) || cfg->abi != PMC_ABI) return nullptr;
    pmc_ctx* c = new (std::nothrow) pmc_ctx();
    if (!c) return nullptr;
    c->logFn = cfg->log; c->logUser = cfg->log_user; c->flags = cfg->flags;
    c->st.struct_size = sizeof(pmc_state);
    c->st.cg_share = -1.f; c->st.model_ms = -1.f; c->st.first_decision_sec = -1;
#if defined(PUSHMPC_CONTENT_MODEL) && PUSHMPC_CONTENT_MODEL
    if (!(c->flags & PMC_FLAG_NO_MODEL)) {
        try { c->model = std::make_unique<ContentModel>([c](const std::string& m) { c->Log("%s", m.c_str()); }); }
        catch (...) { c->model.reset(); }
    }
#endif
    c->st.model = c->ModelStatus();
    return c;
}

PMC_API void pmc_destroy(pmc_ctx* c)
{
    if (!c) return;
    {
        std::lock_guard<std::mutex> lk(c->mx);
        c->stop = true;
    }
    c->cv.notify_all();
    if (c->worker.joinable()) c->worker.join();
#if defined(PUSHMPC_CONTENT_MODEL) && PUSHMPC_CONTENT_MODEL
    c->model.reset();   // joins the model's thread
#endif
    delete c;
}

PMC_API int pmc_push_yuv420(pmc_ctx* c, const void* y, ptrdiff_t ystride, const void* u, const void* v, ptrdiff_t cstride,
                            int w, int h, int bits, int limited, double pts, int discontinuity)
{
    if (!c || !y || !u || w < 16 || h < 16 || bits < 8 || bits > 16 || ystride <= 0 || cstride <= 0) return -1;
    try {
        if (bits == 8)
            return c->Push(static_cast<const uint8_t*>(y), ystride, static_cast<const uint8_t*>(u), static_cast<const uint8_t*>(v),
                           cstride, w, h, 8, limited != 0, pts, discontinuity != 0);
        return c->Push(static_cast<const uint16_t*>(y), ystride / 2, static_cast<const uint16_t*>(u), static_cast<const uint16_t*>(v),
                       cstride / 2, w, h, bits, limited != 0, pts, discontinuity != 0);
    } catch (...) {
        return -2;
    }
}

PMC_API int pmc_get_state(pmc_ctx* c, pmc_state* out)
{
    if (!c || !out || out->struct_size < sizeof(uint32_t)) return -1;
    const uint32_t n = std::min<uint32_t>(out->struct_size, uint32_t(sizeof(pmc_state)));
    const int model = c->ModelStatus();   // the model's thread may have finished loading (or failed) between frames
    std::lock_guard<std::mutex> lk(c->stMx);
    if (model != c->st.model) { c->st.model = model; ++c->st.serial; }
    pmc_state s = c->st;
    s.struct_size = n;
    std::memcpy(out, &s, n);
    return 0;
}

PMC_API void pmc_set_image_evidence(pmc_ctx* c, double pdrawn, double pts, double pcg)
{
    if (!c) return;
    std::lock_guard<std::mutex> lk(c->mx);
    c->evPending = true; c->evP = std::clamp(pdrawn, 0.0, 1.0); c->evT = pts; c->evCg = pcg < 0 ? -1.0 : std::min(pcg, 1.0);
}

PMC_API const char* pmc_class_name(int cls) { return ClassName(cls); }

} // extern "C"
