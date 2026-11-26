// PushMPC: the D3D11 <-> D3D12 bridge (see PushCoreD3D11Bridge.h).
#include "PushCoreD3D11Bridge.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace pushmpc {
namespace {
std::string hx(HRESULT hr) { return std::format("0x{:08X}", unsigned(hr)); }
uint64_t luidOf(const LUID& l) { return (uint64_t(uint32_t(l.HighPart)) << 32) | l.LowPart; }
}  // namespace

bool D3D11Bridge::init(ID3D12Device* d12, uint64_t adapterLuid, std::string& why)
{
    reset();
    ComPtr<IDXGIFactory4> f;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f)))) { why = "no DXGI factory"; return false; }
    LUID l{};
    if (!adapterLuid && d12) l = d12->GetAdapterLuid();
    else { l.LowPart = DWORD(adapterLuid & 0xFFFFFFFFu); l.HighPart = LONG(uint32_t(adapterLuid >> 32)); }
    ComPtr<IDXGIAdapter1> a;
    if (FAILED(f->EnumAdapterByLuid(l, IID_PPV_ARGS(&a)))) { why = std::format("adapter LUID {:016X} not found", luidOf(l)); return false; }
    return initOn(a.Get(), d12, why);
}

bool D3D11Bridge::initWarp(ID3D12Device* d12, std::string& why)
{
    reset();
    ComPtr<IDXGIFactory4> f;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f)))) { why = "no DXGI factory"; return false; }
    ComPtr<IDXGIAdapter1> a;
    if (FAILED(f->EnumWarpAdapter(IID_PPV_ARGS(&a)))) { why = "no WARP adapter"; return false; }
    return initOn(a.Get(), d12, why);
}

bool D3D11Bridge::initOn(IDXGIAdapter1* adapter, ID3D12Device* d12, std::string& why)
{
    DXGI_ADAPTER_DESC1 d{};
    adapter->GetDesc1(&d);
    vendor_ = d.VendorId; deviceId_ = d.DeviceId; luid_ = luidOf(d.AdapterLuid); adapterName_ = d.Description;
    if (d12 && luidOf(d12->GetAdapterLuid()) != luid_) { why = "the D3D12 device is on another adapter"; return false; }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   levels, UINT(std::size(levels)), D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) {
        // no video support on this adapter (WARP, some drivers): the bridge still shares textures and fences; the video
        // processor users then report that (VideoBlit::init)
        videoWhy_ = "D3D11CreateDevice(video support) failed " + hx(hr);
        hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, UINT(std::size(levels)),
                               D3D11_SDK_VERSION, &dev, nullptr, &ctx);
        if (FAILED(hr)) { why = "D3D11CreateDevice failed " + hx(hr); return false; }
    }
    if (FAILED(dev.As(&dev_)) || FAILED(ctx.As(&ctx_))) { why = "the D3D11 device lacks ID3D11Device5 / ID3D11DeviceContext4"; reset(); return false; }
    if (FAILED(dev.As(&vdev_)) || FAILED(ctx.As(&vctx_))) {
        vdev_.Reset(); vctx_.Reset();
        if (videoWhy_.empty()) videoWhy_ = "the D3D11 device has no video processor interfaces (ID3D11VideoContext1)";
    }
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(ctx.As(&mt))) mt->SetMultithreadProtected(TRUE);
    d12_ = d12;
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return true;
}

void D3D11Bridge::reset()
{
    if (ctx_) { ctx_->ClearState(); ctx_->Flush(); }
    videoWhy_.clear();
    vctx_.Reset(); vdev_.Reset(); ctx_.Reset(); dev_.Reset(); d12_.Reset();
    if (event_) { CloseHandle(event_); event_ = nullptr; }
}

bool D3D11Bridge::importFence12(ID3D12Fence* f12, ComPtr<ID3D11Fence>& out, std::string& why)
{
    if (!d12_ || !f12) { why = "no D3D12 device"; return false; }
    HANDLE h = nullptr;
    HRESULT hr = d12_->CreateSharedHandle(f12, nullptr, GENERIC_ALL, nullptr, &h);
    if (FAILED(hr)) { why = "D3D12 CreateSharedHandle(fence) " + hx(hr); return false; }
    hr = dev_->OpenSharedFence(h, IID_PPV_ARGS(&out));
    CloseHandle(h);
    if (FAILED(hr)) { why = "bridge OpenSharedFence(D3D12 fence) " + hx(hr); return false; }
    return true;
}

bool D3D11Bridge::importFence11(ID3D11Fence* foreign, ComPtr<ID3D11Fence>& out, std::string& why)
{
    HANDLE h = nullptr;
    HRESULT hr = foreign ? foreign->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h) : E_POINTER;
    if (FAILED(hr)) { why = "ID3D11Fence::CreateSharedHandle " + hx(hr); return false; }
    hr = dev_->OpenSharedFence(h, IID_PPV_ARGS(&out));
    CloseHandle(h);
    if (FAILED(hr)) { why = "bridge OpenSharedFence(D3D11 fence) " + hx(hr); return false; }
    return true;
}

bool D3D11Bridge::createFence11(Fence11& out, std::string& why)
{
    out = {};
    HRESULT hr = dev_->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&out.f11));
    if (FAILED(hr)) { why = "bridge CreateFence(shared) " + hx(hr); return false; }
    if (!d12_) return true;
    HANDLE h = nullptr;
    hr = out.f11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h);
    if (FAILED(hr)) { why = "bridge fence CreateSharedHandle " + hx(hr); return false; }
    hr = d12_->OpenSharedHandle(h, IID_PPV_ARGS(&out.in12));
    CloseHandle(h);
    if (FAILED(hr)) { why = "D3D12 OpenSharedHandle(bridge fence) " + hx(hr); return false; }
    return true;
}

uint64_t D3D11Bridge::signal(Fence11& f)
{
    if (!f.f11 || FAILED(ctx_->Signal(f.f11.Get(), f.value + 1))) return 0;
    ++f.value;
    ctx_->Flush();
    return f.value;
}

bool D3D11Bridge::cpuWait(Fence11& f, uint64_t value, uint32_t ms)
{
    if (!f.f11 || !value || f.f11->GetCompletedValue() >= value) return true;
    if (FAILED(f.f11->SetEventOnCompletion(value, event_))) return false;
    return WaitForSingleObject(event_, ms) == WAIT_OBJECT_0;
}

bool D3D11Bridge::createFrom12(uint32_t w, uint32_t h, DXGI_FORMAT format, Texture& out, std::string& why)
{
    out = {};
    if (!d12_) { why = "no D3D12 device"; return false; }
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1; td.Format = format; td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;   // D3D11 opens shared textures only with it
    HRESULT hr = d12_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &td, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&out.res));
    if (FAILED(hr)) { why = std::format("D3D12 shared texture {}x{} format {} {}", w, h, int(format), hx(hr)); return false; }
    if (!open12(out.res.Get(), out.tex, why)) { out = {}; return false; }
    out.width = w; out.height = h; out.format = format;
    return true;
}

bool D3D11Bridge::createFrom11(uint32_t w, uint32_t h, DXGI_FORMAT format, UINT bindFlags, Texture& out, std::string& why)
{
    out = {};
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = format; d.SampleDesc = {1, 0};
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = bindFlags;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    HRESULT hr = dev_->CreateTexture2D(&d, nullptr, &out.tex);
    if (FAILED(hr)) { why = std::format("bridge shared texture {}x{} format {} bind 0x{:X} {}", w, h, int(format), bindFlags, hx(hr)); return false; }
    out.width = w; out.height = h; out.format = format;
    if (!d12_) return true;
    ComPtr<IDXGIResource1> r; HANDLE hd = nullptr;
    hr = out.tex.As(&r);
    if (SUCCEEDED(hr)) hr = r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &hd);
    if (FAILED(hr)) { why = "bridge texture CreateSharedHandle " + hx(hr); out = {}; return false; }
    hr = d12_->OpenSharedHandle(hd, IID_PPV_ARGS(&out.res));
    CloseHandle(hd);
    if (FAILED(hr)) { why = "D3D12 OpenSharedHandle(bridge texture) " + hx(hr); out = {}; return false; }
    return true;
}

bool D3D11Bridge::open12(ID3D12Resource* r, ComPtr<ID3D11Texture2D>& out, std::string& why)
{
    HANDLE h = nullptr;
    HRESULT hr = d12_ ? d12_->CreateSharedHandle(r, nullptr, GENERIC_ALL, nullptr, &h) : E_POINTER;
    if (FAILED(hr)) { why = "D3D12 CreateSharedHandle(texture) " + hx(hr); return false; }
    hr = dev_->OpenSharedResource1(h, IID_PPV_ARGS(&out));
    CloseHandle(h);
    if (FAILED(hr)) { why = "bridge OpenSharedResource1(D3D12 texture) " + hx(hr); return false; }
    return true;
}

bool D3D11Bridge::open11(ID3D11Texture2D* foreign, ComPtr<ID3D11Texture2D>& out, std::string& why)
{
    ComPtr<IDXGIResource1> r; HANDLE h = nullptr;
    HRESULT hr = foreign ? foreign->QueryInterface(IID_PPV_ARGS(&r)) : E_POINTER;
    if (SUCCEEDED(hr)) hr = r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h);
    if (FAILED(hr)) hr = r ? r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &h) : hr;
    if (FAILED(hr)) { why = "D3D11 texture CreateSharedHandle " + hx(hr); return false; }
    hr = dev_->OpenSharedResource1(h, IID_PPV_ARGS(&out));
    CloseHandle(h);
    if (FAILED(hr)) { why = "bridge OpenSharedResource1(D3D11 texture) " + hx(hr); return false; }
    return true;
}

bool D3D11Bridge::openOn(ID3D11Device1* other, Texture& t, std::string& why)
{
    ComPtr<IDXGIResource1> r; HANDLE h = nullptr;
    HRESULT hr = t.tex.As(&r);
    if (SUCCEEDED(hr)) hr = r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h);
    if (FAILED(hr) && t.res && d12_) hr = d12_->CreateSharedHandle(t.res.Get(), nullptr, GENERIC_ALL, nullptr, &h);
    if (FAILED(hr)) { why = "shared handle for the caller's device " + hx(hr); return false; }
    hr = other->OpenSharedResource1(h, IID_PPV_ARGS(&t.ext));
    CloseHandle(h);
    if (FAILED(hr)) { why = "caller OpenSharedResource1(bridge texture) " + hx(hr); return false; }
    return true;
}

// ------------------------------------------------------------------------------------------------ video processor
bool VideoBlit::init(D3D11Bridge& b, uint32_t inW, uint32_t inH, DXGI_FORMAT inFormat, uint32_t outW, uint32_t outH, DXGI_FORMAT outFormat,
                     std::string& why)
{
    reset();
    b_ = &b;
    if (!b.videoDevice()) { why = "no video processor on this device (" + b.videoWhy() + ")"; return false; }
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = inW; cd.InputHeight = inH; cd.OutputWidth = outW; cd.OutputHeight = outH;
    cd.InputFrameRate = {24000, 1001}; cd.OutputFrameRate = {24000, 1001};
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_QUALITY;
    HRESULT hr = b.videoDevice()->CreateVideoProcessorEnumerator(&cd, &en_);
    if (FAILED(hr)) { why = "CreateVideoProcessorEnumerator " + std::format("0x{:08X}", unsigned(hr)); return false; }
    UINT fs = 0;
    if (FAILED(en_->CheckVideoProcessorFormat(inFormat, &fs)) || !(fs & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) {
        why = std::format("the video processor does not take format {} as input", int(inFormat)); reset(); return false;
    }
    fs = 0;
    if (FAILED(en_->CheckVideoProcessorFormat(outFormat, &fs)) || !(fs & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
        why = std::format("the video processor does not write format {}", int(outFormat)); reset(); return false;
    }
    hr = b.videoDevice()->CreateVideoProcessor(en_.Get(), 0, &vp_);
    if (FAILED(hr)) { why = "CreateVideoProcessor " + std::format("0x{:08X}", unsigned(hr)); reset(); return false; }
    inW_ = inW; inH_ = inH; outW_ = outW; outH_ = outH;
    auto* v = b.videoContext();
    const RECT src{0, 0, LONG(inW), LONG(inH)}, dst{0, 0, LONG(outW), LONG(outH)};
    v->VideoProcessorSetStreamFrameFormat(vp_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    v->VideoProcessorSetStreamSourceRect(vp_.Get(), 0, TRUE, &src);
    v->VideoProcessorSetStreamDestRect(vp_.Get(), 0, TRUE, &dst);
    v->VideoProcessorSetOutputTargetRect(vp_.Get(), TRUE, &dst);
    v->VideoProcessorSetStreamAutoProcessingMode(vp_.Get(), 0, FALSE);   // no driver "enhancements" besides what we ask for
    return true;
}

void VideoBlit::setColor(bool yuvIn, bool bt709, bool limitedIn, bool yuvOut, bool limitedOut)
{
    if (!vp_) return;
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{}, out{};
    in.Usage = 0; in.RGB_Range = limitedIn && !yuvIn ? 1 : 0; in.YCbCr_Matrix = bt709 ? 1 : 0; in.Nominal_Range = limitedIn ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235 : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    out.Usage = 0; out.RGB_Range = limitedOut && !yuvOut ? 1 : 0; out.YCbCr_Matrix = bt709 ? 1 : 0; out.Nominal_Range = limitedOut ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235 : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    b_->videoContext()->VideoProcessorSetStreamColorSpace(vp_.Get(), 0, &in);
    b_->videoContext()->VideoProcessorSetOutputColorSpace(vp_.Get(), &out);
}

HRESULT VideoBlit::enableIntelSuperResolution(bool on, std::string& why)
{
    if (!vp_) { why = "no video processor"; return E_FAIL; }
    // Intel VPE extension call: {function, pointer to its UINT parameter}
    struct Ext { UINT function; void* param; };
    UINT param = 0;
    Ext ext{0, &param};
    GUID g = kIntelVpeGuid;
    auto* v = b_->videoContext();
    ext.function = 0x01; param = 0x0003;                      // version 3
    HRESULT hr = v->VideoProcessorSetOutputExtension(vp_.Get(), &g, sizeof(ext), &ext);
    if (FAILED(hr)) { why = std::format("Intel VPE version call failed 0x{:08X} (the driver has no Intel video extension)", unsigned(hr)); return hr; }
    ext.function = 0x20; param = on ? 0x1u : 0x0u;          // mode: pre-processing / none
    hr = v->VideoProcessorSetOutputExtension(vp_.Get(), &g, sizeof(ext), &ext);
    if (FAILED(hr)) { why = std::format("Intel VPE mode call failed 0x{:08X}", unsigned(hr)); return hr; }
    ext.function = 0x37; param = on ? 0x2u : 0x0u;          // scaling: super resolution / default
    hr = v->VideoProcessorSetStreamExtension(vp_.Get(), 0, &g, sizeof(ext), &ext);
    if (FAILED(hr)) { why = std::format("Intel VPE scaling call failed 0x{:08X} (no video super resolution in this driver)", unsigned(hr)); return hr; }
    return S_OK;
}

HRESULT VideoBlit::blit(ID3D11Texture2D* in, uint32_t inArraySlice, ID3D11Texture2D* out)
{
    if (!vp_ || !in || !out) return E_POINTER;
    ID3D11VideoProcessorInputView* iv = nullptr;
    for (uint32_t i = 0; i < nIn_; ++i) if (inViews_[i].tex == in && inViews_[i].slice == inArraySlice) iv = inViews_[i].v.Get();
    if (!iv) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC d{};
        d.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D; d.Texture2D.ArraySlice = inArraySlice;
        ComPtr<ID3D11VideoProcessorInputView> v;
        const HRESULT hr = b_->videoDevice()->CreateVideoProcessorInputView(in, en_.Get(), &d, &v);
        if (FAILED(hr)) return hr;
        if (nIn_ == std::size(inViews_)) { for (auto& e : inViews_) e = {}; nIn_ = 0; }
        inViews_[nIn_] = {in, inArraySlice, v};
        iv = inViews_[nIn_++].v.Get();
    }
    ID3D11VideoProcessorOutputView* ov = nullptr;
    for (uint32_t i = 0; i < nOut_; ++i) if (outViews_[i].tex == out) ov = outViews_[i].v.Get();
    if (!ov) {
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC d{};
        d.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11VideoProcessorOutputView> v;
        const HRESULT hr = b_->videoDevice()->CreateVideoProcessorOutputView(out, en_.Get(), &d, &v);
        if (FAILED(hr)) return hr;
        if (nOut_ == std::size(outViews_)) { for (auto& e : outViews_) e = {}; nOut_ = 0; }
        outViews_[nOut_] = {out, v};
        ov = outViews_[nOut_++].v.Get();
    }
    D3D11_VIDEO_PROCESSOR_STREAM s{};
    s.Enable = TRUE; s.pInputSurface = iv;
    return b_->videoContext()->VideoProcessorBlt(vp_.Get(), ov, 0, 1, &s);
}

void VideoBlit::reset()
{
    for (auto& e : inViews_) e = {};
    for (auto& e : outViews_) e = {};
    nIn_ = nOut_ = 0;
    vp_.Reset(); en_.Reset();
}

// ------------------------------------------------------------------------------------------------ WARP round trip test
namespace {
struct Mini12 {
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> q;
    ComPtr<ID3D12CommandAllocator> a;
    ComPtr<ID3D12GraphicsCommandList> l;
    ComPtr<ID3D12Fence> f;
    uint64_t v = 0;
    HANDLE e = nullptr;
    ~Mini12() { if (e) CloseHandle(e); }
    bool init(bool warp, uint64_t luid, std::string& why)
    {
        ComPtr<IDXGIFactory4> fac;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac)))) { why = "no DXGI factory"; return false; }
        ComPtr<IDXGIAdapter1> ad;
        if (warp) fac->EnumWarpAdapter(IID_PPV_ARGS(&ad));
        else { LUID l{}; l.LowPart = DWORD(luid); l.HighPart = LONG(uint32_t(luid >> 32)); fac->EnumAdapterByLuid(l, IID_PPV_ARGS(&ad)); }
        if (!ad) { why = "adapter not found"; return false; }
        HRESULT hr = D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
        if (FAILED(hr)) { why = "D3D12CreateDevice " + hx(hr); return false; }
        D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) || FAILED(dev->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(&a))) ||
            FAILED(dev->CreateCommandList(0, qd.Type, a.Get(), nullptr, IID_PPV_ARGS(&l))) ||
            FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&f)))) { why = "D3D12 queue / list / fence"; return false; }
        l->Close();
        e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return true;
    }
    void begin() { a->Reset(); l->Reset(a.Get(), nullptr); }
    uint64_t submit() { l->Close(); ID3D12CommandList* ls[] = {l.Get()}; q->ExecuteCommandLists(1, ls); q->Signal(f.Get(), ++v); return v; }
    void cpuWait(uint64_t value) { if (f->GetCompletedValue() < value) { f->SetEventOnCompletion(value, e); WaitForSingleObject(e, 20000); } }
    ComPtr<ID3D12Resource> buffer(uint64_t size, D3D12_HEAP_TYPE t)
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = t;
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, t == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
                                     nullptr, IID_PPV_ARGS(&r));
        return r;
    }
    void barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES a0, D3D12_RESOURCE_STATES b0)
    {
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a0, b0};
        l->ResourceBarrier(1, &b);
    }
};
}  // namespace

std::string bridgeRoundTripTest(bool warp, uint64_t adapterLuid, bool& ok)
{
    ok = false;
    std::string why;
    Mini12 m;
    if (!m.init(warp, adapterLuid, why)) return "bridge test: " + why;
    D3D11Bridge b;
    if (!(warp ? b.initWarp(m.dev.Get(), why) : b.init(m.dev.Get(), adapterLuid, why))) return "bridge test: " + why;
    D3D11Bridge::Fence11 bf;
    ComPtr<ID3D11Fence> f12in11;
    if (!b.createFence11(bf, why) || !b.importFence12(m.f.Get(), f12in11, why)) return "bridge test: " + why;

    // 1) D3D12 -> bridge VP plain blit (RGBA 1:1) -> D3D12, exact compare
    const uint32_t W = 256, H = 144;
    D3D11Bridge::Texture a, c;
    if (!b.createFrom12(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, a, why) || !b.createFrom12(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, c, why)) return "bridge test: " + why;
    std::vector<uint8_t> px(size_t(W) * H * 4);
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            uint8_t* p = &px[(size_t(y) * W + x) * 4];
            p[0] = uint8_t(x); p[1] = uint8_t(y * 255 / (H - 1)); p[2] = uint8_t((x ^ y) & 0xFF); p[3] = 255;
        }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 total = 0;
    const D3D12_RESOURCE_DESC ad = a.res->GetDesc();
    m.dev->GetCopyableFootprints(&ad, 0, 1, 0, &fp, nullptr, nullptr, &total);
    auto up = m.buffer(total, D3D12_HEAP_TYPE_UPLOAD), rb = m.buffer(total, D3D12_HEAP_TYPE_READBACK);
    if (!up || !rb) return "bridge test: buffers";
    { uint8_t* d = nullptr; up->Map(0, nullptr, reinterpret_cast<void**>(&d)); for (uint32_t y = 0; y < H; ++y) memcpy(d + fp.Offset + size_t(y) * fp.Footprint.RowPitch, &px[size_t(y) * W * 4], W * 4); up->Unmap(0, nullptr); }
    m.begin();
    m.barrier(a.res.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst{a.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}}, src{up.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    dst.SubresourceIndex = 0; src.PlacedFootprint = fp;
    m.l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    m.barrier(a.res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    const uint64_t v1 = m.submit();
    // with a video processor (any real GPU): its plain blit; without one (WARP has no D3D11 video): a D3D11 copy, so the
    // sharing and both fence directions are still exercised
    const bool vp = b.hasVideo();
    VideoBlit vb;
    if (vp && !vb.init(b, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, why)) return "bridge test: VP " + why;
    if (vp) vb.setColor(false, true, false, false, false);
    b.wait(f12in11.Get(), v1);
    HRESULT hr = vp ? vb.blit(a.tex.Get(), 0, c.tex.Get()) : S_OK;
    if (!vp) b.context()->CopyResource(c.tex.Get(), a.tex.Get());
    if (FAILED(hr)) return "bridge test: VideoProcessorBlt " + hx(hr);
    std::string ext = vp ? std::string() : "no video processor (" + b.videoWhy() + ")";
    const HRESULT ivsr = vp ? vb.enableIntelSuperResolution(true, ext) : E_NOTIMPL;   // on WARP / non-Intel: must fail cleanly (reported only)
    const uint64_t w1 = b.signal(bf);
    m.q->Wait(bf.in12.Get(), w1);
    m.begin();
    m.barrier(c.res.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION rd{rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}}, rs{c.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    rd.PlacedFootprint = fp; rs.SubresourceIndex = 0;
    m.l->CopyTextureRegion(&rd, 0, 0, 0, &rs, nullptr);
    m.barrier(c.res.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    m.cpuWait(m.submit());
    int maxDiff = 0; uint64_t diffs = 0;
    { uint8_t* d = nullptr; rb->Map(0, nullptr, reinterpret_cast<void**>(&d));
      for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < W * 4; ++x) {
          if ((x & 3) == 3) continue;
          const int e = std::abs(int(d[fp.Offset + size_t(y) * fp.Footprint.RowPitch + x]) - int(px[size_t(y) * W * 4 + x]));
          maxDiff = std::max(maxDiff, e); diffs += e > 1;
      }
      rb->Unmap(0, nullptr); }

    // 2) NV12 on the bridge -> VP 2x upscale into an NV12 texture made on the bridge and opened in D3D12 (the Intel VSR
    //    path without the extension) -> D3D12 readback of the luma plane, against a CPU bilinear upscale
    const uint32_t sw = 320, sh = 180, dw = vp ? 640 : sw, dh = vp ? 360 : sh;
    std::vector<uint8_t> nv(size_t(sw) * sh * 3 / 2);
    for (uint32_t y = 0; y < sh; ++y) for (uint32_t x = 0; x < sw; ++x)
        nv[size_t(y) * sw + x] = uint8_t(16 + 219.0 * (0.5 + 0.5 * std::sin(x * 0.07) * std::cos(y * 0.05)));
    std::fill(nv.begin() + size_t(sw) * sh, nv.end(), uint8_t(128));
    D3D11_TEXTURE2D_DESC sd{}; sd.Width = sw; sd.Height = sh; sd.MipLevels = 1; sd.ArraySize = 1; sd.Format = DXGI_FORMAT_NV12; sd.SampleDesc = {1, 0};
    sd.Usage = D3D11_USAGE_DEFAULT; sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{nv.data(), sw, 0};
    ComPtr<ID3D11Texture2D> nsrc;
    hr = b.device()->CreateTexture2D(&sd, &init, &nsrc);
    if (FAILED(hr)) return "bridge test: NV12 source " + hx(hr);
    D3D11Bridge::Texture ndst;
    if (!b.createFrom11(dw, dh, DXGI_FORMAT_NV12, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, ndst, why)) return "bridge test: " + why;
    VideoBlit up2;
    if (vp) {
        if (!up2.init(b, sw, sh, DXGI_FORMAT_NV12, dw, dh, DXGI_FORMAT_NV12, why)) return "bridge test: VP NV12 " + why;
        up2.setColor(true, true, true, true, true);
        hr = up2.blit(nsrc.Get(), 0, ndst.tex.Get());
        if (FAILED(hr)) return "bridge test: NV12 VideoProcessorBlt " + hx(hr);
    } else {
        b.context()->CopyResource(ndst.tex.Get(), nsrc.Get());
    }
    const uint64_t w2 = b.signal(bf);
    m.q->Wait(bf.in12.Get(), w2);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT nfp[2]{}; UINT64 ntotal = 0;
    const D3D12_RESOURCE_DESC nd = ndst.res->GetDesc();
    m.dev->GetCopyableFootprints(&nd, 0, 2, 0, nfp, nullptr, nullptr, &ntotal);
    auto nrb = m.buffer(ntotal, D3D12_HEAP_TYPE_READBACK);
    m.begin();
    m.barrier(ndst.res.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION nrd{nrb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}}, nrs{ndst.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    nrd.PlacedFootprint = nfp[0]; nrs.SubresourceIndex = 0;
    m.l->CopyTextureRegion(&nrd, 0, 0, 0, &nrs, nullptr);
    m.barrier(ndst.res.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    m.cpuWait(m.submit());
    double se = 0; double mean = 0;
    { uint8_t* d = nullptr; nrb->Map(0, nullptr, reinterpret_cast<void**>(&d));
      for (uint32_t y = 0; y < dh; ++y) for (uint32_t x = 0; x < dw; ++x) {
          const double fx = (x + 0.5) * sw / dw - 0.5, fy = (y + 0.5) * sh / dh - 0.5;
          const int x0 = std::clamp(int(std::floor(fx)), 0, int(sw) - 1), y0 = std::clamp(int(std::floor(fy)), 0, int(sh) - 1);
          const int x1 = std::min(x0 + 1, int(sw) - 1), y1 = std::min(y0 + 1, int(sh) - 1);
          const double ax = std::clamp(fx - x0, 0.0, 1.0), ay = std::clamp(fy - y0, 0.0, 1.0);
          auto s = [&](int xx, int yy) { return double(nv[size_t(yy) * sw + xx]); };
          const double ref = (s(x0, y0) * (1 - ax) + s(x1, y0) * ax) * (1 - ay) + (s(x0, y1) * (1 - ax) + s(x1, y1) * ax) * ay;
          const double got = d[nfp[0].Offset + size_t(y) * nfp[0].Footprint.RowPitch + x];
          se += (got - ref) * (got - ref); mean += got;
      }
      nrb->Unmap(0, nullptr); }
    const double mse = se / (double(dw) * dh), psnr = mse > 0 ? 10 * std::log10(255.0 * 255.0 / mse) : 99.0;
    mean /= double(dw) * dh;
    ok = maxDiff <= 2 && psnr >= 30.0 && mean > 30;
    std::wstring an = b.adapterName();
    std::string name(an.size(), 0x3F);
    for (size_t i = 0; i < an.size(); ++i) if (an[i] < 128) name[i] = char(an[i]);
    const char* how = vp ? "video processor blit" : "D3D11 copy (no video processor on this adapter)";
    return std::format("bridge test on {}: RGBA D3D12 -> bridge {} -> D3D12 max diff {} ({} values > 1); NV12 {}x{} -> {}x{} on the bridge, "
                       "opened in D3D12: luma PSNR {:.1f} dB vs bilinear, mean {:.0f}; Intel VPE extension: {}; {}",
                       name, how, maxDiff, diffs, sw, sh, dw, dh, psnr, mean, SUCCEEDED(ivsr) ? "accepted" : ext, ok ? "PASS" : "FAIL");
}

}  // namespace pushmpc
