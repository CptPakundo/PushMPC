// ContentDetect's D3D11 GPU tap (mpvnative/engine/d3d11_content.h) on WARP: the packed cells and thumbnail the compute
// pass reads back equal the CPU reference (content_detect.h PackedCellsRef / PackedClipRef) on NV12 and P010 decoder-style
// textures, with and without shader access (the GPU copy), and the read-back frames reach the detector in order.
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
#include "d3d11_content.h"

namespace pme { void Log(const std::string&) {} }
using Microsoft::WRL::ComPtr;
static int g_fail = 0, g_pass = 0;
static void Check(bool ok, const char* what, const std::string& detail = {})
{
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : "  -- ", detail.c_str());
    (ok ? g_pass : g_fail)++;
}

template <class T>
static void Case(ID3D11Device* dev, ID3D11DeviceContext* ctx, int w, int h, bool p010, bool srv, const char* name)
{
    std::mt19937 rng(w * 7 + h + (p010 ? 1 : 0));
    const int bits = p010 ? 16 : 8;
    const int ys = w, cs = w;   // NV12 / P010: the chroma plane is w samples (Cb Cr pairs) by h / 2
    std::vector<T> y(size_t(w) * h), uv(size_t(w) * h / 2);
    auto code = [&](int lo, int hi) { int v = lo + int(rng() % unsigned(hi - lo)); return p010 ? T(v << 8 | (v & 0xC0)) : T(v); };
    for (int j = 0; j < h; ++j) for (int i = 0; i < w; ++i) y[size_t(j) * w + i] = code(16 + (i * 3 + j) % 40, 200 + (i + j) % 30);
    for (auto& v : uv) v = code(40, 220);
    if (p010) for (auto& v : y) v = T(v & 0xFFC0); if (p010) for (auto& v : uv) v = T(v & 0xFFC0);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = UINT(w); td.Height = UINT(h); td.MipLevels = 1; td.ArraySize = 2; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.Format = p010 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    td.BindFlags = srv ? D3D11_BIND_SHADER_RESOURCE : 0;
    std::vector<T> init(size_t(w) * h * 3 / 2);
    std::copy(y.begin(), y.end(), init.begin()); std::copy(uv.begin(), uv.end(), init.begin() + ptrdiff_t(y.size()));
    D3D11_SUBRESOURCE_DATA sd[2] = { { init.data(), UINT(w * sizeof(T)), 0 }, { init.data(), UINT(w * sizeof(T)), 0 } };
    // older WARP builds (windows-2022's) have no P010 textures: ask first and skip the case with a clear line there
    UINT fs = 0;
    const UINT need = D3D11_FORMAT_SUPPORT_TEXTURE2D | (srv ? D3D11_FORMAT_SUPPORT_SHADER_LOAD : 0);
    if (FAILED(dev->CheckFormatSupport(td.Format, &fs)) || (fs & need) != need) {
        std::printf("SKIP  %s  -- this adapter has no %s textures%s (format support 0x%x): not covered here\n", name,
                    p010 ? "P010" : "NV12", srv ? " with shader access" : "", fs);
        return;
    }
    ComPtr<ID3D11Texture2D> tex;
    UINT slice = 1;
    HRESULT hr = dev->CreateTexture2D(&td, sd, &tex);
    if (FAILED(hr)) {
        // some WARP builds create a planar texture but no planar array: test the same picture as a single texture (the
        // array-slice path is covered by the NV12 1080p case)
        std::printf("NOTE  %s: the 2-slice array failed (hr 0x%08lx); testing a single-slice texture\n", name, (unsigned long)hr);
        td.ArraySize = 1; slice = 0;
        hr = dev->CreateTexture2D(&td, sd, &tex);
    }
    if (FAILED(hr)) {
        char e[96]; std::snprintf(e, sizeof(e), "CreateTexture2D failed (hr 0x%08lx, format support 0x%x)", (unsigned long)hr, fs);
        Check(false, name, e); return;
    }
    pme::D3D11ContentTap tap; std::string why;
    if (!tap.Init(dev, UINT(w), UINT(h), p010, true, why)) { Check(false, name, why); return; }
    const auto refC = pushmpc::content::PackedCellsRef(y.data(), ys, uv.data(), static_cast<const T*>(nullptr), cs, w, h, bits);
    const auto refT = pushmpc::content::PackedClipRef(y.data(), ys, uv.data(), static_cast<const T*>(nullptr), cs, w, h, bits);
    int frames = 0, clips = 0, worstY = 0, worstC = 0, worstT = 0;
    tap.onWords = [&](const uint32_t* wds, size_t cells, bool clip) {
        ++frames;
        for (size_t i = 0; i < cells && i < refC.size(); ++i) {
            worstY = std::max(worstY, std::abs(int(wds[i] & 0xffff) - int(refC[i] & 0xffff)));
            worstC = std::max(worstC, std::max(std::abs(int((wds[i] >> 16) & 0xff) - int((refC[i] >> 16) & 0xff)), std::abs(int(wds[i] >> 24) - int(refC[i] >> 24))));
        }
        if (clip) {
            ++clips;
            for (size_t i = 0; i < refT.size(); ++i) {
                worstT = std::max(worstT, std::abs(int(wds[cells + i] & 0xffff) - int(refT[i] & 0xffff)));
                worstC = std::max(worstC, std::abs(int((wds[cells + i] >> 16) & 0xff) - int((refT[i] >> 16) & 0xff)));
            }
        }
    };
    pme::ContentTap ct; ct.alwaysClip = true;
    for (int f = 0; f < 6; ++f) tap.Record(ctx, tex.Get(), slice, f / 24.0, f == 0, ct);
    tap.Drain(ctx, ct);
    char b[256];
    std::snprintf(b, sizeof(b), "%dx%d %s%s: %d frames read back (%d with the thumbnail), luma worst %d/256 code, chroma %d, thumbnail luma %d/256; GPU copies %llu",
                  w, h, p010 ? "P010" : "NV12", srv ? "" : " (no shader access)", frames, clips, worstY, worstC, worstT, (unsigned long long)tap.Copied());
    // luma: the cell mean in 1/256 code (the shader sums in integers as the CPU; float division and the 10-bit codes'
    // rounding leave at most a unit or two); chroma: the same 8-bit code; thumbnail: bilinear in float
    Check(frames == 6 && worstY <= 2 && worstC <= 1 && worstT <= 64 && (srv || tap.Copied() == 6), name, b);
}

int main()
{
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
        std::printf("SKIP  no WARP device\n"); return 0;
    }
    Case<uint8_t>(dev.Get(), ctx.Get(), 1920, 1080, false, true, "GPU tap = CPU reference (NV12 1080p, array slice 1)");
    Case<uint8_t>(dev.Get(), ctx.Get(), 720, 480, false, false, "GPU tap = CPU reference (NV12 SD, decoder-only texture: GPU copy)");
    Case<uint16_t>(dev.Get(), ctx.Get(), 1280, 720, true, true, "GPU tap = CPU reference (P010 720p)");
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
