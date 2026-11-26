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
inline bool IsHeld(const DiffStats& d)
{
    if (!d.valid || d.cells == 0) return false;
    return d.over[2] <= std::max<uint32_t>(kMaxOver8, d.cells / 20000) && d.meanCodes() < kMaxMean;
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
