// PushMPC: the hold compose on generated frames (anime round 5; our own, not part of PushFPS). HoldPlan.h decides per
// pair (s-1, s) where a generated frame should show a real frame "as drawn"; this pass lays it in. One D3D11 compute
// pass per generated frame on the caller's device (recorded on a deferred context, executed on the immediate one):
//  - inside the plan's mask (cells, bilinear, feathered with a 9-tap tent) or over the whole picture (a flash, an impact
//    frame): the real frame A camera-warped to the frame's phase t (as drawn: a redrawn object pops to B's drawing at
//    B's own time), or the NEAREST real frame (A for t < 0.5, B from 0.5, also camera-warped: transients);
//  - line-art protection (kHoldLineArt): outside the held mask, the generated frame's line contrast (its high-pass
//    energy over 3 x 3 taps) is matched to the nearest real frame's, camera-warped: a softened line is restored (gain
//    1..1.6), a doubled line (much more line energy than the drawing has: an interpolator's double image) is replaced
//    by the nearest real frame there;
//  - the verdict split (PUSHMPC_SPLIT): the compose applies on one side of x = split only; a 2-pixel neutral divider.
// Out: a scratch texture of the frame's format (UAV), copied back over the generated frame. Pixels the pass leaves
// alone are copied unchanged (bit-exact).
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

inline constexpr const char* kHoldHlsl = R"HLSL(
cbuffer Params : register(b0) {
    uint2 size; float2 maskScale;      // pixel -> mask uv
    float2 cam; float camScale; float t; // camera of the pair at output scale (pixels), phase of this frame
    uint flags; float split; uint side; float feather;   // flags: 1 whole, 2 nearest, 4 line art, 8 mask, 16 divider
    float gainMax; float doubled; float2 tap;   // tap: one feather step in mask uv
    float tSwitch; float3 pad3;                  // nearest: B from this t (0.5; P9 film time: the drawing's film time)
};
Texture2D<float4> G : register(t0);
Texture2D<float4> A : register(t1);
Texture2D<float4> B : register(t2);
Texture2D<float> M : register(t3);
SamplerState S : register(s0);
RWTexture2D<float4> O : register(u0);

float2 Warp(float2 x, bool fromB)
{
    const float2 c = 0.5 * float2(size);
    const float st = 1.0 + t * (camScale - 1.0);
    const float2 p = c + (x - c - t * cam) / st;            // where the content at x was in A
    return fromB ? c + camScale * (p - c) + cam : p;       // ... and where it is in B
}
float4 Real(float2 x, bool fromB)
{
    const float2 uv = Warp(x, fromB) / float2(size);
    return fromB ? B.SampleLevel(S, uv, 0) : A.SampleLevel(S, uv, 0);
}
float Luma(float4 c) { return dot(c.rgb, float3(0.2126, 0.7152, 0.0722)); }

[numthreads(16, 16, 1)]
void CS_Hold(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= size.x || id.y >= size.y) return;
    const float2 x = float2(id.xy) + 0.5;
    const float4 g = G[id.xy];
    if ((flags & 16) && abs(x.x - split) < 1.0) { O[id.xy] = float4(0.5, 0.5, 0.5, g.a); return; }
    const bool inside = split < 0.0 || (side == 0 ? x.x < split : x.x >= split);
    if (!inside) { O[id.xy] = g; return; }
    const bool fromB = (flags & 2) != 0 && t >= tSwitch;
    float m = 0.0;
    if (flags & 1) m = 1.0;
    else if (flags & 8) {
        const float2 uv = x * maskScale;
        const float2 d = tap * 0.75;
        float s = 0.0, w = 0.0;
        [unroll] for (int j = -1; j <= 1; ++j) [unroll] for (int i = -1; i <= 1; ++i) {
            const float k = (2.0 - abs(i)) * (2.0 - abs(j));
            s += k * M.SampleLevel(S, uv + float2(i, j) * d, 0); w += k;
        }
        m = smoothstep(0.15, 0.85, s / w);
    }
    float4 o = g;
    if (m > 0.0) o = lerp(g, Real(x, fromB), m);
    if ((flags & 4) && m < 1.0) {
        // line contrast: high-pass of g and of the nearest real frame (camera-warped) over 3 x 3 taps
        const bool nb = t >= 0.5;
        float eg = 0.0, er = 0.0;
        const float lg = Luma(g), lr = Luma(Real(x, nb));
        [unroll] for (int j = -1; j <= 1; ++j) [unroll] for (int i = -1; i <= 1; ++i) {
            if (i == 0 && j == 0) continue;
            const float2 q = x + float2(i, j);
            const float hg = lg - Luma(G[uint2(clamp(q, 0.5, float2(size) - 0.5))]);
            const float hr = lr - Luma(Real(q, nb));
            eg += hg * hg; er += hr * hr;
        }
        const float floorE = 1e-5;
        if (eg > floorE || er > floorE) {
            const float ratio = sqrt((er + floorE) / (eg + floorE));
            if (ratio < doubled) {
                // far more line energy than the drawing has: a doubled line; the nearest real frame there
                o = lerp(o, Real(x, nb), 1.0 - m);
            } else if (ratio > 1.0) {
                // a softened line: its high-pass scaled up (luma direction), at most gainMax
                const float4 avg = (G[uint2(clamp(x + float2(1, 0), 0.5, float2(size) - 0.5))] + G[uint2(clamp(x - float2(1, 0), 0.5, float2(size) - 0.5))] +
                                    G[uint2(clamp(x + float2(0, 1), 0.5, float2(size) - 0.5))] + G[uint2(clamp(x - float2(0, 1), 0.5, float2(size) - 0.5))]) * 0.25;
                const float k = min(ratio, gainMax) - 1.0;
                o.rgb = lerp(o.rgb, o.rgb + k * (g.rgb - avg.rgb), 1.0 - m);
            }
        }
    }
    O[id.xy] = float4(o.rgb, g.a);
}
)HLSL";

class HoldCompose {
public:
    enum : uint32_t { kWhole = 1, kNearest = 2, kLineArt = 4, kMask = 8, kDivider = 16 };
    struct Pass {
        uint32_t flags = 0;
        float camX = 0, camY = 0, camScale = 1.f;   // pixels at the frame's size
        float t = 0.5f;
        float tSwitch = 0.5f;                        // nearest: B from this t (P9 film time)
        float split = -1.f;                          // pixel x, < 0 = whole width
        uint32_t side = 0;                           // 0: x < split, 1: x >= split
        float gainMax = 1.6f, doubled = 0.6f;       // line art: highest gain; energy ratio below which a line is doubled
    };
    bool init(ID3D11Device* device)
    {
        dev_ = device;
        Microsoft::WRL::ComPtr<ID3DBlob> b, err;
        if (FAILED(D3DCompile(kHoldHlsl, std::strlen(kHoldHlsl), "PushMpcHold", nullptr, nullptr, "CS_Hold", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &err)))
            return failWith(std::string("compile CS_Hold: ") + (err ? static_cast<const char*>(err->GetBufferPointer()) : "?"));
        if (FAILED(dev_->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &cs_))) return failWith("CreateComputeShader(hold)");
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 80; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(dev_->CreateBuffer(&bd, nullptr, &cb_))) return failWith("CreateBuffer(hold)");
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(dev_->CreateSamplerState(&sd, &sampler_))) return failWith("CreateSamplerState(hold)");
        if (FAILED(dev_->CreateDeferredContext(0, &deferred_))) return failWith("CreateDeferredContext(hold)");
        ready_ = true;
        return true;
    }
    bool ready() const { return ready_; }
    const std::string& why() const { return why_; }
    void forgetViews() { srvs_.clear(); scratch_.Reset(); scratchUav_.Reset(); }
    // the plan's mask (w x h cells, 0 / 255); coverW / coverH: the share of the frame the cell grid covers (grid cells x
    // cell pixels / source size, ~1); feather in cells
    bool setMask(ID3D11DeviceContext* imm, const std::vector<uint8_t>& mask, uint32_t w, uint32_t h, float coverW, float coverH, float featherCells)
    {
        if (!ready_ || !w || !h || mask.size() != size_t(w) * h) return false;
        if (!maskTex_ || w != mw_ || h != mh_) {
            maskTex_.Reset(); maskSrv_.Reset();
            D3D11_TEXTURE2D_DESC d{};
            d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8_UNORM; d.SampleDesc = {1, 0};
            d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(dev_->CreateTexture2D(&d, nullptr, &maskTex_)) || FAILED(dev_->CreateShaderResourceView(maskTex_.Get(), nullptr, &maskSrv_))) return failWith("hold mask texture");
            mw_ = w; mh_ = h;
        }
        coverW_ = coverW > 0.f ? coverW : 1.f; coverH_ = coverH > 0.f ? coverH : 1.f;
        feather_ = std::clamp(featherCells, 0.25f, 3.0f);
        imm->UpdateSubresource(maskTex_.Get(), 0, nullptr, mask.data(), w, 0);
        return true;
    }
    // g (a generated frame) composed in place from a / b (the pair's real frames; same size and format family)
    bool compose(ID3D11DeviceContext* imm, ID3D11Texture2D* g, ID3D11Texture2D* a, ID3D11Texture2D* b, const Pass& p)
    {
        if (!ready_ || !g || !a || !b) return false;
        D3D11_TEXTURE2D_DESC d{}; g->GetDesc(&d);
        if (!scratch_ || d.Width != sw_ || d.Height != sh_ || d.Format != sf_) {
            scratch_.Reset(); scratchUav_.Reset();
            D3D11_TEXTURE2D_DESC s = d;
            s.MipLevels = 1; s.ArraySize = 1; s.SampleDesc = {1, 0}; s.Usage = D3D11_USAGE_DEFAULT; s.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
            s.CPUAccessFlags = 0; s.MiscFlags = 0;
            if (FAILED(dev_->CreateTexture2D(&s, nullptr, &scratch_)) || FAILED(dev_->CreateUnorderedAccessView(scratch_.Get(), nullptr, &scratchUav_)))
                return failWith("hold scratch texture (format " + std::to_string(int(d.Format)) + ")");
            sw_ = d.Width; sh_ = d.Height; sf_ = d.Format;
        }
        ID3D11ShaderResourceView* sg = srv(g); ID3D11ShaderResourceView* sa = srv(a); ID3D11ShaderResourceView* sb = srv(b);
        if (!sg || !sa || !sb) return false;
        if ((p.flags & kMask) && !maskSrv_) return false;
        struct P { uint32_t w, h; float msx, msy; float cx, cy, cs, t; uint32_t flags; float split; uint32_t side; float feather; float gainMax, doubled, pad[2]; float tSwitch, pad3[3]; } c{};
        static_assert(sizeof(P) == 80, "hold constants");
        c.w = d.Width; c.h = d.Height;
        c.msx = coverW_ / float(d.Width); c.msy = coverH_ / float(d.Height);
        c.cx = p.camX; c.cy = p.camY; c.cs = p.camScale > 0.f ? p.camScale : 1.f; c.t = p.t;
        c.flags = p.flags; c.split = p.split; c.side = p.side;
        c.feather = feather_;
        c.gainMax = p.gainMax; c.doubled = p.doubled; c.tSwitch = p.tSwitch;
        c.pad[0] = feather_ / float(std::max(1u, mw_)); c.pad[1] = feather_ / float(std::max(1u, mh_));
        imm->UpdateSubresource(cb_.Get(), 0, nullptr, &c, 0, 0);
        ID3D11Buffer* cbs[] = { cb_.Get() };
        ID3D11SamplerState* ss[] = { sampler_.Get() };
        ID3D11ShaderResourceView* srvs[] = { sg, sa, sb, maskSrv_ ? maskSrv_.Get() : sg };
        ID3D11UnorderedAccessView* u = scratchUav_.Get();
        deferred_->CSSetConstantBuffers(0, 1, cbs);
        deferred_->CSSetSamplers(0, 1, ss);
        deferred_->CSSetShaderResources(0, 4, srvs);
        deferred_->CSSetUnorderedAccessViews(0, 1, &u, nullptr);
        deferred_->CSSetShader(cs_.Get(), nullptr, 0);
        deferred_->Dispatch((d.Width + 15) / 16, (d.Height + 15) / 16, 1);
        ID3D11ShaderResourceView* none[4] = {};
        ID3D11UnorderedAccessView* noU = nullptr;
        deferred_->CSSetShaderResources(0, 4, none);
        deferred_->CSSetUnorderedAccessViews(0, 1, &noU, nullptr);
        deferred_->CopyResource(g, scratch_.Get());
        Microsoft::WRL::ComPtr<ID3D11CommandList> list;
        if (FAILED(deferred_->FinishCommandList(FALSE, &list))) return false;
        imm->ExecuteCommandList(list.Get(), TRUE);
        return true;
    }

private:
    bool failWith(std::string w) { why_ = std::move(w); ready_ = false; return false; }
    ID3D11ShaderResourceView* srv(ID3D11Texture2D* t)
    {
        if (auto it = srvs_.find(t); it != srvs_.end()) return it->second.Get();
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> v;
        if (FAILED(dev_->CreateShaderResourceView(t, nullptr, &v))) return nullptr;
        if (srvs_.size() > 64) srvs_.clear();
        return (srvs_[t] = v).Get();
    }
    Microsoft::WRL::ComPtr<ID3D11Device> dev_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> deferred_;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> cs_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> maskTex_, scratch_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> maskSrv_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> scratchUav_;
    std::unordered_map<ID3D11Texture2D*, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> srvs_;
    uint32_t mw_ = 0, mh_ = 0, sw_ = 0, sh_ = 0;
    DXGI_FORMAT sf_ = DXGI_FORMAT_UNKNOWN;
    float coverW_ = 1.f, coverH_ = 1.f, feather_ = 2.0f;
    bool ready_ = false;
    std::string why_;
};

}  // namespace pushmpc
