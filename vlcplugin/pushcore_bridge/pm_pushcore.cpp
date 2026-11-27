// pm_pushcore.dll: pushcore for the VLC module (see pm_pushcore.h). MSVC; reuses vsplugin's CoreHost and the filter's
// Auto rules.
#include "pm_pushcore.h"
#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "corehost.h"
#include "autopick.h"
#include "vslog.h"
#include "../../mpvnative/engine/d3d11_content.h"   // ContentDetect on the GPU (measure: the zero-copy textures)

using namespace pmvs;

// the engine module's log (ContentDetect's tap and its detector thread) goes into the host's
namespace pme { void Log(const std::string& line) { pmvs::Log("content: " + line); } }

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
    uint32_t w = 0, h = 0;           // source size
    uint32_t ow = 0, oh = 0;         // output size (the source's unless pushcore's upscaler runs)
    int bits = 8;
    int k = 1;
    bool realOut = false;            // real frames come out of pushcore too (ABI 3: sr, real_out)
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
    // Auto's inputs, kept for pm_pc_update (the measured class changes, the anime model is switched)
    pm_pc_cfg cfg{};                 // the open's, as ABI 3
    pm::Settings s;
    pm::Source src;
    pm::Display real, disp;
    pm::Costs costs;
    pm::Gpu g;
    CoreKey key;                     // the stream (settings filled per acquire)
    PushCoreStreamInfo si{};
    PushCoreSettings ps{};           // what pushcore runs
    pm::Choice c;
    uint32_t target = PUSHCORE_SR_TARGET_4K;
    int updates = 0;
    // measure: ContentDetect on the textures pushed (zero copy)
    std::unique_ptr<pme::ContentTap> cdTap;
    pme::D3D11ContentTap cdGpu;
    int cdState = 0;                 // 0 not set up, 1 running, -1 failed
    int measuredClass = 0, measuredCg = 0;
    // pm_pc_measure_open: ContentDetect alone, on the caller's device and immediate context (no core taken)
    bool measureOnly = false;
    ComPtr<ID3D11DeviceContext> mctx;
    HANDLE mmutex = nullptr;
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

// one pushcore frame -> the player's texture slice (generated frames; real ones too with realOut, else they are the
// player's own pictures)
static bool ZcOut(pm_pc* pc, const PushCoreFrame& fr, std::string& e)
{
    Zc& z = *pc->zc;
    if ((fr.kind != PUSHCORE_FRAME_GENERATED && !pc->realOut) || !z.dest) return false;
    const pm_pc_tex* t = z.dest(pc->user, fr.pts100ns);
    if (!t || !t->texture) return false;
    auto* dst = static_cast<ID3D11Texture2D*>(t->texture);
    D3D11_TEXTURE2D_DESC dd = {};
    dst->GetDesc(&dd);
    if (dd.Format != z.fmt || dd.Width < pc->ow || dd.Height < pc->oh || t->slice >= dd.ArraySize) {
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
        ten ? 65535.0f / 64.0f : 255.0f, pc->ow, pc->oh, 0 };
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
    D3D11_VIEWPORT vp = { 0, 0, float(pc->ow), float(pc->oh), 0, 1 };
    ID3D11RenderTargetView* rt = z.rtvY.Get();
    ctx->OMSetRenderTargets(1, &rt, nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->PSSetShader(z.psY.Get(), nullptr, 0);
    ctx->Draw(3, 0);
    vp.Width = float(pc->ow / 2); vp.Height = float(pc->oh / 2);
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
    const D3D11_BOX box = { 0, 0, 0, pc->ow & ~1u, pc->oh & ~1u, 1 };
    ctx->CopySubresourceRegion(dst, D3D11CalcSubresource(0, t->slice, dd.MipLevels), 0, 0, 0, z.mid.Get(), 0, &box);
    pc->handed++;
    if (pc->done) pc->done(pc->user, fr.pts100ns);
    return true;
}

// the caller's pm_pc_cfg as ABI 3: an ABI 2 struct ends at fullscreen (the ABI 3 fields then take their defaults, which
// behave as ABI 2 did); an ABI 3 struct says its own size (a later ABI's appended fields are not read)
static bool ReadCfg(const pm_pc_cfg* in, pm_pc_cfg& out, std::string& e)
{
    out = pm_pc_cfg{};
    if (!in || in->abi < PM_PC_ABI_MIN) { e = "pm_pushcore: ABI mismatch"; return false; }
    if (in->abi == 2) {
        memcpy(&out, in, offsetof(pm_pc_cfg, struct_size));
    } else {
        if (in->struct_size < sizeof(pm_pc_cfg)) { e = std::format("pm_pushcore: ABI {} with a {}-byte configuration (at least {})", in->abi, in->struct_size, sizeof(pm_pc_cfg)); return false; }
        memcpy(&out, in, sizeof(pm_pc_cfg));
    }
    out.abi = PM_PC_ABI; out.struct_size = sizeof(pm_pc_cfg);
    if (in->abi == 2) {
        out.sr = 0; out.out_width = out.out_height = 0; out.anime_model = PM_PC_ANIME_NONE;
        out.content_class = 0; out.content_cg = 0; out.held_frames = 0; out.deband = 0; out.real_out = 0; out.held_wait = 0;
        out.measure = 0;
    }
    if (out.sr && (out.out_width < 16 || out.out_height < 16)) { e = "pm_pushcore: sr without an output size"; return false; }
    if (out.sr) out.real_out = 1;   // every picture is pushcore's at the output size
    return true;
}

// pushcore's upscaler target box that holds w x h (1440p, 4K, 5K: PUSHCORE_SR_TARGET_*); the output is the source fitted
// into it, the shape kept
static uint32_t TargetFor(uint32_t w, uint32_t h, pm::UpscaleTarget& t)
{
    if (w <= 2560 && h <= 1440) { t = pm::UpscaleTarget::P1440; return PUSHCORE_SR_TARGET_1440P; }
    if (w <= 3840 && h <= 2160) { t = pm::UpscaleTarget::K4; return PUSHCORE_SR_TARGET_4K; }
    t = pm::UpscaleTarget::K5; return PUSHCORE_SR_TARGET_5K;
}

static pm::Upscaler DefaultSr(const pm::Gpu& g)
{
    return g.has(pm::kCapRtxVsr) ? pm::Upscaler::Rtx : g.has(pm::kCapFsrSr) ? pm::Upscaler::Fsr : pm::Upscaler::Nis;
}

// Auto for this stream: the user's PushMPC settings, the measured content (content_class / content_cg), and what the
// route allows (the output size, real frames of the player's or of pushcore's, the caller's own switches). open: the
// first decision; else pm_pc_update, which keeps the frame generation side of the open (multiplier, method, guard).
static void Build(pm_pc* pc, const pm_pc_cfg& cfg, bool open, pm::Choice& c, PushCoreSettings& ps)
{
    CoreHost& host = CoreHost::Get();
    const uint32_t features = host.Features();
    const bool realOut = cfg.real_out != 0;
    pm::Settings s = pc->s;
    pm::Source src = pc->src;
    src.contentClass = std::clamp(cfg.content_class, 0, 4);
    src.contentCg = src.contentClass == pushmpc::content::kDrawn && cfg.content_cg != 0;
    src.contentConf = src.contentClass ? 1.0f : 0.0f;
    // deband: only where the real frames are pushcore's too (else generated frames alone would differ from the real ones)
    if (!realOut || cfg.deband == 0) s.deband = pm::kOff;
    else if (cfg.deband == 1) s.deband = 1;
    pm::UpscaleTarget t = pm::UpscaleTarget::K4;
    pc->target = cfg.sr ? TargetFor(cfg.out_width, cfg.out_height, t) : PUSHCORE_SR_TARGET_4K;
    if (cfg.sr) s.target = t; else s.sharper = pm::kOff;
    c = pm::Decide(WithToneMapBy(s, false), src, pc->real, pc->costs, pc->g);
    if (!cfg.sr) c.sr = false;
    else {
        // the output size is fixed for the stream: pushcore's upscaler always runs (Auto would only leave it out to make
        // room for frame generation, and the player shows this size anyway)
        if (!c.sr) { c.sr = true; c.srLevel = std::max(1, c.srLevel); }
        const bool animeCan = pc->g.has(pm::kCapAnimeSr) && (features & PUSHCORE_FEATURE_ANIME_SR);
        if (cfg.anime_model >= 0 && cfg.anime_model < pm::kAnimeSrModels && animeCan) { c.srMethod = pm::Upscaler::Anime; c.animeModel = cfg.anime_model; }
        else if (cfg.anime_model != PM_PC_ANIME_AUTO && c.srMethod == pm::Upscaler::Anime) c.animeModel = -1;
        if (c.srMethod == pm::Upscaler::Anime && c.animeModel < 0) c.srMethod = DefaultSr(pc->g);
    }
    c.nr = false; c.hdr = false;
    if (!realOut) c.deband = false;
    ps = ToCore(s, c, pc->disp, pc->w, pc->h, pc->src.hdr, pc->disp.hdr, nullptr);
    if (!cfg.sr) ps.sr = 0; else ps.srTarget = pc->target;
    ps.nr = 0; ps.videoHdr = 0;
    if (!realOut) ps.deband = 0;
    if (ps.sr && c.srMethod == pm::Upscaler::Anime && c.animeModel >= 0 && (features & PUSHCORE_FEATURE_ANIME_SR)) {
        ps.srMethod = PUSHCORE_SR_METHOD_PMML; ps.animeSrModel = uint32_t(c.animeModel);
    }
    // the anime preset (3) only on cores with PUSHCORE_FEATURE_ANIME_HOLD; an older core gets Medium (as the filter)
    if (ps.deband && c.debandStrength == 3) ps.debandStrength = (features & PUSHCORE_FEATURE_ANIME_HOLD) ? PUSHCORE_DEBAND_ANIME : PUSHCORE_DEBAND_MEDIUM;
    if (!cfg.sr) {   // no upscaler: its fields as one value, so a class change alone does not reload pushcore for nothing
        ps.srMethod = PUSHCORE_SR_METHOD_RTX; ps.animeSrModel = 0; ps.srQuality = 2; ps.srFamily = PUSHCORE_SR_FAMILY_STANDARD;
        ps.srTarget = PUSHCORE_SR_TARGET_4K;
    }
    if (!open) {
        const PushCoreSettings& o = pc->ps;
        if (ps.sr && ps.srMethod == o.srMethod) ps.srQuality = o.srQuality;
        // the multiplier stays as opened (the caller's pictures are made for it), and with another one the frame
        // generation method Auto chose with it; at the same multiplier the method follows the class (animation: DLSS-G
        // after PushFlow, PushMpcAuto.h)
        if (ps.multiplier != o.multiplier) {
            ps.multiplier = o.multiplier; ps.fgBackend = o.fgBackend; ps.vfgQuality = o.vfgQuality; ps.fgTier = o.fgTier;
            ps.fgLite = o.fgLite; ps.cutGuard = o.cutGuard; ps.subtitleHold = o.subtitleHold;
        }
    }
    // held-frame re-timing (as the filter: with frame generation, unless Animation is Off), only for a caller that takes
    // a pair's frames late (pushcore's lookahead); the animation mode and the holds come with it
    const bool heldCan = (features & PUSHCORE_FEATURE_HELD_FRAMES) && ps.multiplier > 1 && s.enable;
    int held = 0;
    if (cfg.held_frames == PM_PC_AUTO) held = heldCan && realOut && cfg.held_wait && s.animation != pm::kOff ? (s.animation == pm::kAuto ? 1 : 2) : 0;
    else if (cfg.held_frames > 0) held = heldCan ? std::clamp(cfg.held_frames, 1, 2) : 0;
    ps.heldFrames = held;
    ps.animationMode = 0; ps.holdFlags = 0;
    if ((features & PUSHCORE_FEATURE_ANIME_HOLD) && held) { ps.animationMode = c.animMode; ps.holdFlags = c.holdFlags; }
}

static void Describe(pm_pc* pc, const char* how, double ms, bool device)
{
    CoreHost& host = CoreHost::Get();
    const PushCoreOutputInfo& oi = host.OutInfo();
    const pm::Choice& c = pc->c;
    const PushCoreSettings& ps = pc->ps;
    std::string sr = "source size";
    if (ps.sr) sr = std::format("upscaler to {}x{}: {}", oi.width, oi.height, ps.srMethod == PUSHCORE_SR_METHOD_PMML
        ? "anime upscaler " + Narrow(pm::AnimeSrModel(int(ps.animeSrModel)).id) : std::string(ps.srMethod == PUSHCORE_SR_METHOD_RTX ? "RTX VSR" : ps.srMethod == PUSHCORE_SR_METHOD_FSR ? "FSR" : ps.srMethod == PUSHCORE_SR_METHOD_IVSR ? "Intel VSR" : ps.srMethod == PUSHCORE_SR_METHOD_XESS ? "XeSS" : "NIS"));
    pc->describe = std::format("{} ({} in {:.0f} ms on {}; Auto: x{} {}; {}; held frames {}{}{}; deband {}; content {}{}){}", oi.description, how, ms,
                               host.DeviceName(), c.multiplier, Narrow(pm::FgMethodLabel(c.fgMethod)), sr, ps.heldFrames,
                               ps.holdFlags ? std::format(", holds 0x{:X}", ps.holdFlags) : std::string(),
                               ps.heldFrames ? std::format(", animation mode {}", ps.animationMode) : std::string(),
                               ps.deband ? std::format("on ({})", ps.debandStrength) : std::string("off"), pc->cfg.content_class,
                               pc->cfg.content_cg ? " (CG)" : "", device ? "; zero copy on the player's D3D11 device" : "");
}

// the host's callbacks for this owner (set again after every acquire: CoreHost::Release clears them)
static void Hook(pm_pc* pc)
{
    CoreHost& host = CoreHost::Get();
    if (pc->zc) {
        host.sourceTex = [pc](int64_t, ID3D11Texture2D*& tex, uint32_t& slice) { tex = pc->zc->curTex; slice = pc->zc->curSlice; return tex != nullptr; };
        host.rawOut = [pc](const PushCoreFrame& fr, std::string& err) { return ZcOut(pc, fr, err); };
        host.source = nullptr; host.alloc = nullptr; host.release = nullptr; host.tag = nullptr;
        return;
    }
    host.sourceTex = nullptr; host.rawOut = nullptr;
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
        if ((!generated && !pc->realOut) || !pc->dest) return;
        const pm_pc_out* d = pc->dest(pc->user, pts);
        if (!d) return;
        auto* b = static_cast<std::vector<uint8_t>*>(handle);
        const uint32_t w = pc->ow, h = pc->oh;
        const size_t bps = pc->bits > 8 ? 2 : 1, ysz = size_t(w) * h * bps, csz = size_t(w / 2) * (h / 2) * bps;
        if (b->size() < ysz + 2 * csz) return;
        const uint8_t* srcp[3] = { b->data(), b->data() + ysz, b->data() + ysz + csz };
        for (int i = 0; i < 3; ++i) {
            const uint32_t pw = (i ? w / 2 : w), ph = (i ? h / 2 : h);
            for (uint32_t r = 0; r < ph; ++r) memcpy(d->data[i] + ptrdiff_t(r) * d->stride[i], srcp[i] + size_t(r) * pw * bps, pw * bps);
        }
        pc->handed++;
        if (pc->done) pc->done(pc->user, pts);
    };
}

// pushcore for the current pc->ps (open, or setSettings + reload when only the settings differ)
static bool AcquireFor(pm_pc* pc, Reuse& how, double& ms, std::string& e)
{
    CoreHost& host = CoreHost::Get();
    pc->key.settings.assign(reinterpret_cast<const uint8_t*>(&pc->ps), reinterpret_cast<const uint8_t*>(&pc->ps) + sizeof(pc->ps));
    how = Reuse::Open; ms = 0;
    pc->token = host.Acquire(pc->key, pc->si, pc->ps, how, ms, e);
    if (!pc->token) { e = "pushcore_open: " + e; return false; }
    Hook(pc);
    return true;
}

// Auto for this stream, up to the settings pushcore would get (the host loaded; no device, no core taken)
static void Prepare(pm_pc* pc, const pm_pc_cfg* cfg)
{
    CoreHost& host = CoreHost::Get();
    pc->cfg = *cfg;
    pc->cfg.core_dir = nullptr;   // the caller's string is not kept
    pc->w = cfg->width; pc->h = cfg->height; pc->bits = cfg->bits;
    pc->realOut = cfg->real_out != 0;
    // Auto with the user's PushMPC settings; the route's limits are applied in Build
    pm::Settings& s = pc->s;
    s = pm::LoadSettings();
    if (cfg->smoother == 1) s.smoother = pm::kOff;
    else if (cfg->smoother >= 2) s.smoother = std::clamp(cfg->smoother, 2, std::max(2, pm::VfgCeiling()));   // the core's own ceiling (16 with PUSHCORE_FEATURE_VFG16, else 8)
    s.cleanUp = pm::kOff; s.sdrToHdr = pm::kOff;
    pm::Gpu& g = pc->g;
    g.available = 0; g.hardware = 0; g.family = pm::kFamUnknown;
    PushCoreCaps caps;
    if (host.Caps(caps)) {
        g.family = caps.family; g.available = caps.available; g.hardware = caps.hardware; g.name = Widen(caps.adapter);
        for (uint32_t i = 0; i < pm::kCapCount && i < PUSHCORE_CAP_COUNT; ++i) g.reason[i] = Widen(caps.reason[i]);
        pm::ReadMeasured(g);   // the Intel methods' self-test numbers
    }
    pc->costs.card = pm::CardFactor(g.name, g.family);
    LoadLearned(pc->costs, g.name);
    pm::Source& src = pc->src;
    src.w = cfg->width; src.h = cfg->height;
    src.fps = cfg->fps_num > 0 && cfg->fps_den > 0 ? double(cfg->fps_num) / cfg->fps_den : 23.976;
    src.hdr = IsHdrTransfer(cfg->transfer); src.tenBit = cfg->bits > 8;
    // the display: the player's window is not known here (the primary monitor; VRR as for a windowed player), full screen
    // from the player's own option (cfg->fullscreen), which also lets G-SYNC fullscreen-only count (ResolveDisplay)
    pc->real = ResolveDisplay(pm::QueryDisplay(nullptr, s.vrr, g.nvidia()), true, s.vrr, cfg->display_hz, -1, cfg->fullscreen ? 1 : 0);
    pc->disp = pc->real;
    pc->disp.hdr = src.hdr;   // HDR sources stay HDR10 (the player presents them); SDR stays SDR
    Build(pc, *cfg, true, pc->c, pc->ps);
}

// the configuration's checks and the host's load (the open's first steps; pm_pc_decide stops after them)
static bool LoadFor(const pm_pc_cfg* cfgIn, pm_pc_cfg& cfgv, std::string& e)
{
    if (!ReadCfg(cfgIn, cfgv, e)) return false;
    if (!(cfgv.matrix == 1 || cfgv.matrix == 9 || cfgv.matrix == 10)) {
        e = std::format("pm_pushcore: matrix {} (SD / BT.601 sources stay on the portable engine)", cfgv.matrix);
        return false;
    }
    CoreHost& host = CoreHost::Get();
    HostPaths paths;
    paths.coreDir = cfgv.core_dir && *cfgv.core_dir ? cfgv.core_dir : DefaultCoreDir();
    const std::wstring rt = LocalAppDataDir() + L"\\PushMPC\\runtime";
    paths.runtimeDir = GetFileAttributesW(rt.c_str()) != INVALID_FILE_ATTRIBUTES ? rt : paths.coreDir;
    const std::wstring sh = paths.coreDir + L"\\shaders";
    if (GetFileAttributesW(sh.c_str()) != INVALID_FILE_ATTRIBUTES) paths.shaderDir = sh;
    if (!host.Load(paths, e)) return false;
    pm::VfgCeiling() = (host.Features() & PUSHCORE_FEATURE_VFG16) ? int(PUSHCORE_MAX_MULTIPLIER) : 8;   // VFG above 8x only with a core that has it
    pm::PushFlowCeiling() = (host.Features() & PUSHCORE_FEATURE_PUSHFLOW16) ? int(PUSHCORE_MAX_MULTIPLIER) : 4;   // PushFlow above 4x (flow once) the same
    return true;
}

static pm_pc* OpenImpl(const pm_pc_cfg* cfgIn, ID3D11Device* device, HANDLE mutex, int* kOut, char* err, size_t errsize)
{
    if (kOut) *kOut = 1;
    pm_pc_cfg cfgv;
    std::string e;
    CoreHost& host = CoreHost::Get();
    std::lock_guard<std::mutex> lk(host.Lock());
    MutexLock vlc(mutex);   // with a player's device, its immediate context is the player's
    if (!LoadFor(cfgIn, cfgv, e)) { SetErr(err, errsize, e); return nullptr; }
    const pm_pc_cfg* cfg = &cfgv;
    if (device ? !host.AdoptDevice(device, e) : !host.EnsureDevice(0, cfg->warp ? AdapterPick::Warp : AdapterPick::Auto, e)) {
        SetErr(err, errsize, e); return nullptr;
    }

    auto* pc = new pm_pc();
    Prepare(pc, cfg);

    PushCoreStreamInfo& si = pc->si;
    si.structSize = sizeof(si);
    si.width = cfg->width; si.height = cfg->height;
    si.format = cfg->bits > 8 ? PUSHCORE_INPUT_P010 : PUSHCORE_INPUT_NV12;
    si.fpsNum = cfg->fps_num; si.fpsDen = cfg->fps_den;
    si.range = cfg->range; si.matrix = cfg->matrix; si.primaries = cfg->primaries; si.transfer = cfg->transfer;
    si.displayHdr = pc->disp.hdr ? 1u : 0u;
    CoreKey& key = pc->key;
    key.width = si.width; key.height = si.height; key.format = si.format; key.fpsNum = si.fpsNum; key.fpsDen = si.fpsDen;
    key.range = si.range; key.matrix = si.matrix; key.primaries = si.primaries; key.transfer = si.transfer; key.displayHdr = si.displayHdr;
    key.adapterLuid = host.DeviceLuid();
    Reuse how = Reuse::Open; double ms = 0;
    if (!AcquireFor(pc, how, ms, e)) { SetErr(err, errsize, e); delete pc; return nullptr; }
    const PushCoreOutputInfo& oi = host.OutInfo();
    if (!cfg->sr && (oi.width != cfg->width || oi.height != cfg->height)) {
        host.Release(pc->token);
        SetErr(err, errsize, std::format("pushcore output {}x{} differs from the source {}x{}", oi.width, oi.height, cfg->width, cfg->height));
        delete pc; return nullptr;
    }
    if (cfg->sr && ((oi.width & 1) || (oi.height & 1) || oi.width < 16 || oi.height < 16)) {
        host.Release(pc->token);
        SetErr(err, errsize, std::format("pushcore output {}x{} cannot be handed out as 4:2:0", oi.width, oi.height));
        delete pc; return nullptr;
    }
    pc->ow = oi.width; pc->oh = oi.height;
    if (device) {
        pc->zc = new Zc();
        pc->zc->mutex = mutex;
        pc->zc->hdrMatrix = oi.hdrOutput != 0;
        if (!ZcSetup(*pc->zc, device, pc->ow, pc->oh, cfg->bits > 8, e)) {
            host.Release(pc->token); SetErr(err, errsize, "zero copy: " + e); delete pc->zc; delete pc; return nullptr;
        }
    } else if (!host.ConfigureEgress(cfg->bits > 8 || oi.hdrOutput ? OutFmt::Yuv420P10 : OutFmt::Yuv420P8, oi.hdrOutput != 0, e)) {
        host.Release(pc->token); SetErr(err, errsize, "egress: " + e); delete pc; return nullptr;
    }
    pc->k = int(std::clamp<uint32_t>(oi.multiplier ? oi.multiplier : pc->ps.multiplier, 1, PUSHCORE_MAX_MULTIPLIER));
    Describe(pc, ReuseName(how), ms, device != nullptr);
    Log("VLC: " + pc->describe);
    Hook(pc);   // the zero-copy objects exist now
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

extern "C" uint32_t pm_pc_abi(void) { return PM_PC_ABI; }

extern "C" int pm_pc_update(pm_pc* pc, const pm_pc_cfg* cfgIn, char* err, size_t errsize)
{
    if (!pc) { SetErr(err, errsize, "no instance"); return -1; }
    pm_pc_cfg cfg;
    std::string e;
    if (!ReadCfg(cfgIn, cfg, e)) { SetErr(err, errsize, e); return -1; }
    const pm_pc_cfg& o = pc->cfg;
    if (cfg.width != o.width || cfg.height != o.height || cfg.bits != o.bits || cfg.sr != o.sr ||
        (cfg.sr && (cfg.out_width != o.out_width || cfg.out_height != o.out_height))) {
        SetErr(err, errsize, "the stream or its output differs from the open's: open again"); return -1;
    }
    // what may change: the measured content and the caller's own switches; real_out too (a caller that turns held
    // frames on once the picture is measured as drawn: its real frames come from pushcore from the next picture on)
    pm_pc_cfg next = o;
    next.content_class = cfg.content_class; next.content_cg = cfg.content_cg; next.anime_model = cfg.anime_model;
    next.held_frames = cfg.held_frames; next.deband = cfg.deband; next.held_wait = cfg.held_wait;   // measure stays the open's
    next.real_out = cfg.real_out;
    CoreHost& host = CoreHost::Get();
    std::lock_guard<std::mutex> lk(host.Lock());
    if (!host.Owns(pc->token)) { SetErr(err, errsize, "pushcore was taken over by another instance"); return -1; }
    pm::Choice c;
    PushCoreSettings ps;
    Build(pc, next, false, c, ps);
    if (memcmp(&ps, &pc->ps, sizeof(ps)) == 0) { pc->cfg = next; pc->realOut = next.real_out != 0; return 1; }
    MutexLock vlc(pc->zc ? pc->zc->mutex : nullptr);
    const PushCoreSettings oldPs = pc->ps; const pm::Choice oldC = pc->c; const pm_pc_cfg oldCfg = pc->cfg;
    const uint32_t ow = pc->ow, oh = pc->oh;
    host.Release(pc->token);
    pc->ps = ps; pc->c = c; pc->cfg = next; pc->realOut = next.real_out != 0;
    Reuse how = Reuse::Open; double ms = 0;
    bool ok = AcquireFor(pc, how, ms, e);
    if (ok && (host.OutInfo().width != ow || host.OutInfo().height != oh)) { ok = false; e = std::format("the output became {}x{}", host.OutInfo().width, host.OutInfo().height); host.Release(pc->token); }
    if (!ok) {
        // back to what ran (the core reloads with it); the caller keeps playing
        pc->ps = oldPs; pc->c = oldC; pc->cfg = oldCfg; pc->realOut = oldCfg.real_out != 0;
        std::string e2;
        if (!AcquireFor(pc, how, ms, e2)) { SetErr(err, errsize, "update failed (" + e + ") and the open settings did not come back: " + e2); return -2; }
        SetErr(err, errsize, "update not applied: " + e);
        return -1;
    }
    if (!pc->zc) {   // the egress format is per stream; set again (the core may have been reopened)
        const PushCoreOutputInfo& oi = host.OutInfo();
        host.ConfigureEgress(pc->bits > 8 || oi.hdrOutput ? OutFmt::Yuv420P10 : OutFmt::Yuv420P8, oi.hdrOutput != 0, e);
    }
    pc->updates++;
    Describe(pc, ReuseName(how), ms, pc->zc != nullptr);
    Log("VLC: update: " + pc->describe);
    return 0;
}

extern "C" int pm_pc_output(pm_pc* pc, pm_pc_info* info)
{
    if (!pc || !info || info->struct_size < sizeof(pm_pc_info)) return -1;
    pm_pc_info i = {};
    i.struct_size = sizeof(i);
    i.width = pc->ow; i.height = pc->oh; i.k = pc->k;
    i.sr = pc->ps.sr ? 1 : 0; i.sr_method = pc->ps.srMethod;
    i.anime_model = pc->ps.sr && pc->ps.srMethod == PUSHCORE_SR_METHOD_PMML ? int(pc->ps.animeSrModel) : -1;
    i.held_frames = pc->ps.heldFrames; i.animation_mode = pc->ps.animationMode; i.hold_flags = pc->ps.holdFlags;
    i.deband = pc->ps.deband ? 1 : 0; i.deband_strength = pc->ps.debandStrength;
    i.real_out = pc->realOut ? 1 : 0;
    i.content_class = pc->cfg.content_class; i.content_cg = pc->cfg.content_cg;
    i.updates = pc->updates;
    i.measured_class = pc->measuredClass; i.measured_cg = pc->measuredCg;
    *info = i;
    return 0;
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
    if (pc->cfg.measure && pc->cdState >= 0) {   // ContentDetect on the same texture, with the player's lock held
        if (pc->cdState == 0) {
            std::string why;
            pc->cdState = pc->cdGpu.Init(host.Device(), pc->w, pc->h, pc->bits > 8, pc->cfg.range != 2, why) ? 1 : -1;
            Log(pc->cdState > 0 ? std::string("VLC: content measured on the GPU (the player's textures: the cells only)") : "VLC: content not measured: " + why);
            if (pc->cdState > 0) pc->cdTap = std::make_unique<pme::ContentTap>();
        }
        if (pc->cdState > 0) {
            pc->cdGpu.Record(host.Context(), static_cast<ID3D11Texture2D*>(texture), slice, double(pts100ns) / 1e7, discontinuity != 0, *pc->cdTap);
            const pme::ContentTap::Snapshot sn = pc->cdTap->Poll();
            pc->measuredClass = sn.cls; pc->measuredCg = sn.cg ? 1 : 0;
        }
    }
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
    if (pc->measureOnly) { delete pc; return; }   // never took the core
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

// ---- sharing the process's one pushcore (pm_pushcore.h: pm_pc_decide, pm_pc_busy, pm_pc_measure_*)
extern "C" int pm_pc_decide(const pm_pc_cfg* cfgIn, int* kOut, char* err, size_t errsize)
{
    if (kOut) *kOut = 1;
    pm_pc_cfg cfgv;
    std::string e;
    CoreHost& host = CoreHost::Get();
    std::lock_guard<std::mutex> lk(host.Lock());
    if (!LoadFor(cfgIn, cfgv, e)) { SetErr(err, errsize, e); return -1; }
    auto pc = std::make_unique<pm_pc>();
    Prepare(pc.get(), &cfgv);
    const int k = int(std::clamp<uint32_t>(pc->ps.multiplier, 1, PUSHCORE_MAX_MULTIPLIER));
    if (kOut) *kOut = k;
    SetErr(err, errsize, std::format("Auto: x{} {}", k, Narrow(pm::FgMethodLabel(pc->c.fgMethod))));
    Log(std::format("VLC: decided without opening: x{} {}", k, Narrow(pm::FgMethodLabel(pc->c.fgMethod))));
    return 0;
}

extern "C" int pm_pc_busy(void) { return CoreHost::Get().Busy() ? 1 : 0; }

extern "C" pm_pc* pm_pc_measure_open(const pm_pc_cfg* cfgIn, void* device, void* mutex, char* err, size_t errsize)
{
    pm_pc_cfg cfg;
    std::string e;
    if (!device) { SetErr(err, errsize, "no D3D11 device"); return nullptr; }
    if (!ReadCfg(cfgIn, cfg, e)) { SetErr(err, errsize, e); return nullptr; }
    if (cfg.width < 16 || cfg.height < 16) { SetErr(err, errsize, "pm_pushcore: no source size"); return nullptr; }
    auto* dev = static_cast<ID3D11Device*>(device);
    auto pc = std::make_unique<pm_pc>();
    pc->cfg = cfg; pc->cfg.core_dir = nullptr;
    pc->w = cfg.width; pc->h = cfg.height; pc->bits = cfg.bits;
    pc->measureOnly = true;
    pc->mmutex = static_cast<HANDLE>(mutex);
    dev->GetImmediateContext(&pc->mctx);
    std::string why;
    if (!pc->mctx || !pc->cdGpu.Init(dev, pc->w, pc->h, pc->bits > 8, cfg.range != 2, why)) {
        SetErr(err, errsize, "content not measured: " + (why.empty() ? std::string("no immediate context") : why)); return nullptr;
    }
    pc->cdTap = std::make_unique<pme::ContentTap>();
    pc->cdState = 1;
    pc->describe = std::format("ContentDetect alone on the player's device ({}x{}, {}-bit)", pc->w, pc->h, pc->bits);
    Log("VLC: content measured on the GPU without pushcore (the player's textures: the cells only)");
    return pc.release();
}

extern "C" int pm_pc_measure_push(pm_pc* pc, void* texture, unsigned slice, int64_t pts100ns, int discontinuity, char* err, size_t errsize)
{
    if (!pc || !pc->measureOnly || !pc->cdTap) { SetErr(err, errsize, "not a measuring instance (pm_pc_measure_open)"); return -1; }
    if (!texture) { SetErr(err, errsize, "no texture"); return -1; }
    MutexLock vlc(pc->mmutex);
    pc->cdGpu.Record(pc->mctx.Get(), static_cast<ID3D11Texture2D*>(texture), slice, double(pts100ns) / 1e7, discontinuity != 0, *pc->cdTap);
    const pme::ContentTap::Snapshot sn = pc->cdTap->Poll();
    pc->measuredClass = sn.cls; pc->measuredCg = sn.cg ? 1 : 0;
    return 0;
}
