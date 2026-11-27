// SPDX-License-Identifier: GPL-3.0-or-later
// PushMPC: NIS as the output upscaler of the Direct3D 11 video output fork (see pm_nis_d3d11.h).
#include <vlc_common.h>
#include <d3dcompiler.h>
#include <new>
#include <wrl/client.h>
#include <cmath>
#include <cstring>

#include "pm_nis_d3d11.h"
#include "NIS_Config.h"      // NISConfig, NVScalerUpdateConfig, coef_scale / coef_usm (MIT)
#include "pm_nis_shader.h"   // generated at build time: kPmNisScalerHlsl = NIS_Scaler.h

using Microsoft::WRL::ComPtr;

typedef HRESULT (WINAPI *pD3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR,
                                      UINT, UINT, ID3DBlob **, ID3DBlob **);

namespace {
// pass A: the picture's planes -> RGB (gamma-encoded, as displayed) at the intermediate size, bilinear when it differs
const char kConvertHlsl[] = R"(
cbuffer C : register(b0) {
    float4 rowR; float4 rowG; float4 rowB;   // xyz: matrix row, w: offset (after the range expansion)
    float2 srcOrigin; float2 srcSize;          // visible area, in source texels
    float2 texSize; float2 dstSize;            // the plane-0 texture size; the intermediate size
    float yScale; float yOffset; float cScale; float cOffset;
    uint planes; uint3 pad;
};
SamplerState lin : register(s0);
Texture2D<float4> p0 : register(t0);
Texture2D<float4> p1 : register(t1);
Texture2D<float4> p2 : register(t2);
RWTexture2D<float4> outRgb : register(u0);
[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)dstSize.x || id.y >= (uint)dstSize.y) return;
    float2 pos = (srcOrigin + (float2(id.xy) + 0.5) * srcSize / dstSize) / texSize;
    float3 c;
    if (planes == 1) {
        c = p0.SampleLevel(lin, pos, 0).rgb;
    } else {
        float y = p0.SampleLevel(lin, pos, 0).r;
        float2 uv = planes == 2 ? p1.SampleLevel(lin, pos, 0).rg : float2(p1.SampleLevel(lin, pos, 0).r, p2.SampleLevel(lin, pos, 0).r);
        float3 yuv = float3((y - yOffset) * yScale, (uv - cOffset) * cScale);
        c = float3(dot(rowR.xyz, yuv), dot(rowG.xyz, yuv), dot(rowB.xyz, yuv));
    }
    outRgb[id.xy] = float4(saturate(c), 1.0);
}
)";

// pass B: NIS (NVScaler) from the intermediate RGB into the output's viewport
const char kNisMain[] = R"(
cbuffer cb : register(b0) {
    float kDetectRatio; float kDetectThres; float kMinContrastRatio; float kRatioNorm;
    float kContrastBoost; float kEps; float kSharpStartY; float kSharpScaleY;
    float kSharpStrengthMin; float kSharpStrengthScale; float kSharpLimitMin; float kSharpLimitScale;
    float kScaleX; float kScaleY; float kDstNormX; float kDstNormY;
    float kSrcNormX; float kSrcNormY;
    uint kInputViewportOriginX; uint kInputViewportOriginY; uint kInputViewportWidth; uint kInputViewportHeight;
    uint kOutputViewportOriginX; uint kOutputViewportOriginY; uint kOutputViewportWidth; uint kOutputViewportHeight;
    float reserved0; float reserved1;
};
SamplerState samplerLinearClamp : register(s0);
Texture2D in_texture : register(t0);
RWTexture2D<float4> out_texture : register(u0);
Texture2D coef_scaler : register(t1);
Texture2D coef_usm : register(t2);
#include "NIS_Scaler.h"
[numthreads(NIS_THREAD_GROUP_SIZE, 1, 1)]
void main(uint3 blockIdx : SV_GroupID, uint3 threadIdx : SV_GroupThreadID)
{
    NVScaler(blockIdx.xy, threadIdx.x);
}
)";

struct NisInclude : public ID3DInclude {
    HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID *data, UINT *bytes) override
    {
        if (strcmp(name, "NIS_Scaler.h") != 0) return E_FAIL;
        *data = kPmNisScalerHlsl;
        *bytes = (UINT)strlen(kPmNisScalerHlsl);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Close(LPCVOID) override { return S_OK; }
};

struct ConvertCB {
    float rowR[4], rowG[4], rowB[4];
    float srcOrigin[2], srcSize[2];
    float texSize[2], dstSize[2];
    float yScale, yOffset, cScale, cOffset;
    uint32_t planes, pad[3];
};
} // namespace

struct pm_nis {
    ComPtr<ID3D11ComputeShader> convert, nis;
    ComPtr<ID3D11Buffer> convertCB, nisCB;
    ComPtr<ID3D11SamplerState> linear;
    ComPtr<ID3D11Texture2D> coefScale, coefUsm;
    ComPtr<ID3D11ShaderResourceView> coefScaleSRV, coefUsmSRV;
    ComPtr<ID3D11Texture2D> mid;            // the intermediate RGB
    ComPtr<ID3D11ShaderResourceView> midSRV;
    ComPtr<ID3D11UnorderedAccessView> midUAV;
    unsigned midW = 0, midH = 0;
    ComPtr<ID3D11Texture2D> nisOut;         // NIS's own output, placement size
    ComPtr<ID3D11UnorderedAccessView> nisUAV;
    unsigned nisW = 0, nisH = 0;
    ComPtr<ID3D11Texture2D> out;            // the window-size output
    ComPtr<ID3D11ShaderResourceView> outSRV;
    ComPtr<ID3D11UnorderedAccessView> outUAV;
    unsigned outW = 0, outH = 0;
    vout_display_place_t lastPlace{};
};

static bool Compile(vlc_object_t *o, ID3D11Device *dev, const char *src, size_t len, const D3D_SHADER_MACRO *defs,
                    ID3DInclude *inc, ComPtr<ID3D11ComputeShader> &cs)
{
    static HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
    pD3DCompile compile = dll ? (pD3DCompile)(void *)GetProcAddress(dll, "D3DCompile") : nullptr;
    if (!compile) { msg_Err(o, "PushMPC NIS: d3dcompiler_47.dll is missing"); return false; }
    ComPtr<ID3DBlob> code, errs;
    HRESULT hr = compile(src, len, "pushmpc_nis", defs, inc, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errs);
    if (FAILED(hr)) {
        msg_Err(o, "PushMPC NIS: shader: %s", errs ? (const char *)errs->GetBufferPointer() : "compile failed");
        return false;
    }
    hr = dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, cs.GetAddressOf());
    return SUCCEEDED(hr);
}

static bool CoefTexture(ID3D11Device *dev, const float (*coef)[kFilterSize], ComPtr<ID3D11Texture2D> &tex,
                        ComPtr<ID3D11ShaderResourceView> &srv)
{
    // kPhaseCount rows of kFilterSize (8: 6 taps + padding) values = 2 float4 texels per row, as NIS reads them
    static_assert(kFilterSize == 8, "NIS coefficient layout");
    const float (*data)[kFilterSize] = coef;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = 2; d.Height = (UINT)kPhaseCount; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = { data, (UINT)(sizeof(float) * kFilterSize), 0 };
    if (FAILED(dev->CreateTexture2D(&d, &init, tex.GetAddressOf()))) return false;
    return SUCCEEDED(dev->CreateShaderResourceView(tex.Get(), nullptr, srv.GetAddressOf()));
}

pm_nis *PM_NisCreate(vlc_object_t *o, d3d11_device_t *d)
{
    auto *n = new (std::nothrow) pm_nis();
    if (!n) return nullptr;
    ID3D11Device *dev = d->d3ddevice;
    char bw[16], bh[16], tg[16];
    snprintf(bw, sizeof(bw), "%u", 32u); snprintf(bh, sizeof(bh), "%u", 24u); snprintf(tg, sizeof(tg), "%u", 128u);
    const D3D_SHADER_MACRO nisDefs[] = {
        { "NIS_HLSL", "1" }, { "NIS_SCALER", "1" }, { "NIS_HDR_MODE", "0" }, { "NIS_VIEWPORT_SUPPORT", "1" },
        { "NIS_USE_HALF_PRECISION", "0" }, { "NIS_BLOCK_WIDTH", bw }, { "NIS_BLOCK_HEIGHT", bh },
        { "NIS_THREAD_GROUP_SIZE", tg }, { nullptr, nullptr } };
    NisInclude inc;
    D3D11_BUFFER_DESC cb = {};
    cb.Usage = D3D11_USAGE_DYNAMIC; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    bool ok = Compile(o, dev, kConvertHlsl, sizeof(kConvertHlsl) - 1, nullptr, nullptr, n->convert)
           && Compile(o, dev, kNisMain, sizeof(kNisMain) - 1, nisDefs, &inc, n->nis);
    cb.ByteWidth = sizeof(ConvertCB);
    ok = ok && SUCCEEDED(dev->CreateBuffer(&cb, nullptr, n->convertCB.GetAddressOf()));
    cb.ByteWidth = 256;   // NISConfig (aligned 256)
    ok = ok && SUCCEEDED(dev->CreateBuffer(&cb, nullptr, n->nisCB.GetAddressOf()));
    ok = ok && SUCCEEDED(dev->CreateSamplerState(&sd, n->linear.GetAddressOf()));
    ok = ok && CoefTexture(dev, coef_scale, n->coefScale, n->coefScaleSRV)
            && CoefTexture(dev, coef_usm, n->coefUsm, n->coefUsmSRV);
    if (!ok) { delete n; return nullptr; }
    msg_Dbg(o, "PushMPC NIS upscaler ready");
    return n;
}

void PM_NisDestroy(pm_nis *n) { delete n; }

int PM_NisResize(vlc_object_t *o, pm_nis *n, d3d11_device_t *d, unsigned w, unsigned h,
                 ID3D11Texture2D **out_texture, ID3D11ShaderResourceView **out_srv)
{
    if (n->out && n->outW == w && n->outH == h) {
        *out_texture = n->out.Get(); *out_srv = n->outSRV.Get();
        return VLC_SUCCESS;
    }
    n->out.Reset(); n->outSRV.Reset(); n->outUAV.Reset();
    D3D11_TEXTURE2D_DESC t = {};
    t.Width = w; t.Height = h; t.MipLevels = 1; t.ArraySize = 1; t.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    t.SampleDesc.Count = 1; t.Usage = D3D11_USAGE_DEFAULT;
    t.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
    ID3D11Device *dev = d->d3ddevice;
    if (FAILED(dev->CreateTexture2D(&t, nullptr, n->out.GetAddressOf())) ||
        FAILED(dev->CreateShaderResourceView(n->out.Get(), nullptr, n->outSRV.GetAddressOf())) ||
        FAILED(dev->CreateUnorderedAccessView(n->out.Get(), nullptr, n->outUAV.GetAddressOf()))) {
        msg_Err(o, "PushMPC NIS: cannot create the %ux%u output", w, h);
        return VLC_EGENERIC;
    }
    n->outW = w; n->outH = h;
    n->lastPlace = vout_display_place_t{};
    *out_texture = n->out.Get(); *out_srv = n->outSRV.Get();
    return VLC_SUCCESS;
}

static void Matrix(const video_format_t *f, ConvertCB &c)
{
    // Kr / Kb per colour space; the source's range expands to 0..1 first
    float kr = 0.2126f, kb = 0.0722f;
    if (f->space == COLOR_SPACE_BT601 || (f->space == COLOR_SPACE_UNDEF && f->i_visible_height < 600)) { kr = 0.299f; kb = 0.114f; }
    else if (f->space == COLOR_SPACE_BT2020) { kr = 0.2627f; kb = 0.0593f; }
    const float kg = 1.f - kr - kb;
    const float r_v = 2.f * (1.f - kr), b_u = 2.f * (1.f - kb);
    const float g_u = -b_u * kb / kg, g_v = -r_v * kr / kg;
    const float R[4] = { 1.f, 0.f, r_v, 0.f }, G[4] = { 1.f, g_u, g_v, 0.f }, B[4] = { 1.f, b_u, 0.f, 0.f };
    memcpy(c.rowR, R, sizeof(R)); memcpy(c.rowG, G, sizeof(G)); memcpy(c.rowB, B, sizeof(B));
    if (f->b_color_range_full) { c.yScale = 1.f; c.yOffset = 0.f; c.cScale = 1.f; c.cOffset = 128.f / 255.f; }
    else { c.yScale = 255.f / 219.f; c.yOffset = 16.f / 255.f; c.cScale = 255.f / 224.f; c.cOffset = 128.f / 255.f; }
}

int PM_NisProcess(vlc_object_t *o, pm_nis *n, d3d11_device_t *d, ID3D11ShaderResourceView *const planes[], int count,
                  const video_format_t *src, const vout_display_place_t *place, float sharpness)
{
    if (!n->out || count < 1 || count > 3 || !planes[0] || place->width == 0 || place->height == 0) return VLC_EGENERIC;
    for (int i = 0; i < count; i++) if (!planes[i]) return VLC_EGENERIC;
    ID3D11Device *dev = d->d3ddevice;
    ID3D11DeviceContext *ctx = d->d3dcontext;
    const unsigned sw = src->i_visible_width, sh = src->i_visible_height;
    if (place->width < sw || place->height < sh) return VLC_EGENERIC;   // NIS upscales; smaller windows keep the quad
    // NIS covers 1x..2x: above that the conversion pass pre-scales (bilinear) to half the output size
    const unsigned mw = place->width > 2 * sw ? (place->width + 1) / 2 : sw;
    const unsigned mh = place->height > 2 * sh ? (place->height + 1) / 2 : sh;
    if (!n->mid || n->midW != mw || n->midH != mh) {
        n->mid.Reset(); n->midSRV.Reset(); n->midUAV.Reset();
        D3D11_TEXTURE2D_DESC t = {};
        t.Width = mw; t.Height = mh; t.MipLevels = 1; t.ArraySize = 1; t.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        t.SampleDesc.Count = 1; t.Usage = D3D11_USAGE_DEFAULT; t.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(dev->CreateTexture2D(&t, nullptr, n->mid.GetAddressOf())) ||
            FAILED(dev->CreateShaderResourceView(n->mid.Get(), nullptr, n->midSRV.GetAddressOf())) ||
            FAILED(dev->CreateUnorderedAccessView(n->mid.Get(), nullptr, n->midUAV.GetAddressOf())))
            return VLC_EGENERIC;
        n->midW = mw; n->midH = mh;
    }
    // the plane-0 texture's size (decoder pictures are padded, maybe slices of an array)
    ComPtr<ID3D11Resource> res;
    planes[0]->GetResource(res.GetAddressOf());
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(res.As(&tex))) return VLC_EGENERIC;
    D3D11_TEXTURE2D_DESC td;
    tex->GetDesc(&td);

    ConvertCB c = {};
    Matrix(src, c);
    c.srcOrigin[0] = (float)src->i_x_offset; c.srcOrigin[1] = (float)src->i_y_offset;
    c.srcSize[0] = (float)sw; c.srcSize[1] = (float)sh;
    c.texSize[0] = (float)td.Width; c.texSize[1] = (float)td.Height;
    c.dstSize[0] = (float)mw; c.dstSize[1] = (float)mh;
    c.planes = (uint32_t)count;

    // NIS writes whole blocks (32 x 24), past its output viewport: it draws into a placement-size texture, which is then
    // copied into the window texture at the placement, so the letterbox bars stay untouched
    if (!n->nisOut || n->nisW != place->width || n->nisH != place->height) {
        n->nisOut.Reset(); n->nisUAV.Reset();
        D3D11_TEXTURE2D_DESC t = {};
        t.Width = place->width; t.Height = place->height; t.MipLevels = 1; t.ArraySize = 1;
        t.Format = DXGI_FORMAT_R8G8B8A8_UNORM; t.SampleDesc.Count = 1; t.Usage = D3D11_USAGE_DEFAULT;
        t.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(dev->CreateTexture2D(&t, nullptr, n->nisOut.GetAddressOf())) ||
            FAILED(dev->CreateUnorderedAccessView(n->nisOut.Get(), nullptr, n->nisUAV.GetAddressOf())))
            return VLC_EGENERIC;
        n->nisW = place->width; n->nisH = place->height;
    }
    if (place->x + place->width > n->outW || place->y + place->height > n->outH) return VLC_EGENERIC;
    NISConfig cfg = {};
    if (!NVScalerUpdateConfig(cfg, sharpness, 0, 0, mw, mh, mw, mh, 0, 0, place->width, place->height,
                              place->width, place->height, NISHDRMode::None))
        return VLC_EGENERIC;

    d3d11_device_lock(d);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(n->convertCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &c, sizeof(c)); ctx->Unmap(n->convertCB.Get(), 0); }
    if (SUCCEEDED(ctx->Map(n->nisCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &cfg, sizeof(cfg)); ctx->Unmap(n->nisCB.Get(), 0); }
    if (memcmp(&n->lastPlace, place, sizeof(*place)) != 0) {   // letterbox bars: clear once per new placement
        const float black[4] = { 0.f, 0.f, 0.f, 1.f };
        ctx->ClearUnorderedAccessViewFloat(n->outUAV.Get(), black);
        n->lastPlace = *place;
    }
    ID3D11SamplerState *smp = n->linear.Get();
    ID3D11ShaderResourceView *nullSRV[3] = {};
    ID3D11UnorderedAccessView *nullUAV = nullptr;
    // pass A
    ID3D11ShaderResourceView *in[3] = { planes[0], count > 1 ? planes[1] : planes[0], count > 2 ? planes[2] : planes[0] };
    ctx->CSSetShader(n->convert.Get(), nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, n->convertCB.GetAddressOf());
    ctx->CSSetSamplers(0, 1, &smp);
    ctx->CSSetShaderResources(0, 3, in);
    ctx->CSSetUnorderedAccessViews(0, 1, n->midUAV.GetAddressOf(), nullptr);
    ctx->Dispatch((mw + 15) / 16, (mh + 15) / 16, 1);
    ctx->CSSetShaderResources(0, 3, nullSRV);
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
    // pass B
    ID3D11ShaderResourceView *nisIn[3] = { n->midSRV.Get(), n->coefScaleSRV.Get(), n->coefUsmSRV.Get() };
    ctx->CSSetShader(n->nis.Get(), nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, n->nisCB.GetAddressOf());
    ctx->CSSetShaderResources(0, 3, nisIn);
    ctx->CSSetUnorderedAccessViews(0, 1, n->nisUAV.GetAddressOf(), nullptr);
    ctx->Dispatch((place->width + 31) / 32, (place->height + 23) / 24, 1);
    ctx->CSSetShaderResources(0, 3, nullSRV);
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
    ctx->CSSetShader(nullptr, nullptr, 0);
    const D3D11_BOX box = { 0, 0, 0, place->width, place->height, 1 };
    ctx->CopySubresourceRegion(n->out.Get(), 0, place->x, place->y, 0, n->nisOut.Get(), 0, &box);
    d3d11_device_unlock(d);
    (void)o;
    return VLC_SUCCESS;
}

int PM_NisConvert(vlc_object_t *o, pm_nis *n, d3d11_device_t *d, ID3D11ShaderResourceView *const planes[], int count,
                  const video_format_t *src, const vout_display_place_t *place)
{
    if (!n->out || count < 1 || count > 3 || !planes[0] || place->width == 0 || place->height == 0) return VLC_EGENERIC;
    for (int i = 0; i < count; i++) if (!planes[i]) return VLC_EGENERIC;
    if (place->x + place->width > n->outW || place->y + place->height > n->outH) return VLC_EGENERIC;
    ID3D11Device *dev = d->d3ddevice;
    ID3D11DeviceContext *ctx = d->d3dcontext;
    // the placement-size texture (as NIS's pass B uses it), then copied into the window texture at the placement
    if (!n->nisOut || n->nisW != place->width || n->nisH != place->height) {
        n->nisOut.Reset(); n->nisUAV.Reset();
        D3D11_TEXTURE2D_DESC t = {};
        t.Width = place->width; t.Height = place->height; t.MipLevels = 1; t.ArraySize = 1;
        t.Format = DXGI_FORMAT_R8G8B8A8_UNORM; t.SampleDesc.Count = 1; t.Usage = D3D11_USAGE_DEFAULT;
        t.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(dev->CreateTexture2D(&t, nullptr, n->nisOut.GetAddressOf())) ||
            FAILED(dev->CreateUnorderedAccessView(n->nisOut.Get(), nullptr, n->nisUAV.GetAddressOf())))
            return VLC_EGENERIC;
        n->nisW = place->width; n->nisH = place->height;
    }
    ComPtr<ID3D11Resource> res;
    planes[0]->GetResource(res.GetAddressOf());
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(res.As(&tex))) return VLC_EGENERIC;
    D3D11_TEXTURE2D_DESC td;
    tex->GetDesc(&td);

    ConvertCB c = {};
    Matrix(src, c);
    c.srcOrigin[0] = (float)src->i_x_offset; c.srcOrigin[1] = (float)src->i_y_offset;
    c.srcSize[0] = (float)src->i_visible_width; c.srcSize[1] = (float)src->i_visible_height;
    c.texSize[0] = (float)td.Width; c.texSize[1] = (float)td.Height;
    c.dstSize[0] = (float)place->width; c.dstSize[1] = (float)place->height;
    c.planes = (uint32_t)count;

    d3d11_device_lock(d);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(n->convertCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &c, sizeof(c)); ctx->Unmap(n->convertCB.Get(), 0); }
    if (memcmp(&n->lastPlace, place, sizeof(*place)) != 0) {   // letterbox bars: clear once per new placement
        const float black[4] = { 0.f, 0.f, 0.f, 1.f };
        ctx->ClearUnorderedAccessViewFloat(n->outUAV.Get(), black);
        n->lastPlace = *place;
    }
    ID3D11SamplerState *smp = n->linear.Get();
    ID3D11ShaderResourceView *nullSRV[3] = {};
    ID3D11UnorderedAccessView *nullUAV = nullptr;
    ID3D11ShaderResourceView *in[3] = { planes[0], count > 1 ? planes[1] : planes[0], count > 2 ? planes[2] : planes[0] };
    ctx->CSSetShader(n->convert.Get(), nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, n->convertCB.GetAddressOf());
    ctx->CSSetSamplers(0, 1, &smp);
    ctx->CSSetShaderResources(0, 3, in);
    ctx->CSSetUnorderedAccessViews(0, 1, n->nisUAV.GetAddressOf(), nullptr);
    ctx->Dispatch((place->width + 15) / 16, (place->height + 15) / 16, 1);
    ctx->CSSetShaderResources(0, 3, nullSRV);
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
    ctx->CSSetShader(nullptr, nullptr, 0);
    const D3D11_BOX box = { 0, 0, 0, place->width, place->height, 1 };
    ctx->CopySubresourceRegion(n->out.Get(), 0, place->x, place->y, 0, n->nisOut.Get(), 0, &box);
    d3d11_device_unlock(d);
    (void)o;
    return VLC_SUCCESS;
}
