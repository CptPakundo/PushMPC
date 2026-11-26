// PushMPC: held-frame re-timing for frame generation on the pushcore route (filter / MPC-HC, MPC-BE). Pure C++, no
// GPU: pushmpc/tests/held_frames_test.cpp checks it on CI.
//
// Low-budget animation is drawn "on twos" or "on threes": a 24 fps file shows each drawing for 2 or 3 frames. Frame
// generation between each pair of source frames then moves the picture in one step and holds it in the next, so the
// motion pulses at any multiplier. Here a source frame that repeats the one before it (a held frame, found by the GPU
// difference in PushCoreHeld.h) is not given to the graph: the next distinct frame is, and the graph's frames between
// the two distinct frames are spread over the whole held span (EnhanceGraph::process takes the pair's own timestamps,
// so its in-between frames fall between the two distinct frames, and a per-call multiplier, previewMultiplier).
//
// The output grid stays the one of plain playback: with k outputs per source frame (the multiplier the renderer sees),
// a span of g source frames between two distinct frames gets M generated slots, M the largest divisor of g x k that
// the graph can make in one pair (its built multiplier). Every output then falls on the 1/k grid; when M < g x k the
// renderer shows each output for g x k / M grid steps (an even cadence, a lower rate in that span). The graph is built
// with the multiplier that makes the best M possible for g = 1..3 (BuildMultiplier), so more held frames never need a
// rebuild.
//
// Lookahead: a run of up to `lookahead` held frames (3: twos, threes and fours; TV anime holds slow motion for four
// frames about as often as for three) waits for the next distinct frame. A longer
// run is a still picture (a pause, a held shot): its frames go to the graph as they are (an unchanged picture, plain
// frame generation), and only its last interval moves to the next distinct frame - the rule of the GPU engine's Dedup
// (vsplugin/engines/gpu/cadence.h). The extra wait is latency only, never a timestamp change: the outputs keep their
// true times (A/V sync unchanged); the caller lets the source run further ahead (pushcore's pending queue) to absorb it.
//
// A frame that repeats except for a small region (a mouth, a blinking light, a subtitle that appears) is a distinct
// frame: IsHeld() asks for nearly no changed cell anywhere. So partial changes keep plain frame generation, as before.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <vector>

namespace pushmpc::held {

// What the GPU difference pass returns for one frame against the frame before it (PushCoreHeld.h). A cell is a block
// of f x f luma pixels (f by the source width, ~480 cells across); its value is the absolute difference of the two
// frames' cell means, in 1/16 of an 8-bit code value.
struct DiffStats {
    uint32_t cells = 0;          // cells compared
    uint32_t maxQ = 0;           // largest cell difference (1/16 code)
    uint32_t over[4] = {};       // cells above 2, 4, 8, 16 codes
    uint64_t sumQ = 0;           // sum of the cell differences (1/16 code)
    bool valid = false;
    double meanCodes() const { return cells ? double(sumQ) / 16.0 / double(cells) : 0.0; }
};
inline constexpr uint32_t kOverCodes[4] = { 2, 4, 8, 16 };

// Held = the same drawing. Grain, dither and compression noise move a cell mean by a code or two at most (the noise of
// a cell mean falls with its size, as the change a moving line makes does), while a line of the drawing that moves by
// one pixel shifts every cell it crosses by ~150 / f codes (37 at 1080p, 19 at 4K). So a frame is held when
//  - at most kMaxOver8 cells, or 1 in 20000 (a 10x10-pixel spot at 1080p), changed by more than 8 codes, and
//  - the mean change stays below kMaxMean codes (a fade or flicker of the whole picture is a change).
// Calibrated on TV anime and films, and checked on CI (held_test, a synthetic clip on WARP).
inline constexpr uint32_t kMaxOver8 = 3;
inline constexpr double kMaxMean = 3.0;
// tolerant: small parts may change (a mouth, a blink: up to kSmallShare of the cells), for where the per-region span
// stream does not run (the whole frame then waits for the body's next drawing; the small part's change is spread over
// the span). On TV anime this doubles the re-timed frames (Pokémon 21 -> 45 %), live action 2 -> 7 % (near-still shots).
inline constexpr double kSmallShare = 0.003;
inline bool IsHeld(const DiffStats& d, bool tolerant = false)
{
    if (!d.valid || d.cells == 0) return false;
    const uint32_t limit = tolerant ? std::max<uint32_t>(kMaxOver8, uint32_t(d.cells * kSmallShare)) : std::max<uint32_t>(kMaxOver8, d.cells / 20000);
    return d.over[2] <= limit && d.meanCodes() < kMaxMean;
}

// M: the largest divisor of g x k that is at most cap (>= 1).
inline uint32_t SpanMultiplier(uint32_t g, uint32_t k, uint32_t cap)
{
    const uint32_t n = std::max<uint32_t>(1, g) * std::max<uint32_t>(1, k);
    for (uint32_t m = std::min(n, std::max<uint32_t>(1, cap)); m > 1; --m) if (n % m == 0) return m;
    return 1;
}
// The graph's multiplier for k outputs per source frame and a graph limit `cap`: the smallest one that gives the best
// span multiplier for every gap up to lookahead + 1 (no larger than needed: VRAM).
inline constexpr int kLookahead = 3;
inline uint32_t BuildMultiplier(uint32_t k, uint32_t cap, int lookahead = kLookahead)
{
    if (k < 2) return k;
    uint32_t b = std::min(k, cap);
    for (uint32_t g = 2; g <= uint32_t(lookahead) + 1; ++g) b = std::max(b, SpanMultiplier(g, k, cap));
    return std::min(b, cap);
}

// ------------------------------------------------------------------------------------------------ per region
// The cell grid of one frame (cell means in codes, w x h cells of `cell` pixels), read back from the GPU pass.
struct CellGrid {
    uint32_t w = 0, h = 0, cell = 0;
    std::vector<float> v;
    bool ok() const { return w && h && v.size() == size_t(w) * h; }
    float at(int x, int y) const { x = std::clamp(x, 0, int(w) - 1); y = std::clamp(y, 0, int(h) - 1); return v[size_t(y) * w + x]; }
    float sample(float x, float y) const {   // bilinear, edges clamped
        const int x0 = int(std::floor(x)), y0 = int(std::floor(y)); const float fx = x - x0, fy = y - y0;
        return at(x0, y0) * (1 - fx) * (1 - fy) + at(x0 + 1, y0) * fx * (1 - fy) + at(x0, y0 + 1) * (1 - fx) * fy + at(x0 + 1, y0 + 1) * fx * fy;
    }
};

// One frame against the one before: the dominant (camera) shift and, per cell, whether its content changed. A cell is
// "held-or-slid" when it matches the previous frame unmoved OR moved by the camera shift (a held character drawing over a
// panning background, or slid with it); otherwise it changed (a new drawing, a moving part, a cut).
struct RegionFrame {
    bool valid = false;
    float dx = 0, dy = 0;               // camera shift in cells (content moved by +dx, +dy)
    std::vector<uint8_t> changed;       // per cell
    uint32_t changedCells = 0, cells = 0;
    bool cut = false;                   // most of the picture changed and no shift explains it
    double changedShare() const { return cells ? double(changedCells) / cells : 0.0; }
};
inline constexpr float kCellChangeCodes = 8.0f;
inline constexpr double kCutShare = 0.5;

// best integer shift of `cur` against `prev` on a grid aggregated by `a`, within +-r (in aggregated cells), around (cx, cy)
inline void SearchShift(const CellGrid& prev, const CellGrid& cur, int a, int r, int cx, int cy, int& bx, int& by)
{
    const int w = int(cur.w) / a, h = int(cur.h) / a;
    auto agg = [&](const CellGrid& g, int x, int y) { float s = 0; for (int j = 0; j < a; ++j) for (int i = 0; i < a; ++i) s += g.at(x * a + i, y * a + j); return s / float(a * a); };
    std::vector<float> P(size_t(w) * h), C(size_t(w) * h);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) { P[size_t(y) * w + x] = agg(prev, x, y); C[size_t(y) * w + x] = agg(cur, x, y); }
    const int m = r + std::max(std::abs(cx), std::abs(cy)) + 1;
    double best = 1e30; bx = cx; by = cy;
    for (int dy = cy - r; dy <= cy + r; ++dy)
        for (int dx = cx - r; dx <= cx + r; ++dx) {
            double e = 0; int n = 0;
            for (int y = m; y < h - m; y += 1) for (int x = m; x < w - m; x += 1) { e += std::fabs(C[size_t(y) * w + x] - P[size_t(y - dy) * w + (x - dx)]); ++n; }
            if (n && e / n < best) { best = e / n; bx = dx; by = dy; }
        }
}
inline RegionFrame CompareRegions(const CellGrid& prev, const CellGrid& cur)
{
    RegionFrame r;
    if (!prev.ok() || !cur.ok() || prev.w != cur.w || prev.h != cur.h || cur.w < 32 || cur.h < 18) return r;
    // camera shift: coarse (4x4 aggregated, +-8 = +-32 cells), then 2x2 (+-2), then the cell grid (+-1) with a parabola
    int bx = 0, by = 0;
    SearchShift(prev, cur, 4, 8, 0, 0, bx, by);
    SearchShift(prev, cur, 2, 2, bx * 2, by * 2, bx, by);
    const int W = int(cur.w), H = int(cur.h);
    const int mg = std::max(std::abs(bx), std::abs(by)) + 3;
    auto err = [&](float dx, float dy) {
        double e = 0; int n = 0;
        for (int y = mg; y < H - mg; y += 2) for (int x = mg; x < W - mg; x += 2) { e += std::fabs(cur.at(x, y) - prev.sample(x - dx, y - dy)); ++n; }
        return n ? e / n : 1e30;
    };
    float fx = float(bx), fy = float(by);
    {
        double be = err(fx, fy);
        for (int j = -1; j <= 1; ++j) for (int i = -1; i <= 1; ++i) { const double e = err(float(bx + i), float(by + j)); if (e < be) { be = e; fx = float(bx + i); fy = float(by + j); } }
        const double ex0 = err(fx - 1, fy), ex1 = err(fx + 1, fy), ey0 = err(fx, fy - 1), ey1 = err(fx, fy + 1);
        const double dxx = ex0 - 2 * be + ex1, dyy = ey0 - 2 * be + ey1;
        if (dxx > 1e-9) fx += float(std::clamp(0.5 * (ex0 - ex1) / dxx, -0.5, 0.5));
        if (dyy > 1e-9) fy += float(std::clamp(0.5 * (ey0 - ey1) / dyy, -0.5, 0.5));
    }
    const bool slid = std::fabs(fx) + std::fabs(fy) >= 0.25f;
    r.dx = fx; r.dy = fy; r.cells = uint32_t(W) * H; r.changed.assign(r.cells, 0);
    std::vector<uint8_t> raw(r.cells, 0);
    uint32_t rawChanged = 0, rawPlain = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float c = cur.at(x, y);
            const float d0 = std::fabs(c - prev.at(x, y));
            rawPlain += d0 > kCellChangeCodes;
            // cells the shift brings in from outside the picture cannot be compared: unchanged unless they differ plainly
            const bool inside = x - fx >= 0 && x - fx <= W - 1 && y - fy >= 0 && y - fy <= H - 1;
            const float d1 = slid && inside ? std::fabs(c - prev.sample(x - fx, y - fy)) : d0;
            const bool ch = std::min(d0, d1) > kCellChangeCodes;
            raw[size_t(y) * W + x] = ch; rawChanged += ch;
        }
    // an isolated changed cell is noise: a changed cell needs a changed 8-neighbour
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            if (!raw[size_t(y) * W + x]) continue;
            int n = 0;
            for (int j = -1; j <= 1; ++j) for (int i = -1; i <= 1; ++i) {
                if (!i && !j) continue;
                const int xx = x + i, yy = y + j;
                if (xx >= 0 && yy >= 0 && xx < W && yy < H) n += raw[size_t(yy) * W + xx];
            }
            if (n) { r.changed[size_t(y) * W + x] = 1; ++r.changedCells; }
        }
    r.cut = double(rawChanged) / r.cells > kCutShare && double(rawPlain) / r.cells > kCutShare;
    r.valid = true;
    return r;
}

// The phase a region's output shows inside its span: t = (tau - start) / span length. Linear keeps the animator's
// spacing (drawn intent: ease-in / ease-out between drawings stays as drawn). One function, so a Hermite shaping (from the
// neighbouring spans' displacement) can be switched on.
enum class PhaseShape { Linear = 0, Hermite = 1 };
inline float ShapePhase(float t, PhaseShape shape = PhaseShape::Linear, float v0 = 1.f, float v1 = 1.f)
{
    t = std::clamp(t, 0.f, 1.f);
    if (shape == PhaseShape::Linear) return t;
    // cubic Hermite from 0 to 1 with end slopes v0, v1 (relative velocities at the span boundaries; 1 = linear)
    const float t2 = t * t, t3 = t2 * t;
    return std::clamp((t3 - 2 * t2 + t) * v0 + (-2 * t3 + 3 * t2) + (t3 - t2) * v1, 0.f, 1.f);
}

// Source repair (per-region re-timing): at a source frame s, every part of the picture that is held there between two
// of its drawings d0 < s < d1 gets the span stream's pair d0 -> d1 at its own phase t = (s - d0) / (d1 - d0), laid into
// s inside its mask (PushCoreRegion.h); the graph then interpolates the repaired source as usual. A cell belongs to the
// group (d0, d1) when it did not change at s, changed last at d0 (within the lookahead back) and changes next at d1
// (within the lookahead ahead): decided from both ends, so the first span after a hold is caught too. Cells of the same
// (d0, d1) share one span-stream frame however many parts they are; parts out of phase (two characters on twos, one on
// even and one on odd frames) are two groups, each repaired on its own frames.
// Gates: no cut and no fade (more than kFadeShare of the picture changing) in (d0, d1]; a span longer than the cadence
// cap is a hold (only its last interval moves, as the whole-frame re-timer does); at least kMinGroupShare of the cells.
struct RepairGroup {
    uint64_t d0 = 0, d1 = 0;
    float t = 0.5f;
    uint32_t w = 0, h = 0, cells = 0;
    float vx = 0, vy = 0;                 // the group's motion d0 -> d1 in cells (block match on the cell grids)
    std::vector<uint8_t> mask;            // per cell 0 / 255: the sweep of the group along its motion, dilated
};
inline constexpr double kMinGroupShare = 0.0008;   // about 100 cells: a small part (a hand) is still a part
inline constexpr double kFadeShare = 0.6;
inline constexpr uint32_t kCadenceCap = 4;   // the longest drawing span that still moves over its whole length
template <class FrameAt, class GridAt>
inline std::vector<RepairGroup> PlanRepairs(uint64_t s, FrameAt frameAt, GridAt gridAt, int lookahead, PhaseShape shape = PhaseShape::Linear)
{
    std::vector<RepairGroup> out;
    const RegionFrame* fs = frameAt(s);
    if (!fs || !fs->valid || fs->cut) return out;
    const size_t n = fs->changed.size();
    const CellGrid* gs = gridAt(s);
    if (!gs || !gs->ok() || gs->v.size() != n) return out;
    const uint32_t W = gs->w, H = gs->h;
    // the frames around s, and the gates per frame
    std::vector<const RegionFrame*> back, ahead;   // back[k] = frame s - k (k >= 1: the change AT s - k), ahead[k - 1] = s + k
    for (int k = 1; k <= lookahead; ++k) { const RegionFrame* f = frameAt(s - uint64_t(k)); if (!f || !f->valid || s < uint64_t(k)) break; back.push_back(f); }
    for (int k = 1; k <= lookahead; ++k) { const RegionFrame* f = frameAt(s + uint64_t(k)); if (!f || !f->valid) break; ahead.push_back(f); }
    if (back.empty() || ahead.empty()) return out;
    auto bad = [](const RegionFrame* f) { return f->cut || f->changedShare() > kFadeShare; };
    if (bad(fs)) return out;
    // per cell: the last change at or before s - 1 (d0 = s - k where back[k-1] changed... the change AT frame j marks j
    // as the first frame of the new drawing) and the next one after s
    std::vector<std::pair<int, int>> key(n, {0, 0});
    for (size_t i = 0; i < n; ++i) {
        if (fs->changed[i]) continue;
        int k0 = 0;
        for (size_t k = 0; k < back.size(); ++k) if (back[k]->changed[i]) { k0 = int(k) + 1; break; }
        if (!k0) continue;
        int k1 = 0;
        for (size_t k = 0; k < ahead.size(); ++k) if (ahead[k]->changed[i]) { k1 = int(k) + 1; break; }
        if (!k1) continue;
        key[i] = {k0, k1};
    }
    // groups by (k0, k1)
    std::vector<std::pair<std::pair<int, int>, uint32_t>> groups;
    for (size_t i = 0; i < n; ++i) {
        if (!key[i].first) continue;
        bool found = false;
        for (auto& g : groups) if (g.first == key[i]) { ++g.second; found = true; break; }
        if (!found) groups.push_back({key[i], 1u});
    }
    for (const auto& g : groups) {
        const int k0 = g.first.first, k1 = g.first.second;
        if (g.second < kMinGroupShare * n) continue;
        // gates over (d0, d1]: the changes at s - k0 + 1 .. s + k1 (back[k0-2]..back[0], fs, ahead[0..k1-1])
        bool ok = true;
        for (int k = 0; k + 1 < k0 && ok; ++k) ok = !bad(back[size_t(k)]);
        for (int k = 0; k < k1 && ok; ++k) ok = !bad(ahead[size_t(k)]);
        if (!ok) continue;
        RepairGroup r;
        r.d0 = s - uint64_t(k0); r.d1 = s + uint64_t(k1);
        const uint32_t span = uint32_t(k0 + k1);
        // a span longer than the cadence cap is a hold: only its last kCadenceCap... interval moves (cap 4: d0 is moved to
        // d1 - cap, frames before that keep the drawing as it is)
        if (span > kCadenceCap) { if (uint64_t(k1) >= kCadenceCap) continue; r.d0 = r.d1 - kCadenceCap; }
        r.t = ShapePhase(float(double(s - r.d0) / double(r.d1 - r.d0)), shape);
        if (!(r.t > 0.f && r.t < 1.f)) continue;
        r.w = W; r.h = H; r.cells = g.second;
        std::vector<uint8_t> cells(n, 0);
        int x0 = int(W), y0 = int(H), x1 = -1, y1 = -1;
        for (size_t i = 0; i < n; ++i) if (key[i] == g.first) { cells[i] = 1; const int x = int(i % W), y = int(i / W); x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y); }
        // the group's motion d0 -> d1: block match of its bounding box on the two cell grids (+-16 cells)
        const CellGrid* g0 = gridAt(r.d0); const CellGrid* g1 = gridAt(r.d1);
        if (g0 && g1 && g0->ok() && g1->ok() && g0->w == W && g1->w == W) {
            double best = 1e30; int bx = 0, by = 0;
            for (int dy = -16; dy <= 16; dy += 1)
                for (int dx = -16; dx <= 16; dx += 1) {
                    double e = 0; int m = 0;
                    for (int y = y0; y <= y1; y += 2) for (int x = x0; x <= x1; x += 2) {
                        if (!cells[size_t(y) * W + x]) continue;
                        e += std::fabs(g1->at(x + dx, y + dy) - g0->at(x, y)); ++m;
                    }
                    if (m && e / m < best) { best = e / m; bx = dx; by = dy; }
                }
            r.vx = float(bx); r.vy = float(by);
        }
        // the sweep: every group cell along +-(v) (the footprints at d0 and d1 and the positions between), then a dilation
        // of 2 cells (the feather plus the camera's sub-cell error stay outside the part)
        std::vector<uint8_t> sweep(n, 0);
        const int steps = int(std::ceil(std::max(std::fabs(r.vx), std::fabs(r.vy)))) + 1;
        for (size_t i = 0; i < n; ++i) {
            if (!cells[i]) continue;
            const int x = int(i % W), y = int(i / W);
            for (int k = -steps; k <= steps; ++k) {
                const int xx = x + int(std::lround(r.vx * k / float(steps))), yy = y + int(std::lround(r.vy * k / float(steps)));
                if (xx >= 0 && yy >= 0 && xx < int(W) && yy < int(H)) sweep[size_t(yy) * W + xx] = 1;
            }
        }
        r.mask.assign(n, 0);
        const int dil = 2;
        for (int y = 0; y < int(H); ++y)
            for (int x = 0; x < int(W); ++x) {
                bool on = false;
                for (int j = -dil; j <= dil && !on; ++j) for (int i = -dil; i <= dil && !on; ++i) {
                    const int xx = x + i, yy = y + j;
                    on = xx >= 0 && yy >= 0 && xx < int(W) && yy < int(H) && sweep[size_t(yy) * W + xx];
                }
                // a cell that changes at s itself (another part, a transient on ones) keeps the frame's own picture
                if (on && !fs->changed[size_t(y) * W + x]) r.mask[size_t(y) * W + x] = 255;
            }
        out.push_back(std::move(r));
    }
    return out;
}

// One decision for a source frame (in source order).
struct Action {
    enum Kind { Submit, Drop } kind = Submit;
    uint64_t frame = 0;          // the caller's sequence number of the source frame
    uint32_t multiplier = 0;     // Submit: the graph's per-call multiplier for the pair that ends at this frame
    uint32_t gap = 1;            // Submit: source frames since the frame given to the graph before (1 = plain)
};

class Retimer {
public:
    void Configure(uint32_t k, uint32_t graphMultiplier, int lookahead = kLookahead)
    {
        m_k = std::max<uint32_t>(1, k); m_cap = std::max<uint32_t>(1, graphMultiplier); m_look = std::max(0, lookahead);
        Reset();
    }
    void Reset() { m_held.clear(); m_last = -1; m_newest = -1; m_still = false; }
    uint32_t K() const { return m_k; }
    // frames waiting for the next distinct one (their pictures must stay alive)
    size_t Waiting() const { return m_held.size(); }
    bool Waits(uint64_t frame) const { return std::find(m_held.begin(), m_held.end(), frame) != m_held.end(); }

    // the next source frame (consecutive sequence numbers; discontinuity: a seek, a gap, the first frame)
    void Push(uint64_t frame, bool held, bool discontinuity, std::vector<Action>& out)
    {
        if (m_k < 2) { out.push_back({ Action::Submit, frame, m_k, 1 }); m_last = int64_t(frame); return; }
        if (discontinuity || m_last < 0 || int64_t(frame) != m_newest + 1) {
            Flush(out);
            m_still = false;
            submit(frame, 1, out);
            m_newest = int64_t(frame);
            return;
        }
        m_newest = int64_t(frame);
        if (!held) {
            const uint32_t g = uint32_t(int64_t(frame) - m_last);
            if (!m_held.empty() && g == m_held.size() + 1) {
                for (uint64_t h : m_held) out.push_back({ Action::Drop, h, 0, 0 });
                m_held.clear();
                ++m_spans;
                submit(frame, g, out);
            } else {
                Flush(out);
                submit(frame, 1, out);
            }
            m_still = false;
            return;
        }
        ++m_heldSeen;
        if (m_still) { submit(frame, 1, out); return; }
        m_held.push_back(frame);
        if (m_held.size() > size_t(m_look)) {
            // longer than the lookahead: a still picture, shown as it is
            m_still = true;
            Flush(out);
        }
    }
    // the frames that wait go to the graph as they are (a still, the end of the stream, a seek)
    void Flush(std::vector<Action>& out)
    {
        for (uint64_t h : m_held) submit(h, 1, out);
        m_held.clear();
    }
    uint64_t Spans() const { return m_spans; }
    uint64_t HeldSeen() const { return m_heldSeen; }

private:
    void submit(uint64_t frame, uint32_t g, std::vector<Action>& out)
    {
        out.push_back({ Action::Submit, frame, g > 1 ? SpanMultiplier(g, m_k, m_cap) : std::min(m_k, m_cap), g });
        m_last = int64_t(frame);
    }
    uint32_t m_k = 1, m_cap = 1;
    int m_look = kLookahead;
    std::deque<uint64_t> m_held;
    int64_t m_last = -1, m_newest = -1;
    bool m_still = false;
    uint64_t m_spans = 0, m_heldSeen = 0;
};

// Share of held frames over the last `window` source frames (animation detection: TV anime on twos holds 30-60 %,
// live action and 3D CG at 24 distinct ~0).
class HeldRatio {
public:
    explicit HeldRatio(size_t window = 240) : m_window(window) {}
    void Push(bool held) { m_v.push_back(held); m_n += held; if (m_v.size() > m_window) { m_n -= m_v.front(); m_v.pop_front(); } }
    void Reset() { m_v.clear(); m_n = 0; }
    size_t Size() const { return m_v.size(); }
    double Ratio() const { return m_v.empty() ? 0.0 : double(m_n) / double(m_v.size()); }
private:
    size_t m_window;
    std::deque<bool> m_v;
    size_t m_n = 0;
};

} // namespace pushmpc::held
