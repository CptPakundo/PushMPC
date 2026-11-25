// SPDX-License-Identifier: GPL-3.0-or-later
// pushmpc.Enhance, portable GPU engine: Subtitle Hold on planar YUV (see subtitle_hold.h).
#include "subtitle_hold.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "veyra/portable/CrosshairHold.h"

namespace pmvs::gpu {
using namespace veyra::pipeline::overlay;

namespace {
inline int SampleAt(const pmvs::portable::YuvView& v, int plane, int x, int y)
{
    const uint8_t* row = v.data[plane] + ptrdiff_t(y) * v.stride[plane];
    if (v.bits <= 8) return row[x];
    uint16_t s;
    std::memcpy(&s, row + size_t(x) * 2, 2);
    return s;
}
struct Norm {
    float yLo, yScale, cScale;
    explicit Norm(int bits) { const float s = float(1 << (bits - 8)); yLo = 16.f * s; yScale = 219.f * s; cScale = float((1 << bits) - 1); }
    float Y(int v) const { return (float(v) - yLo) / yScale; }
    float C(int v) const { return float(v) / cScale; }
    float YBack(float n) const { return n * yScale + yLo; }
    float CBack(float n) const { return n * cScale; }
};
} // namespace

void SubtitleHoldYuv::Reset() { m_state = State(0); m_width = m_height = 0; }

std::shared_ptr<const HoldFrameYuv> SubtitleHoldYuv::Frame(const pmvs::portable::YuvView& v)
{
    if (v.width <= 0 || v.height <= 0) return nullptr;
    const Zone zone = subtitleZoneFor(uint32_t(v.width), uint32_t(v.height));
    if (zone.size == 0) { Reset(); return nullptr; }
    const int w = int(zone.w), h = int(zone.h);
    if (v.width != m_width || v.height != m_height || m_state.w != w || m_state.h != h) {
        m_state = State(w, h);
        m_width = v.width; m_height = v.height;
    }
    const Rule rule = ruleFor(uint32_t(v.height));
    const Norm n(v.bits);
    auto out = std::make_shared<HoldFrameYuv>();
    out->x = int(zone.x); out->y = int(zone.y); out->w = w; out->h = h;
    out->zone = Picture(w, h);
    Plane luma(w, h, 0.0f);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int px = out->x + x, py = out->y + y;
            Colour c{ n.Y(SampleAt(v, 0, px, py)), n.C(SampleAt(v, 1, px / 2, py / 2)), n.C(SampleAt(v, 2, px / 2, py / 2)) };
            out->zone.at(x, y) = c;
            luma.at(x, y) = c.r;
        }
    out->mask = veyra::portable::stepFast(m_state, luma, rule);
    for (const float m : out->mask.v) if (m > 0) ++out->masked;
    if (out->masked > 0) out->filled = fill(out->zone, out->mask, rule);
    return out;
}

std::shared_ptr<const HoldPairYuv> SubtitleHoldYuv::Pair(const HoldFrameYuv& a, const HoldFrameYuv& b, int bits)
{
    if (a.w <= 0 || a.h <= 0 || a.w != b.w || a.h != b.h || a.x != b.x || a.y != b.y) return nullptr;
    if (a.masked == 0 && b.masked == 0) return nullptr;
    const Norm n(bits);
    const int w = a.w, h = a.h, cw = w / 2, ch = h / 2;
    const Picture& fa = a.masked > 0 ? a.filled : a.zone;
    const Picture& fb = b.masked > 0 ? b.filled : b.zone;
    auto out = std::make_shared<HoldPairYuv>();
    out->x = a.x; out->y = a.y; out->w = w; out->h = h;
    const size_t L = size_t(w) * size_t(h), C = size_t(cw) * size_t(ch);
    out->data.assign(2 * L + 4 * C + L, 1.0f);
    float* ya = out->data.data(); float* yb = ya + L;
    float* ua = yb + L; float* ub = ua + C; float* va = ub + C; float* vb = va + C;
    float* shows = vb + C;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = size_t(y) * size_t(w) + size_t(x);
            ya[i] = n.YBack(fa.at(x, y).r);
            yb[i] = n.YBack(fb.at(x, y).r);
            if (a.mask.at(x, y) > 0 || b.mask.at(x, y) > 0) {
                shows[i] = showsThrough(a.zone, b.zone, fa, fb, a.mask, b.mask, x, y);
                ++out->held;
            }
        }
    // chroma: a chroma sample of a block with a masked luma pixel takes the mean of the block's filled chroma
    auto chroma = [&](const HoldFrameYuv& f, const Picture& filled, float* u, float* v) {
        for (int cy = 0; cy < ch; ++cy)
            for (int cx = 0; cx < cw; ++cx) {
                bool any = false;
                float su = 0, sv = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        const int x = 2 * cx + dx, y = 2 * cy + dy;
                        any = any || f.mask.at(x, y) > 0;
                        su += filled.at(x, y).g; sv += filled.at(x, y).b;
                    }
                const size_t i = size_t(cy) * size_t(cw) + size_t(cx);
                const Colour& own = f.zone.at(2 * cx, 2 * cy);
                u[i] = n.CBack(any ? su * 0.25f : own.g);
                v[i] = n.CBack(any ? sv * 0.25f : own.b);
            }
    };
    chroma(a, fa, ua, va);
    chroma(b, fb, ub, vb);
    return out;
}

} // namespace pmvs::gpu
