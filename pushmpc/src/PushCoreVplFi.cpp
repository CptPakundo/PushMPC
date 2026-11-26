// PushMPC: Intel VPL AI frame interpolation on the D3D11 bridge (see PushCoreVplFi.h).
#include "PushCoreVplFi.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <format>
#include <mutex>
#include <thread>
#include <vector>

#ifdef PUSHMPC_HAVE_VPL
#include <vpl/mfx.h>
#endif

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

namespace pushmpc {
namespace {
double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

// ---------------------------------------------------------------- procedural held-out material (no VPL needed)
// A 1024 x 1024 pattern (value noise in three octaves, lines, blocks) seen through a slow zoom, a small rotation and a
// sinusoidal pan: up to ~25 px between input frames (30 fps) at 1080p, like the VPL probe's testsrc2 material.
struct Material {
    static constexpr int N = 1024;
    std::vector<float> tex;   // RGB
    Material() : tex(size_t(N) * N * 3)
    {
        auto hash = [](uint32_t x, uint32_t y, uint32_t s) { uint32_t h = x * 374761393u + y * 668265263u + s * 2246822519u; h = (h ^ (h >> 13)) * 1274126177u; return float((h ^ (h >> 16)) & 0xFFFF) / 65535.0f; };
        auto noise = [&](float u, float v, int cell, uint32_t s) {
            const float x = u / cell, y = v / cell; const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
            const float fx = x - x0, fy = y - y0; const int m = N / cell;
            auto g = [&](int a, int b) { return hash(uint32_t((a % m + m) % m), uint32_t((b % m + m) % m), s); };
            const float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
            return (g(x0, y0) * (1 - sx) + g(x0 + 1, y0) * sx) * (1 - sy) + (g(x0, y0 + 1) * (1 - sx) + g(x0 + 1, y0 + 1) * sx) * sy;
        };
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x) {
                for (int c = 0; c < 3; ++c) {
                    float v = 0.6f * noise(float(x), float(y), 64, 1 + c) + 0.32f * noise(float(x), float(y), 16, 7 + c) + 0.08f * noise(float(x), float(y), 4, 13 + c);
                    if ((x / 96 + y / 80) % 2 == 0) v = v * 0.7f + 0.25f;
                    if (x % 128 < 2 || y % 112 < 2) v = 0.95f;
                    tex[(size_t(y) * N + x) * 3 + c] = std::clamp(v, 0.0f, 1.0f);
                }
            }
    }
    // frame at 60 fps index o, RGBA8, w x h
    void frame(int o, uint32_t w, uint32_t h, std::vector<uint8_t>& out) const
    {
        out.resize(size_t(w) * h * 4);
        const double t = o / 60.0;
        const double s = (1.25 - 0.08 * std::sin(t * 0.9)) * 1080.0 / h;        // texels per pixel (zoom)
        const double a = 0.03 * std::sin(t * 0.7);                               // rotation
        const double px = 300.0 * std::sin(t * 1.57) / s, py = 120.0 * std::sin(t * 1.1 + 0.4) / s;   // pan, ~25 px / 30 fps step
        const double ca = std::cos(a) * s, sa = std::sin(a) * s;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const double dx = x - w * 0.5 + px, dy = y - h * 0.5 + py;
                double u = 512 + dx * ca - dy * sa, v = 512 + dx * sa + dy * ca;
                u -= std::floor(u / N) * N; v -= std::floor(v / N) * N;
                const int x0 = int(u), y0 = int(v); const float fx = float(u - x0), fy = float(v - y0);
                const int x1 = (x0 + 1) % N, y1 = (y0 + 1) % N;
                uint8_t* p = &out[(size_t(y) * w + x) * 4];
                for (int c = 0; c < 3; ++c) {
                    auto T = [&](int xx, int yy) { return tex[(size_t(yy) * N + xx) * 3 + c]; };
                    const float val = (T(x0, y0) * (1 - fx) + T(x1, y0) * fx) * (1 - fy) + (T(x0, y1) * (1 - fx) + T(x1, y1) * fx) * fy;
                    p[c] = uint8_t(std::lround(val * 255.0f));
                }
                p[3] = 255;
            }
    }
};
double psnrLuma(const uint8_t* a, const uint8_t* b, uint32_t w, uint32_t h, size_t pitchA = 0)
{
    if (!pitchA) pitchA = size_t(w) * 4;
    double se = 0;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* p = a + y * pitchA + size_t(x) * 4; const uint8_t* q = b + (size_t(y) * w + x) * 4;
            const double ya = 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2], yb = 0.2126 * q[0] + 0.7152 * q[1] + 0.0722 * q[2];
            se += (ya - yb) * (ya - yb);
        }
    const double mse = se / (double(w) * h);
    return mse > 0 ? 10.0 * std::log10(255.0 * 255.0 / mse) : 99.0;
}
}  // namespace

std::string vplMaterialBaselines(uint32_t w, uint32_t h)
{
    Material m;
    std::vector<uint8_t> a, b, c, blend;
    double sb = 0, sr = 0; int n = 0;
    for (int k = 2; k < 12; ++k) {   // pairs (2k, 2k+2), truth 2k+1
        m.frame(2 * k, w, h, a); m.frame(2 * k + 2, w, h, c); m.frame(2 * k + 1, w, h, b);
        blend.resize(a.size());
        for (size_t i = 0; i < a.size(); ++i) blend[i] = uint8_t((int(a[i]) + c[i] + 1) / 2);
        sb += psnrLuma(blend.data(), b.data(), w, h); sr += psnrLuma(a.data(), b.data(), w, h); ++n;
    }
    return std::format("procedural material {}x{}: blend {:.2f} dB, repeat {:.2f} dB (mean of {} held-out frames)", w, h, sb / n, sr / n, n);
}

#ifdef PUSHMPC_HAVE_VPL
namespace {
std::string sts(mfxStatus s) { return std::format("mfxStatus {}", int(s)); }
uint32_t fourccOf(DXGI_FORMAT f) { return f == DXGI_FORMAT_R8G8B8A8_UNORM ? MFX_FOURCC_BGR4 : f == DXGI_FORMAT_R10G10B10A2_UNORM ? MFX_FOURCC_A2RGB10 : f == DXGI_FORMAT_NV12 ? MFX_FOURCC_NV12 : MFX_FOURCC_P010; }
void fillInfo(mfxFrameInfo& fi, uint32_t fourcc, uint32_t w, uint32_t h, mfxU32 rateN, mfxU32 rateD)
{
    fi = {};
    fi.FourCC = fourcc;
    fi.ChromaFormat = (fourcc == MFX_FOURCC_NV12 || fourcc == MFX_FOURCC_P010) ? mfxU16(MFX_CHROMAFORMAT_YUV420) : mfxU16(MFX_CHROMAFORMAT_YUV444);
    fi.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    fi.Width = mfxU16((w + 15) & ~15u); fi.Height = mfxU16((h + 15) & ~15u);
    fi.CropW = mfxU16(w); fi.CropH = mfxU16(h);
    fi.FrameRateExtN = rateN; fi.FrameRateExtD = rateD;
    const bool ten = fourcc == MFX_FOURCC_P010 || fourcc == MFX_FOURCC_A2RGB10;
    fi.BitDepthLuma = fi.BitDepthChroma = ten ? 10 : 8;
    if (fourcc == MFX_FOURCC_P010) fi.Shift = 1;
}
}  // namespace

struct VplFi::Impl {
    D3D11Bridge* b = nullptr;
    mfxLoader loader = nullptr;
    mfxSession ses = nullptr;
    bool inited = false, haveFirst = false;
    uint32_t w = 0, h = 0, mult = 0, fourcc = 0;
    DXGI_FORMAT graphFormat = DXGI_FORMAT_UNKNOWN, vplFormat = DXGI_FORMAT_UNKNOWN;
    mfxVideoParam par{};
    mfxExtVPPAIFrameInterpolation fiExt{};
    mfxExtBuffer* ext[1]{};
    uint64_t order = 0;
    double last = -1;
    std::string implName;
    // NV12 / P010 route: the video processor converts in and out
    VideoBlit toYuv, fromYuv;
    ComPtr<ID3D11Texture2D> yuvIn, yuvOut;
    ComPtr<ID3D11Query> event;

    void build(uint32_t m, double fps)
    {
        const mfxU32 d = 1000, n = mfxU32(std::lround((fps > 1 ? fps : 24.0) * d));
        par = {};
        par.IOPattern = MFX_IOPATTERN_IN_VIDEO_MEMORY | MFX_IOPATTERN_OUT_VIDEO_MEMORY;
        par.AsyncDepth = 1;
        fillInfo(par.vpp.In, fourcc, w, h, n, d);
        fillInfo(par.vpp.Out, fourcc, w, h, n * m, d);
        fiExt = {}; fiExt.Header.BufferId = MFX_EXTBUFF_VPP_AI_FRAME_INTERPOLATION; fiExt.Header.BufferSz = sizeof(fiExt);
        fiExt.FIMode = MFX_AI_FRAME_INTERPOLATION_MODE_DEFAULT; fiExt.EnableScd = 1;
        ext[0] = &fiExt.Header;
        par.ExtParam = ext; par.NumExtParam = 1;
    }
    mfxStatus query()
    {
        mfxVideoParam out = par;
        mfxExtVPPAIFrameInterpolation o = fiExt; mfxExtBuffer* oe[1] = {&o.Header};
        out.ExtParam = oe;
        return MFXVideoVPP_Query(ses, &par, &out);
    }
    void closeSession()
    {
        if (ses) { if (inited) MFXVideoVPP_Close(ses); MFXClose(ses); ses = nullptr; }
        if (loader) { MFXUnload(loader); loader = nullptr; }
        inited = haveFirst = false;
    }
    bool waitIdle()
    {
        if (!event) { D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0}; if (FAILED(b->device()->CreateQuery(&qd, &event))) return false; }
        auto* c = b->context();
        c->End(event.Get()); c->Flush();
        BOOL done = FALSE; const auto t = Clock::now();
        while (c->GetData(event.Get(), &done, sizeof(done), 0) != S_OK) { if (Clock::now() - t > std::chrono::seconds(5)) return false; Sleep(0); }
        return true;
    }
    // the native texture of a VPL surface (single textures only)
    bool nativeTex(mfxFrameSurface1* s, ID3D11Texture2D*& t, std::string& why)
    {
        mfxHDL hdl = nullptr; mfxResourceType type{};
        const mfxStatus st = s->FrameInterface->GetNativeHandle(s, &hdl, &type);
        if (st != MFX_ERR_NONE || !hdl || type != MFX_RESOURCE_DX11_TEXTURE) { why = "GetNativeHandle " + sts(st) + std::format(" type {}", int(type)); return false; }
        t = static_cast<ID3D11Texture2D*>(hdl);
        D3D11_TEXTURE2D_DESC d{}; t->GetDesc(&d);
        if (d.ArraySize != 1) { why = std::format("VPL's surfaces are a texture array ({} slices): not supported", d.ArraySize); return false; }
        return true;
    }
    mfxStatus run(mfxFrameSurface1* in, mfxFrameSurface1* out, std::string& why)
    {
        mfxSyncPoint sp = nullptr; mfxStatus st = MFX_ERR_NONE;
        for (int i = 0;; ++i) {
            st = MFXVideoVPP_RunFrameVPPAsync(ses, in, out, nullptr, &sp);
            if (st != MFX_WRN_DEVICE_BUSY || i > 2000) break;
            Sleep(1);
        }
        if ((st == MFX_ERR_NONE || st == MFX_ERR_MORE_SURFACE || st > 0) && sp) {
            mfxStatus ss;
            do { ss = MFXVideoCORE_SyncOperation(ses, sp, 1000); } while (ss == MFX_WRN_IN_EXECUTION);
            if (ss < MFX_ERR_NONE) { why = "SyncOperation " + sts(ss); return ss; }
        }
        return st;
    }
};

VplFi::VplFi() : p_(std::make_unique<Impl>()) {}
VplFi::~VplFi() { close(); }
bool VplFi::built() { return true; }
bool VplFi::ready() const { return p_->inited; }
uint32_t VplFi::multiplier() const { return p_->mult; }
double VplFi::lastMs() const { return p_->last; }
std::string VplFi::describe() const
{
    const char* f = p_->fourcc == MFX_FOURCC_BGR4 ? "RGBA8" : p_->fourcc == MFX_FOURCC_A2RGB10 ? "RGB10A2" : p_->fourcc == MFX_FOURCC_NV12 ? "NV12 (via the video processor)" : "P010 (via the video processor)";
    return std::format("Intel VPL AI frame interpolation {}x, {}x{} {}, {}", p_->mult, p_->w, p_->h, f, p_->implName);
}
void VplFi::close() { if (p_) { p_->closeSession(); p_->toYuv.reset(); p_->fromYuv.reset(); p_->yuvIn.Reset(); p_->yuvOut.Reset(); p_->event.Reset(); } }

bool VplFi::init(D3D11Bridge& b, uint32_t w, uint32_t h, DXGI_FORMAT format, uint32_t wantMultiplier, double fps, std::string& why)
{
    close();
    auto& p = *p_;
    p.b = &b; p.w = w; p.h = h; p.graphFormat = format;
    if (b.vendorId() != 0x8086) { why = "needs an Intel GPU (VPL runs on Intel's driver)"; return false; }
    p.loader = MFXLoad();
    if (!p.loader) { why = "MFXLoad failed (no VPL dispatcher)"; return false; }
    // the hardware implementation on the bridge's adapter (LUID), on D3D11, that lists AI frame interpolation
    int pick = -1;
    for (mfxU32 i = 0; i < 64 && pick < 0; ++i) {
        mfxImplDescription* d = nullptr;
        const mfxStatus st = MFXEnumImplementations(p.loader, i, MFX_IMPLCAPS_IMPLDESCSTRUCTURE, reinterpret_cast<mfxHDL*>(&d));
        if (st != MFX_ERR_NONE || !d) break;
        bool ok = d->Impl == MFX_IMPL_TYPE_HARDWARE;
        bool d3d11 = false;
        for (mfxU16 k = 0; k < d->AccelerationModeDescription.NumAccelerationModes; ++k) d3d11 |= d->AccelerationModeDescription.Mode[k] == MFX_ACCEL_MODE_VIA_D3D11;
        ok = ok && (d3d11 || d->AccelerationMode == MFX_ACCEL_MODE_VIA_D3D11);
        p.implName = d->ImplName;
        MFXDispReleaseImplDescription(p.loader, d);
        if (!ok) continue;
        mfxExtendedDeviceId* e = nullptr;
        if (MFXEnumImplementations(p.loader, i, MFX_IMPLCAPS_DEVICE_ID_EXTENDED, reinterpret_cast<mfxHDL*>(&e)) == MFX_ERR_NONE && e) {
            uint64_t l = 0; std::memcpy(&l, e->DeviceLUID, 8);
            const bool match = !e->LUIDValid || l == b.adapterLuid();
            MFXDispReleaseImplDescription(p.loader, e);
            if (!match) continue;
        }
        pick = int(i);
    }
    if (pick < 0) { why = "no Intel VPL hardware implementation on this adapter (driver runtime missing?)"; close(); return false; }
    mfxStatus st = MFXCreateSession(p.loader, mfxU32(pick), &p.ses);
    if (st != MFX_ERR_NONE) { why = "MFXCreateSession " + sts(st); close(); return false; }
    st = MFXVideoCORE_SetHandle(p.ses, MFX_HANDLE_D3D11_DEVICE, b.device());
    if (st != MFX_ERR_NONE) { why = "SetHandle(D3D11 device) " + sts(st); close(); return false; }
    // formats: the graph's own first, then NV12 / P010 through the video processor; multipliers from want down to 2
    const bool ten = format == DXGI_FORMAT_R10G10B10A2_UNORM;
    const DXGI_FORMAT tries[2] = {format, ten ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12};
    std::string notes;
    for (DXGI_FORMAT f : tries) {
        p.fourcc = fourccOf(f); p.vplFormat = f;
        for (uint32_t m = std::clamp<uint32_t>(wantMultiplier, 2, 8); m >= 2 && !p.inited; --m) {
            p.build(m, fps);
            st = p.query();
            if (st < MFX_ERR_NONE) { notes += std::format("{}x {}: Query {}; ", m, f == format ? "native" : "yuv", int(st)); continue; }
            p.build(m, fps);
            st = MFXVideoVPP_Init(p.ses, &p.par);
            if (st < MFX_ERR_NONE) { notes += std::format("{}x: Init {}; ", m, int(st)); continue; }
            p.inited = true; p.mult = m;
        }
        if (p.inited) break;
    }
    if (!p.inited) { why = "the runtime refused AI frame interpolation: " + notes; close(); return false; }
    if (p.vplFormat != format) {
        D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = p.vplFormat; d.SampleDesc = {1, 0};
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(b.device()->CreateTexture2D(&d, nullptr, &p.yuvIn)) || FAILED(b.device()->CreateTexture2D(&d, nullptr, &p.yuvOut)) ||
            !p.toYuv.init(b, w, h, format, w, h, p.vplFormat, why) || !p.fromYuv.init(b, w, h, p.vplFormat, w, h, format, why)) {
            why = "YUV route: " + why; close(); return false;
        }
        p.toYuv.setColor(false, true, false, true, true);
        p.fromYuv.setColor(true, true, true, false, false);
    }
    return true;
}

bool VplFi::push(ID3D11Texture2D* real, bool discontinuity, ID3D11Texture2D* const* outputs, uint32_t& produced, std::string& why)
{
    auto& p = *p_;
    produced = 0;
    if (!p.inited) { why = "VPL not initialised"; return false; }
    const auto t0 = Clock::now();
    if (discontinuity && p.haveFirst) {
        // a reset: the next frame starts a new sequence (Close + Init keeps the session and the device)
        MFXVideoVPP_Close(p.ses);
        mfxStatus st = MFXVideoVPP_Init(p.ses, &p.par);
        if (st < MFX_ERR_NONE) { p.inited = false; why = "VPL re-init after a discontinuity " + sts(st); return false; }
        p.haveFirst = false;
    }
    auto* c = p.b->context();
    mfxFrameSurface1* in = nullptr;
    mfxStatus st = MFXMemory_GetSurfaceForVPP(p.ses, &in);
    if (st != MFX_ERR_NONE || !in) { why = "GetSurfaceForVPP " + sts(st); return false; }
    ID3D11Texture2D* inTex = nullptr;
    if (!p.nativeTex(in, inTex, why)) { in->FrameInterface->Release(in); return false; }
    if (p.vplFormat == p.graphFormat) c->CopySubresourceRegion(inTex, 0, 0, 0, 0, real, 0, nullptr);
    else {
        const HRESULT hr = p.toYuv.blit(real, 0, p.yuvIn.Get());
        if (FAILED(hr)) { in->FrameInterface->Release(in); why = std::format("RGB -> YUV blit 0x{:08X}", unsigned(hr)); return false; }
        c->CopySubresourceRegion(inTex, 0, 0, 0, 0, p.yuvIn.Get(), 0, nullptr);
    }
    if (!p.waitIdle()) { in->FrameInterface->Release(in); why = "the copy into VPL's surface did not finish"; return false; }
    in->Data.FrameOrder = mfxU32(p.order); in->Data.TimeStamp = mfxU64(p.order) * 3750; ++p.order;
    const bool first = !p.haveFirst;
    bool ok = true;
    for (uint32_t guard = 0; guard < p.mult + 2; ++guard) {
        mfxFrameSurface1* out = nullptr;
        if ((st = MFXMemory_GetSurfaceForVPPOut(p.ses, &out)) != MFX_ERR_NONE || !out) { why = "GetSurfaceForVPPOut " + sts(st); ok = false; break; }
        st = p.run(in, out, why);
        if (st == MFX_ERR_MORE_SURFACE && !first && produced + 1 < p.mult) {
            ID3D11Texture2D* o = nullptr;
            if (!p.nativeTex(out, o, why)) { out->FrameInterface->Release(out); ok = false; break; }
            if (p.vplFormat == p.graphFormat) c->CopySubresourceRegion(outputs[produced], 0, 0, 0, 0, o, 0, nullptr);
            else {
                c->CopySubresourceRegion(p.yuvOut.Get(), 0, 0, 0, 0, o, 0, nullptr);
                const HRESULT hr = p.fromYuv.blit(p.yuvOut.Get(), 0, outputs[produced]);
                if (FAILED(hr)) { out->FrameInterface->Release(out); why = std::format("YUV -> RGB blit 0x{:08X}", unsigned(hr)); ok = false; break; }
            }
            ++produced;
            out->FrameInterface->Release(out);
            continue;
        }
        out->FrameInterface->Release(out);
        if (st == MFX_ERR_MORE_SURFACE) continue;            // more than we have room for: dropped
        if (st == MFX_ERR_NONE || st > 0 || st == MFX_ERR_MORE_DATA) break;   // the real frame's passthrough (not used)
        if (why.empty()) why = "RunFrameVPPAsync " + sts(st);
        ok = false; break;
    }
    in->FrameInterface->Release(in);
    if (ok) { p.haveFirst = true; c->Flush(); }
    p.last = msSince(t0);
    return ok;
}

// ---------------------------------------------------------------- self-test
namespace {
VplSelfTest selfTestBody(uint64_t luid, bool warp)
{
    VplSelfTest r; r.ran = true;
    D3D11Bridge b; std::string why;
    if (!(warp ? b.initWarp(nullptr, why) : b.init(nullptr, luid, why))) { r.reason = "fail: " + why; return r; }
    if (b.vendorId() != 0x8086) {
        // the dispatcher still loads and looks for runtimes (a runner: none, or none on this adapter)
        int impls = 0;
        if (mfxLoader l = MFXLoad()) {
            for (mfxU32 i = 0; i < 64; ++i) { mfxImplDescription* d = nullptr; if (MFXEnumImplementations(l, i, MFX_IMPLCAPS_IMPLDESCSTRUCTURE, reinterpret_cast<mfxHDL*>(&d)) != MFX_ERR_NONE || !d) break; ++impls; MFXDispReleaseImplDescription(l, d); }
            MFXUnload(l);
        }
        r.reason = std::format("unavailable: needs an Intel GPU (VPL runs on Intel's driver); adapter vendor {:04X}; dispatcher loaded, {} VPL implementation(s) found", b.vendorId(), impls);
        return r;
    }
    const uint32_t W = 1920, H = 1080;
    // the largest multiplier the runtime takes (Query only)
    for (uint32_t m = 4; m >= 2; --m) { VplFi probe; std::string e; if (probe.init(b, 320, 180, DXGI_FORMAT_R8G8B8A8_UNORM, m, 30.0, e) && probe.multiplier() == m) { r.maxMultiplier = m; break; } }
    VplFi fi;
    if (!fi.init(b, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, 2, 30.0, why)) { r.reason = "fail: " + why; return r; }
    Material mat;
    D3D11_TEXTURE2D_DESC d{}; d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc = {1, 0};
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> real, gen, staging;
    D3D11_TEXTURE2D_DESC sd = d; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(b.device()->CreateTexture2D(&d, nullptr, &real)) || FAILED(b.device()->CreateTexture2D(&d, nullptr, &gen)) ||
        FAILED(b.device()->CreateTexture2D(&sd, nullptr, &staging))) { r.reason = "fail: textures"; return r; }
    std::vector<uint8_t> px, truth, blendBuf, prev;
    double sp = 0, sb = 0, sms = 0; int n = 0, nms = 0; double minStd = 1e9;
    const int pairs = 14, warm = 2;
    for (int k = 0; k <= pairs; ++k) {
        mat.frame(2 * k, W, H, px);
        b.context()->UpdateSubresource(real.Get(), 0, nullptr, px.data(), W * 4, 0);
        uint32_t produced = 0;
        ID3D11Texture2D* outs[1] = {gen.Get()};
        if (!fi.push(real.Get(), k == 0, outs, produced, why)) { r.reason = "fail: " + why; return r; }
        if (k > warm) { sms += fi.lastMs(); ++nms; }
        if (k > 0 && produced == 1 && k > warm) {
            mat.frame(2 * k - 1, W, H, truth);
            b.context()->CopyResource(staging.Get(), gen.Get());
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(b.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) { r.reason = "fail: Map"; return r; }
            const auto* g = static_cast<const uint8_t*>(m.pData);
            sp += psnrLuma(g, truth.data(), W, H, m.RowPitch);
            double mean = 0, q = 0;
            for (uint32_t y = 0; y < H; y += 4) for (uint32_t x = 0; x < W; x += 4) { const double v = g[y * m.RowPitch + x * 4 + 1]; mean += v; q += v * v; }
            const double cnt = double(H / 4) * (W / 4); mean /= cnt; minStd = std::min(minStd, std::sqrt(std::max(0.0, q / cnt - mean * mean)));
            b.context()->Unmap(staging.Get(), 0);
            blendBuf.resize(px.size());
            for (size_t i = 0; i < px.size(); ++i) blendBuf[i] = uint8_t((int(prev[i]) + px[i] + 1) / 2);
            sb += psnrLuma(blendBuf.data(), truth.data(), W, H);
            ++n;
        } else if (k > warm && k > 0 && produced != 1) { r.reason = std::format("fail: frame {} gave {} interpolated frames (1 expected)", k, produced); return r; }
        prev = px;
    }
    if (!n) { r.reason = "fail: no interpolated frame scored"; return r; }
    r.psnr = sp / n; r.psnrBlend = sb / n; r.msPerSource = nms ? sms / nms : -1;
    const bool blank = minStd < 5.0;
    r.pass = !blank && r.psnr >= r.psnrBlend + 1.0;
    // the T5b gate (>= 28.90 dB = PushFlow Fast on the VPL probe's material, where a 50/50 blend scores 23.43 dB) moved onto this
    // material as the same margin over its own blend: +5.47 dB; and <= 20.8 ms per source frame at 1080p
    r.gate = r.pass && r.psnr - r.psnrBlend >= 28.90 - 23.43 && r.msPerSource > 0 && r.msPerSource <= 20.8;
    r.reason = std::format("{}; psnr={:.2f} blend={:.2f} ms={:.2f} maxM={} gate={} ({})", r.pass ? "pass" : blank ? "fail: blank frames" : "fail: not better than a 50/50 blend",
                           r.psnr, r.psnrBlend, r.msPerSource, r.maxMultiplier, r.gate ? "pass" : "candidate only", fi.describe());
    return r;
}
}  // namespace

VplSelfTest vplFiSelfTest(uint64_t luid, bool warp, uint32_t timeoutMs)
{
    struct Shared { std::mutex m; std::condition_variable cv; bool done = false; VplSelfTest r; };
    auto s = std::make_shared<Shared>();
    std::thread([s, luid, warp] {
        VplSelfTest r;
        try { r = selfTestBody(luid, warp); } catch (...) { r.ran = true; r.reason = "fail: exception"; }
        std::lock_guard<std::mutex> lk(s->m); s->r = r; s->done = true; s->cv.notify_all();
    }).detach();
    std::unique_lock<std::mutex> lk(s->m);
    if (!s->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return s->done; })) {
        VplSelfTest r; r.ran = true; r.reason = std::format("fail: timed out after {} ms (a VPL call hung; abandoned)", timeoutMs);
        return r;
    }
    return s->r;
}

#else   // built without libvpl
struct VplFi::Impl {};
VplFi::VplFi() : p_(std::make_unique<Impl>()) {}
VplFi::~VplFi() = default;
bool VplFi::built() { return false; }
bool VplFi::ready() const { return false; }
uint32_t VplFi::multiplier() const { return 0; }
double VplFi::lastMs() const { return -1; }
std::string VplFi::describe() const { return "Intel VPL not built in"; }
void VplFi::close() {}
bool VplFi::init(D3D11Bridge&, uint32_t, uint32_t, DXGI_FORMAT, uint32_t, double, std::string& why) { why = "this pushcore was built without Intel VPL"; return false; }
bool VplFi::push(ID3D11Texture2D*, bool, ID3D11Texture2D* const*, uint32_t& produced, std::string& why) { produced = 0; why = "no VPL"; return false; }
VplSelfTest vplFiSelfTest(uint64_t, bool, uint32_t)
{
    VplSelfTest r; r.ran = true; r.reason = "unavailable: this pushcore was built without Intel VPL";
    return r;
}
#endif

}  // namespace pushmpc
