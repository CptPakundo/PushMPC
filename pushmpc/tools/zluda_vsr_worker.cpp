// pushcore_zluda_vsr_worker.exe: RTX Video Super Resolution on AMD RX 7000 / 9000 through ZLUDA, in its own process
// (pushmpc/include/pushmpc/zluda_vsr.h says why and how). pushcore starts it; it never runs inside the player.
//
//   pushcore_zluda_vsr_worker.exe <read pipe> <write pipe>
//       the stream mode: Init / Evaluate / Stop over the pipes (zluda_vsr.h protocol) on pushcore's shared textures
//   pushcore_zluda_vsr_worker.exe --self-test --luid <hex> --zluda <dir> --vsr <nvngx_vsr.dll> --cache <dir>
//                                 [--log <file>] [--width 960 --height 540 --scale 2] [--record <vendor:device:driver>]
//                                 [--regbase <HKCU path>]
//       the ZludaVsr self-test: the same chain on a synthetic picture in shared D3D12 textures of this process, each
//       parameter layout in turn until one passes the picture check (zluda_vsr.h Judge), then qualities 2..4 once so
//       ZLUDA's translation cache holds every kernel; with --record the result goes to HKCU\...\SelfTest\ZludaVsr.
//       Exit 0 = pass, 1 = fail, 2 = usage. Prints "RESULT pass=<0|1> layout=<n> ms=<median> psnr=.. sharp=.. why=..".
//   pushcore_zluda_vsr_worker.exe --coverage <nvngx_vsr.dll>
//       CPU only: the PTX index of the DLL and how many of its standalone cubins have a PTX twin (the evidence line).
//
// Built twice: the real worker, and with PUSHMPC_ZLUDA_VSR_STUB the stub that does the same protocol and GPU order with
// a CPU bicubic upscale instead of ZLUDA (pushmpc\tools\zluda_vsr_test.cpp runs it on WARP; never shipped).
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>
#ifndef PUSHMPC_ZLUDA_VSR_STUB
#include <cuda.h>
#undef cuGetProcAddress   // cuda.h maps it to _v2; both entry points are resolved by their own names here
#include "nvsdk_ngx.h"
#include "nvsdk_ngx_defs.h"
#include "nvsdk_ngx_params.h"
#endif
#include "pushmpc/unlocks.h"
#include "pushmpc/zluda_vsr.h"

using Microsoft::WRL::ComPtr;

namespace {

FILE* g_log = nullptr;
void Log(const char* fmt, ...)
{
    va_list a; va_start(a, fmt);
    char b[2048]; vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    if (g_log) { fprintf(g_log, "%s\n", b); fflush(g_log); }
    else { fprintf(stderr, "%s\n", b); fflush(stderr); }
}
std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0'); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::string Hr(const char* what, HRESULT hr) { char b[160]; snprintf(b, sizeof(b), "%s failed (hr 0x%08X)", what, unsigned(hr)); return b; }

// ------------------------------------------------------------------------------------------------ D3D12 side
struct D3D {
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> q;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence; UINT64 fv = 0; HANDLE ev = nullptr;
    DXGI_ADAPTER_DESC1 desc{};
    ~D3D() { if (ev) CloseHandle(ev); }
    std::string Open(uint64_t luid, bool allowSoftware, bool warp = false)
    {
        ComPtr<IDXGIFactory4> f; ComPtr<IDXGIAdapter1> a;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return "no DXGI";
        const LUID l{ DWORD(uint32_t(luid)), LONG(luid >> 32) };
        if (warp) { if (FAILED(f->EnumWarpAdapter(IID_PPV_ARGS(&a)))) return "no WARP adapter"; }
        else if (luid) { if (FAILED(f->EnumAdapterByLuid(l, IID_PPV_ARGS(&a)))) return "the adapter LUID is not present"; }
        else if (FAILED(f->EnumAdapters1(0, &a))) return "no adapter";
        a->GetDesc1(&desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && !allowSoftware) return "a software adapter";
        if (FAILED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) return "no D3D12 device";
        D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) || FAILED(dev->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(&alloc))) ||
            FAILED(dev->CreateCommandList(0, qd.Type, alloc.Get(), nullptr, IID_PPV_ARGS(&list))) || FAILED(list->Close()) ||
            FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return "D3D12 queue setup failed";
        ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return {};
    }
    bool Begin() { return SUCCEEDED(alloc->Reset()) && SUCCEEDED(list->Reset(alloc.Get(), nullptr)); }
    bool Submit(DWORD timeout = 60000)
    {
        if (FAILED(list->Close())) return false;
        ID3D12CommandList* l[] = { list.Get() }; q->ExecuteCommandLists(1, l);
        q->Signal(fence.Get(), ++fv);
        if (fence->GetCompletedValue() >= fv) return true;
        fence->SetEventOnCompletion(fv, ev);
        return WaitForSingleObject(ev, timeout) == WAIT_OBJECT_0;
    }
    bool Alive() const { return dev && dev->GetDeviceRemovedReason() == S_OK; }
    void Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
    {
        D3D12_RESOURCE_BARRIER br{}; br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; br.Transition.pResource = r;
        br.Transition.StateBefore = a; br.Transition.StateAfter = b; br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &br);
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
    // shared RGBA8 texture (UAV: CUDA imports it for surface stores), state COMMON
    ComPtr<ID3D12Resource> SharedTexture(UINT w, UINT h)
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count = 1; rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> r;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r));
        return r;
    }
    // CPU <-> texture (state COMMON before and after); the stub and the self-test only, never the stream path
    bool Upload(ID3D12Resource* tex, const std::vector<uint8_t>& px, UINT w, UINT h)
    {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 total = 0; const auto d = tex->GetDesc();
        dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
        auto staging = Buffer(total, D3D12_HEAP_TYPE_UPLOAD);
        uint8_t* m = nullptr; if (!staging || FAILED(staging->Map(0, nullptr, reinterpret_cast<void**>(&m)))) return false;
        for (UINT y = 0; y < h; ++y) memcpy(m + fp.Offset + size_t(y) * fp.Footprint.RowPitch, &px[size_t(y) * w * 4], size_t(w) * 4);
        staging->Unmap(0, nullptr);
        if (!Begin()) return false;
        D3D12_TEXTURE_COPY_LOCATION dst{ tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{ staging.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; src.PlacedFootprint = fp;
        Barrier(tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barrier(tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        return Submit();
    }
    std::vector<uint8_t> Read(ID3D12Resource* tex, UINT w, UINT h)
    {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 total = 0; const auto d = tex->GetDesc();
        dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
        auto rb = Buffer(total, D3D12_HEAP_TYPE_READBACK);
        std::vector<uint8_t> out;
        if (!rb || !Begin()) return out;
        D3D12_TEXTURE_COPY_LOCATION src{ tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }; src.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION dst{ rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }; dst.PlacedFootprint = fp;
        Barrier(tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barrier(tex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        if (!Submit()) return out;
        out.resize(size_t(w) * h * 4);
        uint8_t* m = nullptr; rb->Map(0, nullptr, reinterpret_cast<void**>(&m));
        for (UINT y = 0; y < h; ++y) memcpy(&out[size_t(y) * w * 4], m + fp.Offset + size_t(y) * fp.Footprint.RowPitch, size_t(w) * 4);
        rb->Unmap(0, nullptr);
        return out;
    }
};

#ifndef PUSHMPC_ZLUDA_VSR_STUB
// ------------------------------------------------------------------------------------------------ CUDA (ZLUDA) side
// The driver API from the gate (zluda\nvcuda.dll); types from NVIDIA's cuda.h, functions by name.
struct Cu {
#define CU_FN(name, ...) CUresult (CUDAAPI* name)(__VA_ARGS__) = nullptr
    CU_FN(cuInit, unsigned);
    CU_FN(cuDeviceGetCount, int*);
    CU_FN(cuDeviceGet, CUdevice*, int);
    CU_FN(cuDeviceGetName, char*, int, CUdevice);
    CU_FN(cuDeviceGetLuid, char*, unsigned*, CUdevice);
    CU_FN(cuDeviceGetAttribute, int*, CUdevice_attribute, CUdevice);
    CU_FN(cuCtxCreate_v2, CUcontext*, unsigned, CUdevice);
    CU_FN(cuCtxDestroy_v2, CUcontext);
    CU_FN(cuCtxSetCurrent, CUcontext);
    CU_FN(cuCtxSynchronize);
    CU_FN(cuStreamCreate, CUstream*, unsigned);
    CU_FN(cuStreamDestroy_v2, CUstream);
    CU_FN(cuStreamSynchronize, CUstream);
    CU_FN(cuImportExternalMemory, CUexternalMemory*, const CUDA_EXTERNAL_MEMORY_HANDLE_DESC*);
    CU_FN(cuExternalMemoryGetMappedMipmappedArray, CUmipmappedArray*, CUexternalMemory, const CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC*);
    CU_FN(cuMipmappedArrayGetLevel, CUarray*, CUmipmappedArray, unsigned);
    CU_FN(cuMipmappedArrayDestroy, CUmipmappedArray);
    CU_FN(cuDestroyExternalMemory, CUexternalMemory);
    CU_FN(cuTexObjectCreate, CUtexObject*, const CUDA_RESOURCE_DESC*, const CUDA_TEXTURE_DESC*, const CUDA_RESOURCE_VIEW_DESC*);
    CU_FN(cuTexObjectDestroy, CUtexObject);
    CU_FN(cuSurfObjectCreate, CUsurfObject*, const CUDA_RESOURCE_DESC*);
    CU_FN(cuSurfObjectDestroy, CUsurfObject);
    CU_FN(cuImportExternalSemaphore, CUexternalSemaphore*, const CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC*);
    CU_FN(cuDestroyExternalSemaphore, CUexternalSemaphore);
    CU_FN(cuWaitExternalSemaphoresAsync, const CUexternalSemaphore*, const CUDA_EXTERNAL_SEMAPHORE_WAIT_PARAMS*, unsigned, CUstream);
    CU_FN(cuSignalExternalSemaphoresAsync, const CUexternalSemaphore*, const CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS*, unsigned, CUstream);
    CU_FN(cuModuleLoadData, CUmodule*, const void*);
    CU_FN(cuModuleLoadDataEx, CUmodule*, const void*, unsigned, CUjit_option*, void**);
    CU_FN(cuGetProcAddress, const char*, void**, int, cuuint64_t);
    CU_FN(cuGetProcAddress_v2, const char*, void**, int, cuuint64_t, CUdriverProcAddressQueryResult*);
#undef CU_FN
    HMODULE gate = nullptr;
    std::string Load(HMODULE m)
    {
        gate = m;
        auto get = [&](auto& f, const char* n) { f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(GetProcAddress(m, n)); return f != nullptr; };
        const char* missing = nullptr;
#define NEED(n) if (!get(n, #n) && !missing) missing = #n
        NEED(cuInit); NEED(cuDeviceGetCount); NEED(cuDeviceGet); NEED(cuDeviceGetName); NEED(cuDeviceGetAttribute); NEED(cuCtxCreate_v2); NEED(cuCtxDestroy_v2);
        NEED(cuCtxSetCurrent); NEED(cuCtxSynchronize); NEED(cuStreamCreate); NEED(cuStreamDestroy_v2); NEED(cuStreamSynchronize);
        NEED(cuImportExternalMemory); NEED(cuExternalMemoryGetMappedMipmappedArray); NEED(cuMipmappedArrayGetLevel); NEED(cuMipmappedArrayDestroy);
        NEED(cuDestroyExternalMemory); NEED(cuTexObjectCreate); NEED(cuTexObjectDestroy); NEED(cuSurfObjectCreate); NEED(cuSurfObjectDestroy);
        NEED(cuImportExternalSemaphore); NEED(cuDestroyExternalSemaphore); NEED(cuWaitExternalSemaphoresAsync); NEED(cuSignalExternalSemaphoresAsync);
        NEED(cuModuleLoadData);
#undef NEED
        get(cuDeviceGetLuid, "cuDeviceGetLuid"); get(cuModuleLoadDataEx, "cuModuleLoadDataEx");
        get(cuGetProcAddress, "cuGetProcAddress"); get(cuGetProcAddress_v2, "cuGetProcAddress_v2");
        return missing ? std::string("the CUDA driver (ZLUDA) lacks ") + missing : std::string();
    }
};
Cu g_cu;

// ---- cubin -> PTX: nvngx_vsr.dll hands CUDA its standalone sm_80 cubins; ZLUDA runs PTX only, so each one is swapped for
// the PTX module of the same kernels (zluda_vsr.h PtxIndex). The DLL resolves the CUDA functions with GetProcAddress (VFX
// build) or imports them from nvcuda.dll (RTX Video SDK build): both are redirected below.
pmz::PtxIndex g_ptx;
long g_mapped = 0, g_unmapped = 0, g_passed = 0;
CUresult CUDAAPI HookLoadData(CUmodule* m, const void* image)
{
    if (pmz::IsCudaElf(image)) {
        if (const std::string* ptx = g_ptx.ForCubin(image)) {
            const CUresult r = g_cu.cuModuleLoadData(m, ptx->c_str());
            if (InterlockedIncrement(&g_mapped) <= 3 || r) Log("cubin -> PTX twin (%zu bytes): %d", ptx->size(), int(r));
            return r;
        }
        InterlockedIncrement(&g_unmapped);
        const auto* b = static_cast<const uint8_t*>(image);
        const auto k = pmz::ElfKernels(b, pmz::ElfSize(b));
        Log("cubin without a PTX twin (%s): passed to ZLUDA as it is", k.empty() ? "?" : k.front().c_str());
    } else {
        InterlockedIncrement(&g_passed);
    }
    return g_cu.cuModuleLoadData(m, image);
}
CUresult CUDAAPI HookLoadDataEx(CUmodule* m, const void* image, unsigned n, CUjit_option* o, void** v)
{
    if (pmz::IsCudaElf(image)) if (const std::string* ptx = g_ptx.ForCubin(image)) { InterlockedIncrement(&g_mapped); return g_cu.cuModuleLoadDataEx(m, ptx->c_str(), n, o, v); }
    return g_cu.cuModuleLoadDataEx(m, image, n, o, v);
}
void* Redirect(const char* name, void* real)
{
    if (!name || !real) return real;
    if (!strcmp(name, "cuModuleLoadData")) return reinterpret_cast<void*>(&HookLoadData);
    if (!strcmp(name, "cuModuleLoadDataEx") && g_cu.cuModuleLoadDataEx) return reinterpret_cast<void*>(&HookLoadDataEx);
    return real;
}
CUresult CUDAAPI HookGetProc(const char* s, void** p, int v, cuuint64_t f)
{
    const CUresult r = g_cu.cuGetProcAddress(s, p, v, f);
    if (r == CUDA_SUCCESS && p) *p = Redirect(s, *p);
    return r;
}
CUresult CUDAAPI HookGetProc2(const char* s, void** p, int v, cuuint64_t f, CUdriverProcAddressQueryResult* q)
{
    const CUresult r = g_cu.cuGetProcAddress_v2(s, p, v, f, q);
    if (r == CUDA_SUCCESS && p) *p = Redirect(s, *p);
    return r;
}
FARPROC WINAPI HookGetProcAddress(HMODULE m, LPCSTR name)
{
    FARPROC real = GetProcAddress(m, name);
    if (m != g_cu.gate || !real || !HIWORD(reinterpret_cast<uintptr_t>(name))) return real;
    if (!strcmp(name, "cuGetProcAddress") && g_cu.cuGetProcAddress) return reinterpret_cast<FARPROC>(&HookGetProc);
    if (!strcmp(name, "cuGetProcAddress_v2") && g_cu.cuGetProcAddress_v2) return reinterpret_cast<FARPROC>(&HookGetProc2);
    return reinterpret_cast<FARPROC>(Redirect(name, reinterpret_cast<void*>(real)));
}
// the IAT slot of dll!fn in module m (nullptr = not imported)
void** ImportSlot(HMODULE m, const char* dll, const char* fn)
{
    auto* base = reinterpret_cast<uint8_t*>(m);
    const auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp(reinterpret_cast<const char*>(base + d->Name), dll) != 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            const auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (!strcmp(reinterpret_cast<const char*>(ibn->Name), fn)) return reinterpret_cast<void**>(&slots->u1.Function);
        }
    }
    return nullptr;
}
bool Patch(void** slot, void* to)
{
    DWORD old = 0;
    if (!slot || !VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *slot = to;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}
int HookVsr(HMODULE vsr)
{
    int n = 0;
    n += Patch(ImportSlot(vsr, "KERNEL32.dll", "GetProcAddress"), reinterpret_cast<void*>(&HookGetProcAddress));
    n += Patch(ImportSlot(vsr, "nvcuda.dll", "cuModuleLoadData"), reinterpret_cast<void*>(&HookLoadData));
    if (g_cu.cuModuleLoadDataEx) n += Patch(ImportSlot(vsr, "nvcuda.dll", "cuModuleLoadDataEx"), reinterpret_cast<void*>(&HookLoadDataEx));
    if (g_cu.cuGetProcAddress) n += Patch(ImportSlot(vsr, "nvcuda.dll", "cuGetProcAddress"), reinterpret_cast<void*>(&HookGetProc));
    return n;
}

// NVSDK_NGX_Parameter of our own (when the snippet cannot hand one out): every Set kept by name
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
template <class F> bool Seh(F&& f, unsigned& code) { __try { f(); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { code = GetExceptionCode(); return false; } }

// RedDuke's NGX host (zluda\nvngx.dll): every NGX call has to come from a module named nvngx.dll
struct Host {
    using LoadFn = const char* (*)(const wchar_t*);
    using InitFn = NVSDK_NGX_Result (*)(unsigned long long, const wchar_t*, NVSDK_NGX_Version, unsigned long long, unsigned, void*);
    using PopulateFn = NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter**);
    using CreateFn = NVSDK_NGX_Result (*)(NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
    using EvalFn = NVSDK_NGX_Result (*)(NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*);
    using ReleaseFn = NVSDK_NGX_Result (*)(NVSDK_NGX_Handle*);
    using ShutdownFn = NVSDK_NGX_Result (*)();
    LoadFn load = nullptr; InitFn init = nullptr; PopulateFn populate = nullptr; CreateFn create = nullptr; EvalFn eval = nullptr;
    ReleaseFn release = nullptr; ShutdownFn shutdown = nullptr;
    std::string Bind(HMODULE m)
    {
        load = reinterpret_cast<LoadFn>(GetProcAddress(m, "ngxrt_load"));
        init = reinterpret_cast<InitFn>(GetProcAddress(m, "ngxrt_init"));
        populate = reinterpret_cast<PopulateFn>(GetProcAddress(m, "ngxrt_populate_parameters"));
        create = reinterpret_cast<CreateFn>(GetProcAddress(m, "ngxrt_create_feature"));
        eval = reinterpret_cast<EvalFn>(GetProcAddress(m, "ngxrt_evaluate"));
        release = reinterpret_cast<ReleaseFn>(GetProcAddress(m, "ngxrt_release_feature"));
        shutdown = reinterpret_cast<ShutdownFn>(GetProcAddress(m, "ngxrt_shutdown"));
        if (!load || !init || !create || !eval || !release) return "the ZLUDA NGX host (zluda\\nvngx.dll) lacks the ngxrt_* exports (an older RedDuke runtime)";
        return {};
    }
};
#endif

// ------------------------------------------------------------------------------------------------ the VSR engine
struct Engine {
    pmz::Init init{};
    D3D d3d;
    ComPtr<ID3D12Resource> in, out;
    ComPtr<ID3D12Fence> producer, done;
    std::string error;
    bool ready = false;
#ifdef PUSHMPC_ZLUDA_VSR_STUB
    uint64_t evaluations = 0;
#else
    HMODULE vsrModule = nullptr, hostModule = nullptr;
    Host host;
    CUcontext ctx = nullptr; CUstream stream = nullptr;
    CUexternalMemory mem[2] = {}; CUmipmappedArray mip[2] = {}; CUarray arr[2] = {};
    CUtexObject tex = 0; CUsurfObject surf = 0;
    CUexternalSemaphore semProducer = nullptr, semDone = nullptr;
    NVSDK_NGX_Parameter* params = nullptr; ParamMap ownParams;
    NVSDK_NGX_Handle* feature = nullptr;
    pmz::CudaImage imgIn{}, imgOut{};
    bool ngxUp = false;
    static std::string CuErr(const char* what, CUresult r) { char b[160]; snprintf(b, sizeof(b), "%s failed (CUDA %d)", what, int(r)); return b; }
#endif

    bool Fail(const std::string& e) { error = e; Log("error: %s", e.c_str()); return false; }

    // D3D12 objects from the handles (the stream mode: pushcore's; the self-test: this process's own)
    bool OpenShared()
    {
        const HANDLE h[4] = { HANDLE(uintptr_t(init.handles[0])), HANDLE(uintptr_t(init.handles[1])), HANDLE(uintptr_t(init.handles[2])), HANDLE(uintptr_t(init.handles[3])) };
        HRESULT hr;
        if (FAILED(hr = d3d.dev->OpenSharedHandle(h[0], IID_PPV_ARGS(&in))) || FAILED(hr = d3d.dev->OpenSharedHandle(h[1], IID_PPV_ARGS(&out)))) return Fail(Hr("opening the shared textures", hr));
        if (FAILED(hr = d3d.dev->OpenSharedHandle(h[2], IID_PPV_ARGS(&producer))) || FAILED(hr = d3d.dev->OpenSharedHandle(h[3], IID_PPV_ARGS(&done)))) return Fail(Hr("opening the shared fences", hr));
        const auto di = in->GetDesc(), dout = out->GetDesc();
        if (di.Width != init.inW || di.Height != init.inH || dout.Width != init.outW || dout.Height != init.outH ||
            di.Format != DXGI_FORMAT_R8G8B8A8_UNORM || dout.Format != DXGI_FORMAT_R8G8B8A8_UNORM) return Fail("the shared textures do not match the stream's sizes / RGBA8");
        return true;
    }

#ifdef PUSHMPC_ZLUDA_VSR_STUB
    // ---- stub: the same GPU order (wait producer, write output, signal done) with a CPU bicubic upscale
    bool Load(bool) { return true; }
    bool Create(uint32_t layout)
    {
        wchar_t v[64] = {};
        if (GetEnvironmentVariableW(L"PUSHMPC_ZLUDA_STUB_REFUSE", v, 64)) return Fail("stub: CreateFeature refused (" + Narrow(v) + ")");
        if (GetEnvironmentVariableW(L"PUSHMPC_ZLUDA_STUB_LAYOUT", v, 64) && uint32_t(_wtoi(v)) != layout) return Fail("stub: this layout is refused");
        ready = true; return true;
    }
    void ReleaseFeature() { ready = false; }
    bool Evaluate(uint64_t seq, float& ms)
    {
        const auto t0 = std::chrono::steady_clock::now();
        wchar_t v[32] = {};
        ++evaluations;
        if (GetEnvironmentVariableW(L"PUSHMPC_ZLUDA_STUB_EXIT_AFTER", v, 32) && evaluations > uint64_t(_wtoi(v))) { Log("stub: exiting after %llu evaluations", evaluations - 1); ExitProcess(42); }
        if (GetEnvironmentVariableW(L"PUSHMPC_ZLUDA_STUB_HANG_AFTER", v, 32) && evaluations > uint64_t(_wtoi(v))) { Log("stub: hanging"); Sleep(INFINITE); }
        if (producer->GetCompletedValue() < seq) {
            HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            producer->SetEventOnCompletion(seq, e);
            const DWORD w = WaitForSingleObject(e, 10000); CloseHandle(e);
            if (w != WAIT_OBJECT_0) return Fail("stub: the producer fence never reached this frame");
        }
        const auto src = d3d.Read(in.Get(), init.inW, init.inH);
        if (src.empty()) return Fail("stub: input read failed");
        std::vector<uint8_t> o;
        if (GetEnvironmentVariableW(L"PUSHMPC_ZLUDA_STUB_BLANK", v, 32)) o.assign(size_t(init.outW) * init.outH * 4, 0);
        else o = pmz::Bicubic(src, init.inW, init.inH, init.outW, init.outH);
        if (!d3d.Upload(out.Get(), o, init.outW, init.outH)) return Fail("stub: output write failed");
        if (FAILED(d3d.q->Signal(done.Get(), seq))) return Fail("stub: done fence signal failed");
        ms = float(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        return true;
    }
    void Shutdown() { ready = false; }
#else
    // ---- the real chain
    bool Load(bool requireAmd)
    {
        if (requireAmd && d3d.desc.VendorId != 0x1002) return Fail("not an AMD adapter");
        const std::wstring zluda = init.zluda.data(), vsr = init.vsr.data(), cache = init.cache.data();
        CreateDirectoryW(cache.c_str(), nullptr);
        const std::wstring tmp = cache + L"\\tmp"; CreateDirectoryW(tmp.c_str(), nullptr);
        SetEnvironmentVariableW(L"TEMP", tmp.c_str()); SetEnvironmentVariableW(L"TMP", tmp.c_str());
        // our own translation cache (never the NR worker's packaged one); translation allowed: VSR's kernels are not in
        // any shipped cache, the self-test fills this one (it can take many minutes once per GPU and driver)
        SetEnvironmentVariableW(L"ZLUDA_CACHE_DIR", cache.c_str());
        SetEnvironmentVariableW(L"DLSS5NR_ALLOW_TRANSLATION", L"1");
        const std::wstring hip = pmz::FindHip();
        if (hip.empty()) return Fail("AMD's HIP runtime (amdhip64_7.dll) is not installed");
        if (!LoadLibraryExW(hip.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)) return Fail("AMD HIP load failed, win32 " + std::to_string(GetLastError()));
        for (const wchar_t* f : { L"nvapi64.dll", L"zluda_real.dll" })
            if (!LoadLibraryExW((zluda + L"\\" + f).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) return Fail("cannot load ZLUDA's " + Narrow(f) + ", win32 " + std::to_string(GetLastError()));
        HMODULE gate = LoadLibraryExW((zluda + L"\\nvcuda.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!gate) return Fail("cannot load ZLUDA's nvcuda.dll, win32 " + std::to_string(GetLastError()));
        if (auto e = g_cu.Load(gate); !e.empty()) return Fail(e);
        CUresult r = g_cu.cuInit(0);
        if (r) return Fail(CuErr("cuInit", r));
        int count = 0; g_cu.cuDeviceGetCount(&count);
        CUdevice dev = -1;
        for (int i = 0; i < count; ++i) {
            CUdevice d = 0; char l[8] = {}; unsigned node = 0;
            if (g_cu.cuDeviceGet(&d, i) == CUDA_SUCCESS && g_cu.cuDeviceGetLuid && g_cu.cuDeviceGetLuid(l, &node, d) == CUDA_SUCCESS) {
                uint64_t v = 0; memcpy(&v, l, 8);
                if (v == init.adapterLuid) { dev = d; break; }
            }
        }
        if (dev < 0 && count == 1) g_cu.cuDeviceGet(&dev, 0);
        if (dev < 0) return Fail("ZLUDA does not see this adapter (" + std::to_string(count) + " HIP devices, no LUID match)");
        char name[128] = {}; g_cu.cuDeviceGetName(name, 127, dev);
        int major = 0, minor = 0;
        g_cu.cuDeviceGetAttribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, dev); g_cu.cuDeviceGetAttribute(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, dev);
        Log("ZLUDA device '%s', reported compute capability %d.%d", name, major, minor);
        if ((r = g_cu.cuCtxCreate_v2(&ctx, 0, dev)) || (r = g_cu.cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING))) return Fail(CuErr("CUDA context", r));
        // the PTX twins of the DLL's cubins
        {
            HANDLE f = CreateFileW(vsr.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (f == INVALID_HANDLE_VALUE) return Fail("cannot read " + Narrow(vsr));
            LARGE_INTEGER sz{}; GetFileSizeEx(f, &sz);
            std::vector<uint8_t> img(size_t(sz.QuadPart)); DWORD got = 0;
            const bool ok = ReadFile(f, img.data(), DWORD(img.size()), &got, nullptr) && got == img.size(); CloseHandle(f);
            if (!ok || !g_ptx.Build(img.data(), img.size())) return Fail("nvngx_vsr.dll holds no PTX this worker can read");
            const auto cov = pmz::Coverage(img.data(), img.size(), g_ptx);
            const size_t c80 = cov.cubins.count(80) ? cov.cubins.at(80) : 0, m80 = cov.mapped.count(80) ? cov.mapped.at(80) : 0;
            Log("nvngx_vsr.dll: %zu PTX modules (%zu LZ4, %zu unreadable), standalone sm_80 cubins %zu, with a PTX twin %zu", g_ptx.Modules(), g_ptx.stats().compressed, g_ptx.stats().bad, c80, m80);
        }
        vsrModule = LoadLibraryExW(vsr.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!vsrModule) return Fail("cannot load " + Narrow(vsr) + ", win32 " + std::to_string(GetLastError()));
        Log("hooks in nvngx_vsr.dll: %d", HookVsr(vsrModule));
        hostModule = LoadLibraryExW((zluda + L"\\nvngx.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!hostModule) return Fail("cannot load ZLUDA's NGX host nvngx.dll, win32 " + std::to_string(GetLastError()));
        if (auto e = host.Bind(hostModule); !e.empty()) return Fail(e);
        unsigned seh = 0; const char* lr = nullptr;
        if (!Seh([&] { lr = host.load(vsr.c_str()); }, seh) || lr) return Fail(std::string("the NGX host did not load nvngx_vsr.dll: ") + (lr ? lr : ("exception " + std::to_string(seh)).c_str()));
        g_cu.cuCtxSetCurrent(ctx);
        NVSDK_NGX_Result res = NVSDK_NGX_Result_Fail;
        if (!Seh([&] { res = host.init(pmz::kAppId, cache.c_str(), NVSDK_NGX_Version(pmz::kNgxVersionApi), 0, 0, ctx); }, seh) || NVSDK_NGX_FAILED(res)) {
            char b[96]; snprintf(b, sizeof(b), "NVSDK_NGX_CUDA_Init_Ext1 0x%08X (exception 0x%X)", unsigned(res), seh); return Fail(b);
        }
        ngxUp = true;
        if (host.populate && Seh([&] { res = host.populate(&params); }, seh) && NVSDK_NGX_SUCCEED(res) && params) Log("parameters from the snippet");
        else { params = &ownParams; Log("parameters: our own map (the snippet gave none: 0x%08X)", unsigned(res)); }
        // the shared textures and fences into CUDA
        const D3D12_RESOURCE_DESC rd[2] = { in->GetDesc(), out->GetDesc() };
        const HANDLE th[2] = { HANDLE(uintptr_t(init.handles[0])), HANDLE(uintptr_t(init.handles[1])) };
        for (int i = 0; i < 2; ++i) {
            const auto info = d3d.dev->GetResourceAllocationInfo(0, 1, &rd[i]);
            CUDA_EXTERNAL_MEMORY_HANDLE_DESC md{}; md.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE; md.handle.win32.handle = th[i];
            md.size = info.SizeInBytes; md.flags = CUDA_EXTERNAL_MEMORY_DEDICATED;
            if ((r = g_cu.cuImportExternalMemory(&mem[i], &md))) return Fail(CuErr("cuImportExternalMemory", r));
            CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC ad{}; ad.offset = 0; ad.numLevels = 1;
            ad.arrayDesc.Width = rd[i].Width; ad.arrayDesc.Height = rd[i].Height; ad.arrayDesc.Depth = 0;
            ad.arrayDesc.Format = CU_AD_FORMAT_UNSIGNED_INT8; ad.arrayDesc.NumChannels = 4; ad.arrayDesc.Flags = CUDA_ARRAY3D_SURFACE_LDST;
            if ((r = g_cu.cuExternalMemoryGetMappedMipmappedArray(&mip[i], mem[i], &ad))) return Fail(CuErr("cuExternalMemoryGetMappedMipmappedArray", r));
            if ((r = g_cu.cuMipmappedArrayGetLevel(&arr[i], mip[i], 0))) return Fail(CuErr("cuMipmappedArrayGetLevel", r));
        }
        CUDA_RESOURCE_DESC res0{}; res0.resType = CU_RESOURCE_TYPE_ARRAY; res0.res.array.hArray = arr[0];
        CUDA_TEXTURE_DESC td{}; td.addressMode[0] = td.addressMode[1] = td.addressMode[2] = CU_TR_ADDRESS_MODE_CLAMP; td.filterMode = CU_TR_FILTER_MODE_POINT;
        if ((r = g_cu.cuTexObjectCreate(&tex, &res0, &td, nullptr))) return Fail(CuErr("cuTexObjectCreate", r));
        CUDA_RESOURCE_DESC res1{}; res1.resType = CU_RESOURCE_TYPE_ARRAY; res1.res.array.hArray = arr[1];
        if ((r = g_cu.cuSurfObjectCreate(&surf, &res1))) return Fail(CuErr("cuSurfObjectCreate", r));
        imgIn = { tex, init.inW, init.inH, init.inW * 4, uint32_t(DXGI_FORMAT_R8G8B8A8_UNORM) };
        imgOut = { surf, init.outW, init.outH, init.outW * 4, uint32_t(DXGI_FORMAT_R8G8B8A8_UNORM) };
        const HANDLE fh[2] = { HANDLE(uintptr_t(init.handles[2])), HANDLE(uintptr_t(init.handles[3])) };
        CUexternalSemaphore* sems[2] = { &semProducer, &semDone };
        for (int i = 0; i < 2; ++i) {
            CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC sd{}; sd.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE; sd.handle.win32.handle = fh[i];
            if ((r = g_cu.cuImportExternalSemaphore(sems[i], &sd))) return Fail(CuErr("cuImportExternalSemaphore", r));
        }
        return true;
    }
    bool Create(uint32_t layout)
    {
        ReleaseFeature();
        params->Reset();
        pmz::PackCreate(*params, init.inW, init.inH, init.outW, init.outH, init.quality, layout, ctx, stream);
        NVSDK_NGX_Result res = NVSDK_NGX_Result_Fail; unsigned seh = 0;
        g_cu.cuCtxSetCurrent(ctx);
        const bool ok = Seh([&] { res = host.create(NVSDK_NGX_Feature(pmz::kFeatureVsr), params, &feature); }, seh);
        Log("CreateFeature(VSR %ux%u -> %ux%u, quality %u, layout %u): 0x%08X exception 0x%X; cubins swapped for PTX %ld, without a twin %ld, other modules %ld",
            init.inW, init.inH, init.outW, init.outH, init.quality, layout, unsigned(res), seh, g_mapped, g_unmapped, g_passed);
        if (!ok || NVSDK_NGX_FAILED(res) || !feature) { feature = nullptr; char b[96]; snprintf(b, sizeof(b), "CreateFeature 0x%08X (exception 0x%X)", unsigned(res), seh); return Fail(b); }
        init.layout = layout;
        ready = true;
        return true;
    }
    void ReleaseFeature()
    {
        if (feature) { unsigned seh = 0; Seh([&] { host.release(feature); }, seh); feature = nullptr; }
        ready = false;
    }
    bool Evaluate(uint64_t seq, float& ms)
    {
        const auto t0 = std::chrono::steady_clock::now();
        CUresult r;
        CUDA_EXTERNAL_SEMAPHORE_WAIT_PARAMS wp{}; wp.params.fence.value = seq;
        if ((r = g_cu.cuWaitExternalSemaphoresAsync(&semProducer, &wp, 1, stream)) || (r = g_cu.cuStreamSynchronize(stream))) return Fail(CuErr("waiting for the producer fence", r));
        params->Reset();
        pmz::PackEvaluate(*params, imgIn, imgOut, init.quality, init.layout);
        NVSDK_NGX_Result res = NVSDK_NGX_Result_Fail; unsigned seh = 0;
        if (!Seh([&] { res = host.eval(feature, params); }, seh) || NVSDK_NGX_FAILED(res)) { char b[96]; snprintf(b, sizeof(b), "EvaluateFeature 0x%08X (exception 0x%X)", unsigned(res), seh); return Fail(b); }
        if ((r = g_cu.cuCtxSynchronize())) return Fail(CuErr("VSR on the GPU", r));
        CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS sp{}; sp.params.fence.value = seq;
        if ((r = g_cu.cuSignalExternalSemaphoresAsync(&semDone, &sp, 1, stream)) || (r = g_cu.cuStreamSynchronize(stream))) return Fail(CuErr("signalling the done fence", r));
        ms = float(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        return true;
    }
    void Shutdown()
    {
        ReleaseFeature();
        if (ngxUp && host.shutdown) { unsigned seh = 0; Seh([&] { host.shutdown(); }, seh); ngxUp = false; }
        if (semProducer) g_cu.cuDestroyExternalSemaphore(semProducer);
        if (semDone) g_cu.cuDestroyExternalSemaphore(semDone);
        if (surf) g_cu.cuSurfObjectDestroy(surf);
        if (tex) g_cu.cuTexObjectDestroy(tex);
        for (int i = 0; i < 2; ++i) { if (mip[i]) g_cu.cuMipmappedArrayDestroy(mip[i]); if (mem[i]) g_cu.cuDestroyExternalMemory(mem[i]); }
        if (stream) g_cu.cuStreamDestroy_v2(stream);
        if (ctx) g_cu.cuCtxDestroy_v2(ctx);
        semProducer = semDone = nullptr; surf = tex = 0; stream = nullptr; ctx = nullptr;
    }
#endif
};

// ------------------------------------------------------------------------------------------------ stream mode
bool ReadAll(HANDLE p, void* d, DWORD n) { DWORD o = 0; while (o < n) { DWORD k = 0; if (!ReadFile(p, static_cast<char*>(d) + o, n - o, &k, nullptr) || !k) return false; o += k; } return true; }
bool WriteAll(HANDLE p, const void* d, DWORD n) { DWORD k = 0; return WriteFile(p, d, n, &k, nullptr) && k == n; }

int StreamMain(HANDLE input, HANDLE output)
{
    Engine e;
    pmz::Init init{};
    if (!ReadAll(input, &init, sizeof(init))) return 3;
    auto reply = [&](uint64_t seq, bool ok, const std::string& why, float ms = -1) { const auto r = pmz::MakeReply(seq, ok ? 0 : 1, why, ms); return WriteAll(output, &r, sizeof(r)); };
    if (const auto bad = pmz::ValidateInit(init); !bad.empty()) { reply(0, false, "invalid initialisation: " + bad); return 4; }
    if (_wfopen_s(&g_log, init.log.data(), L"wb")) g_log = nullptr;
    e.init = init;
    Log("pushcore_zluda_vsr_worker%s: %ux%u -> %ux%u quality %u layout %u",
#ifdef PUSHMPC_ZLUDA_VSR_STUB
        " (STUB: CPU bicubic, no ZLUDA)",
#else
        "",
#endif
        init.inW, init.inH, init.outW, init.outH, init.quality, init.layout);
#ifdef PUSHMPC_ZLUDA_VSR_STUB
    const bool software = true;
#else
    const bool software = false;
#endif
    if (auto err = e.d3d.Open(init.adapterLuid, software); !err.empty()) { reply(0, false, err); return 4; }
    if (!e.OpenShared() || !e.Load(true) || !e.Create(init.layout)) { reply(0, false, e.error); e.Shutdown(); return 4; }
    if (!reply(0, true, "ready")) return 5;
    uint64_t last = 0;
    for (;;) {
        pmz::Header h;
        if (!ReadAll(input, &h, sizeof(h))) { e.Shutdown(); return 5; }
        if (pmz::ValidHeader(h, pmz::Op::Stop, sizeof(h))) { Log("stop after %llu frames", last); e.Shutdown(); return 0; }
        if (!pmz::ValidHeader(h, pmz::Op::Evaluate, pmz::Bytes<pmz::Evaluate>()) || h.sequence != last + 1) { reply(h.sequence, false, "out-of-order or malformed message"); e.Shutdown(); return 6; }
        pmz::Evaluate ev; ev.h = h;
        if (sizeof(ev) > sizeof(h) && !ReadAll(input, reinterpret_cast<char*>(&ev) + sizeof(h), DWORD(sizeof(ev) - sizeof(h)))) { e.Shutdown(); return 7; }
        float ms = -1;
        const bool ok = e.Evaluate(h.sequence, ms);
        if (!reply(h.sequence, ok, ok ? std::string() : e.error, ms) || !ok) { e.Shutdown(); return 8; }
        last = h.sequence;
    }
}

// ------------------------------------------------------------------------------------------------ self-test mode
struct Args { std::map<std::wstring, std::wstring> kv; std::wstring get(const wchar_t* k, const wchar_t* d = L"") const { auto it = kv.find(k); return it == kv.end() ? d : it->second; } };

int SelfTestMain(const Args& a)
{
    const std::wstring logPath = a.get(L"--log");
    if (!logPath.empty() && _wfopen_s(&g_log, logPath.c_str(), L"wb")) g_log = nullptr;
    const uint32_t w = std::clamp<uint32_t>(uint32_t(_wtoi(a.get(L"--width", L"960").c_str())), 64, 4096);
    const uint32_t h = std::clamp<uint32_t>(uint32_t(_wtoi(a.get(L"--height", L"540").c_str())), 64, 2304);
    const uint32_t scale = std::clamp<uint32_t>(uint32_t(_wtoi(a.get(L"--scale", L"2").c_str())), 1, 4);
    const uint64_t luid = _wcstoui64(a.get(L"--luid", L"0").c_str(), nullptr, 16);
    const std::wstring record = a.get(L"--record");
    if (!a.get(L"--regbase").empty()) pmu::RegBase() = a.get(L"--regbase");
    std::string why; bool pass = false; uint32_t passLayout = 0; double msMedian = -1; pmz::Verdict best;
    auto finish = [&](int code) {
        char line[1024];
        snprintf(line, sizeof(line), "RESULT pass=%d layout=%u ms=%.3f psnr=%.2f sharp=%.2f size=%ux%u x%u why=%s", pass ? 1 : 0, passLayout, msMedian, best.psnr, best.sharp, w, h, scale, why.c_str());
        printf("%s\n", line); fflush(stdout); Log("%s", line);
        if (!record.empty()) {
            char reason[400];
            if (pass) snprintf(reason, sizeof(reason), "pass layout=%u ms=%.3f at %ux%u x%u (%s)", passLayout, msMedian, w, h, scale, why.c_str());
            else snprintf(reason, sizeof(reason), "%s", why.c_str());
            const std::string rs = reason;
            pmu::WriteSelfTest(pmu::Unlock::ZludaVsr, record, pass, std::wstring(rs.begin(), rs.end()));
            Log("recorded SelfTest\\ZludaVsr Key=%s Result=%s", Narrow(record).c_str(), pass ? "pass" : "fail");
        }
        return code;
    };
    Engine e;
    e.init = pmz::MakeInit();
    e.init.adapterLuid = luid; e.init.inW = w; e.init.inH = h; e.init.outW = w * scale; e.init.outH = h * scale; e.init.quality = 1;
    pmz::PutPath(e.init.zluda, a.get(L"--zluda")); pmz::PutPath(e.init.vsr, a.get(L"--vsr")); pmz::PutPath(e.init.cache, a.get(L"--cache")); pmz::PutPath(e.init.log, logPath.empty() ? L"-" : logPath);
#ifdef PUSHMPC_ZLUDA_VSR_STUB
    const bool software = true;
#else
    const bool software = false;
    if (a.get(L"--zluda").empty() || a.get(L"--vsr").empty() || a.get(L"--cache").empty()) { why = "usage: --zluda, --vsr and --cache are needed"; return finish(2); }
#endif
    if (auto err = e.d3d.Open(luid, software, !a.get(L"--warp").empty()); !err.empty()) { why = err; return finish(1); }
    Log("self-test on '%s' (vendor 0x%04X device 0x%04X), %ux%u -> x%u", Narrow(e.d3d.desc.Description).c_str(), e.d3d.desc.VendorId, e.d3d.desc.DeviceId, w, h, scale);
    // this process's own shared textures and fences (the stream mode gets pushcore's)
    auto texIn = e.d3d.SharedTexture(w, h), texOut = e.d3d.SharedTexture(w * scale, h * scale);
    ComPtr<ID3D12Fence> fp, fd;
    if (!texIn || !texOut || FAILED(e.d3d.dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fp))) || FAILED(e.d3d.dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fd)))) { why = "shared D3D12 resources could not be created"; return finish(1); }
    ID3D12DeviceChild* objs[4] = { texIn.Get(), texOut.Get(), fp.Get(), fd.Get() };
    for (int i = 0; i < 4; ++i) {
        HANDLE sh = nullptr;
        if (FAILED(e.d3d.dev->CreateSharedHandle(objs[i], nullptr, GENERIC_ALL, nullptr, &sh))) { why = "CreateSharedHandle failed"; return finish(1); }
        e.init.handles[i] = uintptr_t(sh);
    }
    const auto input = pmz::MakeInput(w, h);
    if (!e.d3d.Upload(texIn.Get(), input, w, h)) { why = "the test picture could not be uploaded"; return finish(1); }
    if (!e.OpenShared() || !e.Load(a.get(L"--any-vendor").empty())) { why = e.error; return finish(1); }
    uint64_t seq = 0;
    // one evaluate: the producer fence from this queue, the worker side, then the read back
    auto evalOnce = [&](float& ms, std::vector<uint8_t>* outPx) -> bool {
        ++seq;
        if (FAILED(e.d3d.q->Signal(fp.Get(), seq))) { e.error = "producer signal failed"; return false; }
        if (!e.Evaluate(seq, ms)) return false;
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fd->SetEventOnCompletion(seq, ev); const DWORD wr = WaitForSingleObject(ev, 30000); CloseHandle(ev);
        if (wr != WAIT_OBJECT_0) { e.error = "the done fence was not signalled"; return false; }
        if (outPx) *outPx = e.d3d.Read(texOut.Get(), w * scale, h * scale);
        return true;
    };
    std::string tried;
    for (const uint32_t layout : pmz::kLayoutOrder) {
        if (!e.Create(layout)) { tried += "layout " + std::to_string(layout) + ": " + e.error + "; "; continue; }
        float ms = -1; std::vector<uint8_t> out;
        // the first evaluate may translate the kernels (ZLUDA's cache fills here: minutes on a new GPU / driver)
        const auto t0 = std::chrono::steady_clock::now();
        if (!evalOnce(ms, &out)) { tried += "layout " + std::to_string(layout) + ": " + e.error + "; "; e.ReleaseFeature(); continue; }
        Log("layout %u: first evaluate %.0f ms (kernel translation included)", layout, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        const pmz::Verdict v = pmz::Judge(input, w, h, out, w * scale, h * scale, e.d3d.Alive());
        Log("layout %u: %s", layout, v.why.c_str());
        if (!v.pass) { tried += "layout " + std::to_string(layout) + ": " + v.why + "; "; if (!e.d3d.Alive()) break; e.ReleaseFeature(); continue; }
        std::vector<double> times;
        for (int i = 0; i < 10; ++i) { float t = -1; if (!evalOnce(t, nullptr)) break; times.push_back(t); }
        std::sort(times.begin(), times.end());
        msMedian = times.empty() ? ms : times[times.size() / 2];
        pass = true; passLayout = layout; best = v; why = v.why;
        break;
    }
    if (!pass) { why = tried.empty() ? "no layout ran" : tried; e.Shutdown(); return finish(1); }
    // warm ZLUDA's cache for the other quality levels (their kernels differ); a failure here is logged, not fatal
    for (uint32_t q = 2; q <= 4; ++q) {
        e.init.quality = q; float t = -1;
        const bool ok = e.Create(passLayout) && evalOnce(t, nullptr);
        Log("warm-up quality %u: %s", q, ok ? "ok" : e.error.c_str());
    }
    e.Shutdown();
    return finish(0);
}

int CoverageMain(const std::wstring& dll)
{
    HANDLE f = CreateFileW(dll.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { printf("COVERAGE error cannot read the file\n"); return 1; }
    LARGE_INTEGER sz{}; GetFileSizeEx(f, &sz);
    std::vector<uint8_t> img(size_t(sz.QuadPart)); DWORD got = 0;
    ReadFile(f, img.data(), DWORD(img.size()), &got, nullptr); CloseHandle(f);
    pmz::PtxIndex idx;
    idx.Build(img.data(), img.size());
    const auto c = pmz::Coverage(img.data(), img.size(), idx);
    printf("COVERAGE fatbins=%zu ptx=%zu lz4=%zu unreadable=%zu elf_in_fatbins=%zu\n", idx.stats().fatbins, idx.Modules(), idx.stats().compressed, idx.stats().bad, idx.stats().elfInFatbin);
    for (const auto& [sm, n] : c.cubins) printf("COVERAGE sm_%d cubins=%zu with_ptx_twin=%zu\n", sm, n, c.mapped.count(sm) ? c.mapped.at(sm) : 0);
    for (const auto& u : c.unmapped) printf("COVERAGE no_twin %s\n", u.c_str());
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc >= 2 && !wcscmp(argv[1], L"--self-test")) {
        Args a;
        for (int i = 2; i < argc; ++i) {
            const std::wstring k = argv[i];
            if (k == L"--any-vendor") { a.kv[k] = L"1"; continue; }
            if (i + 1 < argc) a.kv[k] = argv[++i];
        }
        return SelfTestMain(a);
    }
    if (argc == 3 && !wcscmp(argv[1], L"--coverage")) return CoverageMain(argv[2]);
    if (argc != 3) { fwprintf(stderr, L"usage: %s <read pipe> <write pipe> | --self-test ... | --coverage <nvngx_vsr.dll>\n", argv[0]); return 2; }
    const HANDLE input = HANDLE(uintptr_t(_wcstoui64(argv[1], nullptr, 10)));
    const HANDLE output = HANDLE(uintptr_t(_wcstoui64(argv[2], nullptr, 10)));
    return StreamMain(input, output);
}
