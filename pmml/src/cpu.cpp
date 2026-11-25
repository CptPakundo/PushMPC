// SPDX-License-Identifier: GPL-3.0-or-later
// CPU versions of pmml's graphs (make_graphs.py): the ncnn backend's pack/rebuild and the tests' reference.
// They follow the ONNX semantics exactly: Resize linear half_pixel (no antialias, edge clamp), GridSample bilinear
// align_corners = 1 border, AveragePool 2x2 for 4:2:0 chroma.
#include "internal.h"

#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

namespace pmml::detail {

template <class F>
static void parallelRows(int rows, int threads, F&& fn) {
    threads = std::max(1, std::min(threads, rows));
    if (threads == 1) { fn(0, rows); return; }
    std::vector<std::thread> pool;
    const int step = (rows + threads - 1) / threads;
    for (int t = 0; t < threads; ++t) {
        const int y0 = t * step, y1 = std::min(rows, y0 + step);
        if (y0 < y1) pool.emplace_back([&, y0, y1] { fn(y0, y1); });
    }
    for (auto& th : pool) th.join();
}

static inline float sampleClamped(const float* p, int w, int h, float x, float y) {
    x = std::min(std::max(x, 0.f), float(w - 1));
    y = std::min(std::max(y, 0.f), float(h - 1));
    const int x0 = int(x), y0 = int(y);
    const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    const float fx = x - float(x0), fy = y - float(y0);
    const float a = p[size_t(y0) * w + x0] * (1 - fx) + p[size_t(y0) * w + x1] * fx;
    const float b = p[size_t(y1) * w + x0] * (1 - fx) + p[size_t(y1) * w + x1] * fx;
    return a * (1 - fy) + b * fy;
}

void resizeLinear(const float* src, int c, int h, int w, int oh, int ow, float* dst) {
    for (int ch = 0; ch < c; ++ch) {
        const float* s = src + size_t(ch) * h * w;
        float* d = dst + size_t(ch) * oh * ow;
        for (int y = 0; y < oh; ++y) {
            const float sy = (y + 0.5f) * float(h) / float(oh) - 0.5f;
            for (int x = 0; x < ow; ++x) d[size_t(y) * ow + x] = sampleClamped(s, w, h, (x + 0.5f) * float(w) / float(ow) - 0.5f, sy);
        }
    }
}

void planesToRgb(const Frame& f, PlaneFormat fmt, const Colour& c, int w, int h, float* rgb) {
    const float inv = 1.f / c.maxValue;
    float* r = rgb; float* g = rgb + size_t(w) * h; float* b = g + size_t(w) * h;
    if (fmt == PlaneFormat::Rgb) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                r[size_t(y) * w + x] = f.plane[0][y * f.stride[0] + x] * inv;
                g[size_t(y) * w + x] = f.plane[1][y * f.stride[1] + x] * inv;
                b[size_t(y) * w + x] = f.plane[2][y * f.stride[2] + x] * inv;
            }
        return;
    }
    const int cw = w / 2, chh = h / 2;
    std::vector<float> uv(size_t(cw) * chh * 2);
    for (int y = 0; y < chh; ++y)
        for (int x = 0; x < cw; ++x) {
            uv[size_t(y) * cw + x] = f.plane[1][y * f.stride[1] + x] * inv;
            uv[size_t(cw) * chh + size_t(y) * cw + x] = f.plane[2][y * f.stride[2] + x] * inv;
        }
    std::vector<float> up(size_t(w) * h * 2);
    resizeLinear(uv.data(), 2, chh, cw, h, w, up.data());
    const float kg = 1.f - c.kr - c.kb;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = size_t(y) * w + x;
            const float yl = (f.plane[0][y * f.stride[0] + x] * inv - c.yOff) / c.yScale;
            const float cb = (up[i] - c.cOff) / c.cScale, cr = (up[size_t(w) * h + i] - c.cOff) / c.cScale;
            const float rr = yl + cr * 2.f * (1.f - c.kr), bb = yl + cb * 2.f * (1.f - c.kb);
            r[i] = rr; b[i] = bb; g[i] = (yl - c.kr * rr - c.kb * bb) / kg;
        }
}

static inline uint16_t code(float v, float mx) { return uint16_t(std::nearbyint(std::min(std::max(v, 0.f), 1.f) * mx)); }

void rgbToPlanes(const float* rgb, PlaneFormat fmt, const Colour& c, int w, int h, const OutFrame& out) {
    const float* r = rgb; const float* g = rgb + size_t(w) * h; const float* b = g + size_t(w) * h;
    if (fmt == PlaneFormat::Rgb) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(y) * w + x;
                out.plane[0][y * out.stride[0] + x] = code(r[i], c.maxValue);
                out.plane[1][y * out.stride[1] + x] = code(g[i], c.maxValue);
                out.plane[2][y * out.stride[2] + x] = code(b[i], c.maxValue);
            }
        return;
    }
    const float kg = 1.f - c.kr - c.kb;
    auto luma = [&](size_t i) { return c.kr * r[i] + kg * g[i] + c.kb * b[i]; };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) out.plane[0][y * out.stride[0] + x] = code(luma(size_t(y) * w + x) * c.yScale + c.yOff, c.maxValue);
    for (int y = 0; y < h / 2; ++y)
        for (int x = 0; x < w / 2; ++x) {
            float cb = 0, cr = 0;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const size_t i = size_t(2 * y + dy) * w + 2 * x + dx;
                    const float yl = luma(i);
                    cb += (b[i] - yl) / (2.f * (1.f - c.kb));
                    cr += (r[i] - yl) / (2.f * (1.f - c.kr));
                }
            out.plane[1][y * out.stride[1] + x] = code(cb * 0.25f * c.cScale + c.cOff, c.maxValue);
            out.plane[2][y * out.stride[2] + x] = code(cr * 0.25f * c.cScale + c.cOff, c.maxValue);
        }
}

void synthCpu(const float* fa, const float* fb, int w, int h, const float* g0, const float* g1, const float* mask,
              const NetworkSize& n, const std::array<float, 4>& aff, float s0, float s1, float* out, int threads) {
    const int pw = n.padW, ph = n.padH;
    // scaled, affine-mapped grids at the padded network size, as planes [2][ph][pw]
    std::vector<float> G0(size_t(2) * pw * ph), G1(G0.size());
    for (int y = 0; y < ph; ++y)
        for (int x = 0; x < pw; ++x) {
            const float ix = 2.f * x / float(std::max(pw - 1, 1)) - 1.f, iy = 2.f * y / float(std::max(ph - 1, 1)) - 1.f;
            const size_t i = size_t(y) * pw + x;
            const float* a = g0 + i * 2; const float* b = g1 + i * 2;
            G0[i] = (ix + (a[0] - ix) * s0) * aff[0] + aff[1];
            G0[size_t(pw) * ph + i] = (iy + (a[1] - iy) * s0) * aff[2] + aff[3];
            G1[i] = (ix + (b[0] - ix) * s1) * aff[0] + aff[1];
            G1[size_t(pw) * ph + i] = (iy + (b[1] - iy) * s1) * aff[2] + aff[3];
        }
    const size_t plane = size_t(w) * h;
    parallelRows(h, threads, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            // Resize of the cropped [n.h][n.w] fields to [h][w], half_pixel; the crop sits at the top left of the padded
            const float qy = std::min(std::max((y + 0.5f) * float(n.h) / float(h) - 0.5f, 0.f), float(n.h - 1));
            for (int x = 0; x < w; ++x) {
                const float qx = std::min(std::max((x + 0.5f) * float(n.w) / float(w) - 0.5f, 0.f), float(n.w - 1));
                auto field = [&](const float* p) { return sampleClamped(p, pw, ph, qx, qy); };
                const float m = field(mask);
                const float x0 = (field(G0.data()) + 1.f) * 0.5f * float(w - 1), y0p = (field(G0.data() + size_t(pw) * ph) + 1.f) * 0.5f * float(h - 1);
                const float x1 = (field(G1.data()) + 1.f) * 0.5f * float(w - 1), y1p = (field(G1.data() + size_t(pw) * ph) + 1.f) * 0.5f * float(h - 1);
                for (int c = 0; c < 3; ++c) {
                    const float a = sampleClamped(fa + c * plane, w, h, x0, y0p);
                    const float b = sampleClamped(fb + c * plane, w, h, x1, y1p);
                    out[c * plane + size_t(y) * w + x] = b + (a - b) * m;
                }
            }
        }
    });
}

}  // namespace pmml::detail
