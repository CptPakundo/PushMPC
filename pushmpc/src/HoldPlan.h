// PushMPC: hold plans for the generated frames (anime round 5). Pure C++, no GPU: filter/tools/held_test.cpp checks it.
//
// The held-frame repair (HeldFrames.h) changes the SOURCE before frame generation. This file decides where the
// GENERATED frames should instead show a real frame "as drawn" (the hold compose, PushCoreHold.h):
//  - motion classes per changed object of a pair (s-1, s), from a block match on the planner's cell grids:
//      Slid       one local translation explains >= 85 % of its changed cells, on this pair AND the one before (a pan
//                 inside a window, a scrolling credit roll, a cel slid on ones): the generator's output stays (smooth);
//      Redrawn    a new drawing (a character on twos, a mouth): in "Camera only" it is held, camera-warped, and pops to
//                 the next drawing at its own source time;
//      Unexplained no translation within +-40 cells (160 px at 1080p) and no two-motion split explains it (an object
//                 that appears, a smear, a fast non-rigid swing, steam): every interpolator ghosts there, so the nearest
//                 real frame is shown (held in Full mode too);
//  - transients over the whole picture: a single- or two-frame flash (the picture mean jumps away and back) and an
//    inserted impact frame (most cells change at s and at s+1 while s-1 ~ s+1): the nearest real frame, whole frame,
//    for every video (live action included: no blend of a flash).
// Decisions are per object and per pair, all or nothing (an object is never torn into smooth and popping parts).
#pragma once
#include "HeldFrames.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace pushmpc::held {

// AnimationMode (PushCoreSettings.animationMode)
enum class AnimMode : uint8_t { Full = 0, CameraOnly = 1, Off = 2 };
// PushCoreSettings.holdFlags
inline constexpr uint32_t kHoldTransients = 1u;   // flashes, impact frames, unexplained objects
inline constexpr uint32_t kHoldLineArt = 2u;      // line-art protection in the compose (contrast matching, doubled lines)
inline constexpr uint32_t kHoldSigns = 4u;        // typeset signs held (the PushFPS Subtitle Hold sign zone)
inline constexpr uint32_t kHoldLiveTransients = 8u;   // flash / impact hold also when the picture is not animation
inline constexpr uint32_t kHoldFilmTime = 16u;        // P9: drawn parts of 24p-grid shots in 29.97 video switch at film time

// ---- block match (CPU; 8 x 8 cells per block) ----
struct BlockMotion {
    static constexpr int kB = 8;
    uint32_t bw = 0, bh = 0, w = 0, h = 0;
    std::vector<int8_t> sx, sy;           // best shift in cells on top of the camera
    std::vector<float> res, zero;         // residual at the best shift / at the camera shift (codes)
    std::vector<uint16_t> nch;            // changed cells in the block (0: not searched)
    std::vector<uint8_t> camOk;           // the camera's residual is small against the block's contrast (interpolation only)
    std::vector<uint8_t> state;           // 0 not searched, 1 explained by one shift, 2 explained by two motions, 3 unexplained,
                                          // 4 explained by the camera within a cell (its estimate a fraction off there)
    uint64_t searched = 0, coarse = 0;    // counters (blocks searched, blocks that needed the wide search)
    bool rigid = false;                   // a moving camera explains nearly all the textured picture (a pan, a credit roll)
    float camFixX = 0, camFixY = 0;       // quarter-cell correction of the camera estimate (cells), applied to every shift here
};

// prev resampled at the camera-compensated position of every cell of cur (bilinear once, so integer shifts on top are
// plain lookups)
inline void WarpPrev(const CellGrid& prev, const RegionFrame& r, std::vector<float>& out)
{
    const uint32_t W = prev.w, H = prev.h;
    out.resize(size_t(W) * H);
    const float cx = 0.5f * float(W - 1), cy = 0.5f * float(H - 1), sc = r.scale > 0.f ? r.scale : 1.f;
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x)
            out[size_t(y) * W + x] = prev.sample(cx + (float(x) - cx - r.dx) / sc, cy + (float(y) - cy - r.dy) / sc);
}

inline constexpr float kRigidShare = 0.15f;    // camera residual / edge contrast of the whole picture for a rigid move
inline constexpr float kInterpShare = 0.25f;   // camera residual / block contrast below which it is interpolation, not change
inline BlockMotion MatchBlocks(const CellGrid& prev, const CellGrid& cur, const RegionFrame& r, float thr, int fine = 8, int wide = 40)
{
    BlockMotion m;
    if (!r.valid || r.cut || !prev.ok() || !cur.ok() || prev.w != cur.w || prev.h != cur.h || r.changed.size() != cur.v.size()) return m;
    constexpr int B = BlockMotion::kB;
    const int W = int(cur.w), H = int(cur.h), BW = W / B, BH = H / B;
    m.w = cur.w; m.h = cur.h; m.bw = uint32_t(BW); m.bh = uint32_t(BH);
    const size_t nb = size_t(BW) * BH;
    m.sx.assign(nb, 0); m.sy.assign(nb, 0); m.res.assign(nb, 0.f); m.zero.assign(nb, 0.f); m.nch.assign(nb, 0); m.state.assign(nb, 0); m.camOk.assign(nb, 0);
    // the camera refined to a quarter cell over the whole textured picture (not only the changed cells: those would pull
    // it towards a moving character). CompareRegions' estimate can be a fraction off on a slow roll (credits on black:
    // 3 px a frame at 2160p is 0.375 cell), which leaves every glyph edge "changed"; the refined camera explains them.
    RegionFrame rc = r;
    {
        std::vector<uint32_t> probe;
        for (int y = 2; y + 2 < H; y += 2)
            for (int x = 2; x + 2 < W; x += 2) {
                const size_t c = size_t(y) * W + x;
                if (std::fabs(cur.v[c + 1] - cur.v[c - 1]) + std::fabs(cur.v[c + W] - cur.v[c - W]) >= 8.f) probe.push_back(uint32_t(c));
            }
        if (probe.size() >= 64) {
            const float cx = 0.5f * float(W - 1), cy = 0.5f * float(H - 1), sc = r.scale > 0.f ? r.scale : 1.f;
            auto err = [&](float dx, float dy) {
                double e = 0;
                for (uint32_t c : probe) { const float x = float(c % uint32_t(W)), y = float(c / uint32_t(W)); e += std::min(32.f, std::fabs(cur.v[c] - prev.sample(cx + (x - cx - dx) / sc, cy + (y - cy - dy) / sc))); }
                return e;
            };
            double best = err(r.dx, r.dy); float fx = 0.f, fy = 0.f;
            for (int j = -2; j <= 2; ++j)
                for (int i = -2; i <= 2; ++i) { if (!i && !j) continue; const double e = err(r.dx + 0.25f * float(i), r.dy + 0.25f * float(j)); if (e < 0.97 * best) { best = e; fx = 0.25f * float(i); fy = 0.25f * float(j); } }
            rc.dx += fx; rc.dy += fy; m.camFixX = fx; m.camFixY = fy;
            // how well the camera alone explains the textured picture: residual against the edges' own contrast
            double grad = 0;
            for (uint32_t c : probe) grad += std::fabs(cur.v[c + 1] - cur.v[c - 1]) + std::fabs(cur.v[c + W] - cur.v[c - W]);
            m.rigid = grad > 0 && (std::fabs(rc.dx) + std::fabs(rc.dy) >= 0.25f) && best / grad <= kRigidShare;
        }
    }
    std::vector<float> pw;
    WarpPrev(prev, rc, pw);
    auto P = [&](int x, int y) { x = std::clamp(x, 0, W - 1); y = std::clamp(y, 0, H - 1); return pw[size_t(y) * W + x]; };
    for (int by = 0; by < BH; ++by)
        for (int bx = 0; bx < BW; ++bx) {
            int nch = 0;
            for (int j = 0; j < B; ++j) for (int i = 0; i < B; ++i) nch += r.changed[size_t(by * B + j) * W + bx * B + i];
            if (nch < B * B / 4) continue;
            const size_t bi = size_t(by) * BW + bx;
            m.nch[bi] = uint16_t(nch);
            ++m.searched;
            // SAD with early termination (limit: the best so far)
            auto sad = [&](int dx, int dy, float limit, int step) {
                double e = 0; const double lim = double(limit) * (B * B) / (step * step);
                for (int j = 0; j < B; j += step) {
                    const int y = by * B + j;
                    for (int i = 0; i < B; i += step) { const int x = bx * B + i; e += std::fabs(cur.v[size_t(y) * W + x] - P(x - dx, y - dy)); }
                    if (e > lim) return 1e30f;
                }
                return float(e * (step * step) / (B * B));
            };
            const float zero = sad(0, 0, 1e30f, 1);
            float best = zero; int bdx = 0, bdy = 0;
            for (int dy = -fine; dy <= fine && best > 0.f; ++dy)
                for (int dx = -fine; dx <= fine; ++dx) { if (!dx && !dy) continue; const float e = sad(dx, dy, best, 1); if (e < best) { best = e; bdx = dx; bdy = dy; } }
            const float accept = std::max(thr, 0.4f * zero);
            if (best > accept && wide > fine) {
                // fast rigid motion: coarse +-wide step 2 on every second cell, then +-1 around the best on every cell
                float cb = 1e30f; int cx = bdx, cy = bdy;
                for (int dy = -wide; dy <= wide; dy += 2)
                    for (int dx = -wide; dx <= wide; dx += 2) { if (std::abs(dx) <= fine && std::abs(dy) <= fine) continue; const float e = sad(dx, dy, cb, 2); if (e < cb) { cb = e; cx = dx; cy = dy; } }
                for (int dy = cy - 1; dy <= cy + 1; ++dy)
                    for (int dx = cx - 1; dx <= cx + 1; ++dx) { const float e = sad(dx, dy, best, 1); if (e < best) { best = e; bdx = dx; bdy = dy; } }
                ++m.coarse;
            }
            if (best > accept && best > 0.f) {
                // half a cell around the best: a camera estimate a fraction off (text lines, weak texture in one direction)
                const float pcx = 0.5f * float(W - 1), pcy = 0.5f * float(H - 1), psc = r.scale > 0.f ? r.scale : 1.f;
                auto sadf = [&](float dx, float dy) {
                    double e = 0;
                    for (int j = 0; j < B; ++j)
                        for (int i = 0; i < B; ++i) {
                            // one interpolation of prev at the camera + shift position (not of the camera-warped copy: twice
                            // interpolated, a sharp edge would not match at any fraction)
                            const float x = float(bx * B + i), y = float(by * B + j);
                            const float p = prev.sample(pcx + (x - pcx - rc.dx - dx) / psc, pcy + (y - pcy - rc.dy - dy) / psc);
                            e += std::fabs(cur.v[size_t(by * B + j) * W + bx * B + i] - p);
                        }
                    return float(e / (B * B));
                };
                // quarter cells within 0.75 of the best integer shift
                float fb = best; int fi = 0, fj = 0;
                for (int j = -3; j <= 3; ++j)
                    for (int i = -3; i <= 3; ++i) { if (!i && !j) continue; const float e = sadf(float(bdx) + 0.25f * float(i), float(bdy) + 0.25f * float(j)); if (e < fb) { fb = e; fi = i; fj = j; } }
                // explained within a cell of the camera: the camera estimate was a fraction off here, nothing moved on its own
                if (fb <= accept && !bdx && !bdy) { m.res[bi] = fb; m.zero[bi] = zero; m.state[bi] = 4; continue; }
                if (fb < best) { best = fb; (void)fi; (void)fj; }
            }
            m.sx[bi] = int8_t(std::clamp(bdx, -127, 127)); m.sy[bi] = int8_t(std::clamp(bdy, -127, 127));
            m.res[bi] = best; m.zero[bi] = zero;
            m.state[bi] = best > accept ? 3 : (bdx || bdy) ? 1 : 4;   // 4: the camera explains it after all (a cell's change was a fraction of the camera)
            {
                // sharp detail moved by the camera (glyphs of a credit roll, hair lines): the cell grid's interpolation
                // leaves a residual over the threshold that no shift lowers, but it is small against the block's own
                // contrast. A redrawn or appearing object differs by about its contrast.
                double mean = 0, dev = 0;
                for (int j = 0; j < B; ++j) for (int i = 0; i < B; ++i) mean += cur.v[size_t(by * B + j) * W + bx * B + i];
                mean /= B * B;
                for (int j = 0; j < B; ++j) for (int i = 0; i < B; ++i) dev += std::fabs(cur.v[size_t(by * B + j) * W + bx * B + i] - mean);
                dev /= B * B;
                m.camOk[bi] = zero <= kInterpShare * float(dev);
                if (m.state[bi] == 3 && m.camOk[bi] && best >= 0.8f * zero) m.state[bi] = 4;
            }
        }
    // pass 2: a block with two motions (an occlusion edge, a window over a scrolling background) is explained when every
    // cell matches the camera or one of its neighbour blocks' shifts
    for (int by = 0; by < BH; ++by)
        for (int bx = 0; bx < BW; ++bx) {
            const size_t bi = size_t(by) * BW + bx;
            if (m.state[bi] != 3) continue;
            int cand[10][2]; int nc = 0; cand[nc][0] = 0; cand[nc][1] = 0; ++nc;
            for (int j = -1; j <= 1; ++j)
                for (int i = -1; i <= 1; ++i) {
                    const int xx = bx + i, yy = by + j;
                    if (xx < 0 || yy < 0 || xx >= BW || yy >= BH || (!i && !j)) continue;
                    const size_t ni = size_t(yy) * BW + xx;
                    if (m.state[ni] == 1) { cand[nc][0] = m.sx[ni]; cand[nc][1] = m.sy[ni]; ++nc; }
                }
            double e = 0;
            for (int j = 0; j < B; ++j)
                for (int i = 0; i < B; ++i) {
                    const int x = bx * B + i, y = by * B + j; float mn = 1e30f;
                    for (int c = 0; c < nc; ++c) mn = std::min(mn, std::fabs(cur.v[size_t(y) * W + x] - P(x - cand[c][0], y - cand[c][1])));
                    e += mn;
                }
            if (float(e / (B * B)) <= std::max(thr, 0.4f * m.zero[bi])) m.state[bi] = 2;
        }
    return m;
}

// ---- motion classes per changed object ----
struct ObjectClass {
    enum Kind : uint8_t { Slid = 0, Redrawn = 1, Unexplained = 2 } kind = Redrawn;
    uint32_t cells = 0;                   // changed cells
    float vx = 0, vy = 0;                 // dominant shift on top of the camera (cells)
    float slidShare = 0, unexplShare = 0; // of the changed cells in searched blocks
    bool edge = false;                    // touches the picture edge
    bool consistent = false;              // Slid: the same translation on the pair before
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // bounding box (cells, inclusive)
    std::vector<uint32_t> idx;            // its cells
};
inline constexpr float kUnexplainedShare = 0.5f;
inline constexpr uint32_t kMinObjectCells = 16;     // smaller changes are noise or a blink: left to the generator
inline constexpr uint32_t kMinUnexplainedCells = 64;   // ~1,000 px2 at 1080p (a held patch smaller than this is a seam)

/// Objects of a pair, in two steps (research §1: per object, all or nothing, and a body is never torn):
//  1. slide segments: 8-connected changed cells whose blocks moved by the same shift (+-1 cell). A segment that slid by
//     about the same shift on the pair before too (prevSlid: per cell the shift + 128 of the segments of the pair
//     before, 0 = none) moves on ones: a pan inside a window, a credit roll, a cel slid on ones -> Slid (smooth);
//  2. everything else that changed (new drawings, the cells of slide segments that did not slide on the pair before,
//     unexplained blocks): 8-connected components -> Unexplained (most searched cells unexplained, >= 64 cells) or
//     Redrawn. A torso slid on twos next to its redrawn limbs is not a ones-slide, so it stays in the limbs' object.
// slidOut: this pair's slide segments (consistent or not) for the next pair's test.
inline std::vector<ObjectClass> ClassifyObjects(const RegionFrame& r, const BlockMotion& m, const std::vector<int16_t>& prevSlidX,
                                                const std::vector<int16_t>& prevSlidY, std::vector<int16_t>* slidOutX = nullptr,
                                                std::vector<int16_t>* slidOutY = nullptr)
{
    std::vector<ObjectClass> out;
    const size_t n = r.changed.size();
    if (slidOutX) slidOutX->assign(n, 0);
    if (slidOutY) slidOutY->assign(n, 0);
    if (!r.valid || r.cut || m.bw == 0) return out;
    const int W = int(m.w), H = int(m.h), B = BlockMotion::kB, BW = int(m.bw), BH = int(m.bh);
    const bool havePrev = prevSlidX.size() == n && prevSlidY.size() == n;
    auto blockOf = [&](size_t c) -> int { const int x = int(c % size_t(W)) / B, y = int(c / size_t(W)) / B; return x < BW && y < BH ? y * BW + x : -1; };
    // the changed cells, without those the camera explains within a cell (MatchBlocks state 4)
    std::vector<uint8_t> chg(r.changed);
    for (size_t c = 0; c < n; ++c) if (chg[c]) { const int b = blockOf(c); if (b >= 0 && m.state[size_t(b)] == 4) chg[c] = 0; }
    // per changed cell: 1 + its block's slide shift key, or 0 (no slide)
    std::vector<int32_t> key(n, 0);
    for (size_t c = 0; c < n; ++c) {
        if (!chg[c]) continue;
        const int b = blockOf(c);
        if (b >= 0 && m.state[size_t(b)] == 1 && (m.sx[size_t(b)] || m.sy[size_t(b)])) key[c] = 1 + (m.sx[size_t(b)] + 128) * 256 + (m.sy[size_t(b)] + 128);
    }
    auto close = [](int32_t a, int32_t b) { --a; --b; return std::abs(a / 256 - b / 256) <= 1 && std::abs(a % 256 - b % 256) <= 1; };
    std::vector<uint8_t> seen(n, 0), slidCell(n, 0);
    std::vector<uint32_t> stack, idx;
    // 1. slide segments
    for (size_t s0 = 0; s0 < n; ++s0) {
        if (!key[s0] || seen[s0]) continue;
        idx.clear(); stack.clear(); stack.push_back(uint32_t(s0)); seen[s0] = 1;
        std::vector<std::pair<int32_t, uint32_t>> hist;
        while (!stack.empty()) {
            const uint32_t c = stack.back(); stack.pop_back(); idx.push_back(c);
            auto it = std::find_if(hist.begin(), hist.end(), [&](const auto& p) { return p.first == key[c]; });
            if (it == hist.end()) hist.push_back({key[c], 1u}); else ++it->second;
            const int x = int(c % uint32_t(W)), y = int(c / uint32_t(W));
            for (int j = -1; j <= 1; ++j)
                for (int i = -1; i <= 1; ++i) {
                    const int xx = x + i, yy = y + j;
                    if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
                    const size_t k = size_t(yy) * W + xx;
                    if (key[k] && !seen[k] && close(key[k], key[c])) { seen[k] = 1; stack.push_back(uint32_t(k)); }
                }
        }
        if (idx.size() < kMinObjectCells) continue;   // too small to be a slide: its cells go to step 2
        int32_t dk = 0; uint32_t dn = 0;
        for (const auto& [k, v] : hist) if (v > dn) { dn = v; dk = k - 1; }
        const float vx = float(dk / 256 - 128), vy = float(dk % 256 - 128);
        uint32_t agree = 0;
        if (havePrev)
            for (uint32_t c : idx) {
                // where this cell's content was on the pair before (it moved by the camera + its shift), or the same cell
                const int x = int(c % uint32_t(W)), y = int(c / uint32_t(W));
                const int px = std::clamp(x - int(std::lround(r.dx + vx)), 0, W - 1), py = std::clamp(y - int(std::lround(r.dy + vy)), 0, H - 1);
                bool ok = false;
                for (const size_t q : { size_t(py) * W + px, size_t(c) })
                    ok = ok || ((prevSlidX[q] || prevSlidY[q]) && std::fabs(float(prevSlidX[q] - 128) - vx) <= 1.5f && std::fabs(float(prevSlidY[q] - 128) - vy) <= 1.5f);
                agree += ok;
            }
        if (slidOutX && slidOutY) for (uint32_t c : idx) { (*slidOutX)[c] = int16_t(vx + 128); (*slidOutY)[c] = int16_t(vy + 128); }
        if (agree * 2 < idx.size()) continue;   // not a slide on ones: step 2
        ObjectClass o;
        o.kind = ObjectClass::Slid; o.consistent = true; o.vx = vx; o.vy = vy; o.slidShare = 1.f;
        o.x0 = W; o.y0 = H; o.x1 = -1; o.y1 = -1;
        for (uint32_t c : idx) { const int x = int(c % uint32_t(W)), y = int(c / uint32_t(W)); o.x0 = std::min(o.x0, x); o.x1 = std::max(o.x1, x); o.y0 = std::min(o.y0, y); o.y1 = std::max(o.y1, y); slidCell[c] = 1; }
        o.edge = o.x0 == 0 || o.y0 == 0 || o.x1 == W - 1 || o.y1 == H - 1;
        o.cells = uint32_t(idx.size()); o.idx = idx;
        out.push_back(std::move(o));
    }
    // where the slid objects move through (their footprint and the strip they uncover / cover, along their motion)
    std::vector<uint8_t> slidSweep(n, 0);
    for (const auto& o : out) {
        const int steps = int(std::ceil(std::max(std::fabs(o.vx), std::fabs(o.vy)))) + 1;
        for (uint32_t c : o.idx) {
            const int x = int(c % uint32_t(W)), y = int(c / uint32_t(W));
            for (int k = -steps; k <= steps; ++k) {
                const float f = float(k) / float(steps);
                const int xx = x + int(std::lround(f * (std::fabs(o.vx) + 1.f) * (o.vx < 0 ? -1.f : 1.f))), yy = y + int(std::lround(f * (std::fabs(o.vy) + 1.f) * (o.vy < 0 ? -1.f : 1.f)));
                for (int j = -2; j <= 2; ++j)
                    for (int i = -2; i <= 2; ++i) {
                        const int px = xx + i, py = yy + j;
                        if (px >= 0 && py >= 0 && px < W && py < H) { uint8_t& v = slidSweep[size_t(py) * W + px]; v = std::max<uint8_t>(v, (i || j) ? 1 : 2); }
                    }
            }
        }
    }
    // 2. the rest of the change
    std::fill(seen.begin(), seen.end(), 0);
    for (size_t s0 = 0; s0 < n; ++s0) {
        if (!chg[s0] || slidCell[s0] || seen[s0]) continue;
        ObjectClass o;
        o.x0 = W; o.y0 = H; o.x1 = -1; o.y1 = -1;
        stack.clear(); stack.push_back(uint32_t(s0)); seen[s0] = 1;
        while (!stack.empty()) {
            const uint32_t c = stack.back(); stack.pop_back();
            o.idx.push_back(c);
            const int x = int(c % uint32_t(W)), y = int(c / uint32_t(W));
            o.x0 = std::min(o.x0, x); o.x1 = std::max(o.x1, x); o.y0 = std::min(o.y0, y); o.y1 = std::max(o.y1, y);
            for (int j = -1; j <= 1; ++j)
                for (int i = -1; i <= 1; ++i) {
                    const int xx = x + i, yy = y + j;
                    if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
                    const size_t k = size_t(yy) * W + xx;
                    if (chg[k] && !slidCell[k] && !seen[k]) { seen[k] = 1; stack.push_back(uint32_t(k)); }
                }
        }
        o.cells = uint32_t(o.idx.size());
        if (o.cells < kMinObjectCells) continue;
        o.edge = o.x0 == 0 || o.y0 == 0 || o.x1 == W - 1 || o.y1 == H - 1;
        uint32_t searched = 0, unexpl = 0, slidN = 0, camOkN = 0;
        std::vector<std::pair<int32_t, uint32_t>> hist;
        for (uint32_t c : o.idx) {
            const int b = blockOf(c);
            if (b < 0 || !m.state[size_t(b)]) continue;
            ++searched;
            camOkN += m.camOk[size_t(b)];
            if (m.state[size_t(b)] == 3) { ++unexpl; continue; }
            if (key[c]) {
                ++slidN;
                auto it = std::find_if(hist.begin(), hist.end(), [&](const auto& p) { return p.first == key[c]; });
                if (it == hist.end()) hist.push_back({key[c], 1u}); else ++it->second;
            }
        }
        int32_t dk = -1; uint32_t dn = 0;
        for (const auto& [k, v] : hist) if (v > dn) { dn = v; dk = k - 1; }
        if (dk >= 0) { o.vx = float(dk / 256 - 128); o.vy = float(dk % 256 - 128); }   // its drawing's step (for the sweep)
        o.slidShare = searched ? float(slidN) / float(searched) : 0.f;
        o.unexplShare = searched ? float(unexpl) / float(searched) : 0.f;
        o.kind = searched && o.unexplShare >= kUnexplainedShare && o.cells >= kMinUnexplainedCells ? ObjectClass::Unexplained : ObjectClass::Redrawn;
        // a sparse change (most of its cells in blocks with under a quarter changed: thin lines a fraction off, grain):
        // nothing was redrawn there, the generator's output stays
        if (searched * 4 < o.cells) o.kind = ObjectClass::Slid;
        // its blocks match the camera up to interpolation (detail carried by the camera: a credit roll's glyphs)
        if (searched && camOkN * 5 >= searched * 3 && o.kind == ObjectClass::Redrawn) o.kind = ObjectClass::Slid;
        // on a rigid camera move small bits that no shift explains are the cell grid's interpolation of fine detail
        // (glyphs of a credit roll), not drawings: a held patch smaller than kMinUnexplainedCells would only be a seam
        if (m.rigid && o.cells < kMinUnexplainedCells && o.unexplShare >= kUnexplainedShare) o.kind = ObjectClass::Slid;
        // the strip a pan reveals at the picture edge (no earlier frame has it): the generator's output, never a hold
        const int rx = int(std::ceil(std::fabs(r.dx))) + 2, ry = int(std::ceil(std::fabs(r.dy))) + 2;
        if ((o.x0 == 0 && o.x1 < rx) || (o.x1 == W - 1 && o.x0 > W - 1 - rx) || (o.y0 == 0 && o.y1 < ry) || (o.y1 == H - 1 && o.y0 > H - 1 - ry))
            o.kind = ObjectClass::Slid;
        // the background a slid object uncovers (or covers) as it moves: part of its motion, the generator's output
        uint32_t inSweep = 0;
        uint32_t inSweepCore = 0;
        for (uint32_t c : o.idx) { inSweep += slidSweep[c] != 0; inSweepCore += slidSweep[c] == 2; }
        // (2: on its path, 1: within 2 cells of it). A rigidly moving piece there (explained by a translation, a block
        // edge's odd shift) belongs to the slid object; a piece redrawn in place on it (a mouth: no translation explains
        // it) stays redrawn and pops.
        if (o.kind == ObjectClass::Redrawn && inSweep * 2 >= o.cells && o.unexplShare < 0.3f && (o.slidShare >= 0.5f || inSweepCore * 5 >= o.cells * 4))
            o.kind = ObjectClass::Slid;
        out.push_back(std::move(o));
    }
    return out;
}

// ---- a scrolling layer (credits): counted for the close log ("scroll layer: N pairs") ----
// A pair scrolls when a steady, mostly vertical slide (>= 1/4 cell, |vx| <= |vy| / 2) moves the picture: the camera
// itself on a rigid move (a roll on black: the text is all the texture), or a Slid object of >= 64 cells that slid the
// same way on the pair before (a text layer over a held or slower picture). It needs no extra work: Slid objects keep
// the generator's output in every mode (smooth); the counter shows the classification did its job on a credit roll.
inline bool ScrollPair(const RegionFrame& r, const BlockMotion& m, const std::vector<ObjectClass>& objs)
{
    auto vertical = [](float vx, float vy) { return std::fabs(vy) >= 0.25f && std::fabs(vx) <= 0.5f * std::fabs(vy); };
    if (m.rigid && vertical(r.dx + m.camFixX, r.dy + m.camFixY)) return true;
    for (const auto& o : objs)
        if (o.kind == ObjectClass::Slid && o.consistent && o.cells >= kMinUnexplainedCells && vertical(r.dx + m.camFixX + o.vx, r.dy + m.camFixY + o.vy)) return true;
    return false;
}

// ---- transients over the whole picture ----
inline float GridMean(const CellGrid& g)
{
    double s = 0; for (float v : g.v) s += v;
    return g.v.empty() ? 0.f : float(s / double(g.v.size()));
}
// camera-compensated mean |a - b| (b moved by dx, dy cells), every second cell, a 4-cell margin
inline float MeanAbsShifted(const CellGrid& a, const CellGrid& b, float dx, float dy)
{
    double e = 0; int n = 0;
    for (uint32_t y = 4; y + 4 < a.h; y += 2) for (uint32_t x = 4; x + 4 < a.w; x += 2) { e += std::fabs(a.v[size_t(y) * a.w + x] - b.sample(float(x) - dx, float(y) - dy)); ++n; }
    return n ? float(e / n) : 0.f;
}
inline constexpr float kFlashCodes = 8.f;      // mean jump of a flash (codes)
inline constexpr float kFlashAgree = 0.4f;     // the neighbours agree within this share of the jump
struct TransientMarks { bool flash = false, flash2 = false, insert = false; };
// frame s (needs s-1, s+1 and for the two-frame flash s+2): means m[-1..2] = means of s-1, s, s+1, s+2 (NaN: unknown);
// regions r0 = change at s, r1 = at s+1, r2 = at s+2 (may be invalid); grids gm1 = s-1, g0 = s, g1 = s+1
inline TransientMarks DetectTransient(const float m[4], const RegionFrame& r0, const RegionFrame& r1, const RegionFrame& r2,
                                      const CellGrid* gm1, const CellGrid* g0, const CellGrid* g1)
{
    TransientMarks t;
    const float ma = m[0], ms = m[1], mb = m[2];
    if (std::isnan(ma) || std::isnan(ms) || std::isnan(mb)) return t;
    const float a = ms - ma, b = ms - mb, c = std::fabs(mb - ma), j = std::min(std::fabs(a), std::fabs(b));
    t.flash = a * b > 0 && j >= kFlashCodes && c < kFlashAgree * j;
    if (!t.flash && !std::isnan(m[3]) && r0.valid && !r0.cut && !(r1.valid && r1.cut)) {
        const float a2 = ms - ma, b2 = mb - m[3], c2 = std::fabs(m[3] - ma), j2 = std::min(std::fabs(a2), std::fabs(b2));
        t.flash2 = a2 * b2 > 0 && j2 >= kFlashCodes && c2 < kFlashAgree * j2 && std::fabs(mb - ms) < 0.5f * j2;
    }
    (void)r2;
    if (gm1 && g0 && g1 && r0.valid && r1.valid && r0.changedShare() > 0.3 && r1.changedShare() > 0.3) {
        const float e01 = MeanAbsShifted(*g0, *gm1, r0.dx, r0.dy), e12 = MeanAbsShifted(*g1, *g0, r1.dx, r1.dy);
        const float e02 = MeanAbsShifted(*g1, *gm1, r0.dx + r1.dx, r0.dy + r1.dy);
        t.insert = !t.flash && e02 < 0.5f * std::min(e01, e12);
    }
    return t;
}

// ---- the hold plan of one pair (s-1, s) ----
struct HoldPlan {
    enum Kind : uint8_t { Flash = 1, Insert = 2, Unexplained = 4, Redrawn = 8 };
    uint8_t kinds = 0;
    bool whole = false;          // the whole picture (a flash, an impact frame)
    bool nearest = false;        // true: A for t < tSwitch, B from it (transients: 0.5); false: A camera-warped for every t (as drawn)
    float tSwitch = 0.5f;        // P9 film time: the drawn parts switch to B at this t (the drawing's film time inside the pair)
    float camDx = 0, camDy = 0, camScale = 1.f;   // the pair's camera (cells; content moved by +dx, +dy, scale about the centre)
    uint32_t w = 0, h = 0, cells = 0;             // mask size and cells set
    uint32_t objects = 0, slid = 0, redrawn = 0, unexplained = 0;   // counters for the log
    std::vector<uint8_t> mask;                    // per cell 0 / 255 (empty when whole)
    bool any() const { return whole || cells > 0; }
};
inline constexpr int kHoldDilate = 2;   // cells round an object (the feather is in the compose)

// sweep of an object's changed cells back along its motion (the in-between ghost lies between its footprints), dilated
inline void PaintSweep(const ObjectClass& o, uint32_t W, uint32_t H, std::vector<uint8_t>& mask, uint32_t& set)
{
    const int steps = int(std::ceil(std::max(std::fabs(o.vx), std::fabs(o.vy)) / 2.f));
    for (uint32_t c : o.idx) {
        const int x = int(c % W), y = int(c / W);
        for (int k = 0; k <= steps; ++k) {
            const float f = steps ? float(k) / float(steps) : 0.f;
            const int sx = x - int(std::lround(f * o.vx)), sy = y - int(std::lround(f * o.vy));
            for (int j = -kHoldDilate; j <= kHoldDilate; ++j)
                for (int i = -kHoldDilate; i <= kHoldDilate; ++i) {
                    const int xx = sx + i, yy = sy + j;
                    if (xx < 0 || yy < 0 || xx >= int(W) || yy >= int(H)) continue;
                    uint8_t& m = mask[size_t(yy) * W + xx];
                    if (!m) { m = 255; ++set; }
                }
        }
    }
}

// mode: the stream's AnimMode; flags: kHold*; animation: the live-action gate is open (or Animation On); W x H: the cell
// grid. atPrev2 / atPrev / atCur: the transient marks of s-2, s-1 and s (the pair's ends are s-1 and s; a two-frame flash
// marks its first frame, so s-2's mark covers s-1 and s too). camFixX / Y: MatchBlocks' quarter-cell correction of the
// camera (BlockMotion::camFixX / Y), added to the compose's camera warp (a slow roll is otherwise up to half a cell off).
inline HoldPlan PlanHolds(AnimMode mode, uint32_t flags, bool animation, const RegionFrame& r, const std::vector<ObjectClass>& objs,
                          const TransientMarks& atPrev2, const TransientMarks& atPrev, const TransientMarks& atCur, uint32_t W, uint32_t H,
                          float camFixX = 0.f, float camFixY = 0.f, float filmSwitch = -1.f)
{
    HoldPlan p;
    if (!r.valid) return p;
    p.camDx = r.dx + camFixX; p.camDy = r.dy + camFixY; p.camScale = r.scale;
    // flash / impact: every mode but Off, and on live action too with kHoldLiveTransients (the user's rule: a flash is
    // never blended, whatever the video)
    const bool transients = (flags & kHoldTransients) != 0 && mode != AnimMode::Off;
    if (transients && (animation || (flags & kHoldLiveTransients))) {
        // a flash frame at either end of the pair (or inside a two-frame flash) shows for exactly its own interval
        const bool flash = atCur.flash || atPrev.flash || atCur.flash2 || atPrev.flash2 || atPrev2.flash2;
        const bool insert = atCur.insert || atPrev.insert;
        if (flash || insert) {
            p.whole = true; p.nearest = true;
            p.kinds = uint8_t((flash ? HoldPlan::Flash : 0) | (insert ? HoldPlan::Insert : 0));
            return p;
        }
    }
    if (r.cut || objs.empty() || size_t(W) * H != r.changed.size()) return p;
    // P9 film time (filmSwitch in [0, 1): the drawn layer of a 24p-grid shot changes at the end of this pair): the drawn
    // parts are held and switch to the new drawing at its film time; camera and effects (slid objects) keep the generator's
    // own 30p motion. Camera only mode only (Full re-times drawings with the source repair).
    const bool film = filmSwitch >= 0.f && filmSwitch < 1.f && (flags & kHoldFilmTime) != 0 && mode == AnimMode::CameraOnly && animation;
    const bool redrawnHold = mode == AnimMode::CameraOnly && animation;
    const bool unexplHold = transients && animation;
    if (!redrawnHold && !unexplHold) return p;
    p.w = W; p.h = H;
    p.mask.assign(size_t(W) * H, 0);
    for (const auto& o : objs) {
        ++p.objects;
        if (o.kind == ObjectClass::Slid) { ++p.slid; continue; }
        if (o.kind == ObjectClass::Unexplained) ++p.unexplained; else ++p.redrawn;
        const bool hold = o.kind == ObjectClass::Unexplained || redrawnHold;
        if (!hold) continue;
        PaintSweep(o, W, H, p.mask, p.cells);
        p.kinds |= o.kind == ObjectClass::Unexplained ? HoldPlan::Unexplained : HoldPlan::Redrawn;
    }
    // Camera only holds as drawn (A until B's own time); Full holds only unexplained objects, nearest in time. Owner
    // rule: a slid object inside a held object's mask is held with it (the redrawn one wins where they overlap).
    p.nearest = !redrawnHold || film;
    if (film) p.tSwitch = filmSwitch;
    if (!p.cells) { p.mask.clear(); p.w = p.h = 0; }
    return p;
}

// ---- P9 film time: is the drawn layer of this shot on a 24p grid inside 29.97 video (internal cadence report) ----
// Fed once per pair (n-1, n) with whether the DRAWN layer changed (redrawn / unexplained objects; camera and slid
// effects excluded), the pair's cut flag and the pair's own interval (from the frames' timestamps). Over the last 30
// frames of the shot:
//   a run of >= 6 consecutive drawn changes            -> 30p motion (not engaged);
//   one phase of every 5 never changes, others do      -> 24p grid (engaged after it held for kConfirm pairs in a row;
//                                                         that phase is the repeated one);
//   all 5 phases change                                 -> native 30p (not engaged);
//   gaps of exactly 3, three times in a row (3,3,3)    -> native 30p on threes: disengaged at once;
//   a cut, or an interval that is not 1/29.97 (a VFR file's 23.976 body: its timestamps already time it) -> reset.
// Intervals: 32.5-34.6 ms pass (containers with millisecond timestamps give 33 / 34 ms); 1/24 s (41-42 ms) resets.
// Drawings on twos of a 24p grid leave three phases empty and are not engaged (the repeated phase is ambiguous).
class FilmTimeDetector {
public:
    static constexpr int kWindow = 30, kMinFrames = 20, kRun30p = 6, kConfirm = 5;
    static bool Interval30(double ms) { return ms > 32.5 && ms < 34.6; }
    void Reset() { hist_.clear(); dup_ = -1; cand_ = -1; candRun_ = 0; lastChange_ = -1; gaps_ = 0; }
    void Push(uint64_t n, bool drawnChanged, bool cut, double intervalMs) {
        if (cut || !Interval30(intervalMs)) { Reset(); return; }
        hist_.push_back({ n, drawnChanged });
        while (hist_.size() > size_t(kWindow)) hist_.erase(hist_.begin());
        if (drawnChanged) {
            const int64_t gap = lastChange_ >= 0 ? int64_t(n) - lastChange_ : 0;
            gaps_ = gap == 3 ? gaps_ + 1 : 0;
            lastChange_ = int64_t(n);
        }
        const int c = Candidate();
        if (c < 0) { dup_ = -1; cand_ = -1; candRun_ = 0; return; }
        candRun_ = c == cand_ ? candRun_ + 1 : 1;
        cand_ = c;
        dup_ = candRun_ >= kConfirm || dup_ == c ? c : -1;
    }
    bool Engaged() const { return dup_ >= 0; }
    // a drawn change landing at frame n: the t inside the pair (n-1, n) where its film time lies (1 = at n itself).
    // Model: 24p film frames k start at (k + o) * 1.25 frames of 29.97 and each 29.97 frame shows the latest one, so
    // frame n shows film frame floor(0.8 n - o) and the repeated phase fixes o to an interval of 0.2: its centre is used
    // (error <= 0.125 of a 29.97 frame).
    float SwitchT(uint64_t n) const {
        if (dup_ < 0) return 1.f;
        const double o = Origin(dup_);
        const double k = std::floor(0.8 * double(n) - o);
        return std::clamp(float((k + o) * 1.25 - (double(n) - 1.0)), 0.f, 1.f);
    }
    int DupPhase() const { return dup_; }
    // the film origin (in film frames, [0, 1)) that gives repeated phase r: centre of the o for which frame 5q + r shows
    // the same film frame as the frame before it
    static double Origin(int r) {
        double sx = 0, sy = 0;
        for (int i = 0; i < 1000; ++i) {
            const double o = (i + 0.5) / 1000.0;
            if (std::floor(0.8 * r - o) != std::floor(0.8 * (r - 1) - o)) continue;
            sx += std::cos(6.283185307179586 * o); sy += std::sin(6.283185307179586 * o);   // circular mean (the range may wrap)
        }
        double m = std::atan2(sy, sx) / 6.283185307179586;
        return m < 0 ? m + 1.0 : m;
    }
private:
    int Candidate() const {
        if (gaps_ >= 3 || hist_.size() < size_t(kMinFrames)) return -1;
        int run = 0, total = 0, perPhase[5] = {};
        for (const auto& h : hist_) { run = h.changed ? run + 1 : 0; if (run >= kRun30p) return -1; if (h.changed) { ++total; ++perPhase[h.n % 5]; } }
        int zero = -1, zeros = 0;
        for (int k = 0; k < 5; ++k) if (!perPhase[k]) { zero = k; ++zeros; }
        return zeros == 1 && total >= 4 ? zero : -1;
    }
    struct H { uint64_t n; bool changed; };
    std::vector<H> hist_;
    int dup_ = -1, cand_ = -1, candRun_ = 0, gaps_ = 0;
    int64_t lastChange_ = -1;
};

// ---- the show's class (Auto per show) from the cadence of its moving parts over the first minutes ----
enum class ShowClass : uint8_t { Unknown = 0, Feature = 1, TvLimited = 2, MixedCg = 3 };
class ShowClassifier {
public:
    static constexpr uint32_t kFrames = 2880;   // ~2 min at 24 fps
    void Reset() { m_frames = m_ones = m_more = 0; m_class = ShowClass::Unknown; }
    // a classified frame: the median cadence of its changed cells (0 unknown), the live-action gate's state
    void Push(float cadence, bool animation)
    {
        if (m_class != ShowClass::Unknown) return;
        ++m_frames;
        if (cadence > 0.f) { if (cadence <= 1.25f) ++m_ones; else if (cadence >= 1.75f) ++m_more; }
        m_anim += animation;
        if (m_frames >= kFrames) m_class = Decide();
    }
    ShowClass Decide() const
    {
        if (m_frames < 240) return ShowClass::Unknown;
        if (double(m_anim) < 0.25 * double(m_frames)) return ShowClass::MixedCg;
        const uint32_t n = m_ones + m_more;
        if (!n) return ShowClass::TvLimited;
        return double(m_ones) >= 0.5 * double(n) ? ShowClass::Feature : ShowClass::TvLimited;
    }
    ShowClass Class() const { return m_class; }
    // the class so far (before kFrames): for the log
    ShowClass Provisional() const { return m_class != ShowClass::Unknown ? m_class : Decide(); }
private:
    uint32_t m_frames = 0, m_ones = 0, m_more = 0, m_anim = 0;
    ShowClass m_class = ShowClass::Unknown;
};

} // namespace pushmpc::held
