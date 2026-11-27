// pm_pcsr_test: the Direct3D 11 output's pushcore upscaler (pm_pcsr_core.h, the VLC-free part of the output fork's
// pm_pcsr_d3d11.cpp) sharing the process's one pushcore with PushMPC's frame generation filter, on WARP with the stub core
// and the real bridge, both on one D3D11 device (as in VLC):
//   1. pm_pc_decide says the open's multiplier without opening (x1 and x2: the stub log shows no open, flush or reload);
//   2. (a) the output holds the core, the filter at x1 decides and does not open: the output keeps the core;
//   3. (b) the filter at x2 opens and takes the core over: the output closes, waits (backoff, then busy), and opens again
//      after the filter closes;
//   4. (c) taken over again and again: the wait doubles up to 60 s, the output never gives the core up for the stream;
//   5. Auto: the scaler (no core) until the picture is measured as drawn, then pushcore with the anime model; back to the
//      scaler when it leaves drawn, at most one switch per 30 s (first at once); with the filter at x2 holding the core,
//      the anime model waits for it;
//   6. the draw: pushcore's P010 picture into an RGB10A2 target keeps 10-bit steps (an RGBA8 one cannot); NV12 into BGRA8;
//   7. timings: the per-picture work (push, wait, drain) on the stub.
//   pm_pcsr_test <pm_pushcore.dll> <folder with the stub pushcore.dll> <scratch folder>
#include "pm_pcsr_core.h"
#include <d3d11.h>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

using Microsoft::WRL::ComPtr;
static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); fails++; } } while (0)

static const uint32_t W = 1280, H = 720;
static std::wstring g_stub, g_log;
static pmpc::BridgeFns g_b;
static pm_pc_decide_fn decide_;
static pm_pc_busy_fn busy_;
static ComPtr<ID3D11Device> g_dev;
static ComPtr<ID3D11DeviceContext> g_ctx;
static HANDLE g_mutex;
static ComPtr<ID3D11Texture2D> g_src;   // two NV12 slices (luma 60, 180)

static std::string StubLog()
{
    std::ifstream f(g_log, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}
// core-taking events in the stub log (an open, a reuse's flush, a reload)
static int CoreEvents()
{
    const std::string s = StubLog();
    int n = 0;
    for (const char* w : { "\nopen ", "\nflush", "\nreload", "\nsetSettings" })
        for (size_t p = s.find(w); p != std::string::npos; p = s.find(w, p + 1)) ++n;
    return n;
}

static pm_pc_cfg Base(int smoother)
{
    pm_pc_cfg c = {};
    c.abi = PM_PC_ABI; c.struct_size = sizeof(c);
    c.width = W; c.height = H; c.bits = 8; c.fps_num = 24; c.fps_den = 1;
    c.matrix = 1; c.transfer = 1; c.primaries = 1; c.range = 1; c.smoother = smoother; c.warp = 1; c.core_dir = g_stub.c_str();
    c.display_hz = 240;
    c.anime_model = PM_PC_ANIME_NONE;
    return c;
}

// the output's setup as pm_pcsr_d3d11.cpp makes it (sr to the screen, x1)
static pmpc::PcsrCore::Setup OutSetup(pmpc::PcsrMode mode)
{
    pmpc::PcsrCore::Setup s;
    s.dev = g_dev.Get(); s.mutex = g_mutex;
    s.cfg = Base(1);
    s.cfg.sr = 1; s.cfg.out_width = 2560; s.cfg.out_height = 1440;
    s.mode = mode; s.autoModel = "artcnn-c4f16"; s.frameMs = 1000.0 / 24;
    return s;
}
static std::vector<std::string> g_lines;
static pmpc::PcsrCore::LogFn Logger(const char* who)
{
    return [who](bool warn, const std::string& s) { g_lines.push_back(s); std::printf("    [%s%s] %s\n", who, warn ? " warn" : "", s.c_str()); };
}

// the filter as pushmpc_vlc.c opens it on the zero-copy path: decide first, open only when it generates frames
struct Filter {
    pm_pc* pc = nullptr;
    int k = 1;
    bool Open(int smoother)
    {
        pm_pc_cfg c = Base(smoother);
        char err[300] = "";
        int dk = 1;
        CHECK(decide_(&c, &dk, err, sizeof(err)) == 0, "decide: %s", err);
        if (dk <= 1) { k = 1; return false; }
        pc = g_b.open(&c, g_dev.Get(), g_mutex, &k, err, sizeof(err));
        CHECK(pc != nullptr, "filter open: %s", err);
        if (pc && k <= 1) { g_b.close(pc); pc = nullptr; }
        return pc != nullptr;
    }
    void Close() { if (pc) g_b.close(pc); pc = nullptr; }
};

static int64_t g_pts = 0;
static double g_now = 100.0;
// one picture to the output (24 fps clock); returns Got()
static bool Feed(pmpc::PcsrCore& o, double dt = 1.0 / 24)
{
    g_now += dt;
    g_pts += 416667;
    const bool ok = o.Feed(g_src.Get(), unsigned((g_pts / 416667) & 1), g_pts, false, g_now);
    CHECK(ok, "Feed kept going");
    return o.Got();
}

static void TestDecide()
{
    std::printf("== 1. pm_pc_decide: the open's multiplier, without taking the core\n");
    for (int sm : { 1, 2 }) {
        pm_pc_cfg c = Base(sm);
        char err[300] = "";
        int dk = 0;
        const int before = CoreEvents();
        CHECK(decide_(&c, &dk, err, sizeof(err)) == 0, "decide x%d: %s", sm, err);
        CHECK(CoreEvents() == before, "decide x%d took the core (stub log)", sm);
        CHECK(!busy_(), "decide x%d left the core busy", sm);
        int ok = 0;
        pm_pc* pc = g_b.open(&c, g_dev.Get(), g_mutex, &ok, err, sizeof(err));
        CHECK(pc != nullptr, "open x%d: %s", sm, err);
        std::printf("  smoother %d: decide x%d (%s), open x%d\n", sm, dk, err, ok);
        CHECK(dk == ok, "decide x%d == open x%d", dk, ok);
        CHECK(busy_() == 1, "busy while open");
        if (pc) g_b.close(pc);
        CHECK(!busy_(), "free after close");
    }
}

static void TestDisplayFirst()
{
    std::printf("== 2. (a) the output first, then the filter at x1: the output keeps the core\n");
    pmpc::PcsrCore o;
    std::string why;
    CHECK(o.Init(g_b, OutSetup(pmpc::PcsrMode::Anime), Logger("out"), why), "init: %s", why.c_str());
    int got = 0;
    for (int i = 0; i < 6; ++i) got += Feed(o) ? 1 : 0;
    CHECK(o.Running() && got >= 4, "the output runs pushcore (%d of 6 pictures)", got);
    const int before = CoreEvents();
    Filter f;
    CHECK(!f.Open(1), "the filter at x1 does not open");
    CHECK(CoreEvents() == before, "the filter's decision did not touch the core (stub log)");
    got = 0;
    for (int i = 0; i < 6; ++i) got += Feed(o) ? 1 : 0;
    CHECK(o.Running() && got >= 5 && o.Stat().lost == 0, "the output still has the core (%d of 6, lost %d)", got, o.Stat().lost);
}

static void TestFilterTakes()
{
    std::printf("== 3. (b) the filter at x2 takes the core over; the output waits, then opens again\n");
    pmpc::PcsrCore o;
    std::string why;
    CHECK(o.Init(g_b, OutSetup(pmpc::PcsrMode::Anime), Logger("out"), why), "init: %s", why.c_str());
    for (int i = 0; i < 4; ++i) Feed(o);
    CHECK(o.Running(), "the output runs pushcore");
    Filter f;
    CHECK(f.Open(2) && f.k == 2, "the filter at x2 opens (k %d)", f.k);
    Feed(o);
    CHECK(!o.Running() && o.Stat().lost == 1, "taken over: the output closed (lost %d)", o.Stat().lost);
    CHECK(std::fabs(o.Backoff().Delay() - 2.0) < 1e-9, "first wait 2 s (%.1f)", o.Backoff().Delay());
    const int opens = o.Stat().opens;
    for (int i = 0; i < 24; ++i) Feed(o);   // 1 s: inside the wait
    CHECK(o.Stat().opens == opens && !o.Running(), "no open inside the wait");
    for (int i = 0; i < 48; ++i) Feed(o);   // past it, the filter still holds the core: busy, looked at again shortly
    CHECK(o.Stat().opens == opens && !o.Running() && o.Backoff().BusyCount() > 0, "busy: not opened (busy %d)", o.Backoff().BusyCount());
    CHECK(std::fabs(o.Backoff().Delay() - 2.0) < 1e-9, "busy does not grow the wait (%.1f)", o.Backoff().Delay());
    f.Close();
    int got = 0;   // busy is looked at again every 0.5 s (12 pictures here), then pushcore's pictures come in
    for (int i = 0; i < 48; ++i) got += Feed(o) ? 1 : 0;
    CHECK(o.Running() && o.Stat().opens == opens + 1 && got >= 30, "after the filter closed: the output opened again (%d of 48 pictures)", got);
}

static void TestTakenOverAgain()
{
    std::printf("== 4. (c) taken over again and again: a capped wait, never given up\n");
    pmpc::PcsrCore o;
    std::string why;
    CHECK(o.Init(g_b, OutSetup(pmpc::PcsrMode::Anime), Logger("out"), why), "init: %s", why.c_str());
    Feed(o);
    std::vector<double> waits;
    for (int round = 0; round < 8; ++round) {
        CHECK(o.Running(), "round %d: the output holds the core", round);
        Filter f;
        f.Open(2);
        Feed(o);                                        // taken over
        waits.push_back(o.Backoff().Delay());
        f.Close();
        const double until = o.Backoff().At();
        while (g_now < until) Feed(o, 0.5);              // the wait (skipped in big steps)
        Feed(o);                                         // free again: opened
    }
    std::printf("  waits:");
    for (double w : waits) std::printf(" %.0f", w);
    std::printf(" s\n");
    const double want[8] = { 2, 4, 8, 16, 32, 60, 60, 60 };
    for (int i = 0; i < 8; ++i) CHECK(std::fabs(waits[size_t(i)] - want[i]) < 1e-9, "wait %d: %.0f (want %.0f)", i, waits[size_t(i)], want[i]);
    CHECK(o.Running() && o.Stat().lost == 8, "the output runs pushcore after 8 takeovers (lost %d)", o.Stat().lost);
    // a discontinuity (a new file or a seek; VLC's picture dates are system dates: forward) resets the wait
    Filter f;
    f.Open(2);
    Feed(o);
    f.Close();
    CHECK(o.Backoff().Delay() == 60 && !o.Running(), "taken over again: 60 s");
    g_pts += 10 * 10000000LL;
    CHECK(o.Feed(g_src.Get(), 0, g_pts, true, g_now + 0.04), "new stream");
    g_now += 0.04;
    CHECK(o.Backoff().Delay() == 0 && o.Running(), "a discontinuity: no wait (%.0f), opened at once", o.Backoff().Delay());
    // and backwards (the clock starts over)
    f.Open(2);
    Feed(o);
    f.Close();
    g_pts = 0;
    CHECK(o.Feed(g_src.Get(), 0, 0, true, g_now + 0.04), "new stream");
    g_now += 0.04;
    CHECK(o.Backoff().Delay() == 0 && o.Running(), "a new stream: no wait (%.0f), opened at once", o.Backoff().Delay());
}

// Auto with a scripted measured class (the measuring instance is the real one; its class is replaced)
static pm_pc* g_meas = nullptr;
static int g_cls = 0;
static pm_pc_output_fn g_realOutput;
static pm_pc_measure_open_fn g_realMeasOpen;
static int ScriptOutput(pm_pc* pc, pm_pc_info* i)
{
    const int r = g_realOutput(pc, i);
    if (pc == g_meas && r == 0) { i->measured_class = g_cls; i->measured_cg = 0; }
    return r;
}
static pm_pc* ScriptMeasOpen(const pm_pc_cfg* c, void* d, void* m, char* e, size_t n)
{
    g_meas = g_realMeasOpen(c, d, m, e, n);
    return g_meas;
}

static void TestAuto()
{
    std::printf("== 5. Auto: the scaler until drawn is measured, then the anime model; back when it leaves drawn\n");
    pmpc::BridgeFns b = g_b;
    b.output = ScriptOutput; b.measureOpen = ScriptMeasOpen;
    pmpc::PcsrCore o;
    std::string why;
    CHECK(o.Init(b, OutSetup(pmpc::PcsrMode::Auto), Logger("auto"), why), "init: %s", why.c_str());
    CHECK(o.MeasuresAlone() && !busy_(), "measured without the core");
    g_cls = 0;
    for (int i = 0; i < 12; ++i) Feed(o);
    CHECK(!o.Running(), "measuring: the scaler");
    g_cls = 3;
    for (int i = 0; i < 12; ++i) Feed(o);
    CHECK(!o.Running() && !busy_(), "not drawn: the scaler, the core free");
    g_cls = 1;
    const double t0 = g_now;
    int got = 0;
    for (int i = 0; i < 12; ++i) got += Feed(o) ? 1 : 0;
    CHECK(o.Running() && o.Model() == "artcnn-c4f16" && got >= 8, "drawn: pushcore with the anime model at once (%s, %d pictures)", o.Model().c_str(), got);
    g_cls = 3;
    while (g_now < t0 + 20) Feed(o, 0.5);
    CHECK(o.Running(), "left drawn 20 s after the switch: still the anime model (30 s limit)");
    while (g_now < t0 + 31) Feed(o, 0.5);
    CHECK(!o.Running() && !busy_(), "31 s after: the scaler again, the core free");
    const double t1 = g_now;
    g_cls = 1;
    Feed(o);
    CHECK(!o.Running(), "drawn again right after a switch: waits for the 30 s limit");
    while (g_now < t1 + 30.5) Feed(o, 0.5);
    CHECK(o.Running(), "30 s later: the anime model again");
    // the filter at x2 holds the core: the anime model waits for it
    Filter f;
    CHECK(f.Open(2), "the filter at x2 opens");
    Feed(o);
    CHECK(!o.Running(), "taken over: the scaler");
    while (g_now < t1 + 100) Feed(o, 0.5);
    CHECK(!o.Running(), "the filter holds the core: still the scaler");
    f.Close();
    while (g_now < t1 + 101) Feed(o, 0.25);
    CHECK(o.Running() && o.Model() == "artcnn-c4f16", "the filter closed: the anime model again");
}

// ---- the draw
static bool ReadPixels(ID3D11Texture2D* t, std::vector<uint32_t>& px, unsigned& w, unsigned& h, DXGI_FORMAT& f)
{
    D3D11_TEXTURE2D_DESC d = {};
    t->GetDesc(&d);
    w = d.Width; h = d.Height; f = d.Format;
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> st;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &st))) return false;
    g_ctx->CopyResource(st.Get(), t);
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(g_ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    px.resize(size_t(w) * h);
    for (unsigned y = 0; y < h; ++y) memcpy(&px[size_t(y) * w], static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch, size_t(w) * 4);
    g_ctx->Unmap(st.Get(), 0);
    return true;
}
static void TestDraw()
{
    std::printf("== 6. the draw: P010 into RGB10A2 keeps 10-bit steps; NV12 into BGRA8\n");
    // a P010 picture: the left half luma code 500, the right half 501 (10-bit), chroma neutral 512
    const unsigned w = 64, h = 32;
    std::vector<uint16_t> p010(size_t(w) * h * 3 / 2);
    for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) p010[size_t(y) * w + x] = uint16_t((x < w / 2 ? 500 : 501) << 6);
    for (size_t i = size_t(w) * h; i < p010.size(); ++i) p010[i] = uint16_t(512 << 6);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_P010; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = { p010.data(), w * 2, 0 };
    ComPtr<ID3D11Texture2D> pic;
    CHECK(SUCCEEDED(g_dev->CreateTexture2D(&td, &init, &pic)), "P010 picture");
    D3D11_SHADER_RESOURCE_VIEW_DESC v = {};
    v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; v.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> sy, suv;
    v.Format = DXGI_FORMAT_R16_UNORM; g_dev->CreateShaderResourceView(pic.Get(), &v, &sy);
    v.Format = DXGI_FORMAT_R16G16_UNORM; g_dev->CreateShaderResourceView(pic.Get(), &v, &suv);
    pmpc::PcsrDraw d;
    std::string why;
    CHECK(d.Init(g_dev.Get(), why), "draw init: %s", why.c_str());
    for (DXGI_FORMAT tf : { DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM }) {
        D3D11_TEXTURE2D_DESC o = {};
        o.Width = w + 16; o.Height = h + 8; o.MipLevels = 1; o.ArraySize = 1; o.Format = tf; o.SampleDesc.Count = 1;
        o.Usage = D3D11_USAGE_DEFAULT; o.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> out;
        CHECK(SUCCEEDED(g_dev->CreateTexture2D(&o, nullptr, &out)), "target");
        // the player's own pipeline state is kept (the draw runs in a state of its own)
        D3D11_VIEWPORT keep = { 1, 2, 3, 4, 0, 1 };
        g_ctx->RSSetViewports(1, &keep);
        CHECK(d.Draw(g_ctx.Get(), sy.Get(), suv.Get(), true, true, out.Get(), 8, 4, w, h, why), "draw: %s", why.c_str());
        UINT n = 1; D3D11_VIEWPORT after = {};
        g_ctx->RSGetViewports(&n, &after);
        CHECK(n == 1 && after.TopLeftX == 1 && after.Width == 3, "the player's viewport kept");
        std::vector<uint32_t> px; unsigned ow, oh; DXGI_FORMAT of;
        CHECK(ReadPixels(out.Get(), px, ow, oh, of), "read back");
        const bool ten = tf == DXGI_FORMAT_R10G10B10A2_UNORM;
        auto g = [&](unsigned x, unsigned y) { const uint32_t p = px[size_t(y) * ow + x]; return ten ? double((p >> 10) & 1023) : double((p >> 8) & 255); };
        // inside the place, away from the 500 | 501 edge (bilinear): the expected full-range code
        const double l = g(8 + 4, 4 + 8), r = g(8 + w - 5, 4 + 8), bar = g(2, 2);
        const double yl = (500.0 - 64) / 876, yr = (501.0 - 64) / 876;
        const double wl = yl * (ten ? 1023 : 255), wr = yr * (ten ? 1023 : 255);
        std::printf("  %s: left %.0f (want %.1f), right %.0f (want %.1f), outside the place %.0f\n", ten ? "RGB10A2" : "RGBA8", l, wl, r, wr, bar);
        CHECK(std::fabs(l - wl) <= 1 && std::fabs(r - wr) <= 1, "codes within 1");
        if (ten) CHECK(r - l >= 1, "RGB10A2 keeps the one-code 10-bit step (%.0f -> %.0f)", l, r);
        CHECK(bar == 0, "outside the place untouched");
    }
    // NV12 into BGRA8 (an 8-bit source on the driver scaler's usual texture)
    std::vector<uint8_t> nv12(size_t(w) * h * 3 / 2, 128);
    memset(nv12.data(), 180, size_t(w) * h);
    td.Format = DXGI_FORMAT_NV12;
    D3D11_SUBRESOURCE_DATA i8 = { nv12.data(), w, 0 };
    ComPtr<ID3D11Texture2D> p8;
    CHECK(SUCCEEDED(g_dev->CreateTexture2D(&td, &i8, &p8)), "NV12 picture");
    ComPtr<ID3D11ShaderResourceView> y8, uv8;
    v.Format = DXGI_FORMAT_R8_UNORM; g_dev->CreateShaderResourceView(p8.Get(), &v, &y8);
    v.Format = DXGI_FORMAT_R8G8_UNORM; g_dev->CreateShaderResourceView(p8.Get(), &v, &uv8);
    D3D11_TEXTURE2D_DESC o = {};
    o.Width = w; o.Height = h; o.MipLevels = 1; o.ArraySize = 1; o.Format = DXGI_FORMAT_B8G8R8A8_UNORM; o.SampleDesc.Count = 1;
    o.Usage = D3D11_USAGE_DEFAULT; o.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> out8;
    g_dev->CreateTexture2D(&o, nullptr, &out8);
    CHECK(d.Draw(g_ctx.Get(), y8.Get(), uv8.Get(), false, false, out8.Get(), 0, 0, w, h, why), "draw NV12: %s", why.c_str());
    std::vector<uint32_t> px; unsigned ow, oh; DXGI_FORMAT of;
    ReadPixels(out8.Get(), px, ow, oh, of);
    const double gv = double((px[size_t(10) * ow + 10] >> 8) & 255), want = (180.0 - 16) / 219 * 255;
    std::printf("  BGRA8: %.0f (want %.1f)\n", gv, want);
    CHECK(std::fabs(gv - want) <= 1, "NV12 -> BGRA8 code");
}

static void TestTimings()
{
    std::printf("== 7. per-picture work on the stub (the output thread's share)\n");
    pmpc::PcsrCore o;
    std::string why;
    CHECK(o.Init(g_b, OutSetup(pmpc::PcsrMode::Anime), Logger("out"), why), "init: %s", why.c_str());
    int got = 0;
    for (int i = 0; i < 120; ++i) got += Feed(o) ? 1 : 0;
    const auto& s = o.Stat();
    std::printf("  pushcore's picture in time for %d of 120 pictures\n", got);
    CHECK(got >= 110, "pushcore's pictures in time (%d of 120)", got);
    std::printf("  %d pictures: %.2f ms on average, %.0f ms at most (GetTickCount64 steps); open at most %.0f ms; bound %.0f ms\n",
                s.feeds, s.feedMsSum / std::max(1, s.feeds), s.feedMsMax, s.openMsMax, std::clamp(1.5 * 1000.0 / 24, 12.0, 50.0));
    CHECK(s.feedMsMax <= 50 + 16 + s.openMsMax, "the wait stays within the 50 ms bound (open aside)");
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 4) { std::printf("usage: pm_pcsr_test <pm_pushcore.dll> <stub core folder> <scratch folder>\n"); return 2; }
    g_stub = argv[2];
    g_log = std::wstring(argv[3]) + L"\\pm_pcsr_stub.log";
    DeleteFileW(g_log.c_str());
    SetEnvironmentVariableW(L"PUSHCORE_STUB_LOG", g_log.c_str());
    SetEnvironmentVariableW(L"PUSHCORE_STUB_SR", L"1");
    HMODULE b = LoadLibraryW(argv[1]);
    if (!b) { std::printf("cannot load %ls\n", argv[1]); return 1; }
    g_b = pmpc::BridgeFns::From(b);
    decide_ = reinterpret_cast<pm_pc_decide_fn>(GetProcAddress(b, "pm_pc_decide"));
    busy_ = reinterpret_cast<pm_pc_busy_fn>(GetProcAddress(b, "pm_pc_busy"));
    if (!g_b.open || !g_b.update || !g_b.output || !g_b.Sharing() || !g_b.Measures() || !decide_ || !busy_) { std::printf("FAIL: exports missing\n"); return 1; }
    g_realOutput = g_b.output; g_realMeasOpen = g_b.measureOpen;
    const D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl, 2, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx))) {
        std::printf("FAIL: no WARP device\n"); return 1;
    }
    g_mutex = CreateMutexExW(nullptr, nullptr, 0, SYNCHRONIZE);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 2; td.Format = DXGI_FORMAT_NV12; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<uint8_t> f0(size_t(W) * H * 3 / 2, 128), f1(f0.size(), 128);
    memset(f0.data(), 60, size_t(W) * H); memset(f1.data(), 180, size_t(W) * H);
    D3D11_SUBRESOURCE_DATA init[2] = { { f0.data(), W, 0 }, { f1.data(), W, 0 } };
    if (FAILED(g_dev->CreateTexture2D(&td, init, &g_src))) { std::printf("FAIL: source\n"); return 1; }
    TestDecide();
    TestDisplayFirst();
    TestFilterTakes();
    TestTakenOverAgain();
    TestAuto();
    TestDraw();
    TestTimings();
    CloseHandle(g_mutex);
    std::printf(fails ? "pm_pcsr_test: %d of %d checks FAILED\n" : "pm_pcsr_test: %d checks passed\n", fails ? fails : checks, checks);
    return fails ? 1 : 0;
}
