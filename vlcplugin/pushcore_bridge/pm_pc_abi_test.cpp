// pm_pc_abi_test: pm_pushcore.dll's configuration ABI on WARP with the stub pushcore (PUSHCORE_STUB_SR=1: the stub honours
// pushcore's upscaler and reports held frames / the anime upscaler / the anime holds; every settings change is logged).
//   1. an ABI 2 caller (its smaller struct) still opens, at the source size, the real frames left to it;
//   2. ABI 3 with sr: the output is the source fitted into pushcore's target box, every picture (real ones too) comes
//      out at that size through the planes and through the zero-copy path, with the stub's blend in the generated one;
//   3. pm_pc_update: drawn animation measured -> the anime upscaler (PUSHCORE_SR_METHOD_PMML) at the same output size,
//      warm (setSettings + reload); the same values again change nothing; another output size is refused;
//   4. Auto's drawn treatment: held-frame re-timing only for a caller that waits for it; on drawn CG no line art or
//      sign hold; deband only where the real frames are pushcore's.
//   pm_pc_abi_test <pm_pushcore.dll> <folder with the stub pushcore.dll> <scratch folder for the stub's log>
#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "pm_pushcore.h"
#include "pushmpc/pushcore_abi.h"   // PUSHCORE_SR_METHOD_*, PUSHCORE_HOLD_*, PUSHCORE_DEBAND_*

using Microsoft::WRL::ComPtr;
static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); fails++; } } while (0)

// the ABI 2 layout, as a caller built before ABI 3 has it
struct CfgV2 {
    uint32_t abi; uint32_t width, height; int bits; int fps_num, fps_den; int matrix, transfer, primaries, range;
    int smoother; double display_hz; int warp; const wchar_t* core_dir; int fullscreen;
};
static_assert(offsetof(CfgV2, fullscreen) == offsetof(pm_pc_cfg, fullscreen), "ABI 2 prefix moved");
static_assert(sizeof(CfgV2) <= sizeof(pm_pc_cfg), "ABI 3 must only append");

static const uint32_t W = 1280, H = 720;
static pm_pc_open_fn open_; static pm_pc_push_fn push_; static pm_pc_poll_fn poll_; static pm_pc_close_fn close_;
static pm_pc_describe_fn describe_; static pm_pc_abi_fn abi_; static pm_pc_update_fn update_; static pm_pc_output_fn output_;
static pm_pc_open_d3d11_fn openD_; static pm_pc_push_d3d11_fn pushD_; static pm_pc_poll_d3d11_fn pollD_;
static std::wstring g_stub, g_log;

static pm_pc_cfg Base(int k)
{
    pm_pc_cfg c = {};
    c.abi = PM_PC_ABI; c.struct_size = sizeof(c);
    c.width = W; c.height = H; c.bits = 8; c.fps_num = 24; c.fps_den = 1;
    c.matrix = 1; c.transfer = 1; c.primaries = 1; c.range = 1; c.smoother = k; c.warp = 1; c.core_dir = g_stub.c_str();
    c.display_hz = 240;
    c.anime_model = PM_PC_ANIME_NONE; c.held_frames = 0; c.deband = 0;
    return c;
}
static pm_pc_info Info(pm_pc* pc)
{
    pm_pc_info i = {}; i.struct_size = sizeof(i);
    CHECK(output_(pc, &i) == 0, "pm_pc_output");
    return i;
}
static std::string StubLog()
{
    std::ifstream f(g_log, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}
static std::string LastSettings()
{
    const std::string s = StubLog();
    const size_t p = s.rfind("settings ");
    return p == std::string::npos ? std::string() : s.substr(p, s.find('\r', p) - p);
}

// planes: two flat frames (luma 60, 180), out buffers at the output size
struct Planes { std::vector<uint8_t> b; pm_pc_planes p{}; };
static Planes Flat(uint32_t w, uint32_t h, int luma)
{
    Planes f; f.b.assign(size_t(w) * h * 3 / 2, 128);
    memset(f.b.data(), luma, size_t(w) * h);
    f.p.data[0] = f.b.data(); f.p.data[1] = f.b.data() + size_t(w) * h; f.p.data[2] = f.p.data[1] + size_t(w / 2) * (h / 2);
    f.p.stride[0] = w; f.p.stride[1] = f.p.stride[2] = w / 2;
    return f;
}
struct Sink {
    uint32_t w = 0, h = 0;
    std::vector<std::vector<uint8_t>> bufs; std::vector<int64_t> pts; pm_pc_out o{}; int done = 0;
};
static const pm_pc_out* SinkDest(void* u, int64_t pts)
{
    auto* s = static_cast<Sink*>(u);
    s->bufs.emplace_back(size_t(s->w) * s->h * 3 / 2, 0);
    s->pts.push_back(pts);
    uint8_t* b = s->bufs.back().data();
    s->o.data[0] = b; s->o.data[1] = b + size_t(s->w) * s->h; s->o.data[2] = s->o.data[1] + size_t(s->w / 2) * (s->h / 2);
    s->o.stride[0] = s->w; s->o.stride[1] = s->o.stride[2] = s->w / 2;
    return &s->o;
}
static void SinkDone(void* u, int64_t) { static_cast<Sink*>(u)->done++; }
static double MeanY(const std::vector<uint8_t>& b, uint32_t w, uint32_t h)
{
    double s = 0; for (size_t i = 0; i < size_t(w) * h; ++i) s += b[i];
    return s / (double(w) * h);
}
static int Pump(pm_pc* pc, Sink& s, int want)
{
    char err[256] = "";
    for (int i = 0; i < 40 && s.done < want; ++i) if (poll_(pc, 50, SinkDest, SinkDone, &s, err, sizeof(err)) < 0) { std::printf("poll: %s\n", err); break; }
    return s.done;
}

static void TestAbi2()
{
    std::printf("== 1. an ABI 2 caller\n");
    CfgV2 c = {};
    c.abi = 2; c.width = W; c.height = H; c.bits = 8; c.fps_num = 24; c.fps_den = 1; c.matrix = 1; c.transfer = 1; c.primaries = 1; c.range = 1;
    c.smoother = 2; c.display_hz = 240; c.warp = 1; c.core_dir = g_stub.c_str();
    int k = 0; char err[256] = "";
    pm_pc* pc = open_(reinterpret_cast<const pm_pc_cfg*>(&c), &k, err, sizeof(err));
    CHECK(pc != nullptr, "ABI 2 open: %s", err);
    if (!pc) return;
    const pm_pc_info i = Info(pc);
    std::printf("  %s\n  out %ux%u k %d sr %d real_out %d held %d deband %d\n", describe_(pc), i.width, i.height, i.k, i.sr, i.real_out, i.held_frames, i.deband);
    CHECK(i.width == W && i.height == H && i.sr == 0 && i.real_out == 0 && i.held_frames == 0 && i.deband == 0 && k == 2, "ABI 2 runs as before");
    Planes a = Flat(W, H, 60), b = Flat(W, H, 180);
    CHECK(push_(pc, &a.p, 0, 1, err, sizeof(err)) == 0 && push_(pc, &b.p, 417083, 0, err, sizeof(err)) == 0, "push: %s", err);
    Sink s; s.w = W; s.h = H;
    Pump(pc, s, 1);
    for (int g = 0; g < 4; ++g) poll_(pc, 20, SinkDest, SinkDone, &s, err, sizeof(err));
    CHECK(s.done == 1, "ABI 2: only the generated frame goes out (%d)", s.done);
    if (s.done >= 1) CHECK(std::fabs(MeanY(s.bufs[0], W, H) - 120) < 3, "ABI 2 generated luma %.1f", MeanY(s.bufs[0], W, H));
    // a newer ABI with a struct too small for ABI 3 is refused
    pm_pc_cfg bad = Base(2); bad.struct_size = 16;
    CHECK(open_(&bad, &k, err, sizeof(err)) == nullptr, "a short ABI 3 struct must be refused");
    std::printf("  short struct: %s\n", err);
    close_(pc);
}

static void TestSrPlanes()
{
    std::printf("== 2. ABI 3, sr to 2560x1440 (planes)\n");
    pm_pc_cfg c = Base(2);
    c.sr = 1; c.out_width = 2560; c.out_height = 1440;
    int k = 0; char err[256] = "";
    pm_pc* pc = open_(&c, &k, err, sizeof(err));
    CHECK(pc != nullptr, "sr open: %s", err);
    if (!pc) return;
    pm_pc_info i = Info(pc);
    std::printf("  %s\n  out %ux%u sr %d method %u anime %d real_out %d\n", describe_(pc), i.width, i.height, i.sr, i.sr_method, i.anime_model, i.real_out);
    CHECK(i.width == 2560 && i.height == 1440 && i.sr == 1 && i.real_out == 1 && i.anime_model == -1, "sr output");
    CHECK(i.sr_method == PUSHCORE_SR_METHOD_RTX, "the default upscaler on an RTX-class core (%u)", i.sr_method);
    CHECK(LastSettings().find("sr 1 target 0") != std::string::npos, "stub got sr with the 1440p target: %s", LastSettings().c_str());
    Planes a = Flat(W, H, 60), b = Flat(W, H, 180);
    CHECK(push_(pc, &a.p, 0, 1, err, sizeof(err)) == 0 && push_(pc, &b.p, 417083, 0, err, sizeof(err)) == 0, "push: %s", err);
    Sink s; s.w = i.width; s.h = i.height;
    Pump(pc, s, 3);
    for (int g = 0; g < 4; ++g) poll_(pc, 20, SinkDest, SinkDone, &s, err, sizeof(err));
    std::printf("  %d pictures out:", s.done);
    for (int j = 0; j < s.done; ++j) std::printf(" %lld:%.1f", (long long)s.pts[size_t(j)], MeanY(s.bufs[size_t(j)], s.w, s.h));
    std::printf("\n");
    CHECK(s.done == 3, "real, generated, real at the output size (%d)", s.done);
    if (s.done == 3) {
        CHECK(s.pts[0] == 0 && s.pts[2] == 417083, "the real frames keep their times");
        CHECK(std::fabs(MeanY(s.bufs[0], s.w, s.h) - 60) < 1.5 && std::fabs(MeanY(s.bufs[1], s.w, s.h) - 120) < 3 && std::fabs(MeanY(s.bufs[2], s.w, s.h) - 180) < 1.5,
              "pictures 60 / 120 / 180");
    }

    std::printf("== 3. pm_pc_update\n");
    pm_pc_cfg u = c;
    u.content_class = 1; u.anime_model = 0;   // drawn: the caller's switch runs ArtCNN C4F16
    int r = update_(pc, &u, err, sizeof(err));
    i = Info(pc);
    std::printf("  update -> %d (%s); %s\n", r, err, describe_(pc));
    CHECK(r == 0 && i.sr_method == PUSHCORE_SR_METHOD_PMML && i.anime_model == 0 && i.width == 2560 && i.height == 1440 && i.updates == 1, "anime upscaler at the same size");
    CHECK(LastSettings().find("settings reload sr 1 target 0 method 6 model 0") != std::string::npos, "stub reloaded with PMML: %s", LastSettings().c_str());
    CHECK(StubLog().find("setSettings") != std::string::npos, "warm: setSettings + reload");
    err[0] = 0;
    CHECK(update_(pc, &u, err, sizeof(err)) == 1, "the same values: nothing to do");
    u.anime_model = PM_PC_ANIME_NONE; u.content_class = 3;
    CHECK(update_(pc, &u, err, sizeof(err)) == 0 && Info(pc).sr_method == PUSHCORE_SR_METHOD_RTX && Info(pc).anime_model == -1, "back to Video Super Resolution: %s", err);
    u.anime_model = 1;   // a _DS model asked for: the caller's choice is honoured (its switch keeps _DS to hand-drawn)
    CHECK(update_(pc, &u, err, sizeof(err)) == 0 && Info(pc).anime_model == 1, "a _DS model when asked");
    pm_pc_cfg bad = u; bad.out_width = 3840; bad.out_height = 2160;
    CHECK(update_(pc, &bad, err, sizeof(err)) < 0, "another output size must be refused");
    std::printf("  refused: %s\n", err);
    CHECK(Info(pc).k == k && k == 2, "the multiplier stays the open's");
    // still running after the updates
    Sink s2; s2.w = i.width; s2.h = i.height;
    CHECK(push_(pc, &a.p, 834166, 1, err, sizeof(err)) == 0 && push_(pc, &b.p, 1251249, 0, err, sizeof(err)) == 0, "push after update: %s", err);
    Pump(pc, s2, 3);
    CHECK(s2.done == 3, "pictures after the update (%d)", s2.done);
    close_(pc);
}

static void TestDrawn()
{
    std::printf("== 4. Auto's drawn treatment\n");
    char err[256] = ""; int k = 0;
    // held frames: Auto runs them only for a caller that waits; hand-drawn gets line art + sign hold where Auto keeps them
    pm_pc_cfg c = Base(2);
    c.real_out = 1; c.held_wait = 1; c.held_frames = PM_PC_AUTO; c.deband = PM_PC_AUTO; c.content_class = 1;
    pm_pc* pc = open_(&c, &k, err, sizeof(err));
    CHECK(pc != nullptr, "open: %s", err);
    if (!pc) return;
    pm_pc_info i = Info(pc);
    std::printf("  hand-drawn, waits: %s\n", describe_(pc));
    CHECK(i.held_frames >= 1 && i.width == W && i.real_out == 1, "held-frame re-timing on (%d)", i.held_frames);
    const uint32_t handFlags = i.hold_flags;
    if (i.deband) CHECK(i.deband_strength == PUSHCORE_DEBAND_ANIME, "drawn: the anime deband preset (%u)", i.deband_strength);
    // drawn CG: no line art, no sign hold (PolicyFor(kDrawn, cg))
    pm_pc_cfg u = c; u.content_cg = 1;
    const int r = update_(pc, &u, err, sizeof(err));
    i = Info(pc);
    std::printf("  drawn CG -> %d: %s\n", r, describe_(pc));
    CHECK((i.hold_flags & (PUSHCORE_HOLD_LINE_ART | PUSHCORE_HOLD_SIGNS)) == 0, "drawn CG: no line art / sign hold (0x%X)", i.hold_flags);
    if (i.deband) CHECK(i.deband_strength != PUSHCORE_DEBAND_ANIME, "drawn CG: not the anime deband preset");
    CHECK(handFlags == i.hold_flags || r == 0, "a change in the holds was applied");
    // a caller that does not wait: no re-timing whatever Auto would do
    u = c; u.held_wait = 0;
    update_(pc, &u, err, sizeof(err));
    CHECK(Info(pc).held_frames == 0 && Info(pc).hold_flags == 0, "no held frames for a caller that cannot wait");
    close_(pc);
    // real frames the player's own: no deband, whatever is asked
    c = Base(2); c.deband = 1; c.content_class = 1;
    pc = open_(&c, &k, err, sizeof(err));
    CHECK(pc != nullptr, "open: %s", err);
    if (!pc) return;
    CHECK(Info(pc).deband == 0 && Info(pc).real_out == 0, "deband stays off while the real frames are the player's");
    close_(pc);
}

static void TestSrZeroCopy()
{
    std::printf("== 5. ABI 3, sr through the zero-copy path\n");
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    const D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl, 2, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
        CHECK(false, "no WARP device"); return;
    }
    HANDLE mutex = CreateMutexExW(nullptr, nullptr, 0, SYNCHRONIZE);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 2; td.Format = DXGI_FORMAT_NV12; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<uint8_t> f0(size_t(W) * H * 3 / 2, 128), f1(f0.size(), 128);
    memset(f0.data(), 60, size_t(W) * H); memset(f1.data(), 180, size_t(W) * H);
    D3D11_SUBRESOURCE_DATA init[2] = { { f0.data(), W, 0 }, { f1.data(), W, 0 } };
    ComPtr<ID3D11Texture2D> src;
    CHECK(SUCCEEDED(dev->CreateTexture2D(&td, init, &src)), "source");
    pm_pc_cfg c = Base(2);
    c.sr = 1; c.out_width = 3840; c.out_height = 2160;
    c.measure = 1;   // ContentDetect on the pushed textures, on the GPU (VLC's zero-copy route)
    int k = 0; char err[256] = "";
    pm_pc* pc = openD_(&c, dev.Get(), mutex, &k, err, sizeof(err));
    CHECK(pc != nullptr, "zero-copy sr open: %s", err);
    if (!pc) { CloseHandle(mutex); return; }
    const pm_pc_info i = Info(pc);
    std::printf("  %s\n", describe_(pc));
    CHECK(i.width == 3840 && i.height == 2160, "4K output (%ux%u)", i.width, i.height);
    struct D { std::vector<ComPtr<ID3D11Texture2D>> t; std::vector<int64_t> pts; pm_pc_tex x{}; int done = 0; ID3D11Device* dev; uint32_t w, h; } d;
    d.dev = dev.Get(); d.w = i.width; d.h = i.height;
    auto dest = [](void* u, int64_t pts) -> const pm_pc_tex* {
        auto* d = static_cast<D*>(u);
        D3D11_TEXTURE2D_DESC o = {};
        o.Width = d->w; o.Height = d->h; o.MipLevels = 1; o.ArraySize = 1; o.Format = DXGI_FORMAT_NV12; o.SampleDesc.Count = 1;
        o.Usage = D3D11_USAGE_DEFAULT; o.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> t;
        if (FAILED(d->dev->CreateTexture2D(&o, nullptr, &t))) return nullptr;
        d->t.push_back(t); d->pts.push_back(pts);
        d->x = { t.Get(), 0 };
        return &d->x;
    };
    auto done = [](void* u, int64_t) { static_cast<D*>(u)->done++; };
    CHECK(pushD_(pc, src.Get(), 0, 0, 1, err, sizeof(err)) == 0 && pushD_(pc, src.Get(), 1, 417083, 0, err, sizeof(err)) == 0, "push: %s", err);
    for (int n = 0; n < 40 && d.done < 3; ++n) if (pollD_(pc, 50, dest, done, &d, err, sizeof(err)) < 0) break;
    CHECK(d.done == 3, "real, generated, real into 4K textures (%d)", d.done);
    for (int j = 0; j < d.done; ++j) {
        D3D11_TEXTURE2D_DESC sd = {}; d.t[size_t(j)]->GetDesc(&sd);
        sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> st; dev->CreateTexture2D(&sd, nullptr, &st);
        ctx->CopyResource(st.Get(), d.t[size_t(j)].Get());
        D3D11_MAPPED_SUBRESOURCE m = {};
        double sum = 0;
        if (SUCCEEDED(ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) {
            for (uint32_t r = 0; r < d.h; r += 4) for (uint32_t x = 0; x < d.w; x += 4) sum += static_cast<const uint8_t*>(m.pData)[size_t(r) * m.RowPitch + x];
            ctx->Unmap(st.Get(), 0);
        }
        const double y = sum / (double(d.h / 4) * (d.w / 4)), want = j == 0 ? 60 : j == 1 ? 120 : 180;
        std::printf("  picture %d at %lld: luma %.1f (want %.0f)\n", j, (long long)d.pts[size_t(j)], y, want);
        CHECK(std::fabs(y - want) < 3, "picture %d luma %.1f", j, y);
    }
    // the GPU content pass keeps up with more frames (the class itself needs seconds of real pictures: not judged here)
    for (int n = 0; n < 30; ++n) {
        CHECK(pushD_(pc, src.Get(), unsigned(n & 1), 834166 + int64_t(n) * 417083, 0, err, sizeof(err)) == 0, "push %d: %s", n, err);
        d.done = 0; d.t.clear(); d.pts.clear();
        for (int m = 0; m < 10 && d.done < 2; ++m) if (pollD_(pc, 20, dest, done, &d, err, sizeof(err)) < 0) break;
    }
    const pm_pc_info im = Info(pc);
    std::printf("  measured on the GPU: class %d, cg %d\n", im.measured_class, im.measured_cg);
    CHECK(im.measured_class >= 0 && im.measured_class <= 4, "a measured class (%d)", im.measured_class);
    // an update keeps the open's measuring
    pm_pc_cfg u = c; u.content_class = im.measured_class ? im.measured_class : 3;
    CHECK(update_(pc, &u, err, sizeof(err)) >= 0, "update with the measured class: %s", err);
    close_(pc);
    CloseHandle(mutex);
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 4) { std::printf("usage: pm_pc_abi_test <pm_pushcore.dll> <stub core folder> <scratch folder>\n"); return 2; }
    g_stub = argv[2];
    g_log = std::wstring(argv[3]) + L"\\pm_pc_abi_stub.log";
    DeleteFileW(g_log.c_str());
    SetEnvironmentVariableW(L"PUSHCORE_STUB_LOG", g_log.c_str());
    SetEnvironmentVariableW(L"PUSHCORE_STUB_SR", L"1");
    HMODULE b = LoadLibraryW(argv[1]);
    if (!b) { std::printf("cannot load %ls\n", argv[1]); return 1; }
#define GET(v, t, n) v = reinterpret_cast<t>(GetProcAddress(b, n))
    GET(open_, pm_pc_open_fn, "pm_pc_open"); GET(push_, pm_pc_push_fn, "pm_pc_push"); GET(poll_, pm_pc_poll_fn, "pm_pc_poll");
    GET(close_, pm_pc_close_fn, "pm_pc_close"); GET(describe_, pm_pc_describe_fn, "pm_pc_describe"); GET(abi_, pm_pc_abi_fn, "pm_pc_abi");
    GET(update_, pm_pc_update_fn, "pm_pc_update"); GET(output_, pm_pc_output_fn, "pm_pc_output");
    GET(openD_, pm_pc_open_d3d11_fn, "pm_pc_open_d3d11"); GET(pushD_, pm_pc_push_d3d11_fn, "pm_pc_push_d3d11"); GET(pollD_, pm_pc_poll_d3d11_fn, "pm_pc_poll_d3d11");
    if (!open_ || !push_ || !poll_ || !close_ || !describe_ || !abi_ || !update_ || !output_ || !openD_ || !pushD_ || !pollD_) { std::printf("FAIL: exports missing\n"); return 1; }
    CHECK(abi_() == PM_PC_ABI, "pm_pc_abi %u", abi_());
    TestAbi2();
    TestSrPlanes();
    TestDrawn();
    TestSrZeroCopy();
    std::printf(fails ? "pm_pc_abi_test: %d of %d checks FAILED\n" : "pm_pc_abi_test: %d checks passed\n", fails ? fails : checks, checks);
    return fails ? 1 : 0;
}
