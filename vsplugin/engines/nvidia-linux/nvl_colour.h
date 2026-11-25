// pushmpc.Enhance, backend "nvidia" on Linux: the colour conversions between VapourSynth's planar YUV 4:2:0 and the
// packed RGB the NVIDIA Video Effects take (NvCVImage RGBA U8 chunky, RGB10A2 P32 chunky), shared by the CUDA kernels
// (nvl_kernels.cu) and the CPU unit tests (nvl_test.cpp): one definition of every formula.
//
//   in:  Y, Cb, Cr at 8..16 bits, limited or full range, matrix BT.601 / BT.709 / BT.2020 NCL (H.273 codes 5/6, 1, 9;
//        others fall back to BT.709); chroma 4:2:0 left-sited (MPEG-2, H.273 chroma location 0: horizontally co-sited
//        with the even luma column, vertically between the rows), bilinear upsampling.
//   rgb: R'G'B' in the source's own transfer (no linearisation: VSR, TrueHDR and VFG all take gamma-encoded video),
//        packed as RGBA8 (r | g << 8 | b << 16 | 0xFF << 24) or RGB10A2 (r | g << 10 | b << 20 | 3 << 30).
//   out: Y'CbCr 4:2:0 limited range at 8 or 10 bits, chroma = the 2x2 average (centre-sited, _ChromaLocation 1).
#pragma once
#include <math.h>
#include <cmath>
#include <cstdint>

#if defined(__CUDACC__)
#define NVL_HD __host__ __device__ __forceinline__
#else
#define NVL_HD inline
#endif

namespace pmvs::nvl {

struct Coeffs { float kr, kb; };
// H.273 matrix code -> luma coefficients
NVL_HD Coeffs MatrixCoeffs(int matrix)
{
    switch (matrix) {
    case 5: case 6: return { 0.299f, 0.114f };          // BT.601 (470BG / 170M)
    case 9: case 10: return { 0.2627f, 0.0593f };       // BT.2020 (NCL; CL treated as NCL)
    case 7: return { 0.212f, 0.087f };                  // SMPTE 240M
    default: return { 0.2126f, 0.0722f };               // BT.709 and unspecified
    }
}

struct InFormat {
    int bits = 8;        // 8..16
    bool full = false;   // full range (VapourSynth _Range 1)
    int matrix = 1;      // H.273
};

NVL_HD float Clamp01(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }

// normalised Y' (0..1) and Cb, Cr (-0.5..0.5) from code values
NVL_HD void Normalise(const InFormat& f, float y, float cb, float cr, float& Y, float& Cb, float& Cr)
{
    if (f.full) {
        const float mx = float((1u << f.bits) - 1u), mid = float(1u << (f.bits - 1));
        Y = y / mx; Cb = (cb - mid) / mx; Cr = (cr - mid) / mx;
    } else {
        const float s = float(1u << (f.bits - 8));
        Y = (y / s - 16.f) / 219.f; Cb = (cb / s - 128.f) / 224.f; Cr = (cr / s - 128.f) / 224.f;
    }
}

NVL_HD void YuvToRgb(const Coeffs& k, float Y, float Cb, float Cr, float& r, float& g, float& b)
{
    const float kg = 1.f - k.kr - k.kb;
    r = Y + 2.f * (1.f - k.kr) * Cr;
    b = Y + 2.f * (1.f - k.kb) * Cb;
    g = (Y - k.kr * r - k.kb * b) / kg;
    r = Clamp01(r); g = Clamp01(g); b = Clamp01(b);
}
NVL_HD void RgbToYuv(const Coeffs& k, float r, float g, float b, float& Y, float& Cb, float& Cr)
{
    const float kg = 1.f - k.kr - k.kb;
    Y = k.kr * r + kg * g + k.kb * b;
    Cb = (b - Y) / (2.f * (1.f - k.kb));
    Cr = (r - Y) / (2.f * (1.f - k.kr));
}

NVL_HD uint32_t PackRgba8(float r, float g, float b)
{
    const uint32_t R = uint32_t(Clamp01(r) * 255.f + 0.5f), G = uint32_t(Clamp01(g) * 255.f + 0.5f), B = uint32_t(Clamp01(b) * 255.f + 0.5f);
    return R | (G << 8) | (B << 16) | 0xFF000000u;
}
NVL_HD uint32_t PackRgb10a2(float r, float g, float b)
{
    const uint32_t R = uint32_t(Clamp01(r) * 1023.f + 0.5f), G = uint32_t(Clamp01(g) * 1023.f + 0.5f), B = uint32_t(Clamp01(b) * 1023.f + 0.5f);
    return R | (G << 10) | (B << 20) | 0xC0000000u;
}
NVL_HD void Unpack(uint32_t w, bool tenBit, float& r, float& g, float& b)
{
    if (tenBit) { r = float(w & 1023u) / 1023.f; g = float((w >> 10) & 1023u) / 1023.f; b = float((w >> 20) & 1023u) / 1023.f; }
    else { r = float(w & 255u) / 255.f; g = float((w >> 8) & 255u) / 255.f; b = float((w >> 16) & 255u) / 255.f; }
}

// limited-range code values at `bits` (8 or 10)
NVL_HD uint32_t CodeY(float Y, int bits) { const float s = float(1u << (bits - 8)); const float v = (16.f + 219.f * Clamp01(Y)) * s + 0.5f; return uint32_t(v); }
NVL_HD uint32_t CodeC(float C, int bits)
{
    const float s = float(1u << (bits - 8));
    const float c = C < -0.5f ? -0.5f : C > 0.5f ? 0.5f : C;
    return uint32_t((128.f + 224.f * c) * s + 0.5f);
}

// The chroma sample positions for luma pixel (x, y) of a left-sited 4:2:0 plane of cw x ch: the two columns / rows and
// the bilinear weights of the second ones.
struct ChromaTap { int x0, x1, y0, y1; float fx, fy; };
NVL_HD ChromaTap ChromaAt(int x, int y, int cw, int ch)
{
    ChromaTap t;
    // horizontally co-sited: luma x = 2 cx  ->  cx = x / 2
    const float cx = 0.5f * float(x);
    // vertically centred: luma y = 2 cy + 0.5  ->  cy = (y - 0.5) / 2
    const float cy = 0.5f * (float(y) - 0.5f);
    const float fx0 = floorf(cx), fy0 = floorf(cy);
    t.fx = cx - fx0; t.fy = cy - fy0;
    int x0 = int(fx0), y0 = int(fy0);
    int x1 = x0 + 1, y1 = y0 + 1;
    x0 = x0 < 0 ? 0 : x0 >= cw ? cw - 1 : x0; x1 = x1 < 0 ? 0 : x1 >= cw ? cw - 1 : x1;
    y0 = y0 < 0 ? 0 : y0 >= ch ? ch - 1 : y0; y1 = y1 < 0 ? 0 : y1 >= ch ? ch - 1 : y1;
    t.x0 = x0; t.x1 = x1; t.y0 = y0; t.y1 = y1;
    return t;
}
NVL_HD float Bilerp(float a, float b, float c, float d, float fx, float fy)
{
    return (a * (1.f - fx) + b * fx) * (1.f - fy) + (c * (1.f - fx) + d * fx) * fy;
}

// One full conversion of a luma pixel; read(plane, x, y) returns the code value (any integer type).
template <class Read> NVL_HD uint32_t PixelToPacked(const InFormat& f, const Coeffs& k, int x, int y, int cw, int ch, bool tenBitOut, Read read)
{
    const ChromaTap t = ChromaAt(x, y, cw, ch);
    const float cb = Bilerp(float(read(1, t.x0, t.y0)), float(read(1, t.x1, t.y0)), float(read(1, t.x0, t.y1)), float(read(1, t.x1, t.y1)), t.fx, t.fy);
    const float cr = Bilerp(float(read(2, t.x0, t.y0)), float(read(2, t.x1, t.y0)), float(read(2, t.x0, t.y1)), float(read(2, t.x1, t.y1)), t.fx, t.fy);
    float Y, Cb, Cr, r, g, b;
    Normalise(f, float(read(0, x, y)), cb, cr, Y, Cb, Cr);
    YuvToRgb(k, Y, Cb, Cr, r, g, b);
    return tenBitOut ? PackRgb10a2(r, g, b) : PackRgba8(r, g, b);
}

// The 8-bit full-range luma the cut guard samples, from a source luma code value (the same scale as the GPU pair test's
// R'G'B' luma: limited-range video is expanded).
NVL_HD uint32_t GuardLuma(uint32_t y, int bits, bool full)
{
    const float s = float(1u << (bits - 8));
    float v = float(y) / s;
    if (!full) v = (v - 16.f) * (255.f / 219.f);
    v += 0.5f;
    return v <= 0.f ? 0u : v >= 255.f ? 255u : uint32_t(v);
}

} // namespace pmvs::nvl
