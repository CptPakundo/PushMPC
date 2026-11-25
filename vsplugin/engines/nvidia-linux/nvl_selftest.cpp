// The RTX 30 VFG gate (nvl_selftest.h).
#include "nvl_selftest.h"
#include <dlfcn.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>
#include "nvl_ampere.h"
#include "nvl_engine.h"

extern char** environ;

namespace pmvs::nvl {

std::string SelfTestKey(const GpuInfo& g)
{
    return g.name + "|" + std::to_string(g.driverVersion) + "|" + std::string(AmpereCompat::kKnownSha256).substr(0, 16);
}

std::string SelfTestCachePath()
{
    const char* x = std::getenv("XDG_CACHE_HOME");
    if (x && *x == '/') return std::string(x) + "/pushmpc/nvvfx-selftest";
    const char* h = std::getenv("HOME");
    return h ? std::string(h) + "/.cache/pushmpc/nvvfx-selftest" : std::string();
}

int ReadSelfTest(const std::string& key)
{
    std::ifstream f(SelfTestCachePath());
    std::string line;
    int r = -1;
    while (std::getline(f, line)) {
        const size_t t = line.rfind('\t');
        if (t != std::string::npos && line.substr(0, t) == key) r = line.substr(t + 1) == "pass" ? 1 : 0;
    }
    return r;
}

void WriteSelfTest(const std::string& key, bool pass)
{
    const std::string path = SelfTestCachePath();
    if (path.empty()) return;
    std::string dir = path.substr(0, path.rfind('/'));
    for (size_t p = 1; p != std::string::npos; p = dir.find('/', p + 1)) mkdir(dir.substr(0, p).c_str(), 0755);
    mkdir(dir.c_str(), 0755);
    std::ofstream f(path, std::ios::app);
    f << key << '\t' << (pass ? "pass" : "fail") << '\n';
}

std::string SelfTestExe()
{
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void*>(&SelfTestExe), &info) || !info.dli_fname) return {};
    std::string p = info.dli_fname;
    const size_t s = p.rfind('/');
    return (s == std::string::npos ? std::string(".") : p.substr(0, s)) + "/pushmpc-nvvfx-selftest";
}

int RunSelfTest(const std::string& exe, const std::string& runtimeDir, int device, int timeoutS, std::string& log)
{
    if (access(exe.c_str(), X_OK) != 0) { log = exe + " is missing"; return -1; }
    const std::string dev = std::to_string(device);
    std::vector<std::string> args = { exe, "--ampere", "--quiet", "--device", dev };
    if (!runtimeDir.empty()) { args.push_back("--runtime"); args.push_back(runtimeDir); }
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv.data(), environ) != 0) { log = "posix_spawn failed"; return -1; }
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutS);
    int status = 0;
    for (;;) {
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0) { log = "waitpid failed"; return -1; }
        if (std::chrono::steady_clock::now() > until) { kill(pid, SIGKILL); waitpid(pid, &status, 0); log = "timed out"; return 0; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const bool pass = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    log = WIFEXITED(status) ? "exit " + std::to_string(WEXITSTATUS(status)) : "killed by signal " + std::to_string(WTERMSIG(status));
    return pass ? 1 : 0;
}

bool SelfTestRun(const GpuInfo& g, bool ampere, std::string& report)
{
    const int w = 1280, h = 720, box = 160, shift = 64;
    EngineConfig cfg;
    cfg.srcW = w; cfg.srcH = h; cfg.bits = 8;
    cfg.plan.k = 2; cfg.plan.vfgQuality = 1; cfg.plan.outW = w; cfg.plan.outH = h;
    cfg.ampere = ampere; cfg.slots = 4;
    Engine e;
    std::string err;
    if (!e.Open(g, cfg, err)) { report = "open: " + err; return false; }
    // three frames: a bright box on grey moving right by `shift` per frame, with a little texture
    auto frame = [&](int x0, std::vector<uint8_t>& y, std::vector<uint8_t>& u, std::vector<uint8_t>& v) {
        y.assign(size_t(w) * h, 0); u.assign(size_t(w / 2) * (h / 2), 128); v.assign(size_t(w / 2) * (h / 2), 128);
        for (int yy = 0; yy < h; ++yy)
            for (int xx = 0; xx < w; ++xx) {
                const bool in = xx >= x0 && xx < x0 + box && yy >= h / 2 - box / 2 && yy < h / 2 + box / 2;
                y[size_t(yy) * w + xx] = uint8_t(in ? 220 : 60 + ((xx / 8 + yy / 8) & 1) * 20);
            }
    };
    std::vector<uint8_t> Y[3], U[3], V[3];
    for (int i = 0; i < 3; ++i) frame(200 + i * shift, Y[i], U[i], V[i]);
    std::vector<uint8_t> mid, mu, mv; frame(200 + shift + shift / 2, mid, mu, mv);   // truth between frames 1 and 2
    SrcView sv; sv.width = w; sv.height = h; sv.bits = 8; sv.fmt.bits = 8; sv.fmt.full = false; sv.fmt.matrix = 1;
    int gen = -1;
    for (int i = 0; i < 3; ++i) {
        sv.data[0] = Y[i].data(); sv.data[1] = U[i].data(); sv.data[2] = V[i].data();
        sv.stride[0] = w; sv.stride[1] = sv.stride[2] = w / 2;
        if (!e.Upload(sv, i & 1, err)) { report = "upload: " + err; return false; }
        if (i == 2) { gen = e.Generate(i & 1, 1, 2, true, err); if (gen < 0) { report = "VFG: " + err; return false; } }
    }
    if (!e.Wait(gen, 30000)) { report = "VFG did not finish (CUDA error or timeout)"; return false; }
    const OutView o = e.View(gen);
    double dMid = 0, dA = 0, dB = 0, mean = 0, meanIn = 0;
    for (int yy = 0; yy < h; ++yy)
        for (int xx = 0; xx < w; ++xx) {
            const double g8 = o.data[0][o.pitch[0] * size_t(yy) + size_t(xx)];
            const size_t i = size_t(yy) * w + xx;
            dMid += std::fabs(g8 - mid[i]); dA += std::fabs(g8 - Y[1][i]); dB += std::fabs(g8 - Y[2][i]);
            mean += g8; meanIn += 0.5 * (Y[1][i] + Y[2][i]);
        }
    const double n = double(w) * h;
    dMid /= n; dA /= n; dB /= n; mean /= n; meanIn /= n;
    // the engine writes limited range: the test frames were limited too, so the codes compare directly
    char b[256];
    snprintf(b, sizeof(b), "VFG x2 1280x720%s: |gen-mid| %.2f, |gen-A| %.2f, |gen-B| %.2f, mean %.1f (inputs %.1f); hooks: %u retargeted loads, %u spoofs, %u rejected",
             ampere ? " (RTX 30 compatibility)" : "", dMid, dA, dB, mean, meanIn, AmpereCompat::PatchedLoads(), AmpereCompat::AttributeSpoofs(), AmpereCompat::RejectedLoads());
    report = b;
    const bool finite = std::isfinite(dMid) && mean > 10;
    return finite && std::fabs(mean - meanIn) < 0.05 * meanIn + 2 && dMid < dA && dMid < dB && (!ampere || AmpereCompat::PatchedLoads() > 0);
}

} // namespace pmvs::nvl
