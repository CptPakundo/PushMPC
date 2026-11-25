// pushmpc.Enhance, backend "nvidia" on Linux: the engine behind the same Pipeline (schedule.h) the other backends use.
// Output order is pushcore's: for each submitted source s, the generated frames of the pair (s - 1, s), then s's real
// frame; generated frame j of the pair has pts(s - 1) + j x source duration / k. Submit only queues GPU work (the
// slots' events finish later); Poll hands out finished slots in order, copied into new output frames. CanSubmit is
// false while fewer than k readback slots are free, so the GPU runs at most a few source frames ahead.
// The cut guard decides each pair on the CPU from the source luma before any GPU work: a cut pair shows A held.
#pragma once
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <vector>
#include "nvl_colour.h"
#include "nvl_cutguard.h"
#include "nvl_engine.h"
#include "schedule.h"

namespace pmvs::nvl {

template <class Frame> class NvlBackend : public Backend<Frame> {
public:
    std::function<bool(int64_t s, SrcView& v)> source;            // the source frame s (valid during Submit)
    std::function<bool(const OutView& o, Frame& f)> alloc;        // a new output frame holding o's pixels
    std::function<void(Frame f)> release;
    std::function<void(int64_t pts, bool generated, Frame f)> tag;

    Engine* engine = nullptr;
    CutGuard guard;
    Grid grid;
    uint64_t heldCuts = 0, generated = 0;

    bool CanSubmit() override { return engine->FreeSlots() >= grid.k; }
    bool Submit(int64_t s, int64_t pts, bool discontinuity, std::string& err) override
    {
        SrcView v;
        if (!source || !source(s, v)) { err = "nvidia: source frame " + std::to_string(s) + " is not available"; return false; }
        const bool contiguous = !discontinuity && m_last == s - 1;
        const bool gpuGuard = engine->HasGpuGuard();
        // the cut guard: on the GPU (Windows' HLSL, decided there, after the upload) or the CPU port (the source's luma)
        CutDecision cut;
        if (!gpuGuard)
            cut = guard.Push([&](int x, int y) -> uint32_t {
                const uint8_t* row = v.data[0] + v.stride[0] * y;
                const uint32_t c = v.bits > 8 ? reinterpret_cast<const uint16_t*>(row)[x] : row[x];
                return GuardLuma(c, v.bits, v.fmt.full);
            }, contiguous);
        const int parity = int(s & 1);
        if (!engine->Upload(v, parity, err)) return false;
        // NVIDIA optical flow of the pair (the fast tier's motion and the guard's flow measures), before both use it
        if (contiguous && grid.k >= 2 && engine->HasFlow() && !engine->Flow(parity, err)) return false;
        if (gpuGuard && grid.k >= 2 && !engine->Guard(parity, contiguous, err)) return false;
        if (contiguous && grid.k >= 2) {
            if (cut.cut) ++heldCuts;
            if (gpuGuard) heldCuts = engine->GuardCuts();
            for (int j = 1; j < grid.k; ++j) {
                const int slot = cut.cut ? engine->Emit(1 - parity, err) : engine->Generate(parity, j, grid.k, j == 1, err);
                if (slot < 0) return false;
                if (!cut.cut) ++generated;
                const int64_t p = grid.Pts(s - 1) + int64_t(std::llround(double(j) * grid.srcDur100ns / grid.k));
                m_queue.push_back({ p, true, slot });
            }
        }
        const int slot = engine->Emit(parity, err);
        if (slot < 0) return false;
        m_queue.push_back({ pts, false, slot });
        m_last = s;
        return true;
    }
    bool Poll(uint32_t timeoutMs, std::vector<Out<Frame>>& out, std::string& err) override
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (!m_queue.empty()) {
            const Pending q = m_queue.front();
            int st = engine->Query(q.slot);
            if (st == 0 && timeoutMs > 0) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count();
                if (left > 0) st = engine->Wait(q.slot, uint32_t(left)) ? 1 : engine->Query(q.slot);
            }
            if (st < 0) { err = "nvidia: the GPU failed on a frame (CUDA error)"; return false; }
            if (st == 0) break;
            Frame f{};
            if (!alloc(engine->View(q.slot), f)) { err = "nvidia: could not allocate an output frame"; return false; }
            engine->Release(q.slot);
            m_queue.pop_front();
            if (tag) tag(q.pts, q.generated, f);
            out.push_back({ q.pts, q.generated, f });
            timeoutMs = 0;   // the first one waited; the rest only if already done
        }
        return true;
    }
    bool Flush(std::string& err) override
    {
        m_queue.clear();
        guard.Reset();
        m_last = -1000000;
        return engine->Drain(err);
    }
    void Release(Frame f) override { if (release) release(f); }
    uint64_t NowMs() override
    {
        return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }

private:
    struct Pending { int64_t pts; bool generated; int slot; };
    int64_t m_last = -1000000;
    std::deque<Pending> m_queue;
};

} // namespace pmvs::nvl
