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
// The cell grid of one frame (cell means in codes, w x h cells of `cell` pixels), read back from the GPU pass. Optional
// (empty when the pass did not give them): per cell the largest change of its four 2 x 2 sub-cell means against the
// frame before (codes; catches a thin line that moves inside a cell, which leaves the cell mean unchanged), and the
// cell's Cb / Cr means (codes; a colour change at the same brightness).
struct CellGrid {
    uint32_t w = 0, h = 0, cell = 0;
    std::vector<float> v;
    std::vector<float> sub, cb, cr;
    bool ok() const { return w && h && v.size() == size_t(w) * h; }
    bool hasSub() const { return sub.size() == v.size(); }
    bool hasChroma() const { return cb.size() == v.size() && cr.size() == v.size(); }
    // the GPU pass packs the extras into one uint per cell: bits 0-9 sub-cell change x 4, 10-20 Cb x 8, 21-31 Cr x 8
    void unpackAux(const uint32_t* aux)
    {
        const size_t n = v.size();
        sub.resize(n); cb.resize(n); cr.resize(n);
        for (size_t i = 0; i < n; ++i) {
            sub[i] = float(aux[i] & 1023u) * 0.25f; cb[i] = float((aux[i] >> 10) & 2047u) * 0.125f; cr[i] = float(aux[i] >> 21) * 0.125f;
        }
    }
    float at(int x, int y) const { x = std::clamp(x, 0, int(w) - 1); y = std::clamp(y, 0, int(h) - 1); return v[size_t(y) * w + x]; }
    float sample(float x, float y) const {   // bilinear, edges clamped
        const int x0 = int(std::floor(x)), y0 = int(std::floor(y)); const float fx = x - x0, fy = y - y0;
        return at(x0, y0) * (1 - fx) * (1 - fy) + at(x0 + 1, y0) * fx * (1 - fy) + at(x0, y0 + 1) * (1 - fx) * fy + at(x0 + 1, y0 + 1) * fx * fy;
    }
};

// One frame against the one before: the dominant (camera) shift and, per cell, whether its content changed. A cell is
// "held-or-slid" when it matches the previous frame unmoved OR moved by the camera shift (a held character drawing over a
// panning background, or slid with it); otherwise it changed (a new drawing, a moving part, a cut). With the GPU pass's
// extras a cell also changed when a 2 x 2 sub-cell mean moved (a thin line inside the cell) or its colour changed.
struct RegionFrame {
    bool valid = false;
    float dx = 0, dy = 0;               // camera shift in cells (content moved by +dx, +dy)
    std::vector<uint8_t> changed;       // per cell
    uint32_t changedCells = 0, cells = 0;
    bool cut = false;                   // most of the picture changed and no shift explains it
    float gain = 0;                     // median signed change of the cells (codes): a fade, a flash, a light change
    bool zoom = false;                  // a zoom faster than kZoomGate per frame (the similarity model below does not hold)
    float scale = 1.f;                  // camera scale against the previous frame about the picture centre (push-ins, pull-outs)
    double changedShare() const { return cells ? double(changedCells) / cells : 0.0; }
};
inline constexpr float kCellChangeCodes = 8.0f;    // the change threshold until NoiseFloor has seen the clip
inline constexpr float kChromaChangeCodes = 6.0f;  // |dCb| + |dCr| of a cell mean
inline constexpr double kCutShare = 0.5;
inline constexpr float kFadeGain = 1.5f;           // codes of median change: the whole picture brightens or darkens
inline constexpr float kZoomGate = 0.02f;          // scale change per frame beyond which a zoom gates the repair

// The clip's noise floor: the 99th percentile of the cell changes on wholly held frames (grain, dither and compression
// noise of a repeated drawing), smoothed. The change threshold follows it: clean digital anime (p99 about 1 code) gets
// 5 codes, so a dark thin line that moves is not taken for a held part; a grainy BD (p99 3 to 4) gets 10 to 12, so
// its grain is not taken for motion.
class NoiseFloor {
public:
    void Push(float p99) { m_v = m_n == 0 ? p99 : m_v + (p99 - m_v) * 0.05f; ++m_n; }
    float Threshold() const { return m_n < 8 ? kCellChangeCodes : std::clamp(2.5f * m_v + 2.0f, 5.0f, 12.0f); }
    float Value() const { return m_v; }
    void Reset() { m_v = 0; m_n = 0; }
private:
    float m_v = 0;
    uint32_t m_n = 0;
};
// p99 of |cur - prev| over the cells (a histogram of 1/8 codes up to 32 codes)
inline float CellChangeP99(const CellGrid& prev, const CellGrid& cur)
{
    if (!prev.ok() || !cur.ok() || prev.v.size() != cur.v.size()) return 0.f;
    uint32_t hist[257] = {};
    for (size_t i = 0; i < cur.v.size(); ++i) ++hist[std::min<uint32_t>(256u, uint32_t(std::fabs(cur.v[i] - prev.v[i]) * 8.f))];
    const size_t want = cur.v.size() * 99 / 100;
    size_t acc = 0;
    for (uint32_t b = 0; b <= 256; ++b) { acc += hist[b]; if (acc >= want) return float(b) / 8.f; }
    return 32.f;
}

// a cell grid averaged over a x a blocks
struct AggGrid {
    int w = 0, h = 0;
    std::vector<float> v;
    AggGrid(const CellGrid& g, int a) : w(int(g.w) / a), h(int(g.h) / a), v(size_t(w) * h)
    {
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            float s = 0;
            for (int j = 0; j < a; ++j) { const float* row = &g.v[size_t(y * a + j) * g.w + size_t(x * a)]; for (int i = 0; i < a; ++i) s += row[i]; }
            v[size_t(y) * w + x] = s / float(a * a);
        }
    }
};
// best integer shift of `cur` against `prev` (aggregated grids), within +-r around (cx, cy), measured over the part
// [x0, x1) x [y0, y1) of the picture (fractions)
inline void SearchShift(const AggGrid& P, const AggGrid& C, int r, int cx, int cy, int& bx, int& by,
                        float x0 = 0.f, float x1 = 1.f, float y0 = 0.f, float y1 = 1.f)
{
    const int w = C.w, h = C.h;
    const int m = r + std::max(std::abs(cx), std::abs(cy)) + 1;
    const int xs = std::max(m, int(x0 * w)), xe = std::min(w - m, int(x1 * w)), ys = std::max(m, int(y0 * h)), ye = std::min(h - m, int(y1 * h));
    double best = 1e30; bx = cx; by = cy;
    for (int dy = cy - r; dy <= cy + r; ++dy)
        for (int dx = cx - r; dx <= cx + r; ++dx) {
            double e = 0; int n = 0;
            for (int y = ys; y < ye; ++y) for (int x = xs; x < xe; ++x) { e += std::fabs(C.v[size_t(y) * w + x] - P.v[size_t(y - dy) * w + (x - dx)]); ++n; }
            if (n && e / n < best) { best = e / n; bx = dx; by = dy; }
        }
}
inline float SampleOf(const std::vector<float>& v, uint32_t W, uint32_t H, float x, float y)
{
    auto at = [&](int xx, int yy) { xx = std::clamp(xx, 0, int(W) - 1); yy = std::clamp(yy, 0, int(H) - 1); return v[size_t(yy) * W + xx]; };
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y)); const float fx = x - x0, fy = y - y0;
    return at(x0, y0) * (1 - fx) * (1 - fy) + at(x0 + 1, y0) * fx * (1 - fy) + at(x0, y0 + 1) * (1 - fx) * fy + at(x0 + 1, y0 + 1) * fx * fy;
}
// thr: the cell change threshold in codes (NoiseFloor::Threshold)
inline RegionFrame CompareRegions(const CellGrid& prev, const CellGrid& cur, float thr = kCellChangeCodes)
{
    RegionFrame r;
    if (!prev.ok() || !cur.ok() || prev.w != cur.w || prev.h != cur.h || cur.w < 32 || cur.h < 18) return r;
    // camera shift: coarse (4x4 aggregated, +-8 = +-32 cells), then 2x2 (+-2), then the cell grid (+-1) with a parabola
    int bx = 0, by = 0;
    const AggGrid P4(prev, 4), C4(cur, 4), P2(prev, 2), C2(cur, 2);
    SearchShift(P4, C4, 8, 0, 0, bx, by);
    SearchShift(P2, C2, 2, bx * 2, by * 2, bx, by);
    const int W = int(cur.w), H = int(cur.h);
    // zoom: the left and right thirds move apart in x while the top and bottom thirds move apart in y (both the same way:
    // in or out). A multi-plane pan moves bands at different speeds along one axis only, so it is not a zoom.
    const int mg = std::max(std::abs(bx), std::abs(by)) + 3;
    // the shift over a part of the picture [x0, x1) x [y0, y1) (cells), sub-cell
    auto errIn = [&](float dx, float dy, int x0, int x1, int y0, int y1) {
        double e = 0; int n = 0;
        for (int y = std::max(mg, y0); y < std::min(H - mg, y1); y += 3) for (int x = std::max(mg, x0); x < std::min(W - mg, x1); x += 3) { e += std::fabs(cur.at(x, y) - prev.sample(x - dx, y - dy)); ++n; }
        return n ? e / n : 1e30;
    };
    auto err = [&](float dx, float dy) { return errIn(dx, dy, 0, W, 0, H); };
    auto subShift = [&](int x0, int x1, int y0, int y1, float cx, float cy, float& ox, float& oy) {
        double be = 1e30; ox = cx; oy = cy;
        for (int j = -1; j <= 1; ++j) for (int i = -1; i <= 1; ++i) { const double e = errIn(cx + i, cy + j, x0, x1, y0, y1); if (e < be) { be = e; ox = cx + i; oy = cy + j; } }
        const double ex0 = errIn(ox - 1, oy, x0, x1, y0, y1), ex1 = errIn(ox + 1, oy, x0, x1, y0, y1), ey0 = errIn(ox, oy - 1, x0, x1, y0, y1), ey1 = errIn(ox, oy + 1, x0, x1, y0, y1);
        const double dxx = ex0 - 2 * be + ex1, dyy = ey0 - 2 * be + ey1;
        if (dxx > 1e-9) ox += float(std::clamp(0.5 * (ex0 - ex1) / dxx, -0.5, 0.5));
        if (dyy > 1e-9) oy += float(std::clamp(0.5 * (ey0 - ey1) / dyy, -0.5, 0.5));
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
    // the scale (research P7: a similarity camera, so push-ins get repair instead of a gate): the left and right thirds'
    // x shifts and the top and bottom thirds' y shifts spread apart by (scale - 1) x their distance (2/3 of the picture)
    {
        float lx, ly, rx, ry, tx, ty, ux, uy;
        subShift(0, W / 3, 0, H, fx, fy, lx, ly); subShift(2 * W / 3, W, 0, H, fx, fy, rx, ry);
        subShift(0, W, 0, H / 3, fx, fy, tx, ty); subShift(0, W, 2 * H / 3, H, fx, fy, ux, uy);
        const float sx = (rx - lx) / (2.f * W / 3.f), sy = (uy - ty) / (2.f * H / 3.f);
        // a zoom moves both spreads the same way; a multi-plane pan moves one axis only (no scale)
        if (sx * sy > 0.f) {
            r.scale = 1.f + 0.5f * (sx + sy);
            fx = 0.5f * (lx + rx); fy = 0.5f * (ty + uy);   // the shift at the centre
        }
        r.zoom = std::fabs(r.scale - 1.f) > kZoomGate;
    }
    const bool scaled = std::fabs(r.scale - 1.f) > 0.001f;
    const bool slid = std::fabs(fx) + std::fabs(fy) >= 0.25f || scaled;
    const float cxm = 0.5f * float(W - 1), cym = 0.5f * float(H - 1);
    r.dx = fx; r.dy = fy; r.cells = uint32_t(W) * H; r.changed.assign(r.cells, 0);
    // gain: the median signed change (every third cell)
    {
        std::vector<float> d; d.reserve(r.cells / 3 + 1);
        for (size_t i = 0; i < r.cells; i += 3) d.push_back(cur.v[i] - prev.v[i]);
        std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
        r.gain = d[d.size() / 2];
    }
    const bool sub = cur.hasSub(), chroma = cur.hasChroma() && prev.hasChroma();
    std::vector<uint8_t> raw(r.cells, 0);
    uint32_t rawChanged = 0, rawPlain = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const size_t i = size_t(y) * W + x;
            const float c = cur.v[i];
            const float d0 = std::fabs(c - prev.v[i]);
            rawPlain += d0 > thr;
            // unmoved: the cell mean, its sub-cells and its colour all match the previous frame
            bool unmovedOk = d0 <= thr && (!sub || cur.sub[i] <= thr);
            if (unmovedOk && chroma) unmovedOk = std::fabs(cur.cb[i] - prev.cb[i]) + std::fabs(cur.cr[i] - prev.cr[i]) <= kChromaChangeCodes;
            bool slidOk = false;
            // where this cell was in the previous frame (the camera: scale about the centre, then the shift)
            const float px = cxm + (float(x) - cxm - fx) / r.scale, py = cym + (float(y) - cym - fy) / r.scale;
            // cells the camera brings in from outside the picture cannot be compared: unchanged unless they differ plainly
            const bool inside = px >= 0 && px <= W - 1 && py >= 0 && py <= H - 1;
            if (!unmovedOk && slid && inside) {
                slidOk = std::fabs(c - prev.sample(px, py)) <= thr;
                if (slidOk && chroma)
                    slidOk = std::fabs(cur.cb[i] - SampleOf(prev.cb, prev.w, prev.h, px, py)) + std::fabs(cur.cr[i] - SampleOf(prev.cr, prev.w, prev.h, px, py)) <= kChromaChangeCodes;
            }
            const bool ch = !unmovedOk && !slidOk;
            raw[i] = ch; rawChanged += ch;
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
// s inside its mask (PushCoreRegion.h); the graph then interpolates the repaired source as usual.
//
// Nested cadences. The decision is per cell, from both ends: a cell belongs to the group (d0, d1) when it did not change
// at s, changed last at d0 (within the lookahead back) and changes next at d1 (within the lookahead ahead). So every
// layer gets its own span, however the layers nest: a background panning on ones never joins a group (it changes on
// every frame), a body on twos is the group (s-1, s+1), a limb on threes on that body is the group (s-2, s+1) on the
// frames where it is held, and two characters on twos out of phase are two groups on alternate frames. Cells of the
// same (d0, d1) share one span-stream frame however many parts they are.
//
// Per group:
//  - anchor test: each cell must still match its drawing at d0 (camera-compensated), not just the frame before, so a
//    slow fade or drift that stays under the threshold from frame to frame does not pass;
//  - the mask is the sweep of the group's cells along its own motion d0 -> d1 (block match on the cell grids), dilated;
//  - other layers inside the mask: a cell that changes once in (d0, s] and then stays until d1 is a substitution (a
//    mouth, a blink, a hand pose, a subtitle that appears): the pair starts at that frame m instead (pair (m, d1) at the
//    same phase: the held body in m is the body of d0, the new drawing in m is kept and moves with it). A cell that
//    changes in (s, d1), or more than once, belongs to a layer on another cadence (snow, rain, sparkles on ones,
//    another character): it keeps the frame's own picture (carved out of the mask);
//  - a part that moves with the camera gets the camera residual c(s) - lerp(c(d0), c(d1), t) as a shift, so an eased
//    pan or a shake does not make it float against the real background.
// Gates: no cut, fade, flash (median change) or zoom in (d0, d1]; a span longer than the cadence cap is a hold (only its
// last interval moves, as the whole-frame re-timer does); at least kMinGroupShare of the cells.
struct RepairGroup {
    uint64_t d0 = 0, d1 = 0;
    uint64_t pairFrom = 0;                // the pair's first frame: d0, or the substitution frame m (see above)
    float t = 0.5f;
    uint32_t w = 0, h = 0, cells = 0;
    float vx = 0, vy = 0;                 // the group's motion d0 -> d1 in cells (block match on the cell grids)
    float shiftX = 0, shiftY = 0;         // camera residual in cells (a part that moves with the camera), else 0
    float residual = 0;                   // mean |s - d0| of the group's cells (codes): near 0 for a drawing held exactly
    float stepD0 = 0, stepD1 = 0;         // median change of the group's cells at d0 and at d1 (codes): the drawing steps
    float thr = kCellChangeCodes;         // the change threshold the plan used
    uint32_t carved = 0, substituted = 0; // mask cells given back to the frame / carried from pairFrom
    std::vector<uint8_t> mask;            // per cell 0 / 255
};
inline constexpr double kMinGroupShare = 0.0008;   // about 100 cells: a small part (a hand) is still a part
inline constexpr double kFadeShare = 0.6;
inline constexpr uint32_t kCadenceCap = 4;   // the longest drawing span that still moves over its whole length
inline constexpr uint32_t kMinSubstitution = 12;  // cells: a redrawn blob (a mouth, an eye) smaller than this stays in the body's span
inline constexpr uint32_t kLayerCells = 300;      // cells: a key this large inside an object is a second layer in contact
// per source frame at most kMaxGroups parts on at most kMaxPairs span-stream pairs, the largest first: each part costs
// one generated frame of the span stream and each pair one pair analysis (the budget; the largest parts carry the motion)
#ifndef PUSHMPC_HELD_MAX_GROUPS
#define PUSHMPC_HELD_MAX_GROUPS 3
#endif
inline constexpr size_t kMaxGroups = PUSHMPC_HELD_MAX_GROUPS, kMaxPairs = 2;
// Cadence history per cell (research P2): its last four change frames, so a part's cadence (ones, twos, threes, an
// irregular 2-2-3) is measured over the last 24 frames, not guessed from one span. Fed with every classified frame.
class CellCadence {
public:
    void Reset() { m_last.clear(); }
    void Push(uint64_t seq, const RegionFrame& r)
    {
        if (!r.valid) return;
        if (m_last.size() != r.changed.size() * 4) m_last.assign(r.changed.size() * 4, 0);
        for (size_t i = 0; i < r.changed.size(); ++i)
            if (r.changed[i]) { uint64_t* l = &m_last[i * 4]; l[3] = l[2]; l[2] = l[1]; l[1] = l[0]; l[0] = seq + 1; }   // 0 = none
    }
    // the cell's cadence around frame s: the median of its last change intervals within 24 frames (0: not known)
    float At(size_t i, uint64_t s) const
    {
        if (i * 4 + 3 >= m_last.size()) return 0.f;
        const uint64_t* l = &m_last[i * 4];
        float iv[3]; int n = 0;
        for (int k = 0; k < 3; ++k) if (l[k] && l[k + 1] && s + 25 > l[k + 1]) iv[n++] = float(l[k] - l[k + 1]);
        if (!n) return 0.f;
        std::sort(iv, iv + n);
        return iv[n / 2];
    }
    // the median cadence of a set of cells (every step-th), 0 when none is known
    template <class Pred> float Median(uint64_t s, size_t n, Pred in, size_t step = 1) const
    {
        std::vector<float> v;
        for (size_t i = 0; i < n; i += step) if (in(i)) { const float c = At(i, s); if (c > 0.f) v.push_back(c); }
        if (v.empty()) return 0.f;
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return v[v.size() / 2];
    }
private:
    std::vector<uint64_t> m_last;
};

// Ownership (research P3 / P8): every cell belongs to at most one part. Parts on the shorter span come first (a limb on
// threes over a body on twos: the nested, faster-changing part wins where they overlap), so the masks are disjoint and
// one composite pass lays them all in, order-independent.
inline void AssignOwners(std::vector<RepairGroup>& groups)
{
    if (groups.size() < 2) return;
    std::vector<size_t> order(groups.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const uint64_t la = groups[a].d1 - groups[a].d0, lb = groups[b].d1 - groups[b].d0;
        return la != lb ? la < lb : groups[a].cells < groups[b].cells;
    });
    std::vector<uint8_t> taken(groups[0].mask.size(), 0);
    for (size_t o : order) {
        auto& m = groups[o].mask;
        if (m.size() != taken.size()) continue;
        for (size_t i = 0; i < m.size(); ++i) { if (!m[i]) continue; if (taken[i]) m[i] = 0; else taken[i] = 1; }
    }
}

template <class FrameAt, class GridAt>
inline std::vector<RepairGroup> PlanRepairs(uint64_t s, FrameAt frameAt, GridAt gridAt, int lookahead, PhaseShape shape = PhaseShape::Linear,
                                            float thr = kCellChangeCodes, const CellCadence* hist = nullptr)
{
    std::vector<RepairGroup> out;
    const RegionFrame* fs = frameAt(s);
    if (!fs || !fs->valid || fs->cut) return out;
    const size_t n = fs->changed.size();
    const CellGrid* gs = gridAt(s);
    if (!gs || !gs->ok() || gs->v.size() != n) return out;
    const uint32_t W = gs->w, H = gs->h;
    // the frames around s: back[k] = the change AT frame s - 1 - k, ahead[k] = AT frame s + 1 + k
    std::vector<const RegionFrame*> back, ahead;
    for (int k = 1; k <= lookahead; ++k) { if (s < uint64_t(k)) break; const RegionFrame* f = frameAt(s - uint64_t(k)); if (!f || !f->valid || f->changed.size() != n) break; back.push_back(f); }
    for (int k = 1; k <= lookahead; ++k) { const RegionFrame* f = frameAt(s + uint64_t(k)); if (!f || !f->valid || f->changed.size() != n) break; ahead.push_back(f); }
    if (back.empty() || ahead.empty()) return out;
    auto bad = [](const RegionFrame* f) { return f->cut || f->changedShare() > kFadeShare || std::fabs(f->gain) >= kFadeGain || f->zoom; };
    if (bad(fs)) return out;
    // the change AT frame j, for j in [s - back.size(), s + ahead.size()] (j = s - back.size() is the oldest change known)
    auto rfAt = [&](uint64_t j) -> const RegionFrame* {
        if (j == s) return fs;
        if (j < s) { const uint64_t k = s - j; return k <= back.size() ? back[size_t(k - 1)] : nullptr; }
        const uint64_t k = j - s; return k <= ahead.size() ? ahead[size_t(k - 1)] : nullptr;
    };
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
    // Objects, not cells (the research critique C1: per-cell keys split one drawing into several spans at different
    // phases, because edge and shading cells cross the threshold at different frames). 1. a 3x3 majority filter on the
    // key map; 2. 8-connected objects of keyed cells, each taking its cells' majority key, unless another key holds
    // kLayerCells cells and a quarter of the object (a second layer in contact, kept apart). Objects with the same key
    // share one span-stream frame.
    {
        std::vector<std::pair<int, int>> k2 = key;
        for (int y = 0; y < int(H); ++y)
            for (int x = 0; x < int(W); ++x) {
                const size_t i = size_t(y) * W + x;
                if (!key[i].first) continue;
                std::pair<int, int> cand[9]; int cnt[9] = {}, nk = 0;
                for (int j = -1; j <= 1; ++j) for (int q = -1; q <= 1; ++q) {
                    const int xx = x + q, yy = y + j;
                    if (xx < 0 || yy < 0 || xx >= int(W) || yy >= int(H)) continue;
                    const auto& kk = key[size_t(yy) * W + xx];
                    if (!kk.first) continue;
                    int f = 0; while (f < nk && cand[f] != kk) ++f;
                    if (f == nk) cand[nk++] = kk;
                    ++cnt[f];
                }
                int best = 0; for (int f = 1; f < nk; ++f) if (cnt[f] > cnt[best]) best = f;
                if (cnt[best] > 4) k2[i] = cand[best];
            }
        std::vector<int> label(n, -1);
        std::vector<uint32_t> stack, members;
        for (size_t i0 = 0; i0 < n; ++i0) {
            if (!k2[i0].first || label[i0] >= 0) continue;
            members.clear(); stack.assign(1, uint32_t(i0)); label[i0] = 1;
            while (!stack.empty()) {
                const uint32_t c = stack.back(); stack.pop_back(); members.push_back(c);
                const int x = int(c % W), y = int(c / W);
                for (int j = -1; j <= 1; ++j) for (int q = -1; q <= 1; ++q) {
                    const int xx = x + q, yy = y + j;
                    if (xx < 0 || yy < 0 || xx >= int(W) || yy >= int(H)) continue;
                    const size_t d = size_t(yy) * W + xx;
                    if (k2[d].first && label[d] < 0) { label[d] = 1; stack.push_back(uint32_t(d)); }
                }
            }
            std::vector<std::pair<std::pair<int, int>, uint32_t>> votes;
            for (uint32_t c : members) { bool f = false; for (auto& v : votes) if (v.first == k2[c]) { ++v.second; f = true; break; } if (!f) votes.push_back({k2[c], 1u}); }
            auto major = std::max_element(votes.begin(), votes.end(), [](const auto& a, const auto& b) { return a.second < b.second; })->first;
            const uint32_t keepApart = std::max<uint32_t>(kLayerCells, uint32_t(members.size() / 4));
            // with the cadence history a second layer is kept apart only when its own cadence differs (two characters in
            // contact on twos and threes); the same cadence is the same object split by threshold flicker
            float majorCad = 0.f;
            if (hist) {
                std::vector<float> v;
                for (size_t q = 0; q < members.size(); q += std::max<size_t>(1, members.size() / 400)) if (k2[members[q]] == major) { const float c = hist->At(members[q], s); if (c > 0) v.push_back(c); }
                if (!v.empty()) { std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end()); majorCad = v[v.size() / 2]; }
            }
            for (auto& v : votes) {
                if (v.first == major || v.second < keepApart) continue;
                if (hist && majorCad > 0.f) {
                    std::vector<float> c;
                    for (size_t q = 0; q < members.size(); q += std::max<size_t>(1, members.size() / 400)) if (k2[members[q]] == v.first) { const float cc = hist->At(members[q], s); if (cc > 0) c.push_back(cc); }
                    if (!c.empty()) { std::nth_element(c.begin(), c.begin() + c.size() / 2, c.end()); if (std::fabs(c[c.size() / 2] - majorCad) < 0.75f) v.second = 0; }
                }
            }
            for (uint32_t c : members) {
                uint32_t mine = 0; for (auto& v : votes) if (v.first == k2[c]) mine = v.second;
                key[c] = mine >= keepApart ? k2[c] : major;
            }
        }
    }
    std::vector<std::pair<std::pair<int, int>, uint32_t>> groups;
    for (size_t i = 0; i < n; ++i) {
        if (!key[i].first) continue;
        bool found = false;
        for (auto& g : groups) if (g.first == key[i]) { ++g.second; found = true; break; }
        if (!found) groups.push_back({key[i], 1u});
    }
    const uint32_t minCells = uint32_t(kMinGroupShare * double(n)) + 1;
    std::sort(groups.begin(), groups.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    size_t pairsUsed = 0;
    std::vector<std::pair<uint64_t, uint64_t>> pairList;
    for (const auto& g : groups) {
        if (out.size() >= kMaxGroups) break;
        const int k0 = g.first.first, k1 = g.first.second;
        if (g.second < minCells) continue;
        RepairGroup r;
        r.d0 = s - uint64_t(k0); r.d1 = s + uint64_t(k1);
        const uint32_t span = uint32_t(k0 + k1);
        // a span longer than the part's cadence (measured, research P2: its median interval + 1 for 2-2-3 timing; else
        // kCadenceCap) is a hold: only its last `cap` frames move (d0 moves to d1 - cap)
        uint32_t cap = kCadenceCap;
        if (hist) {
            const auto gk = g.first;
            const float c = hist->Median(s, n, [&](size_t i) { return key[i] == gk; }, 3);
            if (c >= 1.5f) cap = std::clamp<uint32_t>(uint32_t(std::lround(c)) + 1, 2u, kCadenceCap);
        }
        if (span > cap) { if (uint64_t(k1) >= cap) continue; r.d0 = r.d1 - cap; }
        // gates over (d0, d1]
        bool ok = true;
        for (uint64_t j = r.d0 + 1; j <= r.d1 && ok; ++j) { const RegionFrame* f = rfAt(j); ok = f && !bad(f); }
        if (!ok) continue;
        r.t = ShapePhase(float(double(s - r.d0) / double(r.d1 - r.d0)), shape);
        if (!(r.t > 0.f && r.t < 1.f)) continue;
        r.w = W; r.h = H;
        // the camera from d0: c(s) and c(d1) (sums of the per-frame shifts)
        float csx = 0, csy = 0, c1x = 0, c1y = 0, cS = 1.f;   // cS: the camera's scale from d0 to s (push-ins)
        for (uint64_t j = r.d0 + 1; j <= r.d1; ++j) { const RegionFrame* f = rfAt(j); if (j <= s) { csx += f->dx; csy += f->dy; cS *= f->scale; } c1x += f->dx; c1y += f->dy; }
        const float cxm = 0.5f * float(W - 1), cym = 0.5f * float(H - 1);
        const CellGrid* g0 = gridAt(r.d0); const CellGrid* g1 = gridAt(r.d1);
        if (!g0 || !g1 || !g0->ok() || !g1->ok() || g0->w != W || g1->w != W || g0->h != H || g1->h != H) continue;
        // the group's cells, with the anchor test against d0 (unmoved or with the camera since d0)
        const bool camMoved = std::fabs(csx) + std::fabs(csy) >= 0.25f || std::fabs(cS - 1.f) > 0.001f;
        std::vector<uint8_t> cells(n, 0);
        int x0 = int(W), y0 = int(H), x1 = -1, y1 = -1;
        double resSum = 0;
        for (size_t i = 0; i < n; ++i) {
            if (key[i] != g.first) continue;
            const int x = int(i % W), y = int(i / W);
            float e = std::fabs(gs->v[i] - g0->v[i]);
            if (camMoved) e = std::min(e, std::fabs(gs->v[i] - g0->sample(cxm + (float(x) - cxm - csx) / cS, cym + (float(y) - cym - csy) / cS)));
            if (e > thr) continue;
            cells[i] = 1; ++r.cells; resSum += e;
            x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
        }
        if (r.cells < minCells) continue;
        r.residual = float(resSum / r.cells);
        // the group's motion d0 -> d1: block match of its cells on the two cell grids (+-16 cells): every second offset on
        // a sparse sample of the cells, then +-1 around the best on a denser one (about 1/30 of a full search)
        {
            std::vector<uint32_t> sparse, dense;
            const int area = (x1 - x0 + 1) * (y1 - y0 + 1);
            const int st = std::max(2, int(std::sqrt(double(area) / 1500.0))), sd = std::max(1, st / 2);
            for (int y = y0; y <= y1; y += sd) for (int x = x0; x <= x1; x += sd) {
                const uint32_t c = uint32_t(y) * W + uint32_t(x);
                if (!cells[c]) continue;
                dense.push_back(c);
                if ((y - y0) % st == 0 && (x - x0) % st == 0) sparse.push_back(c);
            }
            if (sparse.size() < 16) sparse = dense;
            auto cost = [&](const std::vector<uint32_t>& set, int dx, int dy) {
                double e = 0;
                for (uint32_t c : set) e += std::fabs(g1->at(int(c % W) + dx, int(c / W) + dy) - g0->v[c]);
                return set.empty() ? 1e30 : e / double(set.size());
            };
            double best = 1e30; int bx = 0, by = 0;
            for (int dy = -16; dy <= 16; dy += 2) for (int dx = -16; dx <= 16; dx += 2) { const double e = cost(sparse, dx, dy); if (e < best) { best = e; bx = dx; by = dy; } }
            const int cx = bx, cy = by; best = 1e30;
            for (int dy = cy - 1; dy <= cy + 1; ++dy) for (int dx = cx - 1; dx <= cx + 1; ++dx) { const double e = cost(dense, dx, dy); if (e < best) { best = e; bx = dx; by = dy; } }
            r.vx = float(bx); r.vy = float(by);
        }
        // a part that moves with the camera: the camera residual (the span stream places it at lerp(c(d0), c(d1), t))
        if (std::fabs(c1x) + std::fabs(c1y) >= 0.5f && std::fabs(r.vx - c1x) + std::fabs(r.vy - c1y) <= 1.5f) {
            r.shiftX = csx - r.t * c1x; r.shiftY = csy - r.t * c1y;
        }
        // the sweep: every group cell along +-(v) (the footprints at d0 and d1 and the positions between), then a dilation
        // of 2 cells (the feather plus the camera's sub-cell error stay outside the part); only around the group's box
        const int steps = int(std::ceil(std::max(std::fabs(r.vx), std::fabs(r.vy)))) + 1;
        const int reach = steps + 3;
        const int bx0 = std::max(0, x0 - reach), by0 = std::max(0, y0 - reach), bx1 = std::min(int(W) - 1, x1 + reach), by1 = std::min(int(H) - 1, y1 + reach);
        std::vector<uint8_t> sweep(n, 0);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                if (!cells[size_t(y) * W + x]) continue;
                for (int k = -steps; k <= steps; ++k) {
                    const int xx = x + int(std::lround(r.vx * k / float(steps))), yy = y + int(std::lround(r.vy * k / float(steps)));
                    if (xx >= 0 && yy >= 0 && xx < int(W) && yy < int(H)) sweep[size_t(yy) * W + xx] = 1;
                }
            }
        r.mask.assign(n, 0);
        const int dil = 2;
        // Ownership-aware carving (critique C5): inside the sweep (the part's old and new footprints and the way between)
        // nothing is carved; in the dilation band only changes that motion does not explain are (sparkles, flashes, a
        // light pulse): a cell that matches d0 moved by the camera is a background scrolling on ones, which the span
        // stream interpolates correctly (carving it would leave the part's d0 outline behind: a ghost edge).
        std::vector<uint32_t> subCand; std::vector<uint64_t> subM;
        for (int y = by0; y <= by1; ++y)
            for (int x = bx0; x <= bx1; ++x) {
                bool on = false;
                for (int j = -dil; j <= dil && !on; ++j) for (int i = -dil; i <= dil && !on; ++i) {
                    const int xx = x + i, yy = y + j;
                    on = xx >= 0 && yy >= 0 && xx < int(W) && yy < int(H) && sweep[size_t(yy) * W + xx];
                }
                if (!on) continue;
                const size_t c = size_t(y) * W + x;
                if (cells[c]) { r.mask[c] = 255; continue; }
                uint32_t nc = 0; uint64_t m = 0;
                for (uint64_t j = r.d0 + 1; j < r.d1; ++j) if (rfAt(j)->changed[c]) { ++nc; m = j; }
                if (nc == 0) { r.mask[c] = 255; continue; }
                if (sweep[c]) {
                    r.mask[c] = 255;
                    if (nc == 1 && m <= s) { subCand.push_back(uint32_t(c)); subM.push_back(m); }   // a redraw inside the part (decided below)
                    continue;
                }
                const float moved = std::fabs(gs->v[c] - g0->sample(cxm + (float(x) - cxm - csx) / cS, cym + (float(y) - cym - csy) / cS));
                if (moved <= thr) { r.mask[c] = 255; continue; }
                ++r.carved;
            }
        r.pairFrom = r.d0;
        // Substitution (critique C6): only a compact blob inside the part that was redrawn once at m and then held (a
        // mouth, an eye, a hand pose) takes the pair (m, d1), as its own sub-part at the same phase; the body keeps (d0, d1)
        RepairGroup sub;
        if (subCand.size() >= kMinSubstitution) {
            std::vector<uint8_t> isCand(n, 0);
            for (uint32_t c : subCand) isCand[c] = 1;
            std::vector<uint32_t> best, cur, st;
            for (uint32_t c0 : subCand) {
                if (isCand[c0] != 1) continue;
                cur.clear(); st.assign(1, c0); isCand[c0] = 2;
                while (!st.empty()) {
                    const uint32_t c = st.back(); st.pop_back(); cur.push_back(c);
                    const int x = int(c % W), y = int(c / W);
                    for (int j = -1; j <= 1; ++j) for (int q = -1; q <= 1; ++q) {
                        const int xx = x + q, yy = y + j;
                        if (xx < 0 || yy < 0 || xx >= int(W) || yy >= int(H)) continue;
                        const size_t d = size_t(yy) * W + xx;
                        if (isCand[d] == 1) { isCand[d] = 2; st.push_back(uint32_t(d)); }
                    }
                }
                if (cur.size() > best.size()) best = cur;
            }
            if (best.size() >= kMinSubstitution && best.size() * 2 <= r.cells) {
                uint64_t m = 0;
                for (size_t q = 0; q < subCand.size(); ++q) for (uint32_t c : best) if (subCand[q] == c) m = std::max(m, subM[q]);
                sub = r; sub.pairFrom = m; sub.mask.assign(n, 0); sub.cells = uint32_t(best.size()); sub.carved = 0;
                for (uint32_t c : best) {
                    const int x = int(c % W), y = int(c / W);
                    for (int j = -1; j <= 1; ++j) for (int q = -1; q <= 1; ++q) {
                        const int xx = x + q, yy = y + j;
                        if (xx >= 0 && yy >= 0 && xx < int(W) && yy < int(H)) { sub.mask[size_t(yy) * W + xx] = 255; r.mask[size_t(yy) * W + xx] = 0; }
                    }
                }
                r.substituted = sub.substituted = uint32_t(best.size());
            }
        }
        // the step signature (critique C4): the part's cells change by a drawing step at d0 and at d1
        {
            const CellGrid* gp0 = r.d0 > 0 ? gridAt(r.d0 - 1) : nullptr; const CellGrid* gp1 = gridAt(r.d1 - 1);
            std::vector<float> a0, a1;
            const size_t stride = std::max<size_t>(1, r.cells / 2000);
            size_t q = 0;
            for (size_t i = 0; i < n; ++i) if (cells[i] && (q++ % stride) == 0) {
                if (gp0 && gp0->ok() && gp0->v.size() == n) a0.push_back(std::fabs(g0->v[i] - gp0->v[i]));
                if (gp1 && gp1->ok() && gp1->v.size() == n) a1.push_back(std::fabs(g1->v[i] - gp1->v[i]));
            }
            auto med = [](std::vector<float>& v) { if (v.empty()) return 0.f; std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end()); return v[v.size() / 2]; };
            r.stepD0 = med(a0); r.stepD1 = med(a1); r.thr = thr;
            if (sub.cells) { sub.stepD0 = r.stepD0; sub.stepD1 = r.stepD1; sub.thr = thr; sub.residual = r.residual; }
        }        const std::pair<uint64_t, uint64_t> pr{r.pairFrom, r.d1};
        if (std::find(pairList.begin(), pairList.end(), pr) == pairList.end()) {
            if (pairsUsed >= kMaxPairs) continue;
            pairList.push_back(pr); ++pairsUsed;
        }
        out.push_back(std::move(r));
        if (sub.cells && out.size() < kMaxGroups) {
            const std::pair<uint64_t, uint64_t> ps{sub.pairFrom, sub.d1};
            if (std::find(pairList.begin(), pairList.end(), ps) != pairList.end() || pairsUsed < kMaxPairs) {
                if (std::find(pairList.begin(), pairList.end(), ps) == pairList.end()) { pairList.push_back(ps); ++pairsUsed; }
                out.push_back(std::move(sub));
            }
        }
    }
    AssignOwners(out);
    return out;
}

// Animation evidence for the live-action gate. Anime repeats a drawing to the noise floor: a frame is evidence when it
// is wholly held, or when a repair group's cells match their drawing at d0 almost exactly (residual under
// kExactResidual over kExactShare of the picture). Live action has neither: its "held" regions are near-still, not
// repeated (measured offline: 0.1 % of the partly changed frames of a live-action episode, 10 to 20 % on TV anime and
// films, plus their wholly held frames: 1.6 % live action, 11 to 44 % anime). The gate
// opens partial repairs (a wholly held frame is always repaired, as the whole-frame re-timer always re-times it).
inline constexpr float kExactResidual = 1.0f;
inline constexpr double kExactShare = 0.005;   // of the cells (650 at 1080p): a part, not a few quiet cells
// a drawing held exactly between two drawing steps (the step signature): live action moves continuously, with no
// exact hold between two large steps; near-still live-action regions are exact but have no steps
inline bool IsExactRepeat(const RepairGroup& g)
{
    return g.residual < kExactResidual && double(g.cells) >= kExactShare * double(g.w) * double(g.h) && std::min(g.stepD0, g.stepD1) >= 3.f * g.thr;
}
class CadenceEvidence {
public:
    explicit CadenceEvidence(size_t window = 240) : m_window(window) {}
    void Push(bool evidence) { m_v.push_back(evidence); m_n += evidence; if (m_v.size() > m_window) { m_n -= m_v.front(); m_v.pop_front(); } }
    void Reset() { m_v.clear(); m_n = 0; }
    size_t Size() const { return m_v.size(); }
    double Ratio() const { return m_v.empty() ? 0.0 : double(m_n) / double(m_v.size()); }
    // open once 48 frames (2 s) were seen with at least kOpen evidence; closes below kClose (hysteresis)
    static constexpr double kOpen = 0.10, kClose = 0.06;
    bool Animation()
    {
        if (m_v.size() < 48) return m_open = false;
        const double r = Ratio();
        m_open = m_open ? r >= kClose : r >= kOpen;
        return m_open;
    }
private:
    size_t m_window;
    std::deque<bool> m_v;
    size_t m_n = 0;
    bool m_open = false;
};

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
