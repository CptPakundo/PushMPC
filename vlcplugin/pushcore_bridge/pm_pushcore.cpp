// pm_pushcore.dll: pushcore for the VLC module (see pm_pushcore.h). MSVC; reuses vsplugin's CoreHost and the filter's
// Auto rules.
#include "pm_pushcore.h"
#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>
#include <format>
#include <map>
#include <string>
#include <vector>
#include "corehost.h"
#include "autopick.h"
#include "vslog.h"

using namespace pmvs;

// ---- zero copy: pushcore's RGB output -> NV12 / P010 on the player's device (two pixel-shader passes into the luma
// and chroma planes of a texture of ours, then a copy into the player's texture slice), in a device context state of
// our own so the player's pipeline state stays as it was. Same matrix and rounding as CoreHost's 4:2:0 readback.
static const char kZcHlsl[] = R"(
Texture2D<float4> src : register(t0);
cbuffer C : register(b0) { float kr; float kb; float scale; float maxCode; float outDiv; uint w; uint h; uint pad; };
struct V { float4 pos : SV_Position; };
V vs(uint id : SV_VertexID)
{
    V o;
    const float2 t = float2((id << 1) & 2, id & 2);
    o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float3 Px(int2 p) { return saturate(src.Load(int3(min(p, int2(w - 1, h - 1)), 0)).rgb); }
float Y(float3 c) { return kr * c.r + (1.0 - kr - kb) * c.g + kb * c.b; }
float psY(V i) : SV_Target
{
    const float y = Y(Px(int2(i.pos.xy)));
    return clamp(round((16.0 + 219.0 * y) * scale), 0, maxCode) / outDiv;
}
float2 psUV(V i) : SV_Target
{
    const int2 p = int2(i.pos.xy) * 2;
    float cb = 0, cr = 0;
    [unroll] for (int j = 0; j < 2; ++j) [unroll] for (int k = 0; k < 2; ++k) {
        const float3 c = Px(p + int2(k, j));
        const float y = Y(c);
        cb += (c.b - y) / (2.0 * (1.0 - kb));
        cr += (c.r - y) / (2.0 * (1.0 - kr));
    }
    return float2(clamp(round((128.0 + 224.0 * cb * 0.25) * scale), 0, maxCode),
                  clamp(round((128.0 + 224.0 * cr * 0.25) * scale), 0, maxCode)) / outDiv;
}
)";

struct Zc {
    HANDLE mutex = nullptr;          // VLC's context lock (may be null)
    ComPtr<ID3D11Device1> dev1;
    ComPtr<ID3DDeviceContextState> state;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> psY, psUV;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11Texture2D> mid;     // NV12 / P010, render target
    ComPtr<ID3D11RenderTargetView> rtvY, rtvUV;
    std::map<ID3D11Texture2D*, ComPtr<ID3D11ShaderResourceView>> srv;
    DXGI_FORMAT fmt = DXGI_FORMAT_NV12;
    bool hdrMatrix = false;
    // the frame being submitted, and where generated frames go during a poll
    ID3D11Texture2D* curTex = nullptr; uint32_t curSlice = 0;
    pm_pc_dest_tex_fn dest = nullptr;
};

struct pm_pc {
    uint64_t token = 0;
    uint32_t w = 0, h = 0;
    int bits = 8;
    int k = 1;
    // the source frame handed to CoreHost::Submit through its source callback
    PlaneView cur;
    // generated frames land in a buffer of ours first (CoreHost allocates before it knows the frame), then go out
    std::vector<uint8_t> buf[4];
    size_t bufNext = 0;
    pm_pc_dest_fn dest = nullptr;
    pm_pc_done_fn done = nullptr;
    void* user = nullptr;
    int handed = 0;
    std::string describe;
    Zc* zc = nullptr;                // zero copy (pm_pc_open_d3d11)
};

static void SetErr(char* err, size_t n, const std::string& s) { if (err && n) { strncpy_s(err, n, s.c_str(), _TRUNCATE); } }

static std::wstring DefaultCoreDir()
{
    std::wstring d = LocalAppDataDir() + L"\\PushMPC\\core";
    return d;
}

struct MutexLock {                   // VLC's d3d11_device_lock
    HANDLE m;
    explicit MutexLock(HANDLE h) : m(h) { if (m) WaitForSingleObjectEx(m, INFINITE, FALSE); }
    ~MutexLock() { if (m) ReleaseMutex(m); }
};

static bool ZcSetup(Zc& z, ID3D11Device* dev, uint32_t w, uint32_t h, bool tenBit, std::string& e)
{
    z.fmt = tenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    UINT support = 0;
    if (FAILED(dev->CheckFormatSupport(z.fmt, &support)) || !(support & D3D11_FORMAT_SUPPORT_RENDER_TARGET)) {
        e = std::format("this device cannot render into {}", tenBit ? "P010" : "NV12"); return false;
    }
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&z.dev1)))) { e = "no ID3D11Device1"; return false; }
    const D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel();
    HRESULT hr = z.dev1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &z.state);
    if (FAILED(hr)) { e = std::format("CreateDeviceContextState (0x{:08X})", uint32_t(hr)); return false; }
    auto compile = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& code) {
        ComPtr<ID3DBlob> errs;
        hr = D3DCompile(kZcHlsl, sizeof(kZcHlsl) - 1, "pm_zc", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errs);
        if (FAILED(hr)) e = std::format("{}: {}", entry, errs ? static_cast<const char*>(errs->GetBufferPointer()) : "D3DCompile failed");
        return SUCCEEDED(hr);
    };
    ComPtr<ID3DBlob> cv, cy, cuv;
    if (!compile("vs", "vs_4_0", cv) || !compile("psY", "ps_4_0", cy) || !compile("psUV", "ps_4_0", cuv)) return false;
    hr = dev->CreateVertexShader(cv->GetBufferPointer(), cv->GetBufferSize(), nullptr, &z.vs);
    if (SUCCEEDED(hr)) hr = dev->CreatePixelShader(cy->GetBufferPointer(), cy->GetBufferSize(), nullptr, &z.psY);
    if (SUCCEEDED(hr)) hr = dev->CreatePixelShader(cuv->GetBufferPointer(), cuv->GetBufferSize(), nullptr, &z.psUV);
    if (SUCCEEDED(hr)) { D3D11_BUFFER_DESC b = { 32, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 }; hr = dev->CreateBuffer(&b, nullptr, &z.cb); }
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = z.fmt; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (SUCCEEDED(hr)) hr = dev->CreateTexture2D(&d, nullptr, &z.mid);
    D3D11_RENDER_TARGET_VIEW_DESC rd = {};
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rd.Format = tenBit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;       // the luma plane
    if (SUCCEEDED(hr)) hr = dev->CreateRenderTargetView(z.mid.Get(), &rd, &z.rtvY);
    rd.Format = tenBit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;  // the chroma plane
    if (SUCCEEDED(hr)) hr = dev->CreateRenderTargetView(z.mid.Get(), &rd, &z.rtvUV);
    if (FAILED(hr)) { e = std::format("zero-copy objects (0x{:08X})", uint32_t(hr)); return false; }
    return true;
}

// one pushcore frame -> the player's texture slice (generated frames only; real ones are the player's own pictures)
static bool ZcOut(pm_pc* pc, const PushCoreFrame& fr, std::string& e)
{
    Zc& z = *pc->zc;
    if (fr.kind != PUSHCORE_FRAME_GENERATED || !z.dest) return false;
    const pm_pc_tex* t = z.dest(pc->user, fr.pts100ns);
    if (!t || !t->texture) return false;
    auto* dst = static_cast<ID3D11Texture2D*>(t->texture);
    D3D11_TEXTURE2D_DESC dd = {};
    dst->GetDesc(&dd);
    if (dd.Format != z.fmt || dd.Width < pc->w || dd.Height < pc->h || t->slice >= dd.ArraySize) {
        e = std::format("destination texture {}x{} format {} slice {} does not fit", dd.Width, dd.Height, uint32_t(dd.Format), t->slice);
        return false;
    }
    CoreHost& host = CoreHost::Get();
    ID3D11DeviceContext* ctx = host.Context();
    if (fr.readyFence) {
        if (!host.Context4()) { e = "pushcore hands out fences and this device has no ID3D11DeviceContext4"; return false; }
        host.Context4()->Wait(fr.readyFence, fr.readyValue);
    }
    auto& srv = z.srv[fr.texture];
    if (!srv) {
        D3D11_TEXTURE2D_DESC td = {};
        fr.texture->GetDesc(&td);
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = td.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS ? DXGI_FORMAT_R10G10B10A2_UNORM :
                    td.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM :
                    td.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM : td.Format;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
        const HRESULT hr = host.Device()->CreateShaderResourceView(fr.texture, &sd, &srv);
        if (FAILED(hr)) { z.srv.erase(fr.texture); e = std::format("pushcore's output cannot be read by a shader (0x{:08X})", uint32_t(hr)); return false; }
    }
    const bool ten = z.fmt == DXGI_FORMAT_P010;
    struct { float kr, kb, scale, maxCode, outDiv; uint32_t w, h, pad; } cbv = {
        z.hdrMatrix ? 0.2627f : 0.2126f, z.hdrMatrix ? 0.0593f : 0.0722f, ten ? 4.0f : 1.0f, ten ? 1023.0f : 255.0f,
        ten ? 65535.0f / 64.0f : 255.0f, pc->w, pc->h, 0 };
    ComPtr<ID3D11DeviceContext1> ctx1;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1)))) { e = "no ID3D11DeviceContext1"; return false; }
    ID3DDeviceContextState* prev = nullptr;
    ctx1->SwapDeviceContextState(z.state.Get(), &prev);
    ctx->UpdateSubresource(z.cb.Get(), 0, nullptr, &cbv, 0, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(z.vs.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[1] = { srv.Get() };
    ID3D11Buffer* cbs[1] = { z.cb.Get() };
    ctx->PSSetShaderResources(0, 1, srvs);
    ctx->PSSetConstantBuffers(0, 1, cbs);
    D3D11_VIEWPORT vp = { 0, 0, float(pc->w), float(pc->h), 0, 1 };
    ID3D11RenderTargetView* rt = z.rtvY.Get();
    ctx->OMSetRenderTargets(1, &rt, nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->PSSetShader(z.psY.Get(), nullptr, 0);
    ctx->Draw(3, 0);
    vp.Width = float(pc->w / 2); vp.Height = float(pc->h / 2);
    rt = z.rtvUV.Get();
    ctx->OMSetRenderTargets(1, &rt, nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->PSSetShader(z.psUV.Get(), nullptr, 0);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none[1] = {};
    ctx->PSSetShaderResources(0, 1, none);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ctx1->SwapDeviceContextState(prev, nullptr);
    if (prev) prev->Release();
    const D3D11_BOX box = { 0, 0, 0, pc->w & ~1u, pc->h & ~1u, 1 };
    ctx->CopySubresourceRegion(dst, D3D11CalcSubresource(0, t->slice, dd.MipLevels), 0, 0, 0, z.mid.Get(), 0, &box);
    pc->handed++;
    if (pc->done) pc->done(pc->user, fr.pts100ns);
    return true;
}

static pm_pc* OpenImpl(const pm_pc_cfg* cfg, ID3D11Device* device, HANDLE mutex, int* kOut, char* err, size_t errsize)
{
    if (kOut) *kOut = 1;
    if (!cfg || cfg->abi != PM_PC_ABI) { SetErr(err, errsize, "pm_pushcore: ABI mismatch"); return nullptr; }
    if (!(cfg->matrix == 1 || cfg->matrix == 9 || cfg->matrix == 10)) {
        SetErr(err, errsize, std::format("pm_pushcore: matrix {} (SD / BT.601 sources stay on the portable engine)", cfg->matrix));
        return nullptr;
    }
    CoreHost& host = CoreHost::Get();
    std::lock_guard<std::mutex> lk(host.Lock());
    MutexLock vlc(mutex);   // with a player's device, its immediate context is the player's
    HostPaths paths;
    paths.coreDir = cfg->core_dir && *cfg->core_dir ? cfg->core_dir : DefaultCoreDir();
    const std::wstring rt = LocalAppDataDir() + L"\\PushMPC\\runtime";
    paths.runtimeDir = GetFileAttributesW(rt.c_str()) != INVALID_FILE_ATTRIBUTES ? rt : paths.coreDir;
    const std::wstring sh = paths.coreDir + L"\\shaders";
    if (GetFileAttributesW(sh.c_str()) != INVALID_FILE_ATTRIBUTES) paths.shaderDir = sh;
    std::string e;
    if (!host.Load(paths, e)) { SetErr(err, errsize, e); return nullptr; }
    pm::VfgCeiling() = (host.Features() & PUSHCORE_FEATURE_VFG16) ? int(PUSHCORE_MAX_MULTIPLIER) : 8;   // VFG above 8x only with a core that has it
    if (device ? !host.AdoptDevice(device, e) : !host.EnsureDevice(0, cfg->warp ? AdapterPick::Warp : AdapterPick::Auto, e)) {
        SetErr(err, errsize, e); return nullptr;
    }

    // Auto with the user's PushMPC settings; VLC keeps the source size and shows its own real frames
    pm::Settings s = pm::LoadSettings();
    if (cfg->smoother == 1) s.smoother = pm::kOff;
    else if (cfg->smoother >= 2) s.smoother = std::clamp(cfg->smoother, 2, pm::kMaxSmoother);
    s.sharper = pm::kOff; s.cleanUp = pm::kOff; s.deband = pm::kOff; s.sdrToHdr = pm::kOff;
    pm::Gpu g; g.available = 0; g.hardware = 0; g.family = pm::kFamUnknown;
    PushCoreCaps caps;
    if (host.Caps(caps)) { g.family = caps.family; g.available = caps.available; g.hardware = caps.hardware; g.name = Widen(caps.adapter); }
    pm::Costs costs;
    costs.card = pm::CardFactor(g.name, g.family);
    LoadLearned(costs, g.name);
    pm::Source src;
    src.w = cfg->width; src.h = cfg->height;
    src.fps = cfg->fps_num > 0 && cfg->fps_den > 0 ? double(cfg->fps_num) / cfg->fps_den : 23.976;
    src.hdr = IsHdrTransfer(cfg->transfer); src.tenBit = cfg->bits > 8;
    pm::Display disp = pm::QueryDisplay(nullptr, s.vrr, g.nvidia());
    if (cfg->display_hz > 1) disp.refreshHz = cfg->display_hz;
    disp.hdr = src.hdr;   // HDR sources stay HDR10 (VLC presents them); SDR stays SDR
    pm::Choice c = pm::Decide(s, src, disp, costs, g);
    c.sr = false; c.nr = false; c.deband = false; c.hdr = false;
    PushCoreSettings ps = ToCore(s, c, disp, src.w, src.h, src.hdr, disp.hdr, nullptr);
    ps.sr = 0; ps.nr = 0; ps.deband = 0; ps.videoHdr = 0;

    PushCoreStreamInfo si = {};
    si.structSize = sizeof(si);
    si.width = cfg->width; si.height = cfg->height;
    si.format = cfg->bits > 8 ? PUSHCORE_INPUT_P010 : PUSHCORE_INPUT_NV12;
    si.fpsNum = cfg->fps_num; si.fpsDen = cfg->fps_den;
    si.range = cfg->range; si.matrix = cfg->matrix; si.primaries = cfg->primaries; si.transfer = cfg->transfer;
    si.displayHdr = disp.hdr ? 1u : 0u;
    CoreKey key;
    key.width = si.width; key.height = si.height; key.format = si.format; key.fpsNum = si.fpsNum; key.fpsDen = si.fpsDen;
    key.range = si.range; key.matrix = si.matrix; key.primaries = si.primaries; key.transfer = si.transfer; key.displayHdr = si.displayHdr;
    key.adapterLuid = host.DeviceLuid();
    key.settings.assign(reinterpret_cast<const uint8_t*>(&ps), reinterpret_cast<const uint8_t*>(&ps) + sizeof(ps));
    Reuse how = Reuse::Open; double ms = 0;
    auto* pc = new pm_pc();
    pc->token = host.Acquire(key, si, ps, how, ms, e);
    if (!pc->token) { SetErr(err, errsize, "pushcore_open: " + e); delete pc; return nullptr; }
    const PushCoreOutputInfo& oi = host.OutInfo();
    if (oi.width != cfg->width || oi.height != cfg->height) {
        host.Release(pc->token);
        SetErr(err, errsize, std::format("pushcore output {}x{} differs from the source {}x{}", oi.width, oi.height, cfg->width, cfg->height));
        delete pc; return nullptr;
    }
    pc->w = cfg->width; pc->h = cfg->height; pc->bits = cfg->bits;
    if (device) {
        pc->zc = new Zc();
        pc->zc->mutex = mutex;
        pc->zc->hdrMatrix = oi.hdrOutput != 0;
        if (!ZcSetup(*pc->zc, device, pc->w, pc->h, cfg->bits > 8, e)) {
            host.Release(pc->token); SetErr(err, errsize, "zero copy: " + e); delete pc->zc; delete pc; return nullptr;
        }
    } else if (!host.ConfigureEgress(cfg->bits > 8 || oi.hdrOutput ? OutFmt::Yuv420P10 : OutFmt::Yuv420P8, oi.hdrOutput != 0, e)) {
        host.Release(pc->token); SetErr(err, errsize, "egress: " + e); delete pc; return nullptr;
    }
    pc->k = int(std::clamp<uint32_t>(oi.multiplier ? oi.multiplier : ps.multiplier, 1, PUSHCORE_MAX_MULTIPLIER));
    pc->describe = std::format("{} ({} in {:.0f} ms on {}; Auto: x{} {}){}", oi.description, ReuseName(how), ms, host.DeviceName(),
                               c.multiplier, Narrow(pm::FgMethodLabel(c.fgMethod)), device ? "; zero copy on VLC's D3D11 device" : "");
    Log("VLC: " + pc->describe);

    // the callbacks stay set for this owner
    if (pc->zc) {
        host.sourceTex = [pc](int64_t, ID3D11Texture2D*& tex, uint32_t& slice) { tex = pc->zc->curTex; slice = pc->zc->curSlice; return tex != nullptr; };
        host.rawOut = [pc](const PushCoreFrame& fr, std::string& err) { return ZcOut(pc, fr, err); };
        if (kOut) *kOut = pc->k;
        return pc;
    }
    host.source = [pc](int64_t, PlaneView& v) { v = pc->cur; return v.data[0] != nullptr; };
    host.alloc = [pc](uint32_t w, uint32_t h, OutImage& img) -> bool {
        const size_t bps = pc->bits > 8 ? 2 : 1, ysz = size_t(w) * h * bps, csz = size_t(w / 2) * (h / 2) * bps;
        auto& b = pc->buf[pc->bufNext]; pc->bufNext = (pc->bufNext + 1) % 4;
        b.resize(ysz + 2 * csz);
        img.data[0] = b.data(); img.data[1] = b.data() + ysz; img.data[2] = b.data() + ysz + csz;
        img.stride[0] = ptrdiff_t(w * bps); img.stride[1] = img.stride[2] = ptrdiff_t((w / 2) * bps);
        img.handle = &b;
        return true;
    };
    host.release = [](void*) {};
    host.tag = [pc](int64_t pts, bool generated, void* handle) {
        if (!generated || !pc->dest) return;
        const pm_pc_out* d = pc->dest(pc->user, pts);
        if (!d) return;
        auto* b = static_cast<std::vector<uint8_t>*>(handle);
        const size_t bps = pc->bits > 8 ? 2 : 1, ysz = size_t(pc->w) * pc->h * bps, csz = size_t(pc->w / 2) * (pc->h / 2) * bps;
        const uint8_t* srcp[3] = { b->data(), b->data() + ysz, b->data() + ysz + csz };
        for (int i = 0; i < 3; ++i) {
            const uint32_t pw = (i ? pc->w / 2 : pc->w), ph = (i ? pc->h / 2 : pc->h);
            for (uint32_t r = 0; r < ph; ++r) memcpy(d->data[i] + ptrdiff_t(r) * d->stride[i], srcp[i] + size_t(r) * pw * bps, pw * bps);
        }
        pc->handed++;
        if (pc->done) pc->done(pc->user, pts);
    };
    if (kOut) *kOut = pc->k;
    return pc;
}

extern "C" pm_pc* pm_pc_open(const pm_pc_cfg* cfg, int* kOut, char* err, size_t errsize)
{
    return OpenImpl(cfg, nullptr, nullptr, kOut, err, errsize);
}

extern "C" pm_pc* pm_pc_open_d3d11(const pm_pc_cfg* cfg, void* device, void* mutex, int* kOut, char* err, size_t errsize)
{
    if (!device) { SetErr(err, errsize, "no D3D11 device"); if (kOut) *kOut = 1; return nullptr; }
    return OpenImpl(cfg, static_cast<ID3D11Device*>(device), static_cast<HANDLE>(mutex), kOut, err, errsize);
}

extern "C" int pm_pc_push(pm_pc* pc, const pm_pc_planes* src, int64_t pts100ns, int discontinuity, char* err, size_t errsize)
{
    CoreHost& host = CoreHost::Get();
    std::lock_guard<std::mutex> lk(host.Lock());
    if (!host.Owns(pc->token)) { SetErr(err, errsize, "pushcore was taken over by another instance"); return -1; }
    if (pc->zc) { SetErr(err, errsize, "this instance takes textures (pm_pc_push_d3d11)"); return -1; }
    pc->cur = PlaneView{};
    for (int i = 0; i < 3; ++i) { pc->cur.data[i] = src->data[i]; pc->cur.stride[i] = src->stride[i]; }
    pc->cur.width = pc->w; pc->cur.height = pc->h; pc->cur.bits = pc->bits;
    std::string e;
    const bool ok = host.Submit(0, pts100ns, discontinuity != 0, e);
    pc->cur = PlaneView{};
    if (!ok) { SetErr(err, errsize, e); return -1; }
    return 0;
}

extern "C" int pm_pc_push_d3d11(pm_pc* pc, void* texture, unsigned slice, int64_t pts100ns, int discontinuity, char* err, size_t errsize)
{
    CoreHost& host = CoreHost::Get();
    std::lock_guard<std::mutex> lk(host.Lock());
    if (!host.Owns(pc->token)) { SetErr(err, errsize, "pushcore was taken over by another instance"); return -1; }
    if (!pc->zc) { SetErr(err, errsize, "this instance takes planes (pm_pc_push)"); return -1; }
    MutexLock vlc(pc->zc->mutex);
    pc->zc->curTex = static_cast<ID3D11Texture2D*>(texture); pc->zc->curSlice = slice;
    std::string e;
    const bool ok = host.Submit(0, pts100ns, discontinuity != 0, e);
    pc->zc->curTex = nullptr;
    if (!ok) { SetErr(err, errsize, e); return -1; }
    return 0;
}

extern "C" int pm_pc_poll(pm_pc* pc, uint32_t timeout_ms, pm_pc_dest_fn dest, pm_pc_done_fn done, void* user, char* err, size_t errsize)
{
    CoreHost& host = CoreHost::Get();
    std::lock_guard<std::mutex> lk(host.Lock());
    if (!host.Owns(pc->token)) { SetErr(err, errsize, "pushcore was taken over by another instance"); return -1; }
    if (pc->zc) { SetErr(err, errsize, "this instance hands out textures (pm_pc_poll_d3d11)"); return -1; }
    pc->dest = dest; pc->done = done; pc->user = user; pc->handed = 0;
    std::vector<Out<void*>> out;
    std::string e;
    const bool ok = host.Poll(timeout_ms, out, e);
    pc->dest = nullptr; pc->done = nullptr; pc->user = nullptr;
    if (!ok) { SetErr(err, errsize, e); return -1; }
    return int(out.size());   // every frame polled (real ones are returned to pushcore at once)
}

extern "C" int pm_pc_poll_d3d11(pm_pc* pc, uint32_t timeout_ms, pm_pc_dest_tex_fn dest, pm_pc_done_fn done, void* user, char* err, size_t errsize)
{
    CoreHost& host = CoreHost::Get();
    if (!pc->zc) { SetErr(err, errsize, "this instance hands out planes (pm_pc_poll)"); return -1; }
    // VLC's context lock is never held while waiting: poll without a wait under it, sleep outside it
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;) {
        std::vector<Out<void*>> out;
        std::string e;
        bool ok;
        {
            std::lock_guard<std::mutex> lk(host.Lock());
            if (!host.Owns(pc->token)) { SetErr(err, errsize, "pushcore was taken over by another instance"); return -1; }
            MutexLock vlc(pc->zc->mutex);
            pc->zc->dest = dest; pc->done = done; pc->user = user; pc->handed = 0;
            ok = host.Poll(0, out, e);
            pc->zc->dest = nullptr; pc->done = nullptr; pc->user = nullptr;
        }
        if (!ok) { SetErr(err, errsize, e); return -1; }
        if (!out.empty() || GetTickCount64() >= deadline) return int(out.size());
        Sleep(1);
    }
}

extern "C" int pm_pc_copy_d3d11(pm_pc* pc, void* src, unsigned src_slice, void* dst, unsigned dst_slice)
{
    if (!pc || !pc->zc || !src || !dst) return -1;
    CoreHost& host = CoreHost::Get();
    std::lock_guard<std::mutex> lk(host.Lock());
    MutexLock vlc(pc->zc->mutex);
    auto* s = static_cast<ID3D11Texture2D*>(src); auto* d = static_cast<ID3D11Texture2D*>(dst);
    D3D11_TEXTURE2D_DESC sd = {}, dd = {};
    s->GetDesc(&sd); d->GetDesc(&dd);
    if (sd.Format != dd.Format || src_slice >= sd.ArraySize || dst_slice >= dd.ArraySize) return -1;
    const D3D11_BOX box = { 0, 0, 0, std::min(sd.Width, dd.Width) & ~1u, std::min(sd.Height, dd.Height) & ~1u, 1 };
    host.Context()->CopySubresourceRegion(d, D3D11CalcSubresource(0, dst_slice, dd.MipLevels), 0, 0, 0,
                                          s, D3D11CalcSubresource(0, src_slice, sd.MipLevels), &box);
    return 0;
}

extern "C" int pm_pc_flush(pm_pc* pc)
{
    CoreHost& host = CoreHost::Get();
    std::lock_guard<std::mutex> lk(host.Lock());
    if (!host.Owns(pc->token)) return -1;
    MutexLock vlc(pc->zc ? pc->zc->mutex : nullptr);
    std::string e;
    return host.Flush(e) ? 0 : -1;
}

extern "C" void pm_pc_close(pm_pc* pc)
{
    if (!pc) return;
    {
        CoreHost& host = CoreHost::Get();
        std::lock_guard<std::mutex> lk(host.Lock());
        MutexLock vlc(pc->zc ? pc->zc->mutex : nullptr);
        host.Release(pc->token);   // the core stays warm for the next file
    }
    delete pc->zc;
    delete pc;
}

extern "C" const char* pm_pc_describe(pm_pc* pc) { return pc ? pc->describe.c_str() : ""; }
