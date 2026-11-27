// PushMpcPassConvert.h: the pass-through's CPU conversions for renderers that refuse the decoder's NV12 / P010 (EVR on
// an adapter without a hardware video processor took only YUY2 in the integrations run: KMPlayer / GOM / Zoom Player
// with EVR would show no picture). Offered after the decoder's own type: YUY2 (4:2:2, chroma rows repeated), then RGB32
// (BT.601 or BT.709, limited range, top-down). P010 is taken at 8 bits (its top byte). Header-only, CPU: tested by
// filter/tools/pass_convert_test.cpp.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace pm::pass {

enum class Conv { None, Yuy2, Rgb32 };
inline int BytesPerPixel(Conv c) { return c == Conv::Yuy2 ? 2 : c == Conv::Rgb32 ? 4 : 0; }

// one 8-bit sample of the source plane: NV12 a byte, P010 the top byte of a 16-bit little-endian sample
inline uint8_t Sample(const uint8_t* row, int x, bool p010) { return p010 ? row[2 * x + 1] : row[x]; }

// src: the luma plane (srcStride bytes per row, h rows) followed by the interleaved chroma plane (h / 2 rows, U then V)
inline void ToYuy2(const uint8_t* src, size_t srcStride, int w, int h, bool p010, uint8_t* dst, size_t dstStride)
{
    const uint8_t* uv = src + srcStride * size_t(h);
    for (int y = 0; y < h; ++y) {
        const uint8_t* ly = src + srcStride * size_t(y);
        const uint8_t* lc = uv + srcStride * size_t(y / 2);
        uint8_t* d = dst + dstStride * size_t(y);
        for (int x = 0; x + 1 < w; x += 2) {
            d[2 * x + 0] = Sample(ly, x, p010);
            d[2 * x + 1] = Sample(lc, x, p010);       // U of the pair
            d[2 * x + 2] = Sample(ly, x + 1, p010);
            d[2 * x + 3] = Sample(lc, x + 1, p010);   // V of the pair
        }
    }
}

inline uint8_t Clamp8(int v) { return uint8_t(std::clamp(v, 0, 255)); }
// limited range Y'CbCr to B, G, R, 255 (bt709: the HD matrix, else BT.601)
inline void ToRgb32(const uint8_t* src, size_t srcStride, int w, int h, bool p010, bool bt709, uint8_t* dst, size_t dstStride)
{
    const int rv = bt709 ? 459 : 409, gu = bt709 ? 55 : 100, gv = bt709 ? 136 : 208, bu = bt709 ? 541 : 516;
    const uint8_t* uv = src + srcStride * size_t(h);
    for (int y = 0; y < h; ++y) {
        const uint8_t* ly = src + srcStride * size_t(y);
        const uint8_t* lc = uv + srcStride * size_t(y / 2);
        uint8_t* d = dst + dstStride * size_t(y);
        for (int x = 0; x < w; ++x) {
            const int c = 298 * (int(Sample(ly, x, p010)) - 16);
            const int u = int(Sample(lc, x & ~1, p010)) - 128, v = int(Sample(lc, (x & ~1) + 1, p010)) - 128;
            d[4 * x + 0] = Clamp8((c + bu * u + 128) >> 8);
            d[4 * x + 1] = Clamp8((c - gu * u - gv * v + 128) >> 8);
            d[4 * x + 2] = Clamp8((c + rv * v + 128) >> 8);
            d[4 * x + 3] = 255;
        }
    }
}

}  // namespace pm::pass
