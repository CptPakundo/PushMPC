// PushMPC: the held-drawing repair's per-stream scheduling for the portable routes (VapourSynth GPU and ml routes, the
// mpv Vulkan engines). Pure C++ on top of HeldFrames.h: each frame's cell grid in order, then the admitted parts of the
// frame kLookahead behind (the live-action gate and the per-span decisions of pushcore's repairFrame, with a budget on
// the measured repair cost). Also the CPU pieces of the ml route: the cell grid of 16-bit planes and the composite.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <tuple>
#include <vector>
#include "HeldFrames.h"

namespace pushmpc::held {

struct RepairStats { uint64_t frames = 0, parts = 0, gated = 0, overBudget = 0; double ms = 0; };

class RepairScheduler {
public:
    void Reset() { m_grids.clear(); m_regions.clear(); m_dup.clear(); m_hist.Reset(); m_decision.clear(); }
    // the next frame's grid (frames in order; a gap: Reset first). dup: the whole frame repeats the last distinct one
    // (Dedup): its cell changes are the clip's noise, and it counts as unchanged (no pair starts or ends there)
    void Push(int64_t s, CellGrid g, bool dup)
    {
        auto prev = m_grids.find(s - 1);
        if (prev != m_grids.end()) {
            if (dup) { m_noise.Push(CellChangeP99(prev->second, g)); g = prev->second; }
            m_regions[s] = CompareRegions(prev->second, g, m_noise.Threshold());
            m_hist.Push(uint64_t(s), m_regions[s]);
        }
        m_dup[s] = dup;
        m_grids[s] = std::move(g);
        while (m_grids.size() > 12) m_grids.erase(m_grids.begin());
        while (m_regions.size() > 12) m_regions.erase(m_regions.begin());
        while (m_dup.size() > 12) m_dup.erase(m_dup.begin());
    }
    const CellGrid* Grid(int64_t s) const { auto it = m_grids.find(s); return it == m_grids.end() ? nullptr : &it->second; }
    // the parts of frame s to repair now (s + kLookahead pushed): gate and budget decided once per (part, span) and kept
    // for the whole span; a new span starts only while the mean repair cost stays under budgetMs
    std::vector<RepairGroup> Plan(int64_t s, double budgetMs)
    {
        auto frameAt = [&](uint64_t f) -> const RegionFrame* { auto it = m_regions.find(int64_t(f)); return it == m_regions.end() ? nullptr : &it->second; };
        auto gridAt = [&](uint64_t f) -> const CellGrid* { auto it = m_grids.find(int64_t(f)); return it == m_grids.end() ? nullptr : &it->second; };
        std::vector<RepairGroup> keep;
        if (s < 1) return keep;
        auto groups = PlanRepairs(uint64_t(s), frameAt, gridAt, kLookahead, PhaseShape::Linear, m_noise.Threshold(), &m_hist);
        bool exact = false;
        for (const auto& g : groups) exact |= IsExactRepeat(g);
        const auto d = m_dup.find(s);
        m_evidence.Push((d != m_dup.end() && d->second) || exact);
        const bool gate = m_evidence.Animation();
        for (auto it = m_decision.begin(); it != m_decision.end();) { if (std::get<1>(it->first) <= uint64_t(s)) it = m_decision.erase(it); else ++it; }
        for (auto& g : groups) {
            const auto key = std::make_tuple(g.d0, g.d1, g.pairFrom);
            auto it = m_decision.find(key);
            if (it == m_decision.end()) {
                const bool over = m_costMs > budgetMs;
                if (!gate) ++stats.gated; else if (over) ++stats.overBudget;
                it = m_decision.emplace(key, gate && !over).first;
            }
            if (it->second) keep.push_back(std::move(g));
        }
        return keep;
    }
    // the wall time of one repaired frame (the budget follows its running mean)
    void Cost(double ms, size_t parts)
    {
        m_costMs = m_costMs <= 0 ? ms : m_costMs + (ms - m_costMs) * 0.1;
        ++stats.frames; stats.parts += parts; stats.ms += ms;
    }
    RepairStats stats;

private:
    std::map<int64_t, CellGrid> m_grids;
    std::map<int64_t, RegionFrame> m_regions;
    std::map<int64_t, bool> m_dup;
    NoiseFloor m_noise;
    CellCadence m_hist;
    CadenceEvidence m_evidence{240};
    std::map<std::tuple<uint64_t, uint64_t, uint64_t>, bool> m_decision;
    double m_costMs = 0;
};

// The cell grid of 4:2:0 16-bit planes (codes 0..2^bits-1), as the GPU pass makes it (PushCoreHeld.h CS_Held): f x f
// luma cells (f = width / 480, at least 2), means in 8-bit codes; with prevY the largest 2 x 2 sub-cell change; Cb / Cr.
inline CellGrid CellsFromPlanes(const uint16_t* y, ptrdiff_t ys, const uint16_t* u, const uint16_t* v, ptrdiff_t cs, const uint16_t* prevY,
                                int width, int height, int bits)
{
    CellGrid g;
    const int f = std::max(2, width / 480);
    g.cell = uint32_t(f); g.w = uint32_t(width / f); g.h = uint32_t(height / f);
    const size_t n = size_t(g.w) * g.h;
    g.v.assign(n, 0.f); g.sub.assign(n, 0.f); g.cb.assign(n, 0.f); g.cr.assign(n, 0.f);
    const float k = 255.f / float((1 << bits) - 1);
    const int half = f / 2, cf = std::max(1, f / 2);
    for (uint32_t cyi = 0; cyi < g.h; ++cyi)
        for (uint32_t cxi = 0; cxi < g.w; ++cxi) {
            float a = 0, sa[4] = {}, sb[4] = {}, sn[4] = {};
            for (int j = 0; j < f; ++j) {
                const uint16_t* row = y + (ptrdiff_t(cyi) * f + j) * ys + ptrdiff_t(cxi) * f;
                const uint16_t* prow = prevY ? prevY + (ptrdiff_t(cyi) * f + j) * ys + ptrdiff_t(cxi) * f : nullptr;
                for (int i = 0; i < f; ++i) {
                    const float c = row[i] * k;
                    a += c;
                    const int q = (i >= half ? 1 : 0) + (j >= half ? 2 : 0);
                    sa[q] += c; sb[q] += prow ? prow[i] * k : c; sn[q] += 1.f;
                }
            }
            const size_t idx = size_t(cyi) * g.w + cxi;
            g.v[idx] = a / float(f * f);
            float sm = 0;
            for (int q = 0; q < 4; ++q) if (sn[q] > 0) sm = std::max(sm, std::fabs(sa[q] - sb[q]) / sn[q]);
            g.sub[idx] = std::floor(std::min(sm * 4.f + 0.5f, 1023.f)) * 0.25f;
            float cu = 0, cv = 0;
            for (int j = 0; j < cf; ++j) for (int i = 0; i < cf; ++i) {
                const ptrdiff_t o = (ptrdiff_t(cyi) * f / 2 + j) * cs + ptrdiff_t(cxi) * f / 2 + i;
                cu += u[o] * k; cv += v[o] * k;
            }
            g.cb[idx] = cu / float(cf * cf); g.cr[idx] = cv / float(cf * cf);
        }
    return g;
}

// Lays a part picture into a 4:2:0 16-bit frame where alpha (one value per cell, 0..1) says: bilinear between cell
// centres, then smoothstep(0.15, 0.85) (the feather, as PushCoreRegion.h).
inline void CompositePlanes(uint16_t* const dst[3], const ptrdiff_t dstStride[3], const uint16_t* const src[3], const ptrdiff_t srcStride[3],
                            int width, int height, const std::vector<float>& alpha, int cellsW, int cellsH, int cell)
{
    auto alphaAt = [&](float px, float py) {   // px, py in luma pixels (centres)
        const float fx = std::clamp(px / float(cell) - 0.5f, 0.f, float(cellsW - 1)), fy = std::clamp(py / float(cell) - 0.5f, 0.f, float(cellsH - 1));
        const int x0 = int(fx), y0 = int(fy), x1 = std::min(x0 + 1, cellsW - 1), y1 = std::min(y0 + 1, cellsH - 1);
        const float ax = fx - float(x0), ay = fy - float(y0);
        const float m = (alpha[size_t(y0) * cellsW + x0] * (1 - ax) + alpha[size_t(y0) * cellsW + x1] * ax) * (1 - ay)
                      + (alpha[size_t(y1) * cellsW + x0] * (1 - ax) + alpha[size_t(y1) * cellsW + x1] * ax) * ay;
        const float t = std::clamp((m - 0.15f) / 0.7f, 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    };
    for (int p = 0; p < 3; ++p) {
        const int w = p ? width / 2 : width, h = p ? height / 2 : height, sc = p ? 2 : 1;
        for (int yy = 0; yy < h; ++yy)
            for (int xx = 0; xx < w; ++xx) {
                const float a = alphaAt((float(xx) + 0.5f) * float(sc), (float(yy) + 0.5f) * float(sc));
                if (a <= 0.001f) continue;
                uint16_t& d = dst[p][yy * dstStride[p] + xx];
                const float s = src[p][yy * srcStride[p] + xx];
                d = uint16_t(std::lround(float(d) + (s - float(d)) * a));
            }
    }
}

} // namespace pushmpc::held
