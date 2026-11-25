// vsr_gate_probe: the self-test of two PushMPC unlocks for RTX Video Super Resolution off its listed GPUs.
//
//   Gtx16Vsr  GTX 16 (TU116 / TU117, compute capability 7.5). nvngx_vsr.dll ships complete sm_75 kernels (that is how
//             it runs on RTX 20); the gate is NGX's feature table in the driver's NGX core, not the kernels.
//   ZludaVsr  RX 7000 / 9000 through ZLUDA (a CUDA driver on HIP): nvngx_vsr.dll's CUDA entry points with ZLUDA's
//             nvcuda.dll (its PTX is translated; the sm_xx SASS is not used).
//
// Routes, tried in this order (each one in a fresh D3D12 device; a failure never stops the next):
//   core      the public NGX path through the driver's NGX core: NVSDK_NGX_D3D12_Init + CreateFeature(VSR) +
//             EvaluateFeature, exactly as pushcore (PushFPS VideoSrBackend) calls it. If the gate exists, this route
//             fails with FeatureNotSupported / PlatformError on GTX 16 (the log says which).
//   snippet   the NGX core bypassed: nvngx_vsr.dll's own exported NVSDK_NGX_D3D12_Init_Ext / CreateFeature /
//             EvaluateFeature are called directly with our own NVSDK_NGX_Parameter (the NGX feature-DLL contract; the
//             GPU architecture comes from NvAPI inside the DLL, "SetGPUArch", so TU116 gets the Turing kernels).
//   cuda      (--cuda-dll <nvcuda.dll>, ZLUDA) nvngx_vsr.dll's NVSDK_NGX_CUDA_* entry points on CUDA buffers.
// Each route that evaluates is checked: the output is not blank or constant, it is close to a bicubic upscale of the
// input (PSNR >= 24 dB: VSR keeps the picture), it is at least as sharp (gradient energy >= 0.9 x bicubic), and the
// device is not removed. Time per evaluation from D3D12 timestamps (median of 20) at the asked size.
//
// usage: vsr_gate_probe --runtime <folder with nvngx_vsr.dll> [--route core|snippet|cuda|all] [--width 960 --height 540]
//                       [--scale 2] [--quality 1..4] [--cuda-dll <path>] [--record Gtx16Vsr|ZludaVsr] [--out <dir>]
// exit code: 0 = at least one route PASSED, 1 = every route failed, 2 = usage / no adapter.
// --record writes the result to HKCU\Software\PushMPC\SelfTest\<Name> with the key pushcore reads
// (vendor:device:driver), so the unlock turns on (pass) or stays off with the reason (fail) at the next file.
//
// This is a tester tool: it is never run by PushMPC's CI on NVIDIA hardware, and the project does not run it on its own
// development GPU (tests\gtx16-vsr-manual.ps1 / tests\zluda-vsr-manual.ps1 run it on the testers' cards).
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "nvsdk_ngx.h"
#include "nvsdk_ngx_defs.h"
#include "nvsdk_ngx_params.h"
#include "../include/pushmpc/unlocks.h"

using Microsoft::WRL::ComPtr;

namespace {

FILE* g_log = stdout;
void Log(const char* fmt, ...)
{
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a); fputc('\n', g_log); fflush(g_log);
}
std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0'); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// ------------------------------------------------------------------------------------------------ our NGX parameter map
// NVSDK_NGX_Parameter is an abstract class (nvsdk_ngx_params.h); the core normally hands one out. For the snippet route
// we own it: every Set is kept by name; Get answers what was set and NVSDK_NGX_Result_Fail otherwise.
class ParamMap final : public NVSDK_NGX_Parameter {
public:
    struct V { int type = 0; unsigned long long u = 0; double d = 0; void* p = nullptr; };
    std::map<std::string, V> m;
    void Set(const char* n, unsigned long long v) override { V x; x.type = 1; x.u = v; m[n] = x; }
    void Set(const char* n, float v) override { V x; x.type = 2; x.d = v; m[n] = x; }
    void Set(const char* n, double v) override { V x; x.type = 2; x.d = v; m[n] = x; }
    void Set(const char* n, unsigned int v) override { V x; x.type = 1; x.u = v; m[n] = x; }
    void Set(const char* n, int v) override { V x; x.type = 1; x.u = (unsigned long long)(long long)v; m[n] = x; }
    void Set(const char* n, ID3D11Resource* v) override { V x; x.type = 3; x.p = v; m[n] = x; }
    void Set(const char* n, ID3D12Resource* v) override { V x; x.type = 3; x.p = v; m[n] = x; }
    void Set(const char* n, void* v) override { V x; x.type = 3; x.p = v; m[n] = x; }
    template <class T> NVSDK_NGX_Result num(const char* n, T* o) const
    {
        auto it = m.find(n); if (it == m.end() || !o) return NVSDK_NGX_Result_Fail;
        *o = it->second.type == 2 ? T(it->second.d) : T(it->second.u); return NVSDK_NGX_Result_Success;
    }
    NVSDK_NGX_Result Get(const char* n, unsigned long long* o) const override { return num(n, o); }
    NVSDK_NGX_Result Get(const char* n, float* o) const override { return num(n, o); }
    NVSDK_NGX_Result Get(const char* n, double* o) const override { return num(n, o); }
    NVSDK_NGX_Result Get(const char* n, unsigned int* o) const override { return num(n, o); }
    NVSDK_NGX_Result Get(const char* n, int* o) const override { return num(n, o); }
    NVSDK_NGX_Result Get(const char* n, ID3D11Resource** o) const override { auto it = m.find(n); if (it == m.end() || !o) return NVSDK_NGX_Result_Fail; *o = (ID3D11Resource*)it->second.p; return NVSDK_NGX_Result_Success; }
    NVSDK_NGX_Result Get(const char* n, ID3D12Resource** o) const override { auto it = m.find(n); if (it == m.end() || !o) return NVSDK_NGX_Result_Fail; *o = (ID3D12Resource*)it->second.p; return NVSDK_NGX_Result_Success; }
    NVSDK_NGX_Result Get(const char* n, void** o) const override { auto it = m.find(n); if (it == m.end() || !o) return NVSDK_NGX_Result_Fail; *o = it->second.p; return NVSDK_NGX_Result_Success; }
    void Reset() override { m.clear(); }
};

// ------------------------------------------------------------------------------------------------ test picture
// 8-bit RGBA, a mix VSR has to keep: smooth gradients, hard edges at several angles, fine text-like strokes, noise
std::vector<uint8_t> MakeInput(UINT w, UINT h)
{
    std::vector<uint8_t> p(size_t(w) * h * 4);
    uint32_t seed = 12345;
    for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x) {
        seed = seed * 1664525u + 1013904223u;
        const double fx = double(x) / w, fy = double(y) / h;
        double r = 40 + 150 * fx, g = 60 + 120 * fy, b = 90 + 60 * std::sin(fx * 9 + fy * 5);
        if (((x / 24) + (y / 24)) % 2 == 0 && fx > 0.55 && fy < 0.45) { r = 230; g = 230; b = 230; }   // checker
        const double ring = std::hypot(fx - 0.25, fy - 0.65) * 40; if (std::fmod(ring, 2.0) < 1.0) { r *= 0.4; g *= 0.4; }   // rings
        if (fy > 0.80 && fy < 0.90 && (x % 7 == 0 || (y % 9 == 0 && x % 3 != 0))) { r = g = b = 250; }   // strokes
        const double n = double(seed >> 24) / 255.0 * 12 - 6;
        auto c = [&](double v) { return uint8_t(std::clamp(v + n, 0.0, 255.0)); };
        uint8_t* o = &p[(size_t(y) * w + x) * 4]; o[0] = c(r); o[1] = c(g); o[2] = c(b); o[3] = 255;
    }
    return p;
}
double Cubic(double x) { x = std::fabs(x); const double a = -0.5; return x <= 1 ? (a + 2) * x * x * x - (a + 3) * x * x + 1 : x < 2 ? a * x * x * x - 5 * a * x * x + 8 * a * x - 4 * a : 0; }
std::vector<uint8_t> Bicubic(const std::vector<uint8_t>& in, UINT w, UINT h, UINT ow, UINT oh)
{
    std::vector<uint8_t> o(size_t(ow) * oh * 4);
    for (UINT y = 0; y < oh; ++y) for (UINT x = 0; x < ow; ++x) {
        const double sx = (x + 0.5) * w / ow - 0.5, sy = (y + 0.5) * h / oh - 0.5;
        const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
        for (int c = 0; c < 4; ++c) {
            double acc = 0, wsum = 0;
            for (int j = -1; j <= 2; ++j) for (int i = -1; i <= 2; ++i) {
                const int px = std::clamp(ix + i, 0, int(w) - 1), py = std::clamp(iy + j, 0, int(h) - 1);
                const double wt = Cubic(sx - (ix + i)) * Cubic(sy - (iy + j));
                acc += wt * in[(size_t(py) * w + px) * 4 + c]; wsum += wt;
            }
            o[(size_t(y) * ow + x) * 4 + c] = uint8_t(std::clamp(acc / wsum, 0.0, 255.0));
        }
    }
    return o;
}
double Psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    double se = 0; size_t n = 0;
    for (size_t i = 0; i + 3 < a.size(); i += 4) for (int c = 0; c < 3; ++c) { const double d = double(a[i + c]) - b[i + c]; se += d * d; ++n; }
    const double mse = se / std::max<size_t>(n, 1);
    return mse <= 1e-9 ? 99.0 : 10 * std::log10(255.0 * 255.0 / mse);
}
double GradientEnergy(const std::vector<uint8_t>& p, UINT w, UINT h)
{
    double e = 0;
    for (UINT y = 1; y < h; ++y) for (UINT x = 1; x < w; ++x) {
        const size_t i = (size_t(y) * w + x) * 4;
        const double gx = double(p[i + 1]) - p[i + 1 - 4], gy = double(p[i + 1]) - p[i + 1 - size_t(w) * 4];
        e += gx * gx + gy * gy;
    }
    return e / (double(w) * h);
}
bool WritePpm(const std::wstring& path, const std::vector<uint8_t>& p, UINT w, UINT h)
{
    FILE* f = nullptr; if (_wfopen_s(&f, path.c_str(), L"wb") || !f) return false;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (size_t i = 0; i < size_t(w) * h; ++i) fwrite(&p[i * 4], 1, 3, f);
    fclose(f); return true;
}

// ------------------------------------------------------------------------------------------------ D3D12 helpers
struct Gpu {
    ComPtr<IDXGIAdapter1> adapter; DXGI_ADAPTER_DESC1 desc{};
    ComPtr<ID3D12Device> dev; ComPtr<ID3D12CommandQueue> q; ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list; ComPtr<ID3D12Fence> fence; UINT64 fv = 0; HANDLE ev = nullptr;
    ComPtr<ID3D12QueryHeap> ts; ComPtr<ID3D12Resource> tsRead; UINT64 tsFreq = 0;
    bool Init()
    {
        ComPtr<IDXGIFactory6> f; if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f)))) return false;
        if (FAILED(f->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)))) return false;
        adapter->GetDesc1(&desc);
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) return false;
        D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)))) return false;
        dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
        dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list));
        dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        D3D12_QUERY_HEAP_DESC hd{}; hd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; hd.Count = 64;
        dev->CreateQueryHeap(&hd, IID_PPV_ARGS(&ts));
        tsRead = Buffer(64 * 8, D3D12_HEAP_TYPE_READBACK);
        q->GetTimestampFrequency(&tsFreq);
        return list != nullptr && fence != nullptr;
    }
    ComPtr<ID3D12Resource> Buffer(UINT64 size, D3D12_HEAP_TYPE heap)
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = heap;
        D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, heap == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r));
        return r;
    }
    ComPtr<ID3D12Resource> Texture(UINT w, UINT h, bool uav)
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count = 1; rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        ComPtr<ID3D12Resource> r;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r));
        return r;
    }
    bool Submit()
    {
        if (FAILED(list->Close())) return false;
        ID3D12CommandList* l[] = { list.Get() }; q->ExecuteCommandLists(1, l);
        q->Signal(fence.Get(), ++fv); fence->SetEventOnCompletion(fv, ev);
        if (WaitForSingleObject(ev, 20000) != WAIT_OBJECT_0) return false;
        alloc->Reset(); list->Reset(alloc.Get(), nullptr);
        return true;
    }
    HRESULT Removed() const { return dev ? dev->GetDeviceRemovedReason() : E_FAIL; }
    void Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
    {
        D3D12_RESOURCE_BARRIER br{}; br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; br.Transition.pResource = r;
        br.Transition.StateBefore = a; br.Transition.StateAfter = b; br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &br);
    }
    void Upload(ID3D12Resource* tex, const std::vector<uint8_t>& px, UINT w, UINT h, ComPtr<ID3D12Resource>& staging)
    {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 total = 0; const auto d = tex->GetDesc();
        dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
        staging = Buffer(total, D3D12_HEAP_TYPE_UPLOAD);
        uint8_t* m = nullptr; staging->Map(0, nullptr, reinterpret_cast<void**>(&m));
        for (UINT y = 0; y < h; ++y) memcpy(m + fp.Offset + size_t(y) * fp.Footprint.RowPitch, &px[size_t(y) * w * 4], size_t(w) * 4);
        staging->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION dst{ tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{ staging.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; src.PlacedFootprint = fp;
        Barrier(tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barrier(tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    std::vector<uint8_t> Read(ID3D12Resource* tex, UINT w, UINT h, D3D12_RESOURCE_STATES state)
    {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 total = 0; const auto d = tex->GetDesc();
        dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
        auto rb = Buffer(total, D3D12_HEAP_TYPE_READBACK);
        D3D12_TEXTURE_COPY_LOCATION src{ tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; src.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION dst{ rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; dst.PlacedFootprint = fp;
        Barrier(tex, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barrier(tex, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
        std::vector<uint8_t> out;
        if (!Submit()) return out;
        out.resize(size_t(w) * h * 4);
        uint8_t* m = nullptr; rb->Map(0, nullptr, reinterpret_cast<void**>(&m));
        for (UINT y = 0; y < h; ++y) memcpy(&out[size_t(y) * w * 4], m + fp.Offset + size_t(y) * fp.Footprint.RowPitch, size_t(w) * 4);
        rb->Unmap(0, nullptr);
        return out;
    }
};

struct Options {
    std::wstring runtime, route = L"all", cudaDll, record, out;
    UINT w = 960, h = 540, scale = 2, quality = 1;
};
struct RouteResult { std::string route; bool ran = false, pass = false; std::string why; double psnr = 0, sharp = 0, msMedian = -1; };

// the checks every route's output goes through
void Judge(RouteResult& r, const Options& o, const std::vector<uint8_t>& in, const std::vector<uint8_t>& out, HRESULT removed)
{
    const UINT ow = o.w * o.scale, oh = o.h * o.scale;
    if (removed != S_OK) { r.why = "the device was removed (0x" + std::to_string(unsigned(removed)) + ")"; return; }
    if (out.size() != size_t(ow) * oh * 4) { r.why = "no output read back"; return; }
    uint8_t mn = 255, mx = 0; for (size_t i = 0; i < out.size(); i += 4) { mn = std::min(mn, out[i + 1]); mx = std::max(mx, out[i + 1]); }
    if (mx - mn < 16) { r.why = "the output is blank or constant (green " + std::to_string(mn) + ".." + std::to_string(mx) + ")"; return; }
    const auto ref = Bicubic(in, o.w, o.h, ow, oh);
    r.psnr = Psnr(out, ref);
    r.sharp = GradientEnergy(out, ow, oh) / std::max(1e-9, GradientEnergy(ref, ow, oh));
    if (r.psnr < 24.0) { r.why = "the output does not match the picture (PSNR " + std::to_string(r.psnr) + " dB against bicubic, needs 24)"; return; }
    if (r.sharp < 0.9) { r.why = "the output is softer than bicubic (gradient energy x" + std::to_string(r.sharp) + ")"; return; }
    if (!o.out.empty()) WritePpm(o.out + L"\\" + std::wstring(r.route.begin(), r.route.end()) + L".ppm", out, ow, oh);
    r.pass = true;
    r.why = "output sane";
}

// --- route core: the public NGX API through the driver's NGX core (pushcore's own path)
RouteResult RunCore(const Options& o, const std::vector<uint8_t>& in)
{
    RouteResult r; r.route = "core";
    Gpu g; if (!g.Init()) { r.why = "no D3D12 device"; return r; }
    const std::wstring data = o.out.empty() ? L"." : o.out;
    NVSDK_NGX_Result res = NVSDK_NGX_D3D12_Init(0x1u /*app id*/, data.c_str(), g.dev.Get());
    Log("  core: NVSDK_NGX_D3D12_Init 0x%08X", unsigned(res));
    if (NVSDK_NGX_FAILED(res)) { r.why = "NGX init failed 0x" + std::to_string(unsigned(res)); return r; }
    NVSDK_NGX_Parameter* caps = nullptr;
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_GetCapabilityParameters(&caps)) && caps) {
        int vsr = -1, vsr2 = -1; caps->Get("VSR.Available", &vsr); caps->Get("VideoSuperResolution.Available", &vsr2);
        Log("  core: capability VSR.Available=%d VideoSuperResolution.Available=%d (the NGX core's feature table; 0 = the gate)", vsr, vsr2);
        NVSDK_NGX_D3D12_DestroyParameters(caps);
    }
    NVSDK_NGX_Parameter* p = nullptr;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_AllocateParameters(&p)) || !p) { r.why = "AllocateParameters failed"; NVSDK_NGX_D3D12_Shutdown1(g.dev.Get()); return r; }
    const UINT ow = o.w * o.scale, oh = o.h * o.scale;
    auto inTex = g.Texture(o.w, o.h, false), outTex = g.Texture(ow, oh, true);
    ComPtr<ID3D12Resource> staging; g.Upload(inTex.Get(), in, o.w, o.h, staging);
    g.Barrier(outTex.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    p->Set("CreationNodeMask", 1u); p->Set("VisibilityNodeMask", 1u);
    p->Set("Width", o.w); p->Set("Height", o.h); p->Set("OutWidth", ow); p->Set("OutHeight", oh);
    NVSDK_NGX_Handle* h = nullptr;
    res = NVSDK_NGX_D3D12_CreateFeature(g.list.Get(), NVSDK_NGX_Feature_Reserved16, p, &h);
    Log("  core: CreateFeature(VSR) 0x%08X%s", unsigned(res), res == NVSDK_NGX_Result_FAIL_FeatureNotSupported ? " (FeatureNotSupported: the NGX gate)" : "");
    r.ran = NVSDK_NGX_SUCCEED(res) && h;
    std::vector<double> ms;
    if (r.ran) {
        p->Set("Input1", inTex.Get()); p->Set("Output", outTex.Get());
        p->Set("Rect.X", 0u); p->Set("Rect.Y", 0u); p->Set("Rect.W", o.w); p->Set("Rect.H", o.h);
        p->Set("OutRect.X", 0u); p->Set("OutRect.Y", 0u); p->Set("OutRect.W", ow); p->Set("OutRect.H", oh);
        p->Set("VSR.QualityLevel", o.quality);
        for (int i = 0; i < 21 && r.ran; ++i) {
            g.list->EndQuery(g.ts.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            res = NVSDK_NGX_D3D12_EvaluateFeature(g.list.Get(), h, p);
            g.list->EndQuery(g.ts.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            g.list->ResolveQueryData(g.ts.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, g.tsRead.Get(), 0);
            if (NVSDK_NGX_FAILED(res) || !g.Submit()) { r.ran = false; r.why = "EvaluateFeature failed 0x" + std::to_string(unsigned(res)); break; }
            UINT64* t = nullptr; g.tsRead->Map(0, nullptr, reinterpret_cast<void**>(&t));
            if (i > 0) ms.push_back(double(t[1] - t[0]) * 1000.0 / double(g.tsFreq));
            g.tsRead->Unmap(0, nullptr);
        }
    } else r.why = "CreateFeature failed 0x" + std::to_string(unsigned(res));
    if (r.ran) {
        std::sort(ms.begin(), ms.end()); r.msMedian = ms.empty() ? -1 : ms[ms.size() / 2];
        Judge(r, o, in, g.Read(outTex.Get(), ow, oh, D3D12_RESOURCE_STATE_UNORDERED_ACCESS), g.Removed());
    }
    if (h) NVSDK_NGX_D3D12_ReleaseFeature(h);
    NVSDK_NGX_D3D12_DestroyParameters(p);
    NVSDK_NGX_D3D12_Shutdown1(g.dev.Get());
    return r;
}

// --- route snippet: nvngx_vsr.dll's own D3D12 entry points, no NGX core
using InitExtFn = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
using CreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using EvalFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using ReleaseFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
using ShutdownFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);
using PopulateFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
template <class F> bool Seh(F&& f, unsigned& code) { __try { f(); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { code = GetExceptionCode(); return false; } }

RouteResult RunSnippet(const Options& o, const std::vector<uint8_t>& in)
{
    RouteResult r; r.route = "snippet";
    const std::wstring dll = o.runtime + L"\\nvngx_vsr.dll";
    HMODULE m = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) { r.why = "cannot load " + Narrow(dll); return r; }
    auto init = reinterpret_cast<InitExtFn>(GetProcAddress(m, "NVSDK_NGX_D3D12_Init_Ext"));
    auto create = reinterpret_cast<CreateFn>(GetProcAddress(m, "NVSDK_NGX_D3D12_CreateFeature"));
    auto eval = reinterpret_cast<EvalFn>(GetProcAddress(m, "NVSDK_NGX_D3D12_EvaluateFeature"));
    auto rel = reinterpret_cast<ReleaseFn>(GetProcAddress(m, "NVSDK_NGX_D3D12_ReleaseFeature"));
    auto shut = reinterpret_cast<ShutdownFn>(GetProcAddress(m, "NVSDK_NGX_D3D12_Shutdown1"));
    auto populate = reinterpret_cast<PopulateFn>(GetProcAddress(m, "NVSDK_NGX_PopulateParameters"));
    if (!init || !create || !eval || !rel) { r.why = "nvngx_vsr.dll lacks the D3D12 entry points"; FreeLibrary(m); return r; }
    Gpu g; if (!g.Init()) { r.why = "no D3D12 device"; FreeLibrary(m); return r; }
    ParamMap initP, p;
    const std::wstring data = o.out.empty() ? L"." : o.out;
    NVSDK_NGX_Result res = NVSDK_NGX_Result_Fail; unsigned seh = 0;
    Seh([&] { res = init(0x1u, data.c_str(), g.dev.Get(), NVSDK_NGX_Version_API, &initP); }, seh);
    Log("  snippet: NVSDK_NGX_D3D12_Init_Ext 0x%08X seh 0x%X", unsigned(res), seh);
    if (seh || NVSDK_NGX_FAILED(res)) { r.why = "snippet init failed 0x" + std::to_string(unsigned(res)); FreeLibrary(m); return r; }
    if (populate) { seh = 0; Seh([&] { res = populate(&p); }, seh); Log("  snippet: PopulateParameters 0x%08X seh 0x%X (%zu values)", unsigned(res), seh, p.m.size()); }
    const UINT ow = o.w * o.scale, oh = o.h * o.scale;
    auto inTex = g.Texture(o.w, o.h, false), outTex = g.Texture(ow, oh, true);
    ComPtr<ID3D12Resource> staging; g.Upload(inTex.Get(), in, o.w, o.h, staging);
    g.Barrier(outTex.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    p.Set("CreationNodeMask", 1u); p.Set("VisibilityNodeMask", 1u);
    p.Set("Width", o.w); p.Set("Height", o.h); p.Set("OutWidth", ow); p.Set("OutHeight", oh);
    NVSDK_NGX_Handle* h = nullptr; seh = 0;
    Seh([&] { res = create(g.list.Get(), NVSDK_NGX_Feature_Reserved16, &p, &h); }, seh);
    Log("  snippet: CreateFeature(VSR) 0x%08X seh 0x%X", unsigned(res), seh);
    r.ran = !seh && NVSDK_NGX_SUCCEED(res) && h;
    std::vector<double> ms;
    if (r.ran) {
        p.Set("Input1", inTex.Get()); p.Set("Output", outTex.Get());
        p.Set("Rect.X", 0u); p.Set("Rect.Y", 0u); p.Set("Rect.W", o.w); p.Set("Rect.H", o.h);
        p.Set("OutRect.X", 0u); p.Set("OutRect.Y", 0u); p.Set("OutRect.W", ow); p.Set("OutRect.H", oh);
        p.Set("VSR.QualityLevel", o.quality);
        for (int i = 0; i < 21 && r.ran; ++i) {
            g.list->EndQuery(g.ts.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            seh = 0; Seh([&] { res = eval(g.list.Get(), h, &p, nullptr); }, seh);
            g.list->EndQuery(g.ts.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            g.list->ResolveQueryData(g.ts.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, g.tsRead.Get(), 0);
            if (seh || NVSDK_NGX_FAILED(res) || !g.Submit()) { r.ran = false; r.why = "EvaluateFeature failed 0x" + std::to_string(unsigned(res)) + " seh " + std::to_string(seh); break; }
            UINT64* t = nullptr; g.tsRead->Map(0, nullptr, reinterpret_cast<void**>(&t));
            if (i > 0) ms.push_back(double(t[1] - t[0]) * 1000.0 / double(g.tsFreq));
            g.tsRead->Unmap(0, nullptr);
        }
    } else if (r.why.empty()) r.why = "CreateFeature failed 0x" + std::to_string(unsigned(res)) + (seh ? " seh " + std::to_string(seh) : "");
    if (r.ran) {
        std::sort(ms.begin(), ms.end()); r.msMedian = ms.empty() ? -1 : ms[ms.size() / 2];
        Judge(r, o, in, g.Read(outTex.Get(), ow, oh, D3D12_RESOURCE_STATE_UNORDERED_ACCESS), g.Removed());
    }
    if (h) { seh = 0; Seh([&] { rel(h); }, seh); }
    if (shut) { seh = 0; Seh([&] { shut(g.dev.Get()); }, seh); }
    FreeLibrary(m);
    return r;
}

// --- route cuda: nvngx_vsr.dll's CUDA entry points with the given nvcuda.dll (ZLUDA). Buffers are linear RGBA8 device
// memory passed as "Input1" / "Output" (void*, the CUDA NGX convention for CUdeviceptr-backed images in the VFX SDK's
// NGX helper). The spike's open question is exactly whether ZLUDA's PTX translation runs these kernels; the log keeps
// every result for the report.
using CUresult = int; using CUdevice = int; using CUcontext = void*; using CUdeviceptr = unsigned long long;
RouteResult RunCuda(const Options& o, const std::vector<uint8_t>& in)
{
    RouteResult r; r.route = "cuda";
    if (o.cudaDll.empty()) { r.why = "no --cuda-dll given (the ZLUDA nvcuda.dll)"; return r; }
    HMODULE cu = LoadLibraryExW(o.cudaDll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!cu) { r.why = "cannot load " + Narrow(o.cudaDll); return r; }
    auto cuInit = reinterpret_cast<CUresult(__stdcall*)(unsigned)>(GetProcAddress(cu, "cuInit"));
    auto cuDeviceGet = reinterpret_cast<CUresult(__stdcall*)(CUdevice*, int)>(GetProcAddress(cu, "cuDeviceGet"));
    auto cuCtxCreate = reinterpret_cast<CUresult(__stdcall*)(CUcontext*, unsigned, CUdevice)>(GetProcAddress(cu, "cuCtxCreate_v2"));
    auto cuMemAlloc = reinterpret_cast<CUresult(__stdcall*)(CUdeviceptr*, size_t)>(GetProcAddress(cu, "cuMemAlloc_v2"));
    auto cuMemcpyHtoD = reinterpret_cast<CUresult(__stdcall*)(CUdeviceptr, const void*, size_t)>(GetProcAddress(cu, "cuMemcpyHtoD_v2"));
    auto cuMemcpyDtoH = reinterpret_cast<CUresult(__stdcall*)(void*, CUdeviceptr, size_t)>(GetProcAddress(cu, "cuMemcpyDtoH_v2"));
    auto cuCtxSynchronize = reinterpret_cast<CUresult(__stdcall*)()>(GetProcAddress(cu, "cuCtxSynchronize"));
    auto cuDeviceGetName = reinterpret_cast<CUresult(__stdcall*)(char*, int, CUdevice)>(GetProcAddress(cu, "cuDeviceGetName"));
    if (!cuInit || !cuDeviceGet || !cuCtxCreate || !cuMemAlloc || !cuMemcpyHtoD || !cuMemcpyDtoH || !cuCtxSynchronize) { r.why = "the CUDA driver lacks basic entry points"; return r; }
    CUdevice dev = 0; CUcontext ctx = nullptr; char name[128] = {};
    CUresult e = cuInit(0); if (!e) e = cuDeviceGet(&dev, 0); if (!e && cuDeviceGetName) cuDeviceGetName(name, 127, dev); if (!e) e = cuCtxCreate(&ctx, 0, dev);
    Log("  cuda: cuInit/cuDeviceGet/cuCtxCreate %d, device '%s'", e, name);
    if (e) { r.why = "CUDA context failed " + std::to_string(e); return r; }
    // nvngx_vsr resolves the CUDA driver through LoadLibrary("nvcuda.dll"): the ZLUDA DLL is already loaded under that
    // name when its file name is nvcuda.dll (ZLUDA's layout), so the snippet binds to it.
    HMODULE m = LoadLibraryExW((o.runtime + L"\\nvngx_vsr.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) { r.why = "cannot load nvngx_vsr.dll"; return r; }
    using CuInitFn = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
    using CuCreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
    using CuEvalFn = NVSDK_NGX_Result(NVSDK_CONV*)(const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
    auto init = reinterpret_cast<CuInitFn>(GetProcAddress(m, "NVSDK_NGX_CUDA_Init_Ext"));
    auto create = reinterpret_cast<CuCreateFn>(GetProcAddress(m, "NVSDK_NGX_CUDA_CreateFeature"));
    auto eval = reinterpret_cast<CuEvalFn>(GetProcAddress(m, "NVSDK_NGX_CUDA_EvaluateFeature"));
    if (!init || !create || !eval) { r.why = "nvngx_vsr.dll lacks the CUDA entry points"; return r; }
    ParamMap initP, p;
    const std::wstring data = o.out.empty() ? L"." : o.out;
    NVSDK_NGX_Result res = NVSDK_NGX_Result_Fail; unsigned seh = 0;
    Seh([&] { res = init(0x1u, data.c_str(), NVSDK_NGX_Version_API, &initP); }, seh);
    Log("  cuda: NVSDK_NGX_CUDA_Init_Ext 0x%08X seh 0x%X", unsigned(res), seh);
    if (seh || NVSDK_NGX_FAILED(res)) { r.why = "CUDA NGX init failed"; return r; }
    const UINT ow = o.w * o.scale, oh = o.h * o.scale;
    CUdeviceptr din = 0, dout = 0;
    cuMemAlloc(&din, size_t(o.w) * o.h * 4); cuMemAlloc(&dout, size_t(ow) * oh * 4);
    cuMemcpyHtoD(din, in.data(), in.size());
    p.Set("Width", o.w); p.Set("Height", o.h); p.Set("OutWidth", ow); p.Set("OutHeight", oh);
    NVSDK_NGX_Handle* h = nullptr; seh = 0;
    Seh([&] { res = create(&p, &h); }, seh);
    Log("  cuda: CreateFeature(VSR) 0x%08X seh 0x%X", unsigned(res), seh);
    r.ran = !seh && NVSDK_NGX_SUCCEED(res) && h;
    if (!r.ran) { r.why = "CreateFeature failed 0x" + std::to_string(unsigned(res)); return r; }
    p.Set("Input1", reinterpret_cast<void*>(din)); p.Set("Output", reinterpret_cast<void*>(dout));
    p.Set("Rect.W", o.w); p.Set("Rect.H", o.h); p.Set("OutRect.W", ow); p.Set("OutRect.H", oh); p.Set("VSR.QualityLevel", o.quality);
    std::vector<double> ms;
    for (int i = 0; i < 11 && r.ran; ++i) {
        LARGE_INTEGER a, b, f; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
        seh = 0; Seh([&] { res = eval(h, &p, nullptr); }, seh);
        cuCtxSynchronize(); QueryPerformanceCounter(&b);
        if (seh || NVSDK_NGX_FAILED(res)) { r.ran = false; r.why = "EvaluateFeature failed 0x" + std::to_string(unsigned(res)); break; }
        if (i > 0) ms.push_back(double(b.QuadPart - a.QuadPart) * 1000.0 / double(f.QuadPart));
    }
    if (r.ran) {
        std::sort(ms.begin(), ms.end()); r.msMedian = ms.empty() ? -1 : ms[ms.size() / 2];
        std::vector<uint8_t> out(size_t(ow) * oh * 4);
        cuMemcpyDtoH(out.data(), dout, out.size());
        Judge(r, o, in, out, S_OK);
    }
    return r;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    Options o;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::wstring k = argv[i], v = argv[i + 1];
        if (k == L"--runtime") o.runtime = v; else if (k == L"--route") o.route = v; else if (k == L"--cuda-dll") o.cudaDll = v;
        else if (k == L"--record") o.record = v; else if (k == L"--out") o.out = v;
        else if (k == L"--width") o.w = UINT(_wtoi(v.c_str())); else if (k == L"--height") o.h = UINT(_wtoi(v.c_str()));
        else if (k == L"--scale") o.scale = UINT(std::clamp(_wtoi(v.c_str()), 1, 4)); else if (k == L"--quality") o.quality = UINT(std::clamp(_wtoi(v.c_str()), 1, 4));
    }
    if (o.runtime.empty()) { fwprintf(stderr, L"usage: vsr_gate_probe --runtime <folder with nvngx_vsr.dll> [--route core|snippet|cuda|all] [--record Gtx16Vsr|ZludaVsr] [--out dir]\n"); return 2; }
    if (!o.out.empty()) CreateDirectoryW(o.out.c_str(), nullptr);
    Gpu probe; if (!probe.Init()) { Log("no D3D12 hardware adapter"); return 2; }
    const std::wstring driver = [&] { LARGE_INTEGER v{}; if (FAILED(probe.adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &v))) return std::wstring(L"unknown");
        wchar_t b[64]; swprintf_s(b, L"%u.%u.%u.%u", HIWORD(v.HighPart), LOWORD(v.HighPart), HIWORD(v.LowPart), LOWORD(v.LowPart)); return std::wstring(b); }();
    const std::wstring key = pmu::SelfTestKey(probe.desc.VendorId, probe.desc.DeviceId, driver);
    Log("vsr_gate_probe: adapter '%s' vendor 0x%04X device 0x%04X driver %s; GTX 16 %s, NVOFA %s; %ux%u -> x%u, quality %u",
        Narrow(probe.desc.Description).c_str(), probe.desc.VendorId, probe.desc.DeviceId, Narrow(driver).c_str(),
        pmu::IsGtx16(probe.desc.VendorId, probe.desc.DeviceId, probe.desc.Description) ? "yes" : "no",
        pmu::HasNvofa(probe.desc.VendorId, probe.desc.DeviceId, false, probe.desc.Description) ? "yes" : "no", o.w, o.h, o.scale, o.quality);
    probe = Gpu{};
    const auto in = MakeInput(o.w, o.h);
    std::vector<RouteResult> rs;
    if (o.route == L"all" || o.route == L"core") rs.push_back(RunCore(o, in));
    if (o.route == L"all" || o.route == L"snippet") rs.push_back(RunSnippet(o, in));
    if (o.route == L"cuda" || (o.route == L"all" && !o.cudaDll.empty())) rs.push_back(RunCuda(o, in));
    bool any = false; std::string passed;
    for (const auto& r : rs) {
        Log("RESULT route=%s ran=%d pass=%d psnr_vs_bicubic=%.2f sharpness=%.2f ms_median=%.3f why=%s", r.route.c_str(), r.ran, r.pass, r.psnr, r.sharp, r.msMedian, r.why.c_str());
        if (r.pass && !any) { any = true; passed = r.route; }
    }
    Log("OVERALL %s%s", any ? "PASS via route " : "FAIL", passed.c_str());
    if (!o.record.empty()) {
        const pmu::Unlock u = o.record == L"ZludaVsr" ? pmu::Unlock::ZludaVsr : pmu::Unlock::Gtx16Vsr;
        std::string why = any ? "route " + passed : (rs.empty() ? std::string("no route ran") : rs.back().why);
        for (const auto& r : rs) if (r.pass) { char b[96]; snprintf(b, sizeof(b), " (%.2f ms at %ux%u x%u)", r.msMedian, o.w, o.h, o.scale); why += b; break; }
        pmu::WriteSelfTest(u, key, any, std::wstring(why.begin(), why.end()));
        Log("recorded HKCU\\Software\\PushMPC\\SelfTest\\%s Key=%s Result=%s", Narrow(pmu::UnlockName(u)).c_str(), Narrow(key).c_str(), any ? "pass" : "fail");
    }
    return any ? 0 : 1;
}
