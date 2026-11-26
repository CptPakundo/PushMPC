// The frame layouts the engines behind pmeng meet on the CPU: the filter's YUV 4:2:0 (planar 8 / 10 bit LSB-aligned,
// NV12, P010 MSB-aligned) to and from three planes of samples (8-bit bytes, or 16-bit words LSB-aligned at `bits`).
// Used by the sidecar engines (nvl_backend.cpp, ml_backend.cpp), which take planar pictures; a bit depth change on the
// way out (an engine working at 8 bits on a 10-bit stream, or the reverse) shifts the samples.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#include "pmeng.h"

namespace pme::yuv {

inline bool Ten(uint32_t f) { return f == PMENG_FMT_P010 || f == PMENG_FMT_YUV420P10; }
inline bool Semi(uint32_t f) { return f == PMENG_FMT_NV12 || f == PMENG_FMT_P010; }
inline bool Supported(uint32_t f) { return f == PMENG_FMT_YUV420P || f == PMENG_FMT_YUV420P10 || f == PMENG_FMT_NV12 || f == PMENG_FMT_P010; }

// planar scratch of one picture
struct Planes {
    int w = 0, h = 0, bits = 8;
    std::vector<uint8_t> p[3];
    size_t stride[3] = {};
    void Resize(int width, int height, int b)
    {
        w = width; h = height; bits = b;
        const size_t bps = b > 8 ? 2 : 1, cw = size_t((w + 1) / 2), ch = size_t((h + 1) / 2);
        stride[0] = size_t(w) * bps; stride[1] = stride[2] = cw * bps;
        p[0].resize(stride[0] * size_t(h)); p[1].resize(stride[1] * ch); p[2].resize(stride[2] * ch);
    }
};

// the caller's frame -> planar at the frame's own depth (P010's MSB samples moved to LSB)
inline void ToPlanar(const pmeng_image& s, uint32_t fmt, Planes& d)
{
    const int w = d.w, h = d.h, ch = (h + 1) / 2, cw = (w + 1) / 2;
    if (!Semi(fmt)) {
        for (int r = 0; r < h; ++r) std::memcpy(&d.p[0][size_t(r) * d.stride[0]], s.data[0] + r * s.linesize[0], d.stride[0]);
        for (int i = 1; i < 3; ++i) for (int r = 0; r < ch; ++r) std::memcpy(&d.p[i][size_t(r) * d.stride[i]], s.data[i] + r * s.linesize[i], d.stride[i]);
    } else if (!Ten(fmt)) {
        for (int r = 0; r < h; ++r) std::memcpy(&d.p[0][size_t(r) * d.stride[0]], s.data[0] + r * s.linesize[0], d.stride[0]);
        for (int r = 0; r < ch; ++r) {
            const uint8_t* q = s.data[1] + r * s.linesize[1];
            uint8_t* u = &d.p[1][size_t(r) * d.stride[1]]; uint8_t* v = &d.p[2][size_t(r) * d.stride[2]];
            for (int x = 0; x < cw; ++x) { u[x] = q[2 * x]; v[x] = q[2 * x + 1]; }
        }
    } else {
        for (int r = 0; r < h; ++r) {
            const uint16_t* q = reinterpret_cast<const uint16_t*>(s.data[0] + r * s.linesize[0]);
            uint16_t* y = reinterpret_cast<uint16_t*>(&d.p[0][size_t(r) * d.stride[0]]);
            for (int x = 0; x < w; ++x) y[x] = uint16_t(q[x] >> 6);
        }
        for (int r = 0; r < ch; ++r) {
            const uint16_t* q = reinterpret_cast<const uint16_t*>(s.data[1] + r * s.linesize[1]);
            uint16_t* u = reinterpret_cast<uint16_t*>(&d.p[1][size_t(r) * d.stride[1]]);
            uint16_t* v = reinterpret_cast<uint16_t*>(&d.p[2][size_t(r) * d.stride[2]]);
            for (int x = 0; x < cw; ++x) { u[x] = uint16_t(q[2 * x] >> 6); v[x] = uint16_t(q[2 * x + 1] >> 6); }
        }
    }
}

// three planes of samples at `bits` (stored as bytes, or as 16-bit words LSB-aligned when `words`; default: words above
// 8 bits) -> the caller's frame in `fmt`, w x h
inline void FromPlanes(const uint8_t* const src[3], const size_t pitch[3], int bits, int w, int h, uint32_t fmt, const pmeng_image& d, int words = -1)
{
    const bool wide = words < 0 ? bits > 8 : words != 0;
    const int ch = (h + 1) / 2, cw = (w + 1) / 2;
    const int outBits = Ten(fmt) ? 10 : 8;
    auto sample = [&](int plane, int x, int y) -> uint32_t {
        const uint8_t* row = src[plane] + pitch[plane] * size_t(y);
        uint32_t v = wide ? reinterpret_cast<const uint16_t*>(row)[x] : row[x];
        if (bits > outBits) v >>= (bits - outBits); else if (bits < outBits) v <<= (outBits - bits);
        return v;
    };
    if (bits == outBits && wide == (outBits > 8) && !Semi(fmt)) {
        const size_t bps = wide ? 2 : 1;
        for (int r = 0; r < h; ++r) std::memcpy(d.data[0] + r * d.linesize[0], src[0] + pitch[0] * size_t(r), size_t(w) * bps);
        for (int i = 1; i < 3; ++i) for (int r = 0; r < ch; ++r) std::memcpy(d.data[i] + r * d.linesize[i], src[i] + pitch[i] * size_t(r), size_t(cw) * bps);
        return;
    }
    if (!Semi(fmt)) {
        for (int i = 0; i < 3; ++i) {
            const int pw = i ? cw : w, ph = i ? ch : h;
            for (int r = 0; r < ph; ++r) {
                uint8_t* row = d.data[i] + r * d.linesize[i];
                if (outBits > 8) { uint16_t* o = reinterpret_cast<uint16_t*>(row); for (int x = 0; x < pw; ++x) o[x] = uint16_t(sample(i, x, r)); }
                else for (int x = 0; x < pw; ++x) row[x] = uint8_t(sample(i, x, r));
            }
        }
        return;
    }
    const int sh = outBits > 8 ? 6 : 0;   // P010: MSB-aligned
    for (int r = 0; r < h; ++r) {
        uint8_t* row = d.data[0] + r * d.linesize[0];
        if (outBits > 8) { uint16_t* o = reinterpret_cast<uint16_t*>(row); for (int x = 0; x < w; ++x) o[x] = uint16_t(sample(0, x, r) << sh); }
        else for (int x = 0; x < w; ++x) row[x] = uint8_t(sample(0, x, r));
    }
    for (int r = 0; r < ch; ++r) {
        uint8_t* row = d.data[1] + r * d.linesize[1];
        if (outBits > 8) { uint16_t* o = reinterpret_cast<uint16_t*>(row); for (int x = 0; x < cw; ++x) { o[2 * x] = uint16_t(sample(1, x, r) << sh); o[2 * x + 1] = uint16_t(sample(2, x, r) << sh); } }
        else for (int x = 0; x < cw; ++x) { row[2 * x] = uint8_t(sample(1, x, r)); row[2 * x + 1] = uint8_t(sample(2, x, r)); }
    }
}

inline void FromPlanar(const Planes& s, uint32_t fmt, const pmeng_image& d)
{
    const uint8_t* src[3] = { s.p[0].data(), s.p[1].data(), s.p[2].data() };
    FromPlanes(src, s.stride, s.bits, s.w, s.h, fmt, d);
}

} // namespace pme::yuv
