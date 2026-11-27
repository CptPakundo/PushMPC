// pm_pc_zc_test: pm_pushcore.dll's zero-copy path (pm_pc_open_d3d11 / push_d3d11 / poll_d3d11 / copy_d3d11) on a WARP
// device with the stub pushcore (x k: generated frame j blends the two source frames at j/k). Two flat NV12 frames (luma
// 60 and 180) go in as slices of a texture array, the way VLC's decoder hands them over.
//   x2: the generated frame comes back into a slice of a texture array (VLC's pool) with luma ~120, neutral chroma;
//   x8: the 7 generated frames come back into single textures made like the VLC module's own ring (one texture each,
//       with the luma / chroma shader views VLC's output draws from), with luma 60 + 120 j/8, in order;
//   the GPU copy (the fallback for a late frame) carries a frame over intact.
//   pm_pc_zc_test <pm_pushcore.dll> <folder with the stub pushcore.dll>
#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "pm_pushcore.h"

using Microsoft::WRL::ComPtr;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); fails++; } } while (0)

static const uint32_t W = 1280, H = 720;
static pm_pc_open_d3d11_fn open_; static pm_pc_push_d3d11_fn push_; static pm_pc_poll_d3d11_fn poll_;
static pm_pc_copy_d3d11_fn copy_; static pm_pc_close_fn close_; static pm_pc_describe_fn describe_;

struct Dest { std::vector<ID3D11Texture2D*> tex; std::vector<unsigned> slice; std::vector<int64_t> pts; int next = 0, done = 0; pm_pc_tex t{}; };
static const pm_pc_tex* DestFn(void* user, int64_t pts)
{
    auto* d = static_cast<Dest*>(user);
    if (d->next >= int(d->tex.size())) return nullptr;
    d->t = { d->tex[d->next], d->slice[d->next] };
    d->pts.push_back(pts);
    d->next++;
    return &d->t;
}
static void DoneFn(void* user, int64_t) { static_cast<Dest*>(user)->done++; }

static void Measure(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, unsigned slice, double& y, double& u, double& v)
{
    D3D11_TEXTURE2D_DESC d = {};
    tex->GetDesc(&d);
    const UINT mips = d.MipLevels;
    d.ArraySize = 1; d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> st;
    dev->CreateTexture2D(&d, nullptr, &st);
    ctx->CopySubresourceRegion(st.Get(), 0, 0, 0, 0, tex, D3D11CalcSubresource(0, slice, mips), nullptr);
    D3D11_MAPPED_SUBRESOURCE m = {};
    y = u = v = -1;
    if (FAILED(ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
    const uint8_t* p = static_cast<const uint8_t*>(m.pData);
    double sy = 0, su = 0, sv = 0;
    for (uint32_t r = 0; r < H; ++r) for (uint32_t c = 0; c < W; ++c) sy += p[size_t(r) * m.RowPitch + c];
    const uint8_t* uv = p + size_t(m.RowPitch) * d.Height;
    for (uint32_t r = 0; r < H / 2; ++r) for (uint32_t c = 0; c < W / 2; ++c) { su += uv[size_t(r) * m.RowPitch + 2 * c]; sv += uv[size_t(r) * m.RowPitch + 2 * c + 1]; }
    ctx->Unmap(st.Get(), 0);
    y = sy / (double(W) * H); u = su / (double(W / 2) * (H / 2)); v = sv / (double(W / 2) * (H / 2));
}

static void Run(ID3D11Device* dev, ID3D11DeviceContext* ctx, HANDLE mutex, ID3D11Texture2D* src, const wchar_t* stub, int want, bool ring)
{
    std::printf("== x%d into %s\n", want, ring ? "single textures (the module's ring)" : "a texture array (VLC's pool)");
    pm_pc_cfg cfg = {};
    cfg.abi = PM_PC_ABI; cfg.struct_size = sizeof(cfg); cfg.anime_model = PM_PC_ANIME_NONE;   // ABI 3, the source size (as ABI 2)
    cfg.width = W; cfg.height = H; cfg.bits = 8; cfg.fps_num = 24; cfg.fps_den = 1;
    cfg.matrix = 1; cfg.transfer = 1; cfg.primaries = 1; cfg.range = 1; cfg.smoother = want; cfg.warp = 1; cfg.core_dir = stub;
    cfg.display_hz = 240;   // a high-refresh display: x8 of 24 fps fits
    int k = 0; char err[512] = "";
    pm_pc* pc = open_(&cfg, dev, mutex, &k, err, sizeof(err));
    if (!pc) { std::printf("FAIL: pm_pc_open_d3d11: %s\n", err); fails++; return; }
    std::printf("open: x%d, %s\n", k, describe_(pc));
    CHECK(k == want, "multiplier %d (want %d)", k, want);
    std::vector<ComPtr<ID3D11Texture2D>> keep;
    std::vector<ComPtr<ID3D11ShaderResourceView>> views;
    Dest d;
    D3D11_TEXTURE2D_DESC td = {};
    src->GetDesc(&td);
    if (!ring) {
        ComPtr<ID3D11Texture2D> arr;
        CHECK(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &arr)), "destination array");
        keep.push_back(arr); d.tex.push_back(arr.Get()); d.slice.push_back(0);
    } else {
        td.ArraySize = 1;   // as the VLC module's RingNew
        for (int j = 1; j < k; ++j) {
            ComPtr<ID3D11Texture2D> t;
            CHECK(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &t)), "ring texture %d", j);
            for (DXGI_FORMAT vf : { DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8_UNORM }) {
                D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
                sd.Format = vf; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
                ComPtr<ID3D11ShaderResourceView> s;
                CHECK(SUCCEEDED(dev->CreateShaderResourceView(t.Get(), &sd, &s)), "ring texture view (format %d)", int(vf));
                views.push_back(s);
            }
            keep.push_back(t); d.tex.push_back(t.Get()); d.slice.push_back(0);
        }
    }
    CHECK(push_(pc, src, 0, 0, 1, err, sizeof(err)) == 0, "push slice 0: %s", err);
    CHECK(push_(pc, src, 1, 417083, 0, err, sizeof(err)) == 0, "push slice 1: %s", err);
    int polled = 0;
    for (int i = 0; i < 40 && d.done < int(d.tex.size()); ++i) {
        const int n = poll_(pc, 100, DestFn, DoneFn, &d, err, sizeof(err));
        CHECK(n >= 0, "poll: %s", err);
        if (n < 0) break;
        polled += n;
    }
    std::printf("polled %d frames, %d generated into the destinations\n", polled, d.done);
    CHECK(d.done == int(d.tex.size()), "%d of %d generated frames arrived", d.done, int(d.tex.size()));
    double prevY = 0;
    for (int j = 0; j < d.done; ++j) {
        double y, u, v;
        Measure(dev, ctx, d.tex[j], d.slice[j], y, u, v);
        const double wantY = 60.0 + 120.0 * double(j + 1) / double(k);
        std::printf("generated %d/%d at %lld: Y %.2f (want %.2f), U %.2f, V %.2f\n", j + 1, k, (long long)d.pts[j], y, wantY, u, v);
        CHECK(std::fabs(y - wantY) < 3.0, "generated frame %d: luma %.2f, want %.2f", j + 1, y, wantY);
        CHECK(u > 126 && u < 130 && v > 126 && v < 130, "generated frame %d: chroma %.2f %.2f", j + 1, u, v);
        CHECK(y > prevY, "generated frames out of order");
        prevY = y;
    }
    if (!ring) {
        CHECK(copy_(pc, src, 1, d.tex[0], 0) == 0, "copy_d3d11");
        double y, u, v;
        Measure(dev, ctx, d.tex[0], 0, y, u, v);
        std::printf("copied frame: Y %.2f, U %.2f, V %.2f (want 180, 128, 128)\n", y, u, v);
        CHECK(y > 179.5 && y < 180.5 && u > 127.5 && u < 128.5, "copied frame %.2f %.2f", y, u);
    }
    close_(pc);
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) { std::printf("usage: pm_pc_zc_test <pm_pushcore.dll> <stub core folder>\n"); return 2; }
    HMODULE b = LoadLibraryW(argv[1]);
    if (!b) { std::printf("cannot load %ls\n", argv[1]); return 1; }
    open_ = (pm_pc_open_d3d11_fn)GetProcAddress(b, "pm_pc_open_d3d11");
    push_ = (pm_pc_push_d3d11_fn)GetProcAddress(b, "pm_pc_push_d3d11");
    poll_ = (pm_pc_poll_d3d11_fn)GetProcAddress(b, "pm_pc_poll_d3d11");
    copy_ = (pm_pc_copy_d3d11_fn)GetProcAddress(b, "pm_pc_copy_d3d11");
    close_ = (pm_pc_close_fn)GetProcAddress(b, "pm_pc_close");
    describe_ = (pm_pc_describe_fn)GetProcAddress(b, "pm_pc_describe");
    if (!open_ || !push_ || !poll_ || !copy_ || !close_ || !describe_) { std::printf("FAIL: zero-copy exports missing\n"); return 1; }

    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    const D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl, 2, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
        std::printf("no WARP device\n"); return 1;
    }
    HANDLE mutex = CreateMutexExW(nullptr, nullptr, 0, SYNCHRONIZE);   // stands in for VLC's context lock

    // the "decoder" texture array: slice 0 luma 60, slice 1 luma 180, chroma 128
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 2; td.Format = DXGI_FORMAT_NV12; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<uint8_t> f0(size_t(W) * H * 3 / 2, 128), f1(f0.size(), 128);
    memset(f0.data(), 60, size_t(W) * H); memset(f1.data(), 180, size_t(W) * H);
    D3D11_SUBRESOURCE_DATA init[2] = { { f0.data(), W, 0 }, { f1.data(), W, 0 } };
    ComPtr<ID3D11Texture2D> src;
    CHECK(SUCCEEDED(dev->CreateTexture2D(&td, init, &src)), "source texture array");
    if (fails) return 1;

    Run(dev.Get(), ctx.Get(), mutex, src.Get(), argv[2], 2, false);
    Run(dev.Get(), ctx.Get(), mutex, src.Get(), argv[2], 8, true);
    CloseHandle(mutex);
    std::printf(fails ? "FAILED (%d)\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
