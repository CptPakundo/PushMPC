#pragma once
// PushMPC: the anime upscalers (srMethod PUSHCORE_SR_METHOD_PMML, PUSHCORE_FEATURE_ANIME_SR) in the graph's RTX VSR slot
// (EnhanceGraphDesc::externalSr, patches/pushfps-4.5.4-zluda-externalsr.patch). ArtCNN (MIT), CuNNy (LGPL-3.0, a
// separate replaceable component: its weights file + graph), AnimeSR v2 (Apache-2.0) run through pmml (ONNX Runtime:
// TensorRT-RTX on NVIDIA RTX, DirectML on any DirectX 12 GPU) on the graph's encoded RGBA8 picture:
//   copyIn  records a copy of the picture into a buffer;
//   run     runs pmml::Upscaler (picture -> model -> resized to the VSR output size) into the output buffer;
//   copyOut records the copy of that buffer into the VSR output texture.
// Zero copy where the backend allows (PUSHMPC_ANIME_SR_ZERO_COPY=0 turns it off): DirectML runs on shared D3D12 buffers
// opened on pmml's device, ordered by a shared fence (no CPU wait); TensorRT-RTX runs on the same buffers imported into
// CUDA (external memory + the fence as an external semaphore). Otherwise, or when the interop cannot be set up: the
// host round trip (readback buffer, CPU wait, upload buffer).
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
#include <format>
#include <string>
#include <type_traits>
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
// CUDA driver API for the TensorRT-RTX zero copy (nvcuda.dll, loaded at run time; types from cuda.h when the tree has it)
#if __has_include(<cuda.h>)
#include <cuda.h>
#define PUSHMPC_ANIME_SR_CUDA 1
#else
#define PUSHMPC_ANIME_SR_CUDA 0
#endif

class AnimeSr final : public veyra::pipeline::ExternalSr {
public:
    AnimeSr(uint32_t model, bool nvidiaRtx) : model_(model), rtx_(nvidiaRtx) {}
    ~AnimeSr() override { shutdown(); }

    bool initialize(ID3D12Device* device, unsigned inW, unsigned inH, unsigned outW, unsigned outH, unsigned quality) override {
        (void)quality;
        shutdown();
        lost_ = false;
        device_ = device;
        // the row layout both ways (D3D12 copies want 256-byte row pitches): pmml crops / pads to it in its graphs
        auto footprint = [&](UINT w, UINT h, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* fp, UINT64* bytes) {
            D3D12_RESOURCE_DESC tex{};
            tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; tex.Width = w; tex.Height = h; tex.DepthOrArraySize = 1; tex.MipLevels = 1;
            tex.Format = DXGI_FORMAT_R8G8B8A8_UNORM; tex.SampleDesc.Count = 1;
            device->GetCopyableFootprints(&tex, 0, 1, 0, fp, nullptr, nullptr, bytes);
        };
        footprint(inW, inH, &inFp_, &inBytes_);
        footprint(outW, outH, &outFp_, &outBytes_);
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
        o.inPitchPx = int(inFp_.Footprint.RowPitch / 4); o.outPitchPx = int(outFp_.Footprint.RowPitch / 4);
        std::string err;
        up_ = pmml::Upscaler::create(o, &err);
        if (!up_) { error_ = err; return false; }
        inH_ = inH; outH_ = outH;
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(fence_.GetAddressOf())))) { error_ = "fence"; up_.reset(); return false; }
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        // zero copy where the backend can take GPU memory (PUSHMPC_ANIME_SR_ZERO_COPY=0: always the host round trip)
        const char* zc = std::getenv("PUSHMPC_ANIME_SR_ZERO_COPY");
        std::string why;
        if (zc && std::atoi(zc) == 0) why = "switched off (PUSHMPC_ANIME_SR_ZERO_COPY=0)";
        else if (up_->deviceKind() == "dml") mode_ = setupDml(&why) ? Mode::Dml : Mode::Host;
        else if (up_->deviceKind() == "cuda") mode_ = setupCuda(&why) ? Mode::Cuda : Mode::Host;
        else why = "the backend takes host memory only";
        if (mode_ == Mode::Host && !setupHost()) { error_ = "D3D12 buffers for the anime upscaler could not be created"; up_.reset(); return false; }
        const double mb = double(inBytes_ + outBytes_) / 1e6;
        describe_ = up_->describe() + (mode_ == Mode::Host ? std::format("; host round trip ({:.1f} MB a frame{}{})", mb, why.empty() ? "" : "; zero copy: ", why)
                                                         : std::format("; zero copy ({}): {:.1f} MB a frame stay on the GPU", mode_ == Mode::Dml ? "DirectML" : "CUDA", mb));
        return event_ != nullptr;
    }

    bool copyIn(ID3D12GraphicsCommandList* list, ID3D12Resource* src) override {
        if (!up_) return false;
        D3D12_TEXTURE_COPY_LOCATION d{}, s{};
        d.pResource = mode_ == Mode::Host ? readback_.Get() : sharedIn_.Get(); d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; d.PlacedFootprint = inFp_;
        s.pResource = src; s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; s.SubresourceIndex = 0;
        list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        return true;
    }

    bool run(ID3D12CommandQueue* queue) override {
        if (!up_ || lost_) return false;
        const auto t0 = std::chrono::steady_clock::now();
        if (FAILED(queue->Signal(fence_.Get(), ++fenceValue_))) return loseWith("Signal failed");
        std::string err;
        if (mode_ == Mode::Dml) {
            // GPU-side order only: pmml's queue waits for the copy in, the graph's queue for the result (no CPU wait)
            auto* q = static_cast<ID3D12CommandQueue*>(up_->dmlQueue());
            if (FAILED(q->Wait(dmlFence_.Get(), fenceValue_))) return loseWith("Wait (DirectML queue) failed");
            if (!up_->runRgbaDevice(dmlIn_, dmlOut_, &err)) return loseWith(err);
            if (FAILED(q->Signal(dmlFence_.Get(), ++fenceValue_))) return loseWith("Signal (DirectML queue) failed");
            if (FAILED(queue->Wait(fence_.Get(), fenceValue_))) return loseWith("Wait failed");
        }
#if PUSHMPC_ANIME_SR_CUDA
        else if (mode_ == Mode::Cuda) {
            cu_.push(cuCtx_);
            CUDA_EXTERNAL_SEMAPHORE_WAIT_PARAMS w{};
            w.params.fence.value = fenceValue_;
            bool ok = cu_.wait(&cuSem_, &w, 1, nullptr) == CUDA_SUCCESS && cu_.sync(nullptr) == CUDA_SUCCESS;
            if (ok) ok = up_->runRgbaDevice(reinterpret_cast<void*>(cuIn_), reinterpret_cast<void*>(cuOut_), &err);   // returns when written
            CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS sg{};
            sg.params.fence.value = ++fenceValue_;
            if (ok) ok = cu_.signal(&cuSem_, &sg, 1, nullptr) == CUDA_SUCCESS;
            CUcontext popped = nullptr;
            cu_.pop(&popped);
            if (!ok) return loseWith(err.empty() ? std::string("CUDA interop failed") : err);
            if (FAILED(queue->Wait(fence_.Get(), fenceValue_))) return loseWith("Wait failed");
        }
#endif
        else {
            if (fence_->GetCompletedValue() < fenceValue_) {
                if (FAILED(fence_->SetEventOnCompletion(fenceValue_, event_))) return loseWith("SetEventOnCompletion failed");
                if (WaitForSingleObject(event_, 5000) != WAIT_OBJECT_0) return loseWith("the GPU did not finish the picture copy in 5 s");
            }
            void* in = nullptr;
            void* out = nullptr;
            const D3D12_RANGE all{0, SIZE_T(inBytes_)}, none{0, 0};
            if (FAILED(readback_->Map(0, &all, &in))) return loseWith("Map (readback) failed");
            if (FAILED(upload_->Map(0, &none, &out))) { readback_->Unmap(0, &none); return loseWith("Map (upload) failed"); }
            const bool ok = up_->runRgba(static_cast<const uint8_t*>(in) + inFp_.Offset, inFp_.Footprint.RowPitch, static_cast<uint8_t*>(out) + outFp_.Offset,
                                         outFp_.Footprint.RowPitch, &err);
            readback_->Unmap(0, &none);
            upload_->Unmap(0, nullptr);
            if (!ok) return loseWith(err);
        }
        lastMs_.store(float(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count()), std::memory_order_relaxed);
        return true;
    }

    bool copyOut(ID3D12GraphicsCommandList* list, ID3D12Resource* dst) override {
        D3D12_TEXTURE_COPY_LOCATION d{}, s{};
        s.pResource = mode_ == Mode::Host ? upload_.Get() : sharedOut_.Get(); s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; s.PlacedFootprint = outFp_;
        d.pResource = dst; d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; d.SubresourceIndex = 0;
        list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        return true;
    }

    bool lost() const override { return lost_; }
    const char* name() const override { return animeSrModelId(model_); }
    void shutdown() override {
        if (up_) { up_->freeDml(dmlIn_); up_->freeDml(dmlOut_); }
        dmlIn_ = dmlOut_ = nullptr;
#if PUSHMPC_ANIME_SR_CUDA
        if (cu_.loaded()) {
            if (cuCtx_) cu_.push(cuCtx_);
            if (cuIn_) cu_.memFree(cuIn_);
            if (cuOut_) cu_.memFree(cuOut_);
            if (cuMemIn_) cu_.destroyMem(cuMemIn_);
            if (cuMemOut_) cu_.destroyMem(cuMemOut_);
            if (cuSem_) cu_.destroySem(cuSem_);
            CUcontext popped = nullptr;
            if (cuCtx_) { cu_.pop(&popped); cu_.releaseCtx(cuDev_); }
        }
        cuIn_ = cuOut_ = 0; cuMemIn_ = cuMemOut_ = nullptr; cuSem_ = nullptr; cuCtx_ = nullptr;
#endif
        up_.reset();
        readback_.Reset(); upload_.Reset(); sharedIn_.Reset(); sharedOut_.Reset(); fence_.Reset(); dmlInRes_.Reset(); dmlOutRes_.Reset(); dmlFence_.Reset();
        if (event_) { CloseHandle(event_); event_ = nullptr; }
        mode_ = Mode::Host;
    }
    // a cut or a seek: AnimeSR's carry starts again
    void reset() { if (up_) up_->reset(); }
    uint32_t model() const { return model_; }
    float lastMs() const { return lastMs_.load(std::memory_order_relaxed); }
    const std::string& error() const { return error_; }
    const std::string& describe() const { return describe_; }
    bool zeroCopy() const { return mode_ != Mode::Host; }

private:
    enum class Mode { Host, Dml, Cuda };
    bool loseWith(const std::string& why) { error_ = why; lost_ = true; return false; }

    bool committed(D3D12_HEAP_TYPE heap, D3D12_HEAP_FLAGS flags, D3D12_RESOURCE_FLAGS rf, UINT64 bytes, D3D12_RESOURCE_STATES state, Microsoft::WRL::ComPtr<ID3D12Resource>* out) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = heap;
        D3D12_RESOURCE_DESC b{};
        b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; b.Width = bytes; b.Height = 1; b.DepthOrArraySize = 1; b.MipLevels = 1; b.SampleDesc.Count = 1;
        b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; b.Flags = rf;
        return SUCCEEDED(device_->CreateCommittedResource(&hp, flags, &b, state, nullptr, IID_PPV_ARGS(out->GetAddressOf())));
    }
    bool setupHost() {
        return committed(D3D12_HEAP_TYPE_READBACK, D3D12_HEAP_FLAG_NONE, D3D12_RESOURCE_FLAG_NONE, inBytes_, D3D12_RESOURCE_STATE_COPY_DEST, &readback_)
            && committed(D3D12_HEAP_TYPE_UPLOAD, D3D12_HEAP_FLAG_NONE, D3D12_RESOURCE_FLAG_NONE, outBytes_, D3D12_RESOURCE_STATE_GENERIC_READ, &upload_);
    }
    // the two pictures as shared default-heap buffers (COMMON: copies and UAV access promote implicitly)
    bool sharedBuffers(std::string* why) {
        if (committed(D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_FLAG_SHARED, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, inBytes_, D3D12_RESOURCE_STATE_COMMON, &sharedIn_)
            && committed(D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_FLAG_SHARED, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, outBytes_, D3D12_RESOURCE_STATE_COMMON, &sharedOut_)) return true;
        *why = "shared D3D12 buffers could not be created";
        return false;
    }
    bool share(ID3D12DeviceChild* obj, HANDLE* h) { return SUCCEEDED(device_->CreateSharedHandle(obj, nullptr, GENERIC_ALL, nullptr, h)); }

    // DirectML: pmml's own device opens the buffers and the fence (same adapter only)
    bool setupDml(std::string* why) {
        auto* dev = static_cast<ID3D12Device*>(up_->dmlDevice());
        if (!dev) { *why = "pmml has no DirectML device"; return false; }
        if (dev->GetAdapterLuid().LowPart != device_->GetAdapterLuid().LowPart || dev->GetAdapterLuid().HighPart != device_->GetAdapterLuid().HighPart) { *why = "pmml's DirectML device is on another adapter"; return false; }
        if (!sharedBuffers(why)) return false;
        HANDLE hi = nullptr, ho = nullptr, hf = nullptr;
        bool ok = share(sharedIn_.Get(), &hi) && share(sharedOut_.Get(), &ho) && share(fence_.Get(), &hf)
               && SUCCEEDED(dev->OpenSharedHandle(hi, IID_PPV_ARGS(dmlInRes_.GetAddressOf()))) && SUCCEEDED(dev->OpenSharedHandle(ho, IID_PPV_ARGS(dmlOutRes_.GetAddressOf())))
               && SUCCEEDED(dev->OpenSharedHandle(hf, IID_PPV_ARGS(dmlFence_.GetAddressOf())));
        for (HANDLE h : {hi, ho, hf}) if (h) CloseHandle(h);
        if (!ok) { *why = "the shared buffers or fence did not open on pmml's device"; return false; }
        std::string e;
        dmlIn_ = up_->wrapDml(dmlInRes_.Get(), &e);
        dmlOut_ = dmlIn_ ? up_->wrapDml(dmlOutRes_.Get(), &e) : nullptr;
        if (!dmlIn_ || !dmlOut_) { *why = "DirectML allocation: " + e; return false; }
        return true;
    }

#if PUSHMPC_ANIME_SR_CUDA
    struct Cuda {
        HMODULE dll = nullptr;
        decltype(&cuInit) init = nullptr;
        decltype(&cuDeviceGetCount) count = nullptr;
        decltype(&cuDeviceGet) get = nullptr;
        decltype(&cuDeviceGetLuid) luid = nullptr;
        decltype(&cuDevicePrimaryCtxRetain) retain = nullptr;
        decltype(&cuDevicePrimaryCtxRelease) releaseCtx = nullptr;
        decltype(&cuCtxPushCurrent) push = nullptr;
        decltype(&cuCtxPopCurrent) pop = nullptr;
        decltype(&cuImportExternalMemory) importMem = nullptr;
        decltype(&cuExternalMemoryGetMappedBuffer) mapBuffer = nullptr;
        decltype(&cuDestroyExternalMemory) destroyMem = nullptr;
        decltype(&cuImportExternalSemaphore) importSem = nullptr;
        decltype(&cuWaitExternalSemaphoresAsync) wait = nullptr;
        decltype(&cuSignalExternalSemaphoresAsync) signal = nullptr;
        decltype(&cuDestroyExternalSemaphore) destroySem = nullptr;
        decltype(&cuStreamSynchronize) sync = nullptr;
        decltype(&cuMemFree) memFree = nullptr;
        bool loaded() const { return dll && init && sync && memFree; }
        bool load() {
            if (dll) return loaded();
            dll = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!dll) return false;
            auto f = [&](auto& p, const char* n) { p = reinterpret_cast<std::remove_reference_t<decltype(p)>>(GetProcAddress(dll, n)); return p != nullptr; };
            return f(init, "cuInit") && f(count, "cuDeviceGetCount") && f(get, "cuDeviceGet") && f(luid, "cuDeviceGetLuid") && f(retain, "cuDevicePrimaryCtxRetain")
                && f(releaseCtx, "cuDevicePrimaryCtxRelease_v2") && f(push, "cuCtxPushCurrent_v2") && f(pop, "cuCtxPopCurrent_v2") && f(importMem, "cuImportExternalMemory")
                && f(mapBuffer, "cuExternalMemoryGetMappedBuffer") && f(destroyMem, "cuDestroyExternalMemory") && f(importSem, "cuImportExternalSemaphore")
                && f(wait, "cuWaitExternalSemaphoresAsync") && f(signal, "cuSignalExternalSemaphoresAsync") && f(destroySem, "cuDestroyExternalSemaphore")
                && f(sync, "cuStreamSynchronize") && f(memFree, "cuMemFree_v2");
        }
    } cu_;
    CUdevice cuDev_ = 0;
    CUcontext cuCtx_ = nullptr;
    CUexternalMemory cuMemIn_ = nullptr, cuMemOut_ = nullptr;
    CUexternalSemaphore cuSem_ = nullptr;
    CUdeviceptr cuIn_ = 0, cuOut_ = 0;
#endif
    // CUDA (TensorRT-RTX): the shared buffers and fence imported into the primary context of the same adapter
    bool setupCuda(std::string* why) {
#if PUSHMPC_ANIME_SR_CUDA
        if (!cu_.load() || cu_.init(0) != CUDA_SUCCESS) { *why = "nvcuda.dll did not load"; return false; }
        int n = 0;
        cu_.count(&n);
        const LUID want = device_->GetAdapterLuid();
        bool found = false;
        for (int i = 0; i < n && !found; ++i) {
            char l[8]{};
            unsigned mask = 0;
            if (cu_.get(&cuDev_, i) == CUDA_SUCCESS && cu_.luid(l, &mask, cuDev_) == CUDA_SUCCESS && std::memcmp(l, &want, 8) == 0) found = true;
        }
        if (!found) { *why = "no CUDA device on this D3D12 adapter"; return false; }
        if (cu_.retain(&cuCtx_, cuDev_) != CUDA_SUCCESS) { *why = "CUDA context"; cuCtx_ = nullptr; return false; }
        if (!sharedBuffers(why)) return false;
        HANDLE hi = nullptr, ho = nullptr, hf = nullptr;
        bool ok = share(sharedIn_.Get(), &hi) && share(sharedOut_.Get(), &ho) && share(fence_.Get(), &hf);
        cu_.push(cuCtx_);
        auto import = [&](HANDLE h, UINT64 bytes, CUexternalMemory* mem, CUdeviceptr* ptr) {
            CUDA_EXTERNAL_MEMORY_HANDLE_DESC d{};
            d.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE; d.handle.win32.handle = h; d.size = bytes; d.flags = CUDA_EXTERNAL_MEMORY_DEDICATED;
            if (cu_.importMem(mem, &d) != CUDA_SUCCESS) return false;
            CUDA_EXTERNAL_MEMORY_BUFFER_DESC b{};
            b.offset = 0; b.size = bytes;
            return cu_.mapBuffer(ptr, *mem, &b) == CUDA_SUCCESS;
        };
        if (ok) ok = import(hi, inBytes_, &cuMemIn_, &cuIn_) && import(ho, outBytes_, &cuMemOut_, &cuOut_);
        if (ok) {
            CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC s{};
            s.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE; s.handle.win32.handle = hf;
            ok = cu_.importSem(&cuSem_, &s) == CUDA_SUCCESS;
        }
        CUcontext popped = nullptr;
        cu_.pop(&popped);
        for (HANDLE h : {hi, ho, hf}) if (h) CloseHandle(h);
        if (!ok) { *why = "CUDA could not import the shared buffers / fence"; return false; }
        return true;
#else
        *why = "this pushcore was built without the CUDA driver headers";
        return false;
#endif
    }

    uint32_t model_;
    bool rtx_;
    Mode mode_ = Mode::Host;
    ID3D12Device* device_ = nullptr;
    std::unique_ptr<pmml::Upscaler> up_;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback_, upload_, sharedIn_, sharedOut_, dmlInRes_, dmlOutRes_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_, dmlFence_;
    void* dmlIn_ = nullptr;
    void* dmlOut_ = nullptr;
    HANDLE event_ = nullptr;
    UINT64 fenceValue_ = 0, inBytes_ = 0, outBytes_ = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT inFp_{}, outFp_{};
    unsigned inH_ = 0, outH_ = 0;
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
