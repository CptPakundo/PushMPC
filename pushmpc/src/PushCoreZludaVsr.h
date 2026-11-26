#pragma once
// pushcore: RTX Video Super Resolution on AMD RX 7000 / 9000 through ZLUDA (PUSHCORE_SR_METHOD_ZLUDA_VSR, cap
// PUSHCORE_CAP_ZLUDA_VSR, unlock ZludaVsr). The decision (eligibility, files, off switch, the cached self-test), the
// background self-test, and the EnhanceGraph external upscaler that drives pushcore_zluda_vsr_worker.exe.
// The worker and its protocol: pushmpc/include/pushmpc/zluda_vsr.h, pushmpc/src/ZludaVsrClient.h.
#include <windows.h>
#include <dxgi1_6.h>
#include <filesystem>
#include <memory>
#include <string>
#include "pushmpc/unlocks.h"
#include "pushmpc/zluda_vsr.h"
#include "veyra/pipeline/ExternalSr.h"

namespace pushmpc {

struct ZludaVsrState {
    pmz::Eligibility elig;          // hardware (RX 7000 / 9000) and files (HIP, ZLUDA, nvngx_vsr.dll)
    pmu::UnlockMode mode = pmu::UnlockMode::Auto;
    pmu::UnlockDecision decision;   // on = run it; runSelfTest = start the self-test
    pmu::SelfTest selfTest;
    std::wstring key;               // vendor:device:driver
    std::wstring worker, zluda, vsr, cache, logDir;
    uint32_t layout = 0;            // the NGX parameter layout the self-test passed with
    double selfTestMs = -1;         // its median ms at 960x540 -> 1920x1080
    uint64_t luid = 0;
};
// workerDir = the folder of pushcore.dll (pushcore_zluda_vsr_worker.exe beside it); runtimeDir = <root>\runtime\experimental
ZludaVsrState zludaVsrState(const DXGI_ADAPTER_DESC1& adapter, uint32_t family, const std::wstring& driver,
                            const std::filesystem::path& runtimeDir, const std::filesystem::path& workerDir);
// starts the background self-test once per process (eligible, Auto, no result for this adapter + driver), at the given
// stream size too when it is not the default one; false + why when it does not start
bool zludaVsrStartSelfTest(const ZludaVsrState& s, uint32_t width, uint32_t height, uint32_t scale, std::string& why);
// the self-test now, waiting for it (pushcore_selfTest PUSHCORE_SELFTEST_ZLUDA_VSR: Setup, the harness, a tester); runs
// even when a result is cached (the worker replaces it): 1 = passed, 0 = failed, -1 = could not run; why = its reason
int zludaVsrSelfTestNow(const ZludaVsrState& s, uint32_t timeoutMs, std::string& why);

// The graph's external upscaler: one worker per graph
class ZludaVsrSr final : public veyra::pipeline::ExternalSr {
public:
    explicit ZludaVsrSr(const ZludaVsrState& s);
    ~ZludaVsrSr() override;
    bool initialize(ID3D12Device* device, unsigned inputWidth, unsigned inputHeight, unsigned outputWidth, unsigned outputHeight, unsigned quality) override;
    bool copyIn(ID3D12GraphicsCommandList* list, ID3D12Resource* rgba8CopySource) override;
    bool run(ID3D12CommandQueue* queue) override;
    bool copyOut(ID3D12GraphicsCommandList* list, ID3D12Resource* rgba8CopyDest) override;
    bool lost() const override;
    const char* name() const override { return "RTX VSR through ZLUDA"; }
    void shutdown() override;
    std::string error() const;
    double lastMs() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pushmpc
