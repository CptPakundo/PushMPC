// SPDX-License-Identifier: GPL-3.0-or-later
// pushmpc.Enhance, portable GPU engine: the held-drawing repair for the engine's schedulers (GpuBackend for VapourSynth,
// the mpv Vulkan engines). One frame in: its cell grid (Engine::Cells) to the scheduler (pushmpc/src/HeldRepair.h);
// kLookahead frames later: its admitted parts, each from its own pair at its own phase, laid in (Engine::RepairSlot).
// The caller then forgets any motion it estimated on the frame before the repair (invalidate).
// A tree without pushmpc/src builds without the repair (PMVS_REGION_REPAIR undefined).
#pragma once
#if __has_include("../../../pushmpc/src/HeldRepair.h")
#include "../../../pushmpc/src/HeldRepair.h"
#define PMVS_REGION_REPAIR 1
#endif
#include <chrono>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "gpu_engine.h"

namespace pmvs::gpu {

struct RegionRepairStats { uint64_t frames = 0, parts = 0, gated = 0, overBudget = 0; double ms = 0; };
#ifdef PMVS_REGION_REPAIR
class RegionRepair {
public:
    bool on = false;
    pushmpc::held::RepairScheduler sched;
    void Reset() { sched.Reset(); m_upTo = -1; m_repaired.clear(); }
    // frame s's picture was repaired: its own output must come from the GPU slot, not from the source planes
    bool Repaired(int64_t s) const { return m_repaired.count(s) != 0; }
    RegionRepairStats Stats() const { const auto& t = sched.stats; return { t.frames, t.parts, t.gated, t.overBudget, t.ms }; }
    // slot: the frame's picture (still alive for a duplicate), prevSlot: the last distinct one (-1: none)
    bool Classify(Engine& e, int64_t s, int slot, int prevSlot, bool dup, std::string& err)
    {
        if (!on) return true;
        Engine::CellGridOut c;
        if (!e.Cells(slot, prevSlot, c, err)) return false;
        pushmpc::held::CellGrid g;
        g.w = uint32_t(c.w); g.h = uint32_t(c.h); g.cell = uint32_t(c.cell); g.v = std::move(c.v); g.unpackAux(c.aux.data());
        sched.Push(s, std::move(g), dup);
        return true;
    }
    // every distinct frame up to q is repaired once (its next drawings are classified); sourceMs: a source frame's
    // duration (budget: half of it per repaired frame, the engine waits for the GPU here)
    template <class Invalidate>
    bool RepairUpTo(Engine& e, int64_t q, const std::map<int64_t, int>& slotOf, double sourceMs, Invalidate invalidate, std::string& err)
    {
        if (!on) return true;
        for (int64_t s = std::max<int64_t>(m_upTo + 1, q); s <= q; ++s) {
            m_upTo = s;
            auto self = slotOf.find(s);
            if (self == slotOf.end()) continue;   // a duplicate: Dedup re-times it whole
            const auto groups = sched.Plan(s, 0.5 * sourceMs);
            std::vector<Engine::RepairPart> parts;
            const pushmpc::held::CellGrid* g = sched.Grid(s);
            const float cell = g ? float(g->cell) : 1.f;
            for (const auto& gr : groups) {
                auto a = slotOf.find(int64_t(gr.pairFrom)), b = slotOf.find(int64_t(gr.d1));
                if (a == slotOf.end() || b == slotOf.end() || parts.size() >= 3) continue;
                Engine::RepairPart p;
                p.slotA = a->second; p.slotB = b->second; p.t = gr.t; p.mask = &gr.mask; p.shiftX = gr.shiftX * cell; p.shiftY = gr.shiftY * cell;
                parts.push_back(p);
            }
            if (parts.empty()) continue;
            invalidate(s);
            const auto t0 = std::chrono::steady_clock::now();
            const float feather = std::max(0.5f, 8.0f * float(e.Cfg().width) / 3840.0f / cell);
            const char* d = std::getenv("PUSHMPC_REPAIR_DETAIL");
            if (!e.RepairSlot(self->second, parts.data(), int(parts.size()), feather, !(d && std::atoi(d) == 0), err)) return false;
            sched.Cost(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), parts.size());
            m_repaired.insert(s);
            while (m_repaired.size() > 64) m_repaired.erase(m_repaired.begin());
        }
        return true;
    }
    // the slots a repair can still read: pair starts up to kLookahead + 1 behind the frame repaired
    bool Needs(int64_t frame, int64_t newest, int lookahead) const { return on && frame >= newest - 2 * int64_t(lookahead) - 2; }
private:
    int64_t m_upTo = -1;
    std::set<int64_t> m_repaired;
};
#else
class RegionRepair {
public:
    bool on = false;
    void Reset() {}
    RegionRepairStats Stats() const { return {}; }
    bool Classify(Engine&, int64_t, int, int, bool, std::string&) { return true; }
    template <class Invalidate> bool RepairUpTo(Engine&, int64_t, const std::map<int64_t, int>&, double, Invalidate, std::string&) { return true; }
    bool Needs(int64_t, int64_t, int) const { return false; }
    bool Repaired(int64_t) const { return false; }
};
#endif

// the repair is wanted (Dedup on, PUSHMPC_REGION_REPAIR not 0)
inline bool RegionRepairWanted(bool dedup) { const char* e = std::getenv("PUSHMPC_REGION_REPAIR"); return dedup && !(e && std::atoi(e) == 0); }

} // namespace pmvs::gpu
