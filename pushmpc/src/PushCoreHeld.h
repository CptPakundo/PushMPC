// PushMPC: the held-frame test on pushcore's ingress (our own; not part of PushFPS). One D3D11 compute pass per source
// frame, on the caller's device, compares the luma of the new ingress slot with the one before it: the picture is
// cut into cells of f x f pixels (f = width / 480, at least 2: 4 at 1080p, 8 at 4K), each cell's mean is compared,
// and the pass sums the result into eight counters (HeldFrames.h DiffStats). Cell means average away grain and
// compression noise (a 4x4 mean moves well under one code on a repeated drawing) while a moving line of a drawing
// shifts the mean of every cell it crosses by tens of codes. A thread group collects its 8x8 cells in group memory,
// one thread sums them and adds them to the counters with one atomic each.
// No CPU wait in the pass: the counters are copied into one of 8 staging buffers and read back later (the frame waits
// in pushcore's pending queue meanwhile; a read only waits when the queue would otherwise grow). Recorded on a deferred
// context and executed with RestoreContextState, as the deband pass is, so the shared immediate context keeps its state.
#pragma once
#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

#include "HeldFrames.h"

namespace pushmpc {

inline constexpr const char* kHeldHlsl = R"HLSL(
cbuffer Params : register(b0)
{
    uint2 cells;       // cells across, down
    uint cellSize;     // f: pixels per cell side
    uint pad;
};
Texture2D<float> cur : register(t0);
Texture2D<float> prev : register(t1);
RWByteAddressBuffer counters : register(u0);   // 0 cells, 1 max, 2..5 over 2/4/8/16 codes, 6 sum
RWStructuredBuffer<float> grid : register(u1);  // the current frame's cell means (codes), cells.x * cells.y (region tracking)

groupshared uint sQ[64];
groupshared uint sIn[64];

[numthreads(8, 8, 1)]
void CS_Held(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    uint q = 0, inside = 0;
    if (id.x < cells.x && id.y < cells.y) {
        const int2 o = int2(id.xy * cellSize);
        float a = 0, b = 0;
        for (uint y = 0; y < cellSize; ++y)
            for (uint x = 0; x < cellSize; ++x) {
                a += cur.Load(int3(o + int2(x, y), 0));
                b += prev.Load(int3(o + int2(x, y), 0));
            }
        const float codes = abs(a - b) * 255.0 / float(cellSize * cellSize);
        q = uint(codes * 16.0 + 0.5);
        inside = 1;
        grid[id.y * cells.x + id.x] = a * 255.0 / float(cellSize * cellSize);
    }
    sQ[gi] = q; sIn[gi] = inside;
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0) {
        uint n = 0, mx = 0, sum = 0, o0 = 0, o1 = 0, o2 = 0, o3 = 0;
        for (uint i = 0; i < 64; ++i) {
            if (!sIn[i]) continue;
            const uint v = sQ[i];
            n += 1; mx = max(mx, v); sum += v;
            o0 += v > 32 ? 1 : 0; o1 += v > 64 ? 1 : 0; o2 += v > 128 ? 1 : 0; o3 += v > 256 ? 1 : 0;
        }
        uint d;
        counters.InterlockedAdd(0, n, d);
        counters.InterlockedMax(4, mx, d);
        counters.InterlockedAdd(8, o0, d);
        counters.InterlockedAdd(12, o1, d);
        counters.InterlockedAdd(16, o2, d);
        counters.InterlockedAdd(20, o3, d);
        counters.InterlockedAdd(24, sum, d);
    }
}
)HLSL";

class HeldDetector {
public:
    static constexpr uint32_t kResults = 8;
    static constexpr uint32_t kMaxCells = 512 * 320;   // f = width / 480: about 480 x 270 cells at 16:9
    bool init(ID3D11Device* device)
    {
        dev_ = device;
        if (FAILED(dev_.As(&dev3_))) return failWith("no ID3D11Device3 (plane views)");
        Microsoft::WRL::ComPtr<ID3DBlob> cs, err;
        if (FAILED(D3DCompile(kHeldHlsl, std::strlen(kHeldHlsl), "PushMpcHeld", nullptr, nullptr, "CS_Held", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &err)))
            return failWith(std::string("compile CS_Held: ") + (err ? static_cast<const char*>(err->GetBufferPointer()) : "?"));
        if (FAILED(dev_->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr, &cs_))) return failWith("CreateComputeShader");
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(dev_->CreateBuffer(&bd, nullptr, &cb_))) return failWith("CreateBuffer(constants)");
        bd = {};
        bd.ByteWidth = 32; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS; bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        if (FAILED(dev_->CreateBuffer(&bd, nullptr, &counters_))) return failWith("CreateBuffer(counters)");
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_TYPELESS; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.FirstElement = 0; ud.Buffer.NumElements = 8; ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        if (FAILED(dev_->CreateUnorderedAccessView(counters_.Get(), &ud, &uav_))) return failWith("CreateUnorderedAccessView(counters)");
        bd = {};
        bd.ByteWidth = 32; bd.Usage = D3D11_USAGE_STAGING; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto& r : results_) if (FAILED(dev_->CreateBuffer(&bd, nullptr, &r.staging))) return failWith("CreateBuffer(staging)");
        // the cell grid (structured, up to kMaxCells floats) and its staging copies
        bd = {};
        bd.ByteWidth = kMaxCells * 4; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = 4;
        if (FAILED(dev_->CreateBuffer(&bd, nullptr, &grid_))) return failWith("CreateBuffer(grid)");
        D3D11_UNORDERED_ACCESS_VIEW_DESC gu{};
        gu.Format = DXGI_FORMAT_UNKNOWN; gu.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; gu.Buffer.NumElements = kMaxCells;
        if (FAILED(dev_->CreateUnorderedAccessView(grid_.Get(), &gu, &gridUav_))) return failWith("CreateUnorderedAccessView(grid)");
        bd = {};
        bd.ByteWidth = kMaxCells * 4; bd.Usage = D3D11_USAGE_STAGING; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto& r : results_) if (FAILED(dev_->CreateBuffer(&bd, nullptr, &r.gridStaging))) return failWith("CreateBuffer(grid staging)");
        if (FAILED(dev_->CreateDeferredContext(0, &deferred_))) return failWith("CreateDeferredContext");
        ready_ = true;
        return true;
    }
    bool ready() const { return ready_; }
    const std::string& why() const { return why_; }
    void forgetViews() { views_.clear(); }

    // Records "compare cur with prev" into result `index` (mod kResults). Both are ingress textures (NV12 / P010).
    bool run(ID3D11DeviceContext* imm, ID3D11Texture2D* cur, ID3D11Texture2D* prev, uint32_t width, uint32_t height, uint64_t index)
    {
        if (!ready_ || !cur || !prev) return false;
        ID3D11ShaderResourceView* a = view(cur);
        ID3D11ShaderResourceView* b = view(prev);
        if (!a || !b) return false;
        const uint32_t f = std::max<uint32_t>(2, (width + 479) / 480);
        const uint32_t cx = width / f, cy = height / f;
        if (!cx || !cy || uint64_t(cx) * cy > kMaxCells) return false;
        const uint32_t params[4] = { cx, cy, f, 0 };
        imm->UpdateSubresource(cb_.Get(), 0, nullptr, params, 0, 0);
        const UINT zero[4] = { 0, 0, 0, 0 };
        deferred_->ClearUnorderedAccessViewUint(uav_.Get(), zero);
        ID3D11ShaderResourceView* srv[] = { a, b };
        ID3D11UnorderedAccessView* uav[] = { uav_.Get(), gridUav_.Get() };
        ID3D11Buffer* cb[] = { cb_.Get() };
        deferred_->CSSetShaderResources(0, 2, srv);
        deferred_->CSSetUnorderedAccessViews(0, 2, uav, nullptr);
        deferred_->CSSetConstantBuffers(0, 1, cb);
        deferred_->CSSetShader(cs_.Get(), nullptr, 0);
        deferred_->Dispatch((cx + 7) / 8, (cy + 7) / 8, 1);
        auto& r = results_[index % kResults];
        deferred_->CopyResource(r.staging.Get(), counters_.Get());
        deferred_->CopyResource(r.gridStaging.Get(), grid_.Get());
        r.cx = cx; r.cy = cy; r.cell = f;
        Microsoft::WRL::ComPtr<ID3D11CommandList> list;
        if (FAILED(deferred_->FinishCommandList(FALSE, &list))) return false;
        imm->ExecuteCommandList(list.Get(), TRUE);
        r.pending = true; r.index = index;
        return true;
    }
    // Reads result `index`; false = not there (never recorded, overwritten) or not done yet when !wait.
    bool read(ID3D11DeviceContext* imm, uint64_t index, held::DiffStats& out, bool wait, held::CellGrid* grid = nullptr)
    {
        auto& r = results_[index % kResults];
        if (!r.pending || r.index != index) return false;
        D3D11_MAPPED_SUBRESOURCE m{};
        const HRESULT hr = imm->Map(r.staging.Get(), 0, D3D11_MAP_READ, wait ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return false;
        if (FAILED(hr)) { r.pending = false; out = {}; out.valid = false; return true; }   // answered: not comparable
        uint32_t c[8]; std::memcpy(c, m.pData, sizeof(c));
        imm->Unmap(r.staging.Get(), 0);
        r.pending = false;
        out = {};
        out.cells = c[0]; out.maxQ = c[1];
        for (int i = 0; i < 4; ++i) out.over[i] = c[2 + i];
        out.sumQ = c[6];
        out.valid = out.cells > 0;
        if (grid) {
            grid->w = r.cx; grid->h = r.cy; grid->cell = r.cell; grid->v.assign(size_t(r.cx) * r.cy, 0.f);
            D3D11_MAPPED_SUBRESOURCE g{};
            if (SUCCEEDED(imm->Map(r.gridStaging.Get(), 0, D3D11_MAP_READ, 0, &g))) { std::memcpy(grid->v.data(), g.pData, grid->v.size() * 4); imm->Unmap(r.gridStaging.Get(), 0); }
            else grid->v.clear();
        }
        return true;
    }

private:
    bool failWith(std::string w) { why_ = std::move(w); ready_ = false; return false; }
    ID3D11ShaderResourceView* view(ID3D11Texture2D* t)
    {
        auto it = views_.find(t);
        if (it != views_.end()) return it->second.Get();
        D3D11_TEXTURE2D_DESC d{}; t->GetDesc(&d);
        D3D11_SHADER_RESOURCE_VIEW_DESC1 sv{};
        sv.Format = d.Format == DXGI_FORMAT_NV12 ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R16_UNORM;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1; sv.Texture2D.PlaneSlice = 0;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView1> s;
        if (FAILED(dev3_->CreateShaderResourceView1(t, &sv, &s))) { failWith("luma plane SRV"); return nullptr; }
        if (views_.size() > 32) views_.clear();
        views_[t] = s;
        return s.Get();
    }
    struct Result { Microsoft::WRL::ComPtr<ID3D11Buffer> staging, gridStaging; bool pending = false; uint64_t index = 0; uint32_t cx = 0, cy = 0, cell = 0; };
    Microsoft::WRL::ComPtr<ID3D11Device> dev_;
    Microsoft::WRL::ComPtr<ID3D11Device3> dev3_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> deferred_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> cs_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_, counters_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav_, gridUav_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> grid_;
    std::array<Result, kResults> results_{};
    std::unordered_map<ID3D11Texture2D*, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> views_;
    bool ready_ = false;
    std::string why_;
};

}  // namespace pushmpc
