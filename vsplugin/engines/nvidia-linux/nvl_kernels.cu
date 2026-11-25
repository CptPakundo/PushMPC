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

extern "C" __global__ void nvl_packed_to_yuv420(const uint32_t* in, unsigned inPitch, int tenBitIn, int w, int h, Coeffs k, int bits, OutPlanes out)
{
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
