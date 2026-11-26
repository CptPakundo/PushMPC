// PushMPC: per-region re-timing by source repair (our own; not part of PushFPS). Animation often moves its parts at
// different rates: the background pans on ones while a character is drawn on twos and its mouth on threes. For each
// part that is held at a source frame s between its drawings d0 < s < d1, the span stream (a second VFG effect at the
// source size, SDK timestep mode) makes the pair d0 -> d1 at t = (s - d0) / (d1 - d0), and this pass lays that part of it
// into frame s, inside the part's mask; the graph then runs on the repaired source as usual, at any multiplier. So every
// part moves at its own cadence, the main frame generation never sees the jump from one drawing to the next, and the
// cost is one pair per repaired source frame, whatever the multiplier.
//
// Two D3D11 compute passes on the caller's device (recorded on a deferred context, executed with RestoreContextState):
//  - ToRgb: the NV12 / P010 ingress slot into an RGBA texture (BT.709 limited range, the same matrix both ways, so the
//    round trip is exact up to rounding; the span stream only ever sees these),
//  - Repair: into a scratch NV12 / P010 texture, Y and CbCr of the slot blended with the span frame's inside the mask
//    (bilinear from the cell grid, feathered with a 5x5 tent and a smoothstep); outside the mask the slot's own values
//    are copied unchanged, so untouched pixels stay bit-exact.
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
cbuffer Params : register(b0) { uint2 size; float2 maskScale; float lo, hi; float depthScale; float pad; float2 shift; float2 pad2; };   // shift: the camera residual (pixels)
Texture2D<float> inY : register(t0);
Texture2D<float2> inUV : register(t1);
Texture2D<float4> rep : register(t2);
Texture2D<float> mask : register(t3);
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
float maskAt(float2 uv)
{
    float m = 0, ws = 0;
    [unroll] for (int j = -2; j <= 2; ++j) [unroll] for (int i = -2; i <= 2; ++i) {
        const float w = (3 - abs(i)) * (3 - abs(j));
        m += w * mask.SampleLevel(lin, uv + float2(i, j) * 0.75 * maskScale, 0); ws += w;
    }
    return smoothstep(lo, hi, m / ws);
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
    const float m = maskAt((float2(id.xy) + 0.5) / float2(size));
    outY[id.xy] = m > 0.001 ? lerp(y, toYuv(rep.SampleLevel(lin, (float2(id.xy) + 0.5 - shift) / float2(size), 0).rgb).x, m) : y;
}
[numthreads(16, 16, 1)]
void CS_RepairUV(uint3 id : SV_DispatchThreadID)
{
    const uint2 c = size / 2;
    if (id.x >= c.x || id.y >= c.y) return;
    const float2 uv = inUV.Load(int3(id.xy, 0));
    const float m = maskAt((float2(id.xy * 2) + 1.0) / float2(size));
    if (m <= 0.001) { outUV[id.xy] = uv; return; }
    // the 2 x 2 luma pixels of this chroma sample: bilinear at their centre is their mean (with the camera residual)
    const float3 s = rep.SampleLevel(lin, (float2(id.xy * 2) + 1.0 - shift) / float2(size), 0).rgb;
    outUV[id.xy] = lerp(uv, toYuv(s).yz, m);
}
)HLSL";

class RegionRepair {
public:
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
        bd.ByteWidth = 48; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
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
        setParams(imm, w, h, p->p010);
        ID3D11ShaderResourceView* srv[] = { p->y.Get(), p->uv.Get() };
        deferred_->CSSetShaderResources(0, 2, srv);
        deferred_->CSSetUnorderedAccessViews(0, 1, &u, nullptr);
        dispatch(toRgb_.Get(), (w + 15) / 16, (h + 15) / 16);
        return finish(imm);
    }
    // the part's mask (w x h cells, 0..255) for the next repair
    bool setMask(ID3D11DeviceContext* imm, const std::vector<uint8_t>& m, uint32_t w, uint32_t h, float featherCells)
    {
        if (!ready_ || m.size() != size_t(w) * h || !w || !h) return false;
        if (!maskTex_ || w != mw_ || h != mh_) {
            maskTex_.Reset(); maskSrv_.Reset();
            D3D11_TEXTURE2D_DESC d{};
            d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8_UNORM; d.SampleDesc = {1, 0};
            d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(dev_->CreateTexture2D(&d, nullptr, &maskTex_)) || FAILED(dev_->CreateShaderResourceView(maskTex_.Get(), nullptr, &maskSrv_))) return failWith("mask texture");
            mw_ = w; mh_ = h;
        }
        feather_ = std::clamp(featherCells, 0.25f, 2.0f);
        imm->UpdateSubresource(maskTex_.Get(), 0, nullptr, m.data(), w, 0);
        return true;
    }
    // out (NV12 / P010 with plane UAVs) = slot with the span frame `rep` laid in inside the mask, shifted by (sx, sy)
    // pixels (the camera residual of a part that moves with the camera; 0 for the rest)
    bool repair(ID3D11DeviceContext* imm, ID3D11Texture2D* slot, ID3D11Texture2D* rep, ID3D11Texture2D* out, uint32_t w, uint32_t h, float sx = 0.f, float sy = 0.f)
    {
        Planes* p = planes(slot); Planes* o = planes(out, true); ID3D11ShaderResourceView* r = srv(rep);
        if (!p || !o || !r || !maskSrv_) return false;
        setParams(imm, w, h, p->p010, sx, sy);
        ID3D11ShaderResourceView* srvs[] = { p->y.Get(), p->uv.Get(), r, maskSrv_.Get() };
        ID3D11UnorderedAccessView* uavs[] = { nullptr, o->uy.Get(), o->uuv.Get() };
        deferred_->CSSetShaderResources(0, 4, srvs);
        deferred_->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
        dispatch(repY_.Get(), (w + 15) / 16, (h + 15) / 16);
        dispatch(repUV_.Get(), (w / 2 + 15) / 16, (h / 2 + 15) / 16);
        return finish(imm);
    }

private:
    struct Planes { Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> y, uv; Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uy, uuv; bool p010 = false; };
    bool failWith(std::string w) { why_ = std::move(w); ready_ = false; return false; }
    void setParams(ID3D11DeviceContext* imm, uint32_t w, uint32_t h, bool p010, float shiftX = 0.f, float shiftY = 0.f)
    {
        // feather: maskScale steps of 0.75 cell per tap at feather 1
        struct P { uint32_t w, h; float sx, sy, lo, hi, depth, pad, shx, shy, pad2[2]; } p{ w, h, feather_ / float(std::max(1u, mw_)), feather_ / float(std::max(1u, mh_)), 0.15f, 0.85f,
                                                                          p010 ? 65535.0f / 65472.0f * 1.0f : 1.0f, 0, shiftX, shiftY, {0, 0} };
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
    std::unordered_map<ID3D11Texture2D*, Planes> planes_;
    std::unordered_map<ID3D11Texture2D*, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> srvs_;
    std::unordered_map<ID3D11Texture2D*, Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>> uavs_;
    uint32_t mw_ = 0, mh_ = 0;
    float feather_ = 1.0f;
    bool ready_ = false;
    std::string why_;
};

}  // namespace pushmpc
