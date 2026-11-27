// The anime upscaler in front of a Kodi engine (EngineCfg::animeModel): every source frame goes through pmml's
// Upscaler (ArtCNN / CuNNy / AnimeSR on ONNX Runtime, pmml/include/pmml/upscale.h), then into the inner engine (GPU or
// CPU) at the upscaled size, so frame generation and any further scaling see the upscaled picture. Built with pmml only
// (CMake: PMML_ORT_INCLUDE); a failure of the upscaler mid-stream drops it for the rest of the stream (the inner engine
// cannot change size, so the source is resized plainly to its size).
#pragma once
#include "kodi_engine.h"
#include "pmml/upscale.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace pmk {

class AnimeSrEngine final : public IEngine {
public:
    AnimeSrEngine(std::unique_ptr<pmml::Upscaler> up, std::unique_ptr<IEngine> inner, int srcW, int srcH, int bits)
        : m_up(std::move(up)), m_in(std::move(inner)), m_w(srcW), m_h(srcH), m_bits(bits) {}

    std::string Describe() const override { return m_in->Describe() + "; before it " + (m_up ? m_up->describe() : std::string("anime upscaler stopped: plain resize")); }
    bool IsGpu() const override { return m_in->IsGpu(); }
    int OutWidth() const override { return m_in->OutWidth(); }
    int OutHeight() const override { return m_in->OutHeight(); }
    bool Push(const pm_yuv& src, bool contiguous, std::string& err) override
    {
        if (!contiguous && m_up) m_up->reset();   // a seek or a gap: AnimeSR's carry starts again
        std::swap(m_prev, m_cur);
        if (!Upscale(src, m_cur)) Resize(src, m_cur);
        return m_in->Push(View(m_cur), contiguous, err);
    }
    bool HavePair() const override { return m_in->HavePair(); }
    bool PairIsCut() const override { return m_in->PairIsCut(); }
    bool Generate(float t, const pm_yuv_out& out, std::string& err) override { return m_in->Generate(t, out, err); }
    bool Real(bool current, const pm_yuv_out& out, std::string& err) override
    {
        if (m_in->Real(current, out, err)) return true;
        const Planes& p = current ? m_cur : m_prev;   // the inner engine leaves the copy to the caller: ours is upscaled
        if (p.p[0].empty()) return false;
        const int bpp = m_bits > 8 ? 2 : 1, uw = m_in->OutWidth(), uh = m_in->OutHeight();
        for (int i = 0; i < 3; ++i) {
            const int w = i ? uw / 2 : uw, h = i ? uh / 2 : uh;
            for (int y = 0; y < h; ++y) std::memcpy(out.data[i] + y * out.stride[i], p.p[i].data() + size_t(y) * w * bpp, size_t(w) * bpp);
        }
        return true;
    }
    void Forget() override { if (m_up) m_up->reset(); m_in->Forget(); }
    EngineStats Stats() const override { return m_in->Stats(); }
    double AnimeMs() const override { return m_up ? m_ms : -1; }
    int DecideK(int smoother, double srcFps, double displayHz, std::string& why) override
    {
        // the upscaler's own time comes off the frame budget the inner engine measures against
        const int k = m_in->DecideK(smoother, srcFps, displayHz, why);
        why += " (anime upscaler " + std::to_string(int(m_ms + 0.5)) + " ms a source frame)";
        return k;
    }

private:
    struct Planes { std::vector<uint8_t> p[3]; };
    pm_yuv View(const Planes& s) const
    {
        pm_yuv v{};
        const int bpp = m_bits > 8 ? 2 : 1, uw = m_in->OutWidth(), uw2 = uw / 2;
        for (int i = 0; i < 3; ++i) { v.data[i] = s.p[i].data(); v.stride[i] = ptrdiff_t(i ? uw2 : uw) * bpp; }
        return v;
    }
    bool Upscale(const pm_yuv& src, Planes& dst)
    {
        if (!m_up) return false;
        pmml::Frame in;
        for (int i = 0; i < 3; ++i) {
            const int w = i ? m_w / 2 : m_w, h = i ? m_h / 2 : m_h;
            if (m_bits > 8) { in.plane[i] = reinterpret_cast<const uint16_t*>(src.data[i]); in.stride[i] = src.stride[i] / 2; continue; }
            m_in16[i].resize(size_t(w) * h);
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) m_in16[i][size_t(y) * w + x] = src.data[i][y * src.stride[i] + x];
            in.plane[i] = m_in16[i].data(); in.stride[i] = w;
        }
        const int ow = m_up->outWidth(), oh = m_up->outHeight();
        pmml::OutFrame out;
        for (int i = 0; i < 3; ++i) { const int w = i ? ow / 2 : ow, h = i ? oh / 2 : oh; m_out16[i].resize(size_t(w) * h); out.plane[i] = m_out16[i].data(); out.stride[i] = w; }
        std::string err;
        const auto t0 = std::chrono::steady_clock::now();
        if (!m_up->run(in, out, &err)) { m_up.reset(); return false; }
        m_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        for (int i = 0; i < 3; ++i) {
            if (m_bits > 8) { dst.p[i].resize(m_out16[i].size() * 2); std::memcpy(dst.p[i].data(), m_out16[i].data(), dst.p[i].size()); }
            else { dst.p[i].resize(m_out16[i].size()); for (size_t k = 0; k < m_out16[i].size(); ++k) dst.p[i][k] = uint8_t(m_out16[i][k]); }
        }
        return true;
    }
    // after a failure: nearest-neighbour to the inner engine's size (never a black frame)
    void Resize(const pm_yuv& src, Planes& dst) const
    {
        const int bpp = m_bits > 8 ? 2 : 1, uw = m_in->OutWidth(), uh = m_in->OutHeight();
        for (int i = 0; i < 3; ++i) {
            const int w = i ? uw / 2 : uw, h = i ? uh / 2 : uh, sw = i ? m_w / 2 : m_w, sh = i ? m_h / 2 : m_h;
            dst.p[i].resize(size_t(w) * h * bpp);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    std::memcpy(&dst.p[i][(size_t(y) * w + x) * bpp], src.data[i] + (y * sh / h) * src.stride[i] + (x * sw / w) * bpp, size_t(bpp));
        }
    }
    std::unique_ptr<pmml::Upscaler> m_up;
    std::unique_ptr<IEngine> m_in;
    int m_w, m_h, m_bits;
    Planes m_prev, m_cur;
    std::vector<uint16_t> m_in16[3], m_out16[3];
    double m_ms = 0;
};

}  // namespace pmk
