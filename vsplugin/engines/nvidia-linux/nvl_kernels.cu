// pushmpc.Enhance, backend "nvidia" on Linux: the CUDA kernels around the Video Effects (built by nvcc into a fatbin
// with SASS for SM75 / 86 / 89 / 120 and PTX for later GPUs, embedded in the plugin, loaded with cuModuleLoadData).
// Every formula is in nvl_colour.h, shared with the CPU unit tests.
//   nvl_yuv8_to_packed / nvl_yuv16_to_packed  planar YUV 4:2:0 (8 / 9..16 bits) -> RGBA8 or RGB10A2 (one thread per pixel)
//   nvl_packed_to_yuv420                      RGBA8 / RGB10A2 -> Y'CbCr 4:2:0 limited, 8 or 10 bits (one thread per 2x2)
#include <cstdint>
#include "nvl_colour.h"

using namespace pmvs::nvl;

struct Planes { const uint8_t* p[3]; unsigned pitch[3]; };
struct OutPlanes { uint8_t* p[3]; unsigned pitch[3]; };

template <class T> __device__ __forceinline__ void ToPacked(Planes in, int w, int h, InFormat f, Coeffs k, int tenBitOut, uint32_t* out, unsigned outPitch)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x, y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) return;
    const int cw = (w + 1) >> 1, ch = (h + 1) >> 1;
    auto read = [&](int plane, int px, int py) -> uint32_t { return reinterpret_cast<const T*>(in.p[plane] + size_t(py) * in.pitch[plane])[px]; };
    const uint32_t v = PixelToPacked(f, k, x, y, cw, ch, tenBitOut != 0, read);
    reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(out) + size_t(y) * outPitch)[x] = v;
}

extern "C" __global__ void nvl_yuv8_to_packed(Planes in, int w, int h, InFormat f, Coeffs k, int tenBitOut, uint32_t* out, unsigned outPitch)
{
    ToPacked<uint8_t>(in, w, h, f, k, tenBitOut, out, outPitch);
}
extern "C" __global__ void nvl_yuv16_to_packed(Planes in, int w, int h, InFormat f, Coeffs k, int tenBitOut, uint32_t* out, unsigned outPitch)
{
    ToPacked<uint16_t>(in, w, h, f, k, tenBitOut, out, outPitch);
}
// The decoder's own CUDA frames (NVDEC: NV12, or P010 with the samples in the top bits): luma in.p[0], interleaved
// chroma in.p[1]; wide = 16-bit words, shift = how far the code sits above bit 0 (P010: 6). One thread per pixel.
extern "C" __global__ void nvl_semi_to_packed(Planes in, int w, int h, InFormat f, Coeffs k, int tenBitOut, int wide, int shift, uint32_t* out, unsigned outPitch)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x, y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) return;
    const int cw = (w + 1) >> 1, ch = (h + 1) >> 1;
    auto read = [&](int plane, int px, int py) -> uint32_t {
        const int p = plane == 0 ? 0 : 1, i = plane == 0 ? px : 2 * px + plane - 1;
        const uint8_t* row = in.p[p] + size_t(py) * in.pitch[p];
        return wide ? uint32_t(reinterpret_cast<const uint16_t*>(row)[i]) >> shift : uint32_t(row[i]);
    };
    const uint32_t v = PixelToPacked(f, k, x, y, cw, ch, tenBitOut != 0, read);
    reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(out) + size_t(y) * outPitch)[x] = v;
}

// hold: when the GPU cut guard flags this pair (holdFlag[0] != 0), the picture comes from `alt` (the pair's first real
// frame A) instead of `in` (the generated frame): Windows' predicated copies, decided on the GPU with no host wait.
// The second flag (the soft cut after the pair's midpoint) takes `alt2` (the current real frame B) instead.
extern "C" __global__ void nvl_packed_to_yuv420(const uint32_t* in, unsigned inPitch, int tenBitIn, int w, int h, Coeffs k, int bits, OutPlanes out,
                                                const uint32_t* holdFlag, const uint32_t* alt, unsigned altPitch,
                                                const uint32_t* holdFlag2, const uint32_t* alt2, unsigned alt2Pitch)
{
    if (holdFlag && alt && holdFlag[0] != 0u) { in = alt; inPitch = altPitch; }
    else if (holdFlag2 && alt2 && holdFlag2[0] != 0u) { in = alt2; inPitch = alt2Pitch; }
    const int cx = blockIdx.x * blockDim.x + threadIdx.x, cy = blockIdx.y * blockDim.y + threadIdx.y;
    const int cw = (w + 1) >> 1, ch = (h + 1) >> 1;
    if (cx >= cw || cy >= ch) return;
    float sCb = 0.f, sCr = 0.f; int n = 0;
    for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx) {
            const int x = 2 * cx + dx, y = 2 * cy + dy;
            if (x >= w || y >= h) continue;
            const uint32_t wd = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(in) + size_t(y) * inPitch)[x];
            float r, g, b, Y, Cb, Cr;
            Unpack(wd, tenBitIn != 0, r, g, b);
            RgbToYuv(k, r, g, b, Y, Cb, Cr);
            const uint32_t cy8 = CodeY(Y, bits);
            if (bits > 8) reinterpret_cast<uint16_t*>(out.p[0] + size_t(y) * out.pitch[0])[x] = uint16_t(cy8);
            else (out.p[0] + size_t(y) * out.pitch[0])[x] = uint8_t(cy8);
            sCb += Cb; sCr += Cr; ++n;
        }
    const uint32_t u = CodeC(sCb / float(n), bits), v = CodeC(sCr / float(n), bits);
    if (bits > 8) {
        reinterpret_cast<uint16_t*>(out.p[1] + size_t(cy) * out.pitch[1])[cx] = uint16_t(u);
        reinterpret_cast<uint16_t*>(out.p[2] + size_t(cy) * out.pitch[2])[cx] = uint16_t(v);
    } else {
        (out.p[1] + size_t(cy) * out.pitch[1])[cx] = uint8_t(u);
        (out.p[2] + size_t(cy) * out.pitch[2])[cx] = uint8_t(v);
    }
}

// The planar output (nvl_packed_to_yuv420's 8-bit bytes or 10-bit LSB words) as NV12 / P010 for the decoder's CUDA
// frames (PMENG_MEM_CUDA output): luma and interleaved chroma, shifted up by `shift` (P010: 6). One thread per 2x2.
extern "C" __global__ void nvl_planar_to_semi(OutPlanes in, int w, int h, int wide, int shift, uint8_t* y, unsigned yPitch, uint8_t* uv, unsigned uvPitch)
{
    const int cx = blockIdx.x * blockDim.x + threadIdx.x, cy = blockIdx.y * blockDim.y + threadIdx.y;
    const int cw = (w + 1) >> 1, ch = (h + 1) >> 1;
    if (cx >= cw || cy >= ch) return;
    for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx) {
            const int xx = 2 * cx + dx, yy = 2 * cy + dy;
            if (xx >= w || yy >= h) continue;
            if (wide) reinterpret_cast<uint16_t*>(y + size_t(yy) * yPitch)[xx] = uint16_t(reinterpret_cast<const uint16_t*>(in.p[0] + size_t(yy) * in.pitch[0])[xx] << shift);
            else (y + size_t(yy) * yPitch)[xx] = (in.p[0] + size_t(yy) * in.pitch[0])[xx];
        }
    if (wide) {
        uint16_t* o = reinterpret_cast<uint16_t*>(uv + size_t(cy) * uvPitch);
        o[2 * cx] = uint16_t(reinterpret_cast<const uint16_t*>(in.p[1] + size_t(cy) * in.pitch[1])[cx] << shift);
        o[2 * cx + 1] = uint16_t(reinterpret_cast<const uint16_t*>(in.p[2] + size_t(cy) * in.pitch[2])[cx] << shift);
    } else {
        uint8_t* o = uv + size_t(cy) * uvPitch;
        o[2 * cx] = (in.p[1] + size_t(cy) * in.pitch[1])[cx];
        o[2 * cx + 1] = (in.p[2] + size_t(cy) * in.pitch[2])[cx];
    }
}

// ------------------------------------------------------------------------------------------------ optical flow tier
// The source luma (8 or 16-bit codes) as the 8-bit grayscale NVOF reads (one thread per pixel).
extern "C" __global__ void nvl_luma_to_gray8(const uint8_t* y, unsigned yPitch, int bits, int w, int h, uint8_t* out, unsigned outPitch)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x, yy = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || yy >= h) return;
    const uint32_t v = bits > 8 ? (uint32_t(reinterpret_cast<const uint16_t*>(y + size_t(yy) * yPitch)[x]) >> (bits - 8)) : y[size_t(yy) * yPitch + x];
    out[size_t(yy) * outPitch + x] = uint8_t(v > 255u ? 255u : v);
}

// NVOF's S10.5 vectors on its grid, bilinear, in source pixels
__device__ __forceinline__ float2 FlowAt(const int16_t* f, unsigned pitch, int gw, int gh, float gx, float gy)
{
    gx = fminf(fmaxf(gx, 0.f), float(gw - 1)); gy = fminf(fmaxf(gy, 0.f), float(gh - 1));
    const int x0 = int(gx), y0 = int(gy), x1 = min(x0 + 1, gw - 1), y1 = min(y0 + 1, gh - 1);
    const float fx = gx - float(x0), fy = gy - float(y0);
    auto v = [&](int x, int y) { const int16_t* p = reinterpret_cast<const int16_t*>(reinterpret_cast<const uint8_t*>(f) + size_t(y) * pitch) + 2 * x; return make_float2(float(p[0]), float(p[1])); };
    const float2 a = v(x0, y0), b = v(x1, y0), c = v(x0, y1), d = v(x1, y1);
    return make_float2(Bilerp(a.x, b.x, c.x, d.x, fx, fy) / 32.f, Bilerp(a.y, b.y, c.y, d.y, fx, fy) / 32.f);
}
__device__ __forceinline__ void SampleRgb(const uint32_t* img, unsigned pitch, int w, int h, bool tenBit, float x, float y, float& r, float& g, float& b)
{
    x = fminf(fmaxf(x, 0.f), float(w - 1)); y = fminf(fmaxf(y, 0.f), float(h - 1));
    const int x0 = int(x), y0 = int(y), x1 = min(x0 + 1, w - 1), y1 = min(y0 + 1, h - 1);
    const float fx = x - float(x0), fy = y - float(y0);
    auto px = [&](int xx, int yy) { return reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(img) + size_t(yy) * pitch)[xx]; };
    float r0, g0, b0, r1, g1, b1, r2, g2, b2, r3, g3, b3;
    Unpack(px(x0, y0), tenBit, r0, g0, b0); Unpack(px(x1, y0), tenBit, r1, g1, b1); Unpack(px(x0, y1), tenBit, r2, g2, b2); Unpack(px(x1, y1), tenBit, r3, g3, b3);
    r = Bilerp(r0, r1, r2, r3, fx, fy); g = Bilerp(g0, g1, g2, g3, fx, fy); b = Bilerp(b0, b1, b2, b3, fx, fy);
}
// The frame at time t between A (t = 0) and B (t = 1), at the output size, from NVOF's two flows at the source size:
// fwd = A -> B (vectors at A's pixels), bwd = B -> A (at B's). Intermediate flows as in Super SloMo (Jiang et al.):
// F_t0 = -(1 - t) t F_01 + t^2 F_10, F_t1 = (1 - t)^2 F_01 - t (1 - t) F_10; each side weighted by its time and by how
// well its vectors round-trip (an occluded or wrong vector does not come back), so a covered area takes the other frame.
extern "C" __global__ void nvl_warp(const uint32_t* A, const uint32_t* B, unsigned pitch, int tenBit, int w, int h,
                                    const int16_t* fwd, const int16_t* bwd, unsigned flowPitch, int gw, int gh, int grid, float sx, float sy,
                                    float t, uint32_t* out, unsigned outPitch)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x, y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) return;
    // output pixel -> source pixel -> flow grid
    const float srcx = (float(x) + 0.5f) / sx - 0.5f, srcy = (float(y) + 0.5f) / sy - 0.5f;
    const float ig = 1.f / float(grid);
    const float2 f01 = FlowAt(fwd, flowPitch, gw, gh, srcx * ig, srcy * ig), f10 = FlowAt(bwd, flowPitch, gw, gh, srcx * ig, srcy * ig);
    const float u = 1.f - t;
    float2 ft0, ft1;
    IntermediateFlows(t, f01.x, f01.y, f10.x, f10.y, ft0.x, ft0.y, ft1.x, ft1.y);
    // the sample positions (source pixels)
    const float ax = srcx + ft0.x, ay = srcy + ft0.y, bx = srcx + ft1.x, by = srcy + ft1.y;
    // round trips: A's vector there, then B's vector where it lands, should cancel (and the other way round)
    const float2 fa = FlowAt(fwd, flowPitch, gw, gh, ax * ig, ay * ig), fb = FlowAt(bwd, flowPitch, gw, gh, bx * ig, by * ig);
    const float2 ra = FlowAt(bwd, flowPitch, gw, gh, (ax + fa.x) * ig, (ay + fa.y) * ig), rb = FlowAt(fwd, flowPitch, gw, gh, (bx + fb.x) * ig, (by + fb.y) * ig);
    const float ea = sqrtf((fa.x + ra.x) * (fa.x + ra.x) + (fa.y + ra.y) * (fa.y + ra.y)), eb = sqrtf((fb.x + rb.x) * (fb.x + rb.x) + (fb.y + rb.y) * (fb.y + rb.y));
    const float wa = RoundTripWeight(u, ea), wb = RoundTripWeight(t, eb);
    float r0, g0, b0, r1, g1, b1;
    SampleRgb(A, pitch, w, h, tenBit != 0, (ax + 0.5f) * sx - 0.5f, (ay + 0.5f) * sy - 0.5f, r0, g0, b0);
    SampleRgb(B, pitch, w, h, tenBit != 0, (bx + 0.5f) * sx - 0.5f, (by + 0.5f) * sy - 0.5f, r1, g1, b1);
    const float s = wa + wb > 1e-6f ? 1.f / (wa + wb) : 0.f;
    const float r = s > 0.f ? (wa * r0 + wb * r1) * s : u * r0 + t * r1, g = s > 0.f ? (wa * g0 + wb * g1) * s : u * g0 + t * g1, b = s > 0.f ? (wa * b0 + wb * b1) * s : u * b0 + t * b1;
    reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(out) + size_t(y) * outPitch)[x] = tenBit ? PackRgb10a2(r, g, b) : PackRgba8(r, g, b);
}
