// pm_pc_zc_test: pm_pushcore.dll's zero-copy path (pm_pc_open_d3d11 / push_d3d11 / poll_d3d11 / copy_d3d11) on a WARP
// device with the stub pushcore (x2: the generated frame blends the two source frames). Two flat NV12 frames (luma 60
// and 180) go in as slices of a texture array, the way VLC's decoder hands them over; the generated frame must come back
// into the destination slice as NV12 with luma ~120 and neutral chroma, and the GPU copy must carry a frame over intact.
//   pm_pc_zc_test <pm_pushcore.dll> <folder with the stub pushcore.dll>
#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "pm_pushcore.h"

using Microsoft::WRL::ComPtr;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); fails++; } } while (0)

static const uint32_t W = 1280, H = 720;
struct Dest { ID3D11Texture2D* tex; int done; pm_pc_tex t; };
static const pm_pc_tex* DestFn(void* user, int64_t pts) { auto* d = static_cast<Dest*>(user); std::printf("generated frame at %lld\n", (long long)pts); d->t = { d->tex, 0 }; return &d->t; }
static void DoneFn(void* user, int64_t) { static_cast<Dest*>(user)->done++; }

static void Measure(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, unsigned slice, double& y, double& u, double& v)
{
    D3D11_TEXTURE2D_DESC d = {};
    tex->GetDesc(&d);
    d.ArraySize = 1; d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> st;
    dev->CreateTexture2D(&d, nullptr, &st);
    ctx->CopySubresourceRegion(st.Get(), 0, 0, 0, 0, tex, D3D11CalcSubresource(0, slice, 1), nullptr);
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

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) { std::printf("usage: pm_pc_zc_test <pm_pushcore.dll> <stub core folder>\n"); return 2; }
    HMODULE b = LoadLibraryW(argv[1]);
    if (!b) { std::printf("cannot load %ls\n", argv[1]); return 1; }
    auto open = (pm_pc_open_d3d11_fn)GetProcAddress(b, "pm_pc_open_d3d11");
    auto push = (pm_pc_push_d3d11_fn)GetProcAddress(b, "pm_pc_push_d3d11");
    auto poll = (pm_pc_poll_d3d11_fn)GetProcAddress(b, "pm_pc_poll_d3d11");
    auto copy = (pm_pc_copy_d3d11_fn)GetProcAddress(b, "pm_pc_copy_d3d11");
    auto close = (pm_pc_close_fn)GetProcAddress(b, "pm_pc_close");
    auto describe = (pm_pc_describe_fn)GetProcAddress(b, "pm_pc_describe");
    if (!open || !push || !poll || !copy || !close || !describe) { std::printf("FAIL: zero-copy exports missing\n"); return 1; }

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
    ComPtr<ID3D11Texture2D> src, dst;
    CHECK(SUCCEEDED(dev->CreateTexture2D(&td, init, &src)), "source texture array");
    CHECK(SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &dst)), "destination texture array");
    if (fails) return 1;

    pm_pc_cfg cfg = {};
    cfg.abi = PM_PC_ABI; cfg.width = W; cfg.height = H; cfg.bits = 8; cfg.fps_num = 24; cfg.fps_den = 1;
    cfg.matrix = 1; cfg.transfer = 1; cfg.primaries = 1; cfg.range = 1; cfg.smoother = 2; cfg.warp = 1; cfg.core_dir = argv[2];
    int k = 0; char err[512] = "";
    pm_pc* pc = open(&cfg, dev.Get(), mutex, &k, err, sizeof(err));
    if (!pc) { std::printf("FAIL: pm_pc_open_d3d11: %s\n", err); return 1; }
    std::printf("open: x%d, %s\n", k, describe(pc));
    CHECK(k == 2, "multiplier %d (want 2)", k);
    CHECK(push(pc, src.Get(), 0, 0, 1, err, sizeof(err)) == 0, "push slice 0: %s", err);
    CHECK(push(pc, src.Get(), 1, 417083, 0, err, sizeof(err)) == 0, "push slice 1: %s", err);
    Dest d = { dst.Get(), 0, {} };
    int polled = 0;
    for (int i = 0; i < 20 && d.done == 0; ++i) {
        const int n = poll(pc, 100, DestFn, DoneFn, &d, err, sizeof(err));
        CHECK(n >= 0, "poll: %s", err);
        if (n < 0) break;
        polled += n;
    }
    std::printf("polled %d frames, %d generated into the destination\n", polled, d.done);
    CHECK(d.done >= 1, "no generated frame reached the destination");
    double y, u, v;
    Measure(dev.Get(), ctx.Get(), dst.Get(), 0, y, u, v);
    std::printf("generated frame: Y %.2f, U %.2f, V %.2f (want ~120, 128, 128)\n", y, u, v);
    CHECK(y > 112 && y < 128, "generated luma %.2f", y);
    CHECK(u > 124 && u < 132 && v > 124 && v < 132, "generated chroma %.2f %.2f", u, v);
    CHECK(copy(pc, src.Get(), 1, dst.Get(), 1) == 0, "copy_d3d11");
    Measure(dev.Get(), ctx.Get(), dst.Get(), 1, y, u, v);
    std::printf("copied frame: Y %.2f, U %.2f, V %.2f (want 180, 128, 128)\n", y, u, v);
    CHECK(y > 179.5 && y < 180.5 && u > 127.5 && u < 128.5, "copied frame %.2f %.2f", y, u);
    close(pc);
    CloseHandle(mutex);
    std::printf(fails ? "FAILED (%d)\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
