// pushmpc.Enhance, backend "ml": PushFlow (RIFE) through pmml (ONNX Runtime with the best EP for the GPU, or ncnn on
// Vulkan) behind the same Pipeline (schedule.h) as the other backends, in pushcore's output order: for each submitted
// source s, the generated frames of the pair (s - 1, s), then s's real frame.
// Per pair the network runs once ("flow once": k - 1 > 1 timesteps rebuilt from its grids) or per timestep, as Auto
// decided. A provisional cut check (luma difference of 1/8-size pictures) shows the nearer real frame at a hard cut
// until the portable engine's cut guard (T1) is shared: RIFE has no cut awareness of its own.
// Frame: a VSFrame* in the plugin.
#pragma once
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "pmml/pmml.h"
// the held-drawing repair (pushmpc/src/HeldRepair.h); a tree without it builds without the repair
#if __has_include("../../../pushmpc/src/HeldRepair.h")
#include "../../../pushmpc/src/HeldRepair.h"
#define PMVS_ML_REGION_REPAIR 1
#endif
#include "portable_engine.h"   // YuvView / YuvOut
#include "schedule.h"

namespace pmvs::ml {

using portable::YuvOut;
using portable::YuvView;

struct Planes16 { std::vector<uint16_t> p[3]; int w = 0, h = 0; };

inline void ToPlanes16(const YuvView& v, Planes16& o)
{
    o.w = v.width; o.h = v.height;
    for (int i = 0; i < 3; ++i) {
        const int w = i ? v.width / 2 : v.width, h = i ? v.height / 2 : v.height;
        o.p[i].resize(size_t(w) * h);
        for (int y = 0; y < h; ++y) {
            const uint8_t* row = v.data[i] + y * v.stride[i];
            uint16_t* dst = &o.p[i][size_t(y) * w];
            if (v.bits > 8) std::memcpy(dst, row, size_t(w) * 2);
            else for (int x = 0; x < w; ++x) dst[x] = row[x];
        }
    }
}

inline void FromPlanes16(const Planes16& s, int bits, const YuvOut& o)
{
    for (int i = 0; i < 3; ++i) {
        const int w = i ? s.w / 2 : s.w, h = i ? s.h / 2 : s.h;
        for (int y = 0; y < h; ++y) {
            const uint16_t* src = &s.p[i][size_t(y) * w];
            uint8_t* row = o.data[i] + y * o.stride[i];
            if (bits > 8) std::memcpy(row, src, size_t(w) * 2);
            else for (int x = 0; x < w; ++x) row[x] = uint8_t(src[x]);
        }
    }
}

// a hard cut: the mean absolute luma difference of 1/8-size pictures above `threshold` of full scale (a pan of a
// textured picture stays well below it at 1/8 size; a cut to unrelated content goes above)
inline bool LooksLikeCut(const Planes16& a, const Planes16& b, int bits, double threshold = 0.12)
{
    const int f = 8, w = a.w / f, h = a.h / f;
    if (w < 4 || h < 4) return false;
    double sum = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double sa = 0, sb = 0;
            for (int dy = 0; dy < f; dy += 2)
                for (int dx = 0; dx < f; dx += 2) {
                    const size_t i = size_t(y * f + dy) * a.w + x * f + dx;
                    sa += a.p[0][i]; sb += b.p[0][i];
                }
            sum += std::fabs(sa - sb) / 16.0;
        }
    return sum / (double(w) * h) / double((1 << bits) - 1) > threshold;
}

template <class Frame> class MlBackend : public Backend<Frame> {
public:
    std::function<bool(int64_t s, YuvView& v)> source;
    std::function<bool(YuvOut& o, Frame& f)> alloc;
    std::function<void(Frame f)> release;
    std::function<void(int64_t pts, bool generated, Frame f)> tag;

    std::unique_ptr<pmml::Interpolator> interp;
    Grid grid;
    int bits = 8;
    bool flowOnce = false;
    uint64_t generated = 0, cuts = 0;
    double lastMs = 0;
    // The held-drawing repair (HeldRepair.h): frames go out kLookahead (3) source frames late; before frame q goes out
    // its held parts are laid in from their own pair (pmml renderMap: one network run per pair, a phase per pixel; or
    // render at the pair's one phase). The caller sets the Pipeline's lookahead to RepairLookahead() and `frames`.
    bool regionRepair = false;
    int64_t frames = 0;
    static constexpr int kRepairLookahead = 3;
    int RepairLookahead() const { return regionRepair ? kRepairLookahead : 0; }
    // the stream ends before frame e (an unknown-length source, eof_guard.h): the repair's waiting frames go out
    bool End(int64_t e, std::string& err)
    {
        if (e > 0 && (frames <= 0 || e < frames)) frames = e;
#ifdef PMVS_ML_REGION_REPAIR
        if (regionRepair && m_last >= e - 1) return EmitUpTo(m_last, err);
#endif
        (void)err;
        return true;
    }
    struct RepairCounts { uint64_t frames = 0, parts = 0, pairs = 0, mapRuns = 0, gated = 0, overBudget = 0; double ms = 0; } repairCounts;

    bool CanSubmit() override { return true; }
    bool Submit(int64_t s, int64_t pts, bool discontinuity, std::string& err) override
    {
#ifdef PMVS_ML_REGION_REPAIR
        if (regionRepair) return SubmitDelayed(s, pts, discontinuity, err);
#endif
        YuvView v;
        if (!source || !source(s, v)) { err = "ml: source frame " + std::to_string(s) + " is not available"; return false; }
        const bool contiguous = !discontinuity && m_last == s - 1 && m_have;
        std::swap(m_prev, m_cur);
        ToPlanes16(v, m_cur);
        if (contiguous && grid.k > 1) {
            const bool cut = LooksLikeCut(m_prev, m_cur, bits);
            if (cut) ++cuts;
            pmml::Frame A, B;
            for (int i = 0; i < 3; ++i) {
                A.plane[i] = m_prev.p[i].data(); B.plane[i] = m_cur.p[i].data();
                A.stride[i] = B.stride[i] = i ? m_cur.w / 2 : m_cur.w;
            }
            const auto t0 = std::chrono::steady_clock::now();
            if (!cut && !interp->setPair(A, B, flowOnce ? grid.k - 1 : 1, &err)) return false;
            for (int j = 1; j < grid.k; ++j) {
                Frame f{}; YuvOut o;
                if (!alloc(o, f)) { err = "ml: could not allocate an output frame"; return false; }
                const float t = float(j) / float(grid.k);
                if (cut) FromPlanes16(t < 0.5f ? m_prev : m_cur, bits, o);
                else {
                    m_out.w = m_cur.w; m_out.h = m_cur.h;
                    for (int i = 0; i < 3; ++i) m_out.p[i].resize(m_cur.p[i].size());
                    pmml::OutFrame O;
                    for (int i = 0; i < 3; ++i) { O.plane[i] = m_out.p[i].data(); O.stride[i] = i ? m_cur.w / 2 : m_cur.w; }
                    if (!interp->render(t, O, &err)) { release(f); return false; }
                    FromPlanes16(m_out, bits, o);
                }
                const int64_t p = grid.Pts(s - 1) + int64_t(std::llround(double(j) * grid.srcDur100ns / grid.k));
                if (tag) tag(p, true, f);
                m_queue.push_back({ p, true, f });
                ++generated;
            }
            lastMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        }
        Frame f{}; YuvOut o;
        if (!alloc(o, f)) { err = "ml: could not allocate an output frame"; return false; }
        FromPlanes16(m_cur, bits, o);
        if (tag) tag(pts, false, f);
        m_queue.push_back({ pts, false, f });
        m_last = s;
        m_have = true;
        return true;
    }
    bool Poll(uint32_t, std::vector<Out<Frame>>& out, std::string&) override
    {
        for (auto& q : m_queue) out.push_back(q);
        m_queue.clear();
        return true;
    }
    bool Flush(std::string&) override
    {
        for (auto& q : m_queue) Release(q.frame);
        m_queue.clear();
        m_have = false;
        m_last = -1000000;
#ifdef PMVS_ML_REGION_REPAIR
        m_hist.clear(); m_ptsOf.clear(); m_rs.Reset(); m_next = -1; m_first = -1;
#endif
        return true;
    }
    void Release(Frame f) override { if (release) release(f); }
    uint64_t NowMs() override
    {
        return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }

private:
    // the generated frames of the pair (a, b) and b's real frame (b = a + 1 unless the stream started at b)
    bool EmitPair(const Planes16* a, const Planes16& b, int64_t sb, int64_t ptsB, std::string& err)
    {
        if (a && grid.k > 1) {
            const bool cut = LooksLikeCut(*a, b, bits);
            if (cut) ++cuts;
            pmml::Frame A, B;
            for (int i = 0; i < 3; ++i) { A.plane[i] = a->p[i].data(); B.plane[i] = b.p[i].data(); A.stride[i] = B.stride[i] = i ? b.w / 2 : b.w; }
            const auto t0 = std::chrono::steady_clock::now();
            if (!cut && !interp->setPair(A, B, flowOnce ? grid.k - 1 : 1, &err)) return false;
            for (int j = 1; j < grid.k; ++j) {
                Frame f{}; YuvOut o;
                if (!alloc(o, f)) { err = "ml: could not allocate an output frame"; return false; }
                const float t = float(j) / float(grid.k);
                if (cut) FromPlanes16(t < 0.5f ? *a : b, bits, o);
                else {
                    m_out.w = b.w; m_out.h = b.h;
                    for (int i = 0; i < 3; ++i) m_out.p[i].resize(b.p[i].size());
                    pmml::OutFrame O;
                    for (int i = 0; i < 3; ++i) { O.plane[i] = m_out.p[i].data(); O.stride[i] = i ? b.w / 2 : b.w; }
                    if (!interp->render(t, O, &err)) { release(f); return false; }
                    FromPlanes16(m_out, bits, o);
                }
                const int64_t p = grid.Pts(sb - 1) + int64_t(std::llround(double(j) * grid.srcDur100ns / grid.k));
                if (tag) tag(p, true, f);
                m_queue.push_back({ p, true, f });
                ++generated;
            }
            lastMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        }
        Frame f{}; YuvOut o;
        if (!alloc(o, f)) { err = "ml: could not allocate an output frame"; return false; }
        FromPlanes16(b, bits, o);
        if (tag) tag(ptsB, false, f);
        m_queue.push_back({ ptsB, false, f });
        return true;
    }
#ifdef PMVS_ML_REGION_REPAIR
    bool SubmitDelayed(int64_t s, int64_t pts, bool discontinuity, std::string& err)
    {
        using namespace pushmpc::held;
        YuvView v;
        if (!source || !source(s, v)) { err = "ml: source frame " + std::to_string(s) + " is not available"; return false; }
        if (discontinuity || m_last != s - 1 || m_next < 0) {   // a seek or a gap: what waits goes out as it is
            if (!EmitUpTo(m_last, err)) return false;
            m_hist.clear(); m_ptsOf.clear(); m_rs.Reset(); m_next = s; m_first = s;
        }
        m_last = s;
        Planes16& cur = m_hist[s];
        ToPlanes16(v, cur);
        m_ptsOf[s] = pts;
        auto prev = m_hist.find(s - 1);
        CellGrid g = CellsFromPlanes(cur.p[0].data(), cur.w, cur.p[1].data(), cur.p[2].data(), cur.w / 2, prev != m_hist.end() ? prev->second.p[0].data() : nullptr, cur.w, cur.h, bits);
        bool dup = false;
        if (const CellGrid* pg = m_rs.Grid(s - 1); pg && pg->v.size() == g.v.size()) {
            DiffStats d; d.valid = true;
            for (size_t i = 0; i < g.v.size(); ++i) {
                const uint32_t q = uint32_t(std::fabs(g.v[i] - pg->v[i]) * 16.f + 0.5f);
                ++d.cells; d.maxQ = std::max(d.maxQ, q); d.sumQ += q; for (int j = 0; j < 4; ++j) d.over[j] += q > kOverCodes[j] * 16;
            }
            dup = IsHeld(d);
        }
        m_rs.Push(s, std::move(g), dup);
        const int64_t q = s - kRepairLookahead;
        if (q >= m_next) { if (!Repair(q, err) || !EmitUpTo(q, err)) return false; }
        if (frames > 0 && s >= frames - 1 && !EmitUpTo(s, err)) return false;   // the clip's end: the rest as it is
        while (!m_hist.empty() && m_hist.begin()->first < s - 8) { m_ptsOf.erase(m_hist.begin()->first); m_hist.erase(m_hist.begin()); }
        return true;
    }
    bool EmitUpTo(int64_t q, std::string& err)
    {
        for (; m_next >= 0 && m_next <= q; ++m_next) {
            auto b = m_hist.find(m_next);
            if (b == m_hist.end()) continue;
            auto a = m_hist.find(m_next - 1);
            if (!EmitPair(m_next > m_first && a != m_hist.end() ? &a->second : nullptr, b->second, m_next, m_ptsOf[m_next], err)) return false;
        }
        return true;
    }
    // frame q's held parts from their pairs (pairs whose frames are still kept), composited on the CPU
    bool Repair(int64_t q, std::string& err)
    {
        using namespace pushmpc::held;
        const auto groups = m_rs.Plan(q, 0.5 * grid.srcDur100ns / 10000.0);
        const CellGrid* g = m_rs.Grid(q);
        auto dst = m_hist.find(q);
        if (groups.empty() || !g || dst == m_hist.end()) return true;
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<std::pair<uint64_t, uint64_t>> pairs;
        for (const auto& gr : groups) if (std::find(pairs.begin(), pairs.end(), std::make_pair(gr.pairFrom, gr.d1)) == pairs.end()) pairs.push_back({ gr.pairFrom, gr.d1 });
        size_t parts = 0;
        for (const auto& pr : pairs) {
            auto a = m_hist.find(int64_t(pr.first)), b = m_hist.find(int64_t(pr.second));
            if (a == m_hist.end() || b == m_hist.end()) continue;
            std::vector<float> tmap, alpha;
            if (!PairPhaseMap(groups, pr.first, pr.second, tmap, alpha)) continue;
            pmml::Frame A, B;
            for (int i = 0; i < 3; ++i) { A.plane[i] = a->second.p[i].data(); B.plane[i] = b->second.p[i].data(); A.stride[i] = B.stride[i] = i ? a->second.w / 2 : a->second.w; }
            m_rep.w = a->second.w; m_rep.h = a->second.h;
            for (int i = 0; i < 3; ++i) m_rep.p[i].resize(a->second.p[i].size());
            pmml::OutFrame O;
            for (int i = 0; i < 3; ++i) { O.plane[i] = m_rep.p[i].data(); O.stride[i] = i ? m_rep.w / 2 : m_rep.w; }
            // one network run at tRef, every part at its own phase (synth_map); else the parts' mean phase per t
            std::string e2;
            bool ok = interp->setPair(A, B, 2, &err) && interp->renderMap(tmap.data(), int(g->w), int(g->h), O, &e2);
            if (ok) ++repairCounts.mapRuns;
            else {
                float tm = 0, n = 0;
                for (const auto& gr : groups) if (gr.pairFrom == pr.first && gr.d1 == pr.second) { tm += gr.t * float(gr.cells); n += float(gr.cells); }
                if (n <= 0 || !interp->setPair(A, B, 1, &err) || !interp->render(tm / n, O, &err)) return false;
            }
            uint16_t* d[3] = { dst->second.p[0].data(), dst->second.p[1].data(), dst->second.p[2].data() };
            const uint16_t* sp[3] = { m_rep.p[0].data(), m_rep.p[1].data(), m_rep.p[2].data() };
            const ptrdiff_t ds[3] = { dst->second.w, dst->second.w / 2, dst->second.w / 2 };
            CompositePlanes(d, ds, sp, ds, dst->second.w, dst->second.h, alpha, int(g->w), int(g->h), int(g->cell));
            ++repairCounts.pairs;
            for (const auto& gr : groups) parts += gr.pairFrom == pr.first && gr.d1 == pr.second;
        }
        if (!parts) return true;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        m_rs.Cost(ms, parts);
        repairCounts.frames = m_rs.stats.frames; repairCounts.parts = m_rs.stats.parts; repairCounts.ms = m_rs.stats.ms;
        repairCounts.gated = m_rs.stats.gated; repairCounts.overBudget = m_rs.stats.overBudget;
        return true;
    }
    pushmpc::held::RepairScheduler m_rs;
    std::map<int64_t, Planes16> m_hist;
    std::map<int64_t, int64_t> m_ptsOf;
    Planes16 m_rep;
    int64_t m_next = -1, m_first = -1;
#endif
    Planes16 m_prev, m_cur, m_out;
    bool m_have = false;
    int64_t m_last = -1000000;
    std::vector<Out<Frame>> m_queue;
};

} // namespace pmvs::ml
