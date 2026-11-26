#pragma once
// PushMPC: the anime upscalers (srMethod PUSHCORE_SR_METHOD_PMML, PUSHCORE_FEATURE_ANIME_SR) in the graph's RTX VSR slot
// (EnhanceGraphDesc::externalSr, patches/pushfps-4.5.4-zluda-externalsr.patch). ArtCNN (MIT), CuNNy (LGPL-3.0, a
// separate replaceable component: its weights file + graph), AnimeSR v2 (Apache-2.0) run through pmml (ONNX Runtime:
// TensorRT-RTX on NVIDIA RTX, DirectML on any DirectX 12 GPU) on the graph's encoded RGBA8 picture:
//   copyIn  records a copy of the picture into a readback buffer;
//   run     waits for it on the CPU (the graph's queue signals a fence), runs pmml::Upscaler (picture -> model ->
//           resized to the VSR output size) straight into an upload buffer;
//   copyOut records the copy of the upload buffer into the VSR output texture.
// Any failure makes it lost(): the graph resizes plainly from that frame on and pushcore runs RTX VSR / FSR from its
// next rebuild. Files: <pmml runtime>\graphs\sr_*.onnx and <pmml runtime>\graphs\anime-sr\ (Setup's
// anime-models-fetch.ps1, pinned by SHA-256). The pmml runtime: PMML_RUNTIME, HKCU\Software\PushMPC MlRuntime, else
// <install>\pmml next to pushcore's own folder.
#include "pushmpc/pushcore_abi.h"

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#if __has_include("veyra/pipeline/ExternalSr.h")
#include "veyra/pipeline/ExternalSr.h"
#define PUSHMPC_HAVE_EXTERNAL_SR 1
#else
#define PUSHMPC_HAVE_EXTERNAL_SR 0
#endif
#if defined(PUSHMPC_HAVE_PMML) && PUSHMPC_HAVE_PMML
#include "pmml/upscale.h"
#endif

namespace pushmpc {

// PUSHCORE_ANIME_SR_* -> pmml's model ids (pmml/src/upscale.cpp)
inline const char* animeSrModelId(uint32_t m) {
    static const char* ids[PUSHCORE_ANIME_SR_COUNT] = {"artcnn-c4f16", "artcnn-c4f16-ds", "artcnn-c4f16-dn", "artcnn-c4f32", "artcnn-c4f32-ds",
                                                       "artcnn-c4f32-dn", "artcnn-r8f64", "cunny-fast", "cunny-4x16", "cunny-8x32", "animesr-v2"};
    return m < PUSHCORE_ANIME_SR_COUNT ? ids[m] : ids[0];
}

// The pmml runtime folder (with ort\<flavor>\onnxruntime.dll), "" when there is none.
inline std::filesystem::path animeSrRuntime() {
    namespace fs = std::filesystem;
    auto ok = [](const fs::path& p) {
        std::error_code ec;
        return !p.empty() && (fs::exists(p / L"ort" / L"core" / L"onnxruntime.dll", ec) || fs::exists(p / L"ort" / L"directml" / L"onnxruntime.dll", ec));
    };
    if (const wchar_t* e = _wgetenv(L"PMML_RUNTIME"); e && *e && ok(e)) return e;
    wchar_t buf[1024]{};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\PushMPC", L"MlRuntime", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS && ok(buf)) return buf;
    HMODULE self = nullptr;   // <install>\core\pushcore.dll -> <install>\pmml
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&animeSrRuntime), &self)) {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(self, path, MAX_PATH);
        for (fs::path p = fs::path(path).parent_path(); !p.empty() && p != p.root_path(); p = p.parent_path())
            if (ok(p / L"pmml")) return p / L"pmml";
    }
    if (const wchar_t* la = _wgetenv(L"LOCALAPPDATA"); la && ok(fs::path(la) / L"PushMPC" / L"pmml")) return fs::path(la) / L"PushMPC" / L"pmml";
    return {};
}

// The backend pmml runs the upscaler on: TensorRT-RTX (NVIDIA RTX, its plugin EP installed), DirectML (any DirectX 12
// GPU, the directml flavor installed); "" = none (the CPU is never used for playback). PMML_ANIME_SR_BACKEND overrides.
inline std::string animeSrBackend(const std::filesystem::path& rt, bool nvidiaRtx) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (const char* e = std::getenv("PMML_ANIME_SR_BACKEND"); e && *e) return e;
    if (nvidiaRtx && fs::exists(rt / L"ort" / L"core" / L"onnxruntime.dll", ec) && fs::exists(rt / L"eps" / L"trt-rtx" / L"ep.json", ec)) return "ort:core:trt-rtx:0";
    if (fs::exists(rt / L"ort" / L"directml" / L"onnxruntime.dll", ec)) return "ort:directml:dml:0";
    return {};
}

// What is missing for this model ("" = it can run): the runtime, a GPU backend, pmml's graphs and the model files.
inline std::string animeSrMissing(uint32_t model, bool nvidiaRtx) {
#if !(defined(PUSHMPC_HAVE_PMML) && PUSHMPC_HAVE_PMML) || !PUSHMPC_HAVE_EXTERNAL_SR
    (void)model; (void)nvidiaRtx;
    return "this pushcore was built without pmml";
#else
    const auto rt = animeSrRuntime();
    if (rt.empty()) return "pmml's runtime is not installed (Setup: anime upscalers)";
    if (animeSrBackend(rt, nvidiaRtx).empty()) return "pmml's runtime has no GPU backend (TensorRT-RTX or DirectML)";
    pmml::UpscaleOptions o;
    o.graphsDir = (rt / L"graphs").string();
    o.model = animeSrModelId(model);
    o.io = pmml::UpscaleIo::Rgba8;
    const std::string miss = pmml::upscaleFilesMissing(o);
    return miss.empty() ? std::string() : "the anime upscaler files are not installed (" + miss + ")";
#endif
}

#if defined(PUSHMPC_HAVE_PMML) && PUSHMPC_HAVE_PMML && PUSHMPC_HAVE_EXTERNAL_SR
class AnimeSr final : public veyra::pipeline::ExternalSr {
public:
    AnimeSr(uint32_t model, bool nvidiaRtx) : model_(model), rtx_(nvidiaRtx) {}
    ~AnimeSr() override { shutdown(); }

    bool initialize(ID3D12Device* device, unsigned inW, unsigned inH, unsigned outW, unsigned outH, unsigned quality) override {
        (void)quality;
        shutdown();
        lost_ = false;
        const auto rt = animeSrRuntime();
        pmml::UpscaleOptions o;
        o.runtimeDir = rt.string();
        o.graphsDir = (rt / L"graphs").string();
        wchar_t la[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH)) o.cacheDir = (std::filesystem::path(la) / L"PushMPC" / L"pmml-cache").string();
        const std::string backend = animeSrBackend(rt, rtx_);
        if (rt.empty() || backend.empty() || !pmml::Backend::parse(backend, &o.backend)) { error_ = "no pmml runtime with a GPU backend"; return false; }
        o.model = animeSrModelId(model_);
        o.io = pmml::UpscaleIo::Rgba8;
        o.width = int(inW); o.height = int(inH); o.outWidth = int(outW); o.outHeight = int(outH);
        std::string err;
        up_ = pmml::Upscaler::create(o, &err);
        if (!up_) { error_ = err; return false; }
        inW_ = inW; inH_ = inH; outW_ = outW; outH_ = outH;
        auto buffer = [&](D3D12_HEAP_TYPE heap, UINT w, UINT h, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* fp, Microsoft::WRL::ComPtr<ID3D12Resource>* out) {
            D3D12_RESOURCE_DESC tex{};
            tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; tex.Width = w; tex.Height = h; tex.DepthOrArraySize = 1; tex.MipLevels = 1;
            tex.Format = DXGI_FORMAT_R8G8B8A8_UNORM; tex.SampleDesc.Count = 1;
            UINT64 bytes = 0;
            device->GetCopyableFootprints(&tex, 0, 1, 0, fp, nullptr, nullptr, &bytes);
            D3D12_HEAP_PROPERTIES hp{}; hp.Type = heap;
            D3D12_RESOURCE_DESC b{};
            b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; b.Width = bytes; b.Height = 1; b.DepthOrArraySize = 1; b.MipLevels = 1; b.SampleDesc.Count = 1;
            b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &b, heap == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_GENERIC_READ,
                                                             nullptr, IID_PPV_ARGS(out->GetAddressOf())));
        };
        if (!buffer(D3D12_HEAP_TYPE_READBACK, inW, inH, &inFp_, &readback_) || !buffer(D3D12_HEAP_TYPE_UPLOAD, outW, outH, &outFp_, &upload_)
            || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence_.GetAddressOf())))) {
            error_ = "D3D12 buffers for the anime upscaler could not be created";
            up_.reset();
            return false;
        }
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        describe_ = up_->describe();
        return event_ != nullptr;
    }

    bool copyIn(ID3D12GraphicsCommandList* list, ID3D12Resource* src) override {
        if (!up_) return false;
        D3D12_TEXTURE_COPY_LOCATION d{}, s{};
        d.pResource = readback_.Get(); d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; d.PlacedFootprint = inFp_;
        s.pResource = src; s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; s.SubresourceIndex = 0;
        list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        return true;
    }

    bool run(ID3D12CommandQueue* queue) override {
        if (!up_ || lost_) return false;
        const auto t0 = std::chrono::steady_clock::now();
        if (FAILED(queue->Signal(fence_.Get(), ++fenceValue_))) return loseWith("Signal failed");
        if (fence_->GetCompletedValue() < fenceValue_) {
            if (FAILED(fence_->SetEventOnCompletion(fenceValue_, event_))) return loseWith("SetEventOnCompletion failed");
            if (WaitForSingleObject(event_, 5000) != WAIT_OBJECT_0) return loseWith("the GPU did not finish the picture copy in 5 s");
        }
        void* in = nullptr;
        void* out = nullptr;
        const D3D12_RANGE all{0, SIZE_T(inFp_.Footprint.RowPitch) * inH_}, none{0, 0};
        if (FAILED(readback_->Map(0, &all, &in))) return loseWith("Map (readback) failed");
        if (FAILED(upload_->Map(0, &none, &out))) { readback_->Unmap(0, &none); return loseWith("Map (upload) failed"); }
        std::string err;
        const bool ok = up_->runRgba(static_cast<const uint8_t*>(in) + inFp_.Offset, inFp_.Footprint.RowPitch, static_cast<uint8_t*>(out) + outFp_.Offset,
                                     outFp_.Footprint.RowPitch, &err);
        readback_->Unmap(0, &none);
        upload_->Unmap(0, nullptr);
        if (!ok) return loseWith(err);
        lastMs_.store(float(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count()), std::memory_order_relaxed);
        return true;
    }

    bool copyOut(ID3D12GraphicsCommandList* list, ID3D12Resource* dst) override {
        D3D12_TEXTURE_COPY_LOCATION d{}, s{};
        s.pResource = upload_.Get(); s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; s.PlacedFootprint = outFp_;
        d.pResource = dst; d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; d.SubresourceIndex = 0;
        list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        return true;
    }

    bool lost() const override { return lost_; }
    const char* name() const override { return animeSrModelId(model_); }
    void shutdown() override {
        up_.reset();
        readback_.Reset(); upload_.Reset(); fence_.Reset();
        if (event_) { CloseHandle(event_); event_ = nullptr; }
    }
    // a cut or a seek: AnimeSR's carry starts again
    void reset() { if (up_) up_->reset(); }
    uint32_t model() const { return model_; }
    float lastMs() const { return lastMs_.load(std::memory_order_relaxed); }
    const std::string& error() const { return error_; }
    const std::string& describe() const { return describe_; }

private:
    bool loseWith(const std::string& why) { error_ = why; lost_ = true; return false; }
    uint32_t model_;
    bool rtx_;
    std::unique_ptr<pmml::Upscaler> up_;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback_, upload_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    HANDLE event_ = nullptr;
    UINT64 fenceValue_ = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT inFp_{}, outFp_{};
    unsigned inW_ = 0, inH_ = 0, outW_ = 0, outH_ = 0;
    std::atomic<bool> lost_{false};
    std::atomic<float> lastMs_{-1.f};
    std::string error_, describe_;
};
#endif

// Self-test PUSHCORE_SELFTEST_ANIME_SR (256): the model on this machine's backend on a synthetic 64x48 picture (a flat
// colour must come back as that colour at 128x96). No D3D12 device; seconds at most (a TensorRT-RTX engine build the
// first time). why = the reason either way.
inline bool animeSrSelfTest(uint32_t model, bool nvidiaRtx, std::string* why) {
    const std::string miss = animeSrMissing(model, nvidiaRtx);
    if (!miss.empty()) { if (why) *why = miss; return false; }
#if defined(PUSHMPC_HAVE_PMML) && PUSHMPC_HAVE_PMML
    const auto rt = animeSrRuntime();
    pmml::UpscaleOptions o;
    o.runtimeDir = rt.string();
    o.graphsDir = (rt / L"graphs").string();
    pmml::Backend::parse(animeSrBackend(rt, nvidiaRtx), &o.backend);
    o.model = animeSrModelId(model);
    o.io = pmml::UpscaleIo::Rgba8;
    o.width = 64; o.height = 48; o.outWidth = 128; o.outHeight = 96;
    std::string err;
    auto up = pmml::Upscaler::create(o, &err);
    if (!up) { if (why) *why = "pmml: " + err; return false; }
    std::vector<uint8_t> in(64 * 48 * 4), out(128 * 96 * 4);
    for (size_t i = 0; i < in.size(); i += 4) { in[i] = 180; in[i + 1] = 90; in[i + 2] = 40; in[i + 3] = 255; }
    if (!up->runRgba(in.data(), 64 * 4, out.data(), 128 * 4, &err)) { if (why) *why = "pmml run: " + err; return false; }
    int worst = 0;
    for (int y = 8; y < 88; ++y)
        for (int x = 8; x < 120; ++x)
            for (int c = 0; c < 3; ++c) worst = std::max(worst, std::abs(int(out[size_t(y) * 512 + x * 4 + c]) - int(in[size_t(c)])));
    if (why) *why = std::string(animeSrModelId(model)) + " on " + o.backend.key() + ": flat colour within " + std::to_string(worst) + " codes";
    return worst <= (model == PUSHCORE_ANIME_SR_ANIMESR_V2 ? 6 : 4);   // AnimeSR (a restoration GAN) redraws a little: 4 codes on the CPU EP
#else
    return false;
#endif
}

}  // namespace pushmpc
