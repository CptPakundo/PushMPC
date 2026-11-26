#pragma once
// PushMPC: a D3D11 device next to pushcore's D3D12 graph, on the same adapter, for the vendor video methods that only
// exist as D3D11 APIs: Intel's video processor super resolution (PushCoreIntelVsr) and Intel VPL's AI frame
// interpolation (PushCoreVplFi, VPL on D3D11 video memory).
//
// What it shares: NT-handle textures (made on either side, opened on the other) and NT-handle fences in both directions:
//   D3D12 -> D3D11: the D3D12 queue signals a D3D12 fence, the bridge context waits on it (GPU side)
//   D3D11 -> D3D12: the bridge context signals a D3D11 fence, the D3D12 queue waits on it (GPU side)
//   another D3D11 device (the caller's) -> bridge: its shared D3D11 fence opened here, the bridge context waits on it.
// The bridge device is multithread-protected (the VPL worker thread and the submit thread both record on it).
// Win32 / D3D11.4 / D3D12; no dependency on PushFPS's engine.
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>

namespace pushmpc {

class D3D11Bridge {
public:
    // A D3D11 fence made on the bridge device and shared into D3D12 (bridge signals, D3D12 waits)
    struct Fence11 {
        Microsoft::WRL::ComPtr<ID3D11Fence> f11;
        Microsoft::WRL::ComPtr<ID3D12Fence> in12;
        uint64_t value = 0;
    };
    // A texture both sides see: res (D3D12) and tex (bridge D3D11); ext (optional) = the same texture opened on a third
    // D3D11 device (the caller's)
    struct Texture {
        Microsoft::WRL::ComPtr<ID3D12Resource> res;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> ext;
        uint32_t width = 0, height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        uint64_t releaseValue = 0;   // free-form: what its user waits for before writing it again
        bool held = false;
    };

    // d12 may be null (a D3D11-only user such as a self-test): then no D3D12 sharing is set up.
    bool init(ID3D12Device* d12, uint64_t adapterLuid, std::string& why);
    // WARP (tests): the bridge on the software adapter, with d12 a WARP D3D12 device
    bool initWarp(ID3D12Device* d12, std::string& why);
    bool ready() const { return dev_ != nullptr; }
    void reset();

    ID3D11Device5* device() const { return dev_.Get(); }
    ID3D11DeviceContext4* context() const { return ctx_.Get(); }
    ID3D11VideoDevice* videoDevice() const { return vdev_.Get(); }
    ID3D11VideoContext1* videoContext() const { return vctx_.Get(); }
    ID3D12Device* d12() const { return d12_.Get(); }
    uint32_t vendorId() const { return vendor_; }
    uint32_t deviceId() const { return deviceId_; }
    const std::wstring& adapterName() const { return adapterName_; }
    uint64_t adapterLuid() const { return luid_; }
    bool hasVideo() const { return vdev_ != nullptr; }
    const std::string& videoWhy() const { return videoWhy_; }   // why there is no video processor (empty when there is)

    // D3D12 -> bridge: the D3D12 fence is opened on the bridge device once per fence object
    bool importFence12(ID3D12Fence* f12, Microsoft::WRL::ComPtr<ID3D11Fence>& out, std::string& why);
    // another D3D11 device's shared fence (made with D3D11_FENCE_FLAG_SHARED) opened on the bridge device
    bool importFence11(ID3D11Fence* foreign, Microsoft::WRL::ComPtr<ID3D11Fence>& out, std::string& why);
    bool createFence11(Fence11& out, std::string& why);
    // bridge context: Signal(f, ++f.value), Flush; returns the value (0 on failure)
    uint64_t signal(Fence11& f);
    // bridge context waits on the GPU (value 0 = nothing to wait for)
    void wait(ID3D11Fence* f, uint64_t value) { if (f && value) ctx_->Wait(f, value); }
    // CPU wait for a bridge fence value (ms; false on timeout)
    bool cpuWait(Fence11& f, uint64_t value, uint32_t ms);

    // A texture made in D3D12 (default heap, shared, render target) and opened on the bridge device: RGBA-class formats
    bool createFrom12(uint32_t w, uint32_t h, DXGI_FORMAT format, Texture& out, std::string& why);
    // A texture made on the bridge device (NV12 / P010 / RGB, shared NT handle, bind flags as given) and opened in D3D12
    bool createFrom11(uint32_t w, uint32_t h, DXGI_FORMAT format, UINT bindFlags, Texture& out, std::string& why);
    // An existing D3D12 resource (made shared) opened on the bridge device
    bool open12(ID3D12Resource* r, Microsoft::WRL::ComPtr<ID3D11Texture2D>& out, std::string& why);
    // An existing D3D11 texture of another device (made with SHARED_NTHANDLE) opened on the bridge device
    bool open11(ID3D11Texture2D* foreign, Microsoft::WRL::ComPtr<ID3D11Texture2D>& out, std::string& why);
    // Opens the bridge texture on another D3D11 device (out.ext)
    bool openOn(ID3D11Device1* other, Texture& t, std::string& why);

private:
    bool initOn(IDXGIAdapter1* adapter, ID3D12Device* d12, std::string& why);
    Microsoft::WRL::ComPtr<ID3D11Device5> dev_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> ctx_;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> vdev_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext1> vctx_;
    Microsoft::WRL::ComPtr<ID3D12Device> d12_;
    HANDLE event_ = nullptr;
    uint32_t vendor_ = 0, deviceId_ = 0;
    uint64_t luid_ = 0;
    std::wstring adapterName_;
    std::string videoWhy_;
};

// A D3D11 video processor blit (stream 0 -> output) on the bridge device, with optional vendor extensions. Plain scaling
// and colour conversion as the driver's video processor does it.
class VideoBlit {
public:
    bool init(D3D11Bridge& b, uint32_t inW, uint32_t inH, DXGI_FORMAT inFormat, uint32_t outW, uint32_t outH, DXGI_FORMAT outFormat,
              std::string& why);
    // Intel's VPE interface (the same GUID / function numbers as VLC's d3d11 scaler and mpv's d3d11vpp use): version 3,
    // pre-processing mode, scaling mode super resolution on stream 0. Returns the first failing HRESULT (S_OK = on).
    HRESULT enableIntelSuperResolution(bool on, std::string& why);
    // colour spaces of the stream and the output (D3D11_VIDEO_PROCESSOR_COLOR_SPACE semantics, simplified)
    void setColor(bool yuvIn, bool bt709, bool limitedIn, bool yuvOut, bool limitedOut);
    // stream 0 = in[subresource], whole frame scaled into out (both on the bridge device)
    HRESULT blit(ID3D11Texture2D* in, uint32_t inArraySlice, ID3D11Texture2D* out);
    void reset();
    bool ready() const { return vp_ != nullptr; }
    uint32_t outWidth() const { return outW_; }
    uint32_t outHeight() const { return outH_; }

private:
    D3D11Bridge* b_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> en_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> vp_;
    // views are cached per texture (the rings are small and fixed)
    struct InView { ID3D11Texture2D* tex; uint32_t slice; Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> v; };
    struct OutView { ID3D11Texture2D* tex; Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> v; };
    InView inViews_[16]{}; OutView outViews_[16]{};
    uint32_t nIn_ = 0, nOut_ = 0;
    uint32_t inW_ = 0, inH_ = 0, outW_ = 0, outH_ = 0;
};

// The GUID of Intel's video pre-processing extension interface (public: Intel's driver extension, used by VLC, mpv,
// Chromium for Intel Video Super Resolution)
inline constexpr GUID kIntelVpeGuid = {0xedd1d4b9, 0x8659, 0x4cbc, {0xa4, 0xd6, 0x98, 0x31, 0xa2, 0x16, 0x3a, 0xc3}};

// The bridge's WARP round trip (tests): D3D12 fills a frame, the bridge's video processor copies it (plain blit, no
// extension), D3D12 reads it back; pixels compared. Returns a one-line verdict, ok = the round trip is exact enough.
std::string bridgeRoundTripTest(bool warp, uint64_t adapterLuid, bool& ok);

}  // namespace pushmpc
