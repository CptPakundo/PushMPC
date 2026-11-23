// PushMPC's deband pass (our own; not part of PushFPS). Runs in pushcore's ingress on the caller's D3D11 device, on the
// 8-bit NV12 source before the graph: banding is a compression artifact of the source, so it is removed where it
// arises, at the source size (a quarter of the pixels of a 4K output), before RTX VSR could sharpen the band edges into
// steps and before frame generation interpolates them. GPU only: copy the source slice into a scratch texture, two
// compute passes (luma, chroma) into a second scratch texture, which then replaces the plain ingress copy. Recorded on
// a deferred context and executed with RestoreContextState, so the immediate context's state (shared with the filter
// and other threads) is left as it was. No CPU wait: the timing comes from D3D11 timestamp queries read back frames
// later with DONOTFLUSH.
//
// Algorithm: the libplacebo / mpv deband (described there as iterative random-offset sampling with a threshold, then
// grain), implemented here from that description: per pixel and iteration i = 1..N, a random direction and a random
// distance up to i x radius; the average of the four samples at quarter turns around the pixel replaces the pixel
// when it differs from it by less than threshold / i (a smooth gradient), else the pixel is kept (an edge or detail).
// Where a pixel was replaced, triangular grain of +-grain code values is added before the 8-bit store, so the
// smoothed gradient is dithered and not quantised back into the same bands. Pixels that kept their value (edges,
// texture, flat areas with no nearby step) are stored unchanged, so clean sources stay bit-exact outside gradients.
// The random numbers are a PCG hash of the pixel, the plane and a per-frame counter.
#pragma once
#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <format>
#include <string>
#include <vector>

namespace pushmpc {

inline constexpr const char* kDebandHlsl = R"HLSL(
cbuffer Params : register(b0)
{
    uint2 size;        // plane size in texels
    float threshold;   // normalised code difference below which the average replaces the pixel (iteration 1)
    float radius;      // texels, iteration 1 (iteration i reaches i x radius)
    float grain;       // normalised triangular grain amplitude where a pixel was replaced
    uint iterations;
    uint seed;         // frame counter and plane
    uint pad;
};
Texture2D<float4> src : register(t0);
SamplerState linearClamp : register(s0);
RWTexture2D<unorm float> outY : register(u0);
RWTexture2D<unorm float2> outUV : register(u1);

uint pcg(uint v)
{
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
float rnd(inout uint s) { s = pcg(s); return float(s >> 8) * (1.0 / 16777216.0); }

float4 at(float2 p) { return src.SampleLevel(linearClamp, p / float2(size), 0); }

float4 deband(uint2 id, out bool replaced)
{
    const float2 p = float2(id) + 0.5;
    const float4 original = src.Load(int3(id, 0));
    float4 res = original;
    uint s = pcg(id.x + pcg(id.y + pcg(seed)));
    [loop] for (uint i = 1; i <= iterations; ++i) {
        const float d = rnd(s) * float(i) * radius;
        float sn, cs;
        sincos(rnd(s) * 6.2831853, sn, cs);
        const float2 o = d * float2(cs, sn);
        const float4 avg = 0.25 * (at(p + o) + at(p - o) + at(p + float2(-o.y, o.x)) + at(p + float2(o.y, -o.x)));
        const float4 diff = abs(res - avg);
        const float limit = threshold / float(i);
        res = float4(diff.x > limit ? res.x : avg.x, diff.y > limit ? res.y : avg.y,
                     diff.z > limit ? res.z : avg.z, diff.w > limit ? res.w : avg.w);
    }
    replaced = any(abs(res - original) > 1e-6);
    if (replaced && grain > 0) {
        const float4 g = float4(rnd(s) + rnd(s) - 1.0, rnd(s) + rnd(s) - 1.0, 0, 0);
        res += g * grain;
    }
    return res;
}

[numthreads(8, 8, 1)] void CS_DebandY(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y) return;
    bool replaced;
    const float4 v = deband(id.xy, replaced);
    outY[id.xy] = replaced ? saturate(v.x) : src.Load(int3(id.xy, 0)).x;
}
[numthreads(8, 8, 1)] void CS_DebandUV(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y) return;
    bool replaced;
    const float4 v = deband(id.xy, replaced);
    outUV[id.xy] = replaced ? saturate(v.xy) : src.Load(int3(id.xy, 0)).xy;
}
)HLSL";

struct DebandPreset { uint32_t iterations; float thresholdCodes, radius, grainCodes; const char* name; };
// Low ~ libplacebo's defaults (1 iteration, threshold ~1 code value, radius 16, grain ~1.5 code values: here grain is
// applied only where a pixel was smoothed); Medium and High add iterations and a wider threshold for the coarse bands
// of low-bitrate web video.
inline constexpr DebandPreset kDebandPresets[3] = {
    { 1, 1.0f, 16.0f, 1.0f, "low" },
    { 2, 1.6f, 16.0f, 1.0f, "medium" },
    { 4, 2.4f, 20.0f, 1.25f, "high" },
};

class Deband {
public:
    // Compiles the shaders and checks NV12 plane UAV support. False = the pass is not available on this device (logged
    // by the caller with why()).
    bool init(ID3D11Device* device)
    {
        using Microsoft::WRL::ComPtr;
        dev_ = device;
        if (FAILED(dev_.As(&dev3_))) return failWith("no ID3D11Device3 (plane views)");
        // NV12 plane UAVs (R8 / R8G8 typed stores) are tried when the textures are made (ensure): that is the real test;
        // CheckFeatureSupport's answer for planar formats differs between drivers.
        ComPtr<ID3DBlob> y, uv, err;
        const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
        if (FAILED(D3DCompile(kDebandHlsl, std::strlen(kDebandHlsl), "PushMpcDeband", nullptr, nullptr, "CS_DebandY", "cs_5_0", flags, 0, &y, &err)))
            return failWith(std::string("compile CS_DebandY: ") + (err ? static_cast<const char*>(err->GetBufferPointer()) : "?"));
        if (FAILED(D3DCompile(kDebandHlsl, std::strlen(kDebandHlsl), "PushMpcDeband", nullptr, nullptr, "CS_DebandUV", "cs_5_0", flags, 0, &uv, &err)))
            return failWith(std::string("compile CS_DebandUV: ") + (err ? static_cast<const char*>(err->GetBufferPointer()) : "?"));
        if (FAILED(dev_->CreateComputeShader(y->GetBufferPointer(), y->GetBufferSize(), nullptr, &csY_)) ||
            FAILED(dev_->CreateComputeShader(uv->GetBufferPointer(), uv->GetBufferSize(), nullptr, &csUV_)))
            return failWith("CreateComputeShader");
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(dev_->CreateSamplerState(&sd, &sampler_))) return failWith("CreateSamplerState");
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 32; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(dev_->CreateBuffer(&bd, nullptr, &cbY_)) || FAILED(dev_->CreateBuffer(&bd, nullptr, &cbUV_))) return failWith("CreateBuffer");
        if (FAILED(dev_->CreateDeferredContext(0, &deferred_))) return failWith("CreateDeferredContext");
        for (auto& q : queries_) {
            D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            if (FAILED(dev_->CreateQuery(&qd, &q.disjoint))) return failWith("CreateQuery");
            qd.Query = D3D11_QUERY_TIMESTAMP;
            if (FAILED(dev_->CreateQuery(&qd, &q.begin)) || FAILED(dev_->CreateQuery(&qd, &q.end))) return failWith("CreateQuery");
        }
        ready_ = true;
        return true;
    }
    bool ready() const { return ready_; }
    const std::string& why() const { return why_; }

    // Records "src[slice] -> deband -> out()" on `imm` (immediate context). out() is valid until the next call.
    bool run(ID3D11DeviceContext* imm, ID3D11Texture2D* src, UINT subresource, UINT width, UINT height, uint32_t strength)
    {
        if (!ready_ || !ensure(width, height)) return false;
        readTimings(imm);
        Query* q = nullptr;
        for (auto& c : queries_) if (!c.pending) { q = &c; break; }
        if (q) { imm->Begin(q->disjoint.Get()); imm->End(q->begin.Get()); }
        imm->CopySubresourceRegion(in_.Get(), 0, 0, 0, 0, src, subresource, nullptr);
        const DebandPreset& p = kDebandPresets[std::min<uint32_t>(strength, 2)];
        ++frame_;
        struct Params { uint32_t w, h; float threshold, radius, grain; uint32_t iterations, seed, pad; };
        const Params py{ w_, h_, p.thresholdCodes / 255.0f, p.radius, p.grainCodes / 255.0f, p.iterations, frame_ * 2u, 0 };
        const Params pc{ w_ / 2, h_ / 2, p.thresholdCodes / 255.0f, p.radius * 0.5f, p.grainCodes / 255.0f, p.iterations, frame_ * 2u + 1u, 0 };
        imm->UpdateSubresource(cbY_.Get(), 0, nullptr, &py, 0, 0);
        imm->UpdateSubresource(cbUV_.Get(), 0, nullptr, &pc, 0, 0);
        ID3D11SamplerState* samplers[] = { sampler_.Get() };
        deferred_->CSSetSamplers(0, 1, samplers);
        // luma
        ID3D11ShaderResourceView* srv[] = { inY_.Get() };
        ID3D11UnorderedAccessView* uav[] = { outY_.Get(), outUV_.Get() };
        deferred_->CSSetUnorderedAccessViews(0, 2, uav, nullptr);
        ID3D11Buffer* cb[] = { cbY_.Get() };
        deferred_->CSSetConstantBuffers(0, 1, cb);
        deferred_->CSSetShaderResources(0, 1, srv);
        deferred_->CSSetShader(csY_.Get(), nullptr, 0);
        deferred_->Dispatch((w_ + 7) / 8, (h_ + 7) / 8, 1);
        // chroma (half size)
        srv[0] = inUV_.Get(); cb[0] = cbUV_.Get();
        deferred_->CSSetConstantBuffers(0, 1, cb);
        deferred_->CSSetShaderResources(0, 1, srv);
        deferred_->CSSetShader(csUV_.Get(), nullptr, 0);
        deferred_->Dispatch((w_ / 2 + 7) / 8, (h_ / 2 + 7) / 8, 1);
        Microsoft::WRL::ComPtr<ID3D11CommandList> list;
        if (FAILED(deferred_->FinishCommandList(FALSE, &list))) return false;
        imm->ExecuteCommandList(list.Get(), TRUE);
        if (q) { imm->End(q->end.Get()); imm->End(q->disjoint.Get()); q->pending = true; }
        return true;
    }
    ID3D11Texture2D* out() const { return out_.Get(); }
    // median of the last 64 measured passes (copy + both dispatches), < 0 = none yet
    double medianMs() const
    {
        if (ms_.empty()) return -1;
        std::vector<double> v(ms_.begin(), ms_.end());
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return v[v.size() / 2];
    }
    void resetTimings() { ms_.clear(); }

private:
    struct Query { Microsoft::WRL::ComPtr<ID3D11Query> disjoint, begin, end; bool pending = false; };
    bool failWith(std::string w) { why_ = std::move(w); ready_ = false; return false; }
    bool ensure(UINT w, UINT h)
    {
        w &= ~1u; h &= ~1u;
        if (out_ && w == w_ && h == h_) return true;
        in_.Reset(); out_.Reset(); inY_.Reset(); inUV_.Reset(); outY_.Reset(); outUV_.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_NV12;
        td.SampleDesc = {1, 0}; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &in_))) return failWith("deband input texture");
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &out_))) return failWith("deband output texture");
        for (UINT plane = 0; plane < 2; ++plane) {
            D3D11_SHADER_RESOURCE_VIEW_DESC1 sv{};
            sv.Format = plane ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM;
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = 1; sv.Texture2D.PlaneSlice = plane;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView1> s;
            if (FAILED(dev3_->CreateShaderResourceView1(in_.Get(), &sv, &s))) return failWith("deband plane SRV");
            (plane ? inUV_ : inY_) = s;
            D3D11_UNORDERED_ACCESS_VIEW_DESC1 ud{};
            ud.Format = sv.Format;
            ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            ud.Texture2D.PlaneSlice = plane;
            Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView1> u;
            if (FAILED(dev3_->CreateUnorderedAccessView1(out_.Get(), &ud, &u))) return failWith("deband plane UAV");
            (plane ? outUV_ : outY_) = u;
        }
        w_ = w; h_ = h;
        return true;
    }
    void readTimings(ID3D11DeviceContext* imm)
    {
        for (auto& q : queries_) {
            if (!q.pending) continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
            UINT64 a = 0, b = 0;
            if (imm->GetData(q.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            if (imm->GetData(q.begin.Get(), &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                imm->GetData(q.end.Get(), &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            q.pending = false;
            if (dj.Disjoint || !dj.Frequency || b < a) continue;
            ms_.push_back(double(b - a) * 1000.0 / double(dj.Frequency));
            if (ms_.size() > 64) ms_.pop_front();
        }
    }

    Microsoft::WRL::ComPtr<ID3D11Device> dev_;
    Microsoft::WRL::ComPtr<ID3D11Device3> dev3_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> deferred_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> csY_, csUV_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cbY_, cbUV_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> in_, out_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> inY_, inUV_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> outY_, outUV_;
    std::array<Query, 4> queries_{};
    std::deque<double> ms_;
    UINT w_ = 0, h_ = 0;
    uint32_t frame_ = 0;
    bool ready_ = false;
    std::string why_;
};

}  // namespace pushmpc
