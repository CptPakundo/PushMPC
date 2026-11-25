// pushmpc-nvvfx-selftest: the RTX 30 VFG gate (nvl_selftest.h) and a smoke test of the Linux NVIDIA backend.
//   pushmpc-nvvfx-selftest [--ampere] [--device N] [--runtime DIR] [--quiet]
// Without --ampere: VFG as NVIDIA ships it (Ada / Blackwell / Hopper). With --ampere: through the RTX 30 compatibility;
// the result is cached for Auto. Exit 0 = plausible output, 1 = implausible or failed, 2 = cannot run here.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "nvl_engine.h"
#include "nvl_selftest.h"

using namespace pmvs::nvl;

int main(int argc, char** argv)
{
    bool ampere = false, quiet = false;
    int device = -1;
    std::string runtime;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--ampere")) ampere = true;
        else if (!std::strcmp(argv[i], "--quiet")) quiet = true;
        else if (!std::strcmp(argv[i], "--device") && i + 1 < argc) device = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--runtime") && i + 1 < argc) runtime = argv[++i];
        else { std::fprintf(stderr, "usage: %s [--ampere] [--device N] [--runtime DIR] [--quiet]\n", argv[0]); return 2; }
    }
    const GpuInfo g = ProbeGpu(device, runtime);
    if (!quiet) std::printf("GPU: %s (SM %d.%d, %s), driver %d, runtime %s%s\n", g.driver ? g.name.c_str() : "none", g.major, g.minor, GpuClassName(g.cls), g.driverVersion,
                            g.runtimeDir.c_str(), g.runtime ? "" : " (missing)");
    if (!g.driver) { std::fprintf(stderr, "no NVIDIA GPU: %s\n", g.why.c_str()); return 2; }
    if (!g.runtime) { std::fprintf(stderr, "%s\n", g.runtimeWhy.c_str()); return 2; }
    std::string report;
    const bool pass = SelfTestRun(g, ampere, report);
    std::printf("%s: %s\n", pass ? "PASS" : "FAIL", report.c_str());
    if (ampere) WriteSelfTest(SelfTestKey(g), pass);
    return pass ? 0 : 1;
}
