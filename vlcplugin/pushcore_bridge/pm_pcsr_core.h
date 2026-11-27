// pm_pcsr_core.h: pushcore's upscaler as an OUTPUT upscaler (PushMPC's fork of VLC 3's Direct3D 11 video output,
// display/vlc3/.../pm_pcsr_d3d11.cpp), as pure D3D11 + bridge logic without VLC, so it runs on WARP with the stub core
// (pm_pcsr_test.cpp). The output owns the scaler and its window-size texture; this part decides, per picture, whether
// pushcore's picture is drawn into that texture instead of the scaler's own result.
//
// Two modes:
//   Auto  (the output's default): the scaler is the GPU driver's Super Resolution (RTX / Intel VSR), else NIS, for every
//         picture; pushcore runs only with the anime model, and only while ContentDetect measures the picture as drawn
//         animation (DrawnPolicy.animeUpscaler; Kodi's AnimeAutoSwitch: the first switch at once, then at most one per
//         30 s), and only on the NIS scaler: next to the driver's Super Resolution the anime model waits for the
//         phase-4 verdict, as in every player (PcsrAutoRefusal, content_detect.h). Rule: fidelity to the source. For anything else the driver's upscaler on the decoder's picture is closer
//         to the source than pushcore's (whose output takes one more 4:2:0 round trip and a bilinear step to the window).
//         The class is measured without pushcore (pm_pc_measure_open: ContentDetect alone on the output's device).
//   Anime (asked for): pushcore's upscaler for every picture (its Video Super Resolution until the picture is measured
//         as drawn, then the anime model).
// Sharing: one pushcore per process, and PushMPC's frame generation filter in VLC's chain wants it too. The filter asks
// the bridge first (pm_pc_decide) and opens only when it generates frames; frame generation wins: its open takes the core
// over. Here the core is opened only when no instance holds it (pm_pc_busy), and any failure of an instance (taken over,
// or an open that did not work) closes it and tries again later with a capped exponential backoff (ShareBackoff); the
// scaler draws meanwhile. Nothing here gives the core up for the stream, except pushcore not keeping up (the late count).
// A bridge without those exports: the caller decides (it keeps its older check that the filter is absent).
// pm_pc_busy is lock-free and reads "free" while another instance is inside its open or update (before Acquire sets the
// owner). That is safe in VLC 3 because the filter's open and update and this Feed all run on the vout thread, one after
// the other; a caller with another threading would need the host's lock around the check and the open.
#pragma once
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include "pm_pushcore.h"
#include "win_anime_route.h"    // kodiaddon/src/engine: AnimeAutoSwitch (the measured class -> the anime model)
#include "PushMpcAnimeSr.h"     // filter/src: the models' ids and their index as the bridge takes it

namespace pmpc {

// ---------------------------------------------------------------------------------------------------- the backoff
// When to try the core again. Lost (taken over, an open or a call failed): 2 s, then doubling up to 60 s; Busy (another
// instance holds it now): look again in 0.5 s, the delay unchanged. A new stream starts over.
class ShareBackoff {
public:
    static constexpr double kFirstS = 2.0, kMaxS = 60.0, kBusyS = 0.5;
    void NewStream() { m_delay = 0; m_at = 0; }
    bool MayTry(double nowS) const { return nowS >= m_at; }
    void Lost(double nowS) { m_delay = m_delay <= 0 ? kFirstS : std::min(m_delay * 2, kMaxS); m_at = nowS + m_delay; ++m_lost; }
    void Busy(double nowS) { m_at = nowS + kBusyS; ++m_busy; }
    double Delay() const { return m_delay; }
    double At() const { return m_at; }
    int LostCount() const { return m_lost; }
    int BusyCount() const { return m_busy; }
private:
    double m_delay = 0, m_at = 0;
    int m_lost = 0, m_busy = 0;
};

// ---------------------------------------------------------------------------------------------------- the bridge
struct BridgeFns {
    pm_pc_open_d3d11_fn open = nullptr;
    pm_pc_push_d3d11_fn push = nullptr;
    pm_pc_poll_d3d11_fn pollTex = nullptr;   // not "poll": VLC's vlc_threads.h defines poll() as a macro
    pm_pc_close_fn close = nullptr;
    pm_pc_describe_fn describe = nullptr;
    pm_pc_update_fn update = nullptr;
    pm_pc_output_fn output = nullptr;
    pm_pc_busy_fn busy = nullptr;                   // optional (sharing)
    pm_pc_measure_open_fn measureOpen = nullptr;    // optional (Auto's measurement without the core)
    pm_pc_measure_push_fn measurePush = nullptr;
    bool Sharing() const { return busy != nullptr; }
    bool Measures() const { return measureOpen && measurePush; }
    // from a loaded pm_pushcore.dll (abi: what pm_pc_abi says, 2 without it)
    static BridgeFns From(HMODULE dll)
    {
        BridgeFns b;
        if (!dll) return b;
        auto sym = [dll](const char* n) { return reinterpret_cast<void*>(GetProcAddress(dll, n)); };
        b.open = reinterpret_cast<pm_pc_open_d3d11_fn>(sym("pm_pc_open_d3d11"));
        b.push = reinterpret_cast<pm_pc_push_d3d11_fn>(sym("pm_pc_push_d3d11"));
        b.pollTex = reinterpret_cast<pm_pc_poll_d3d11_fn>(sym("pm_pc_poll_d3d11"));
        b.close = reinterpret_cast<pm_pc_close_fn>(sym("pm_pc_close"));
        b.describe = reinterpret_cast<pm_pc_describe_fn>(sym("pm_pc_describe"));
        b.update = reinterpret_cast<pm_pc_update_fn>(sym("pm_pc_update"));
        b.output = reinterpret_cast<pm_pc_output_fn>(sym("pm_pc_output"));
        b.busy = reinterpret_cast<pm_pc_busy_fn>(sym("pm_pc_busy"));
        b.measureOpen = reinterpret_cast<pm_pc_measure_open_fn>(sym("pm_pc_measure_open"));
        b.measurePush = reinterpret_cast<pm_pc_measure_push_fn>(sym("pm_pc_measure_push"));
        return b;
    }
};

inline int AnimeModelIndexOf(const std::string& id)
{
    return id.empty() ? -1 : pm::AnimeSrModelIndex(std::wstring(id.begin(), id.end()));
}

// Auto's anime upscaler in the output, by the rules every player shares (content_detect.h): "" when it may run, else
// why not. driverSr: the output's scaler is the driver's Super Resolution (RTX / Intel VSR; not NIS): it stays first
// until the verdict. transfer (H.273): HDR is allowed, pushcore feeds the model its SDR-like proxy.
inline std::string PcsrAutoRefusal(bool driverSr, int transfer)
{
    if (!pushmpc::content::AutoAnimeSrBesideVsr(driverSr))
        return "the driver's Super Resolution stays first until the anime upscaler's verdict";
    if (!pushmpc::content::AutoAnimeSrTakesTransfer(pushmpc::content::AnimeSrFeed::PushcoreProxy, transfer))
        return "an HDR source (the anime models are made for SDR pictures): the driver's Super Resolution";
    return {};
}

// ---------------------------------------------------------------------------------------------------- the draw
// pushcore's NV12 / P010 picture (BT.709 or BT.2020, limited range) -> full-range RGB in the source's transfer (as the
// driver's scaler and NIS hand the output's quad), bilinear into place of the target (a render target of any format the
// scaler uses: BGRA8 / RGBA8 / RGB10A2 / RGBA16), in a device context state of its own (the player's state is kept).
class PcsrDraw {
public:
    bool Init(ID3D11Device* dev, std::string& why)
    {
        using Microsoft::WRL::ComPtr;
        if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&m_dev1)))) { why = "no ID3D11Device1 (Direct3D 11.1)"; return false; }
        const D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel();
        if (FAILED(m_dev1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &m_state))) {
            why = "CreateDeviceContextState failed"; return false;
        }
        typedef HRESULT (WINAPI *Compile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
        static HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
        const Compile compile = dc ? reinterpret_cast<Compile>(reinterpret_cast<void*>(GetProcAddress(dc, "D3DCompile"))) : nullptr;
        if (!compile) { why = "d3dcompiler_47.dll is missing"; return false; }
        ComPtr<ID3DBlob> vs, ps, eb;
        if (FAILED(compile(kHlsl, sizeof(kHlsl) - 1, "pm_pcsr_draw", nullptr, nullptr, "vs", "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &eb)) ||
            FAILED(compile(kHlsl, sizeof(kHlsl) - 1, "pm_pcsr_draw", nullptr, nullptr, "ps", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &eb))) {
            why = std::string("draw shader: ") + (eb ? static_cast<const char*>(eb->GetBufferPointer()) : "compile failed"); return false;
        }
        D3D11_BUFFER_DESC cb = {};
        cb.ByteWidth = sizeof(Consts); cb.Usage = D3D11_USAGE_DEFAULT; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_vs)) ||
            FAILED(dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &m_ps)) ||
            FAILED(dev->CreateBuffer(&cb, nullptr, &m_cb)) || FAILED(dev->CreateSamplerState(&sd, &m_smp))) {
            why = "draw objects"; return false;
        }
        return true;
    }
    // srvY / srvUV: the picture's planes (R8 / R8G8, or R16 / R16G16 for P010); target: the scaler's window texture;
    // place: where the picture goes in it; under the player's context lock
    bool Draw(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* srvY, ID3D11ShaderResourceView* srvUV, bool tenBit, bool bt2020,
              ID3D11Texture2D* target, unsigned x, unsigned y, unsigned w, unsigned h, std::string& why)
    {
        using Microsoft::WRL::ComPtr;
        if (!target || !w || !h) { why = "no target"; return false; }
        if (m_target.Get() != target) {
            m_rtv.Reset(); m_target = target;
            D3D11_TEXTURE2D_DESC td = {};
            target->GetDesc(&td);
            m_tw = td.Width; m_th = td.Height;
            if (!(td.BindFlags & D3D11_BIND_RENDER_TARGET)) { why = "the scaler's texture is not a render target"; m_target.Reset(); return false; }
            ComPtr<ID3D11Device> dev;
            ctx->GetDevice(&dev);
            if (FAILED(dev->CreateRenderTargetView(target, nullptr, &m_rtv))) { why = "no render target view on the scaler's texture"; m_target.Reset(); return false; }
        }
        if (x + w > m_tw || y + h > m_th) { why = "the place is outside the scaler's texture"; return false; }
        Consts c = {};
        const float kr = bt2020 ? 0.2627f : 0.2126f, kb = bt2020 ? 0.0593f : 0.0722f, kg = 1.f - kr - kb;
        c.rowR[0] = 1.f; c.rowR[1] = 0.f;                       c.rowR[2] = 2.f * (1.f - kr);
        c.rowG[0] = 1.f; c.rowG[1] = -2.f * (1.f - kb) * kb / kg; c.rowG[2] = -2.f * (1.f - kr) * kr / kg;
        c.rowB[0] = 1.f; c.rowB[1] = 2.f * (1.f - kb);           c.rowB[2] = 0.f;
        // limited range, exactly per bit depth (P010 holds the 10-bit code in the top bits of 16)
        const float one = tenBit ? 65535.f : 255.f, step = tenBit ? 64.f : 1.f, base = tenBit ? 4.f : 1.f;
        c.yOff = 16.f * base * step / one;  c.yScale = one / (219.f * base * step);
        c.cOff = 128.f * base * step / one; c.cScale = one / (224.f * base * step);
        c.origin[0] = float(x); c.origin[1] = float(y); c.size[0] = float(w); c.size[1] = float(h);
        ComPtr<ID3D11DeviceContext1> ctx1;
        if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1)))) { why = "no ID3D11DeviceContext1"; return false; }
        ID3DDeviceContextState* prev = nullptr;
        ctx1->SwapDeviceContextState(m_state.Get(), &prev);
        ctx->UpdateSubresource(m_cb.Get(), 0, nullptr, &c, 0, 0);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(m_vs.Get(), nullptr, 0);
        ctx->PSSetShader(m_ps.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srv[2] = { srvY, srvUV };
        ctx->PSSetShaderResources(0, 2, srv);
        ID3D11Buffer* cbs[1] = { m_cb.Get() };
        ctx->PSSetConstantBuffers(0, 1, cbs);
        ID3D11SamplerState* smp = m_smp.Get();
        ctx->PSSetSamplers(0, 1, &smp);
        const D3D11_VIEWPORT vp = { float(x), float(y), float(w), float(h), 0.f, 1.f };
        ctx->RSSetViewports(1, &vp);
        ID3D11RenderTargetView* rt = m_rtv.Get();
        ctx->OMSetRenderTargets(1, &rt, nullptr);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* none[2] = {};
        ctx->PSSetShaderResources(0, 2, none);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        ctx1->SwapDeviceContextState(prev, nullptr);
        if (prev) prev->Release();
        return true;
    }

private:
    struct Consts { float rowR[4], rowG[4], rowB[4]; float origin[2], size[2]; float yOff, yScale, cOff, cScale; };
    static constexpr char kHlsl[] = R"(
cbuffer C : register(b0) { float4 rowR; float4 rowG; float4 rowB; float2 origin; float2 size; float yOff; float yScale; float cOff; float cScale; };
Texture2D<float> py : register(t0);
Texture2D<float2> puv : register(t1);
SamplerState lin : register(s0);
struct V { float4 pos : SV_Position; };
V vs(uint id : SV_VertexID)
{
    V o;
    const float2 t = float2((id << 1) & 2, id & 2);
    o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 ps(V i) : SV_Target
{
    const float2 uv = (i.pos.xy - origin) / size;
    const float3 yuv = float3((py.SampleLevel(lin, uv, 0) - yOff) * yScale, (puv.SampleLevel(lin, uv, 0) - cOff) * cScale);
    return float4(saturate(float3(dot(rowR.xyz, yuv), dot(rowG.xyz, yuv), dot(rowB.xyz, yuv))), 1.0);
}
)";
    Microsoft::WRL::ComPtr<ID3D11Device1> m_dev1;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> m_state;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cb;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_smp;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_target;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_rtv;
    unsigned m_tw = 0, m_th = 0;
};

// ---------------------------------------------------------------------------------------------------- the route
enum class PcsrMode { Auto, Anime };

class PcsrCore {
public:
    using LogFn = std::function<void(bool warn, const std::string&)>;
    struct Setup {
        ID3D11Device* dev = nullptr;
        HANDLE mutex = nullptr;        // the player's context lock (the bridge takes it itself)
        pm_pc_cfg cfg{};               // sr 1 with the output size, anime_model NONE, measure 0 (it measures here)
        PcsrMode mode = PcsrMode::Auto;
        std::string autoModel;         // the anime model Auto picked for this source ("" = none)
        double frameMs = 1000.0 / 23.976;
        bool tenBit = false, hdr = false;
    };
    // feed: the whole call (an open in it too); wait: from the push to the end of the drain (the bound is on this part)
    struct Stats { int opens = 0, openFails = 0, lost = 0, updates = 0; double openMsMax = 0, feedMsMax = 0, feedMsSum = 0; int feeds = 0;
                   double waitMsMax = 0, waitMsSum = 0; int waits = 0; };

    PcsrCore() = default;
    PcsrCore(const PcsrCore&) = delete;
    PcsrCore& operator=(const PcsrCore&) = delete;
    ~PcsrCore() { Close(); if (m_meas && m_b.close) m_b.close(m_meas); }

    // false with the reason: nothing of pushcore runs for this stream (Auto without measurement, no draw objects)
    bool Init(const BridgeFns& b, const Setup& s, LogFn log, std::string& why)
    {
        m_b = b; m_s = s; m_log = std::move(log);
        m_s.cfg.sr = 1; m_s.cfg.measure = 0; m_s.cfg.held_frames = 0; m_s.cfg.deband = 0; m_s.cfg.held_wait = 0; m_s.cfg.real_out = 1;
        m_s.cfg.smoother = 1;          // x1: one picture out per picture
        if (!b.open || !b.push || !b.pollTex || !b.close || !b.output || !b.update) { why = "the bridge has no zero-copy path or no settings update"; return false; }
        if (!m_draw.Init(m_s.dev, why)) return false;
        if (b.Measures()) {
            char err[300] = "";
            m_meas = b.measureOpen(&m_s.cfg, m_s.dev, m_s.mutex, err, sizeof(err));
            if (!m_meas) { why = std::string("ContentDetect: ") + (err[0] ? err : "not measured"); if (m_s.mode == PcsrMode::Auto) return false; }
        } else if (m_s.mode == PcsrMode::Auto) {
            why = "the bridge cannot measure without pushcore (an older pm_pushcore.dll)"; return false;
        }
        if (m_s.mode == PcsrMode::Auto && m_s.autoModel.empty()) { why = "no anime model fits this source"; return false; }
        m_sw.Arm(m_s.autoModel);
        return true;
    }

    // a new picture (not a repeat of the last one); after it, Got() says whether pushcore's picture of it is ready.
    // false: pushcore does not keep up here (anime mode): the caller ends this for the stream
    bool Feed(ID3D11Texture2D* tex, unsigned slice, int64_t pts100, bool disc, double nowS)
    {
        const double t0 = Ms();
        m_got = false;
        // a discontinuity (a new file, a seek, a resume: VLC's picture dates are the system clock's, so a new stream does
        // not step back): the backoff starts over (at most one extra try per such event; a held core stays busy)
        if (disc && m_havePts) m_back.NewStream();
        m_havePts = true; m_lastPts = pts100;
        char err[300] = "";
        // the measured class
        int cls = 0; bool cg = false;
        if (m_meas) {
            if (m_b.measurePush(m_meas, tex, slice, pts100, disc ? 1 : 0, err, sizeof(err)) != 0) {
                Log(true, std::string("ContentDetect stopped (") + err + ")");
                m_b.close(m_meas); m_meas = nullptr;
                if (m_s.mode == PcsrMode::Auto) m_sw.Disarm();
            } else Class(m_meas, cls, cg);
        } else if (m_pc) Class(m_pc, cls, cg);
        // the model wanted now (the switch's rate limit), and whether pushcore runs at all
        const bool switching = m_sw.Update(cls, cg, nowS);
        const std::string target = switching ? m_sw.Next() : m_sw.Running();
        const bool wantCore = m_s.mode == PcsrMode::Anime || (!target.empty() && !m_off);
        if (!wantCore) {
            if (m_pc) { Close(); Log(false, "the picture is not measured as drawn: the scaler (driver Super Resolution / NIS) from here"); }
            if (switching) m_sw.Commit(nowS);
            return Done(t0);
        }
        if (m_pc && m_model != target) Update(target, cls, cg, switching, nowS);
        else if (!m_pc && m_back.MayTry(nowS)) {
            if (m_b.busy && m_b.busy()) m_back.Busy(nowS);   // another instance (PushMPC's frame generation) holds it
            else Open(target, cls, cg, switching, nowS);
        } else if (m_pc && switching) m_sw.Commit(nowS);
        if (!m_pc) return Done(t0);
        // the picture to pushcore, and its upscaled one back within about a frame (the bridge waits outside the lock)
        m_want = pts100; m_pending = false;
        const double tw = Ms();
        if (m_b.push(m_pc, tex, slice, pts100, disc ? 1 : 0, err, sizeof(err)) != 0) { Lost("pushcore refused a picture", err, nowS); return Done(t0); }
        // the wait is bounded from the push (an open before it is not: it happens once per switch or retry)
        const double deadline = tw + std::clamp(1.5 * m_s.frameMs, 12.0, 50.0);
        for (;;) {
            const double left = deadline - Ms();
            if (left <= 0) break;
            if (m_b.pollTex(m_pc, uint32_t(std::min(2.0, std::max(1.0, left))), Dest, Doneb, this, err, sizeof(err)) < 0) { Lost("pushcore failed", err, nowS); return Done(t0); }
            if (m_got) break;
        }
        for (int guard = 0; guard < 64; ++guard)
            if (m_b.pollTex(m_pc, 0, Dest, Doneb, this, err, sizeof(err)) <= 0) break;
        m_pending = false;
        const double wms = Ms() - tw;
        m_stats.waits++; m_stats.waitMsSum += wms; m_stats.waitMsMax = std::max(m_stats.waitMsMax, wms);
        // late pictures: the scaler draws them; when most are late, pushcore does not keep up here
        if (m_grace > 0) m_grace--;
        else {
            m_seen++;
            if (!m_got) m_late++;
            if (m_seen >= 48) {
                if (m_late * 2 > m_seen) {
                    Log(true, std::to_string(m_late) + " of the last " + std::to_string(m_seen) + " pictures from pushcore came too late");
                    Close();
                    if (m_s.mode == PcsrMode::Anime) { Done(t0); return false; }
                    m_sw.Disarm(); m_off = true;   // Auto: the scaler for the rest of the stream
                }
                m_seen = m_late = 0;
            }
        }
        return Done(t0);
    }

    // under the player's context lock, after Feed for the same picture: pushcore's picture into place of target
    bool Draw(ID3D11DeviceContext* ctx, ID3D11Texture2D* target, unsigned x, unsigned y, unsigned w, unsigned h)
    {
        if (!m_pc || !m_got) return false;
        std::string why;
        if (!m_draw.Draw(ctx, m_srvY.Get(), m_srvUV.Get(), m_s.tenBit, m_s.hdr, target, x, y, w, h, why)) {
            if (!m_drawWarned) { m_drawWarned = true; Log(true, "pushcore's picture cannot be drawn (" + why + "): the scaler draws"); }
            return false;
        }
        return true;
    }

    bool Got() const { return m_pc && m_got; }
    bool Running() const { return m_pc != nullptr; }
    const std::string& Model() const { return m_model; }
    const ShareBackoff& Backoff() const { return m_back; }
    const Stats& Stat() const { return m_stats; }
    unsigned OutW() const { return m_ow; }
    unsigned OutH() const { return m_oh; }
    ID3D11Texture2D* Picture() const { return m_dst.Get(); }
    bool MeasuresAlone() const { return m_meas != nullptr; }

private:
    void Log(bool warn, const std::string& s) { if (m_log) m_log(warn, s); }
    static double Ms()
    {
        LARGE_INTEGER f, c;
        QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
        return double(c.QuadPart) * 1000.0 / double(f.QuadPart);
    }
    bool Done(double t0)
    {
        const double ms = Ms() - t0;
        m_stats.feeds++; m_stats.feedMsSum += ms; m_stats.feedMsMax = std::max(m_stats.feedMsMax, ms);
        return true;
    }
    void Class(pm_pc* pc, int& cls, bool& cg)
    {
        pm_pc_info i = {};
        i.struct_size = sizeof(i);
        if (m_b.output(pc, &i) == 0) { cls = i.measured_class; cg = i.measured_cg != 0; }
    }
    void Close()
    {
        if (m_pc) m_b.close(m_pc);
        m_pc = nullptr; m_got = m_pending = false; m_model.clear();
    }
    // the instance failed (taken over, refused): closed; tried again after the backoff (never given up for the stream)
    void Lost(const char* what, const char* err, double nowS)
    {
        Close();
        m_back.Lost(nowS);
        m_stats.lost++;
        char d[32]; snprintf(d, sizeof(d), "%.0f", m_back.Delay());
        Log(false, std::string(what) + " (" + (err && *err ? err : "no reason") + "): the scaler draws; pushcore again in " + d + " s");
    }
    bool CheckModel(const std::string& target)
    {
        pm_pc_info i = {};
        i.struct_size = sizeof(i);
        if (m_b.output(m_pc, &i) != 0) return false;
        return i.anime_model == AnimeModelIndexOf(target);
    }
    void Open(const std::string& target, int cls, bool cg, bool switching, double nowS)
    {
        pm_pc_cfg c = m_s.cfg;
        const int model = AnimeModelIndexOf(target);
        c.anime_model = model >= 0 ? model : PM_PC_ANIME_NONE;
        c.content_class = cls; c.content_cg = cg ? 1 : 0;
        c.measure = m_meas ? 0 : 1;    // without the measuring instance, pushcore's own (an older bridge, anime mode)
        char err[300] = "";
        int k = 1;
        const double t0 = Ms();
        m_pc = m_b.open(&c, m_s.dev, m_s.mutex, &k, err, sizeof(err));
        const double ms = Ms() - t0;
        m_stats.openMsMax = std::max(m_stats.openMsMax, ms);
        pm_pc_info info{};
        info.struct_size = sizeof(info);
        std::string why;
        if (!m_pc) why = err[0] ? err : "pushcore refused the stream";
        else if (m_b.output(m_pc, &info) != 0) why = "pushcore does not say what runs";
        else if (k != 1 || !info.sr || (info.width & 1) || (info.height & 1) || info.width < 16 || info.height < 16) why = "pushcore does not run its upscaler at x1 here";
        else if (!Target(info.width, info.height, why)) {}
        if (!why.empty()) {
            m_stats.openFails++;
            if (m_pc) { m_b.close(m_pc); m_pc = nullptr; }
            m_back.Lost(nowS);
            Log(false, "pushcore not opened (" + why + "): the scaler draws; tried again later");
            return;
        }
        m_stats.opens++;
        m_grace = 12; m_seen = m_late = 0;
        if (model >= 0 && info.anime_model != model) {
            // the GPU cannot run that model: no anime upscaler this stream
            m_sw.Disarm();
            if (m_s.mode == PcsrMode::Auto) { Close(); m_off = true; Log(true, "pushcore cannot run the anime upscaler " + target + " on this GPU: the scaler"); return; }
            m_model.clear();
            Log(true, "pushcore cannot run the anime upscaler " + target + " on this GPU: its Video Super Resolution");
        } else {
            m_model = target;
            if (switching) m_sw.Commit(nowS);
        }
        char d[64]; snprintf(d, sizeof(d), "%ux%u, %.0f ms", m_ow, m_oh, ms);
        Log(false, std::string("pushcore's upscaler: ") + (m_model.empty() ? "Video Super Resolution" : m_model) + " (" + d + "); " + (m_b.describe ? m_b.describe(m_pc) : ""));
    }
    void Update(const std::string& target, int cls, bool cg, bool switching, double nowS)
    {
        pm_pc_cfg c = m_s.cfg;
        const int model = AnimeModelIndexOf(target);
        c.anime_model = model >= 0 ? model : PM_PC_ANIME_NONE;
        c.content_class = cls; c.content_cg = cg ? 1 : 0;
        char err[300] = "";
        const int r = m_b.update(m_pc, &c, err, sizeof(err));
        if (r < 0) { Lost("pushcore's model not switched", err, nowS); return; }   // taken over, or the reload failed
        m_stats.updates++;
        if (r == 0) m_grace = 12;      // reloaded: the pictures in flight were dropped
        if (!CheckModel(target)) {
            m_sw.Disarm();
            Log(true, "pushcore cannot run the anime upscaler " + target + " on this GPU");
            if (m_s.mode == PcsrMode::Auto) { Close(); m_off = true; }
            return;
        }
        m_model = target;
        if (switching) m_sw.Commit(nowS);
        Log(false, std::string("pushcore's upscaler: ") + (target.empty() ? "Video Super Resolution" : target) + " (measured class " + std::to_string(cls) + (cg ? ", CG" : "") + ")");
    }
    // the picture pushcore hands out (NV12 / P010 at its output size)
    bool Target(unsigned w, unsigned h, std::string& why)
    {
        if (m_dst && m_ow == w && m_oh == h) return true;
        m_dst.Reset(); m_srvY.Reset(); m_srvUV.Reset();
        D3D11_TEXTURE2D_DESC t = {};
        t.Width = w; t.Height = h; t.MipLevels = 1; t.ArraySize = 1;
        t.Format = m_s.tenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
        t.SampleDesc.Count = 1; t.Usage = D3D11_USAGE_DEFAULT; t.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SHADER_RESOURCE_VIEW_DESC v = {};
        v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; v.Texture2D.MipLevels = 1;
        HRESULT hr = m_s.dev->CreateTexture2D(&t, nullptr, &m_dst);
        v.Format = m_s.tenBit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
        if (SUCCEEDED(hr)) hr = m_s.dev->CreateShaderResourceView(m_dst.Get(), &v, &m_srvY);
        v.Format = m_s.tenBit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
        if (SUCCEEDED(hr)) hr = m_s.dev->CreateShaderResourceView(m_dst.Get(), &v, &m_srvUV);
        if (FAILED(hr)) { why = std::string("cannot create the ") + (m_s.tenBit ? "P010" : "NV12") + " picture for pushcore's output"; m_dst.Reset(); return false; }
        m_ow = w; m_oh = h;
        m_tex.texture = m_dst.Get(); m_tex.slice = 0;
        return true;
    }
    static const pm_pc_tex* Dest(void* user, int64_t pts100)
    {
        auto* p = static_cast<PcsrCore*>(user);
        if (pts100 != p->m_want || p->m_got) return nullptr;   // an older picture (late, or before a seek): not wanted
        p->m_pending = true;
        return &p->m_tex;
    }
    static void Doneb(void* user, int64_t pts100)
    {
        auto* p = static_cast<PcsrCore*>(user);
        if (p->m_pending && pts100 == p->m_want) p->m_got = true;
        p->m_pending = false;
    }

    BridgeFns m_b;
    Setup m_s;
    LogFn m_log;
    PcsrDraw m_draw;
    pm_pc* m_pc = nullptr;             // pushcore's upscaler (only while it is wanted and the core was free)
    pm_pc* m_meas = nullptr;           // ContentDetect alone (pm_pc_measure_open)
    std::string m_model;               // the anime model m_pc runs ("" = its Video Super Resolution)
    pmk::AnimeAutoSwitch m_sw;
    ShareBackoff m_back;
    Stats m_stats;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_dst;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srvY, m_srvUV;
    pm_pc_tex m_tex{};
    unsigned m_ow = 0, m_oh = 0;
    int64_t m_want = INT64_MIN, m_lastPts = 0;
    bool m_havePts = false, m_pending = false, m_got = false, m_drawWarned = false;
    bool m_off = false;                // Auto: pushcore is not used again this stream (its model cannot run, or it was late)
    int m_seen = 0, m_late = 0, m_grace = 12;
};

} // namespace pmpc
