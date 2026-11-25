#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
//
// PushFPS Crosshair Hold: a crosshair that frame generation never sees.
//
// A crosshair stands still while the world behind it moves. Frame generation moves it with the world, so in
// every generated frame it is gone from its place and stands twice, half as strong, half the motion to either
// side; some methods also warp the picture round it. Putting its pixels back afterwards (Live HUD Lock, HUD
// protection) leaves the two ghosts, and a crosshair that is partly see-through never "stays the same" at all:
// its pixels change with what is behind it.
//
// So the crosshair is taken out of what frame generation is given, and laid back over what it makes:
//   1. FIND (every real frame, in a zone round the screen centre). A crosshair is made of thin bars: pixels that
//      are brighter (or darker) than something close by on both sides, whatever is behind them. A bar is counted
//      while the zone's picture moves and the picture near the bar with it; a bar of the world moves with it and
//      loses its count, a bar of an overlay keeps it. A bar whose every pixel has stood for kStand counted frames, and whose two ends stand still, is
//      FOUND and remembered; a bar lying where one was found may then move its ends (a reticle spreads and
//      narrows) and is FOLLOWED. A ring is found as the short bars it is made of, a dot as one short bar or,
//      when it is wider, by its outline (the inside of a small closed shape goes with it).
//   2. FILL. The found bars and a few pixels round them are filled in from the pixels beside them, in a copy of
//      the real frame that only frame generation gets. The frame that is shown is never changed.
//   3. COMPOSE (every generated frame). On those pixels the generated frame becomes
//          lerp(A, B, t) + m * (G - lerp(fA, fB, t))
//      A, B the two real frames, fA, fB their filled copies, G what frame generation made of the copies, and m how
//      much of the background shows through the overlay there, worked out from the two frames themselves
//      (0 for a solid overlay: the real pixels; about a half for a half see-through reticle).
//
// Everything here is the rule itself, without graphics: the shaders (shaders/PushOverlay*.hlsl) are the same
// statements, and tests/integration/PushGpuTests.cpp compares them with this file pixel by pixel. Change both
// together. The numbers were found on real frames of a real game (five clips, 23 November 2026) and are not to
// be changed without running that study again: a rule of this kind that is a little too eager draws artifacts
// round everything that moves (PushHudStill.h says how that went).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace veyra::pipeline::overlay {

// What the rule measures in, for a picture of a given height (the numbers are for 2160 lines).
struct Rule {
    int flank = 5;       // a bar's flank is looked for this many pixels to each side
    int stand = 4;       // counted frames in a row before a pixel has stood (the same at every size)
    int longest = 110;   // a bar longer than this along its own direction is not an overlay's
    int shortest = 4;    // nor one shorter than this
    int fresh = 24;      // a followed bar may have this many pixels outside what is remembered
    int memory = 60;     // real frames a place stays remembered after a bar was found there
    int grow = 3;        // the mask is the bars and this many pixels round them
    int reach = 24;      // how far the fill looks for a pixel outside the mask
    int hole = 9;        // a pixel with an accepted bar this near on all four sides is inside a small shape, and masked
};
inline constexpr float kContrast = 0.10f;       // a bar differs this much from its flanks (the picture as encoded, 0..1)
inline constexpr float kMoveRatio = 2.0f;       // a block moves when its change is this many times its own detail
inline constexpr float kMoveFloor = 0.004f * 0.004f;   // and more than this (mean square): below it there is nothing to judge
inline constexpr int kMoveShareDivisor = 10;    // the zone moves when a tenth of its blocks do
inline constexpr int kBlock = 16;
inline constexpr int kNearBlocks = 1;           // a frame counts for a pixel when a block this near to its own moves
inline constexpr float kWeightFloor = 1e-4f;    // added to the denominator of m
inline constexpr int kKinds = 4;                // bright with flanks left/right, bright up/down, dark left/right, dark up/down
inline constexpr int kMaxFlank = 8, kMaxGrow = 4, kMaxReach = 32, kMaxLongest = 160, kMaxHole = 12;

inline int scaled(double value, uint32_t height, int low, int high) {
    return std::clamp(int(std::floor(value * double(height) / 2160.0 + 0.5)), low, high);
}
inline Rule ruleFor(uint32_t height) {
    Rule r;
    r.flank = scaled(5, height, 2, kMaxFlank);
    r.longest = scaled(110, height, 16, kMaxLongest);
    r.shortest = scaled(4, height, 2, 8);
    r.fresh = scaled(24, height, 4, 48);
    r.grow = scaled(3, height, 1, kMaxGrow);
    r.reach = scaled(24, height, 4, kMaxReach);
    r.hole = scaled(9, height, 2, kMaxHole);
    return r;
}
// The zone: a square round the screen centre, a whole number of blocks, on even pixels. Since Subtitle Hold
// (after 4.4.5) a zone may be a rectangle: w x h; for the crosshair's square, w = h = size.
struct Zone { uint32_t x = 0, y = 0, size = 0, w = 0, h = 0; };
inline Zone zoneFor(uint32_t width, uint32_t height) {
    uint32_t size = uint32_t(std::floor(400.0 * double(height) / 2160.0 + 0.5));
    size = std::min({size, width, height}) / kBlock * kBlock;
    Zone z;
    if (size < 4 * kBlock) return z;     // too small a picture: nothing is done
    z.size = z.w = z.h = size; z.x = ((width - size) / 2) & ~1u; z.y = ((height - size) / 2) & ~1u;
    return z;
}
// PushFPS Subtitle Hold: the band where a game draws its subtitles, along the bottom of the picture: the middle
// 62.5 % of the width and the bottom 400 / 2160 of the height, from 4 % above the bottom edge (GTA 6's lines sit
// at about 90 % of the height). Whole blocks, on even pixels; `size` is the width, so code that only knows
// squares sees a zone it cannot take for one (`w != h`).
inline Zone subtitleZoneFor(uint32_t width, uint32_t height) {
    Zone z;
    uint32_t h = uint32_t(std::floor(400.0 * double(height) / 2160.0 + 0.5)) / kBlock * kBlock;
    uint32_t w = uint32_t(std::floor(double(width) * 0.625 + 0.5)) / kBlock * kBlock;
    if (h < 4 * kBlock || w < 4 * kBlock || h > height || w > width) return z;
    const uint32_t gap = uint32_t(std::floor(double(height) * 0.04 + 0.5));
    if (h + gap > height) return z;
    z.w = z.size = w; z.h = h;
    z.x = ((width - w) / 2) & ~1u;
    z.y = (height - gap - h) & ~1u;
    return z;
}
// How a frame's values become the 0..1 the thresholds are in (as Live HUD Lock): sRGB and PQ are used as
// they are, scRGB is linear with 1 = 80 nits.
inline float perceptual(float v, unsigned outputMode) { return outputMode == 1 ? std::sqrt(std::max(v, 0.0f) / 2.5f) : v; }
inline float lumaOf(float r, float g, float b, unsigned outputMode) {
    return 0.2126f * perceptual(r, outputMode) + 0.7152f * perceptual(g, outputMode) + 0.0722f * perceptual(b, outputMode);
}
inline bool kindRunsAcross(int kind) { return (kind & 1) != 0; }   // flanks above and below: the bar runs across
inline float kindSign(int kind) { return kind < 2 ? 1.0f : -1.0f; }

// A picture of the zone, one value a pixel: n x n, or w x h for a rectangular zone (size = w).
struct Plane {
    int size = 0, w = 0, h = 0; std::vector<float> v;
    Plane() = default;
    explicit Plane(int n, float fill = 0) : size(n), w(n), h(n), v(size_t(n) * n, fill) {}
    Plane(int width, int height, float fill) : size(width), w(width), h(height), v(size_t(width) * height, fill) {}
    float at(int x, int y) const { return v[size_t(y) * w + x]; }
    float& at(int x, int y) { return v[size_t(y) * w + x]; }
    bool inside(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
};
struct Colour { float r = 0, g = 0, b = 0; };
struct Picture {
    int size = 0, w = 0, h = 0; std::vector<Colour> v;
    Picture() = default;
    explicit Picture(int n) : size(n), w(n), h(n), v(size_t(n) * n) {}
    Picture(int width, int height) : size(width), w(width), h(height), v(size_t(width) * height) {}
    const Colour& at(int x, int y) const { return v[size_t(y) * w + x]; }
    Colour& at(int x, int y) { return v[size_t(y) * w + x]; }
};
inline Plane lumaPlane(const Picture& p, unsigned outputMode) {
    Plane l(p.w, p.h, 0.0f);
    for (size_t i = 0; i < p.v.size(); ++i) l.v[i] = lumaOf(p.v[i].r, p.v[i].g, p.v[i].b, outputMode);
    return l;
}

// 1a. Is this pixel a bar of this kind: it differs by kContrast from the least (for a bright bar; the greatest
//     for a dark one) of the `flank` pixels on EACH side. A side with no pixel in the zone fails.
inline bool barAt(const Plane& l, int x, int y, int kind, const Rule& r) {
    const float sign = kindSign(kind);
    const int dx = kindRunsAcross(kind) ? 0 : 1, dy = kindRunsAcross(kind) ? 1 : 0;
    const float here = sign * l.at(x, y);
    float a = 1e30f, b = 1e30f;
    for (int i = 1; i <= r.flank; ++i) {
        if (l.inside(x - i * dx, y - i * dy)) a = std::min(a, sign * l.at(x - i * dx, y - i * dy));
        if (l.inside(x + i * dx, y + i * dy)) b = std::min(b, sign * l.at(x + i * dx, y + i * dy));
    }
    return here - a >= kContrast && here - b >= kContrast;
}
// 1b. Does the zone's picture move between two real frames: in a tenth of its blocks of 16 x 16 the change is
//     more than the block's own detail explains (about two pixels of motion or more).
inline bool blockMoves(const Plane& before, const Plane& now, int bx, int by) {
    float change = 0, detail = 0;
    for (int y = by * kBlock; y < (by + 1) * kBlock; ++y)
        for (int x = bx * kBlock; x < (bx + 1) * kBlock; ++x) {
            const float d = before.at(x, y) - now.at(x, y);
            change += d * d;
            float g = 0;
            if (x > 0) { const float p = before.at(x, y) - before.at(x - 1, y), q = now.at(x, y) - now.at(x - 1, y); g += p * p + q * q; }
            if (y > 0) { const float p = before.at(x, y) - before.at(x, y - 1), q = now.at(x, y) - now.at(x, y - 1); g += p * p + q * q; }
            detail += g * 0.5f;
        }
    const float n = float(kBlock * kBlock);
    change /= n; detail /= n;
    return change > kMoveFloor && change >= kMoveRatio * (detail + 1e-7f);
}
// Which blocks move (row by row), and whether a tenth of them do.
inline std::vector<unsigned char> movingBlocks(const Plane& before, const Plane& now) {
    const int bw = now.w / kBlock, bh = now.h / kBlock;
    std::vector<unsigned char> flags(size_t(bw) * bh, 0);
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) flags[size_t(by) * bw + bx] = blockMoves(before, now, bx, by) ? 1 : 0;
    return flags;
}
inline bool zoneMoves(const std::vector<unsigned char>& flags) {
    int moving = 0;
    for (const unsigned char f : flags) moving += f ? 1 : 0;
    return moving * kMoveShareDivisor >= int(flags.size());
}
inline bool zoneMoves(const Plane& before, const Plane& now) { return zoneMoves(movingBlocks(before, now)); }
// Does the picture move NEAR this pixel: in its own block or one of the blocks round it. A crosshair is a bar
// that stands while the picture round it moves; a pole that stands while a car passes elsewhere in the zone is
// scenery (on the driving clip this took the pixels marked outside the crosshair from 162 000 to 8 000, and
// what is left stands right beside what moves).
inline bool movesNear(const std::vector<unsigned char>& flags, int blocksX, int blocksY, int x, int y) {
    const int bx = std::min(x / kBlock, blocksX - 1), by = std::min(y / kBlock, blocksY - 1);
    for (int j = std::max(by - kNearBlocks, 0); j <= std::min(by + kNearBlocks, blocksY - 1); ++j)
        for (int i = std::max(bx - kNearBlocks, 0); i <= std::min(bx + kNearBlocks, blocksX - 1); ++i)
            if (flags[size_t(j) * blocksX + i]) return true;
    return false;
}
inline bool movesNear(const std::vector<unsigned char>& flags, int blocks, int x, int y) { return movesNear(flags, blocks, blocks, x, y); }
// 1c. The count of one pixel and kind. Above 0: a bar, and 1 more than the counted frames it has been one;
//     below 0: no bar, likewise; 0: never judged. Only a frame in which the zone moves, and the picture near
//     this pixel with it, counts.
inline float nextCount(float count, bool bar, bool moving, const Rule& r) {
    if (bar) {
        const float c = count >= 1.0f ? count : 1.0f;
        return moving ? std::min(c + 1.0f, 255.0f) : c;
    }
    const float c = count <= -1.0f ? count : -1.0f;
    return moving ? std::max(c - 1.0f, -float(r.stand) - 1.0f) : c;
}
inline bool isBar(float count) { return count >= 1.0f; }
inline bool hasStood(float count, const Rule& r) { return count >= float(r.stand) + 1.0f; }
inline bool longNoBar(float count, const Rule& r) { return count <= -float(r.stand) - 1.0f; }

// What the rule keeps from frame to frame, per kind.
struct State {
    int size = 0, w = 0, h = 0;
    Plane count[kKinds], memory[kKinds];
    Plane before;            // the luma of the frame before
    bool hasBefore = false, moving = false;
    explicit State(int n = 0) : State(n, n) {}
    State(int width, int height) : size(width), w(width), h(height), before(width, height, 0.0f) {
        for (auto& c : count) c = Plane(width, height, 0.0f);
        for (auto& m : memory) m = Plane(width, height, 0.0f);
    }
};
struct Decision { bool accepted = false, found = false; };
// 1d. One pixel of one kind: the unbroken run of bar pixels through it, along the bar's own direction.
inline Decision judge(const Plane& count, const Plane& memory, int x, int y, int kind, const Rule& r) {
    Decision d;
    if (!isBar(count.at(x, y))) return d;
    const int dx = kindRunsAcross(kind) ? 1 : 0, dy = kindRunsAcross(kind) ? 0 : 1;
    const int nw = count.w, nh = count.h;
    bool stood = hasStood(count.at(x, y), r);
    int length = 1, known = memory.at(x, y) > 0 ? 1 : 0;
    float beyond[2] = {0, 0};
    for (int side = 0; side < 2; ++side) {
        const int step = side ? 1 : -1;
        int px = x, py = y;
        for (;;) {
            px += step * dx; py += step * dy;
            if (px < 0 || py < 0 || px >= nw || py >= nh) return d;        // the bar reaches the zone's edge
            const float c = count.at(px, py);
            if (!isBar(c)) { beyond[side] = c; break; }
            if (++length > r.longest) return d;
            stood = stood && hasStood(c, r);
            known += memory.at(px, py) > 0 ? 1 : 0;
        }
    }
    if (length < r.shortest) return d;
    if (stood && longNoBar(beyond[0], r) && longNoBar(beyond[1], r)) { d.accepted = d.found = true; return d; }
    d.accepted = known >= r.shortest && length - known <= r.fresh;
    return d;
}
inline bool within(int dx, int dy, int grow) { return dx * dx + dy * dy <= grow * grow + 1; }
// Is this pixel inside a small closed shape: an accepted bar within `hole` pixels to its left, to its right,
// above and below. A dot with an outline is found by its outline, a thin ring, and its middle is too wide to be a
// bar: without this the outline was taken out and the middle left for frame generation to move (a dot 16 pixels
// across lost a third of itself). A ring wider than twice `hole` keeps its inside.
inline bool enclosed(const Plane& accepted, int x, int y, int hole) {
    static constexpr int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (const auto& d : dirs) {
        bool hit = false;
        for (int k = 1; k <= hole && !hit; ++k) hit = accepted.inside(x + d[0] * k, y + d[1] * k) && accepted.at(x + d[0] * k, y + d[1] * k) > 0;
        if (!hit) return false;
    }
    return true;
}
// One real frame: counts, memory, and the mask (1 where frame generation is not to see the picture).
inline Plane step(State& s, const Plane& luma, const Rule& r) {
    const int nw = s.w, nh = s.h, blocksX = nw / kBlock, blocksY = nh / kBlock;
    const std::vector<unsigned char> flags = s.hasBefore ? movingBlocks(s.before, luma) : std::vector<unsigned char>(size_t(blocksX) * blocksY, 0);
    s.moving = s.hasBefore && zoneMoves(flags);
    for (int k = 0; k < kKinds; ++k)
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x)
                s.count[k].at(x, y) = nextCount(s.count[k].at(x, y), barAt(luma, x, y, k, r), s.moving && movesNear(flags, blocksX, blocksY, x, y), r);
    Plane accepted(nw, nh, 0.0f);
    for (int k = 0; k < kKinds; ++k) {
        Plane memory(nw, nh, 0.0f);
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x) {
                const Decision d = judge(s.count[k], s.memory[k], x, y, k, r);
                if (d.accepted) accepted.at(x, y) = 1;
                memory.at(x, y) = d.found ? float(r.memory) : std::max(s.memory[k].at(x, y) - 1.0f, 0.0f);
            }
        s.memory[k] = memory;
    }
    Plane mask(nw, nh, 0.0f);
    for (int y = 0; y < nh; ++y)
        for (int x = 0; x < nw; ++x) {
            bool any = false;
            for (int j = -r.grow; j <= r.grow && !any; ++j)
                for (int i = -r.grow; i <= r.grow && !any; ++i)
                    any = within(i, j, r.grow) && accepted.inside(x + i, y + j) && accepted.at(x + i, y + j) > 0;
            mask.at(x, y) = any || enclosed(accepted, x, y, r.hole) ? 1.0f : 0.0f;
        }
    s.before = luma; s.hasBefore = true;
    return mask;
}
// 2. A masked pixel filled in from the nearest pixels outside the mask in eight directions, each weighted by
//    one over its distance. A pixel with none within reach stays as it is.
inline Colour fillAt(const Picture& frame, const Plane& mask, int x, int y, const Rule& r) {
    if (!(mask.at(x, y) > 0)) return frame.at(x, y);
    static constexpr int dirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    float weight = 0; Colour sum;
    for (const auto& d : dirs) {
        const float unit = (d[0] != 0 && d[1] != 0) ? 1.41421356f : 1.0f;
        for (int k = 1; k <= r.reach; ++k) {
            const int px = x + d[0] * k, py = y + d[1] * k;
            if (!mask.inside(px, py)) break;
            if (mask.at(px, py) > 0) continue;
            const float w = 1.0f / (float(k) * unit);
            const Colour& c = frame.at(px, py);
            sum.r += c.r * w; sum.g += c.g * w; sum.b += c.b * w; weight += w;
            break;
        }
    }
    if (!(weight > 0)) return frame.at(x, y);
    return {sum.r / weight, sum.g / weight, sum.b / weight};
}
inline Picture fill(const Picture& frame, const Plane& mask, const Rule& r) {
    Picture out(frame.w, frame.h);
    for (int y = 0; y < frame.h; ++y)
        for (int x = 0; x < frame.w; ++x) out.at(x, y) = fillAt(frame, mask, x, y, r);
    return out;
}
// 3. One pixel of a generated frame. a, b: the two real frames; fa, fb: what frame generation was given of them;
//    g: what it made; maskA, maskB: the masks the two frames were filled with; t: where between a and b.
inline float showsThrough(const Picture& a, const Picture& b, const Picture& fa, const Picture& fb,
                          const Plane& maskA, const Plane& maskB, int x, int y) {
    float num = 0, den = 0;
    for (int j = -2; j <= 2; ++j)
        for (int i = -2; i <= 2; ++i) {
            const int px = x + i, py = y + j;
            if (!maskA.inside(px, py) || !(maskA.at(px, py) > 0 || maskB.at(px, py) > 0)) continue;
            const Colour& ca = a.at(px, py); const Colour& cb = b.at(px, py); const Colour& ua = fa.at(px, py); const Colour& ub = fb.at(px, py);
            const float dr = ca.r - cb.r, dg = ca.g - cb.g, db = ca.b - cb.b;
            const float er = ua.r - ub.r, eg = ua.g - ub.g, eb = ua.b - ub.b;
            num += dr * er + dg * eg + db * eb; den += er * er + eg * eg + eb * eb;
        }
    return std::clamp(num / (den + kWeightFloor), 0.0f, 1.0f);
}
inline Colour composeAt(const Picture& a, const Picture& b, const Picture& fa, const Picture& fb, const Picture& g,
                        const Plane& maskA, const Plane& maskB, int x, int y, float t) {
    if (!(maskA.at(x, y) > 0 || maskB.at(x, y) > 0)) return g.at(x, y);
    const float m = showsThrough(a, b, fa, fb, maskA, maskB, x, y);
    const Colour& ca = a.at(x, y); const Colour& cb = b.at(x, y); const Colour& ua = fa.at(x, y); const Colour& ub = fb.at(x, y); const Colour& cg = g.at(x, y);
    const auto one = [&](float pa, float pb, float qa, float qb, float pg) { return pa + (pb - pa) * t + m * (pg - (qa + (qb - qa) * t)); };
    return {one(ca.r, cb.r, ua.r, ub.r, cg.r), one(ca.g, cb.g, ua.g, ub.g, cg.g), one(ca.b, cb.b, ua.b, ub.b, cg.b)};
}

// Whether the user wants it (process-wide, as the still rule's switch: not a field of the settings shared
// with the engine release). Off unless switched on.
inline bool& wantedStore() { static bool wanted = false; return wanted; }
inline void setWanted(bool on) { wantedStore() = on; }
inline bool wanted() { return wantedStore(); }
// PushFPS Subtitle Hold (after 4.4.5): the same rule in the subtitle band (subtitleZoneFor). Its own switch, off
// unless switched on: not every player uses subtitles.
inline bool& subtitlesWantedStore() { static bool wanted = false; return wanted; }
inline void setSubtitlesWanted(bool on) { subtitlesWantedStore() = on; }
inline bool subtitlesWanted() { return subtitlesWantedStore(); }
}
