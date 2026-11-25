// The RTX 30 VFG gate: Auto turns on VFG through the Ampere compatibility only after pushmpc-nvvfx-selftest (a separate
// process, so a driver fault cannot take the player down) ran VFG on a synthetic moving pair on this GPU and driver and
// the generated frame was plausible. The result is cached per GPU name, driver version and VFG library:
//   $XDG_CACHE_HOME/pushmpc/nvvfx-selftest, else ~/.cache/pushmpc/nvvfx-selftest   (one line: "<key>\t<pass|fail>")
// PUSHMPC_VFG_AMPERE=1 skips the gate (forced on), =0 keeps it off.
#pragma once
#include <string>

namespace pmvs::nvl {

struct GpuInfo;
std::string SelfTestKey(const GpuInfo& g);
std::string SelfTestCachePath();
// -1 unknown, 0 failed, 1 passed
int ReadSelfTest(const std::string& key);
void WriteSelfTest(const std::string& key, bool pass);
// runs `exe --ampere --quiet` (timeout in seconds), caches and returns the result (-1 when it could not run)
int RunSelfTest(const std::string& exe, const std::string& runtimeDir, int device, int timeoutS, std::string& log);
// the helper next to the plugin (dladdr of this code): <dir>/pushmpc-nvvfx-selftest
std::string SelfTestExe();

// the in-process test itself (the helper's main): VFG x2 on a 1280x720 moving box, ampere = the compatibility on.
// true = plausible (finite, not blank, mean luma kept, closer to the true midpoint than to either input)
bool SelfTestRun(const GpuInfo& g, bool ampere, std::string& report);

} // namespace pmvs::nvl
