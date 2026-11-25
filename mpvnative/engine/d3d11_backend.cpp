// D3D11Backend: pushcore on the decoder's own D3D11 device, zero copy in.
//   in:  the decoder texture (array slice) -> pushcore_submit2 (pushcore copies it into its ingress ring on the GPU)
//   out: pushcore's output texture -> one GPU draw / copy into the filter's texture (BGRA, X2BGR10 or RGBAF16, the
//        formats FFmpeg's D3D11 frames and mpv's d3d11 interop both know; pushcore's SDR RGBA8 lands in BGRA through
//        the view formats) -> pushcore_presented at once. mpv may hold many frames; pushcore's ring never waits on it.
// Settings: PushMPC's Auto (filter/src/PushMpcAuto.h through vsplugin/src/autopick.h, as the VapourSynth route), the
// HKCU\Software\PushMPC values, then the filter's options. One pushcore per process stays warm across instances
// (mpv rebuilds the filter on every seek): same device + stream + settings -> flush, settings only -> setSettings +
// reload, else reopen.
#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <tuple>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "autopick.h"
#include "engine.h"
#include "pushmpc/pushcore_abi.h"

using Microsoft::WRL::ComPtr;

namespace pme {
namespace {

std::wstring Widen(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}
std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
bool DirExists(const std::wstring& d) { const DWORD a = GetFileAttributesW(d.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
std::wstring RegFilterStr(const wchar_t* name)
{
    wchar_t buf[1024] = {}; DWORD size = sizeof(buf) - sizeof(wchar_t), type = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\PushMPC\\Filter", name, RRF_RT_REG_SZ, &type, buf, &size) != ERROR_SUCCESS) return {};
    return buf;
}
std::wstring LocalAppData() { wchar_t b[MAX_PATH] = {}; return GetEnvironmentVariableW(L"LOCALAPPDATA", b, MAX_PATH) ? b : L""; }
bool IsHdr(int32_t t) { return t == 16 || t == 18; }

// ------------------------------------------------------------------------------------------------ the warm core
struct Warm {
    std::mutex lock;
    HMODULE dll = nullptr;
    const PushCoreApi* api = nullptr;
    PushCoreCapabilitiesFn caps = nullptr;
    std::wstring dllPath;
    PushCore* core = nullptr;
    ID3D11Device* device = nullptr;        // weak: identity only (the core holds its own reference)
    std::vector<uint8_t> key;              // stream info + settings bytes
    std::vector<uint8_t> settings;
    PushCoreOutputInfo out{};
    bool owned = false;
    std::map<std::tuple<uint32_t, uint32_t, uint32_t, int32_t, int32_t>, std::array<int32_t, 4>> colours;   // last seen per stream
};
Warm& W() { static Warm* w = new Warm(); return *w; }   // never destroyed: the core may outlive every instance

bool LoadCore(const Options& o, std::wstring& coreDir, std::string& why)
{
    Warm& w = W();
    coreDir = Widen(Opt(o, "core_dir"));
    if (coreDir.empty()) coreDir = RegFilterStr(L"CoreFolder");
    if (coreDir.empty()) coreDir = LocalAppData() + L"\\PushMPC\\core";
    if (w.api) return true;
    const std::wstring dll = coreDir + L"\\pushcore.dll";
    HMODULE h = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!h) { why = "pushcore.dll not found in " + Narrow(coreDir); return false; }
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(h), &pinned);
    auto getApi = reinterpret_cast<PushCoreGetApiFn>(GetProcAddress(h, "pushcore_getApi"));
    const PushCoreApi* api = getApi ? getApi(PUSHCORE_ABI_VERSION) : nullptr;
    if (!api) { why = "pushcore.dll has no ABI " + std::to_string(PUSHCORE_ABI_VERSION) + " table"; return false; }
    w.dll = h; w.api = api; w.dllPath = dll;
    w.caps = reinterpret_cast<PushCoreCapabilitiesFn>(GetProcAddress(h, "pushcore_capabilities"));
    return true;
}

// ------------------------------------------------------------------------------------------------ the copy pass
const char kShader[] = R"(
Texture2D<float4> src : register(t0);
float4 vs(uint id : SV_VertexID) : SV_Position { float2 p = float2((id << 1) & 2, id & 2); return float4(p * float2(2, -2) + float2(-1, 1), 0, 1); }
float4 ps(float4 pos : SV_Position) : SV_Target { return src.Load(int3(pos.xy, 0)); }
)";

uint32_t PmFmtOfDxgi(uint32_t f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM: return PMENG_FMT_BGRA;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return PMENG_FMT_X2BGR10;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return PMENG_FMT_RGBAF16;
    default: return PMENG_FMT_NONE;
    }
}

class D3D11Backend : public Backend {
public:
    ~D3D11Backend() override
    {
        Warm& w = W();
        std::lock_guard<std::mutex> lk(w.lock);
        if (!m_owns) return;
        Lock();
        ReturnAll();
        Unlock();
        w.owned = false;   // the core stays warm for the next instance
    }

    bool Init(const pmeng_device& dev, const pmeng_stream& st, const Options& o, pmeng_output& out, std::string& why)
    {
        if (!dev.d3d11_device) { why = "no D3D11 device"; return false; }
        if (st.fmt != PMENG_FMT_NV12 && st.fmt != PMENG_FMT_P010) { why = "pushcore takes NV12 / P010 textures"; return false; }
        m_dev = static_cast<ID3D11Device*>(dev.d3d11_device);
        m_dev->GetImmediateContext(&m_ctx);
        m_ctx.As(&m_ctx4);
        m_lock = dev.lock; m_unlock = dev.unlock; m_lockCtx = dev.lock_ctx;
        m_st = st;

        Warm& w = W();
        std::lock_guard<std::mutex> lk(w.lock);
        if (w.owned) { why = "pushcore is in use by another instance in this process"; return false; }
        std::wstring coreDir;
        if (!LoadCore(o, coreDir, why)) return false;

        // settings: registry, then the options (same words as the VapourSynth route's arguments)
        pm::Settings s = pm::LoadSettings();
        static const char* keys[] = { "enable", "smoother", "sharper", "cleanup", "sdr_to_hdr", "deband", "fg_quality", "fg_method", "vrr", "upscaler", "target", "vsr_style", "cleanup_strength", "on_battery", "subtitle_hold" };
        std::string overrides;
        for (const char* k : keys)
            if (const std::string v = Opt(o, k); !v.empty() && pmvs::ApplyOverride(s, Widen(k), Widen(v))) overrides += std::format(" {}={}", k, v);
        if (!s.enable) { why = "PushMPC is off (Enable = 0)"; return false; }

        // colour: the frame's when known (first frame), else the last instance of this stream, else a guess
        const bool ten = st.fmt == PMENG_FMT_P010;
        auto ck = std::make_tuple(st.width, st.height, st.fmt, st.fps_num, st.fps_den);
        std::array<int32_t, 4> col = { st.range, st.matrix, st.primaries, st.transfer };
        if (auto it = w.colours.find(ck); it != w.colours.end()) col = it->second;
        else {
            if (col[1] == 2 || col[1] == 0) col[1] = ten && (st.width >= 3200 || st.height >= 1800) ? 9 : st.width < 1024 && st.height < 600 ? 6 : 1;
            if (col[3] == 2 || col[3] == 0) col[3] = col[1] == 9 && ten ? 16 : 1;
            if (col[2] == 2 || col[2] == 0) col[2] = col[1] == 9 ? 9 : 1;
            if (col[0] == 0) col[0] = 1;
        }
        m_col = col;

        // the GPU and the display as Auto sees them
        pm::Gpu g; g.available = 0; g.hardware = 0; g.family = pm::kFamUnknown;
        PushCoreCaps caps{};
        if (w.caps && w.caps(0, coreDir.c_str(), &caps) == PUSHCORE_OK) {
            g.family = caps.family; g.available = caps.available; g.hardware = caps.hardware; g.name = Widen(caps.adapter);
            for (uint32_t i = 0; i < pm::kCapCount && i < PUSHCORE_CAP_COUNT; ++i) g.reason[i] = Widen(caps.reason[i]);
        }
        pm::Costs costs;
        costs.card = pm::CardFactor(g.name, g.family);
        pmvs::LoadLearned(costs, g.name);
        m_src.w = st.width; m_src.h = st.height;
        m_src.fps = st.fps_num > 0 && st.fps_den > 0 ? double(st.fps_num) / st.fps_den : 23.976;
        m_src.hdr = IsHdr(col[3]); m_src.tenBit = ten; m_src.bppEq = -1;
        const HWND wnd = pmvs::MpvWindow();
        const double displayFps = std::atof(Opt(o, "display_fps", "0").c_str());
        const int displayHdr = Opt(o, "display_hdr").empty() ? -1 : std::atoi(Opt(o, "display_hdr").c_str());
        m_disp = pmvs::ResolveDisplay(pm::QueryDisplay(wnd, s.vrr, g.nvidia()), wnd != nullptr, s.vrr, displayFps, displayHdr);
        m_choice = pm::Decide(s, m_src, m_disp, costs, g);
        m_settings = s;
        std::wstring note;
        m_ps = pmvs::ToCore(s, m_choice, m_disp, m_src.w, m_src.h, m_src.hdr, m_disp.hdr, &note);
        Log(std::format("d3d11: {}x{} {}, colour {}/{}/{}/{}; display {:.3f} Hz HDR {}; GPU {} ({}); Auto x{} SR {} -> {}x{}{}{}",
            st.width, st.height, ten ? "P010" : "NV12", col[0], col[1], col[2], col[3], m_disp.refreshHz, m_disp.hdr ? "on" : "off", Narrow(g.name),
            Narrow(pm::FamilyName(g.family)), m_choice.multiplier, m_choice.sr ? "on" : "off", m_choice.outW, m_choice.outH,
            overrides.empty() ? "" : "; overridden:", overrides));

        if (!Acquire(coreDir, why)) return false;
        w.owned = true;
        m_owns = true;
        Describe(out);
        return true;
    }

    int Submit(const pmeng_image& src, int64_t ptsUs, uint32_t flags) override
    {
        Warm& w = W();
        std::lock_guard<std::mutex> lk(w.lock);
        if (src.mem != PMENG_MEM_D3D11 || !src.d3d11_texture) { err = "pushcore: expected a D3D11 texture"; return PMENG_ERROR; }
        // the frame's own colour: reopen once if the guess was wrong (format of the pool stays; tags follow the core)
        const std::array<int32_t, 4> fc = { src.range ? src.range : m_col[0], src.matrix != 2 && src.matrix ? src.matrix : m_col[1],
                                            src.primaries != 2 && src.primaries ? src.primaries : m_col[2], src.transfer != 2 && src.transfer ? src.transfer : m_col[3] };
        w.colours[std::make_tuple(m_st.width, m_st.height, m_st.fmt, m_st.fps_num, m_st.fps_den)] = fc;
        if (fc != m_col) {
            if (!m_pending.empty() || m_sinceOpen > 0) { /* mid-stream colour change: keep going with the open core */ }
            else {
                Log(std::format("d3d11: colour {}/{}/{}/{} differs from the guess: reopening", fc[0], fc[1], fc[2], fc[3]));
                m_col = fc; m_src.hdr = IsHdr(fc[3]);
                m_ps = pmvs::ToCore(m_settings, m_choice, m_disp, m_src.w, m_src.h, m_src.hdr, m_disp.hdr, nullptr);
                std::string why;
                if (!Acquire(W().dllPath.substr(0, W().dllPath.find_last_of(L'\\')), why)) { err = why; return PMENG_ERROR; }
            }
        }
        Lock();
        const int can = w.api->canSubmit(w.core);
        if (!can) { Unlock(); return PMENG_BUSY; }
        PushCoreSubmitTiming t = {};
        t.structSize = PUSHCORE_SUBMIT_TIMING_SIZE_V2;
        t.streamNow100ns = INT64_MIN;   // no presentation clock: the full group, always
        const bool disc = (flags & PMENG_SUBMIT_DISCONTINUITY) || m_sinceOpen == 0;
        const int r = w.api->submit2(w.core, static_cast<ID3D11Texture2D*>(src.d3d11_texture), src.d3d11_slice, ptsUs * 10,
                                     disc ? PUSHCORE_SUBMIT_DISCONTINUITY : 0u, &t);
        Unlock();
        if (r == PUSHCORE_BUSY) return PMENG_BUSY;
        if (r != PUSHCORE_OK) { err = std::format("pushcore_submit2 -> {}: {}", r, w.api->lastError(w.core)); return r == PUSHCORE_DEVICE_LOST ? PMENG_DEVICE_LOST : PMENG_ERROR; }
        ++m_submitted; ++m_sinceOpen;
        return PMENG_OK;
    }

    int Peek(pmeng_out_info& info, uint32_t timeoutMs) override
    {
        Warm& w = W();
        const ULONGLONG until = GetTickCount64() + timeoutMs;
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(w.lock);
                if (m_pending.empty()) {
                    PushCoreFrame fr[16]; uint32_t n = 0;
                    Lock();
                    const int r = w.api->poll(w.core, fr, 16, &n, 0);   // never wait while holding the decoder's lock
                    Unlock();
                    if (r != PUSHCORE_OK && r != PUSHCORE_BUSY) { err = std::format("pushcore_poll -> {}: {}", r, w.api->lastError(w.core)); return PMENG_ERROR; }
                    for (uint32_t i = 0; i < n; ++i) {
                        Item it; it.fr = fr[i];
                        // the n-th real output belongs to the n-th submit; generated frames to the next real one
                        it.info.source = m_realOut;
                        it.info.subframe = fr[i].kind == PUSHCORE_FRAME_REAL ? 0 : std::max<uint32_t>(fr[i].subframe, 1);
                        it.info.multiplier = std::max<uint32_t>(w.out.multiplier, 1);
                        it.info.generated = fr[i].kind == PUSHCORE_FRAME_REAL ? 0 : 1;
                        if (fr[i].kind == PUSHCORE_FRAME_REAL) ++m_realOut;
                        m_pending.push_back(it);
                    }
                }
                if (!m_pending.empty()) { info = m_pending.front().info; return 1; }
            }
            if (timeoutMs == 0 || GetTickCount64() >= until) return 0;
            Sleep(1);
        }
    }

    int Write(const pmeng_image& dst) override
    {
        Warm& w = W();
        std::lock_guard<std::mutex> lk(w.lock);
        if (m_pending.empty()) { err = "write without an output"; return PMENG_ERROR; }
        Item it = m_pending.front();
        m_pending.pop_front();
        if (dst.mem != PMENG_MEM_D3D11 || !dst.d3d11_texture) { w.api->presented(w.core, it.fr.lease, nullptr); err = "pushcore: expected a D3D11 output texture"; return PMENG_ERROR; }
        auto* out = static_cast<ID3D11Texture2D*>(dst.d3d11_texture);
        Lock();
        if (it.fr.readyFence && m_ctx4) m_ctx4->Wait(it.fr.readyFence, it.fr.readyValue);
        D3D11_TEXTURE2D_DESC sd = {}, dd = {};
        it.fr.texture->GetDesc(&sd);
        out->GetDesc(&dd);
        const UINT ow = std::min<UINT>(dst.width, it.fr.width ? it.fr.width : w.out.width), oh = std::min<UINT>(dst.height, it.fr.height ? it.fr.height : w.out.height);
        std::string e;
        bool ok;
        if (sd.Format == dd.Format) {
            D3D11_BOX box = { 0, 0, 0, ow, oh, 1 };
            m_ctx->CopySubresourceRegion(out, D3D11CalcSubresource(0, dst.d3d11_slice, dd.MipLevels), 0, 0, 0, it.fr.texture, 0, &box);
            ok = true;
        } else {
            ok = Draw(it.fr.texture, sd.Format, out, dd, dst.d3d11_slice, ow, oh, e);
        }
        w.api->presented(w.core, it.fr.lease, m_ctx.Get());
        Unlock();
        if (!ok) { err = e; return PMENG_ERROR; }
        ++m_written;
        if (it.info.generated) ++m_generated;
        return PMENG_OK;
    }

    int Skip() override
    {
        Warm& w = W();
        std::lock_guard<std::mutex> lk(w.lock);
        if (m_pending.empty()) return PMENG_OK;
        Lock();
        w.api->presented(w.core, m_pending.front().fr.lease, nullptr);
        Unlock();
        m_pending.pop_front();
        return PMENG_OK;
    }
    uint32_t Pending() override
    {
        std::lock_guard<std::mutex> lk(W().lock);
        return uint32_t(m_pending.size()) + uint32_t(m_submitted > m_realOut ? m_submitted - m_realOut : 0);
    }
    int Flush() override
    {
        Warm& w = W();
        std::lock_guard<std::mutex> lk(w.lock);
        Lock();
        ReturnAll();
        const int r = w.api->flush(w.core);
        Unlock();
        m_realOut = m_submitted;
        m_sinceOpen = 0;
        if (r != PUSHCORE_OK) { err = std::format("pushcore_flush -> {}: {}", r, w.api->lastError(w.core)); return PMENG_ERROR; }
        return PMENG_OK;
    }
    std::string Stats() override
    {
        Warm& w = W();
        std::lock_guard<std::mutex> lk(w.lock);
        PushCoreStats s{}; s.structSize = sizeof(s);
        std::string extra;
        if (w.core && w.api->stats(w.core, &s) == PUSHCORE_OK)
            extra = std::format("; pushcore: {} submitted, {} generated, {} skipped, {} busy, cut guard {} ({} cuts)", s.submitted, s.generated, s.skippedGenerated, s.busy,
                                s.cutGuardActive ? "on" : "off", s.cutGuardCuts);
        return std::format("pushcore d3d11: {} sources, {} outputs ({} generated){}", m_submitted, m_written, m_generated, extra);
    }

private:
    struct Item { PushCoreFrame fr{}; pmeng_out_info info{}; };

    void Lock() { if (m_lock) m_lock(m_lockCtx); }
    void Unlock() { if (m_unlock) m_unlock(m_lockCtx); }
    void ReturnAll()
    {
        Warm& w = W();
        for (auto& it : m_pending) w.api->presented(w.core, it.fr.lease, nullptr);
        m_pending.clear();
    }

    bool Acquire(const std::wstring& coreDir, std::string& why)
    {
        Warm& w = W();
        PushCoreStreamInfo si = {};
        si.structSize = sizeof(si);
        si.width = m_st.width; si.height = m_st.height;
        si.format = m_st.fmt == PMENG_FMT_P010 ? PUSHCORE_INPUT_P010 : PUSHCORE_INPUT_NV12;
        si.fpsNum = m_st.fps_num; si.fpsDen = m_st.fps_den;
        si.range = m_col[0]; si.matrix = m_col[1]; si.primaries = m_col[2]; si.transfer = m_col[3];
        si.masteringPeakNits = m_st.mastering_peak_nits; si.maxCllNits = m_st.max_cll_nits; si.maxFallNits = m_st.max_fall_nits;
        si.displayHdr = m_disp.hdr ? 1u : 0u;
        std::vector<uint8_t> settings(reinterpret_cast<const uint8_t*>(&m_ps), reinterpret_cast<const uint8_t*>(&m_ps) + sizeof(m_ps));
        std::vector<uint8_t> key(reinterpret_cast<const uint8_t*>(&si), reinterpret_cast<const uint8_t*>(&si) + sizeof(si));
        const ULONGLONG t0 = GetTickCount64();
        const char* how = "open (cold)";
        Lock();
        if (w.core && w.device == m_dev.Get() && w.key == key) {
            if (w.settings == settings) { w.api->flush(w.core); how = "flush (warm core reused)"; }
            else if (w.api->setSettings(w.core, &m_ps) == PUSHCORE_OK && w.api->reload(w.core) == PUSHCORE_OK) { how = "setSettings + reload (warm)"; }
            else { w.api->close(w.core); w.core = nullptr; }
        } else if (w.core) {
            w.api->close(w.core); w.core = nullptr; how = "reopen (stream or device changed)";
        }
        if (!w.core) {
            const std::wstring runtime = [&] { std::wstring r = RegFilterStr(L"RuntimeRoot"); if (r.empty()) r = LocalAppData() + L"\\PushMPC\\runtime"; return DirExists(r) ? r : coreDir; }();
            const std::wstring shaders = coreDir + L"\\shaders";
            PushCoreConfig cfg = {};
            cfg.structSize = sizeof(cfg); cfg.abiVersion = PUSHCORE_ABI_VERSION;
            cfg.pushfpsDir = coreDir.c_str(); cfg.dataDir = L""; cfg.runtimeDir = runtime.c_str();
            cfg.sr = -1; cfg.fg = -1; cfg.multiplier = -1; cfg.vfgQuality = -1;
            cfg.shaderDir = DirExists(shaders) ? shaders.c_str() : nullptr;
            cfg.settings = &m_ps;
            char eb[1024] = {};
            w.core = w.api->open(&cfg, &si, m_dev.Get(), eb, sizeof(eb));
            if (!w.core) { Unlock(); why = std::string("pushcore_open: ") + (eb[0] ? eb : "failed"); return false; }
        }
        w.device = m_dev.Get(); w.key = key; w.settings = settings;
        w.out = {};
        w.api->outputInfo(w.core, &w.out);
        Unlock();
        m_sinceOpen = 0; m_realOut = m_submitted;
        Log(std::format("d3d11: core {} in {} ms: {}", how, GetTickCount64() - t0, w.out.description));
        return true;
    }

    void Describe(pmeng_output& out)
    {
        const PushCoreOutputInfo& oi = W().out;
        out.mem = PMENG_MEM_D3D11;
        out.fmt = PmFmtOfDxgi(oi.dxgiFormat);
        if (out.fmt == PMENG_FMT_NONE) out.fmt = oi.hdrOutput ? PMENG_FMT_X2BGR10 : PMENG_FMT_BGRA;
        // a guessed SDR stream that might turn out HDR at the first frame keeps the 10-bit pool (the copy pass converts)
        out.width = oi.width ? oi.width : m_st.width;
        out.height = oi.height ? oi.height : m_st.height;
        out.multiplier = std::clamp<uint32_t>(oi.multiplier ? oi.multiplier : m_ps.multiplier, 1, 8);
        const bool hdr = oi.hdrOutput != 0;
        out.hdr_output = hdr ? 1 : 0;
        out.range = 2;                    // AVCOL_RANGE_JPEG (full)
        out.matrix = 0;                   // AVCOL_SPC_RGB
        out.primaries = hdr ? 9 : 1;      // BT.2020 / BT.709
        out.transfer = hdr ? 16 : 13;     // SMPTE 2084 / IEC 61966-2-1 (sRGB)
        out.keep_dynamic_hdr = 0;
        if (hdr) {
            out.mastering_peak_nits = float(oi.hdr10Metadata.maxMasteringLuminance);
            out.mastering_min_nits = float(oi.hdr10Metadata.minMasteringLuminance) / 10000.0f;
            out.max_cll_nits = oi.hdr10Metadata.maxContentLightLevel;
            out.max_fall_nits = oi.hdr10Metadata.maxFrameAverageLightLevel;
        }
        std::snprintf(out.engine, sizeof(out.engine), "pushcore");
        std::snprintf(out.description, sizeof(out.description), "%s", oi.description);
    }

    bool Draw(ID3D11Texture2D* src, DXGI_FORMAT srcFmt, ID3D11Texture2D* dst, const D3D11_TEXTURE2D_DESC& dd, UINT slice, UINT w, UINT h, std::string& e)
    {
        if (!m_vs) {
            ComPtr<ID3DBlob> vsb, psb, eb;
            if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "pushmpc_copy", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, &eb)) ||
                FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "pushmpc_copy", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, &eb)) ||
                FAILED(m_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &m_vs)) ||
                FAILED(m_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &m_ps5))) {
                e = std::string("copy shader: ") + (eb ? static_cast<const char*>(eb->GetBufferPointer()) : "compile failed");
                m_vs.Reset();
                return false;
            }
        }
        ComPtr<ID3D11ShaderResourceView>& srv = m_srv[src];
        if (!srv) {
            D3D11_SHADER_RESOURCE_VIEW_DESC d = {};
            d.Format = srcFmt; d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; d.Texture2D.MipLevels = 1;
            if (FAILED(m_dev->CreateShaderResourceView(src, &d, &srv))) { e = "SRV on pushcore's output failed"; m_srv.erase(src); return false; }
        }
        ComPtr<ID3D11RenderTargetView> rtv;
        D3D11_RENDER_TARGET_VIEW_DESC r = {};
        r.Format = dd.Format;
        if (dd.ArraySize > 1) { r.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY; r.Texture2DArray.FirstArraySlice = slice; r.Texture2DArray.ArraySize = 1; }
        else r.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(m_dev->CreateRenderTargetView(dst, &r, &rtv))) { e = "RTV on the output texture failed (needs D3D11_BIND_RENDER_TARGET)"; return false; }
        D3D11_VIEWPORT vp = { 0, 0, float(w), float(h), 0, 1 };
        ID3D11RenderTargetView* rt = rtv.Get();
        ID3D11ShaderResourceView* sr = srv.Get();
        m_ctx->OMSetRenderTargets(1, &rt, nullptr);
        m_ctx->RSSetViewports(1, &vp);
        m_ctx->IASetInputLayout(nullptr);
        m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_ctx->VSSetShader(m_vs.Get(), nullptr, 0);
        m_ctx->PSSetShader(m_ps5.Get(), nullptr, 0);
        m_ctx->PSSetShaderResources(0, 1, &sr);
        m_ctx->Draw(3, 0);
        ID3D11ShaderResourceView* nullSrv = nullptr; ID3D11RenderTargetView* nullRt = nullptr;
        m_ctx->PSSetShaderResources(0, 1, &nullSrv);
        m_ctx->OMSetRenderTargets(1, &nullRt, nullptr);
        if (m_srv.size() > 64) m_srv.clear();   // pushcore's ring is small; guard against a growing map anyway
        return true;
    }

    bool m_owns = false;
    ComPtr<ID3D11Device> m_dev;
    ComPtr<ID3D11DeviceContext> m_ctx;
    ComPtr<ID3D11DeviceContext4> m_ctx4;
    void (*m_lock)(void*) = nullptr;
    void (*m_unlock)(void*) = nullptr;
    void* m_lockCtx = nullptr;
    pmeng_stream m_st{};
    std::array<int32_t, 4> m_col{};
    pm::Settings m_settings;
    pm::Source m_src;
    pm::Display m_disp;
    pm::Choice m_choice;
    PushCoreSettings m_ps{};
    std::deque<Item> m_pending;
    uint64_t m_submitted = 0, m_realOut = 0, m_sinceOpen = 0, m_written = 0, m_generated = 0;
    ComPtr<ID3D11VertexShader> m_vs;
    ComPtr<ID3D11PixelShader> m_ps5;
    std::map<ID3D11Texture2D*, ComPtr<ID3D11ShaderResourceView>> m_srv;
};

} // namespace

Backend* OpenD3D11(const pmeng_device& dev, const pmeng_stream& st, const Options& o, pmeng_output& out, std::string& why)
{
    auto* b = new D3D11Backend();
    if (!b->Init(dev, st, o, out, why)) { delete b; return nullptr; }
    return b;
}

} // namespace pme
