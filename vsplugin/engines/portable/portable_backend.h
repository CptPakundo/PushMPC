// pushmpc.Enhance, backend "portable": the engine behind the same Pipeline (schedule.h) the pushcore backend uses.
// Output order is pushcore's: for each submitted source s, the generated frames of the pair (s - 1, s), then s's real
// frame. Generated frame j of the pair has pts(s - 1) + j x source duration / k, so it lands on output (s - 1) k + j.
// Frame: a VSFrame* in the plugin, an int handle in portable_test.
// Dedup (held frames, as mpvnative's cpu_backend.cpp): a frame that repeats the last distinct one (held_dedup.h) is not
// pushed; when the next distinct frame s comes within the lookahead, the outputs of the whole held span (from the last
// distinct frame d0 = s - g) come from the one pair (d0, s), each at its own phase t = tau / g; a longer run is a still
// (shown as it is, only its last interval moves). The Pipeline must request Lookahead() frames ahead.
#pragma once
#include <chrono>
#include <cmath>
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include "held_dedup.h"
#include "portable_engine.h"
#include "schedule.h"

namespace pmvs::portable {

template <class Frame> class PortableBackend : public Backend<Frame> {
public:
    std::function<bool(int64_t s, YuvView& v)> source;              // the source frame s (valid during Submit)
    std::function<bool(YuvOut& o, Frame& f)> alloc;                 // a new output frame
    std::function<void(Frame f)> release;
    std::function<void(int64_t pts, bool generated, Frame f)> tag;  // frame props, after the pixels

    Engine engine;
    Grid grid;   // k and the source duration (Pipeline::GridOf())
    bool dedup = false;         // held frames (set before the first Submit)
    int64_t srcFrames = 0;      // the clip's length: a run held at its end is shown as it is
    uint64_t repeats = 0;
    int Lookahead() const { return dedup ? held::kLookahead : 0; }
    // the stream ends before frame e (an unknown-length source, eof_guard.h): a held run waiting at the end is shown as it is
    bool End(int64_t e, std::string& err)
    {
        if (e > 0 && (srcFrames <= 0 || e < srcFrames)) srcFrames = e;
        if (m_last < e - 1 || m_pending.empty()) return true;
        for (int64_t p : m_pending) if (!Hold(p, err)) return false;
        m_pending.clear();
        return true;
    }

    bool CanSubmit() override { return true; }
    bool Submit(int64_t s, int64_t pts, bool discontinuity, std::string& err) override
    {
        YuvView v;
        if (!source || !source(s, v)) { err = "portable: source frame " + std::to_string(s) + " is not available"; return false; }
        const bool contiguous = !discontinuity && m_last == s - 1;
        int64_t g = 1;   // held frames: source intervals from the last distinct frame to s
        if (dedup) {
            if (!contiguous) { m_pending.clear(); m_still = false; m_haveDistinct = false; }
            held::QuarterLuma(v.data[0], v.stride[0], v.width, v.height, v.bits > 8 ? 2 : 1, v.bits > 8 ? v.bits - 8 : 0, m_curQ);
            if (m_haveDistinct && held::QuarterDuplicate(m_lastQ, m_curQ, v.width, v.height)) {
                m_last = s;
                ++repeats;
                m_pending.push_back(s);
                if (!m_still && int(m_pending.size()) > held::kLookahead) m_still = true;
                if (m_still || (srcFrames > 0 && s >= srcFrames - 1)) {   // a still, or the clip's end: shown as it is
                    for (int64_t p : m_pending) if (!Hold(p, err)) return false;
                    m_pending.clear();
                }
                return true;
            }
            std::swap(m_lastQ, m_curQ);
            m_haveDistinct = true;
            g = m_still ? 1 : int64_t(m_pending.size()) + 1;
            m_pending.clear();
            m_still = false;
        }
        if (!engine.Push(v, contiguous, err)) return false;
        m_last = s;
        if (g > 1 && engine.HavePair()) { if (!Span(s, g, err)) return false; }
        else if (engine.HavePair())
            for (int j = 1; j < grid.k; ++j) {
                Frame f{}; YuvOut o;
                if (!alloc(o, f)) { err = "portable: could not allocate an output frame"; return false; }
                engine.Generate(float(j) / float(grid.k), o);
                const int64_t p = grid.Pts(s - 1) + int64_t(std::llround(double(j) * grid.srcDur100ns / grid.k));
                if (tag) tag(p, true, f);
                m_queue.push_back({ p, true, f });
            }
        Frame f{}; YuvOut o;
        if (!alloc(o, f)) { err = "portable: could not allocate an output frame"; return false; }
        engine.Current(o);
        if (tag) tag(pts, false, f);
        m_queue.push_back({ pts, false, f });
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
        engine.Forget();
        m_last = -1000000;
        m_pending.clear(); m_still = false; m_haveDistinct = false;
        return true;
    }
    void Release(Frame f) override { if (release) release(f); }
    uint64_t NowMs() override
    {
        return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }

private:
    // one output: the pair at phase t (t < 0: the current picture as it is) at pts
    bool Put(int64_t p, float t, std::string& err)
    {
        Frame f{}; YuvOut o;
        if (!alloc(o, f)) { err = "portable: could not allocate an output frame"; return false; }
        if (t < 0.f) engine.Current(o);
        else engine.Generate(t, o);
        if (tag) tag(p, t >= 0.f, f);
        m_queue.push_back({ p, t >= 0.f, f });
        return true;
    }
    int64_t SubPts(int64_t p, int j) const { return grid.Pts(p - 1) + int64_t(std::llround(double(j) * grid.srcDur100ns / grid.k)); }
    // a held position p (a still): its outputs show the last distinct picture (the engine's current frame)
    bool Hold(int64_t p, std::string& err)
    {
        for (int j = 1; j < grid.k; ++j) if (!Put(SubPts(p, j), -1.f, err)) return false;
        return Put(grid.Pts(p), -1.f, err);
    }
    // the held span d0 = s - g .. s from the pair (d0, s): position p's outputs at tau = (p - 1 - d0) + j / k, t = tau / g;
    // the held positions' own outputs at t = (p - d0) / g (s's real frame follows in Submit)
    bool Span(int64_t s, int64_t g, std::string& err)
    {
        const int64_t d0 = s - g;
        for (int64_t p = d0 + 1; p <= s; ++p) {
            for (int j = 1; j < grid.k; ++j)
                if (!Put(SubPts(p, j), float((double(p - 1 - d0) + double(j) / grid.k) / double(g)), err)) return false;
            if (p < s && !Put(grid.Pts(p), float(double(p - d0) / double(g)), err)) return false;
        }
        return true;
    }

    int64_t m_last = -1000000;
    std::vector<Out<Frame>> m_queue;
    bool m_still = false, m_haveDistinct = false;
    std::deque<int64_t> m_pending;            // repeats waiting for the next distinct frame
    std::vector<uint8_t> m_lastQ, m_curQ;     // quarter-size luma: the last distinct frame, the new one
};

} // namespace pmvs::portable
