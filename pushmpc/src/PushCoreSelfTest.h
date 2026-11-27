// pushcore: the vendor self-tests behind the pushcore_selfTest export (PushCoreSelfTest.cpp). Internal to pushcore.dll.
//
// Each method with a self-test (Intel XeSS FG and SR, Intel VSR, Intel VPL frame interpolation, the D3D11 <-> D3D12 bridge;
// later ZLUDA VSR and community ports) registers it from its own source file with a static Registrar, under its bit of the
// global PUSHCORE_SELFTEST_* space and the report key named in pushcore_abi.h. pushcore_selfTest resolves the adapter once,
// runs every asked-for test that is registered and writes one JSON report. The registered methods' PUSHCORE_FEATURE_* bits
// are part of pushcore_features(), so a feature bit appears exactly when its code is linked in.
#pragma once
#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace pushmpc::selftest {

struct Context {
    bool haveAdapter = false;
    DXGI_ADAPTER_DESC1 adapter{};
    uint64_t luid = 0;            // the adapter's LUID (WARP's own with warp)
    std::wstring driver;          // user-mode driver version "a.b.c.d" as pushcore_capabilities keys the results, or "unknown"
    bool warp = false;            // PUSHCORE_SELFTEST_FLAG_WARP: Windows' software adapter
    bool record = false;          // PUSHCORE_SELFTEST_FLAG_RECORD: store hardware results (never WARP ones)
    std::filesystem::path root;   // the runtime root (rootDir, else pushcore's folder); already set as Veyra's root override
};

struct Result {
    bool pass = false;
    std::string verdict;                                        // "pass; ..." / "fail: ..." / "unavailable: ..."
    std::vector<std::pair<std::string, std::string>> fields;   // further report fields: "<key>.<name>" = value (strings)
};

using RunFn = Result (*)(const Context&);
struct Test {
    uint32_t bit;          // PUSHCORE_SELFTEST_*
    const char* key;       // the report key (pushcore_abi.h)
    uint32_t features;     // PUSHCORE_FEATURE_* this method brings (0 = none)
    RunFn run;
};
bool add(const Test& test);
struct Registrar { explicit Registrar(const Test& test) { add(test); } };

uint32_t features();       // the registered methods' PUSHCORE_FEATURE_* bits

// helpers for the tests
std::string narrow(const std::wstring& s);
std::wstring widen(const std::string& s);
std::wstring driverVersion(const LUID& luid);     // the key pushcore_capabilities uses (pmu::SelfTestKey)
uint64_t luidOf(const LUID& luid);

// ---------------------------------------------------------------------------------------------------- upscaler probe
// A 2x temporal upscaler (XeSS SR, FSR) driven over a synthetic scene for a number of frames, each output compared with the
// scene's ground truth: what the XeSS SR test measured, shared so XeSS and FSR are measured alike. Scenes:
//   Pan          the picture pans by (2.6, 1.3) output px a frame with exact motion vectors (new sub-pixel phases every
//                frame: what a temporal upscaler is built for)
//   Static       a still picture, zero motion, history kept (video without camera jitter: the case where an accumulating
//                upscaler can collapse to the source grid)
//   StaticReset  the same with a reset every frame (a single-frame upscale)
// Frames 0, 1 and the last 12 are measured: PSNR against the truth, against bicubic and bilinear upscales of the input, and
// the 2x2 block ratio (mean |step| inside 2x2 output blocks over that across their borders: about 1 for a smooth upscale,
// near 0 for one that repeats each input pixel). On WARP these are functional numbers of a software rasteriser, not
// quality claims for any GPU.
enum class UpscaleScene { Pan, Static, StaticReset };
const char* sceneName(UpscaleScene s);
struct UpscaleFrame { uint32_t k = 0; double psnr = 0, bicubic = 0, bilinear = 0, block = 0, blockBicubic = 0; };
struct UpscaleRun {
    bool ok = false;
    std::string note;             // why not ok
    std::vector<UpscaleFrame> frames;
    double msPerFrame = -1;       // evaluate + submit + wait, averaged over every frame
};
// eval: record the upscale of color (w x h, R16G16B16A16_FLOAT, NON_PIXEL_SHADER_RESOURCE) with motion (R16G16_FLOAT, input
// px, previous = current + motion), depth (R32_FLOAT) into out (W x H, R16G16B16A16_FLOAT, UNORDERED_ACCESS)
using UpscaleEval = std::function<bool(ID3D12GraphicsCommandList* list, ID3D12Resource* color, ID3D12Resource* motion, ID3D12Resource* depth,
                                       ID3D12Resource* out, uint32_t w, uint32_t h, uint32_t W, uint32_t H, bool reset)>;
UpscaleRun upscaleProbe(ID3D12Device* device, ID3D12CommandQueue* queue, uint32_t W, uint32_t H, uint32_t frames, UpscaleScene scene, const UpscaleEval& eval);
// "f0 psnr 18.2 (bicubic 18.0) block 0.91 (bicubic 0.86); f1 ...; f36-47 mean ..." (one report field)
std::string upscaleSummary(const UpscaleRun& run, bool warp);   // warp: the numbers are labelled WARP-functional
// a device and a direct queue on the context's adapter (WARP with the flag)
bool probeDevice(const Context& ctx, Microsoft::WRL::ComPtr<ID3D12Device>& device, Microsoft::WRL::ComPtr<ID3D12CommandQueue>& queue, std::string& why);

}  // namespace pushmpc::selftest
