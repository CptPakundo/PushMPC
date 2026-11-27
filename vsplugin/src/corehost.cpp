// pushmpc.Enhance: the process-wide pushcore host (see corehost.h).
#include "corehost.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>
#include <format>
#include <thread>
#include "vslog.h"

namespace pmvs {

CoreHost& CoreHost::Get()
{
    static CoreHost* h = [] {
        // pin this plugin: VapourSynth frees plugins with each core (mpv: on every seek); the warm core must stay
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&CoreHost::Get), &self);
        return new CoreHost();
    }();
    return *h;
}

// ------------------------------------------------------------------------------------------------ SEH wrappers
// pushcore calls that may raise (an NVIDIA stack failure), in functions without C++ unwinding
static PushCore* SehOpen(const PushCoreApi* a, const PushCoreConfig* c, const PushCoreStreamInfo* s, ID3D11Device* d, char* e, uint32_t n)
{
    __try { return a->open(c, s, d, e, n); } __except (EXCEPTION_EXECUTE_HANDLER) { if (n) strcpy_s(e, n, "pushcore_open raised an exception"); return nullptr; }
}
static int SehCaps(PushCoreCapabilitiesFn f, uint64_t luid, const wchar_t* root, PushCoreCaps* c)
{
    __try { return f(luid, root, c); } __except (EXCEPTION_EXECUTE_HANDLER) { return PUSHCORE_ERROR; }
}
static void SehClose(const PushCoreApi* a, PushCore* c)
{
    __try { a->close(c); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ------------------------------------------------------------------------------------------------ load
bool CoreHost::Load(const HostPaths& paths, std::string& err)
{
    if (m_api) return true;
    m_paths = paths;
    const std::wstring dll = paths.coreDir + L"\\pushcore.dll";
    DLL_DIRECTORY_COOKIE cookie = paths.runtimeDir.empty() ? nullptr : AddDllDirectory(paths.runtimeDir.c_str());
    HMODULE h = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_SYSTEM32);
    const DWORD le = GetLastError();
    if (cookie) RemoveDllDirectory(cookie);
    if (!h) { err = std::format("cannot load {} (error {}): is PushMPC's core installed?", Narrow(dll), le); return false; }
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(h), &pinned);
    auto getApi = reinterpret_cast<PushCoreGetApiFn>(GetProcAddress(h, "pushcore_getApi"));
    const PushCoreApi* api = getApi ? getApi(PUSHCORE_ABI_VERSION) : nullptr;
    if (!api || api->structSize < sizeof(PushCoreApi) || !api->submit2 || !api->setSettings) { err = "pushcore ABI mismatch in " + Narrow(dll) + " (the plugin and the core must be the same release)"; return false; }
    m_dll = h; m_api = api;
    if (auto feat = reinterpret_cast<uint32_t (*)(void)>(GetProcAddress(h, "pushcore_features"))) m_features = feat();
    m_caps = reinterpret_cast<PushCoreCapabilitiesFn>(GetProcAddress(h, "pushcore_capabilities"));
    Log(std::format("loaded {} (ABI {}, features 0x{:X}), runtime {}", Narrow(dll), api->abiVersionFn(), m_features, Narrow(paths.runtimeDir)));
    return true;
}
bool CoreHost::Caps(PushCoreCaps& c)
{
    memset(&c, 0, sizeof(c)); c.structSize = sizeof(c);
    return m_caps && SehCaps(m_caps, 0, m_paths.runtimeDir.c_str(), &c) == PUSHCORE_OK;
}

// ------------------------------------------------------------------------------------------------ device
bool CoreHost::EnsureDevice(uint64_t luid, AdapterPick pick, std::string& err)
{
    if (m_dev) return true;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    HRESULT hr = E_FAIL;
    if (pick != AdapterPick::Warp) {
        ComPtr<IDXGIFactory4> f;
        ComPtr<IDXGIAdapter1> ad;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) {
            if (luid) { LUID l; l.LowPart = DWORD(luid & 0xFFFFFFFFu); l.HighPart = LONG(luid >> 32); f->EnumAdapterByLuid(l, IID_PPV_ARGS(&ad)); }
            if (!ad) { ComPtr<IDXGIFactory6> f6; if (SUCCEEDED(f.As(&f6))) f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&ad)); }
        }
        if (ad) {
            hr = D3D11CreateDevice(ad.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, UINT(std::size(levels)), D3D11_SDK_VERSION, &dev, nullptr, &ctx);
            if (SUCCEEDED(hr)) { DXGI_ADAPTER_DESC1 d{}; ad->GetDesc1(&d); m_deviceName = Narrow(d.Description); m_luid = (uint64_t(uint32_t(d.AdapterLuid.HighPart)) << 32) | d.AdapterLuid.LowPart; }
        }
    }
    if (FAILED(hr)) {
        if (pick != AdapterPick::Warp) Log(std::format("no hardware D3D11 device (0x{:08X}): WARP", uint32_t(hr)));
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags & ~D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, UINT(std::size(levels)), D3D11_SDK_VERSION, &dev, nullptr, &ctx);
        if (FAILED(hr)) { err = std::format("D3D11CreateDevice failed (0x{:08X})", uint32_t(hr)); return false; }
        m_warp = true; m_deviceName = "WARP"; m_luid = 0;
    }
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(ctx.As(&mt))) mt->SetMultithreadProtected(TRUE);
    ctx.As(&m_ctx4);
    m_dev = dev; m_ctx = ctx;
    Log(std::format("D3D11 device on {} (LUID {:016X}, feature level 0x{:X}){}", m_deviceName, m_luid, uint32_t(dev->GetFeatureLevel()), m_ctx4 ? "" : ", no ID3D11DeviceContext4 (fences unusable)"));
    return true;
}
bool CoreHost::AdoptDevice(ID3D11Device* dev, std::string& err)
{
    if (!dev) { err = "no D3D11 device"; return false; }
    if (m_dev.Get() == dev) return true;
    CloseCore();   // the core, the ingress ring and the egress objects belong to the old device
    m_up.clear(); m_upW = m_upH = 0;
    m_stage.Reset(); m_stageFmt = DXGI_FORMAT_UNKNOWN; m_stageW = m_stageH = 0;
    m_cs.Reset(); m_cb.Reset();
    for (int i = 0; i < 3; ++i) { m_plane[i].Reset(); m_planeStage[i].Reset(); m_uav[i].Reset(); }
    m_planeW = m_planeH = 0;
    ComPtr<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(&ctx);
    ComPtr<ID3D10Multithread> mt;   // pushcore records on the immediate context from its own threads
    if (SUCCEEDED(ctx.As(&mt))) mt->SetMultithreadProtected(TRUE);
    m_ctx4.Reset();
    ctx.As(&m_ctx4);
    m_dev = dev; m_ctx = ctx;
    m_warp = false; m_luid = 0; m_deviceName = "the player's device";
    ComPtr<IDXGIDevice> dd; ComPtr<IDXGIAdapter> ad;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dd))) && SUCCEEDED(dd->GetAdapter(&ad))) {
        DXGI_ADAPTER_DESC d{}; ad->GetDesc(&d);
        m_deviceName = Narrow(d.Description); m_luid = (uint64_t(uint32_t(d.AdapterLuid.HighPart)) << 32) | d.AdapterLuid.LowPart;
        m_warp = d.VendorId == 0x1414 && d.DeviceId == 0x8c;
    }
    Log(std::format("D3D11 device adopted: {} (LUID {:016X}){}", m_deviceName, m_luid, m_ctx4 ? "" : ", no ID3D11DeviceContext4 (fences unusable)"));
    return true;
}

// ------------------------------------------------------------------------------------------------ acquire / release
void CoreHost::ReturnFrom(PushCore* core)
{
    if (!core) return;
    PushCoreFrame fr[16]; uint32_t n = 0; int dropped = 0;
    while (m_api->poll(core, fr, 16, &n, 0) == PUSHCORE_OK && n) { for (uint32_t i = 0; i < n; i++) m_api->presented(core, fr[i].lease, nullptr); dropped += int(n); n = 0; }
    if (dropped) Log(std::format("returned {} undelivered pushcore frames", dropped));
}
void CoreHost::ReturnAll()
{
    if (m_old) RetireOld("frames returned");
    ReturnFrom(m_core);
}
void CoreHost::CloseCore()
{
    DropQuick();
    if (!m_core) return;
    ReturnAll();
    SehClose(m_api, m_core);
    m_core = nullptr;
    m_srv.clear();
}

// ------------------------------------------------------------------------------------------------ fast start
// a core closed on its own thread (a close waits for any graph build in the process)
void CoreHost::CloseAsync(PushCore* core, const char* what)
{
    if (!core) return;
    const PushCoreApi* api = m_api;
    std::thread([api, core, what] { const uint64_t t0 = GetTickCount64(); SehClose(api, core); Log(std::format("{} closed in {} ms (own thread)", what, GetTickCount64() - t0)); }).detach();
}
void CoreHost::RetireOld(const char* why)
{
    if (!m_old) return;
    ReturnFrom(m_old);
    CloseAsync(m_old, "the quick graph");
    m_old = nullptr;
    Log(std::format("quick start: the quick graph retired ({}), {} ms after it opened", why, GetTickCount64() - m_quickTick));
}
void CoreHost::DropQuick()
{
    std::lock_guard<std::mutex> lk(m_nextLock);
    ++m_gen;
    m_quick = false; m_nextDone = false;
    if (m_next) { CloseAsync(m_next, "the full graph (not taken over)"); m_next = nullptr; }
    if (m_old) { ReturnFrom(m_old); CloseAsync(m_old, "the quick graph"); m_old = nullptr; }
    m_discardUpTo = INT64_MIN;
}
// as the filter's QuickStartWanted: NVIDIA, frame generation that starts without NGX, an NGX stage in the full graph,
// no RTX Video HDR, and no full graph opened before in this process (its NGX core is warm then)
bool CoreHost::QuickWanted(const PushCoreSettings& ps, std::string& why)
{
    wchar_t v[8] = {};
    if (GetEnvironmentVariableW(L"PUSHMPC_FILTER_QuickStart", v, 8) && v[0] == L'0') { why = "switched off (QuickStart 0)"; return false; }
    if (!(m_features & PUSHCORE_FEATURE_FAST_START)) { why = "the core has no fast start"; return false; }
    // PUSHMPC_TEST_QUICK_ANY_GPU=1 (tests only: the stub core on WARP): no GPU and no warm-core condition
    wchar_t anyGpu[8] = {};
    const bool test = GetEnvironmentVariableW(L"PUSHMPC_TEST_QUICK_ANY_GPU", anyGpu, 8) && anyGpu[0] == L'1';
    ComPtr<IDXGIDevice> dd; ComPtr<IDXGIAdapter> ad; DXGI_ADAPTER_DESC d{};
    if (!test && (m_warp || !m_dev || FAILED(m_dev.As(&dd)) || FAILED(dd->GetAdapter(&ad)) || FAILED(ad->GetDesc(&d)) || d.VendorId != 0x10DE)) { why = "not an NVIDIA GPU"; return false; }
    if (ps.multiplier < 2) { why = "no frame generation"; return false; }
    if (ps.fgBackend != PUSHCORE_FG_VFG && ps.fgBackend != PUSHCORE_FG_NVOF && ps.fgBackend != PUSHCORE_FG_FSR) { why = "the frame generation method needs NGX"; return false; }
    if (ps.videoHdr) { why = "RTX Video HDR sets the output signal"; return false; }
    if (!((ps.sr && ps.srMethod == PUSHCORE_SR_METHOD_RTX) || ps.nr)) { why = "no NGX stage: the full graph opens fast"; return false; }
    if (m_fullOpened && !test) { why = "a full graph opened before in this process: its NGX core is warm"; return false; }
    return true;
}
// the full graph on a worker: pushcore_open with the GPU-only warm-up, no lock of ours; handed to Submit through m_next
void CoreHost::StartFull(const PushCoreStreamInfo& si, const PushCoreSettings& ps)
{
    uint64_t gen;
    {
        std::lock_guard<std::mutex> lk(m_nextLock);
        m_quick = true; m_nextDone = false; m_next = nullptr; m_nextErr.clear();
        gen = ++m_gen;
    }
    m_quickTick = GetTickCount64();
    const PushCoreApi* api = m_api;
    ComPtr<ID3D11Device> dev = m_dev;
    const HostPaths paths = m_paths;
    std::thread([this, api, dev, paths, si, ps, gen] {   // the host is never destroyed
        { std::lock_guard<std::mutex> lk(m_nextLock); if (m_gen != gen) return; }
        PushCoreConfig pc = {};
        pc.structSize = sizeof(pc); pc.abiVersion = PUSHCORE_ABI_VERSION;
        pc.pushfpsDir = paths.runtimeDir.c_str();
        pc.dataDir = L"";
        pc.logFile = paths.logFile.empty() ? nullptr : paths.logFile.c_str();
        pc.shaderDir = paths.shaderDir.empty() ? nullptr : paths.shaderDir.c_str();
        pc.settings = &ps;
        pc.sr = pc.fg = pc.multiplier = pc.vfgQuality = -1;
        pc.flags = PUSHCORE_FLAG_WARMUP_GPU_ONLY;
        char eb[1024] = {};
        const uint64_t t0 = GetTickCount64();
        PushCore* core = SehOpen(api, &pc, &si, dev.Get(), eb, sizeof(eb));
        Log(std::format("quick start: the full graph {} in {} ms{}", core ? "opened" : "did not open", GetTickCount64() - t0, core ? std::string() : std::string(": ") + eb));
        std::lock_guard<std::mutex> lk(m_nextLock);
        if (m_gen != gen || !m_quick) { if (core) CloseAsync(core, "the full graph (no longer wanted)"); return; }
        m_next = core; m_nextErr = eb; m_nextDone = true;
        if (core) m_fullOpened = true;
    }).detach();
}
// after a submit of source frame pts into the quick graph (m_lock held): the full graph gets the same frame as its first
// one (its own output for it is dropped); the quick graph delivers up to and including it
void CoreHost::Handover(ID3D11Texture2D* tex, uint32_t slice, int64_t pts)
{
    PushCore* next = nullptr; std::string nextErr;
    {
        std::lock_guard<std::mutex> lk(m_nextLock);
        if (!m_quick || m_old || !m_nextDone) return;
        next = m_next; m_next = nullptr; m_quick = false; nextErr = m_nextErr;
    }
    if (!next) { Log("quick start: the full graph did not open (" + nextErr + "): the quick graph stays"); return; }
    PushCoreSubmitTiming t = {};
    t.structSize = PUSHCORE_SUBMIT_TIMING_SIZE_V2;
    t.streamNow100ns = INT64_MIN;
    const int r = m_api->submit2(next, tex, slice, pts, PUSHCORE_SUBMIT_DISCONTINUITY, &t);
    if (r != PUSHCORE_OK) { Log(std::format("quick start: the full graph refused its first frame ({} {}): the quick graph stays", r, m_api->lastError(next))); CloseAsync(next, "the full graph"); return; }
    const PushCoreOutputInfo before = m_out;
    m_old = m_core; m_core = next;
    m_handoverPts = pts; m_discardUpTo = pts;
    m_out = {};
    m_api->outputInfo(m_core, &m_out);
    m_srv.clear();
    Log(std::format("quick start: handover at pts {:.3f} s, {} ms after the quick graph opened: {} (output {}x{} x{}; quick graph {}x{} x{})",
        pts / 1e7, GetTickCount64() - m_quickTick, m_out.description, m_out.width, m_out.height, m_out.multiplier, before.width, before.height, before.multiplier));
}
uint64_t CoreHost::Acquire(const CoreKey& key, const PushCoreStreamInfo& si, const PushCoreSettings& ps, Reuse& how, double& ms, std::string& err)
{
    const uint64_t t0 = GetTickCount64();
    if (m_owner) Log("a new script instance takes the core over from the previous one (mpv reload)");
    m_owner = 0;
    how = DecideReuse(m_core != nullptr, m_key, key);
    // fast start: the quick graph cannot take other settings (a reload there would start NGX inline): open again
    if (how == Reuse::Reload) { bool quick; { std::lock_guard<std::mutex> lk(m_nextLock); quick = m_quick; } if (quick) how = Reuse::Reopen; }
    if (how == Reuse::Flush || how == Reuse::Reload) {
        ReturnAll();
        int r = m_api->flush(m_core);
        if (r == PUSHCORE_OK && how == Reuse::Reload) {
            r = m_api->setSettings(m_core, &ps);
            if (r == PUSHCORE_OK) r = m_api->reload(m_core);
        }
        if (r != PUSHCORE_OK) { Log(std::format("{} failed ({} {}): reopening", ReuseName(how), r, m_api->lastError(m_core))); how = Reuse::Reopen; }
    }
    if (how == Reuse::Reopen) CloseCore();
    if (!m_core) {
        PushCoreConfig pc = {};
        pc.structSize = sizeof(pc); pc.abiVersion = PUSHCORE_ABI_VERSION;
        pc.pushfpsDir = m_paths.runtimeDir.c_str();
        pc.dataDir = L"";
        pc.logFile = m_paths.logFile.empty() ? nullptr : m_paths.logFile.c_str();
        pc.shaderDir = m_paths.shaderDir.empty() ? nullptr : m_paths.shaderDir.c_str();
        pc.settings = &ps;
        pc.sr = pc.fg = pc.multiplier = pc.vfgQuality = -1;
        pc.flags = 0;
        char eb[1024] = {};
        std::string qwhy;
        if (QuickWanted(ps, qwhy)) {
            // the quick graph: the driver's RTX VSR through D3D11 where it answers (else NVIDIA Image Scaling), no AI
            // clean-up, the cut guard without NVIDIA's optical flow; everything else as the full graph
            PushCoreSettings qs = ps;
            qs.srMethod = PUSHCORE_SR_METHOD_NIS; qs.nr = 0; if (qs.cutGuard & 2) qs.cutGuard &= ~2;
            wchar_t qv[8] = {};
            const bool quickVsr = (m_features & PUSHCORE_FEATURE_QUICK_VSR) && !(GetEnvironmentVariableW(L"PUSHMPC_FILTER_QuickVsr", qv, 8) && qv[0] == L'0');
            pc.settings = &qs;
            pc.flags = PUSHCORE_FLAG_NO_WARM_CACHE | (quickVsr ? PUSHCORE_FLAG_QUICK_VSR : 0u);
            m_core = SehOpen(m_api, &pc, &si, m_dev.Get(), eb, sizeof(eb));
            pc.settings = &ps; pc.flags = 0;
            if (m_core) {
                PushCoreStats qst = {}; qst.structSize = sizeof(qst);
                const bool vsr = quickVsr && m_api->stats(m_core, &qst) == PUSHCORE_OK && qst.quickVsr;
                Log(std::format("quick start: quick graph open in {} ms ({}); the full graph follows", GetTickCount64() - t0,
                    vsr ? "the driver's RTX VSR through the D3D11 video processor" : "NVIDIA Image Scaling"));
                StartFull(si, ps);
            } else {
                Log(std::string("quick start: the quick graph did not open (") + (eb[0] ? eb : "failed") + "): the full graph opens now");
                eb[0] = 0;
            }
        } else Log("quick start: not used (" + qwhy + ")");
        if (!m_core) {
            m_core = SehOpen(m_api, &pc, &si, m_dev.Get(), eb, sizeof(eb));
            if (m_core) m_fullOpened = true;
        }
        if (!m_core) { err = eb[0] ? eb : "pushcore_open failed"; ms = double(GetTickCount64() - t0); return 0; }
    }
    m_out = {};
    m_api->outputInfo(m_core, &m_out);
    m_key = key;
    m_owner = m_nextToken++;
    ms = double(GetTickCount64() - t0);
    return m_owner.load();
}
void CoreHost::Release(uint64_t token)
{
    if (!Owns(token)) return;
    ReturnAll();
    m_owner = 0;
    source = nullptr; alloc = nullptr; release = nullptr; tag = nullptr; sourceTex = nullptr; rawOut = nullptr;
}
bool CoreHost::Stats(PushCoreStats& st)
{
    memset(&st, 0, sizeof(st)); st.structSize = sizeof(st);
    return m_core && m_api->stats(m_core, &st) == PUSHCORE_OK;
}

// ------------------------------------------------------------------------------------------------ ingress
bool CoreHost::EnsureUpload(uint32_t w, uint32_t h, bool p010, std::string& err)
{
    if (!m_up.empty() && m_upW == w && m_upH == h && m_upP010 == p010) return true;
    m_up.clear(); m_upNext = 0;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.SampleDesc.Count = 1;
    d.Format = p010 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    for (int i = 0; i < 4; ++i) {
        Up u;
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        HRESULT hr = m_dev->CreateTexture2D(&d, nullptr, &u.staging);
        if (SUCCEEDED(hr)) { d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.CPUAccessFlags = 0; hr = m_dev->CreateTexture2D(&d, nullptr, &u.tex); }
        if (FAILED(hr)) { err = std::format("cannot create the {} upload textures {}x{} (0x{:08X})", p010 ? "P010" : "NV12", w, h, uint32_t(hr)); m_up.clear(); return false; }
        m_up.push_back(u);
    }
    m_upW = w; m_upH = h; m_upP010 = p010;
    return true;
}
bool CoreHost::CanSubmit() { return m_core && m_api->canSubmit(m_core) != 0; }
bool CoreHost::Submit(int64_t s, int64_t pts, bool discontinuity, std::string& err)
{
    if (sourceTex) {   // zero copy: the player's own texture (pushcore copies it into its ingress ring)
        ID3D11Texture2D* tex = nullptr; uint32_t slice = 0;
        if (!sourceTex(s, tex, slice) || !tex) { err = std::format("source texture {} is not available", s); return false; }
        PushCoreSubmitTiming t = {};
        t.structSize = PUSHCORE_SUBMIT_TIMING_SIZE_V2;
        t.streamNow100ns = INT64_MIN;
        const int r = m_api->submit2(m_core, tex, slice, pts, discontinuity ? PUSHCORE_SUBMIT_DISCONTINUITY : 0u, &t);
        if (r != PUSHCORE_OK) { err = std::format("pushcore_submit2 -> {} {}", r, m_api->lastError(m_core)); return false; }
        if (m_quick) Handover(tex, slice, pts);
        return true;
    }
    PlaneView v;
    if (!source || !source(s, v)) { err = std::format("source frame {} is not available", s); return false; }
    const bool p010 = v.bits > 8;
    if (!EnsureUpload(v.width, v.height, p010, err)) return false;
    Up& u = m_up[m_upNext]; m_upNext = (m_upNext + 1) % m_up.size();
    D3D11_MAPPED_SUBRESOURCE m = {};
    HRESULT hr = m_ctx->Map(u.staging.Get(), 0, D3D11_MAP_WRITE, 0, &m);
    if (FAILED(hr)) { err = std::format("Map(upload) failed (0x{:08X})", uint32_t(hr)); return false; }
    uint8_t* y = static_cast<uint8_t*>(m.pData);
    uint8_t* uv = y + size_t(m.RowPitch) * v.height;   // NV12 / P010: the chroma plane follows the luma rows
    const uint32_t cw = v.width / 2, ch = v.height / 2;
    if (!p010) {
        for (uint32_t r = 0; r < v.height; ++r) memcpy(y + size_t(r) * m.RowPitch, v.data[0] + r * v.stride[0], v.width);
        for (uint32_t r = 0; r < ch; ++r) {
            const uint8_t* pu = v.data[1] + r * v.stride[1]; const uint8_t* pv = v.data[2] + r * v.stride[2];
            uint8_t* o = uv + size_t(r) * m.RowPitch;
            for (uint32_t x = 0; x < cw; ++x) { o[2 * x] = pu[x]; o[2 * x + 1] = pv[x]; }
        }
    } else {
        const int shift = 16 - std::clamp(v.bits, 9, 16);   // P010 keeps the sample in the high bits
        for (uint32_t r = 0; r < v.height; ++r) {
            const uint16_t* pi = reinterpret_cast<const uint16_t*>(v.data[0] + r * v.stride[0]);
            uint16_t* o = reinterpret_cast<uint16_t*>(y + size_t(r) * m.RowPitch);
            for (uint32_t x = 0; x < v.width; ++x) o[x] = uint16_t(pi[x] << shift);
        }
        for (uint32_t r = 0; r < ch; ++r) {
            const uint16_t* pu = reinterpret_cast<const uint16_t*>(v.data[1] + r * v.stride[1]);
            const uint16_t* pv = reinterpret_cast<const uint16_t*>(v.data[2] + r * v.stride[2]);
            uint16_t* o = reinterpret_cast<uint16_t*>(uv + size_t(r) * m.RowPitch);
            for (uint32_t x = 0; x < cw; ++x) { o[2 * x] = uint16_t(pu[x] << shift); o[2 * x + 1] = uint16_t(pv[x] << shift); }
        }
    }
    m_ctx->Unmap(u.staging.Get(), 0);
    m_ctx->CopyResource(u.tex.Get(), u.staging.Get());
    PushCoreSubmitTiming t = {};
    t.structSize = PUSHCORE_SUBMIT_TIMING_SIZE_V2;  // no Dolby Vision block
    t.streamNow100ns = INT64_MIN;                     // no presentation clock: always the full group
    t.presentLead100ns = 0; t.fgLevel = 0;
    const int r = m_api->submit2(m_core, u.tex.Get(), 0, pts, discontinuity ? PUSHCORE_SUBMIT_DISCONTINUITY : 0u, &t);
    if (r != PUSHCORE_OK) { err = std::format("pushcore_submit2 -> {} {}", r, m_api->lastError(m_core)); return false; }
    if (m_quick) Handover(u.tex.Get(), 0, pts);
    return true;
}
bool CoreHost::Flush(std::string& err)
{
    ReturnAll();   // also retires the quick graph after a handover (a seek: a quiet moment)
    m_discardUpTo = INT64_MIN;
    const int r = m_api->flush(m_core);
    if (r != PUSHCORE_OK) { err = std::format("pushcore_flush -> {} {}", r, m_api->lastError(m_core)); return false; }
    return true;
}

// ------------------------------------------------------------------------------------------------ egress
static const char kCs[] = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<uint> outY : register(u0);
RWTexture2D<uint> outU : register(u1);
RWTexture2D<uint> outV : register(u2);
cbuffer C : register(b0) { float kr; float kb; float scale; float maxCode; uint w; uint h; uint pad0; uint pad1; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x * 2 >= w || id.y * 2 >= h) return;
    const float kg = 1.0 - kr - kb;
    float cb = 0, cr = 0;
    [unroll] for (uint j = 0; j < 2; ++j) [unroll] for (uint i = 0; i < 2; ++i) {
        const uint2 p = min(id.xy * 2 + uint2(i, j), uint2(w - 1, h - 1));
        const float3 c = saturate(src.Load(int3(p, 0)).rgb);
        const float y = kr * c.r + kg * c.g + kb * c.b;
        outY[p] = (uint)clamp(round((16.0 + 219.0 * y) * scale), 0, maxCode);
        cb += (c.b - y) / (2.0 * (1.0 - kb));
        cr += (c.r - y) / (2.0 * (1.0 - kr));
    }
    outU[id.xy] = (uint)clamp(round((128.0 + 224.0 * cb * 0.25) * scale), 0, maxCode);
    outV[id.xy] = (uint)clamp(round((128.0 + 224.0 * cr * 0.25) * scale), 0, maxCode);
}
)";
bool CoreHost::ConfigureEgress(OutFmt fmt, bool hdrMatrix, std::string& err, bool sdMatrix)
{
    m_fmt = fmt; m_hdrMatrix = hdrMatrix; m_sdMatrix = !hdrMatrix && sdMatrix;
    if (!OutIs420(fmt) || m_cs) return true;
    ComPtr<ID3DBlob> code, errs;
    HRESULT hr = D3DCompile(kCs, sizeof(kCs) - 1, "pushmpc_420", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errs);
    if (FAILED(hr)) { err = "4:2:0 shader: " + std::string(errs ? static_cast<const char*>(errs->GetBufferPointer()) : "D3DCompile failed"); return false; }
    hr = m_dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &m_cs);
    if (SUCCEEDED(hr)) { D3D11_BUFFER_DESC b = { 32, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 }; hr = m_dev->CreateBuffer(&b, nullptr, &m_cb); }
    if (FAILED(hr)) { err = std::format("4:2:0 shader objects (0x{:08X})", uint32_t(hr)); m_cs.Reset(); return false; }
    return true;
}
bool CoreHost::EnsureStaging(DXGI_FORMAT fmt, uint32_t w, uint32_t h, std::string& err)
{
    if (!OutIs420(m_fmt)) {
        if (m_stage && m_stageFmt == fmt && m_stageW == w && m_stageH == h) return true;
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.SampleDesc.Count = 1; d.Format = fmt;
        d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        m_stage.Reset();
        const HRESULT hr = m_dev->CreateTexture2D(&d, nullptr, &m_stage);
        if (FAILED(hr)) { err = std::format("readback texture {}x{} format {} (0x{:08X})", w, h, uint32_t(fmt), uint32_t(hr)); return false; }
        m_stageFmt = fmt; m_stageW = w; m_stageH = h;
        return true;
    }
    if (m_plane[0] && m_planeW == w && m_planeH == h) return true;
    for (int i = 0; i < 3; ++i) {
        const uint32_t pw = i ? w / 2 : w, ph = i ? h / 2 : h;
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = pw; d.Height = ph; d.MipLevels = 1; d.ArraySize = 1; d.SampleDesc.Count = 1; d.Format = DXGI_FORMAT_R16_UINT;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        m_plane[i].Reset(); m_uav[i].Reset(); m_planeStage[i].Reset();
        HRESULT hr = m_dev->CreateTexture2D(&d, nullptr, &m_plane[i]);
        if (SUCCEEDED(hr)) hr = m_dev->CreateUnorderedAccessView(m_plane[i].Get(), nullptr, &m_uav[i]);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (SUCCEEDED(hr)) hr = m_dev->CreateTexture2D(&d, nullptr, &m_planeStage[i]);
        if (FAILED(hr)) { err = std::format("4:2:0 plane textures {}x{} (0x{:08X})", pw, ph, uint32_t(hr)); m_plane[0].Reset(); return false; }
    }
    m_planeW = w; m_planeH = h;
    return true;
}
static DXGI_FORMAT Typed(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_UNORM;
    default: return f;
    }
}
bool CoreHost::Readback(PushCore* core, const PushCoreFrame& fr, OutImage& img, std::string& err)
{
    D3D11_TEXTURE2D_DESC td = {};
    fr.texture->GetDesc(&td);
    const uint32_t w = fr.width ? fr.width : td.Width, h = fr.height ? fr.height : td.Height;
    if (fr.readyFence) {
        if (!m_ctx4) { m_api->presented(core, fr.lease, nullptr); err = "pushcore hands out fences and this device has no ID3D11DeviceContext4"; return false; }
        m_ctx4->Wait(fr.readyFence, fr.readyValue);
    }
    if (!EnsureStaging(td.Format, td.Width, td.Height, err)) { m_api->presented(core, fr.lease, nullptr); return false; }
    if (OutIs420(m_fmt)) {
        auto& srv = m_srv[fr.texture];
        if (!srv) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
            sd.Format = Typed(td.Format); sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
            const HRESULT hr = m_dev->CreateShaderResourceView(fr.texture, &sd, &srv);
            if (FAILED(hr)) { m_srv.erase(fr.texture); m_api->presented(core, fr.lease, nullptr); err = std::format("the output texture cannot be read by a shader (0x{:08X}): use chroma=444", uint32_t(hr)); return false; }
        }
        const bool tenBit = m_fmt == OutFmt::Yuv420P10;
        struct { float kr, kb, scale, maxCode; uint32_t w, h, p0, p1; } cb = {
            m_hdrMatrix ? 0.2627f : m_sdMatrix ? 0.299f : 0.2126f, m_hdrMatrix ? 0.0593f : m_sdMatrix ? 0.114f : 0.0722f,
            tenBit ? 4.0f : 1.0f, tenBit ? 1023.0f : 255.0f, w, h, 0, 0 };
        m_ctx->UpdateSubresource(m_cb.Get(), 0, nullptr, &cb, 0, 0);
        ID3D11ShaderResourceView* srvs[1] = { srv.Get() };
        ID3D11UnorderedAccessView* uavs[3] = { m_uav[0].Get(), m_uav[1].Get(), m_uav[2].Get() };
        ID3D11Buffer* cbs[1] = { m_cb.Get() };
        m_ctx->CSSetShader(m_cs.Get(), nullptr, 0);
        m_ctx->CSSetShaderResources(0, 1, srvs);
        m_ctx->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
        m_ctx->CSSetConstantBuffers(0, 1, cbs);
        m_ctx->Dispatch((w / 2 + 7) / 8, (h / 2 + 7) / 8, 1);
        ID3D11ShaderResourceView* nullSrv[1] = {}; ID3D11UnorderedAccessView* nullUav[3] = {};
        m_ctx->CSSetShaderResources(0, 1, nullSrv);
        m_ctx->CSSetUnorderedAccessViews(0, 3, nullUav, nullptr);
        for (int i = 0; i < 3; ++i) m_ctx->CopyResource(m_planeStage[i].Get(), m_plane[i].Get());
    } else {
        m_ctx->CopyResource(m_stage.Get(), fr.texture);
    }
    m_api->presented(core, fr.lease, m_ctx.Get());   // the release fence follows the copy on our context
    if (OutIs420(m_fmt)) {
        for (int i = 0; i < 3; ++i) {
            D3D11_MAPPED_SUBRESOURCE m = {};
            const HRESULT hr = m_ctx->Map(m_planeStage[i].Get(), 0, D3D11_MAP_READ, 0, &m);
            if (FAILED(hr)) { err = std::format("Map(4:2:0 plane) failed (0x{:08X})", uint32_t(hr)); return false; }
            const uint32_t pw = i ? w / 2 : w, ph = i ? h / 2 : h;
            for (uint32_t r = 0; r < ph; ++r) {
                const uint16_t* s = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(m.pData) + size_t(r) * m.RowPitch);
                if (m_fmt == OutFmt::Yuv420P10) memcpy(img.data[i] + r * img.stride[i], s, size_t(pw) * 2);
                else { uint8_t* o = img.data[i] + r * img.stride[i]; for (uint32_t x = 0; x < pw; ++x) o[x] = uint8_t(s[x]); }
            }
            m_ctx->Unmap(m_planeStage[i].Get(), 0);
        }
        return true;
    }
    D3D11_MAPPED_SUBRESOURCE m = {};
    const HRESULT hr = m_ctx->Map(m_stage.Get(), 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) { err = std::format("Map(readback) failed (0x{:08X})", uint32_t(hr)); return false; }
    const DXGI_FORMAT f = Typed(td.Format);
    for (uint32_t r = 0; r < h; ++r) {
        const uint8_t* row = static_cast<const uint8_t*>(m.pData) + size_t(r) * m.RowPitch;
        if (f == DXGI_FORMAT_R10G10B10A2_UNORM) {
            const uint32_t* p = reinterpret_cast<const uint32_t*>(row);
            uint16_t* o[3]; for (int i = 0; i < 3; ++i) o[i] = reinterpret_cast<uint16_t*>(img.data[i] + r * img.stride[i]);
            if (m_fmt == OutFmt::Rgb30) for (uint32_t x = 0; x < w; ++x) { const uint32_t v = p[x]; o[0][x] = uint16_t(v & 1023u); o[1][x] = uint16_t((v >> 10) & 1023u); o[2][x] = uint16_t((v >> 20) & 1023u); }
            else { uint8_t* o8[3]; for (int i = 0; i < 3; ++i) o8[i] = img.data[i] + r * img.stride[i];
                   for (uint32_t x = 0; x < w; ++x) { const uint32_t v = p[x]; o8[0][x] = uint8_t((v & 1023u) >> 2); o8[1][x] = uint8_t(((v >> 10) & 1023u) >> 2); o8[2][x] = uint8_t(((v >> 20) & 1023u) >> 2); } }
        } else if (f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
            const bool bgr = f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            const int ri = bgr ? 2 : 0, bi = bgr ? 0 : 2;
            if (m_fmt == OutFmt::Rgb24) {
                uint8_t* o[3]; for (int i = 0; i < 3; ++i) o[i] = img.data[i] + r * img.stride[i];
                for (uint32_t x = 0; x < w; ++x) { o[0][x] = row[4 * x + ri]; o[1][x] = row[4 * x + 1]; o[2][x] = row[4 * x + bi]; }
            } else {
                uint16_t* o[3]; for (int i = 0; i < 3; ++i) o[i] = reinterpret_cast<uint16_t*>(img.data[i] + r * img.stride[i]);
                for (uint32_t x = 0; x < w; ++x) { o[0][x] = uint16_t(row[4 * x + ri] << 2); o[1][x] = uint16_t(row[4 * x + 1] << 2); o[2][x] = uint16_t(row[4 * x + bi] << 2); }
            }
        } else { m_ctx->Unmap(m_stage.Get(), 0); err = std::format("unsupported pushcore output format {}", uint32_t(f)); return false; }
    }
    m_ctx->Unmap(m_stage.Get(), 0);
    return true;
}
bool CoreHost::Poll(uint32_t timeoutMs, std::vector<Out<void*>>& out, std::string& err)
{
    PushCoreFrame fr[16]; uint32_t n = 0;
    // fast start: the quick graph until its last frame (the real one at the handover), then the full graph
    PushCore* src = m_old ? m_old : m_core;
    const int r = m_api->poll(src, fr, 16, &n, src == m_old ? 0 : timeoutMs);
    if (r != PUSHCORE_OK && r != PUSHCORE_BUSY) { err = std::format("pushcore_poll -> {} {}", r, m_api->lastError(src)); return false; }
    if (src == m_old && !n) {
        PushCoreStats ps = {}; ps.structSize = sizeof(ps);
        if (m_api->stats(src, &ps) == PUSHCORE_OK && ps.pendingFrames == 0) RetireOld("drained");
    }
    // the full graph's own output for the handover frame (and anything before it) is not shown: the quick graph's is
    uint32_t kept = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (src == m_core && m_discardUpTo != INT64_MIN) {
            if (fr[i].pts100ns <= m_discardUpTo) { m_api->presented(src, fr[i].lease, nullptr); continue; }
            m_discardUpTo = INT64_MIN;
        }
        fr[kept++] = fr[i];
    }
    n = kept;
    bool retire = false;
    for (uint32_t i = 0; i < n; ++i) if (src == m_old && fr[i].kind == PUSHCORE_FRAME_REAL && fr[i].pts100ns >= m_handoverPts) retire = true;
    if (rawOut) {      // zero copy: the caller converts on the device; nothing is read back
        for (uint32_t i = 0; i < n; ++i) {
            const bool drawn = rawOut(fr[i], err);
            m_api->presented(src, fr[i].lease, drawn ? m_ctx.Get() : nullptr);
            out.push_back({ fr[i].pts100ns, fr[i].kind == PUSHCORE_FRAME_GENERATED, nullptr });
        }
        if (retire) RetireOld("after its last frame");
        return true;
    }
    for (uint32_t i = 0; i < n; ++i) {
        OutImage img;
        const uint32_t w = fr[i].width ? fr[i].width : m_out.width, h = fr[i].height ? fr[i].height : m_out.height;
        if (!alloc || !alloc(w, h, img)) {
            for (uint32_t j = i; j < n; ++j) m_api->presented(src, fr[j].lease, nullptr);
            err = "cannot allocate an output frame"; return false;
        }
        if (!Readback(src, fr[i], img, err)) {
            if (release) release(img.handle);
            for (uint32_t j = i + 1; j < n; ++j) m_api->presented(src, fr[j].lease, nullptr);
            return false;
        }
        const bool gen = fr[i].kind == PUSHCORE_FRAME_GENERATED;
        if (tag) tag(fr[i].pts100ns, gen, img.handle);
        out.push_back({ fr[i].pts100ns, gen, img.handle });
    }
    if (retire) RetireOld("after its last frame");
    return true;
}

} // namespace pmvs
