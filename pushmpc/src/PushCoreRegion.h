// PushMPC: per-region re-timing by source repair (our own; not part of PushFPS). Animation often moves its parts at
// different rates: the background pans on ones while a character is drawn on twos and its mouth on threes. For each
// part that is held at a source frame s between its drawings d0 < s < d1, the span stream (a second VFG effect at the
// source size, SDK timestep mode, or PushFlow) makes the pair d0 -> d1 at t = (s - d0) / (d1 - d0), and this pass lays
// that part of it into frame s, inside the part's mask; the graph then runs on the repaired source as usual, at any
// multiplier. So every part moves at its own cadence, the main frame generation never sees the jump from one drawing to
// the next, and the cost is one pair per repaired span, whatever the multiplier.
//
// Two D3D11 compute passes on the caller's device (recorded on a deferred context, executed with RestoreContextState):
//  - ToRgb: the NV12 / P010 ingress slot into an RGBA texture (BT.709 limited range, the same matrix both ways, so the
//    round trip is exact up to rounding; the span stream only ever sees these),
//  - Repair: ONE pass for all the frame's parts (up to 3; HeldFrames.h AssignOwners makes their masks disjoint, so the
//    order does not matter): into a scratch NV12 / P010 texture, Y and CbCr of the slot blended with each part's span
//    frame inside its mask (one channel of an RGBA mask texture each, bilinear from the cell grid, feathered with a
//    5x5 tent and a smoothstep), each part's frame shifted by its camera residual; outside every mask the slot's own
//    values are copied unchanged, so untouched pixels stay bit-exact.
//  - Detail matching (research P6): a synthesised frame is softer than a drawn one, so a held part would alternate crisp
//    (drawn frames) and soft (repaired frames) at the cadence, 8-12 Hz. In the mask, the span frame's high-pass (its
//    pixel minus the mean of its 4 neighbours) is scaled so its local energy (9 taps over 5x5 pixels) matches the slot's
//    own drawing there, gain clamped to 0.8-1.6.
#pragma once
#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace pushmpc {

inline constexpr const char* kRegionHlsl = R"HLSL(
cbuffer Params : register(b0) { uint2 size; float2 maskScale; float lo, hi; float depthScale; uint parts; float4 shift01; float2 shift2; float detail; float pad; };
Texture2D<float> inY : register(t0);
Texture2D<float2> inUV : register(t1);
Texture2D<float4> rep0 : register(t2);
Texture2D<float4> mask : register(t3);
Texture2D<float4> rep1 : register(t4);
Texture2D<float4> rep2 : register(t5);
SamplerState lin : register(s0);
RWTexture2D<unorm float4> outRgb : register(u0);
RWTexture2D<unorm float> outY : register(u1);
RWTexture2D<unorm float2> outUV : register(u2);

// BT.709 limited range, normalised values (depthScale maps a P010 R16 sample to the same 0..1 code scale as NV12)
float3 toRgb(float y, float2 uv)
{
    const float Y = (y * depthScale - 16.0 / 255.0) * (255.0 / 219.0);
    const float U = (uv.x * depthScale - 128.0 / 255.0) * (255.0 / 224.0);
    const float V = (uv.y * depthScale - 128.0 / 255.0) * (255.0 / 224.0);
    return saturate(float3(Y + 1.5748 * V, Y - 0.1873 * U - 0.4681 * V, Y + 1.8556 * U));
}
float3 toYuv(float3 c)
{
    const float Y = 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
    const float U = (c.b - Y) / 1.8556, V = (c.r - Y) / 1.5748;
    return float3((Y * 219.0 + 16.0) / 255.0, (U * 224.0 + 128.0) / 255.0, (V * 224.0 + 128.0) / 255.0) / depthScale;
}
float3 maskAt(float2 uv)
{
    float3 m = 0; float ws = 0;
    [unroll] for (int j = -2; j <= 2; ++j) [unroll] for (int i = -2; i <= 2; ++i) {
        const float w = (3 - abs(i)) * (3 - abs(j));
        m += w * mask.SampleLevel(lin, uv + float2(i, j) * 0.75 * maskScale, 0).rgb; ws += w;
    }
    return smoothstep(lo, hi, m / ws);
}
float2 shiftOf(uint i) { return i == 0 ? shift01.xy : i == 1 ? shift01.zw : shift2; }
float4 repAt(uint i, float2 uv)
{
    if (i == 0) return rep0.SampleLevel(lin, uv, 0);
    if (i == 1) return rep1.SampleLevel(lin, uv, 0);
    return rep2.SampleLevel(lin, uv, 0);
}
float repY(uint i, float2 p) { return toYuv(repAt(i, p / float2(size)).rgb).x; }   // p in pixels (centres at +0.5)
float slotY(int2 q) { return inY.Load(int3(clamp(q, int2(0, 0), int2(size) - 1), 0)); }
// local high-pass energy: 9 taps over 5 x 5 pixels of |pixel - mean of its 4 neighbours|
float energyRep(uint i, float2 p)
{
    float e = 0;
    [unroll] for (int j = -2; j <= 2; j += 2) [unroll] for (int k = -2; k <= 2; k += 2) {
        const float2 q = p + float2(k, j);
        e += abs(repY(i, q) - 0.25 * (repY(i, q + float2(1, 0)) + repY(i, q - float2(1, 0)) + repY(i, q + float2(0, 1)) + repY(i, q - float2(0, 1))));
    }
    return e / 9.0;
}
float energySlot(int2 p)
{
    float e = 0;
    [unroll] for (int j = -2; j <= 2; j += 2) [unroll] for (int k = -2; k <= 2; k += 2) {
        const int2 q = p + int2(k, j);
        e += abs(slotY(q) - 0.25 * (slotY(q + int2(1, 0)) + slotY(q - int2(1, 0)) + slotY(q + int2(0, 1)) + slotY(q - int2(0, 1))));
    }
    return e / 9.0;
}

[numthreads(16, 16, 1)]
void CS_ToRgb(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y) return;
    outRgb[id.xy] = float4(toRgb(inY.Load(int3(id.xy, 0)), inUV.Load(int3(id.xy / 2, 0))), 1);
}
[numthreads(16, 16, 1)]
void CS_RepairY(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y) return;
    const float y = inY.Load(int3(id.xy, 0));
    const float3 m = maskAt((float2(id.xy) + 0.5) / float2(size));
    float c = y;
    [loop] for (uint i = 0; i < parts; ++i) {
        const float mi = i == 0 ? m.r : i == 1 ? m.g : m.b;
        if (mi <= 0.001) continue;
        const float2 p = float2(id.xy) + 0.5 - shiftOf(i);
        float r = repY(i, p);
        if (detail > 0) {
            const float hf = r - 0.25 * (repY(i, p + float2(1, 0)) + repY(i, p - float2(1, 0)) + repY(i, p + float2(0, 1)) + repY(i, p - float2(0, 1)));
            const float g = clamp(sqrt((energySlot(int2(id.xy)) + 1e-4) / (energyRep(i, p) + 1e-4)), 0.8, 1.6);
            r += (g - 1.0) * hf;
        }
        c = lerp(c, r, mi);
    }
    outY[id.xy] = c;
}
[numthreads(16, 16, 1)]
void CS_RepairUV(uint3 id : SV_DispatchThreadID)
{
    const uint2 cs = size / 2;
    if (id.x >= cs.x || id.y >= cs.y) return;
    const float2 uv = inUV.Load(int3(id.xy, 0));
    const float3 m = maskAt((float2(id.xy * 2) + 1.0) / float2(size));
    float2 c = uv;
    [loop] for (uint i = 0; i < parts; ++i) {
        const float mi = i == 0 ? m.r : i == 1 ? m.g : m.b;
        if (mi <= 0.001) continue;
        // the 2 x 2 luma pixels of this chroma sample: bilinear at their centre is their mean (with the camera residual)
        const float3 s = repAt(i, (float2(id.xy * 2) + 1.0 - shiftOf(i)) / float2(size)).rgb;
        c = lerp(c, toYuv(s).yz, mi);
    }
    outUV[id.xy] = c;
}
)HLSL";

class RegionRepair {
public:
    static constexpr uint32_t kMaxParts = 3;
    bool init(ID3D11Device* device)
    {
        dev_ = device;
        if (FAILED(dev_.As(&dev3_))) return failWith("no ID3D11Device3 (plane views)");
        auto compile = [&](const char* entry, Microsoft::WRL::ComPtr<ID3D11ComputeShader>& cs) {
            Microsoft::WRL::ComPtr<ID3DBlob> b, err;
            if (FAILED(D3DCompile(kRegionHlsl, std::strlen(kRegionHlsl), "PushMpcRegion", nullptr, nullptr, entry, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &err)))
                return failWith(std::string("compile ") + entry + ": " + (err ? static_cast<const char*>(err->GetBufferPointer()) : "?"));
            if (FAILED(dev_->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &cs))) return failWith("CreateComputeShader");
            return true;
        };
        if (!compile("CS_ToRgb", toRgb_) || !compile("CS_RepairY", repY_) || !compile("CS_RepairUV", repUV_)) return false;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 64; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(dev_->CreateBuffer(&bd, nullptr, &cb_))) return failWith("CreateBuffer(region)");
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(dev_->CreateSamplerState(&sd, &sampler_))) return failWith("CreateSamplerState(region)");
        if (FAILED(dev_->CreateDeferredContext(0, &deferred_))) return failWith("CreateDeferredContext(region)");
        ready_ = true;
        return true;
    }
    bool ready() const { return ready_; }
    const std::string& why() const { return why_; }
    void forgetViews() { planes_.clear(); uavs_.clear(); srvs_.clear(); }

    // slot (NV12 / P010) -> rgb (R8G8B8A8 / R10G10B10A2 with UAV binding)
    bool toRgb(ID3D11DeviceContext* imm, ID3D11Texture2D* slot, ID3D11Texture2D* rgb, uint32_t w, uint32_t h)
    {
        Planes* p = planes(slot); ID3D11UnorderedAccessView* u = uav(rgb);
        if (!p || !u) return false;
        setParams(imm, w, h, p->p010, 0, nullptr, false);
        ID3D11ShaderResourceView* srv[] = { p->y.Get(), p->uv.Get() };
        deferred_->CSSetShaderResources(0, 2, srv);
        deferred_->CSSetUnorderedAccessViews(0, 1, &u, nullptr);
        dispatch(toRgb_.Get(), (w + 15) / 16, (h + 15) / 16);
        return finish(imm);
    }
    // the parts' masks (w x h cells, 0..255 each, disjoint), one channel each, for the next repair
    bool setMasks(ID3D11DeviceContext* imm, const std::vector<const std::vector<uint8_t>*>& masks, uint32_t w, uint32_t h, float featherCells)
    {
        if (!ready_ || masks.empty() || masks.size() > kMaxParts || !w || !h) return false;
        for (auto* m : masks) if (!m || m->size() != size_t(w) * h) return false;
        if (!maskTex_ || w != mw_ || h != mh_) {
            maskTex_.Reset(); maskSrv_.Reset();
            D3D11_TEXTURE2D_DESC d{};
            d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc = {1, 0};
            d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(dev_->CreateTexture2D(&d, nullptr, &maskTex_)) || FAILED(dev_->CreateShaderResourceView(maskTex_.Get(), nullptr, &maskSrv_))) return failWith("mask texture");
            mw_ = w; mh_ = h;
        }
        packed_.assign(size_t(w) * h * 4, 0);
        for (size_t k = 0; k < masks.size(); ++k) { const auto& m = *masks[k]; for (size_t i = 0; i < m.size(); ++i) packed_[i * 4 + k] = m[i]; }
        feather_ = std::clamp(featherCells, 0.25f, 2.0f);
        imm->UpdateSubresource(maskTex_.Get(), 0, nullptr, packed_.data(), w * 4, 0);
        return true;
    }
    // out (NV12 / P010 with plane UAVs) = slot with each part's span frame laid in inside its mask, shifted by its camera
    // residual (pixels; shifts[2 i], shifts[2 i + 1]); detail: match the drawn sharpness (P6)
    bool repair(ID3D11DeviceContext* imm, ID3D11Texture2D* slot, ID3D11Texture2D* const* reps, uint32_t parts, ID3D11Texture2D* out,
                uint32_t w, uint32_t h, const float* shifts, bool detail)
    {
        if (!parts || parts > kMaxParts) return false;
        Planes* p = planes(slot); Planes* o = planes(out, true);
        ID3D11ShaderResourceView* r[kMaxParts] = {};
        for (uint32_t i = 0; i < parts; ++i) if (!(r[i] = srv(reps[i]))) return false;
        for (uint32_t i = parts; i < kMaxParts; ++i) r[i] = r[0];
        if (!p || !o || !maskSrv_) return false;
        setParams(imm, w, h, p->p010, parts, shifts, detail);
        ID3D11ShaderResourceView* srvs[] = { p->y.Get(), p->uv.Get(), r[0], maskSrv_.Get(), r[1], r[2] };
        ID3D11UnorderedAccessView* uavs[] = { nullptr, o->uy.Get(), o->uuv.Get() };
        deferred_->CSSetShaderResources(0, 6, srvs);
        deferred_->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
        dispatch(repY_.Get(), (w + 15) / 16, (h + 15) / 16);
        dispatch(repUV_.Get(), (w / 2 + 15) / 16, (h / 2 + 15) / 16);
        return finish(imm);
    }

private:
    struct Planes { Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> y, uv; Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uy, uuv; bool p010 = false; };
    bool failWith(std::string w) { why_ = std::move(w); ready_ = false; return false; }
    void setParams(ID3D11DeviceContext* imm, uint32_t w, uint32_t h, bool p010, uint32_t parts, const float* shifts, bool detail)
    {
        // feather: maskScale steps of 0.75 cell per tap at feather 1
        struct P { uint32_t w, h; float sx, sy, lo, hi, depth; uint32_t parts; float sh[6]; float detail, pad; } p{};
        p.w = w; p.h = h; p.sx = feather_ / float(std::max(1u, mw_)); p.sy = feather_ / float(std::max(1u, mh_)); p.lo = 0.15f; p.hi = 0.85f;
        p.depth = p010 ? 65535.0f / 65472.0f : 1.0f; p.parts = parts; p.detail = detail ? 1.f : 0.f;
        for (uint32_t i = 0; shifts && i < parts && i < kMaxParts; ++i) { p.sh[2 * i] = shifts[2 * i]; p.sh[2 * i + 1] = shifts[2 * i + 1]; }
        static_assert(sizeof(P) == 64, "region constants");
        imm->UpdateSubresource(cb_.Get(), 0, nullptr, &p, 0, 0);
        ID3D11Buffer* cbs[] = { cb_.Get() };
        ID3D11SamplerState* ss[] = { sampler_.Get() };
        deferred_->CSSetConstantBuffers(0, 1, cbs);
        deferred_->CSSetSamplers(0, 1, ss);
    }
    void dispatch(ID3D11ComputeShader* cs, UINT x, UINT y) { deferred_->CSSetShader(cs, nullptr, 0); deferred_->Dispatch(x, y, 1); }
    bool finish(ID3D11DeviceContext* imm)
    {
        Microsoft::WRL::ComPtr<ID3D11CommandList> list;
        if (FAILED(deferred_->FinishCommandList(FALSE, &list))) return false;
        imm->ExecuteCommandList(list.Get(), TRUE);
        return true;
    }
    Planes* planes(ID3D11Texture2D* t, bool writable = false)
    {
        auto it = planes_.find(t);
        if (it != planes_.end() && (!writable || it->second.uy)) return &it->second;
        D3D11_TEXTURE2D_DESC d{}; t->GetDesc(&d);
        Planes p; p.p010 = d.Format != DXGI_FORMAT_NV12;
        for (UINT plane = 0; plane < 2; ++plane) {
            D3D11_SHADER_RESOURCE_VIEW_DESC1 sv{};
            sv.Format = plane ? (p.p010 ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM) : (p.p010 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM);
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1; sv.Texture2D.PlaneSlice = plane;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView1> s;
            if (FAILED(dev3_->CreateShaderResourceView1(t, &sv, &s))) { failWith("plane SRV"); return nullptr; }
            (plane ? p.uv : p.y) = s;
            if (writable) {
                D3D11_UNORDERED_ACCESS_VIEW_DESC1 ud{};
                ud.Format = sv.Format; ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D; ud.Texture2D.PlaneSlice = plane;
                Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView1> u;
                if (FAILED(dev3_->CreateUnorderedAccessView1(t, &ud, &u))) { failWith("plane UAV"); return nullptr; }
                (plane ? p.uuv : p.uy) = u;
            }
        }
        if (planes_.size() > 64) planes_.clear();
        return &(planes_[t] = p);
    }
    ID3D11ShaderResourceView* srv(ID3D11Texture2D* t)
    {
        if (auto it = srvs_.find(t); it != srvs_.end()) return it->second.Get();
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> v;
        if (FAILED(dev_->CreateShaderResourceView(t, nullptr, &v))) return nullptr;
        if (srvs_.size() > 64) srvs_.clear();
        return (srvs_[t] = v).Get();
    }
    ID3D11UnorderedAccessView* uav(ID3D11Texture2D* t)
    {
        if (auto it = uavs_.find(t); it != uavs_.end()) return it->second.Get();
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> v;
        if (FAILED(dev_->CreateUnorderedAccessView(t, nullptr, &v))) return nullptr;
        if (uavs_.size() > 64) uavs_.clear();
        return (uavs_[t] = v).Get();
    }
    Microsoft::WRL::ComPtr<ID3D11Device> dev_;
    Microsoft::WRL::ComPtr<ID3D11Device3> dev3_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> deferred_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> toRgb_, repY_, repUV_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> maskTex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> maskSrv_;
    std::vector<uint8_t> packed_;
    std::unordered_map<ID3D11Texture2D*, Planes> planes_;
    std::unordered_map<ID3D11Texture2D*, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> srvs_;
    std::unordered_map<ID3D11Texture2D*, Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>> uavs_;
    uint32_t mw_ = 0, mh_ = 0;
    float feather_ = 1.0f;
    bool ready_ = false;
    std::string why_;
};

}  // namespace pushmpc
