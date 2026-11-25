// SPDX-License-Identifier: GPL-3.0-only
// PushMPC (local addition, not upstream): Fourier-Mellin global motion of one frame pair for the scene-cut guard
// (PushMpcCutGuard, feature bit 8). At 64 x 64 over the active picture area:
//   1. both frames, Hann-windowed (mode 0); their 2D FFTs (modes 1 / 2 = rows / columns);
//   2. log-polar resampling of the log-magnitude spectra (mode 3; translation-invariant: a zoom by s becomes a shift of
//      ln s along log-radius, a rotation a shift along the angle); their FFTs; the normalised cross-power spectrum
//      (mode 4), its inverse FFT and the peak (mode 5): scale, rotation and the peak height (0..1, how much of the
//      spectrum agrees: near 1 for a pure zoom / rotation, low for a cut);
//   3. the previous frame warped by that scale and rotation (mode 6), its FFT, the cross-power spectrum with the
//      current frame's, inverse FFT and peak: the translation and its peak height.
// The results go to the guard's result buffer at kFmOffset; VfgShotChange.hlsl then compares the frames' structure
// after this similarity transform. Every dispatch is small (64 x 64); the CPU never waits.
cbuffer Constants : register(b0)
{
    uint mode;
    uint width;
    uint height;
    uint pitch;
    uint tenBit;
    uint slabA;      // first scratch slab of the pass
    uint slabB;      // second input slab (mode 4)
    uint slabOut;    // output slab (mode 4)
    uint inverse;    // modes 1 / 2: inverse FFT (scaled by 1/64 per pass)
    uint which;      // mode 5: 0 = scale / rotation, 1 = translation
    uint areaWord0;  // fixed active area (0 = bars from the result buffer): x0 | x1 << 16
    uint areaWord1;  // y0 | y1 << 16
};
ByteAddressBuffer previousFrame : register(t0);
ByteAddressBuffer currentFrame : register(t1);
RWByteAddressBuffer scratch : register(u0);   // slabs of 64 x 64 float2
RWByteAddressBuffer results : register(u1);   // the guard's result buffer

static const uint N = 64;
static const uint kSlab = N * N * 8;
static const uint kFmOffset = 1408;           // dLogR, dAngle, peak, dx, dy, peakT (floats)
static const uint kBarsOffset = 80;
static const uint kProfX = 128, kProfY = 72;
static const float kPi = 3.14159265;
static const float kR0 = 2.5, kRMax = 31.0;   // log-polar radius range (frequency bins; the window's leakage lies below)

groupshared float2 sh[N];
groupshared float shMax[N];
groupshared uint shIdx[N];
groupshared float shSum[N];

float2 ld(uint slab, uint i) { return asfloat(scratch.Load2(slab * kSlab + i * 8)); }
void st(uint slab, uint i, float2 v) { scratch.Store2(slab * kSlab + i * 8, asuint(v)); }
uint lumaAt(ByteAddressBuffer frame, uint x, uint y)
{
    const uint word = frame.Load(y * pitch + x * 4);
    if (tenBit != 0) {
        const uint r = word & 1023u, g = (word >> 10) & 1023u, b = (word >> 20) & 1023u;
        return min(255u, (54u * r + 183u * g + 19u * b + 512u) >> 10);
    }
    const uint r = word & 255u, g = (word >> 8) & 255u, b = (word >> 16) & 255u;
    return (54u * r + 183u * g + 19u * b + 128u) >> 8;
}
float lumaBox(ByteAddressBuffer frame, float x, float y, float d)
{
    const float xm = clamp(x - d, 0.0, float(width - 1)), xp = clamp(x + d, 0.0, float(width - 1));
    const float ym = clamp(y - d, 0.0, float(height - 1)), yp = clamp(y + d, 0.0, float(height - 1));
    return 0.25 * float(lumaAt(frame, uint(xm), uint(ym)) + lumaAt(frame, uint(xp), uint(ym)) + lumaAt(frame, uint(xm), uint(yp)) + lumaAt(frame, uint(xp), uint(yp)));
}
// the active area: a fixed one, else the bar geometry in use (as VfgShotChange.hlsl), else the frame
uint4 activeRect()
{
    if ((areaWord0 >> 16) > (areaWord0 & 0xFFFFu)) return uint4(areaWord0 & 0xFFFFu, areaWord0 >> 16, areaWord1 & 0xFFFFu, areaWord1 >> 16);
    if (results.Load(kBarsOffset + 4) != 0) {
        const uint p = results.Load(kBarsOffset);
        const uint4 g = uint4(p & 255u, (p >> 8) & 255u, (p >> 16) & 255u, p >> 24);
        const uint4 r = uint4(((g.x + (g.x ? 1u : 0u)) * width) / kProfX, width - ((g.y + (g.y ? 1u : 0u)) * width) / kProfX,
                              ((g.z + (g.z ? 1u : 0u)) * height) / kProfY, height - ((g.w + (g.w ? 1u : 0u)) * height) / kProfY);
        if (r.y > r.x + width / 4 && r.w > r.z + height / 4) return r;
    }
    return uint4(0, width, 0, height);
}
float hann(uint i) { return 0.5 - 0.5 * cos(2.0 * kPi * (float(i) + 0.5) / float(N)); }
// radix-2 FFT of sh (loaded in bit-reversed order) by the group's first 32 threads
void fft64(uint t, bool inv)
{
    [unroll] for (uint len = 2; len <= N; len <<= 1) {
        GroupMemoryBarrierWithGroupSync();
        if (t < N / 2) {
            const uint hf = len >> 1, k = t % hf, i = (t / hf) * len + k, j = i + hf;
            const float ang = (inv ? 2.0 : -2.0) * kPi * float(k) / float(len);
            const float2 w = float2(cos(ang), sin(ang)), a = sh[i], b = sh[j];
            const float2 bw = float2(b.x * w.x - b.y * w.y, b.x * w.y + b.y * w.x);
            sh[i] = a + bw; sh[j] = a - bw;
        }
    }
    GroupMemoryBarrierWithGroupSync();
}
uint rev6(uint x) { return reversebits(x) >> 26; }
// magnitude spectrum of slab s at a fractional (centred) frequency, bilinear with wrap-around
float magAt(uint s, float u, float v)
{
    const float x = u < 0.0 ? u + float(N) : u, y = v < 0.0 ? v + float(N) : v;
    const uint x0 = uint(floor(x)) % N, y0 = uint(floor(y)) % N, x1 = (x0 + 1) % N, y1 = (y0 + 1) % N;
    const float fx = frac(x), fy = frac(y);
    const float a = length(ld(s, y0 * N + x0)), b = length(ld(s, y0 * N + x1)), c = length(ld(s, y1 * N + x0)), d = length(ld(s, y1 * N + x1));
    return lerp(lerp(a, b, fx), lerp(c, d, fx), fy);
}

[numthreads(64, 1, 1)]
void main(uint3 gid3 : SV_GroupID, uint t : SV_GroupIndex)
{
    const uint gid = gid3.x;
    if (mode == 0 || mode == 6) {
        // a windowed 64 x 64 image of the active area: mode 0 both frames (slabs 0 / 1), mode 6 the previous frame
        // warped by the scale and rotation found (slab 5): sample A at c + R(-phi) (x - c) / s
        const uint img = mode == 0 ? gid / N : 0, row = gid % N;
        const uint4 r = activeRect();
        const float rw = float(r.y - r.x), rh = float(r.w - r.z), d = max(1.0, rw / 256.0);
        float px = float(r.x) + (float(t) + 0.5) * rw / float(N), py = float(r.z) + (float(row) + 0.5) * rh / float(N);
        if (mode == 6) {
            const float lnS = asfloat(results.Load(kFmOffset)), phi = asfloat(results.Load(kFmOffset + 4));
            const float s = exp(lnS), cx = float(r.x) + 0.5 * rw, cy = float(r.z) + 0.5 * rh;
            const float2 p = float2(px - cx, py - cy) / s;
            px = cx + cos(phi) * p.x + sin(phi) * p.y; py = cy - sin(phi) * p.x + cos(phi) * p.y;
        }
        const float v = (img == 0 ? lumaBox(previousFrame, px, py, d) : lumaBox(currentFrame, px, py, d)) / 255.0;
        st(mode == 6 ? 5 : img, row * N + t, float2(v * hann(t) * hann(row), 0.0));
    } else if (mode == 1 || mode == 2) {
        // FFT of one row (mode 1) or column (mode 2) of slab slabA + gid / 64
        const uint s = slabA + gid / N, ln = gid % N;
        const uint idx = mode == 1 ? ln * N + t : t * N + ln;
        sh[rev6(t)] = ld(s, idx);
        fft64(t, inverse != 0);
        st(s, idx, sh[t] * (inverse != 0 ? 1.0 / float(N) : 1.0));
    } else if (mode == 3) {
        // log-polar resampling of log(1 + |F|) (rows: log radius, columns: angle 0..pi), high frequencies emphasised
        const uint img = gid / N, lr = gid % N;
        const float rad = kR0 * exp(float(lr) * log(kRMax / kR0) / float(N)), ang = kPi * float(t) / float(N);
        // log magnitude minus its mean over the angles at this radius (a 1/f falloff is the same at every scale and
        // would pull the correlation to zero shift), windowed along log radius (not periodic; the angle is)
        const float m = log(1.0 + magAt(img, rad * cos(ang), rad * sin(ang)));
        shSum[t] = m;
        GroupMemoryBarrierWithGroupSync();
        if (t == 0) { float s = 0.0; for (uint k = 0; k < N; ++k) s += shSum[k]; shMax[0] = s / float(N); }
        GroupMemoryBarrierWithGroupSync();
        st(2 + img, lr * N + t, float2((m - shMax[0]) * hann(lr), 0.0));
    } else if (mode == 4) {
        // normalised cross-power spectrum A x conj(B)
        const uint i = gid * N + t;
        const float2 a = ld(slabA, i), b = ld(slabB, i);
        const float2 c = float2(a.x * b.x + a.y * b.y, a.y * b.x - a.x * b.y);
        st(slabOut, i, c / (length(c) + 1e-9));
    } else if (mode == 5) {
        // the correlation peak of slab slabA (one group): position with a parabolic sub-bin refinement, height
        float best = -1e9; uint bi = 0; float sum = 0.0;
        for (uint k = 0; k < N; ++k) { const uint i = t * N + k; const float v = ld(slabA, i).x; sum += abs(v); if (v > best) { best = v; bi = i; } }
        shMax[t] = best; shIdx[t] = bi; shSum[t] = sum;
        GroupMemoryBarrierWithGroupSync();
        if (t == 0) {
            for (uint k = 1; k < N; ++k) if (shMax[k] > shMax[0]) { shMax[0] = shMax[k]; shIdx[0] = shIdx[k]; }
            const uint i = shIdx[0], x = i % N, y = i / N;
            const float c = shMax[0];
            const float l = ld(slabA, y * N + (x + N - 1) % N).x, r = ld(slabA, y * N + (x + 1) % N).x;
            const float u = ld(slabA, ((y + N - 1) % N) * N + x).x, dn = ld(slabA, ((y + 1) % N) * N + x).x;
            const float ddx = l - 2.0 * c + r, ddy = u - 2.0 * c + dn;
            float fx = float(x) + (ddx < -1e-9 ? clamp(0.5 * (l - r) / ddx, -0.5, 0.5) : 0.0);
            float fy = float(y) + (ddy < -1e-9 ? clamp(0.5 * (u - dn) / ddy, -0.5, 0.5) : 0.0);
            if (fx >= float(N / 2)) fx -= float(N);
            if (fy >= float(N / 2)) fy -= float(N);
            if (which == 0) {
                // log-polar shift: rows = log radius (scale), columns = angle (pi per 64)
                const float lnS = fy * log(kRMax / kR0) / float(N), phi = -fx * kPi / float(N);
                results.Store3(kFmOffset, uint3(asuint(lnS), asuint(phi), asuint(saturate(c))));
            } else {
                results.Store3(kFmOffset + 12, uint3(asuint(fx), asuint(fy), asuint(saturate(c))));
            }
        }
    }
}
