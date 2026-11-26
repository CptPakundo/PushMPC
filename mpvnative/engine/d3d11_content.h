// ContentDetect on D3D11 decoder textures (the engine module's zero-copy pushcore route): one compute pass makes the
// 480-wide cell grid of every frame and, about once a second, the image model's 224 x 224 thumbnail as packed words
// (content_detect.h, "GPU taps"); only those are read back (~0.5 MB a frame at 16:9 plus 196 KB a second), a few frames
// later through a staging ring, so the decoder thread never waits for the GPU. The frame itself never leaves the GPU.
// A decoder pool without shader access (BindFlags without SHADER_RESOURCE) is copied GPU-side into one texture of ours
// first (a GPU copy, no host memory).
#pragma once
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "content_tap.h"

namespace pme {

class D3D11ContentTap {
public:
    // fmt: DXGI_FORMAT_NV12 or DXGI_FORMAT_P010; limited: video range
    bool Init(ID3D11Device* dev, uint32_t w, uint32_t h, bool p010, bool limited, std::string& why)
    {
        using Microsoft::WRL::ComPtr;
        m_dev = dev; m_w = w; m_h = h; m_p010 = p010; m_limited = limited;
        m_cw = pushmpc::content::kCellsW; m_ch = pushmpc::content::CellsHeight(int(w), int(h));
        m_cells = uint32_t(m_cw * m_ch);
        const uint32_t words = m_cells + 224 * 224;
        ComPtr<ID3DBlob> cs, eb;
        if (FAILED(D3DCompile(kHlsl, sizeof(kHlsl) - 1, "pushmpc_content", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &eb))) {
            why = std::string("content pass: shader: ") + (eb ? static_cast<const char*>(eb->GetBufferPointer()) : "compile failed"); return false;
        }
        if (FAILED(dev->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr, &m_cs))) { why = "content pass: CreateComputeShader failed"; return false; }
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = words * 4; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = 4;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &m_out))) { why = "content pass: output buffer"; return false; }
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_UNKNOWN; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = words;
        if (FAILED(dev->CreateUnorderedAccessView(m_out.Get(), &ud, &m_uav))) { why = "content pass: UAV"; return false; }
        D3D11_BUFFER_DESC sd{};
        sd.ByteWidth = words * 4; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto& r : m_ring) if (FAILED(dev->CreateBuffer(&sd, nullptr, &r.staging))) { why = "content pass: staging buffer"; return false; }
        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth = sizeof(Params); cbd.Usage = D3D11_USAGE_DYNAMIC; cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev->CreateBuffer(&cbd, nullptr, &m_cb))) { why = "content pass: constants"; return false; }
        return true;
    }
    // the decoder thread, its lock held: record the pass for this frame, then hand finished older frames to the tap
    void Record(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, uint32_t slice, double t, bool disc, ContentTap& tap)
    {
        while (Collect(ctx, tap, false)) {}
        Slot& s = m_ring[m_next];
        if (s.busy) Collect(ctx, tap, true);   // the ring is full: the oldest must be read now (it finished long ago)
        uint32_t useSlice = slice;
        ID3D11Texture2D* src = Source(ctx, tex, slice, useSlice);
        if (!src) return;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> y, uv;
        if (!Views(src, useSlice, y, uv)) return;
        const bool clip = tap.WantClip(t);
        Params p{};
        p.w = m_w; p.h = m_h; p.cw = uint32_t(m_cw); p.ch = uint32_t(m_ch); p.slice = 0; p.maxCode = m_p010 ? 65535.f : 255.f;   // slice 0: the views start at the frame's array slice
        const uint32_t side = std::min(m_w, m_h);
        p.side = side; p.x0 = (m_w - side) / 2; p.y0 = (m_h - side) / 2;
        ID3D11ShaderResourceView* srv[2] = { y.Get(), uv.Get() };
        ID3D11UnorderedAccessView* uav = m_uav.Get();
        ctx->CSSetShader(m_cs.Get(), nullptr, 0);
        ctx->CSSetShaderResources(0, 2, srv);
        ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        for (uint32_t mode = 0; mode < (clip ? 2u : 1u); ++mode) {
            p.mode = mode; p.base = mode ? m_cells : 0;
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) break;
            std::memcpy(m.pData, &p, sizeof(p)); ctx->Unmap(m_cb.Get(), 0);
            ID3D11Buffer* cb = m_cb.Get();
            ctx->CSSetConstantBuffers(0, 1, &cb);
            ctx->Dispatch(((mode ? 224u * 224u : m_cells) + 63) / 64, 1, 1);
        }
        ID3D11ShaderResourceView* nsrv[2] = {}; ID3D11UnorderedAccessView* nuav = nullptr;
        ctx->CSSetShaderResources(0, 2, nsrv); ctx->CSSetUnorderedAccessViews(0, 1, &nuav, nullptr); ctx->CSSetShader(nullptr, nullptr, 0);
        D3D11_BOX box{ 0, 0, 0, (clip ? m_cells + 224 * 224 : m_cells) * 4, 1, 1 };
        ctx->CopySubresourceRegion(s.staging.Get(), 0, 0, 0, 0, m_out.Get(), 0, &box);
        s.busy = true; s.t = t; s.disc = disc; s.clip = clip; s.seq = m_seq++;
        m_next = (m_next + 1) % m_ring.size();
        ++m_recorded;
    }
    // everything recorded so far (end of stream, tests)
    void Drain(ID3D11DeviceContext* ctx, ContentTap& tap) { while (Collect(ctx, tap, true)) {} }
    uint64_t Recorded() const { return m_recorded; }
    uint64_t Copied() const { return m_copied; }
    // tests: every read-back slot's packed words (cells, then the thumbnail when it was made) before they go to the tap
    std::function<void(const uint32_t* words, size_t cells, bool clip)> onWords;

private:
    struct Params { uint32_t w, h, cw, ch, slice, mode, base, side, x0, y0; float maxCode; uint32_t pad; };
    struct Slot { Microsoft::WRL::ComPtr<ID3D11Buffer> staging; bool busy = false, disc = false, clip = false; double t = 0; uint64_t seq = 0; };

    // read the oldest recorded slot when the GPU has finished it (wait: block for it); in recording order
    bool Collect(ID3D11DeviceContext* ctx, ContentTap& tap, bool wait)
    {
        Slot* o = nullptr;
        for (auto& r : m_ring) if (r.busy && (!o || r.seq < o->seq)) o = &r;
        if (!o) return false;
        D3D11_MAPPED_SUBRESOURCE m{};
        HRESULT hr = ctx->Map(o->staging.Get(), 0, D3D11_MAP_READ, wait ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return false;
        o->busy = false;
        if (FAILED(hr)) { tap.Discontinuity(); return true; }
        const auto* words = static_cast<const uint32_t*>(m.pData);
        if (onWords) onWords(words, m_cells, o->clip);
        pushmpc::content::Cells c = pushmpc::content::CellsFromPacked(words, m_cw, m_cw, m_ch, m_limited);
        std::vector<float> in;
        if (o->clip) in = pushmpc::content::ClipInputFromPacked(words + m_cells, 224, m_h >= 720, m_limited);
        ctx->Unmap(o->staging.Get(), 0);
        if (!in.empty()) tap.OfferClip(std::move(in), o->t);
        if (o->disc) tap.Discontinuity();
        tap.Push(std::move(c), o->t, false);
        return true;
    }
    ID3D11Texture2D* Source(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, uint32_t slice, uint32_t& useSlice)
    {
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        if (d.BindFlags & D3D11_BIND_SHADER_RESOURCE) return tex;
        if (!m_copy || m_copyFmt != d.Format) {
            D3D11_TEXTURE2D_DESC c = d;
            c.ArraySize = 1; c.MipLevels = 1; c.BindFlags = D3D11_BIND_SHADER_RESOURCE; c.MiscFlags = 0; c.Usage = D3D11_USAGE_DEFAULT; c.CPUAccessFlags = 0;
            m_copy.Reset();
            if (FAILED(m_dev->CreateTexture2D(&c, nullptr, &m_copy))) return nullptr;
            m_copyFmt = d.Format;
        }
        ctx->CopySubresourceRegion(m_copy.Get(), 0, 0, 0, 0, tex, D3D11CalcSubresource(0, slice, d.MipLevels), nullptr);
        ++m_copied;
        useSlice = 0;
        return m_copy.Get();
    }
    bool Views(ID3D11Texture2D* tex, uint32_t slice, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& y, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& uv)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};
        v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        v.Texture2DArray.MipLevels = 1; v.Texture2DArray.FirstArraySlice = slice; v.Texture2DArray.ArraySize = 1;
        v.Format = m_p010 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
        if (FAILED(m_dev->CreateShaderResourceView(tex, &v, &y))) return false;
        v.Format = m_p010 ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
        return SUCCEEDED(m_dev->CreateShaderResourceView(tex, &v, &uv));
    }

    // mode 0: one thread a cell (the cell's luma mean over every pixel, the chroma point CellsFromYuv takes);
    // mode 1: one thread a thumbnail pixel (ClipInput's centre square, bilinear luma, chroma point). Integer cell edges:
    // floor(i * w / cw + 0.5) as (2 i w + cw) / (2 cw), exactly CellsFromYuv's lround.
    static constexpr char kHlsl[] = R"(
Texture2DArray<float> Y : register(t0);
Texture2DArray<float2> UV : register(t1);
RWStructuredBuffer<uint> O : register(u0);
cbuffer P : register(b0) { uint w, h, cw, ch, slice, mode, base, side, x0, y0; float maxCode; uint pad; };
uint Pack(float y8, float cb8, float cr8)
{
    return min(uint(y8 * 256.0 + 0.5), 65535u) | (min(uint(cb8 + 0.5), 255u) << 16) | (min(uint(cr8 + 0.5), 255u) << 24);
}
float L(int x, int y) { return Y.Load(int4(clamp(x, 0, int(w) - 1), clamp(y, 0, int(h) - 1), slice, 0)); }
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const float s8 = 255.0 / maxCode;   // a code of maxCode -> 255
    if (mode == 0) {
        if (id.x >= cw * ch) return;
        const uint i = id.x % cw, j = id.x / cw;
        const uint xs = (2 * i * w + cw) / (2 * cw), ys = (2 * j * h + ch) / (2 * ch);
        const uint xe = max((2 * (i + 1) * w + cw) / (2 * cw), xs + 1), ye = max((2 * (j + 1) * h + ch) / (2 * ch), ys + 1);
        uint sum = 0;
        for (uint yy = ys; yy < ye; ++yy)
            for (uint xx = xs; xx < xe; ++xx) sum += uint(Y.Load(int4(xx, yy, slice, 0)) * maxCode + 0.5);
        const float m = float(sum) / float((xe - xs) * (ye - ys));
        const uint cx = min((xs + xe) / 4, w / 2 - 1), cy = min((ys + ye) / 4, h / 2 - 1);
        const float2 c = UV.Load(int4(cx, cy, slice, 0)) * maxCode;
        O[base + id.x] = Pack(m * s8, round(c.x) * s8, round(c.y) * s8);
    } else {
        if (id.x >= 224 * 224) return;
        const uint i = id.x % 224, j = id.x / 224;
        const float fx = float(x0) + (float(i) + 0.5) * float(side) / 224.0 - 0.5, fy = float(y0) + (float(j) + 0.5) * float(side) / 224.0 - 0.5;
        const int ix = int(floor(fx)), iy = int(floor(fy));
        const float ax = fx - ix, ay = fy - iy;
        const float l = ((L(ix, iy) * (1 - ax) + L(ix + 1, iy) * ax) * (1 - ay) + (L(ix, iy + 1) * (1 - ax) + L(ix + 1, iy + 1) * ax) * ay) * maxCode;
        const uint cx = uint(clamp(int(fx) / 2, 0, int(w / 2) - 1)), cy = uint(clamp(int(fy) / 2, 0, int(h / 2) - 1));
        const float2 c = UV.Load(int4(cx, cy, slice, 0)) * maxCode;
        O[base + id.x] = Pack(l * s8, round(c.x) * s8, round(c.y) * s8);
    }
}
)";

    ID3D11Device* m_dev = nullptr;
    uint32_t m_w = 0, m_h = 0, m_cells = 0;
    int m_cw = 0, m_ch = 0;
    bool m_p010 = false, m_limited = true;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_cs;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_out, m_cb;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_uav;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_copy;
    DXGI_FORMAT m_copyFmt = DXGI_FORMAT_UNKNOWN;
    std::array<Slot, 4> m_ring;
    size_t m_next = 0;
    uint64_t m_seq = 0, m_recorded = 0, m_copied = 0;
};

} // namespace pme
