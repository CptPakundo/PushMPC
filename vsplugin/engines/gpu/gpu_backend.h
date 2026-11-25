// SPDX-License-Identifier: GPL-3.0-or-later
// pushmpc.Enhance, backend "portable" on a GPU: the GPU engine (gpu_engine.h) behind the same Pipeline (schedule.h) as
// the other backends. Per source frame: upload, the duplicate test (Dedup), then the outputs Cadence settles (with Dedup
// they come `lookahead` source frames later, see cadence.h). Real frames without upscaling are copied from the source
// frame unchanged (byte for byte); generated frames and upscaled real frames come from the GPU, each with its own
// ticket, and are copied out when the GPU has them (Poll), so the CPU copies one while the GPU makes the next.
//
// The cut guard: PushFPS's scene-cut rule (most blocks changed and motion explains little of it), and
// PushMPC's guard (the idea of the pushcore guard, with this engine's own measures): forward and backward fields that
// disagree on a changed picture make a cut; fields that agree on real motion veto the rule (see CutGuard). A cut
// pair's outputs show the nearer real frame (no morph).
#pragma once
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cmath>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <string>
#include <vector>
#include "cadence.h"
#include "gpu_engine.h"
#include "schedule.h"
#include "subtitle_hold.h"
#include "transitions.h"

namespace pmvs::gpu {

struct CutGuard {
    // Calibrated on the CI benchmark's cut matrix (vsplugin-gpu.yml: 20 junctions between 5 public clips, 300 motion pairs
    // incl. rippling water, a fast pan, zooms, CG animation): forward-backward agreement was <= 0.014 at every junction
    // and >= 0.818 on every motion pair. So:
    //  - PushMPC's guard: a cut when the two fields disagree (agreement < maxAgreement), the picture changed (zero-motion
    //    error > minChange) and motion leaves some of it unexplained (>= minUnexplained): 20/20 junctions, 0/300 false;
    //  - PushFPS's rule (most blocks unexplained) is vetoed when the fields agree on real motion (agreement >=
    //    vetoAgreement with >= vetoMoving of the blocks moving): it fired on 45 of 57 rippling-water pairs that motion
    //    in fact explains (blend-like interpolation is right there), and on no junction the guard missed.
    float maxAgreement = 0.30f, minChange = 8.0f, minUnexplained = 0.10f;
    float vetoAgreement = 0.50f, vetoMoving = 0.20f;
    bool enabled = true;
    // returns 0 no cut, 1 PushFPS's rule, 2 the guard
    int Decide(const PairStats& st)
    {
        if (!enabled) return st.cutRule ? 1 : 0;
        const bool coherent = st.consistent >= vetoAgreement && st.moving >= vetoMoving;
        if (st.cutRule && !coherent) return 1;
        if (st.consistent < maxAgreement && st.zeroCost > minChange && std::max(st.unexplainedF, st.unexplainedB) >= minUnexplained) return 2;
        return 0;
    }
    void Reset() {}
};

// the decision for a pair of real frames (d0, d1), with the thumbnails of d0 - 1, d0, d1, d1 + 1 (any may be null):
// 0 interpolate, 1 cut by PushFPS's rule, 2 cut by the guard, 3 a flash on either side (hold), 4 a dissolve or fade that
// a cut rule fired on (the plain blend instead of a jump). transitions.h says why.
inline int DecidePair(CutGuard& guard, const PairStats& st, const TransitionRules& r, const Thumb* t0m, const Thumb* t0, const Thumb* t1, const Thumb* t1p)
{
    int cut = guard.Decide(st);
    if (cut && ((t0m && t0 && t1 && LinearTransition(*t0m, *t0, *t1, r)) || (t0 && t1 && t1p && LinearTransition(*t0, *t1, *t1p, r)))) cut = 4;
    if ((t0m && t0 && t1 && Flash(*t0m, *t0, *t1, r)) || (t0 && t1 && t1p && Flash(*t0, *t1, *t1p, r))) cut = 3;
    return cut;
}

// Auto's budget on the GPU: one source frame's cost (upload, the duplicate test, motion both ways) and one output's
// (generation, upscaling, the wait and the copy out), measured on a synthetic moving picture at the real size with a
// configured engine (left empty afterwards). The median of `runs` after one warm-up.
struct GpuCalibration { double meMs = 0, genMs = 0; bool ok = false; };
inline GpuCalibration CalibrateGpu(Engine& e, int runs = 3)
{
    GpuCalibration cal;
    const Config& c = e.Cfg();
    const int bps = c.bits > 8 ? 2 : 1, cw = (c.width + 1) / 2, ch = (c.height + 1) / 2;
    std::vector<uint8_t> planes[2][3];
    for (int f = 0; f < 2; ++f)
        for (int i = 0; i < 3; ++i) {
            const int w = i ? cw : c.width, h = i ? ch : c.height;
            planes[f][i].assign(size_t(w) * size_t(h) * size_t(bps), 0);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const int xs = x + f * (i ? 3 : 6);
                    int v = i ? 128 : ((xs * 7 + y * 3) & 255) ^ ((xs / 16 + y / 16) & 1 ? 0x55 : 0);
                    v <<= (c.bits - 8);
                    if (bps == 1) planes[f][i][size_t(y) * size_t(w) + size_t(x)] = uint8_t(v);
                    else std::memcpy(&planes[f][i][(size_t(y) * size_t(w) + size_t(x)) * 2], &v, 2);
                }
        }
    YuvView v[2];
    for (int f = 0; f < 2; ++f) {
        v[f].width = c.width; v[f].height = c.height; v[f].bits = c.bits;
        for (int i = 0; i < 3; ++i) { v[f].data[i] = planes[f][i].data(); v[f].stride[i] = ptrdiff_t(i ? cw : c.width) * bps; }
    }
    const int ow = e.OutWidth(), oh = e.OutHeight(), ocw = (ow + 1) / 2, och = (oh + 1) / 2;
    std::vector<uint8_t> sink[3];
    YuvOut o;
    for (int i = 0; i < 3; ++i) { sink[i].resize(size_t(i ? ocw * och : ow * oh) * size_t(bps)); o.data[i] = sink[i].data(); o.stride[i] = ptrdiff_t(i ? ocw : ow) * bps; }
    auto now = [] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
    std::vector<double> me, gen;
    std::string err;
    for (int r = 0; r <= runs; ++r) {
        const int a = e.Upload(v[0], err);
        const double t0 = now();
        const int b = a >= 0 ? e.Upload(v[1], err) : -1;
        DedupStats ds; PairStats ps;
        const int p = b >= 0 && e.Duplicate(a, b, ds, err) ? e.Estimate(a, b, -1, ps, err) : -1;
        const double t1 = now();
        int out = -1; uint64_t ticket = 0;
        const bool ok = p >= 0 && e.Generate(p, 0.5f, 0, out, ticket, err) && e.Wait(ticket, err);
        if (ok) e.Read(out, o);
        const double t2 = now();
        if (p >= 0) e.ReleasePair(p);
        if (a >= 0) e.ReleaseSlot(a);
        if (b >= 0) e.ReleaseSlot(b);
        if (!ok) return cal;
        if (r > 0) { me.push_back(t1 - t0); gen.push_back(t2 - t1); }
    }
    std::sort(me.begin(), me.end()); std::sort(gen.begin(), gen.end());
    cal.meMs = me[me.size() / 2]; cal.genMs = gen[gen.size() / 2]; cal.ok = true;
    return cal;
}

template <class Frame> class GpuBackend : public Backend<Frame> {
public:
    // the source frame s (valid during Submit): its planes, and a reference the backend keeps until unkeep
    std::function<bool(int64_t s, YuvView& v, void*& keep)> source;
    std::function<void(void* keep)> unkeep;
    // a new output frame (the output size), with its properties from the kept source frame propSrc
    std::function<bool(YuvOut& o, Frame& f, void* propSrc)> alloc;
    std::function<void(Frame f)> release;
    std::function<void(int64_t pts, int kind, Frame f)> tag;   // kind 0 real, 1 generated, 2 held real (a still picture)

    Engine engine;
    Cadence cadence;
    Grid grid;
    CutGuard guard;
    bool dedup = true, subtitleHold = true;
    struct Stats { uint64_t pushed = 0, duplicates = 0, pairs = 0, cuts = 0, guardCuts = 0, generated = 0, holds = 0, heldPairs = 0, heldPixels = 0, flashes = 0, linearBlends = 0; double meMs = 0, waitMs = 0; };
    TransitionRules transitions;
    bool transitionRules = true;   // dissolves and fades blended instead of held, flashes held (transitions.h)
    Stats stats;

    // the lookahead the Pipeline must request (schedule.h SetLookahead)
    int Lookahead() const { return cadence.Lookahead(); }

    bool CanSubmit() override { return true; }
    bool Submit(int64_t s, int64_t pts, bool discontinuity, std::string& err) override
    {
        (void)pts;
        YuvView v; void* keep = nullptr;
        if (!source || !source(s, v, keep)) { err = "GPU engine: source frame " + std::to_string(s) + " is not available"; return false; }
        const bool contiguous = !discontinuity && m_last == s - 1;
        if (!contiguous) { if (!Drop(err)) { Unkeep(keep); return false; } }
        m_last = s;
        m_kept[s] = { keep, v };
        if (transitionRules) m_thumbs[s] = Thumb::Of(v);
        const int slot = engine.Upload(v, err);
        if (slot < 0) return false;
        bool dup = false;
        if (dedup && m_lastDistinctSlot >= 0) {
            DedupStats ds;
            if (!engine.Duplicate(m_lastDistinctSlot, slot, ds, err)) return false;
            dup = ds.duplicate;
        }
        stats.pushed++;
        if (dup) { engine.ReleaseSlot(slot); stats.duplicates++; }
        else {
            m_slotOf[s] = slot;
            m_lastDistinctSlot = slot;
            if (m_prevDistinct >= 0 && !StartPair(m_prevDistinct, s, err)) return false;
            m_prevDistinct = s;
            if (subtitleHold) {
                // the rule runs over the distinct frames in order, on a worker (the frame stays kept until it is used)
                std::shared_future<std::shared_ptr<const HoldFrameYuv>> prev = m_holdChain;
                SubtitleHoldYuv* h = &m_hold;
                m_holdChain = std::async(std::launch::async, [prev, h, v]() { if (prev.valid()) prev.wait(); return h->Frame(v); }).share();
                m_holdOf[s] = m_holdChain;
            }
        }
        cadence.Push(s, dup);
        std::vector<Settled> settled;
        cadence.Settle(settled);
        for (const Settled& st : settled) if (!Emit(st, err)) return false;
        Prune();
        return Collect(false, err);
    }
    bool Poll(uint32_t timeoutMs, std::vector<Out<Frame>>& out, std::string& err) override
    {
        if (!Collect(false, err)) return false;
        if (m_ready.empty() && !m_pending.empty() && timeoutMs > 0 && !Complete(m_pending.front(), err)) return false;
        for (auto& q : m_ready) out.push_back(q);
        m_ready.clear();
        return true;
    }
    bool Flush(std::string& err) override
    {
        for (auto& q : m_ready) Release(q.frame);
        m_ready.clear();
        return Drop(err);
    }
    void Release(Frame f) override { if (release) release(f); }
    uint64_t NowMs() override
    {
        return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    ~GpuBackend() override { std::string e; Drop(e); }

private:
    struct Kept { void* keep = nullptr; YuvView v; };
    struct PairInfo { int id = -1; int cut = 0; bool pending = false; };
    struct Pending { uint64_t ticket = 0; int out = -1; Out<Frame> o; };

    void Unkeep(void* k) { if (k && unkeep) unkeep(k); }
    // a seek or gap: every queued output is finished and dropped, every picture forgotten
    bool Drop(std::string& err)
    {
        bool ok = true;
        for (auto& p : m_pending) {
            if (p.out >= 0) { if (!engine.Wait(p.ticket, err)) ok = false; engine.ReleaseOut(p.out); m_outPlanes.erase(p.o.frame); }
            Release(p.o.frame);
        }
        m_pending.clear();
        if (m_holdChain.valid()) m_holdChain.wait();
        m_holdChain = {};
        m_holdOf.clear();
        m_hold.Reset();
        for (auto& [s, k] : m_kept) Unkeep(k.keep);
        m_kept.clear();
        m_thumbs.clear();
        for (auto& [key, p] : m_pairs) engine.ReleasePair(p.id);
        m_pairs.clear();
        m_lastPair = -1; m_lastPairKey = { -1, -1 };
        for (auto& [s, slot] : m_slotOf) engine.ReleaseSlot(slot);
        m_slotOf.clear();
        m_lastDistinctSlot = -1;
        m_prevDistinct = -1;
        cadence.Reset();
        guard.Reset();
        m_last = -1000000;
        return ok;
    }
    int64_t PtsOf(int64_t n) const { const int64_t s = n / grid.k, j = n % grid.k; return grid.Pts(s) + int64_t(std::llround(double(j) * grid.srcDur100ns / grid.k)); }
    const Kept* KeptAt(int64_t s) const
    {
        auto it = m_kept.find(s);
        if (it != m_kept.end()) return &it->second;
        auto lo = m_kept.upper_bound(s);   // the nearest kept frame (it only happens for a frame dropped as too old)
        if (lo != m_kept.begin()) return &std::prev(lo)->second;
        return m_kept.empty() ? nullptr : &m_kept.begin()->second;
    }
    // the GPU picture of source frame s: its own, or the distinct frame it repeats
    int SlotAt(int64_t s) const
    {
        auto it = m_slotOf.upper_bound(s);
        if (it == m_slotOf.begin()) return m_slotOf.empty() ? -1 : m_slotOf.begin()->second;
        return std::prev(it)->second;
    }
    bool CopyReal(int64_t src, Frame& f, const Kept& k, std::string& err)
    {
        YuvOut o;
        if (!alloc(o, f, k.keep)) { err = "GPU engine: could not allocate an output frame"; return false; }
        const int bps = k.v.bits > 8 ? 2 : 1;
        for (int p = 0; p < 3; ++p) {
            const int w = p ? (k.v.width + 1) / 2 : k.v.width, h = p ? (k.v.height + 1) / 2 : k.v.height;
            for (int y = 0; y < h; ++y) std::memcpy(o.data[p] + ptrdiff_t(y) * o.stride[p], k.v.data[p] + ptrdiff_t(y) * k.v.stride[p], size_t(w) * size_t(bps));
        }
        (void)src;
        return true;
    }
    bool PairFor(int64_t d0, int64_t d1, PairInfo& out, std::string& err)
    {
        const auto key = std::make_pair(d0, d1);
        auto it = m_pairs.find(key);
        if (it == m_pairs.end()) { if (!StartPair(d0, d1, err)) return false; it = m_pairs.find(key); }
        if (!it->second.pending) { out = it->second; return true; }
        PairStats st;
        const int id = it->second.id;
        if (!engine.EstimateEnd(id, st, err)) return false;
        PairInfo pi; pi.id = id;
        pi.cut = transitionRules ? DecidePair(guard, st, transitions, ThumbAt(d0 - 1), ThumbAt(d0), ThumbAt(d1), ThumbAt(d1 + 1)) : guard.Decide(st);
        if (pi.cut == 4) stats.linearBlends++;
        if (pi.cut == 3) stats.flashes++;
        stats.pairs++; stats.meMs += st.ms;
        if (pi.cut == 1 || pi.cut == 2) { stats.cuts++; if (pi.cut == 2) stats.guardCuts++; }
        if (subtitleHold && !pi.cut) {
            auto ha = m_holdOf.find(d0), hb = m_holdOf.find(d1);
            if (ha != m_holdOf.end() && hb != m_holdOf.end()) {
                auto fa = ha->second.get(), fb = hb->second.get();
                if (fa && fb) {
                    auto hp = SubtitleHoldYuv::Pair(*fa, *fb, engine.Cfg().bits);
                    if (!engine.SetHold(id, hp.get(), err)) return false;
                    if (hp && hp->held > 0) { stats.heldPairs++; stats.heldPixels += uint64_t(hp->held); }
                }
            }
        }
        it->second = pi;
        out = pi;
        return true;
    }
    // the motion of a pair, submitted now and read when its outputs are due (the GPU works on it meanwhile)
    bool StartPair(int64_t d0, int64_t d1, std::string& err)
    {
        const auto key = std::make_pair(d0, d1);
        if (m_pairs.count(key)) return true;
        const int sa = m_slotOf.count(d0) ? m_slotOf[d0] : -1, sb = m_slotOf.count(d1) ? m_slotOf[d1] : -1;
        if (sa < 0 || sb < 0) { err = "GPU engine: a picture of the pair is gone"; return false; }
        const int prev = m_lastPairKey.second == d0 ? m_lastPair : -1;
        const int id = engine.EstimateBegin(sa, sb, prev, err);
        if (id < 0) return false;
        PairInfo pi; pi.id = id; pi.pending = true;
        m_pairs[key] = pi;
        m_lastPair = id; m_lastPairKey = key;
        return true;
    }
    bool NeedOut(std::string& err)
    {
        while (engine.FreeOuts() == 0 && !m_pending.empty()) if (!Complete(m_pending.front(), err)) return false;
        return true;
    }
    bool Emit(const Settled& st, std::string& err)
    {
        const bool up = engine.Cfg().upscaler != Upscaler::None;
        const int64_t pts = PtsOf(st.n);
        const Shot& sh = st.shot;
        Frame f{};
        if (sh.kind != Shot::Generated) {
            const Kept* k = KeptAt(sh.src);
            if (!k) { err = "GPU engine: source frame " + std::to_string(sh.src) + " is gone"; return false; }
            const int kind = sh.kind == Shot::Real ? 0 : 2;
            if (sh.kind == Shot::Hold) stats.holds++;
            if (!up) {
                if (!CopyReal(sh.src, f, *k, err)) return false;
                if (tag) tag(pts, kind, f);
                // queued behind the GPU outputs before it: the Pipeline must see the outputs in order (a real frame
                // ahead of the generated ones before it would end its wait for them)
                Pending p; p.o = { pts, false, f };
                m_pending.push_back(p);
                return true;
            }
            const int slot = SlotAt(sh.src);
            if (slot < 0 || !NeedOut(err)) { if (err.empty()) err = "GPU engine: no picture for frame " + std::to_string(sh.src); return false; }
            YuvOut o;
            if (!alloc(o, f, k->keep)) { err = "GPU engine: could not allocate an output frame"; return false; }
            Pending p; p.o = { pts, false, f };
            if (!engine.Real(slot, p.out, p.ticket, err)) { Release(f); return false; }
            m_outPlanes[f] = o;
            if (tag) tag(pts, kind, f);
            m_pending.push_back(p);
            return true;
        }
        PairInfo pi;
        if (!PairFor(sh.d0, sh.d1, pi, err)) return false;
        const Kept* k = KeptAt(sh.t < 0.5f ? sh.d0 : sh.d1);
        if (!k || !NeedOut(err)) { if (err.empty()) err = "GPU engine: the pair's frames are gone"; return false; }
        YuvOut o;
        if (!alloc(o, f, k->keep)) { err = "GPU engine: could not allocate an output frame"; return false; }
        Pending p; p.o = { pts, (st.n % grid.k) != 0, f };   // a source position's own output counts as its real frame for the Pipeline
        // cut: 1 PushFPS's rule, 2 the guard, 3 a flash: the nearer real frame; 4 a dissolve or fade: the plain blend
        const int mode = pi.cut == 4 ? 3 : pi.cut ? (sh.t < 0.5f ? 1 : 2) : 0;
        if (!engine.Generate(pi.id, sh.t, mode, p.out, p.ticket, err)) { Release(f); return false; }
        m_outPlanes[f] = o;
        stats.generated++;
        if (tag) tag(pts, pi.cut && pi.cut != 4 ? 2 : 1, f);
        m_pending.push_back(p);
        return true;
    }
    bool Complete(Pending& p, std::string& err)
    {
        if (p.out >= 0) {   // a GPU output (a copied real frame has out -1 and is complete)
            if (!engine.Wait(p.ticket, err)) return false;
            auto it = m_outPlanes.find(p.o.frame);
            if (it != m_outPlanes.end()) { engine.Read(p.out, it->second); m_outPlanes.erase(it); }
            else engine.ReleaseOut(p.out);
        }
        m_ready.push_back(p.o);
        m_pending.pop_front();
        return true;
    }
    // the finished outputs, in order (all = wait for every one)
    bool Collect(bool all, std::string& err)
    {
        while (!m_pending.empty() && (all || m_pending.front().out < 0 || engine.Done(m_pending.front().ticket)))
            if (!Complete(m_pending.front(), err)) return false;
        return true;
    }
    // forget what no later output can need: pairs before the first referenced distinct frame (but the last pair: the
    // next one's temporal predictor), their pictures, and kept source frames older than lookahead + 2
    void Prune()
    {
        const auto& dist = cadence.Distinct();
        const int64_t front = dist.empty() ? m_last : dist.front();
        for (auto it = m_pairs.begin(); it != m_pairs.end();) {
            if (it->first.first < front && it->second.id != m_lastPair) { engine.ReleasePair(it->second.id); it = m_pairs.erase(it); }
            else ++it;
        }
        for (auto it = m_slotOf.begin(); it != m_slotOf.end();) {
            bool usedByPair = false;
            for (auto& [key, p] : m_pairs) usedByPair = usedByPair || key.first == it->first || key.second == it->first;
            if (it->first < front && !usedByPair && it->second != m_lastDistinctSlot) { engine.ReleaseSlot(it->second); it = m_slotOf.erase(it); }
            else ++it;
        }
        const int64_t keepFrom = m_last - (cadence.Lookahead() + 2);
        for (auto it = m_kept.begin(); it != m_kept.end();) {
            if (it->first < keepFrom) {
                // a frame still feeding Subtitle Hold's worker stays until that is done
                auto h = m_holdOf.find(it->first);
                if (h != m_holdOf.end()) { h->second.wait(); m_holdOf.erase(h); }
                m_thumbs.erase(it->first);
                Unkeep(it->second.keep); it = m_kept.erase(it);
            } else ++it;
        }
        stats.waitMs = engine.Times().waitMs;
    }

    int64_t m_last = -1000000;
    int m_lastDistinctSlot = -1;
    int64_t m_prevDistinct = -1;
    std::map<int64_t, Kept> m_kept;
    std::map<int64_t, Thumb> m_thumbs;
    const Thumb* ThumbAt(int64_t s) const { auto it = m_thumbs.find(s); return it == m_thumbs.end() ? nullptr : &it->second; }
    std::map<int64_t, int> m_slotOf;
    std::map<std::pair<int64_t, int64_t>, PairInfo> m_pairs;
    int m_lastPair = -1;
    std::pair<int64_t, int64_t> m_lastPairKey{ -1, -1 };
    SubtitleHoldYuv m_hold;
    std::shared_future<std::shared_ptr<const HoldFrameYuv>> m_holdChain;
    std::map<int64_t, std::shared_future<std::shared_ptr<const HoldFrameYuv>>> m_holdOf;
    std::deque<Pending> m_pending;
    std::vector<Out<Frame>> m_ready;
    std::map<Frame, YuvOut> m_outPlanes;
};

} // namespace pmvs::gpu
