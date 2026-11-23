// pushmpc.Enhance, backend "portable": the engine behind the same Pipeline (schedule.h) the pushcore backend uses.
// Output order is pushcore's: for each submitted source s, the generated frames of the pair (s - 1, s), then s's real
// frame. Generated frame j of the pair has pts(s - 1) + j x source duration / k, so it lands on output (s - 1) k + j.
// Frame: a VSFrame* in the plugin, an int handle in portable_test.
#pragma once
#include <chrono>
#include <cmath>
#include <functional>
#include <string>
#include <vector>
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

    bool CanSubmit() override { return true; }
    bool Submit(int64_t s, int64_t pts, bool discontinuity, std::string& err) override
    {
        YuvView v;
        if (!source || !source(s, v)) { err = "portable: source frame " + std::to_string(s) + " is not available"; return false; }
        const bool contiguous = !discontinuity && m_last == s - 1;
        if (!engine.Push(v, contiguous, err)) return false;
        m_last = s;
        if (engine.HavePair())
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
        return true;
    }
    void Release(Frame f) override { if (release) release(f); }
    uint64_t NowMs() override
    {
        return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }

private:
    int64_t m_last = -1000000;
    std::vector<Out<Frame>> m_queue;
};

} // namespace pmvs::portable
