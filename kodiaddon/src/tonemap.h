// tonemap: HDR (PQ or HLG, BT.2020) to SDR (BT.709, 8-bit, limited range) on the CPU, for Kodi 21, whose add-on
// pictures carry no colour information (CAddonVideoCodec there marks them all UNSPECIFIED, so Kodi shows them as SDR).
// Opt-in (add-on setting): on an HDR display Kodi's own decoder plays HDR files as real HDR, which beats frame
// generation in SDR. Pure C++ so cadence_test checks it.
//
// Per pixel: limited-range Y'CbCr (BT.2020 non-constant luminance) -> R'G'B' -> linear light (PQ EOTF, or HLG inverse
// OETF + OOTF for a 1000 cd/m2 display) -> a soft-knee curve on the maximum of R, G, B (hue-preserving), mapping the
// content peak to the SDR peak with reference white (203 cd/m2) at 100 % -> BT.2020 to BT.709 primaries -> BT.709
// gamma (BT.1886 inverse, 2.4) -> limited-range Y'CbCr BT.709. Chroma: the 2x2 block's four results averaged. Lookup
// tables keep it at a few ns per pixel; rows are split over threads.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

namespace pmk {

class ToneMapper {
public:
    // pq: true PQ (SMPTE 2084), false HLG; peakNits: the content's mastering peak (0: 1000)
    void Configure(bool pq, double peakNits)
    {
        if (m_ready && pq == m_pq && peakNits == m_peak) return;
        m_pq = pq; m_peak = peakNits > 0 ? peakNits : 1000.0;
        m_toLin.resize(kIn);
        for (int i = 0; i < kIn; ++i) {
            const double e = double(i) / (kIn - 1);
            m_toLin[size_t(i)] = float(pq ? PqEotf(e) / kRefWhite : HlgToDisplay(e) / kRefWhite);
        }
        m_toGamma.resize(kOut);
        for (int i = 0; i < kOut; ++i) m_toGamma[size_t(i)] = float(std::pow(double(i) / (kOut - 1), 1.0 / 2.4));
        m_white = float((pq ? m_peak : 1000.0) / kRefWhite);
        m_ready = true;
    }

    // src: planar 4:2:0, 16-bit containers holding `bits` (10/12); dst: planar 4:2:0, 8-bit
    void Run(const uint16_t* const src[3], const ptrdiff_t srcStride[3], int bits, int w, int h, uint8_t* const dst[3],
             const ptrdiff_t dstStride[3], int threads) const
    {
        const int n = std::max(1, std::min(threads > 0 ? threads : int(std::thread::hardware_concurrency()), 16));
        const int blocks = (h + 1) / 2;
        std::vector<std::thread> pool;
        for (int t = 0; t < n; ++t) {
            const int b0 = blocks * t / n, b1 = blocks * (t + 1) / n;
            pool.emplace_back([=, this] { Rows(src, srcStride, bits, w, h, dst, dstStride, b0, b1); });
        }
        for (auto& th : pool) th.join();
    }

private:
    static constexpr int kIn = 4096, kOut = 4096;
    static constexpr double kRefWhite = 203.0;

    static double PqEotf(double e)   // cd/m2
    {
        const double m1 = 2610.0 / 16384, m2 = 2523.0 / 4096 * 128, c1 = 3424.0 / 4096, c2 = 2413.0 / 4096 * 32, c3 = 2392.0 / 4096 * 32;
        const double p = std::pow(std::max(e, 0.0), 1.0 / m2);
        return 10000.0 * std::pow(std::max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
    }
    static double HlgToDisplay(double e)   // HLG inverse OETF, then the OOTF of a 1000 cd/m2 display (gamma 1.2) on each channel
    {
        const double a = 0.17883277, b = 1 - 4 * a, c = 0.5 - a * std::log(4 * a);
        const double scene = e <= 0.5 ? e * e / 3 : (std::exp((e - c) / a) + b) / 12;
        return 1000.0 * std::pow(scene, 1.2);
    }

    void Rows(const uint16_t* const src[3], const ptrdiff_t ss[3], int bits, int w, int h, uint8_t* const dst[3],
              const ptrdiff_t ds[3], int b0, int b1) const
    {
        const float scale = 1.f / float(1 << (bits - 8));   // to 8-bit code values, fractional
        // BT.2020 NCL Y'CbCr -> R'G'B' (normalised); BT.2020 -> BT.709 primaries (linear)
        const float kr = 0.2627f, kb = 0.0593f, kg = 1 - kr - kb;
        const float m[3][3] = {{1.6605f, -0.5876f, -0.0728f}, {-0.1246f, 1.1329f, -0.0083f}, {-0.0182f, -0.1006f, 1.1187f}};
        for (int by = b0; by < b1; ++by) {
            const int y0 = by * 2;
            const uint16_t* pu = reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(src[1]) + ss[1] * by);
            const uint16_t* pv = reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(src[2]) + ss[2] * by);
            uint8_t* du = dst[1] + ds[1] * by;
            uint8_t* dv = dst[2] + ds[2] * by;
            for (int bx = 0; bx < (w + 1) / 2; ++bx) {
                const float cb = (float(pu[bx]) * scale - 128.f) / 224.f, cr = (float(pv[bx]) * scale - 128.f) / 224.f;
                float sumCb = 0, sumCr = 0; int cnt = 0;
                for (int dy = 0; dy < 2 && y0 + dy < h; ++dy) {
                    const uint16_t* py = reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(src[0]) + ss[0] * (y0 + dy));
                    uint8_t* dyp = dst[0] + ds[0] * (y0 + dy);
                    for (int dx = 0; dx < 2 && bx * 2 + dx < w; ++dx) {
                        const int x = bx * 2 + dx;
                        const float yy = (float(py[x]) * scale - 16.f) / 219.f;
                        float rgb[3] = {yy + 2 * (1 - kr) * cr, yy - (2 * kb * (1 - kb) * cb + 2 * kr * (1 - kr) * cr) / kg, yy + 2 * (1 - kb) * cb};
                        float lin[3];
                        for (int i = 0; i < 3; ++i) lin[i] = m_toLin[size_t(std::clamp(int(rgb[i] * (kIn - 1) + 0.5f), 0, kIn - 1))];
                        // hue-preserving soft knee on the brightest channel: x (1 + x / W^2) / (1 + x), W = peak / white
                        const float mx = std::max({lin[0], lin[1], lin[2], 1e-6f});
                        const float tm = mx * (1 + mx / (m_white * m_white)) / (1 + mx);
                        const float k = tm / mx;
                        float o[3];
                        for (int i = 0; i < 3; ++i) {
                            float v = (m[i][0] * lin[0] + m[i][1] * lin[1] + m[i][2] * lin[2]) * k;
                            v = std::clamp(v, 0.f, 1.f);
                            o[i] = m_toGamma[size_t(int(v * (kOut - 1) + 0.5f))];
                        }
                        // BT.709 Y'CbCr
                        const float y7 = 0.2126f * o[0] + 0.7152f * o[1] + 0.0722f * o[2];
                        dyp[x] = uint8_t(std::clamp(int(16.f + 219.f * y7 + 0.5f), 0, 255));
                        sumCb += (o[2] - y7) / 1.8556f; sumCr += (o[0] - y7) / 1.5748f; ++cnt;
                    }
                }
                du[bx] = uint8_t(std::clamp(int(128.f + 224.f * sumCb / float(cnt) + 0.5f), 0, 255));
                dv[bx] = uint8_t(std::clamp(int(128.f + 224.f * sumCr / float(cnt) + 0.5f), 0, 255));
            }
        }
    }

    bool m_ready = false, m_pq = true;
    double m_peak = 1000;
    float m_white = 1000.f / 203.f;
    std::vector<float> m_toLin, m_toGamma;
};

} // namespace pmk
