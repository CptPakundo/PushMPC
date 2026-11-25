// SPDX-License-Identifier: GPL-3.0-or-later
// PushMPC portable GPU engine: helpers shared by the compute shaders (GLSL 450, Vulkan 1.2).
// The arithmetic follows the CPU reference (vsplugin/engines/portable/portable_engine.cpp, itself a planar port of
// PushFPS's portable/core Interpolate.cpp) operation for operation, so the GPU and CPU results agree within one code
// value; gpu_test checks that.

// smoothstep as Interpolate.cpp has it (GLSL's is undefined for e1 <= e0)
float pmSmoothstep(float e0, float e1, float x)
{
    if (e1 <= e0) return x < e0 ? 0.0 : 1.0;
    float t = clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// MotionField::sample on a field held as vec2 per block (row-major, bx x by): bilinear between block centres, clamped.
// FIELD_BUF names the buffer's vec2 array; written as a macro so one shader can sample two fields.
#define PM_FIELD_SAMPLE(FIELD_BUF, bx, by, bs, x, y, outV)                                              \
    {                                                                                                  \
        float gx_ = clamp((x) / float(bs) - 0.5, 0.0, float((bx) - 1));                               \
        float gy_ = clamp((y) / float(bs) - 0.5, 0.0, float((by) - 1));                               \
        int i0_ = int(gx_), j0_ = int(gy_);                                                            \
        int i1_ = min(i0_ + 1, (bx) - 1), j1_ = min(j0_ + 1, (by) - 1);                                \
        float fx_ = gx_ - float(i0_), fy_ = gy_ - float(j0_);                                          \
        vec2 a_ = FIELD_BUF[j0_ * (bx) + i0_], b_ = FIELD_BUF[j0_ * (bx) + i1_];                       \
        vec2 c_ = FIELD_BUF[j1_ * (bx) + i0_], d_ = FIELD_BUF[j1_ * (bx) + i1_];                       \
        vec2 top_ = a_ + (b_ - a_) * fx_;                                                              \
        vec2 bot_ = c_ + (d_ - c_) * fx_;                                                              \
        outV = top_ + (bot_ - top_) * fy_;                                                             \
    }

// Interpolate.cpp's sampleBilinear on one plane in raw code values (0..maxValue): pixel centres at +0.5, clamped to the
// edge. One textureGather fetches the four texels: gathered at the corner shared by (x0, y0) and (x0 + 1, y0 + 1), with
// clamp-to-edge doing what the reference's min(x0 + 1, w - 1) does. kRaw: 255 for 8-bit planes, 65535 for 16-bit ones
// (the image is UNORM, the samples are the integers it holds).
float pmSample(sampler2D tex, vec2 size, float kRaw, float x, float y)
{
    float fxc = min(max(x == x ? x - 0.5 : 0.0, 0.0), size.x - 1.0);
    float fyc = min(max(y == y ? y - 0.5 : 0.0, 0.0), size.y - 1.0);
    float x0 = floor(fxc), y0 = floor(fyc);
    float fx = fxc - x0, fy = fyc - y0;
    // gather order: x = (i0, j1), y = (i1, j1), z = (i1, j0), w = (i0, j0)
    vec4 g = floor(textureGather(tex, vec2(x0 + 1.0, y0 + 1.0) / size, 0) * kRaw + 0.5);
    float top = g.w + (g.z - g.w) * fx;
    float bottom = g.x + (g.y - g.x) * fx;
    return top + (bottom - top) * fy;
}
