// engine_test: the engine module through its C table (as vf_pushmpc drives it), no FFmpeg.
//   CPU, every OS: a moving box at 24 fps, smoother=4 -> 1 + 47 x 4 outputs in pushcore order (generated 1..3, then
//   the real frame); real frames byte-identical; the box of a generated frame sits between its neighbours; a hard cut
//   makes no blend (every generated frame equals a real one); a discontinuity makes no pair; NV12 / P010 / 10-bit
//   planar round trips.
//   D3D11, Windows: WARP device, NV12 textures, the stub pushcore (CPU blend) -> BGRA textures with the right pixels,
//   x2 order, pending() drains to 0.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "pmeng.h"
#ifdef _WIN32
#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#endif

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

struct Pic {
    int w, h; uint32_t fmt; int bps;
    std::vector<uint8_t> p[3]; ptrdiff_t ls[3];
    Pic(int W, int H, uint32_t f) : w(W), h(H), fmt(f)
    {
        bps = (f == PMENG_FMT_YUV420P10 || f == PMENG_FMT_P010) ? 2 : 1;
        const bool semi = f == PMENG_FMT_NV12 || f == PMENG_FMT_P010;
        ls[0] = ptrdiff_t(w) * bps; p[0].assign(size_t(ls[0]) * h, 0);
        if (semi) { ls[1] = ptrdiff_t(w) * bps; p[1].assign(size_t(ls[1]) * (h / 2), 0); ls[2] = 0; }
        else { ls[1] = ls[2] = ptrdiff_t(w / 2) * bps; p[1].assign(size_t(ls[1]) * (h / 2), 0); p[2].assign(size_t(ls[2]) * (h / 2), 0); }
    }
    // luma value at (x, y) in 8-bit terms
    int Y(int x, int y) const
    {
        if (bps == 1) return p[0][size_t(y) * ls[0] + x];
        const uint16_t v = reinterpret_cast<const uint16_t*>(&p[0][size_t(y) * ls[0]])[x];
        return fmt == PMENG_FMT_P010 ? (v >> 8) : (v >> 2);
    }
    void SetY(int x, int y, int v8)
    {
        if (bps == 1) { p[0][size_t(y) * ls[0] + x] = uint8_t(v8); return; }
        uint16_t* r = reinterpret_cast<uint16_t*>(&p[0][size_t(y) * ls[0]]);
        r[x] = fmt == PMENG_FMT_P010 ? uint16_t((v8 << 2) << 6) : uint16_t(v8 << 2);
    }
    void FillChroma(int v8)
    {
        for (int i = 1; i < 3; ++i) {
            if (p[i].empty()) continue;
            if (bps == 1) std::memset(p[i].data(), v8, p[i].size());
            else { auto* q = reinterpret_cast<uint16_t*>(p[i].data()); for (size_t k = 0; k < p[i].size() / 2; ++k) q[k] = fmt == PMENG_FMT_P010 ? uint16_t((v8 << 2) << 6) : uint16_t(v8 << 2); }
        }
    }
    pmeng_image Img()
    {
        pmeng_image im{}; im.mem = PMENG_MEM_CPU; im.fmt = fmt; im.width = w; im.height = h;
        for (int i = 0; i < 3; ++i) { im.data[i] = p[i].empty() ? nullptr : p[i].data(); im.linesize[i] = ls[i]; }
        im.range = 1; im.matrix = 1; im.primaries = 1; im.transfer = 1;
        return im;
    }
};

// background 40, a 32x32 box of 200 at x = bx
static void Box(Pic& pic, int bx, int bg = 40)
{
    for (int y = 0; y < pic.h; ++y) for (int x = 0; x < pic.w; ++x) pic.SetY(x, y, (x >= bx && x < bx + 32 && y >= 64 && y < 96) ? 200 : bg);
    pic.FillChroma(128);
}
// x of the box's left edge on row 80 (first pixel > 120), -1 when none
static int BoxX(const Pic& p) { for (int x = 0; x < p.w; ++x) if (p.Y(x, 80) > 120) return x; return -1; }

static void TestCpu(const pmeng_api* api, uint32_t fmt, const char* name)
{
    const int W = 320, H = 180;
    pmeng_device dev{}; dev.struct_size = sizeof(dev); dev.mem = PMENG_MEM_CPU;
    pmeng_stream st{}; st.struct_size = sizeof(st); st.width = W; st.height = H; st.fmt = fmt; st.fps_num = 24000; st.fps_den = 1001;
    pmeng_output out{}; out.struct_size = sizeof(out);
    char err[512] = {};
    void* h = api->open(&dev, &st, "smoother=4\nportable_threads=2\n", &out, err, sizeof(err));
    CHECK(h, "%s: open: %s", name, err);
    if (!h) return;
    std::printf("%s: %s | %s\n", name, out.engine, out.description);
    CHECK(out.multiplier == 4 && out.mem == PMENG_MEM_CPU && out.fmt == fmt && out.width == uint32_t(W), "%s: output x%u mem %u fmt %u", name, out.multiplier, out.mem, out.fmt);

    int outputs = 0, gens = 0, orderErr = 0, cutBlend = 0, motionErr = 0;
    int prevBox = -1;
    const int N = 48, cutAt = 30, discAt = 40;
    for (int s = 0; s < N; ++s) {
        Pic src(W, H, fmt);
        const bool cut = s >= cutAt && s < discAt;
        if (cut) { Box(src, 4 * s, 230); } else Box(src, 4 * s);   // after the cut: another scene (bright background)
        pmeng_image im = src.Img();
        const uint32_t flags = s == discAt ? PMENG_SUBMIT_DISCONTINUITY : 0u;
        int r = api->submit(h, &im, int64_t(s) * 41708, flags);
        CHECK(r == PMENG_OK, "%s: submit %d -> %d %s", name, s, r, api->last_error(h));
        pmeng_out_info oi{};
        uint32_t expectSub = (s == 0 || s == discAt) ? 0 : 1;
        int groupGen = 0;
        while (api->peek(h, &oi, 0) == 1) {
            Pic dst(W, H, fmt);
            pmeng_image di = dst.Img();
            CHECK(api->write(h, &di) == PMENG_OK, "%s: write", name);
            ++outputs;
            if (oi.source != uint64_t(s) || oi.subframe != (expectSub % 4)) ++orderErr;
            expectSub = oi.subframe == 0 ? 99 : oi.subframe + 1;
            if (oi.subframe == 0) {
                // real frame: identical to the source
                bool same = true;
                for (int i = 0; i < 3; ++i) if (src.p[i] != dst.p[i]) same = false;
                CHECK(same, "%s: real frame %d differs from its source", name, s);
                prevBox = BoxX(dst);
            } else {
                ++gens; ++groupGen;
                const int bx = BoxX(dst);
                if (s == cutAt) {
                    // a cut: the nearer real frame, never a blend -> background is exactly 40 or 230
                    const int bgv = dst.Y(2, 2);
                    if (bgv != 40 && bgv != 230) ++cutBlend;
                } else if (s < cutAt) {
                    const int want = 4 * (s - 1) + int(oi.subframe);   // linear motion, 4 px per frame
                    if (bx < 0 || std::abs(bx - want) > 2) { if (motionErr++ < 6) std::printf("%s: pair %d phase %u: box at %d, expected %d\n", name, s, oi.subframe, bx, want); }
                }
            }
        }
        if (s == discAt) CHECK(groupGen == 0, "%s: discontinuity still made %d generated frames", name, groupGen);
    }
    (void)prevBox;
    CHECK(api->pending(h) == 0, "%s: pending %u", name, api->pending(h));
    const int expect = 1 + (N - 1) * 4 - 3;   // minus the discontinuity's group
    CHECK(outputs == expect, "%s: %d outputs, expected %d", name, outputs, expect);
    CHECK(orderErr == 0, "%s: %d outputs out of order", name, orderErr);
    CHECK(cutBlend == 0, "%s: %d generated frames blended across the cut", name, cutBlend);
    CHECK(motionErr == 0, "%s: %d generated frames with the box in the wrong place", name, motionErr);
    char sbuf[512]; api->stats(h, sbuf, sizeof(sbuf));
    std::printf("%s: %d outputs, %d generated; %s\n", name, outputs, gens, sbuf);
    api->close(h);
}

#ifdef _WIN32
static void TestD3D11(const pmeng_api* api)
{
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1 };
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    CHECK(SUCCEEDED(hr), "WARP device: 0x%08lx", hr);
    if (FAILED(hr)) return;
    ComPtr<ID3D10Multithread> mt; if (SUCCEEDED(ctx.As(&mt))) mt->SetMultithreadProtected(TRUE);
    const UINT W = 256, H = 144;
    pmeng_device pd{}; pd.struct_size = sizeof(pd); pd.mem = PMENG_MEM_D3D11; pd.d3d11_device = dev.Get();
    pmeng_stream st{}; st.struct_size = sizeof(st); st.width = W; st.height = H; st.fmt = PMENG_FMT_NV12; st.fps_num = 24; st.fps_den = 1;
    st.range = 1; st.matrix = 1; st.primaries = 1; st.transfer = 1;
    pmeng_output out{}; out.struct_size = sizeof(out);
    char err[512] = {};
    const std::string opts = std::string("core_dir=") + PME_STUB_DIR + "\nsmoother=2\nsharper=off\ndisplay_fps=48\n";
    void* h = api->open(&pd, &st, opts.c_str(), &out, err, sizeof(err));
    CHECK(h, "d3d11: open: %s", err);
    if (!h) return;
    std::printf("d3d11: %s | %s | x%u %ux%u fmt %u\n", out.engine, out.description, out.multiplier, out.width, out.height, out.fmt);
    CHECK(out.mem == PMENG_MEM_D3D11 && out.fmt == PMENG_FMT_BGRA, "d3d11: output mem %u fmt %u", out.mem, out.fmt);
    const uint32_t k = out.multiplier;

    D3D11_TEXTURE2D_DESC td = {}; td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 4; td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = 0;   // as FFmpeg's hwupload pool (WARP refuses NV12 arrays with shader binding)
    ComPtr<ID3D11Texture2D> in;
    hr = dev->CreateTexture2D(&td, nullptr, &in);
    CHECK(SUCCEEDED(hr), "NV12 array texture: 0x%08lx", hr);
    if (FAILED(hr)) { api->close(h); return; }
    D3D11_TEXTURE2D_DESC od = {}; od.Width = out.width; od.Height = out.height; od.MipLevels = 1; od.ArraySize = 1; od.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    od.SampleDesc.Count = 1; od.Usage = D3D11_USAGE_DEFAULT; od.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    D3D11_TEXTURE2D_DESC sd = od; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stage; dev->CreateTexture2D(&sd, nullptr, &stage);

    std::vector<uint8_t> nv12(size_t(W) * H * 3 / 2);
    int outputs = 0, reals = 0, wrong = 0;
    const int N = 12;
    auto take = [&](uint32_t timeout) {
        pmeng_out_info oi{};
        while (api->peek(h, &oi, timeout) == 1) {
            ComPtr<ID3D11Texture2D> o; dev->CreateTexture2D(&od, nullptr, &o);
            pmeng_image di{}; di.mem = PMENG_MEM_D3D11; di.fmt = PMENG_FMT_BGRA; di.width = out.width; di.height = out.height; di.d3d11_texture = o.Get();
            CHECK(api->write(h, &di) == PMENG_OK, "d3d11: write: %s", api->last_error(h));
            ++outputs; if (oi.subframe == 0) ++reals;
            // the real frame s has luma 16 + 16 s (limited range grey): its RGB is close to (Y - 16) * 255 / 219
            if (oi.subframe == 0) {
                ctx->CopyResource(stage.Get(), o.Get());
                D3D11_MAPPED_SUBRESOURCE m{};
                if (SUCCEEDED(ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                    const uint8_t* px = static_cast<const uint8_t*>(m.pData) + size_t(H / 2) * m.RowPitch + (W / 2) * 4;
                    const int want = int(std::lround(16.0 * oi.source * 255.0 / 219.0));
                    if (std::abs(int(px[1]) - want) > 6) { ++wrong; std::printf("d3d11: real %llu: G %d, expected ~%d\n", (unsigned long long)oi.source, px[1], want); }
                    ctx->Unmap(stage.Get(), 0);
                }
            }
            timeout = 0;
        }
    };
    for (int s = 0; s < N; ++s) {
        std::memset(nv12.data(), 16 + 16 * s, size_t(W) * H);
        std::memset(nv12.data() + size_t(W) * H, 128, size_t(W) * H / 2);
        const UINT slice = UINT(s % 4);
        ctx->UpdateSubresource(in.Get(), D3D11CalcSubresource(0, slice, 1), nullptr, nv12.data(), W, W * H);
        pmeng_image im{}; im.mem = PMENG_MEM_D3D11; im.fmt = PMENG_FMT_NV12; im.width = W; im.height = H; im.d3d11_texture = in.Get(); im.d3d11_slice = slice;
        im.range = 1; im.matrix = 1; im.primaries = 1; im.transfer = 1;
        int r, tries = 0;
        while ((r = api->submit(h, &im, int64_t(s) * 41667, 0)) == PMENG_BUSY && tries++ < 500) take(20);
        CHECK(r == PMENG_OK, "d3d11: submit %d -> %d %s", s, r, api->last_error(h));
        take(0);
    }
    for (int i = 0; i < 200 && api->pending(h) > 0; ++i) take(20);
    CHECK(api->pending(h) == 0, "d3d11: pending %u", api->pending(h));
    CHECK(reals == N, "d3d11: %d real frames, expected %d", reals, N);
    CHECK(outputs == N + (N - 1) * int(k - 1), "d3d11: %d outputs, expected %d (x%u)", outputs, N + (N - 1) * int(k - 1), k);
    CHECK(wrong == 0, "d3d11: %d real frames with wrong pixels", wrong);
    char sbuf[512]; api->stats(h, sbuf, sizeof(sbuf));
    std::printf("d3d11: %d outputs (%d real); %s\n", outputs, reals, sbuf);
    api->close(h);
}
#endif

int main()
{
    const pmeng_api* api = pmeng_get_api(PMENG_ABI_VERSION);
    CHECK(api && api->abi_version == PMENG_ABI_VERSION, "pmeng_get_api");
    if (!api) return 1;
    CHECK(pmeng_get_api(PMENG_ABI_VERSION + 1) == nullptr, "a wrong ABI must be refused");
    std::printf("%s\n", api->version());
    TestCpu(api, PMENG_FMT_YUV420P, "yuv420p");
    TestCpu(api, PMENG_FMT_YUV420P10, "yuv420p10");
    TestCpu(api, PMENG_FMT_NV12, "nv12");
    TestCpu(api, PMENG_FMT_P010, "p010");
    {   // a Vulkan open must fail with a reason (the filter then takes the copy fallback)
        pmeng_device dev{}; dev.struct_size = sizeof(dev); dev.mem = PMENG_MEM_VULKAN;
        pmeng_stream st{}; st.struct_size = sizeof(st); st.width = 64; st.height = 64; st.fmt = PMENG_FMT_NV12;
        pmeng_output out{}; char err[256] = {};
        CHECK(api->open(&dev, &st, "", &out, err, sizeof(err)) == nullptr && err[0], "vulkan open should fail with a reason");
        std::printf("vulkan: %s\n", err);
    }
#ifdef _WIN32
    TestD3D11(api);
#endif
    std::printf(g_fail ? "engine_test: %d FAILED\n" : "engine_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
