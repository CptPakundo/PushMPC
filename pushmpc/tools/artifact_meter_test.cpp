// artifact_meter_test: the artifact meter (pushmpc/artifact_meter.h) on synthetic luma, CPU only, every OS.
//  - a clean picture (gradients with grain, texture, edges) reads near zero on block, band and ring;
//  - an 8x8 block-averaged copy reads blocky (also on a crop offset that is not on the grid);
//  - a smooth ramp in plateaus 1 code apart reads banded, the same ramp dithered does not (8-bit and 10-bit);
//  - noise only beside the edges (mosquito noise) reads ringing, the same noise everywhere (grain) does not;
//  - a blurred copy of the texture reads more detail loss than the sharp one;
//  - a scene cut closes a shot, a still or a moving picture does not.
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
#include "pushmpc/artifact_meter.h"

using namespace pushmpc::artifacts;
static int g_fail = 0, g_pass = 0;
static void Check(bool ok, const std::string& what, double v)
{
    std::printf("%s  %s  -- %.4f\n", ok ? "PASS" : "FAIL", what.c_str(), v);
    (ok ? g_pass : g_fail)++;
}

static constexpr int W = 1920, H = 1080;
using Plane = std::vector<float>;
static std::vector<uint8_t> To8(const Plane& p) { std::vector<uint8_t> o(p.size()); for (size_t i = 0; i < p.size(); ++i) o[i] = uint8_t(std::clamp(std::lround(p[i]), 0L, 255L)); return o; }

// clean: a diagonal gradient, a textured band, hard-edged boxes, grain
static Plane Clean(std::mt19937& rng, float grain, int shift = 0)
{
    Plane p(size_t(W) * H);
    std::normal_distribution<float> g(0.f, grain);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const int xx = x + shift;
            float v = 40.f + 120.f * float(xx + y) / float(W + H);
            if (y > 300 && y < 600) v += 25.f * std::sin(xx * 0.21f) * std::sin(y * 0.17f) + 12.f * std::sin(xx * 0.83f + y * 0.05f);
            if (((xx / 160) + (y / 160)) % 5 == 0 && y > 650) v = (xx / 160) % 2 ? 210.f : 30.f;
            p[size_t(y) * W + x] = v + g(rng);
        }
    return p;
}
static Plane Blocky(const Plane& s)   // each 8x8 block replaced by its mean (the DC term only)
{
    Plane p(s.size());
    for (int by = 0; by < H; by += 8)
        for (int bx = 0; bx < W; bx += 8) {
            double m = 0; for (int y = by; y < by + 8; ++y) for (int x = bx; x < bx + 8; ++x) m += s[size_t(y) * W + x];
            m /= 64.0;
            for (int y = by; y < by + 8; ++y) for (int x = bx; x < bx + 8; ++x) p[size_t(y) * W + x] = float(m);
        }
    return p;
}
// a slow ramp (sky): 60..80 over the width (one code every 96 px), plus a vertical component; dither: noise before rounding
static std::vector<uint16_t> Ramp(int bits, float dither, std::mt19937& rng)
{
    std::vector<uint16_t> o(size_t(W) * H);
    std::normal_distribution<float> g(0.f, dither);
    const float sc = float(1 << (bits - 8));   // native codes per 8-bit code
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            o[size_t(y) * W + x] = uint16_t(std::lround((60.f + 20.f * float(x) / W + 8.f * float(y) / H) * sc + g(rng)));
    return o;
}
static Plane Edges(std::mt19937& rng, bool mosquito)
{
    Plane p(size_t(W) * H);
    std::normal_distribution<float> g(0.f, 3.f);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const int cx = x % 64, cy = y % 64;
            const bool in = cx >= 16 && cx < 48 && cy >= 16 && cy < 48;
            float v = in ? 200.f : 50.f;
            const int dx = std::min(std::abs(cx - 16), std::abs(cx - 48)), dy = std::min(std::abs(cy - 16), std::abs(cy - 48));
            const bool nearEdge = std::min(dx, dy) <= 6;
            if (!mosquito || nearEdge) v += g(rng);
            p[size_t(y) * W + x] = v;
        }
    return p;
}
static Plane Blur(const Plane& s)   // 5-tap binomial, both directions
{
    Plane t(s.size()), o(s.size());
    const float k[5] = { 1 / 16.f, 4 / 16.f, 6 / 16.f, 4 / 16.f, 1 / 16.f };
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) { float a = 0; for (int i = -2; i <= 2; ++i) a += k[i + 2] * s[size_t(y) * W + std::clamp(x + i, 0, W - 1)]; t[size_t(y) * W + x] = a; }
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) { float a = 0; for (int i = -2; i <= 2; ++i) a += k[i + 2] * t[size_t(std::clamp(y + i, 0, H - 1)) * W + x]; o[size_t(y) * W + x] = a; }
    return o;
}
static FrameScores M8(const std::vector<uint8_t>& p, int offX = 0)
{
    return ArtifactMeter::Measure(ArtifactMeter::Sample(p.data() + offX, ptrdiff_t(W), W - offX, H, 0, 8, 0.0));
}

int main()
{
    std::mt19937 rng(7);
    const Plane clean = Clean(rng, 2.f);
    const auto c8 = To8(clean);
    const FrameScores fc = M8(c8);
    std::printf("clean: block %.3f ring %.3f band %.4f detail %.3f\n", fc.block, fc.ring, fc.band, fc.detail);
    Check(fc.block < 0.15f, "clean picture: block near 0", fc.block);
    Check(fc.band < 0.02f, "clean picture: band near 0", fc.band);

    const auto b8 = To8(Blocky(clean));
    const FrameScores fb = M8(b8), fbo = M8(b8, 3);
    std::printf("blocky: block %.3f (offset 3: %.3f) ring %.3f band %.4f detail %.3f\n", fb.block, fbo.block, fb.ring, fb.band, fb.detail);
    Check(fb.block > 0.5f && fb.block > 5.f * fc.block, "8x8 block means: blocky", fb.block);
    Check(fbo.block > 0.5f, "8x8 block means, crop 3 px off the grid: still blocky", fbo.block);

    for (int bits : { 8, 10 }) {
        const auto flat = Ramp(bits, 0.f, rng), dith = Ramp(bits, bits > 8 ? 2.5f : 0.7f, rng);
        const int shift = 0;
        const FrameScores ff = ArtifactMeter::Measure(ArtifactMeter::Sample(flat.data(), ptrdiff_t(W), W, H, shift, bits, 0.0));
        const FrameScores fd = ArtifactMeter::Measure(ArtifactMeter::Sample(dith.data(), ptrdiff_t(W), W, H, shift, bits, 0.0));
        std::printf("ramp %d-bit: plateaus band %.4f, dithered band %.4f\n", bits, ff.band, fd.band);
        Check(ff.band > 0.5f, std::to_string(bits) + "-bit ramp in plateaus: banded", ff.band);
        Check(fd.band < 0.1f, std::to_string(bits) + "-bit dithered ramp: not banded", fd.band);
    }

    const FrameScores fg = M8(To8(Edges(rng, false))), fm = M8(To8(Edges(rng, true)));
    std::printf("edges: grain everywhere ring %.3f, noise beside edges ring %.3f\n", fg.ring, fm.ring);
    Check(fm.ring > 1.0f && fm.ring > 3.f * fg.ring + 0.3f, "noise beside the edges only: ringing; grain everywhere: not", fm.ring);

    // natural texture: noise octaves at 1-32 px with a 1/f-like falloff (amplitude ~ scale^0.5)
    Plane sharp(size_t(W) * H, 110.f);
    for (int k = 0; k <= 5; ++k) {
        const int sc = 1 << k, gw = W / sc + 2, gh = H / sc + 2;
        std::uniform_real_distribution<float> un(-1.f, 1.f);
        std::vector<float> grid(size_t(gw) * gh); for (float& v : grid) v = un(rng);
        const float amp = 6.f * std::sqrt(float(sc));
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const float fx = float(x) / sc, fy = float(y) / sc; const int ix = int(fx), iy = int(fy); const float ax = fx - ix, ay = fy - iy;
                const float* g0 = &grid[size_t(iy) * gw + ix]; const float* g1 = g0 + gw;
                sharp[size_t(y) * W + x] += amp * ((g0[0] * (1 - ax) + g0[1] * ax) * (1 - ay) + (g1[0] * (1 - ax) + g1[1] * ax) * ay);
            }
    }
    const FrameScores fs = M8(To8(sharp)), fbl = M8(To8(Blur(Blur(sharp))));
    std::printf("detail: sharp %.3f, blurred %.3f\n", fs.detail, fbl.detail);
    Check(fbl.detail > fs.detail + 0.2f, "blurred copy: more detail loss than the sharp one", fbl.detail - fs.detail);

    {   // shots: a still clip does not cut, a cut closes the first shot with its scores
        ArtifactMeter m; int closed = 0;
        for (int i = 0; i < 12; ++i) closed += m.Push(ArtifactMeter::Sample(c8.data(), ptrdiff_t(W), W, H, 0, 8, i / 6.0, uint32_t(i)));
        Check(closed == 0, "a still shot: no cut", closed);
        const auto e8 = To8(Edges(rng, false));
        for (int i = 12; i < 24; ++i) closed += m.Push(ArtifactMeter::Sample(e8.data(), ptrdiff_t(W), W, H, 0, 8, i / 6.0, uint32_t(i)));
        Check(closed == 1 && m.LastShot().valid && m.LastShot().frames == 12 && m.LastShot().endedByCut, "a scene cut closes the shot (12 frames)", closed);
        Check(m.LastShot().block < 0.15f && m.LastShot().confBlock > 0.5f, "the closed shot carries the clean scores", m.LastShot().block);
        Check(m.Push(ArtifactMeter::Sample(c8.data(), ptrdiff_t(W), W, H, 0, 8, 4.0, 0u), 1) && m.LastShot().frames == 12, "the caller's cut signal closes a shot", m.LastShot().frames);
        ArtifactMeter mv; int mvc = 0;   // a moving picture (the clean frame panning 6 px a sample) does not cut
        for (int i = 0; i < 16; ++i) mvc += mv.Push(ArtifactMeter::Sample(c8.data() + 6 * i, ptrdiff_t(W), W - 96, H, 0, 8, i / 6.0, uint32_t(i)));
        Check(mvc == 0, "a pan: no cut", mvc);
    }
    std::printf("artifact meter: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
