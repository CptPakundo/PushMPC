// SPDX-License-Identifier: GPL-3.0-or-later
// pushmpc.Enhance, portable GPU engine: what every output frame shows, with duplicate removal (PushFPS portable 0.3.0's
// Dedup: a frame that repeats the last different one is not a new picture). Pure C++, no GPU: portable_test checks it.
//
// Anime is drawn "on twos" or "on threes": a 24 fps file shows each drawing for 2 or 3 frames. Interpolating each pair
// of source frames then moves the picture in one step and holds it in the next (judder at every multiplier). With
// Dedup the outputs are placed between the distinct pictures instead: an output at time tau (in source frames) between
// distinct frames d0 <= tau < d1 shows the generated frame at phase (tau - d0) / (d1 - d0), so a drawing held for two
// frames moves smoothly over both. That needs the next distinct frame before the outputs of a duplicate are known: the
// engine looks `lookahead` source frames ahead (2 covers twos and threes). A run of repeats longer than the lookahead
// is a still picture (a pause, a held shot): it is shown as it is, and only its last interval moves to the next one.
// Without Dedup every frame is distinct and the rule is plain frame interpolation: output tau between s and s + 1
// shows the generated frame at phase tau - s.
//
// Output n = s k + j (j = 0: source frame s's own place; j > 0: between s and s + 1), as in schedule.h. The outputs of
// position p (the generated frames between p - 1 and p, and p's own) are settled when source frame p + lookahead has
// been pushed, or at the end of the clip.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <vector>

namespace pmvs::gpu {

struct Shot {
    enum Kind { Real, Generated, Hold } kind = Real;
    int64_t src = 0;           // Real / Hold: the source frame shown as it is
    int64_t d0 = 0, d1 = 0;    // Generated: the two distinct frames
    float t = 0;               // Generated: the phase between them
};
struct Settled { int64_t n = 0; Shot shot; };

class Cadence {
public:
    void Configure(int k, int lookahead, int64_t srcFrames)
    {
        m_k = std::max(1, k); m_look = std::max(0, lookahead); m_frames = srcFrames;
        Reset();
    }
    int Lookahead() const { return m_look; }
    // the stream ends after the newest frame (when the length was not known): the next Settle gives the rest
    void Finish() { if (m_newest >= 0) m_frames = m_newest + 1; }
    // a seek or a gap: nothing before the next pushed frame is known
    void Reset() { m_distinct.clear(); m_first = -1; m_next = -1; m_newest = -1; m_lastDistinct = -1; }
    // the next source frame in order (after a Reset, any frame); dup: it repeats the last distinct frame
    void Push(int64_t s, bool dup)
    {
        if (m_first < 0 || s != m_newest + 1) { Reset(); m_first = s; m_next = s; dup = false; }
        m_newest = s;
        if (!dup) { m_distinct.push_back(s); m_lastDistinct = s; }
    }
    int64_t LastDistinct() const { return m_lastDistinct; }
    bool IsDistinct(int64_t s) const { return std::binary_search(m_distinct.begin(), m_distinct.end(), s); }
    // the outputs that are settled now, in output order; end: no frame comes after the newest (the clip's end)
    void Settle(std::vector<Settled>& out)
    {
        if (m_first < 0) return;
        const bool end = m_frames > 0 && m_newest >= m_frames - 1;
        const int64_t upto = end ? m_newest : m_newest - m_look;
        for (; m_next <= upto; ++m_next) {
            const int64_t p = m_next;
            if (p > m_first)
                for (int j = 1; j < m_k; ++j) out.push_back({ (p - 1) * m_k + j, ShotAt(double(p - 1) + double(j) / double(m_k), p - 1, j) });
            out.push_back({ p * m_k, ShotAt(double(p), p, 0) });
        }
        // keep the last distinct frame before the next position and everything after it
        while (m_distinct.size() > 1 && m_distinct[1] <= m_next - 1) m_distinct.pop_front();
    }
    // the distinct frames still referenced (the engine keeps their pictures)
    const std::deque<int64_t>& Distinct() const { return m_distinct; }

private:
    // tau: the output's time; base, j: its place (base + j / k)
    Shot ShotAt(double tau, int64_t base, int j) const
    {
        Shot s;
        if (j == 0 && IsDistinct(base)) { s.kind = Shot::Real; s.src = base; return s; }
        // d0: the last distinct frame at or before tau; d1: the first one after tau
        int64_t d0 = -1, d1 = -1;
        for (int64_t d : m_distinct) {
            if (double(d) <= tau) d0 = d;
            else { d1 = d; break; }
        }
        const int64_t nearest = j == 0 ? base : (j * 2 <= m_k ? base : base + 1);
        if (d0 < 0 || d1 < 0) { s.kind = Shot::Hold; s.src = nearest; return s; }
        const int64_t gap = d1 - d0;
        if (gap <= m_look + 1) {
            s.kind = Shot::Generated; s.d0 = d0; s.d1 = d1; s.t = float((tau - double(d0)) / double(gap));
            return s;
        }
        // a still picture longer than the lookahead: only its last interval moves to the next distinct frame
        if (tau > double(d1 - 1)) { s.kind = Shot::Generated; s.d0 = d0; s.d1 = d1; s.t = float(tau - double(d1 - 1)); return s; }
        s.kind = Shot::Hold; s.src = nearest;
        return s;
    }
    int m_k = 1, m_look = 0;
    int64_t m_frames = 0;
    std::deque<int64_t> m_distinct;
    int64_t m_first = -1, m_next = -1, m_newest = -1, m_lastDistinct = -1;
};

} // namespace pmvs::gpu
