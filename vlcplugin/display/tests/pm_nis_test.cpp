// SPDX-License-Identifier: GPL-3.0-or-later
// pm_nis_test: the Direct3D 11 output fork's NIS upscaler on WARP, without VLC. An NV12 test picture (sharp squares, a
// thin diagonal, a gradient, a red block) is scaled 4x (bilinear pre-scale + NIS 2x) and 1.5x (NIS only) into a
// window-size texture with a letterbox placement, read back and checked against a CPU bilinear reference: same mean
// level, the red block stays red, the bars stay black, and NIS's edges are sharper than bilinear. A P010 picture into an
// RGB10A2 output keeps 10-bit steps.
#include <windows.h>
#include <d3d11.h>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <vector>

#include <vlc_common.h>
#include "../vlc3/modules/video_output/win32/pm_nis_d3d11.h"

// the module's msg_* go to vlc_Log; print them here
const char vlc_module_name[] = "pm_nis_test";   // declared extern by vlc_messages.h (C++ linkage)
extern "C" void vlc_Log(vlc_object_t *, int prio, const char *, const char *, unsigned, const char *, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    std::printf("[%d] ", prio); std::vprintf(fmt, ap); std::printf("\n");
    va_end(ap);
}

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); fails++; } } while (0)

struct Img { int w, h; std::vector<uint8_t> y, uv; };
static Img Pattern(int w, int h)
{
    Img im{ w, h, std::vector<uint8_t>(size_t(w) * h), std::vector<uint8_t>(size_t(w) * h / 2, 128) };
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++) {
            int v = ((c / 8 + r / 8) & 1) ? 200 : 60;                 // squares
            if (r > h / 2) v = 16 + (235 - 16) * c / (w - 1);           // gradient in the lower half
            if (std::abs(c - r * w / h) < 1) v = 235;                   // a thin diagonal
            im.y[size_t(r) * w + c] = uint8_t(v);
        }
    for (int r = h / 8; r < h / 4; r++)                                 // a red block (BT.709, limited)
        for (int c = w / 8; c < w / 4; c++) im.y[size_t(r) * w + c] = 63;
    for (int r = h / 16; r < h / 8; r++)
        for (int c = w / 16; c < w / 8; c++) { im.uv[size_t(r) * w + 2 * c] = 102; im.uv[size_t(r) * w + 2 * c + 1] = 240; }
    return im;
}

static double Grad(const std::vector<float> &l, int w, int x0, int y0, int x1, int y1)
{
    double g = 0;
    for (int y = y0; y < y1; y++) for (int x = x0 + 1; x < x1; x++) g += std::fabs(l[size_t(y) * w + x] - l[size_t(y) * w + x - 1]);
    return g / double((x1 - x0) * (y1 - y0));
}

static void RunCase(ID3D11Device *dev, ID3D11DeviceContext *ctx, int sw, int sh, int ow, int oh)
{
    std::printf("== %dx%d -> %dx%d (letterboxed in %dx%d)\n", sw, sh, ow, oh, ow, oh + 40);
    const Img im = Pattern(sw, sh);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = sw; td.Height = sh; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<uint8_t> nv12(size_t(sw) * sh * 3 / 2);
    memcpy(nv12.data(), im.y.data(), im.y.size()); memcpy(nv12.data() + im.y.size(), im.uv.data(), im.uv.size());
    D3D11_SUBRESOURCE_DATA init = { nv12.data(), UINT(sw), 0 };
    ID3D11Texture2D *src = nullptr;
    HRESULT hr = dev->CreateTexture2D(&td, &init, &src);
    CHECK(SUCCEEDED(hr), "NV12 texture (0x%08lX)", hr);
    if (FAILED(hr)) return;
    ID3D11ShaderResourceView *srv[3] = {};
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
    sd.Format = DXGI_FORMAT_R8_UNORM; dev->CreateShaderResourceView(src, &sd, &srv[0]);
    sd.Format = DXGI_FORMAT_R8G8_UNORM; dev->CreateShaderResourceView(src, &sd, &srv[1]);
    CHECK(srv[0] && srv[1], "NV12 plane views");

    d3d11_device_t d = {};
    d.d3ddevice = dev; d.d3dcontext = ctx; d.context_mutex = INVALID_HANDLE_VALUE;
    pm_nis *n = PM_NisCreate(nullptr, &d);
    CHECK(n != nullptr, "PM_NisCreate");
    if (!n) return;
    const int W = ow, H = oh + 40;   // 20 px bars top and bottom
    ID3D11Texture2D *out = nullptr; ID3D11ShaderResourceView *outSrv = nullptr;
    CHECK(PM_NisResize(nullptr, n, &d, W, H, &out, &outSrv) == VLC_SUCCESS, "PM_NisResize");
    video_format_t f = {};
    f.i_width = f.i_visible_width = sw; f.i_height = f.i_visible_height = sh;
    f.space = COLOR_SPACE_BT709; f.b_color_range_full = false;
    vout_display_place_t place = {};
    place.x = 0; place.y = 20; place.width = ow; place.height = oh;
    CHECK(PM_NisProcess(nullptr, n, &d, srv, 2, &f, &place, 0.5f) == VLC_SUCCESS, "PM_NisProcess");

    D3D11_TEXTURE2D_DESC rd = {};
    out->GetDesc(&rd);
    rd.Usage = D3D11_USAGE_STAGING; rd.BindFlags = 0; rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; rd.MiscFlags = 0;
    ID3D11Texture2D *rb = nullptr;
    dev->CreateTexture2D(&rd, nullptr, &rb);
    ctx->CopyResource(rb, out);
    D3D11_MAPPED_SUBRESOURCE m;
    hr = ctx->Map(rb, 0, D3D11_MAP_READ, 0, &m);
    CHECK(SUCCEEDED(hr), "readback");
    if (FAILED(hr)) return;
    std::vector<float> lum(size_t(ow) * oh);
    double mean = 0; float barMax = 0; int redOk = 0, redN = 0;
    for (int y = 0; y < H; y++) {
        const uint8_t *row = (const uint8_t *)m.pData + size_t(y) * m.RowPitch;
        for (int x = 0; x < W; x++) {
            const uint8_t *p = row + 4 * x;
            const float l = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
            if (y < 20 || y >= 20 + oh) { if (l > barMax) barMax = l; continue; }
            lum[size_t(y - 20) * ow + x] = l; mean += l;
            const int sx = x * sw / ow, sy = (y - 20) * sh / oh;
            if (sx > sw / 8 + 2 && sx < sw / 4 - 2 && sy > sh / 8 + 2 && sy < sh / 4 - 2) { redN++; if (p[0] > p[1] + 60 && p[0] > p[2] + 60) redOk++; }
        }
    }
    ctx->Unmap(rb, 0);
    mean /= double(ow) * oh;
    // the CPU reference: bilinear of the source luma, converted the same way (limited -> full)
    std::vector<float> ref(size_t(ow) * oh);
    double refMean = 0;
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++) {
            const float fx = (x + 0.5f) * sw / ow - 0.5f, fy = (y + 0.5f) * sh / oh - 0.5f;
            const int x0 = std::max(0, std::min(sw - 1, int(std::floor(fx)))), y0 = std::max(0, std::min(sh - 1, int(std::floor(fy))));
            const int x1 = std::min(sw - 1, x0 + 1), y1 = std::min(sh - 1, y0 + 1);
            const float ax = std::max(0.f, std::min(1.f, fx - x0)), ay = std::max(0.f, std::min(1.f, fy - y0));
            auto Y = [&](int xx, int yy) { return float(im.y[size_t(yy) * sw + xx]); };
            const float v = (Y(x0, y0) * (1 - ax) + Y(x1, y0) * ax) * (1 - ay) + (Y(x0, y1) * (1 - ax) + Y(x1, y1) * ax) * ay;
            ref[size_t(y) * ow + x] = std::max(0.f, std::min(255.f, (v - 16.f) * 255.f / 219.f));
            refMean += ref[size_t(y) * ow + x];
        }
    refMean /= double(ow) * oh;
    const double gN = Grad(lum, ow, 0, 0, ow, oh / 2), gB = Grad(ref, ow, 0, 0, ow, oh / 2);
    std::printf("mean %.2f (bilinear %.2f), bars max %.1f, red block %d/%d, edge energy NIS %.3f vs bilinear %.3f (%.0f%%)\n",
                mean, refMean, barMax, redOk, redN, gN, gB, 100.0 * (gN / gB - 1.0));
    CHECK(std::fabs(mean - refMean) < 0.03 * refMean + 2.0, "mean level %.2f vs %.2f", mean, refMean);
    CHECK(barMax < 3.0f, "letterbox bars not black (%.1f)", barMax);
    CHECK(redN > 0 && redOk > redN * 9 / 10, "red block %d/%d", redOk, redN);
    CHECK(gN > gB * 1.02, "NIS not sharper than bilinear (%.3f vs %.3f)", gN, gB);
    rb->Release(); PM_NisDestroy(n); srv[0]->Release(); srv[1]->Release(); src->Release();
}

// 10-bit: a P010 picture (left half luma code 500, right half 501, chroma neutral) scaled 1.5x into an RGB10A2 output
// keeps the one-code step (an RGBA8 output, as before, could not); the levels are the exact 10-bit limited-range ones
static void RunTenBit(ID3D11Device *dev, ID3D11DeviceContext *ctx)
{
    std::printf("== 10-bit: P010 -> RGB10A2\n");
    const int sw = 128, sh = 64, ow = 192, oh = 96;
    std::vector<uint16_t> p010(size_t(sw) * sh * 3 / 2, uint16_t(512 << 6));
    for (int r = 0; r < sh; r++) for (int c = 0; c < sw; c++) p010[size_t(r) * sw + c] = uint16_t((c < sw / 2 ? 500 : 501) << 6);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = sw; td.Height = sh; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_P010;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = { p010.data(), UINT(sw * 2), 0 };
    ID3D11Texture2D *src = nullptr;
    CHECK(SUCCEEDED(dev->CreateTexture2D(&td, &init, &src)), "P010 texture");
    if (!src) return;
    ID3D11ShaderResourceView *srv[3] = {};
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
    sd.Format = DXGI_FORMAT_R16_UNORM; dev->CreateShaderResourceView(src, &sd, &srv[0]);
    sd.Format = DXGI_FORMAT_R16G16_UNORM; dev->CreateShaderResourceView(src, &sd, &srv[1]);
    d3d11_device_t d = {};
    d.d3ddevice = dev; d.d3dcontext = ctx; d.context_mutex = INVALID_HANDLE_VALUE;
    CHECK(PM_NisFormatOk(&d, DXGI_FORMAT_R10G10B10A2_UNORM), "RGB10A2 written by a compute shader");
    pm_nis *n = PM_NisCreateFormat(nullptr, &d, DXGI_FORMAT_R10G10B10A2_UNORM);
    CHECK(n != nullptr, "PM_NisCreateFormat RGB10A2");
    if (!n) return;
    ID3D11Texture2D *out = nullptr; ID3D11ShaderResourceView *outSrv = nullptr;
    CHECK(PM_NisResize(nullptr, n, &d, ow, oh, &out, &outSrv) == VLC_SUCCESS, "PM_NisResize");
    D3D11_TEXTURE2D_DESC od = {};
    out->GetDesc(&od);
    CHECK(od.Format == DXGI_FORMAT_R10G10B10A2_UNORM, "output format %d", int(od.Format));
    video_format_t f = {};
    f.i_width = f.i_visible_width = sw; f.i_height = f.i_visible_height = sh;
    f.space = COLOR_SPACE_BT2020; f.b_color_range_full = false;
    vout_display_place_t place = {};
    place.width = ow; place.height = oh;
    CHECK(PM_NisProcess(nullptr, n, &d, srv, 2, &f, &place, 0.0f) == VLC_SUCCESS, "PM_NisProcess");
    od.Usage = D3D11_USAGE_STAGING; od.BindFlags = 0; od.CPUAccessFlags = D3D11_CPU_ACCESS_READ; od.MiscFlags = 0;
    ID3D11Texture2D *rb = nullptr;
    dev->CreateTexture2D(&od, nullptr, &rb);
    ctx->CopyResource(rb, out);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(rb, 0, D3D11_MAP_READ, 0, &m))) { CHECK(false, "readback"); return; }
    auto G = [&](int x, int y) { return double((((const uint32_t *)((const uint8_t *)m.pData + size_t(y) * m.RowPitch))[x] >> 10) & 1023); };
    const double l = G(ow / 8, oh / 2), r = G(ow - ow / 8, oh / 2);
    ctx->Unmap(rb, 0);
    const double wl = (500.0 - 64) / 876 * 1023, wr = (501.0 - 64) / 876 * 1023;
    std::printf("left %.0f (want %.1f), right %.0f (want %.1f)\n", l, wl, r, wr);
    CHECK(std::fabs(l - wl) <= 1 && std::fabs(r - wr) <= 1, "10-bit levels");
    CHECK(r - l >= 1, "the one-code 10-bit step kept (%.0f -> %.0f)", l, r);
    rb->Release(); PM_NisDestroy(n); srv[0]->Release(); srv[1]->Release(); src->Release();
}

int main()
{
    ID3D11Device *dev = nullptr; ID3D11DeviceContext *ctx = nullptr;
    const D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl, 2,
                                   D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) { std::printf("no WARP device (0x%08lX)\n", hr); return 1; }
    RunCase(dev, ctx, 320, 180, 1280, 720);   // 4x: bilinear to 640x360, then NIS 2x
    RunCase(dev, ctx, 640, 360, 960, 540);    // 1.5x: NIS alone
    RunTenBit(dev, ctx);
    std::printf(fails ? "FAILED (%d)\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
