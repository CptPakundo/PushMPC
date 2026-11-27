// pushcore_route.cpp: see pushcore_route.h. Windows only (MSVC).
#include "pushcore_route.h"

#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <memory>
#include <cstdio>
#include <cstring>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

#include "pm_pushcore.h"
#include "../../../mpvnative/engine/d3d11_content.h"   // ContentDetect's GPU tap and its detector thread

using Microsoft::WRL::ComPtr;

namespace pmk {

namespace {

std::wstring AddonDir()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&AddonDir), &self);
    wchar_t p[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(self, p, DWORD(std::size(p)));
    std::wstring s(p, n);
    const size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : s.substr(0, slash);
}

std::wstring EnvW(const wchar_t* name)
{
    wchar_t b[MAX_PATH * 2] = {};
    const DWORD n = GetEnvironmentVariableW(name, b, DWORD(std::size(b)));
    return n && n < std::size(b) ? std::wstring(b, n) : std::wstring();
}

// pushcore.dll's folder: $PUSHMPC_CORE_DIR (tests), else the PushMPC install
std::wstring CoreDir()
{
    std::wstring d = EnvW(L"PUSHMPC_CORE_DIR");
    if (!d.empty()) return d;
    const std::wstring la = EnvW(L"LOCALAPPDATA");
    return la.empty() ? std::wstring() : la + L"\\PushMPC\\core";
}

bool Exists(const std::wstring& p) { return !p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

struct Bridge {
    HMODULE dll = nullptr;
    pm_pc_open_d3d11_fn open = nullptr;
    pm_pc_push_d3d11_fn push = nullptr;
    pm_pc_poll_d3d11_fn poll = nullptr;
    pm_pc_flush_fn flush = nullptr;
    pm_pc_close_fn close = nullptr;
    pm_pc_describe_fn describe = nullptr;
    // ABI 3 (absent in an older bridge: the source size only)
    pm_pc_update_fn update = nullptr;
    pm_pc_output_fn output = nullptr;
    uint32_t abi = 2;
    std::string why;
    bool Load()
    {
        if (dll) return open != nullptr;
        const std::wstring p = AddonDir() + L"\\pm_pushcore.dll";
        if (!Exists(p)) { why = "pm_pushcore.dll is not in the add-on folder"; return false; }
        dll = LoadLibraryExW(p.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!dll) { why = "pm_pushcore.dll does not load (error " + std::to_string(GetLastError()) + ")"; return false; }
        open = reinterpret_cast<pm_pc_open_d3d11_fn>(GetProcAddress(dll, "pm_pc_open_d3d11"));
        push = reinterpret_cast<pm_pc_push_d3d11_fn>(GetProcAddress(dll, "pm_pc_push_d3d11"));
        poll = reinterpret_cast<pm_pc_poll_d3d11_fn>(GetProcAddress(dll, "pm_pc_poll_d3d11"));
        flush = reinterpret_cast<pm_pc_flush_fn>(GetProcAddress(dll, "pm_pc_flush"));
        close = reinterpret_cast<pm_pc_close_fn>(GetProcAddress(dll, "pm_pc_close"));
        describe = reinterpret_cast<pm_pc_describe_fn>(GetProcAddress(dll, "pm_pc_describe"));
        if (!open || !push || !poll || !flush || !close || !describe) { why = "pm_pushcore.dll is incomplete"; open = nullptr; return false; }
        const auto abiFn = reinterpret_cast<pm_pc_abi_fn>(GetProcAddress(dll, "pm_pc_abi"));
        update = reinterpret_cast<pm_pc_update_fn>(GetProcAddress(dll, "pm_pc_update"));
        output = reinterpret_cast<pm_pc_output_fn>(GetProcAddress(dll, "pm_pc_output"));
        abi = abiFn && update && output ? std::min<uint32_t>(abiFn(), PM_PC_ABI) : 2u;
        return true;
    }
};
Bridge& TheBridge() { static Bridge b; return b; }   // never unloaded: pushcore stays warm for the next file

struct Lock {   // the device's context lock (FFmpeg's D3D11VA lock, also handed to the bridge)
    HANDLE m;
    explicit Lock(HANDLE h) : m(h) { if (m) WaitForSingleObjectEx(m, INFINITE, FALSE); }
    ~Lock() { if (m) ReleaseMutex(m); }
};

} // namespace

std::function<void(const std::string&)>& LogFn() { static std::function<void(const std::string&)> f; return f; }

struct PushcoreRoute::Impl {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    HANDLE mutex = nullptr;
    bool ownMutex = false;
    AVBufferRef* hw = nullptr;          // FFmpeg D3D11VA device on dev
    pm_pc* pc = nullptr;
    std::vector<ComPtr<ID3D11Texture2D>> ring;   // NV12 / P010 textures at the output size (one each: WARP and some drivers refuse larger NV12 arrays)
    unsigned slots = 0, next = 0;
    // AllOut: the decoded frames at the source size (slot ids slots + i), shown only where pushcore missed a picture
    std::vector<ComPtr<ID3D11Texture2D>> srcRing;
    unsigned srcNext = 0;
    ComPtr<ID3D11Texture2D> upStage, downStage, downSrc;
    DXGI_FORMAT fmt = DXGI_FORMAT_NV12;
    // Collect's routing
    const std::vector<int64_t>* want = nullptr;
    std::vector<int>* got = nullptr;
    int64_t tol100 = 0;
    pm_pc_tex tex{};
    // ContentDetect on the decoded textures
    std::unique_ptr<pme::ContentTap> cdTap;
    pme::D3D11ContentTap cdGpu;
    int cdState = 0;   // 0 not set up, 1 running, -1 failed
    unsigned NextSlot() { const unsigned s = next; next = (next + 1) % slots; return s; }
    // where a decoded frame is kept: the ring (its real picture) or, AllOut, the source ring
    unsigned NextIn() { if (srcRing.empty()) return NextSlot(); const unsigned s = srcNext; srcNext = (srcNext + 1) % unsigned(srcRing.size()); return slots + s; }
    ID3D11Texture2D* Tex(unsigned id) { return id < slots ? ring[id].Get() : srcRing[id - slots].Get(); }
};

PushcoreRoute::PushcoreRoute() : m(new Impl) {}

PushcoreRoute::~PushcoreRoute()
{
    if (m->pc) TheBridge().close(m->pc);
    if (m->hw) av_buffer_unref(&m->hw);
    if (m->ownMutex && m->mutex) CloseHandle(m->mutex);
    delete m;
}

AVBufferRef* PushcoreRoute::HwDevice() const { return m->hw ? av_buffer_ref(m->hw) : nullptr; }

bool PushcoreAvailable(std::string& why)
{
    if (!Exists(AddonDir() + L"\\pm_pushcore.dll")) { why = "pm_pushcore.dll is not in the add-on folder"; return false; }
    if (!Exists(CoreDir() + L"\\pushcore.dll")) { why = "PushMPC's core is not installed (run PushMPC Setup)"; return false; }
    return true;
}

std::string AdapterOfContext(void* d3d11Context)
{
    auto* ctx = static_cast<ID3D11DeviceContext*>(d3d11Context);
    if (!ctx) return "unknown";
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    ComPtr<IDXGIDevice> dd;
    ComPtr<IDXGIAdapter> ad;
    if (!dev || FAILED(dev.As(&dd)) || FAILED(dd->GetAdapter(&ad))) return "unknown";
    DXGI_ADAPTER_DESC d{};
    ad->GetDesc(&d);
    return Narrow(d.Description);
}

bool PushcoreRoute::Open(const PcConfig& c, std::string& why)
{
    if (!PushcoreAvailable(why)) return false;
    Bridge& b = TheBridge();
    if (!b.Load()) { why = b.why; return false; }
    if ((c.width & 1) || (c.height & 1)) { why = "odd picture size"; return false; }
    m_w = c.width; m_h = c.height; m_bits = c.bits > 8 ? 10 : 8;
    m->fmt = m_bits > 8 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;

    // the device: the high-performance GPU (WARP for the CI stub core)
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    HRESULT hr = E_FAIL;
    std::string adapter = "WARP";
    if (!c.warp) {
        ComPtr<IDXGIFactory6> f;
        ComPtr<IDXGIAdapter1> ad;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f))))
            f->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&ad));
        if (ad) {
            DXGI_ADAPTER_DESC1 d{};
            ad->GetDesc1(&d);
            adapter = Narrow(d.Description);
            hr = D3D11CreateDevice(ad.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, UINT(std::size(levels)), D3D11_SDK_VERSION, &m->dev, nullptr, &m->ctx);
            if (FAILED(hr))
                hr = D3D11CreateDevice(ad.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags & ~D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, UINT(std::size(levels)),
                                       D3D11_SDK_VERSION, &m->dev, nullptr, &m->ctx);
        }
    }
    if (FAILED(hr)) {
        adapter = "WARP";
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags & ~D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels + 2, UINT(std::size(levels)) - 2,
                               D3D11_SDK_VERSION, &m->dev, nullptr, &m->ctx);
    }
    if (FAILED(hr)) { char e[64]; std::snprintf(e, sizeof(e), "no D3D11 device (0x%08lX)", static_cast<unsigned long>(hr)); why = e; return false; }
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(m->ctx.As(&mt))) mt->SetMultithreadProtected(TRUE);

    // FFmpeg's D3D11VA device on it (hardware decoding straight onto pushcore's device); its lock guards the context
    AVBufferRef* hw = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (hw) {
        auto* dc = reinterpret_cast<AVHWDeviceContext*>(hw->data);
        auto* d3d = static_cast<AVD3D11VADeviceContext*>(dc->hwctx);
        d3d->device = m->dev.Get();
        m->dev->AddRef();   // FFmpeg releases it with the device context
        if (av_hwdevice_ctx_init(hw) >= 0) {
            m->hw = hw;
            m->mutex = d3d->lock_ctx;   // FFmpeg's default lock: a Win32 mutex (d3d11va_default_lock)
        } else {
            av_buffer_unref(&hw);       // no video device here (WARP): software decoding, frames uploaded
        }
    }
    if (!m->mutex) { m->mutex = CreateMutexW(nullptr, FALSE, nullptr); m->ownMutex = true; }

    const bool abi3 = b.abi >= 3;
    pm_pc_cfg pcc{};
    pcc.abi = abi3 ? PM_PC_ABI : 2u;
    pcc.width = uint32_t(c.width); pcc.height = uint32_t(c.height); pcc.bits = m_bits;
    pcc.fps_num = c.fpsNum; pcc.fps_den = c.fpsDen;
    pcc.matrix = c.matrix; pcc.transfer = c.transfer; pcc.primaries = c.primaries; pcc.range = c.range;
    pcc.smoother = c.smoother;
    pcc.display_hz = c.displayHz;
    pcc.warp = adapter == "WARP" ? 1 : 0;
    const std::wstring core = EnvW(L"PUSHMPC_CORE_DIR");
    pcc.core_dir = core.empty() ? nullptr : core.c_str();
    if (abi3) {
        pcc.struct_size = sizeof(pcc);
        pcc.sr = c.sr && c.outW > 0 && c.outH > 0 ? 1 : 0;
        pcc.out_width = uint32_t(std::max(0, c.outW)); pcc.out_height = uint32_t(std::max(0, c.outH));
        pcc.anime_model = c.animeModel >= 0 ? c.animeModel : PM_PC_ANIME_NONE;
        pcc.content_class = c.contentClass; pcc.content_cg = c.contentCg ? 1 : 0;
        // the drawn treatment: Auto's (held frames need a caller that takes a pair's frames late: this decoder does)
        pcc.held_frames = c.drawn ? PM_PC_AUTO : 0;
        pcc.deband = c.drawn ? PM_PC_AUTO : 0;
        pcc.real_out = pcc.sr || c.drawn ? 1 : 0;
        pcc.held_wait = c.drawn ? 1 : 0;
    } else if (c.sr) {
        why = "pm_pushcore.dll is older than ABI 3 (no upscaler, no settings update): update the add-on";
        return false;
    }
    m_cfg = c;
    char err[256] = {};
    int k = 1;
    m->pc = b.open(&pcc, m->dev.Get(), m->mutex, &k, err, sizeof(err));
    if (!m->pc) { why = err[0] ? err : "pushcore refused the stream"; return false; }
    m_k = std::max(1, k);
    m_ow = m_w; m_oh = m_h;
    if (abi3) {
        pm_pc_info info{};
        info.struct_size = sizeof(info);
        if (b.output(m->pc, &info) == 0) {
            m_ow = int(info.width); m_oh = int(info.height);
            m_sr = info.sr != 0; m_allOut = info.real_out != 0; m_held = info.held_frames > 0; m_anime = info.anime_model;
        }
    }
    m_desc = std::string(b.describe(m->pc)) + "; " + (m->hw ? "hardware decoding on its device" : "software decoding, frames uploaded") +
             "; GPU " + adapter;
    if (m_k <= 1 && !m_sr) { why = "pushcore runs x1 here (" + m_desc + ")"; return false; }

    // the ring: every picture of two pairs in flight, plus room; with held frames, a re-timed span (up to four source
    // frames) handed out at once on top (pushcore hands its pictures out in order, so nothing waits behind a missing one)
    m->slots = unsigned((m_held ? 7 : 2) * (m_k + 1) + 4);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = UINT(m_ow); td.Height = UINT(m_oh); td.MipLevels = 1; td.ArraySize = 1; td.Format = m->fmt;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    int stage = 0;
    m->ring.resize(m->slots);
    for (auto& t : m->ring) if (SUCCEEDED(hr)) hr = m->dev->CreateTexture2D(&td, nullptr, &t);
    if (m_allOut) {   // the decoded frames, kept as long as a picture of theirs may still be missing
        D3D11_TEXTURE2D_DESC sd = td;
        sd.Width = UINT(m_w); sd.Height = UINT(m_h);
        m->srcRing.resize(size_t(Lag() + 6));
        stage = 3;
        for (auto& t : m->srcRing) if (SUCCEEDED(hr)) hr = m->dev->CreateTexture2D(&sd, nullptr, &t);
        sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (SUCCEEDED(hr) && (m_ow != m_w || m_oh != m_h)) { stage = 4; hr = m->dev->CreateTexture2D(&sd, nullptr, &m->downSrc); }
    }
    D3D11_TEXTURE2D_DESC sd = td;
    sd.Width = UINT(m_w); sd.Height = UINT(m_h);   // the upload: a decoded frame
    sd.ArraySize = 1; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (SUCCEEDED(hr)) { stage = 1; hr = m->dev->CreateTexture2D(&sd, nullptr, &m->upStage); }
    sd.Width = UINT(m_ow); sd.Height = UINT(m_oh);  // the readback: a picture at the output size
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(hr)) { stage = 2; hr = m->dev->CreateTexture2D(&sd, nullptr, &m->downStage); }
    if (FAILED(hr)) { char e[96]; std::snprintf(e, sizeof(e), "cannot create the %s picture ring (step %d, 0x%08lX)", m_bits > 8 ? "P010" : "NV12", stage, static_cast<unsigned long>(hr)); why = e; return false; }
    return true;
}

bool PushcoreRoute::BridgeHasAbi3()
{
    Bridge& b = TheBridge();
    return b.Load() && b.abi >= 3;
}

void PushcoreRoute::SetLog(std::function<void(const std::string&)> fn) { LogFn() = std::move(fn); }

bool PushcoreRoute::MeasureTexture(void* texture, unsigned slice, int64_t ptsUs, bool disc, bool limited, int& cls, bool& cg)
{
    if (!m->dev || !texture || m->cdState < 0) return false;
    {
        Lock l(m->mutex);
        if (m->cdState == 0) {
            std::string why;
            m->cdState = m->cdGpu.Init(m->dev.Get(), uint32_t(m_w), uint32_t(m_h), m_bits > 8, limited, why) ? 1 : -1;
            if (LogFn()) LogFn()(m->cdState > 0 ? std::string("content measured on the GPU (the decoded textures: the cells and the image model's thumbnail only)")
                                                : "content not measured on the GPU: " + why);
            if (m->cdState < 0) return false;
            m->cdTap = std::make_unique<pme::ContentTap>();
        }
        m->cdGpu.Record(m->ctx.Get(), static_cast<ID3D11Texture2D*>(texture), slice, double(ptsUs) / 1e6, disc, *m->cdTap);
    }
    const pme::ContentTap::Snapshot sn = m->cdTap->Poll();
    cls = sn.cls; cg = sn.cg;
    return true;
}

void PushcoreRoute::WithDevice(const std::function<void(void*, void*)>& fn)
{
    if (!m->dev) return;
    Lock l(m->mutex);
    fn(m->dev.Get(), m->ctx.Get());
}

bool PushcoreRoute::Update(int contentClass, bool contentCg, int animeModel, std::string& why)
{
    Bridge& b = TheBridge();
    if (!m->pc || b.abi < 3) { why = "the bridge has no settings update (ABI 2)"; return false; }
    pm_pc_cfg pcc{};
    pcc.abi = PM_PC_ABI; pcc.struct_size = sizeof(pcc);
    pcc.width = uint32_t(m_w); pcc.height = uint32_t(m_h); pcc.bits = m_bits;
    pcc.sr = m_cfg.sr && m_cfg.outW > 0 && m_cfg.outH > 0 ? 1 : 0;
    pcc.out_width = uint32_t(std::max(0, m_cfg.outW)); pcc.out_height = uint32_t(std::max(0, m_cfg.outH));
    pcc.real_out = m_allOut ? 1 : 0;
    pcc.anime_model = animeModel >= 0 ? animeModel : PM_PC_ANIME_NONE;
    pcc.content_class = contentClass; pcc.content_cg = contentCg ? 1 : 0;
    pcc.held_frames = m_cfg.drawn ? PM_PC_AUTO : 0;
    pcc.deband = m_cfg.drawn ? PM_PC_AUTO : 0;
    pcc.held_wait = m_cfg.drawn ? 1 : 0;
    char err[256] = {};
    const int r = b.update(m->pc, &pcc, err, sizeof(err));
    if (r < 0) { why = err[0] ? err : "refused"; return false; }
    pm_pc_info info{};
    info.struct_size = sizeof(info);
    if (b.output(m->pc, &info) == 0) { m_anime = info.anime_model; m_held = info.held_frames > 0; }
    if (r == 0) m_desc = std::string(b.describe(m->pc));
    if (animeModel >= 0 && m_anime != animeModel) { why = "pushcore cannot run that anime upscaler on this GPU"; return false; }
    return true;
}

int PushcoreRoute::PushTexture(void* texture, unsigned slice, int64_t ptsUs, bool contiguous, std::string& err)
{
    if (!m->pc || !texture) { err = "pushcore is not open"; return -1; }
    auto* src = static_cast<ID3D11Texture2D*>(texture);
    D3D11_TEXTURE2D_DESC d{};
    src->GetDesc(&d);
    if (d.Format != m->fmt) { err = "decoder texture format differs from the ring"; return -1; }
    const unsigned s = m->NextIn();
    {
        Lock l(m->mutex);
        const D3D11_BOX box = { 0, 0, 0, UINT(m_w), UINT(m_h), 1 };
        m->ctx->CopySubresourceRegion(m->Tex(s), 0, 0, 0, 0, src, D3D11CalcSubresource(0, slice, d.MipLevels), &box);
    }
    char e[256] = {};
    if (TheBridge().push(m->pc, texture, slice, ptsUs * 10, contiguous ? 0 : 1, e, sizeof(e)) != 0) { err = e; return -1; }
    return int(s);
}

int PushcoreRoute::PushPlanes(const pm_yuv& src, int64_t ptsUs, bool contiguous, std::string& err)
{
    if (!m->pc) { err = "pushcore is not open"; return -1; }
    const unsigned s = m->NextIn();
    {
        Lock l(m->mutex);
        D3D11_MAPPED_SUBRESOURCE mp{};
        const HRESULT hr = m->ctx->Map(m->upStage.Get(), 0, D3D11_MAP_WRITE, 0, &mp);
        if (FAILED(hr)) { err = "Map(upload) failed"; return -1; }
        auto* y = static_cast<uint8_t*>(mp.pData);
        uint8_t* uv = y + size_t(mp.RowPitch) * size_t(m_h);
        const int cw = m_w / 2, ch = m_h / 2;
        if (m_bits <= 8) {
            for (int r = 0; r < m_h; ++r) std::memcpy(y + size_t(r) * mp.RowPitch, src.data[0] + r * src.stride[0], size_t(m_w));
            for (int r = 0; r < ch; ++r) {
                const uint8_t* pu = src.data[1] + r * src.stride[1];
                const uint8_t* pv = src.data[2] + r * src.stride[2];
                uint8_t* o = uv + size_t(r) * mp.RowPitch;
                for (int x = 0; x < cw; ++x) { o[2 * x] = pu[x]; o[2 * x + 1] = pv[x]; }
            }
        } else {   // planar 10-bit (low bits) -> P010 (high bits)
            for (int r = 0; r < m_h; ++r) {
                const auto* sp = reinterpret_cast<const uint16_t*>(src.data[0] + r * src.stride[0]);
                auto* o = reinterpret_cast<uint16_t*>(y + size_t(r) * mp.RowPitch);
                for (int x = 0; x < m_w; ++x) o[x] = uint16_t(sp[x] << 6);
            }
            for (int r = 0; r < ch; ++r) {
                const auto* pu = reinterpret_cast<const uint16_t*>(src.data[1] + r * src.stride[1]);
                const auto* pv = reinterpret_cast<const uint16_t*>(src.data[2] + r * src.stride[2]);
                auto* o = reinterpret_cast<uint16_t*>(uv + size_t(r) * mp.RowPitch);
                for (int x = 0; x < cw; ++x) { o[2 * x] = uint16_t(pu[x] << 6); o[2 * x + 1] = uint16_t(pv[x] << 6); }
            }
        }
        m->ctx->Unmap(m->upStage.Get(), 0);
        m->ctx->CopySubresourceRegion(m->Tex(s), 0, 0, 0, 0, m->upStage.Get(), 0, nullptr);
    }
    char e[256] = {};
    if (TheBridge().push(m->pc, m->Tex(s), 0, ptsUs * 10, contiguous ? 0 : 1, e, sizeof(e)) != 0) { err = e; return -1; }
    return int(s);
}

void PushcoreRoute::Collect(const std::vector<int64_t>& wantUs, double frameUs, std::vector<int>& slots)
{
    slots.assign(wantUs.size(), -1);
    const double step = wantUs.size() > 0 && frameUs > 0 ? frameUs / double(wantUs.size() + 1) : 4000.0;
    // wait for the pair's frames: at most 2.5 source frames (pushcore keeps up or the real frame stands in)
    const double waitMs = std::clamp(frameUs > 0 ? 2.5 * frameUs / 1000.0 : 100.0, 40.0, 250.0);
    CollectInto(wantUs, int64_t(step / 2.0), waitMs, slots);
    for (int g : slots) Count(g >= 0);
}

void PushcoreRoute::CollectInto(const std::vector<int64_t>& wantUs, int64_t tolUs, double waitMs, std::vector<int>& slots)
{
    if (slots.size() != wantUs.size()) slots.resize(wantUs.size(), -1);
    if (!m->pc) return;
    m->want = &wantUs; m->got = &slots;
    m->tol100 = tolUs * 10;
    auto dest = [](void* user, int64_t pts100) -> const pm_pc_tex* {
        auto* im = static_cast<Impl*>(user);
        int best = -1; int64_t bestD = INT64_MAX;
        for (size_t j = 0; j < im->want->size(); ++j) {
            if ((*im->got)[j] >= 0 || (*im->got)[j] <= -2) continue;
            const int64_t d = std::llabs((*im->want)[j] * 10 - pts100);
            if (d <= im->tol100 && d < bestD) { best = int(j); bestD = d; }
        }
        if (best < 0) return nullptr;
        const unsigned s = im->NextSlot();
        (*im->got)[size_t(best)] = -2 - int(s);   // routed, pixels not in yet
        im->tex.texture = im->ring[s].Get(); im->tex.slice = 0;
        return &im->tex;
    };
    auto done = [](void* user, int64_t) {
        auto* im = static_cast<Impl*>(user);
        for (int& g : *im->got) if (g <= -2) g = -2 - g;   // the routed one is filled
    };
    // wait for the first missing picture at most waitMs (0: take only what is finished)
    const ULONGLONG deadline = GetTickCount64() + ULONGLONG(std::max(0.0, waitMs));
    char e[256] = {};
    if (waitMs > 0) {
        for (;;) {
            const bool all = std::all_of(slots.begin(), slots.end(), [](int v) { return v >= 0; });
            if (all || GetTickCount64() >= deadline) break;
            if (TheBridge().poll(m->pc, 5, dest, done, m, e, sizeof(e)) < 0) break;
        }
    }
    // what pushcore has finished besides (its real frames above all: they hold its queue until returned)
    for (int guard = 0; guard < 64; ++guard)
        if (TheBridge().poll(m->pc, 0, dest, done, m, e, sizeof(e)) <= 0) break;
    for (int& g : slots) if (g <= -2) g = -1;   // routed but never completed
    m->want = nullptr; m->got = nullptr;
}

bool PushcoreRoute::Read(int slot, uint8_t* const dst[3], const int stride[3], bool semi, std::string& err)
{
    if (!m->pc || slot < 0 || unsigned(slot) >= m->slots + unsigned(m->srcRing.size())) { err = "no such picture"; return false; }
    // a decoded frame kept at the source size, shown at a larger output size: read at its own size, enlarged below
    const bool enlarge = unsigned(slot) >= m->slots && (m_ow != m_w || m_oh != m_h);
    const int sw = enlarge ? m_w : m_ow, sh = enlarge ? m_h : m_oh;
    ID3D11Texture2D* stage = enlarge ? m->downSrc.Get() : m->downStage.Get();
    Lock l(m->mutex);
    m->ctx->CopySubresourceRegion(stage, 0, 0, 0, 0, m->Tex(unsigned(slot)), 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE mp{};
    const HRESULT hr = m->ctx->Map(stage, 0, D3D11_MAP_READ, 0, &mp);
    if (FAILED(hr)) { err = "Map(readback) failed"; return false; }
    const auto* y = static_cast<const uint8_t*>(mp.pData);
    const uint8_t* uv = y + size_t(mp.RowPitch) * size_t(sh);
    const int bps = m_bits > 8 ? 2 : 1, cw = m_ow / 2, ch = m_oh / 2;
    // output row / column -> the source's (nearest pixel; the identity at the same size)
    auto rowY = [&](int r) { return enlarge ? int(int64_t(r) * sh / m_oh) : r; };
    auto colY = [&](int x) { return enlarge ? int(int64_t(x) * sw / m_ow) : x; };
    auto rowC = [&](int r) { return enlarge ? std::min(sh / 2 - 1, int(int64_t(r) * (sh / 2) / ch)) : r; };
    auto colC = [&](int x) { return enlarge ? std::min(sw / 2 - 1, int(int64_t(x) * (sw / 2) / cw)) : x; };
    for (int r = 0; r < m_oh; ++r) {
        const uint8_t* srow = y + size_t(rowY(r)) * mp.RowPitch;
        if (!enlarge && (semi || bps == 1)) { std::memcpy(dst[0] + ptrdiff_t(r) * stride[0], srow, size_t(m_ow) * bps); continue; }
        if (bps == 1) {
            uint8_t* o = dst[0] + ptrdiff_t(r) * stride[0];
            for (int x = 0; x < m_ow; ++x) o[x] = srow[colY(x)];
        } else {
            const auto* sp = reinterpret_cast<const uint16_t*>(srow);
            auto* o = reinterpret_cast<uint16_t*>(dst[0] + ptrdiff_t(r) * stride[0]);
            for (int x = 0; x < m_ow; ++x) o[x] = semi ? sp[colY(x)] : uint16_t(sp[colY(x)] >> 6);
        }
    }
    for (int r = 0; r < ch; ++r) {
        const uint8_t* row = uv + size_t(rowC(r)) * mp.RowPitch;
        if (semi) {
            if (!enlarge) { std::memcpy(dst[1] + ptrdiff_t(r) * stride[1], row, size_t(cw) * 2 * bps); continue; }
            if (bps == 1) {
                uint8_t* o = dst[1] + ptrdiff_t(r) * stride[1];
                for (int x = 0; x < cw; ++x) { const int c = colC(x); o[2 * x] = row[2 * c]; o[2 * x + 1] = row[2 * c + 1]; }
            } else {
                const auto* sp = reinterpret_cast<const uint16_t*>(row);
                auto* o = reinterpret_cast<uint16_t*>(dst[1] + ptrdiff_t(r) * stride[1]);
                for (int x = 0; x < cw; ++x) { const int c = colC(x); o[2 * x] = sp[2 * c]; o[2 * x + 1] = sp[2 * c + 1]; }
            }
            continue;
        }
        if (bps == 1) {
            uint8_t* pu = dst[1] + ptrdiff_t(r) * stride[1];
            uint8_t* pv = dst[2] + ptrdiff_t(r) * stride[2];
            for (int x = 0; x < cw; ++x) { const int c = colC(x); pu[x] = row[2 * c]; pv[x] = row[2 * c + 1]; }
        } else {
            const auto* sp = reinterpret_cast<const uint16_t*>(row);
            auto* pu = reinterpret_cast<uint16_t*>(dst[1] + ptrdiff_t(r) * stride[1]);
            auto* pv = reinterpret_cast<uint16_t*>(dst[2] + ptrdiff_t(r) * stride[2]);
            for (int x = 0; x < cw; ++x) { const int c = colC(x); pu[x] = uint16_t(sp[2 * c] >> 6); pv[x] = uint16_t(sp[2 * c + 1] >> 6); }
        }
    }
    m->ctx->Unmap(stage, 0);
    return true;
}

void PushcoreRoute::Flush()
{
    if (m->pc) TheBridge().flush(m->pc);
}

} // namespace pmk

// the engine module's log (its ContentDetect tap and detector thread) goes where the route's does
namespace pme {
void Log(const std::string& line) { if (pmk::LogFn()) pmk::LogFn()(line); }
}
