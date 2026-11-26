// SPDX-License-Identifier: GPL-3.0-or-later
// The Linux NVIDIA backend's host-side rules, shared by every host that runs nvl::Engine (the VapourSynth plugin,
// nvidia_linux_vs.cpp; capture mode, capture/src/cap_engines.cpp), so both decide the same way:
//   ProbeGated     ProbeGpu once per process, then the RTX 30 VFG gate: VFG through the Ampere compatibility only after
//                  pushmpc-nvvfx-selftest passed on this GPU and driver (cached; PUSHMPC_VFG_AMPERE=1/0 forces it)
//   flow costs     the optical-flow tier's measured costs per GPU and size, written when an engine closes and read by
//                  Auto next time (Costs::flowMeasured / warpMeasured): <cache>/pushmpc/nvl-flow-costs
#pragma once
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include "nvl_engine.h"
#include "nvl_selftest.h"

namespace pmvs::nvl {

// once per process (the first call's arguments count); log gets one line per finding
inline const GpuInfo& ProbeGated(int device, const std::string& runtimeDir, const std::function<void(const std::string&)>& log)
{
    static std::mutex m;
    static bool done = false;
    static GpuInfo g;
    std::lock_guard<std::mutex> lk(m);
    if (done) return g;
    done = true;
    g = ProbeGpu(device, runtimeDir);
    if (!g.driver) { log("nvidia (Linux): " + g.why); return g; }
    char b[512];
    std::snprintf(b, sizeof(b), "nvidia (Linux): %s (SM %d.%d, %s, %zu MB), driver %d.%d; runtime %s%s", g.name.c_str(), g.major, g.minor, GpuClassName(g.cls),
                  g.memBytes >> 20, g.driverVersion / 1000, (g.driverVersion % 1000) / 10, g.runtimeDir.c_str(), g.runtime ? "" : (" MISSING (" + g.runtimeWhy + ")").c_str());
    log(b);
    if (g.runtime && VfgAmpere(g.major, g.minor)) {
        const char* force = std::getenv("PUSHMPC_VFG_AMPERE");
        const std::string key = SelfTestKey(g);
        int r = force ? (std::strcmp(force, "0") ? 1 : 0) : ReadSelfTest(key);
        std::string why = force ? std::string("PUSHMPC_VFG_AMPERE=") + force : std::string("cached");
        if (r < 0) r = RunSelfTest(SelfTestExe(), g.runtimeDir, g.device, 120, why);
        if (r == 1) { g.caps.vfg = true; g.caps.vfgWhy.clear(); }
        else g.caps.vfgWhy = r == 0 ? "RTX 30: the VFG compatibility self-test failed on this GPU / driver (" + why + ")" : "RTX 30: the VFG compatibility self-test could not run (" + why + ")";
        log("nvidia (Linux): RTX 30 VFG compatibility: " + std::string(r == 1 ? "on" : "off") + " (" + (r == 1 ? why : g.caps.vfgWhy) + ")");
    }
    return g;
}

inline std::string FlowCostKey(const std::string& gpu, int w, int h, int outH) { return gpu + "|" + std::to_string(w) + "x" + std::to_string(h) + "|" + std::to_string(outH); }
inline std::string FlowCostFile() { std::string p = SelfTestCachePath(); const size_t s = p.find_last_of("/\\"); return s == std::string::npos ? std::string() : p.substr(0, s + 1) + "nvl-flow-costs"; }
inline void ReadFlowCosts(const std::string& key, double& flow, double& warp)
{
    std::ifstream f(FlowCostFile());
    std::string line;
    while (std::getline(f, line)) {
        const size_t a = line.find('\t'), b = line.find('\t', a + 1);
        if (a != std::string::npos && b != std::string::npos && line.substr(0, a) == key) { flow = std::atof(line.c_str() + a + 1); warp = std::atof(line.c_str() + b + 1); }
    }
}
inline void WriteFlowCosts(const std::string& key, double flow, double warp)
{
    const std::string file = FlowCostFile();
    if (file.empty() || flow <= 0 || warp <= 0) return;
    double f0 = 0, w0 = 0;
    ReadFlowCosts(key, f0, w0);   // write only what changed by more than 10 % (the last line of a key wins)
    if (f0 > 0 && w0 > 0 && std::fabs(flow - f0) <= 0.1 * f0 && std::fabs(warp - w0) <= 0.1 * w0) return;
    std::ofstream f(file, std::ios::app);
    f << key << '\t' << flow << '\t' << warp << '\n';
}

} // namespace pmvs::nvl
