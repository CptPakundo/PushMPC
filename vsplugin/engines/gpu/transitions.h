// SPDX-License-Identifier: GPL-3.0-or-later
// pushmpc.Enhance, portable GPU engine: gradual transitions and flashes, from small luma thumbnails of the source frames
// (pure C++, portable_test checks it). The cut guard (gpu_backend.h) holds the nearer real frame across a cut; two
// cases need something else:
//  - dissolves and fades: the picture is a linear mix in time (A (1 - a) + B a, or A g with a gain ramp), so a plain
//    blend of the two real frames is exact for every static part. Holding there turns a smooth transition into a jump,
//    and motion compensation on a mix of two pictures warps. Test: over three consecutive frames the second temporal
//    difference |I(k+1) - 2 I(k) + I(k-1)| stays small next to the first |I(k+1) - I(k)| (a linear ramp has none), with
//    a real change going on. Then the pair's outputs are the plain linear blend.
//  - flashes (a camera flash, lightning, a muzzle flash, an inserted white frame): frame k is much brighter than both
//    k-1 and k+1, which match each other. Interpolating spreads the flash over the generated frames (a soft glow);
//    holding keeps it one crisp event. Uses the Dedup lookahead (frame k+1 is known when the pair (k-1, k) is due).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace pmvs::gpu {

// every 8th luma sample of every 8th row, as 0..255
struct Thumb {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
    double mean = 0;
    template <class View> static Thumb Of(const View& v)
    {
        Thumb t;
        t.w = std::max(1, v.width / 8); t.h = std::max(1, v.height / 8);
        t.px.resize(size_t(t.w) * size_t(t.h));
        const int shift = std::max(0, v.bits - 8);
        double sum = 0;
        for (int y = 0; y < t.h; ++y) {
            const uint8_t* row = v.data[0] + ptrdiff_t(y * 8) * v.stride[0];
            for (int x = 0; x < t.w; ++x) {
                int s;
                if (v.bits <= 8) s = row[x * 8];
                else { uint16_t q; std::memcpy(&q, row + size_t(x) * 16, 2); s = std::min(255, int(q) >> shift); }
                t.px[size_t(y) * size_t(t.w) + size_t(x)] = uint8_t(s);
                sum += s;
            }
        }
        t.mean = sum / double(t.px.size());
        return t;
    }
};

struct TransitionRules {
    double minChange = 2.0;      // mean |I(k+1) - I(k)| (0..255) for a transition to be going on at all
    double linearRatio = 0.35;   // second difference below this share of the first: a linear (dissolve / fade) ramp
    double flashRise = 0.20;     // frame k brighter than both neighbours by this share of their mean luma ...
    double flashFloor = 12.0;    // ... and by at least this (0..255)
    double flashMatch = 0.05;    // ... while the neighbours' means differ by less than this share
};

// the three frames k-1, k, k+1 form a linear ramp (a dissolve or a fade)
inline bool LinearTransition(const Thumb& a, const Thumb& b, const Thumb& c, const TransitionRules& r = {})
{
    if (a.px.size() != b.px.size() || b.px.size() != c.px.size() || a.px.empty()) return false;
    double d1 = 0, d2 = 0;
    for (size_t i = 0; i < a.px.size(); ++i) {
        const int x = a.px[i], y = b.px[i], z = c.px[i];
        d1 += std::abs(z - y) + std::abs(y - x);
        d2 += std::abs(z - 2 * y + x);
    }
    d1 /= 2.0 * double(a.px.size());
    d2 /= double(a.px.size());
    return d1 >= r.minChange && d2 < r.linearRatio * d1;
}

// frame k (b) is a flash between a and c
inline bool Flash(const Thumb& a, const Thumb& b, const Thumb& c, const TransitionRules& r = {})
{
    const double base = std::max(a.mean, c.mean);
    const double rise = b.mean - base;
    return rise > std::max(r.flashFloor, r.flashRise * std::max(base, 1.0)) && std::abs(a.mean - c.mean) < r.flashMatch * std::max(base, 16.0);
}

} // namespace pmvs::gpu
