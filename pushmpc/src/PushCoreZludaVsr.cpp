// pushcore: RTX Video Super Resolution on AMD RX 7000 / 9000 through ZLUDA (PushCoreZludaVsr.h).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "PushCoreZludaVsr.h"
#include "ZludaVsrClient.h"
#include "veyra/RuntimePaths.h"
#include "veyra/Log.h"
#include <atomic>
#include <algorithm>
#include <format>

namespace pushmpc {
namespace {

std::string narrowZ(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::filesystem::path zludaDataDir() { return veyra::runtime::localDataDirectory() / L"zluda-vsr"; }
// the worker's translation cache and the log folder, made when something runs (never at a capability query)
void makeDirs(const ZludaVsrState& s)
{
    std::error_code ec;
    std::filesystem::create_directories(s.cache, ec);
    std::filesystem::create_directories(s.logDir, ec);
}

std::atomic<bool> gSelfTestStarted{false};

}  // namespace

ZludaVsrState zludaVsrState(const DXGI_ADAPTER_DESC1& adapter, uint32_t family, const std::wstring& driver,
                            const std::filesystem::path& runtimeDir, const std::filesystem::path& workerDir)
{
    ZludaVsrState s;
    const std::wstring rt = runtimeDir.lexically_normal().wstring();
    s.luid = (uint64_t(uint32_t(adapter.AdapterLuid.HighPart)) << 32) | adapter.AdapterLuid.LowPart;
    s.worker = (workerDir / L"pushcore_zluda_vsr_worker.exe").wstring();
    s.zluda = std::filesystem::path(pmz::ZludaDir(rt)).lexically_normal().wstring();
    s.vsr = pmz::FindVsr(rt);
    const auto data = zludaDataDir();
    s.cache = (data / L"cache").wstring();
    s.logDir = (veyra::runtime::logsDirectory()).wstring();
    s.elig = pmz::Check(adapter.VendorId, family, !pmz::FindHip().empty(), pmz::MissingZluda(rt), !s.vsr.empty());
    if (s.elig.files && !pmz::FileThere(s.worker)) { s.elig.files = false; s.elig.why = "pushcore_zluda_vsr_worker.exe is missing next to pushcore.dll"; }
    std::wstring src;
    s.mode = pmu::ReadUnlockMode(pmu::Unlock::ZludaVsr, &src);
    s.key = pmu::SelfTestKey(adapter.VendorId, adapter.DeviceId, driver);
    s.selfTest = pmu::ReadSelfTest(pmu::Unlock::ZludaVsr, s.key);
    const std::wstring why(s.elig.why.begin(), s.elig.why.end());
    s.decision = pmu::DecideUnlock(pmu::Unlock::ZludaVsr, s.mode, s.elig.hardware && s.elig.files, why, s.selfTest, false, src);
    if (s.selfTest.known && s.selfTest.passed) {
        s.layout = pmz::LayoutFromReason(s.selfTest.reason);
        s.selfTestMs = pmz::MsFromReason(s.selfTest.reason);
    }
    return s;
}

bool zludaVsrStartSelfTest(const ZludaVsrState& s, uint32_t width, uint32_t height, uint32_t scale, std::string& why)
{
    if (!s.decision.runSelfTest) { why = "no self-test needed (" + narrowZ(s.decision.why) + ")"; return false; }
    if (!s.elig.hardware || !s.elig.files) { why = s.elig.why; return false; }
    bool expected = false;
    if (!gSelfTestStarted.compare_exchange_strong(expected, true)) { why = "the ZludaVsr self-test was already started by this process"; return false; }
    std::wstring extra;
    // the stream's own size when it fits RTX VSR's range (else the worker's default 960x540 x2)
    if (scale >= 1 && scale <= 4 && pmz::ValidExtent(width, height, width * scale, height * scale) && !(width == 960 && height == 540 && scale == 2))
        extra = std::format(L"--width {} --height {} --scale {}", width, height, scale);
    makeDirs(s);
    const std::wstring log = (std::filesystem::path(s.logDir) / L"zluda-vsr-selftest.log").wstring();
    const std::wstring regBase = pmu::RegBase() == L"Software\\PushMPC" ? std::wstring() : pmu::RegBase();
    if (!pmz::StartSelfTest(s.worker, s.luid, s.zluda, s.vsr, s.cache, log, s.key, regBase, why, nullptr, extra)) {
        gSelfTestStarted = false;
        return false;
    }
    veyra::log::info("zluda-vsr", "self-test started in the background (" + narrowZ(s.decision.why) + "); FSR until it has passed");
    return true;
}

int zludaVsrSelfTestNow(const ZludaVsrState& s, uint32_t timeoutMs, std::string& why)
{
    if (!s.elig.hardware || !s.elig.files) { why = s.elig.why; return -1; }
    makeDirs(s);
    const std::wstring log = (std::filesystem::path(s.logDir) / L"zluda-vsr-selftest.log").wstring();
    const std::wstring regBase = pmu::RegBase() == L"Software\\PushMPC" ? std::wstring() : pmu::RegBase();
    HANDLE process = nullptr;
    if (!pmz::StartSelfTest(s.worker, s.luid, s.zluda, s.vsr, s.cache, log, s.key, regBase, why, &process)) return -1;
    const DWORD w = WaitForSingleObject(process, timeoutMs ? timeoutMs : INFINITE);
    DWORD code = 1;
    if (w != WAIT_OBJECT_0) { TerminateProcess(process, 124); WaitForSingleObject(process, 3000); why = "the self-test did not finish in time"; CloseHandle(process); return -1; }
    GetExitCodeProcess(process, &code);
    CloseHandle(process);
    const pmu::SelfTest t = pmu::ReadSelfTest(pmu::Unlock::ZludaVsr, s.key);
    why = narrowZ(t.reason);
    if (code == 2) { why = "the worker refused its arguments"; return -1; }
    return code == 0 && t.known && t.passed ? 1 : 0;
}

struct ZludaVsrSr::Impl {
    ZludaVsrState state;
    pmz::WorkerClient client;
};

ZludaVsrSr::ZludaVsrSr(const ZludaVsrState& s) : impl_(std::make_unique<Impl>()) { impl_->state = s; }
ZludaVsrSr::~ZludaVsrSr() { shutdown(); }

bool ZludaVsrSr::initialize(ID3D12Device* device, unsigned inputWidth, unsigned inputHeight, unsigned outputWidth, unsigned outputHeight, unsigned quality)
{
    const auto& s = impl_->state;
    makeDirs(s);
    pmz::WorkerClient::Config cfg;
    cfg.worker = s.worker; cfg.zluda = s.zluda; cfg.vsr = s.vsr; cfg.cache = s.cache;
    cfg.log = (std::filesystem::path(s.logDir) / L"zluda-vsr-worker.log").wstring();
    cfg.inW = inputWidth; cfg.inH = inputHeight; cfg.outW = outputWidth; cfg.outH = outputHeight;
    cfg.quality = std::clamp(quality, 1u, 4u);
    cfg.layout = s.layout;
    if (!impl_->client.Open(device, cfg)) {
        veyra::log::warn("zluda-vsr", "the worker did not start: " + impl_->client.Error());
        return false;
    }
    veyra::log::info("zluda-vsr", std::format("worker pid {} ready: {}x{} -> {}x{} quality {} layout {}", impl_->client.Pid(), inputWidth, inputHeight,
                                              outputWidth, outputHeight, cfg.quality, cfg.layout));
    return true;
}
bool ZludaVsrSr::copyIn(ID3D12GraphicsCommandList* list, ID3D12Resource* src) { return impl_->client.CopyIn(list, src); }
bool ZludaVsrSr::run(ID3D12CommandQueue* queue) { return impl_->client.Run(queue); }
bool ZludaVsrSr::copyOut(ID3D12GraphicsCommandList* list, ID3D12Resource* dst) { return impl_->client.CopyOut(list, dst); }
bool ZludaVsrSr::lost() const { return impl_->client.Lost(); }
void ZludaVsrSr::shutdown()
{
    if (!impl_) return;
    if (impl_->client.IsOpen() && impl_->client.Lost()) veyra::log::warn("zluda-vsr", "the worker stopped: " + impl_->client.Error());
    impl_->client.Close();
}
std::string ZludaVsrSr::error() const { return impl_->client.Error(); }
double ZludaVsrSr::lastMs() const { return impl_->client.LastMs(); }

}  // namespace pushmpc
