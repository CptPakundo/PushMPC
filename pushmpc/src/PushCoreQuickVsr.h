// Fast start, Route B (PushMPC 1.0, PUSHCORE_FEATURE_QUICK_VSR): the quick graph's upscaler is the driver's RTX Video
// Super Resolution, reached through the D3D11 video processor and NVIDIA's stream extension (the way browsers and mpv's
// d3d11vpp scaling-mode=nvidia ask for it). It needs no NGX core, so it starts in milliseconds while the full graph
// (RTX VSR through NGX, ~2 s to initialise) is still opening; the first frames are RTX-upscaled instead of NVIDIA Image
// Scaling.
//
// Where it runs: pushcore's ingress on the caller's D3D11 device. The source slice is copied into a private input
// texture (the decoder's own surfaces are never held), the video processor scales it to the size the full graph's SR
// would produce (srStageExtents(source, target).evaluated) straight into the ingress ring slot (NV12 -> NV12, P010 ->
// P010, the same YCbCr colour space in and out, so the processor only scales), and the graph then runs on that size
// with its own SR stage off. The full graph's output has the same size, so the handover is seamless.
//
// Whether the driver applied VSR cannot be queried. The open checks it once per process, adapter, size and format: a
// detailed test picture goes through two video processors, one with the extension and one without; when both outputs
// are the same (the NVIDIA Control Panel's "Super resolution" is off, an older driver, not an RTX GPU), Route B is not
// used and the quick graph keeps NVIDIA Image Scaling. PUSHMPC_QUICK_VSR_NOPROBE=1 skips the check (tests).
#pragma once
#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

namespace pushmpc {

// NVIDIA's video processor extension ("PPE" interface): {version 1, method 2 = super resolution, enable}
inline constexpr GUID kNvidiaPpeGuid = { 0xd43ce1b3, 0x1f4b, 0x48ac, { 0xba, 0xee, 0xc3, 0xc2, 0x53, 0x75, 0xe6, 0xf7 } };
struct NvidiaPpeExt { UINT version; UINT method; UINT enable; };

class QuickVsr {
public:
    bool active() const { return m_active; }
    uint32_t outWidth() const { return m_outW; }
    uint32_t outHeight() const { return m_outH; }
    const std::string& why() const { return m_why; }

    // srcW/srcH = the visible source; outW/outH = the SR target size. range/matrix/transfer: FFmpeg enum values.
    bool init(ID3D11Device* dev, ID3D11DeviceContext* imm, uint32_t srcW, uint32_t srcH, uint32_t outW, uint32_t outH,
              bool p010, int range, int matrix, int transfer)
    {
        reset();
        m_srcW = srcW; m_srcH = srcH; m_outW = outW; m_outH = outH;
        m_fmt = p010 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
        m_cs = colorSpace(range, matrix, transfer);
        m_legacyCs = {};
        m_legacyCs.YCbCr_Matrix = (matrix == 5 || matrix == 6) ? 0 : 1;   // BT.601 : BT.709 (BT.2020 has only the ColorSpace1 path)
        m_legacyCs.Nominal_Range = range == 2 ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255 : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        m_dev = dev; m_imm = imm;
        if (FAILED(m_dev.As(&m_vdev)) || FAILED(m_imm.As(&m_vctx))) return fail("the D3D11 device has no video processor");
        m_vctx.As(&m_vctx1);
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputWidth = srcW; cd.InputHeight = srcH; cd.OutputWidth = outW; cd.OutputHeight = outH;
        cd.InputFrameRate = { 1, 1 }; cd.OutputFrameRate = { 1, 1 };
        cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        HRESULT hr = m_vdev->CreateVideoProcessorEnumerator(&cd, &m_en);
        if (FAILED(hr)) return fail(std::format("CreateVideoProcessorEnumerator 0x{:08X}", uint32_t(hr)));
        UINT fs = 0;
        if (FAILED(m_en->CheckVideoProcessorFormat(m_fmt, &fs)) || !(fs & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) || !(fs & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))
            return fail(std::format("the video processor cannot scale {} to {}", p010 ? "P010" : "NV12", p010 ? "P010" : "NV12"));
        if (!makeProcessor(m_vp, true)) return false;
        const bool force = noProbe();
        const auto key = std::make_tuple(luidOf(dev), srcW, srcH, outW, outH, uint32_t(m_fmt));
        int verdict = -1;
        {
            std::lock_guard<std::mutex> lk(probeLock());
            auto it = probeCache().find(key);
            if (it != probeCache().end()) verdict = it->second;
        }
        if (force) verdict = 1;
        if (verdict < 0) {
            std::string note;
            verdict = probe(note) ? 1 : 0;
            m_probeNote = note;
            std::lock_guard<std::mutex> lk(probeLock());
            probeCache()[key] = verdict;
        } else {
            m_probeNote = force ? "check skipped (PUSHMPC_QUICK_VSR_NOPROBE)" : "checked earlier in this process";
        }
        if (verdict != 1) return fail("the driver did not apply RTX VSR to the test picture (" + m_probeNote + "; NVIDIA Control Panel > RTX video enhancement > Super resolution off?)");
        m_active = true;
        m_why = std::format("driver RTX VSR through the D3D11 video processor, {}x{} -> {}x{} ({})", srcW, srcH, outW, outH, m_probeNote);
        return true;
    }

    // src[sub] (the visible source in its top-left corner) -> dst (an ingress slot, outW x outH, BIND_RENDER_TARGET).
    // Immediate context only: the caller serialises it with its other ingress work.
    bool blit(ID3D11Texture2D* src, UINT sub, ID3D11Texture2D* dst, std::string& err)
    {
        if (!m_active) { err = "quick VSR not active"; return false; }
        D3D11_TEXTURE2D_DESC sd = {};
        src->GetDesc(&sd);
        if (!m_in || m_inW != sd.Width || m_inH != sd.Height) {
            m_in.Reset(); m_inView.Reset();
            D3D11_TEXTURE2D_DESC d = {};
            d.Width = sd.Width; d.Height = sd.Height; d.MipLevels = 1; d.ArraySize = 1; d.Format = m_fmt;
            d.SampleDesc = { 1, 0 }; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_DECODER;
            HRESULT hr = m_dev->CreateTexture2D(&d, nullptr, &m_in);
            if (FAILED(hr)) { d.BindFlags = D3D11_BIND_SHADER_RESOURCE; hr = m_dev->CreateTexture2D(&d, nullptr, &m_in); }
            if (SUCCEEDED(hr)) {
                D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv = {};
                iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
                hr = m_vdev->CreateVideoProcessorInputView(m_in.Get(), m_en.Get(), &iv, &m_inView);
            }
            if (FAILED(hr)) { m_in.Reset(); err = std::format("video processor input {}x{} (0x{:08X})", sd.Width, sd.Height, uint32_t(hr)); return false; }
            m_inW = sd.Width; m_inH = sd.Height;
        }
        auto& ov = m_outViews[dst];
        if (!ov) {
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC od = {};
            od.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            const HRESULT hr = m_vdev->CreateVideoProcessorOutputView(dst, m_en.Get(), &od, &ov);
            if (FAILED(hr)) { m_outViews.erase(dst); err = std::format("video processor output view (0x{:08X})", uint32_t(hr)); return false; }
        }
        // P016 sources go into the P010 input the way the ingress ring takes them (same layout)
        m_imm->CopySubresourceRegion(m_in.Get(), 0, 0, 0, 0, src, sub, nullptr);
        return run(m_vp.Get(), m_inView.Get(), ov.Get(), err);
    }

    void forget(ID3D11Texture2D* dst) { m_outViews.erase(dst); }   // an ingress slot is about to be released
    void reset()
    {
        m_active = false; m_why.clear(); m_probeNote.clear();
        m_outViews.clear(); m_inView.Reset(); m_in.Reset(); m_inW = m_inH = 0;
        m_vp.Reset(); m_en.Reset(); m_vctx1.Reset(); m_vctx.Reset(); m_vdev.Reset(); m_imm.Reset(); m_dev.Reset();
    }

private:
    bool fail(const std::string& why) { m_why = why; m_active = false; return false; }

    static bool noProbe()
    {
        wchar_t v[8] = {};
        return GetEnvironmentVariableW(L"PUSHMPC_QUICK_VSR_NOPROBE", v, 8) && v[0] == L'1';
    }
    static uint64_t luidOf(ID3D11Device* dev)
    {
        Microsoft::WRL::ComPtr<IDXGIDevice> dd; Microsoft::WRL::ComPtr<IDXGIAdapter> ad; DXGI_ADAPTER_DESC d = {};
        if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dd))) || FAILED(dd->GetAdapter(&ad)) || FAILED(ad->GetDesc(&d))) return 0;
        return (uint64_t(uint32_t(d.AdapterLuid.HighPart)) << 32) | d.AdapterLuid.LowPart;
    }
    using ProbeKey = std::tuple<uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;
    static std::mutex& probeLock() { static std::mutex m; return m; }
    static std::map<ProbeKey, int>& probeCache() { static std::map<ProbeKey, int> c; return c; }

    // YCbCr in and out alike (the processor only scales). FFmpeg values: range 2 = full; matrix 5/6 = BT.601,
    // 9/10 = BT.2020; transfer 16 = PQ, 18 = HLG.
    static DXGI_COLOR_SPACE_TYPE colorSpace(int range, int matrix, int transfer)
    {
        const bool full = range == 2;
        if (transfer == 16) return DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020;
        if (transfer == 18) return full ? DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020 : DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020;
        if (matrix == 9 || matrix == 10) return full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020;
        if (matrix == 5 || matrix == 6) return full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
        return full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
    }

    bool makeProcessor(Microsoft::WRL::ComPtr<ID3D11VideoProcessor>& vp, bool vsr)
    {
        HRESULT hr = m_vdev->CreateVideoProcessor(m_en.Get(), 0, &vp);
        if (FAILED(hr)) return fail(std::format("CreateVideoProcessor 0x{:08X}", uint32_t(hr)));
        if (m_vctx1) {
            m_vctx1->VideoProcessorSetStreamColorSpace1(vp.Get(), 0, m_cs);
            m_vctx1->VideoProcessorSetOutputColorSpace1(vp.Get(), m_cs);
        } else {
            m_vctx->VideoProcessorSetStreamColorSpace(vp.Get(), 0, &m_legacyCs);
            m_vctx->VideoProcessorSetOutputColorSpace(vp.Get(), &m_legacyCs);
        }
        m_vctx->VideoProcessorSetStreamFrameFormat(vp.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        m_vctx->VideoProcessorSetStreamAutoProcessingMode(vp.Get(), 0, FALSE);
        m_vctx->VideoProcessorSetStreamOutputRate(vp.Get(), 0, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_NORMAL, FALSE, nullptr);
        const RECT srcRect = { 0, 0, LONG(m_srcW), LONG(m_srcH) }, dstRect = { 0, 0, LONG(m_outW), LONG(m_outH) };
        m_vctx->VideoProcessorSetStreamSourceRect(vp.Get(), 0, TRUE, &srcRect);
        m_vctx->VideoProcessorSetStreamDestRect(vp.Get(), 0, TRUE, &dstRect);
        m_vctx->VideoProcessorSetOutputTargetRect(vp.Get(), TRUE, &dstRect);
        if (vsr) {
            NvidiaPpeExt ext = { 1, 2, 1 };
            hr = m_vctx->VideoProcessorSetStreamExtension(vp.Get(), 0, &kNvidiaPpeGuid, sizeof(ext), &ext);
            if (FAILED(hr)) return fail(std::format("the driver refused the RTX VSR extension (0x{:08X})", uint32_t(hr)));
        }
        return true;
    }

    bool run(ID3D11VideoProcessor* vp, ID3D11VideoProcessorInputView* in, ID3D11VideoProcessorOutputView* out, std::string& err)
    {
        D3D11_VIDEO_PROCESSOR_STREAM s = {};
        s.Enable = TRUE; s.pInputSurface = in;
        const HRESULT hr = m_vctx->VideoProcessorBlt(vp, out, 0, 1, &s);
        if (FAILED(hr)) { err = std::format("VideoProcessorBlt 0x{:08X}", uint32_t(hr)); return false; }
        return true;
    }

    // A detailed test picture (fine lines, edges, noise) through the VSR processor and through a plain one; VSR is on
    // when the luma of the two differs on average by more than a code value. ~10 ms, once per process and stream shape.
    bool probe(std::string& note)
    {
        using Microsoft::WRL::ComPtr;
        const bool ten = m_fmt == DXGI_FORMAT_P010;
        const uint32_t w = m_srcW, h = (m_srcH + 1) & ~1u, bpp = ten ? 2 : 1, pitch = w * bpp;
        std::vector<uint8_t> pic(size_t(pitch) * h * 3 / 2);
        uint32_t seed = 0x9E3779B9u;
        auto rnd = [&seed] { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
        auto put = [&](size_t at, uint32_t v8) { if (ten) { const uint16_t v = uint16_t(v8 << 8); memcpy(&pic[at], &v, 2); } else pic[at] = uint8_t(v8); };
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                uint32_t v = ((x / 3 + y / 5) & 1) ? 180 : 60;          // fine diagonal stripes
                if ((x % 97) < 2 || (y % 61) < 2) v = 235;            // thin lines
                v = std::clamp<uint32_t>(v + (rnd() % 24) - 12, 16, 235);
                put(size_t(y) * pitch + size_t(x) * bpp, v);
            }
        for (uint32_t y = 0; y < h / 2; ++y)
            for (uint32_t x = 0; x < w; ++x) put(size_t(h + y) * pitch + size_t(x) * bpp, 128 + ((x / 8 + y / 8) & 1) * 20);
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = m_fmt; d.SampleDesc = { 1, 0 };
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_DECODER;
        D3D11_SUBRESOURCE_DATA sr = { pic.data(), pitch, 0 };
        ComPtr<ID3D11Texture2D> in, outA, outB, stA, stB;
        HRESULT hr = m_dev->CreateTexture2D(&d, &sr, &in);
        if (FAILED(hr)) { d.BindFlags = D3D11_BIND_SHADER_RESOURCE; hr = m_dev->CreateTexture2D(&d, &sr, &in); }
        if (FAILED(hr)) { note = std::format("test picture 0x{:08X}", uint32_t(hr)); return false; }
        d.Width = m_outW; d.Height = m_outH; d.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(hr = m_dev->CreateTexture2D(&d, nullptr, &outA)) || FAILED(hr = m_dev->CreateTexture2D(&d, nullptr, &outB))) { note = std::format("test output 0x{:08X}", uint32_t(hr)); return false; }
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(hr = m_dev->CreateTexture2D(&d, nullptr, &stA)) || FAILED(hr = m_dev->CreateTexture2D(&d, nullptr, &stB))) { note = std::format("test readback 0x{:08X}", uint32_t(hr)); return false; }
        ComPtr<ID3D11VideoProcessor> plain;
        const std::string keepWhy = m_why;
        if (!makeProcessor(plain, false)) { note = m_why; m_why = keepWhy; return false; }
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv = {}; iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {}; ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11VideoProcessorInputView> inV; ComPtr<ID3D11VideoProcessorOutputView> oA, oB;
        if (FAILED(hr = m_vdev->CreateVideoProcessorInputView(in.Get(), m_en.Get(), &iv, &inV)) ||
            FAILED(hr = m_vdev->CreateVideoProcessorOutputView(outA.Get(), m_en.Get(), &ovd, &oA)) ||
            FAILED(hr = m_vdev->CreateVideoProcessorOutputView(outB.Get(), m_en.Get(), &ovd, &oB))) { note = std::format("test views 0x{:08X}", uint32_t(hr)); return false; }
        std::string e;
        // twice through the VSR processor: a driver that engages VSR from the second frame on is still seen
        if (!run(m_vp.Get(), inV.Get(), oA.Get(), e) || !run(m_vp.Get(), inV.Get(), oA.Get(), e) || !run(plain.Get(), inV.Get(), oB.Get(), e)) { note = e; return false; }
        m_imm->CopyResource(stA.Get(), outA.Get());
        m_imm->CopyResource(stB.Get(), outB.Get());
        D3D11_MAPPED_SUBRESOURCE ma = {}, mb = {};
        if (FAILED(hr = m_imm->Map(stA.Get(), 0, D3D11_MAP_READ, 0, &ma))) { note = std::format("Map 0x{:08X}", uint32_t(hr)); return false; }
        if (FAILED(hr = m_imm->Map(stB.Get(), 0, D3D11_MAP_READ, 0, &mb))) { m_imm->Unmap(stA.Get(), 0); note = std::format("Map 0x{:08X}", uint32_t(hr)); return false; }
        double sum = 0; uint64_t n = 0;
        for (uint32_t y = 0; y < m_outH; y += 2) {
            const uint8_t* a = static_cast<const uint8_t*>(ma.pData) + size_t(y) * ma.RowPitch;
            const uint8_t* b = static_cast<const uint8_t*>(mb.pData) + size_t(y) * mb.RowPitch;
            for (uint32_t x = 0; x < m_outW; x += 2, ++n) {
                if (ten) { uint16_t va, vb; memcpy(&va, a + 2 * x, 2); memcpy(&vb, b + 2 * x, 2); sum += std::fabs(double(va >> 8) - double(vb >> 8)); }
                else sum += std::fabs(double(a[x]) - double(b[x]));
            }
        }
        m_imm->Unmap(stA.Get(), 0); m_imm->Unmap(stB.Get(), 0);
        const double mad = n ? sum / double(n) : 0;
        note = std::format("test picture: VSR vs plain scaling differ by {:.2f} code values on average", mad);
        return mad > 1.0;
    }

    bool m_active = false;
    std::string m_why, m_probeNote;
    uint32_t m_srcW = 0, m_srcH = 0, m_outW = 0, m_outH = 0;
    DXGI_FORMAT m_fmt = DXGI_FORMAT_NV12;
    DXGI_COLOR_SPACE_TYPE m_cs = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE m_legacyCs = {};
    Microsoft::WRL::ComPtr<ID3D11Device> m_dev;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_imm;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> m_vdev;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> m_vctx;
    Microsoft::WRL::ComPtr<ID3D11VideoContext1> m_vctx1;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> m_en;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> m_vp;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_in;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> m_inView;
    uint32_t m_inW = 0, m_inH = 0;
    std::map<ID3D11Texture2D*, Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView>> m_outViews;
};

}  // namespace pushmpc
