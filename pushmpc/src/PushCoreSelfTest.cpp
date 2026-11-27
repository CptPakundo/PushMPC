// pushcore: the pushcore_selfTest export (PushCoreSelfTest.h). The tests themselves live with their methods.
#define PUSHCORE_BUILD 1
#include "PushCoreSelfTest.h"
#include "pushmpc/pushcore_abi.h"
#include "veyra/RuntimePaths.h"
#include "veyra/Log.h"
#include "veyra/gfx/FsrSrBackend.h"

#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>

#pragma comment(lib, "d3d12.lib")

using Microsoft::WRL::ComPtr;

namespace pushmpc::selftest {
namespace {
std::vector<Test>& registry() { static std::vector<Test> r; return r; }   // filled by static initialisers before any export runs
std::string json(const std::string& v) {
    std::string e;
    for (const char ch : v) {
        if (ch == '"' || ch == '\\') { e += '\\'; e += ch; }
        else if (ch == '\n') e += "\\n";
        else if (ch == '\r' || ch == '\t') e += ' ';
        else if (static_cast<unsigned char>(ch) < 0x20) e += '?';
        else e += ch;
    }
    return e;
}
std::filesystem::path moduleDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&registry), &self);
    wchar_t path[32768]{};
    const DWORD n = GetModuleFileNameW(self, path, DWORD(std::size(path)));
    if (!n || n >= std::size(path)) return {};
    return std::filesystem::path(path).parent_path();
}
// the adapter: WARP with the flag; else by LUID, or the first hardware adapter in DXGI's high-performance order
bool findAdapter(uint64_t luid, bool warp, DXGI_ADAPTER_DESC1& out) {
    ComPtr<IDXGIFactory6> f;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f)))) return false;
    if (warp) {
        ComPtr<IDXGIAdapter1> a;
        return SUCCEEDED(f->EnumWarpAdapter(IID_PPV_ARGS(&a))) && SUCCEEDED(a->GetDesc1(&out));
    }
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> a;
        if (f->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)) == DXGI_ERROR_NOT_FOUND || !a) return false;
        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(a->GetDesc1(&d)) || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        if (luid && luidOf(d.AdapterLuid) != luid) continue;
        out = d;
        return true;
    }
}
}  // namespace

bool add(const Test& test) {
    auto& r = registry();
    for (const auto& t : r) if (t.bit == test.bit) return false;
    r.push_back(test);
    return true;
}
uint32_t features() {
    uint32_t f = 0;
    for (const auto& t : registry()) f |= t.features;
    return f;
}
std::string narrow(const std::wstring& s) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string r(size_t(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n, nullptr, nullptr);
    return r;
}
std::wstring widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring r(size_t(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n);
    return r;
}
std::wstring driverVersion(const LUID& luid) {
    ComPtr<IDXGIFactory4> f;
    ComPtr<IDXGIAdapter> a;
    LARGE_INTEGER v{};
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f))) || FAILED(f->EnumAdapterByLuid(luid, IID_PPV_ARGS(&a))) ||
        FAILED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &v))) return L"unknown";
    return std::format(L"{}.{}.{}.{}", HIWORD(v.HighPart), LOWORD(v.HighPart), HIWORD(v.LowPart), LOWORD(v.LowPart));
}
uint64_t luidOf(const LUID& l) { return (uint64_t(uint32_t(l.HighPart)) << 32) | l.LowPart; }

// ---------------------------------------------------------------------------------------------------- upscaler probe
namespace {
using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }
// the scene (output px): smooth shading, a checker of blocks, thin lines, a fine grating, small dots (the XeSS SR test's)
double pattern(double x, double y) {
    const double a = 0.5 + 0.2 * std::sin(x * 0.11) * std::cos(y * 0.07) + 0.1 * std::sin((x + y) * 0.031);
    const bool block = (int(std::floor(x / 23.0)) + int(std::floor(y / 17.0))) & 1;
    double v = a * (block ? 0.8 : 1.0) + (block ? 0.08 : 0.0);
    const double lx = std::fmod(x, 9.0), ly = std::fmod(y, 13.0);
    if (lx >= 0 && lx < 1.5) v = 0.25;
    if (ly >= 0 && ly < 1.5) v = 0.8;
    if (std::fmod(x + 2.0 * y, 64.0) < 32.0) v += 0.05 * std::sin((x - y) * 1.3);
    const double dx = std::fmod(x, 6.0) - 3.0, dy = std::fmod(y, 6.0) - 3.0;
    if (std::fmod(y, 48.0) < 24.0 && dx * dx + dy * dy < 1.2) v = 0.05;
    return std::clamp(v, 0.0, 1.0);
}
uint16_t half(float f) { uint32_t u; std::memcpy(&u, &f, 4); const uint32_t s = (u >> 16) & 0x8000; int e = int((u >> 23) & 0xFF) - 127 + 15; uint32_t m = u & 0x7FFFFF;
    if (e <= 0) return uint16_t(s); if (e >= 31) return uint16_t(s | 0x7C00); return uint16_t(s | (uint32_t(e) << 10) | (m >> 13)); }
float unhalf(uint16_t h) { const uint32_t s = (h & 0x8000u) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF; uint32_t u;
    if (!e) u = s; else if (e == 31) u = s | 0x7F800000 | (m << 13); else u = s | ((e + 112) << 23) | (m << 13); float f; std::memcpy(&f, &u, 4); return f; }
double psnrOf(const std::vector<float>& a, const std::vector<float>& b) {
    double se = 0; for (size_t i = 0; i < a.size(); ++i) { const double d = double(a[i]) - b[i]; se += d * d; }
    const double mse = se / double(a.size());
    return mse <= 1e-12 ? 99.0 : 10.0 * std::log10(1.0 / mse);
}
// mean |horizontal step| inside 2x2 blocks over that across block borders
double blockRatio(const std::vector<float>& p, uint32_t W, uint32_t H) {
    double in = 0, across = 0;
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x + 1 < W; ++x) (x & 1 ? across : in) += std::abs(double(p[size_t(y) * W + x + 1]) - p[size_t(y) * W + x]);
    return across > 0 ? in / across : 0;
}
double cubicW(double t) {   // Catmull-Rom (a = -0.5)
    t = std::abs(t);
    return t < 1 ? 1.5 * t * t * t - 2.5 * t * t + 1 : t < 2 ? -0.5 * t * t * t + 2.5 * t * t - 4 * t + 2 : 0;
}
}  // namespace

const char* sceneName(UpscaleScene s) { return s == UpscaleScene::Pan ? "pan" : s == UpscaleScene::Static ? "static" : "static-reset"; }

bool probeDevice(const Context& ctx, ComPtr<ID3D12Device>& device, ComPtr<ID3D12CommandQueue>& queue, std::string& why) {
    ComPtr<IDXGIFactory4> f;
    ComPtr<IDXGIAdapter1> a;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f))) || FAILED(f->EnumAdapterByLuid(ctx.adapter.AdapterLuid, IID_PPV_ARGS(&a)))) { why = "adapter not found"; return false; }
    if (FAILED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) { why = "D3D12CreateDevice failed"; return false; }
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) { why = "CreateCommandQueue failed"; return false; }
    return true;
}

UpscaleRun upscaleProbe(ID3D12Device* device, ID3D12CommandQueue* queue, uint32_t W, uint32_t H, uint32_t frames, UpscaleScene scene, const UpscaleEval& eval) {
    UpscaleRun r;
    W = std::max(64u, W) & ~1u; H = std::max(64u, H) & ~1u; frames = std::clamp(frames, 4u, 240u);
    const uint32_t w = W / 2, h = H / 2;
    auto tex = [&](uint32_t tw, uint32_t th, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS fl, ComPtr<ID3D12Resource>& t) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = tw; rd.Height = th; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = f; rd.SampleDesc = {1, 0}; rd.Flags = fl;
        return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&t)));
    };
    auto buf = [&](uint64_t size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES st, ComPtr<ID3D12Resource>& b) {
        D3D12_HEAP_PROPERTIES hp{type};
        D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = size; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc = {1, 0};
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, st, nullptr, IID_PPV_ARGS(&b)));
    };
    ComPtr<ID3D12Resource> color, motion, depth, out, upC, upM, upD, rb;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fpC{}, fpM{}, fpD{}, fpO{}; UINT64 sC = 0, sM = 0, sD = 0, sO = 0;
    auto fp = [&](uint32_t tw, uint32_t th, DXGI_FORMAT f, D3D12_PLACED_SUBRESOURCE_FOOTPRINT& o, UINT64& s) {
        D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = tw; rd.Height = th; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = f; rd.SampleDesc = {1, 0};
        device->GetCopyableFootprints(&rd, 0, 1, 0, &o, nullptr, nullptr, &s);
    };
    fp(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, fpC, sC); fp(w, h, DXGI_FORMAT_R16G16_FLOAT, fpM, sM); fp(w, h, DXGI_FORMAT_R32_FLOAT, fpD, sD); fp(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, fpO, sO);
    ComPtr<ID3D12CommandAllocator> alloc; ComPtr<ID3D12GraphicsCommandList> list; ComPtr<ID3D12Fence> fence; uint64_t fv = 0;
    if (!tex(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, color) || !tex(w, h, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, motion) ||
        !tex(w, h, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, depth) || !tex(W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, out) ||
        !buf(sC, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, upC) || !buf(sM, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, upM) ||
        !buf(sD, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, upD) || !buf(sO, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, rb) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) { r.note = "test resources"; return r; }
    list->Close();
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    auto idle = [&] { queue->Signal(fence.Get(), ++fv); if (fence->GetCompletedValue() < fv) { fence->SetEventOnCompletion(fv, ev); WaitForSingleObject(ev, 60000); } };
    auto bar = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER br{}; br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; br.Transition = {res, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b}; list->ResourceBarrier(1, &br);
    };
    auto up = [&](ID3D12Resource* dst, ID3D12Resource* src, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& f) {
        bar(dst, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION d{dst, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}; d.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION s{src, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}; s.PlacedFootprint = f;
        list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        bar(dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    };
    // constant depth; the pan moves left/up by (vx, vy) output px a frame, so a point's previous position is current +
    // (vx, vy): the motion is +(vx, vy) / 2 input px. Static scenes: zero motion.
    const bool pan = scene == UpscaleScene::Pan;
    const double vx = pan ? 2.6 : 0.0, vy = pan ? 1.3 : 0.0;
    { uint8_t* m = nullptr; upD->Map(0, nullptr, reinterpret_cast<void**>(&m)); for (uint32_t y = 0; y < h; ++y) { auto* row = reinterpret_cast<float*>(m + fpD.Offset + size_t(y) * fpD.Footprint.RowPitch); for (uint32_t x = 0; x < w; ++x) row[x] = 0.5f; } upD->Unmap(0, nullptr); }
    { uint8_t* m = nullptr; upM->Map(0, nullptr, reinterpret_cast<void**>(&m)); const uint16_t mx = half(float(vx / 2)), my = half(float(vy / 2));
      for (uint32_t y = 0; y < h; ++y) { auto* row = reinterpret_cast<uint16_t*>(m + fpM.Offset + size_t(y) * fpM.Footprint.RowPitch); for (uint32_t x = 0; x < w; ++x) { row[2 * x] = mx; row[2 * x + 1] = my; } } upM->Unmap(0, nullptr); }
    double execMs = 0;
    std::vector<float> truth(size_t(W) * H), got(size_t(W) * H), bil(size_t(W) * H), bic(size_t(W) * H), tmp(size_t(W) * h), lowres(size_t(w) * h);
    for (uint32_t k = 0; k < frames; ++k) {
        const double ox = vx * k, oy = vy * k;
        uint8_t* m = nullptr; upC->Map(0, nullptr, reinterpret_cast<void**>(&m));
        for (uint32_t y = 0; y < h; ++y) {   // input: 2x2 box average of the full-resolution scene (a downscaled source)
            auto* row = reinterpret_cast<uint16_t*>(m + fpC.Offset + size_t(y) * fpC.Footprint.RowPitch);
            for (uint32_t x = 0; x < w; ++x) {
                double s = 0;
                for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) s += pattern(2.0 * x + dx + 0.5 + ox, 2.0 * y + dy + 0.5 + oy);
                const float v = float(s / 4); lowres[size_t(y) * w + x] = v;
                row[4 * x] = row[4 * x + 1] = row[4 * x + 2] = half(v); row[4 * x + 3] = half(1.0f);
            }
        }
        upC->Unmap(0, nullptr);
        alloc->Reset(); list->Reset(alloc.Get(), nullptr);
        up(color.Get(), upC.Get(), fpC);
        if (k == 0) { up(motion.Get(), upM.Get(), fpM); up(depth.Get(), upD.Get(), fpD); bar(out.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); }
        const auto t = Clock::now();
        if (!eval(list.Get(), color.Get(), motion.Get(), depth.Get(), out.Get(), w, h, W, H, k == 0 || scene == UpscaleScene::StaticReset)) {
            list->Close(); CloseHandle(ev); r.note = std::format("the upscaler failed at frame {}", k); return r;
        }
        bar(color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        list->Close();
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        idle();
        execMs += msSince(t);   // the upscale alone (evaluate + submit + wait); the readback below is not timed
        if (FAILED(device->GetDeviceRemovedReason())) { CloseHandle(ev); r.note = "device removed"; return r; }
        const bool measure = k < 2 || k + 12 >= frames;
        if (!measure) continue;
        alloc->Reset(); list->Reset(alloc.Get(), nullptr);
        bar(out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        {
            D3D12_TEXTURE_COPY_LOCATION d{rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}; d.PlacedFootprint = fpO;
            D3D12_TEXTURE_COPY_LOCATION s{out.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}; s.SubresourceIndex = 0;
            list->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        }
        bar(out.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->Close();
        queue->ExecuteCommandLists(1, lists);
        idle();
        uint8_t* o = nullptr; rb->Map(0, nullptr, reinterpret_cast<void**>(&o));
        double mean = 0;
        for (uint32_t y = 0; y < H; ++y) {
            auto* row = reinterpret_cast<const uint16_t*>(o + fpO.Offset + size_t(y) * fpO.Footprint.RowPitch);
            for (uint32_t x = 0; x < W; ++x) { got[size_t(y) * W + x] = unhalf(row[4 * x]); mean += got[size_t(y) * W + x]; truth[size_t(y) * W + x] = float(pattern(x + 0.5 + ox, y + 0.5 + oy)); }
        }
        D3D12_RANGE none{0, 0}; rb->Unmap(0, &none);
        mean /= double(W) * H;
        if (!(mean > 0.05 && mean < 0.95)) { CloseHandle(ev); r.note = std::format("the output is blank or saturated (mean {:.3f}) at frame {}", mean, k); return r; }
        for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < W; ++x) {   // bilinear upscale of the input
            const double sx = std::clamp((x + 0.5) / 2 - 0.5, 0.0, double(w - 1)), sy = std::clamp((y + 0.5) / 2 - 0.5, 0.0, double(h - 1));
            const uint32_t x0 = uint32_t(sx), y0 = uint32_t(sy), x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
            const double fx = sx - x0, fy = sy - y0;
            bil[size_t(y) * W + x] = float((lowres[y0 * w + x0] * (1 - fx) + lowres[y0 * w + x1] * fx) * (1 - fy) + (lowres[y1 * w + x0] * (1 - fx) + lowres[y1 * w + x1] * fx) * fy);
        }
        for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < W; ++x) {   // bicubic (Catmull-Rom), separable: rows first
            const double sx = (x + 0.5) / 2 - 0.5; const int i0 = int(std::floor(sx)); double s = 0, ws = 0;
            for (int i = i0 - 1; i <= i0 + 2; ++i) { const double wt = cubicW(sx - i); s += wt * lowres[size_t(y) * w + size_t(std::clamp(i, 0, int(w) - 1))]; ws += wt; }
            tmp[size_t(y) * W + x] = float(s / ws);
        }
        for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < W; ++x) {
            const double sy = (y + 0.5) / 2 - 0.5; const int j0 = int(std::floor(sy)); double s = 0, ws = 0;
            for (int j = j0 - 1; j <= j0 + 2; ++j) { const double wt = cubicW(sy - j); s += wt * tmp[size_t(std::clamp(j, 0, int(h) - 1)) * W + x]; ws += wt; }
            bic[size_t(y) * W + x] = float(std::clamp(s / ws, 0.0, 1.0));
        }
        r.frames.push_back({k, psnrOf(got, truth), psnrOf(bic, truth), psnrOf(bil, truth), blockRatio(got, W, H), blockRatio(bic, W, H)});
    }
    CloseHandle(ev);
    r.msPerFrame = execMs / frames;
    r.ok = true;
    return r;
}

std::string upscaleSummary(const UpscaleRun& run, bool warp) {
    if (!run.ok) return "not run: " + run.note;
    std::string s;
    double p = 0, b = 0, q = 0, qb = 0; uint32_t n = 0, lo = UINT32_MAX, hi = 0;
    for (const auto& f : run.frames) {
        if (f.k < 2) { s += std::format("f{} psnr {:.2f} (bicubic {:.2f}) block {:.3f} (bicubic {:.3f}); ", f.k, f.psnr, f.bicubic, f.block, f.blockBicubic); continue; }
        p += f.psnr; b += f.bicubic; q += f.block; qb += f.blockBicubic; ++n; lo = std::min(lo, f.k); hi = std::max(hi, f.k);
    }
    if (n) s += std::format("f{}-{} mean psnr {:.2f} (bicubic {:.2f}) block {:.3f} (bicubic {:.3f}); ", lo, hi, p / n, b / n, q / n, qb / n);
    s += std::format("{:.1f} ms a frame{}", run.msPerFrame, warp ? " (WARP-functional: a software adapter, not a quality claim for any GPU)" : "");
    return s;
}

// ---------------------------------------------------------------------------------------------------- FSR SR probe
// PUSHCORE_SELFTEST_FSR_SR ("fsrsr"): the graph's FSR upscaler (PushFPS's FsrSrBackend with the FidelityFX loader and
// provider of the runtime root, runtime_local\amd\fidelityfx or PUSHMPC_FSR_SR_DIR) in the upscaler probe, in both of its
// modes for video (PUSHMPC_FSR_SR_SINGLE_FRAME = 1: every frame a new sample; 0: temporal) on each scene. Numbers only:
// passes when FSR starts and every run produces a picture; "unavailable" without the FidelityFX files or when no FSR
// provider runs on this adapter.
namespace {
uint32_t envFrames() {
    wchar_t v[16]{};
    const DWORD n = GetEnvironmentVariableW(L"PUSHMPC_SELFTEST_FRAMES", v, 16);
    const uint32_t f = n && n < 16 ? uint32_t(_wtoi(v)) : 16u;
    return std::clamp(f, 4u, 240u);
}
Result testFsrSr(const Context& ctx) {
    Result o;
    ComPtr<ID3D12Device> device; ComPtr<ID3D12CommandQueue> queue; std::string why;
    if (!probeDevice(ctx, device, queue, why)) { o.verdict = "fail: " + why; return o; }
    const uint32_t W = 1280, H = 720, w = W / 2, h = H / 2, frames = envFrames();
    wchar_t saved[8]{};
    const DWORD hadMode = GetEnvironmentVariableW(L"PUSHMPC_FSR_SR_SINGLE_FRAME", saved, 8);
    std::string provider;
    bool allOk = true;
    for (const wchar_t* mode : {L"1", L"0"}) {
        SetEnvironmentVariableW(L"PUSHMPC_FSR_SR_SINGLE_FRAME", mode);   // read at initialize
        for (const UpscaleScene scene : {UpscaleScene::Pan, UpscaleScene::Static, UpscaleScene::StaticReset}) {
            veyra::gfx::FsrSrBackend fsr;
            if (!fsr.initialize(device.Get(), w, h, W, H, false)) {
                if (hadMode && hadMode < 8) SetEnvironmentVariableW(L"PUSHMPC_FSR_SR_SINGLE_FRAME", saved); else SetEnvironmentVariableW(L"PUSHMPC_FSR_SR_SINGLE_FRAME", nullptr);
                o.verdict = "unavailable: FSR did not start on this adapter (the FidelityFX files in runtime_local\\amd\\fidelityfx, or no provider for it; see the log)";
                return o;
            }
            if (fsr.providerVersion() && *fsr.providerVersion()) provider = fsr.providerVersion();
            const UpscaleRun run = upscaleProbe(device.Get(), queue.Get(), W, H, frames, scene,
                [&](ID3D12GraphicsCommandList* list, ID3D12Resource* color, ID3D12Resource* motion, ID3D12Resource* depth, ID3D12Resource* out,
                    uint32_t iw, uint32_t ih, uint32_t ow, uint32_t oh, bool reset) {
                    return fsr.evaluate(list, color, motion, depth, out, iw, ih, ow, oh, reset, 33.3f);
                });
            allOk = allOk && run.ok;
            const std::string key = std::string(mode[0] == L'1' ? "single." : "temporal.") + sceneName(scene);
            o.fields.push_back({key, upscaleSummary(run, ctx.warp)});
        }
    }
    if (hadMode && hadMode < 8) SetEnvironmentVariableW(L"PUSHMPC_FSR_SR_SINGLE_FRAME", saved); else SetEnvironmentVariableW(L"PUSHMPC_FSR_SR_SINGLE_FRAME", nullptr);
    o.fields.push_back({"provider", provider.empty() ? "unknown" : provider});
    o.pass = allOk;
    o.verdict = std::format("{}; FSR {} {}x{} -> {}x{}, {} frames, single-frame and temporal on pan / still / still reset{}", allOk ? "pass" : "fail: a run produced no picture",
                            provider.empty() ? "?" : provider, w, h, W, H, frames, ctx.warp ? " (WARP-functional numbers)" : "");
    return o;
}
const Registrar kFsrSr({PUSHCORE_SELFTEST_FSR_SR, "fsrsr", 0, &testFsrSr});
}  // namespace

}  // namespace pushmpc::selftest

extern "C" PUSHCORE_API int pushcore_selfTest(uint64_t adapterLuid, const wchar_t* rootDir, uint32_t which, uint32_t flags, char* report, uint32_t reportSize) {
    using namespace pushmpc::selftest;
    static std::mutex serial;   // one self-test run at a time (they own devices, files and registry values)
    std::lock_guard<std::mutex> lock(serial);
    std::string out = "{";
    auto field = [&](const std::string& k, const std::string& v) { out += std::format("{}\"{}\": \"{}\"", out.size() > 1 ? ", " : "", json(k), json(v)); };
    bool all = which != 0;
    try {
        Context ctx;
        ctx.warp = (flags & PUSHCORE_SELFTEST_FLAG_WARP) != 0;
        ctx.record = (flags & PUSHCORE_SELFTEST_FLAG_RECORD) != 0 && !ctx.warp;
        ctx.root = rootDir && *rootDir ? std::filesystem::path(rootDir) : moduleDir();
        veyra::runtime::setApplicationRootOverride(ctx.root);
        ctx.haveAdapter = findAdapter(adapterLuid, ctx.warp, ctx.adapter);
        if (ctx.haveAdapter) {
            ctx.luid = luidOf(ctx.adapter.AdapterLuid);
            ctx.driver = driverVersion(ctx.adapter.AdapterLuid);
            field("adapter", std::format("{} (vendor {:04X} device {:04X})", narrow(ctx.adapter.Description), ctx.adapter.VendorId, ctx.adapter.DeviceId));
            field("driver", narrow(ctx.driver));
        } else {
            field("adapter", ctx.warp ? "none (no WARP adapter)" : "none");
            all = false;
        }
        out += std::format(", \"features\": {}", features());
        uint32_t known = 0;
        for (const auto& t : registry()) {
            known |= t.bit;
            if (!(which & t.bit)) continue;
            Result r;
            if (!ctx.haveAdapter) r.verdict = "fail: no adapter";
            else {
                try { r = t.run(ctx); }
                catch (const std::exception& e) { r = {}; r.verdict = std::string("fail: exception: ") + e.what(); }
                catch (...) { r = {}; r.verdict = "fail: exception"; }
            }
            field(t.key, r.verdict);
            for (const auto& [k, v] : r.fields) field(std::string(t.key) + "." + k, v);
            veyra::log::info("pushcore", std::format("self-test {}: {}", t.key, r.verdict));
            all = all && r.pass;
        }
        for (uint32_t b = 1; b; b <<= 1)
            if ((which & b) && !(known & b)) { field(std::format("test{}", b), "unavailable: not in this build"); all = false; }
    } catch (const std::exception& e) { field("exception", e.what()); all = false; }
    catch (...) { field("exception", "unknown"); all = false; }
    out += "}";
    if (report && reportSize) {
        const size_t n = std::min<size_t>(out.size(), reportSize - 1);
        std::memcpy(report, out.data(), n);
        report[n] = 0;
    }
    return all ? PUSHCORE_OK : PUSHCORE_ERROR;
}
