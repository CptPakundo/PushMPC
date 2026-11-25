// SPDX-License-Identifier: GPL-3.0-only
// Shot-change test for one frame-generation input pair. It replaces the VFG
// SDK's AutomaticShotChangeDetection, which synchronised the CPU with every Run
// and was evaluated again for every subframe of the same pair.
//
// Same measures and thresholds as core::SceneCadenceAnalyzer on the CPU path:
// 64x36 luma samples, 256-bin histogram distance (L1 / 2) and normalised SAD;
// a flash (large SAD, near-identical histogram) is not a cut. The samples come
// from the encoded A/B buffers the generator itself reads, so every source kind
// (capture, software and hardware decode, streaming) is covered without a CPU
// readback.
//
// PushMPC (local addition, not upstream). The same shader serves VFG
// (VfgBackend's own input buffers) and every other backend (PushMpcCutGuard:
// DLSS-G, PushFlow, FSR FG, AMD FRC), with these rules on top of the fixed ones:
//  1. Adaptive rule: the mean-removed SAD (a flash or fade moves the mean, not
//     the structure) against a running baseline of this stream's own pair
//     differences, and the 16-bin histogram must move too (a fast pan keeps
//     its histogram):  mrSad > max(adaptiveMin, adaptiveRatio * baseline)
//                      and histogram16 > adaptiveHistogram16.
//     The baseline follows non-cut pairs with a fast attack and a slow release
//     (hysteresis); cut pairs and held pairs (animation on twos) never update it.
//  2. Active picture area: letterbox and pillarbox bars are found once per
//     stream (superseded by rule 6: tracked every frame; per-column / per-row maximum luma over the first barFrames
//     real frames; a bar is a run of near-black bins at both opposite edges of
//     about the same width) and every measure is then taken inside the picture
//     only. A new stream size builds a new graph and so a new detection.
//  3. Dark cuts: when both frames are dark the histograms are compared after
//     each frame's own luma range is stretched to 16 bins (histogramN16), and
//     the mean-removed SAD is divided by the pair's contrast (mrSadN), with its
//     own baseline.
//  5. Block matching (PushMPC, bit 4): 96x54 box-filtered luma of the active area in group memory, 108 blocks of
//     8x6 texels, each searched +-6 / +-4 texels for its best match in A; the mean of the best (mean-removed) block
//     differences is the motion-compensated difference mcSad. Where it explains the pair (mcSad / the same at zero
//     shift below mcVeto) a fired rule is motion (zooms, pans, animation on twos); where it does not and mcSad is
//     far above its own baseline, the pair is a cut even inside fast motion.
//  6. Bar geometry, every frame (PushMPC, bit 5): the letterbox / pillarbox bars of each real frame from per-column
//     and per-row maxima; a symmetric change of at least barChangeBins bins between A and B, where the band is black
//     in one frame and clearly picture (>= barPicture) in the other, is a cut (bars appearing or going are never
//     interpolated: IMAX / non-IMAX switches, a windowed insert taking the frame). The area the measures use follows
//     the bars with hysteresis: a new geometry is taken after barPersist frames, at once after such a cut.
//  7. Scene score (PushMPC, bit 6): a zoom-aware fit (scale 1/1.8..1.8 about the centre and shift, coarse then fine search
//     on the 96x54 image; the residual over the frames' mean deviation), the chroma histogram (8x8 bins of B-Y / R-Y),
//     an orientation histogram of log-luma gradients per quadrant, per-block matching around the fit (the lower quartile
//     of best residual / block deviation) and the rise of these over the last 6 moving pairs, combined by fixed weights
//     (logistic fit, kScoreW); a score above scoreThreshold is a cut. Zooms, pans and on-twos drawing changes keep the
//     fit residual low; effects that cover the frame (fire, a creature crossing) are the closest non-cuts.
//  4. Global motion (bit 1, off by default): a scale + shift per axis is fitted between the two frames'
//     column and row luma profiles, and the mean-removed SAD is taken again
//     with A warped by it (mrSadW). Where the warp explains the difference
//     (mrSadW / mrSad below warpVeto) a fired adaptive or dark rule is a zoom or
//     pan, not a cut (false holds in zooms); where it does not, a large warped
//     residual against its own baseline is a cut even inside fast motion (the
//     motion raised the plain baseline).
// State lives in the result buffer, so nothing is read back. Layout (bytes):
//   0 / 8      64-bit cut flag per parity (predication)
//   16 / 32    last SAD, histogram, compare per parity (upstream diagnostics)
//   48         cut count            52  pair count
//   64         baseline, initialised, baselineN, baselineW
//   80         bars: geometry in use (l | r << 8 | t << 16 | b << 24, bins), valid, candidate, its count
//   96         baseline of mcSad; 100 / 104: the previous frame's bar geometry, valid
//   112        spike rule: the last moving pair's mrSad, histogram16, valid
//   128        the previous real frame's per-column maximum luma (128 uint)   640  per-row maximum (72 uint)
//   928        trace record of the last pair (56 x 32-bit, see the end of main)
//   1152       scene score state (64 x 32-bit); buffer 1408 bytes
//
// flags[parity] (64-bit, offset parity*8) = 1 when the pair crosses a cut; the
// subframe copies of that pair are predicated on it and show the pair's first
// real frame A held (PushMPC; upstream shows the current input B).

cbuffer Constants : register(b0)
{
    uint width;
    uint height;
    uint pitch;      // bytes per row of both buffers
    uint tenBit;     // 1: R10G10B10A2, 0: R8G8B8A8
    uint parity;
    uint compare;    // 0 when the pair has no valid A (reset): never a cut
    float cutSad;    // SceneCadenceAnalyzer::Config::sceneCutSadThreshold
    float cutHistogram;
    float replacementSad;
    float replacementHistogram;
    float flashSad;
    float flashHistogram;
    // PushMPC adaptive rule
    float adaptiveMin;        // absolute floor of the mean-removed SAD threshold (0 = rule off)
    float adaptiveRatio;      // times the baseline
    float adaptiveHistogram16;
    float baselineAttack;     // EMA weight when the pair differs more than the baseline
    float baselineRelease;    // EMA weight when it differs less
    // PushMPC dark rule
    float darkMean;           // both frames' mean luma (0..1) below this: dark pair
    float darkMin;            // floor of the contrast-normalised threshold
    float darkRatio;          // times its baseline
    float darkHistogram;      // histogramN16 above this
    // PushMPC warp check
    float warpVeto;           // mrSadW / mrSad below this: the difference is global motion
    float warpMin;            // floor of the warped-residual threshold
    float warpRatio;          // times its baseline
    float warpHistogram;      // histogram16 above this (or histogramN16 above darkHistogram)
    float holdSad;            // SAD and mrSad below this: a held frame (no baseline update)
    // PushMPC active area
    float barLuma;            // a bar bin's maximum luma (0..1) is at most this
    uint barTaps;             // a bar bin has at most this many of its 32 taps brighter than barLuma (a watermark in a bar)
    uint features;            // bit 0 bars, bit 1 warp check, bit 2 dark rule, bit 3 spike rule
    float areaX0;             // fixed active area (fractions); areaX1 <= areaX0 = detect
    float areaY0;
    float areaX1;
    float areaY1;
    // PushMPC spike rule: the pair against the last pair that moved (held pairs skipped)
    float spikeRatio;         // mrSad above this times the last moving pair's
    float spikeMin;           // and above this
    float spikeHistogram;     // histogram16 above max(this, spikeHistogramRatio x the last moving pair's)
    float spikeHistogramRatio;
    // PushMPC block matching
    float mcVeto;             // mcSad / zero-shift difference below this: the pair is motion
    float mcMin;              // floor of the motion-compensated cut threshold
    float mcRatio;            // times its baseline
    // PushMPC bar geometry
    float barChangeBins;      // a bar edge moving by at least this many bins (of 72 rows / 128 columns)
    float barPicture;         // ... where the band is at least this bright (0..1) in the frame showing picture
    uint barPersist;          // frames a new bar geometry must last before the measures follow it
    // PushMPC scene score (bit 6)
    float scoreThreshold;     // the combined score of the scene measures above this: a cut
    // PushMPC optical flow (bit 7): this pair's flow, when the graph ran it (1 forward, 2 cost, 4 backward, 8 FidelityFX)
    uint flowFlags;
    uint flowPitch;           // bytes per row of the forward / backward vectors (R16G16_SINT)
    uint costPitch;           // bytes per row of the cost (R8_UINT)
    uint flowGrid;            // grid width | height << 16
    uint flowExtent;          // flow-pixel extent width | height << 16 (the source frame)
    float scoreThresholdFlow; // the scene score with NVOF's measures (its own weights) above this: a cut
};

ByteAddressBuffer previousFrame : register(t0);
ByteAddressBuffer currentFrame : register(t1);
RWByteAddressBuffer flags : register(u0);
ByteAddressBuffer flowForward : register(t2);
ByteAddressBuffer flowCost : register(t3);
ByteAddressBuffer flowBackward : register(t4);

static const uint kColumns = 64;
static const uint kRows = 36;
static const uint kSamples = kColumns * kRows;    // 9 per thread
static const uint kPerThread = kSamples / 256;
static const float kInitialBaseline = 0.02;       // a quiet shot (typical pair mrSad 0.01-0.03)
static const float kInitialBaselineN = 0.25;
static const float kInitialBaselineW = 0.02;
static const uint kStateOffset = 64;
static const uint kBarsOffset = 80;
static const uint kSpikeOffset = 112;               // last moving pair: mrSad, histogram16, valid
static const uint kColMaxOffset = 128;
static const uint kRowMaxOffset = 640;
static const uint kTraceOffset = 928;
static const uint kScoreOffset = 1152;              // the last kScoreHist moving pairs' zoomRes, chroma, histogram16, edge; count; head
static const uint kScoreHist = 6;
// with NVOF (forward, backward, cost): the 11 features, then flowRes, flowLap, flowFb, flowCost, flowCostHi, then the bias
static const float kScoreWF[17] = { 2.4888, 1.8562, 0.9291, 0.0774, 0.5541, 2.5735, 2.3833, 0.9605, 1.3440, 1.4429, 1.0801, 1.8154, -1.6515, -1.6559, 0.1255, 0.0000, -4.6165 };
// NVOF's forward and backward vectors agreeing on more than this share of the frame: motion (a flash, an effect over a
// moving scene), never a cut; real cuts reached 0.30 at most in the calibration (round 4)
static const float kFlowVetoFb = 0.45;
// without NVOF but with Fourier-Mellin (round 5): the 11 features, then fmRes, fmPeak, fmPeakT, |ln scale|, the bias;
// threshold between the non-cuts' maximum 4.79 and the new hits 7.22 / 8.80
static const float kScoreWM[16] = { 2.4749, 2.3527, 1.9973, 0.4635, 1.3661, 2.0152, 2.3969, 1.2112, 1.6009, 1.1158, 2.1457, 1.7366, -1.2315, -1.6694, 1.1647, -6.9630 };
static const float kScoreThresholdFm = 6.0;
// soft response (round 6): with NVOF, a pair scoring above this but below the cut threshold is shown nearest-real
// (generated slots up to the midpoint show A, later ones B, no morph); without NVOF there is no soft band (its
// uncertain range held more non-cuts than cuts). Words at kSoftOffset + parity * 16: hard-or-soft, soft-only.
static const float kSoftThresholdFlow = 3.6;
static const uint kSoftOffset = 1440;
// weights of the scene score (logistic fit on the study, synthetic and IMAX-switching segments, round 3; features in
// the order of x[] in main, then the bias)
static const float kScoreW[12] = { 3.6174, 2.4906, 2.0914, 0.8920, 1.5484, 2.1688, 2.4610, 1.2270, 1.7546, 1.1497, 2.9329, -7.0951 };
static const uint kLowW = 96, kLowH = 54, kLowTexels = kLowW * kLowH;   // block matching image (box-filtered)
static const uint kBlockW = 8, kBlockH = 6, kBlocksX = 12, kBlocksY = 9, kBlocks = kBlocksX * kBlocksY;
static const uint kSearchX = 13, kSearchY = 9, kCandidates = kSearchX * kSearchY;   // +-6 / +-4 texels
static const uint kProfX = 128;                   // column profile bins
static const uint kProfY = 72;                    // row profile bins
static const uint kProfTaps = 32;                 // samples per profile bin
static const uint kScales = 11;
static const uint kShiftsX = 49;                  // -24..24 bins
static const uint kShiftsY = 33;                  // -16..16 bins

groupshared uint histogramPrevious[256];
groupshared uint histogramCurrent[256];
groupshared uint histNPrevious[16];
groupshared uint histNCurrent[16];
groupshared uint sadSum;
groupshared uint histogramL1;
groupshared uint histogram16L1;
groupshared uint histogramN16L1;
groupshared uint sumPrevious;
groupshared uint sumCurrent;
groupshared uint sqPrevious;
groupshared uint sqCurrent;
groupshared uint centredSadSum;   // sum of |(b - a) - (meanB - meanA)| in 1/16 code values
groupshared uint boxSadSum;       // the same with 2x2 box samples (the warp check's reference)
groupshared uint warpSadSum;      // the same with A warped by the fitted motion
groupshared uint warpCount;
groupshared float profAx[kProfX];
groupshared float profBx[kProfX];
groupshared float profAy[kProfY];
groupshared float profBy[kProfY];
groupshared uint colMaxB[kProfX];
groupshared uint rowMaxB[kProfY];
groupshared uint bestX;
groupshared uint bestY;
groupshared uint rect[4];         // active area x0, x1, y0, y1 (pixels, exclusive end)
groupshared float stats[8];       // meanA, meanB, sigA, sigB, loA, hiA, loB, hiB (code values)
groupshared float profMeans[4];   // mean of profAx, profBx, profAy, profBy
groupshared float warp[4];        // sx, tx (pixels), sy, ty (pixels)
groupshared uint warpValid;
groupshared uint lowA[kLowTexels / 2];   // two 16-bit texels per uint (sums of 4 luma samples)
groupshared uint lowB[kLowTexels / 2];
groupshared uint blockMin[kBlocks];
groupshared uint blockZero[kBlocks];
groupshared uint mcSum;
groupshared uint zeroSum;
// scene measures (bit 6): chroma histograms (8 x 8 bins of B-Y / R-Y) of the 64x36 grid, orientation histograms
// (2 x 2 quadrants x 8 orientations, log luma, magnitude weighted) of the 96x54 image, the zoom-aware fit
groupshared uint chromaA[64];
groupshared uint chromaB[64];
groupshared uint chromaL1;
groupshared uint edgeHA[32];
groupshared uint edgeHB[32];
groupshared uint zoomCoarse;
groupshared uint zoomFine;
groupshared uint zoomSum;
groupshared uint zoomN;
groupshared uint zoomZeroSum;
groupshared uint zoomDevSum;
groupshared uint zoomDevSumA;
// local match (bit 6): per block of B the best match in A around the zoom fit's prediction or in place (+-4 / +-3 texels),
// over the block's own mean deviation; a cut leaves no textured block matching, motion and effects leave some
groupshared uint lmMin[kBlocks];
groupshared uint lmDev[kBlocks];
groupshared uint lmHist[32];
// optical flow (bit 7): a 32 x 18 lattice of forward vectors inside the active area (frame-normalised, half2), sums
static const uint kLatX = 32, kLatY = 18, kLat = kLatX * kLatY;
groupshared uint flowLat[kLat];
groupshared uint flowLapGood;
groupshared uint flowFbGood;
groupshared uint flowCostSum;
groupshared uint flowCostHigh;
groupshared uint flowResSum;
groupshared uint flowResN;
groupshared uint flowLatN;
// Fourier-Mellin (bit 8, VfgFourierMellin.hlsl ran before this test): the residual after its similarity transform, for
// the 8 sign conventions of scale, rotation and translation (the best is kept)
static const uint kFmOffset = 1408;
groupshared uint fmSum[8];
groupshared uint fmN[8];

static const uint kZs = 11, kZx = 13, kZy = 9, kZoomCoarse = kZs * kZx * kZy;    // scales 1/1.8..1.8, shifts +-24 / +-16 texels
static const uint kLmX = 5, kLmY = 5, kLmCandidates = kLmX * kLmY + 9;   // around the zoom fit (+-2), in place (+-1)
static const uint kZoomFine = 125;                                                 // 5 scales x 5 x 5 shifts around the best
static const float kZoomStep = 4.0;                                                // texels between coarse shifts
static const float kScale[kScales] = { 0.80, 0.85, 0.90, 0.94, 0.97, 1.0, 1.03, 1.06, 1.11, 1.17, 1.25 };

// Integer luma weights of the CPU sampler (packedIngressLuma): 8-bit
// (54R+183G+19B+128)>>8, 10-bit min(255,(54R+183G+19B+512)>>10).
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
// 2x2 box around a (fractional) pixel position, clamped to the frame; d = half the tap distance
float boxAt(ByteAddressBuffer frame, float x, float y, float d)
{
    const float xm = clamp(x - d, 0.0, float(width - 1)), xp = clamp(x + d, 0.0, float(width - 1));
    const float ym = clamp(y - d, 0.0, float(height - 1)), yp = clamp(y + d, 0.0, float(height - 1));
    return 0.25 * float(lumaAt(frame, uint(xm), uint(ym)) + lumaAt(frame, uint(xp), uint(ym)) +
                        lumaAt(frame, uint(xm), uint(yp)) + lumaAt(frame, uint(xp), uint(yp)));
}
// profile value at a fractional bin (linear interpolation); -1 outside
float profileAt(uint which, float bin, uint count)
{
    if (bin < 0.0 || bin > float(count - 1)) return -1.0;
    const uint i = min(uint(bin), count - 2);
    const float f = bin - float(i);
    if (which == 0) return lerp(profAx[i], profAx[i + 1], f);
    return lerp(profAy[i], profAy[i + 1], f);
}
// mean-removed L1 distance between B's profile and A's profile mapped by (scale, shift); bins of A outside are skipped
float profileError(uint axis, float scale, float shift)
{
    const uint count = axis == 0 ? kProfX : kProfY;
    const float centre = 0.5 * float(count - 1);
    const float mA = profMeans[axis == 0 ? 0 : 2], mB = profMeans[axis == 0 ? 1 : 3];
    float sum = 0.0; uint n = 0;
    for (uint x = 0; x < count; ++x) {
        const float a = profileAt(axis, centre + scale * (float(x) - centre) + shift, count);
        if (a < 0.0) continue;
        const float b = axis == 0 ? profBx[x] : profBy[x];
        sum += abs((b - mB) - (a - mA));
        ++n;
    }
    return n * 10 >= count * 6 ? sum / (255.0 * float(n)) : 1.0;   // at least 60 % overlap
}
uint lowAt(uint which, uint t) { const uint w = which == 0 ? lowA[t >> 1] : lowB[t >> 1]; return (w >> ((t & 1u) * 16u)) & 0xFFFFu; }
uint packGeom(uint4 g) { return g.x | (g.y << 8) | (g.z << 16) | (g.w << 24); }
uint4 unpackGeom(uint p) { return uint4(p & 255u, (p >> 8) & 255u, (p >> 16) & 255u, p >> 24); }
bool nearGeom(uint4 a, uint4 b) { return all(uint4(abs(int4(a) - int4(b))) <= uint4(1, 1, 1, 1)); }
uint keyOf(float err, uint candidate) { return (min(uint(saturate(err) * 2097151.0), 2097151u) << 10) | candidate; }
uint zoomKey(float err, uint candidate) { return (min(uint(saturate(err) * 524287.0), 524287u) << 12) | candidate; }
// B-Y / R-Y of one pixel in 8 x 8 bins of 24 code values (+-96)
uint chromaBin(ByteAddressBuffer frame, uint x, uint y)
{
    const uint word = frame.Load(y * pitch + x * 4);
    int r, g, b;
    if (tenBit != 0) { r = int(word & 1023u) >> 2; g = int((word >> 10) & 1023u) >> 2; b = int((word >> 20) & 1023u) >> 2; }
    else { r = int(word & 255u); g = int((word >> 8) & 255u); b = int((word >> 16) & 255u); }
    const int yy = (54 * r + 183 * g + 19 * b + 128) >> 8;
    return uint(clamp((b - yy + 96) / 24, 0, 7) * 8 + clamp((r - yy + 96) / 24, 0, 7));
}
// flow vector at grid cell (gx, gy) of buffer b, in flow pixels (NVOF S10.5, FidelityFX integer pixels)
float2 flowAt(uint which, uint gx, uint gy)
{
    const uint w = which == 0 ? flowForward.Load(gy * flowPitch + gx * 4) : flowBackward.Load(gy * flowPitch + gx * 4);
    const float2 v = float2(float(int(w << 16) >> 16), float(int(w) >> 16));
    return (flowFlags & 8u) != 0 ? v : v / 32.0;
}
uint costAt(uint gx, uint gy) { const uint a = gy * costPitch + gx; return (flowCost.Load(a & ~3u) >> ((a & 3u) * 8u)) & 255u; }
float lowLog(uint which, uint t) { return log2(float(lowAt(which, t)) + 16.0); }
// A's 96x54 image at a fractional texel (bilinear)
float lowA2(float x, float y)
{
    const uint x0 = min(uint(x), kLowW - 2), y0 = min(uint(y), kLowH - 2);
    const float fx = x - float(x0), fy = y - float(y0);
    const uint t0 = y0 * kLowW + x0;
    return lerp(lerp(float(lowAt(0, t0)), float(lowAt(0, t0 + 1)), fx), lerp(float(lowAt(0, t0 + kLowW)), float(lowAt(0, t0 + kLowW + 1)), fx), fy);
}
float zoomScaleAt(float i) { return exp2((i - 0.5 * float(kZs - 1)) * (log2(1.8) / (0.5 * float(kZs - 1)))); }   // 1/1.8 .. 1.8, 1 exact
// mean-removed difference of B against A mapped by scale s about the image centre and shift (tx, ty), on every
// step-th texel; at least 55 % of the points must land inside A (else 1e9). Texel units (sums of 4 samples).
float zoomError(float s, float tx, float ty, uint step, uint offset, float shift4)
{
    const float cx = 0.5 * float(kLowW - 1), cy = 0.5 * float(kLowH - 1);
    float sum = 0.0; uint n = 0, total = 0;
    for (uint y = offset; y < kLowH; y += step)
        for (uint x = offset; x < kLowW; x += step) {
            ++total;
            const float xa = cx + s * (float(x) - cx) + tx, ya = cy + s * (float(y) - cy) + ty;
            if (xa < 0.0 || ya < 0.0 || xa > float(kLowW - 1) || ya > float(kLowH - 1)) continue;
            sum += abs(float(lowAt(1, y * kLowW + x)) - lowA2(xa, ya) - shift4);
            ++n;
        }
    return n * 20 >= total * 11 ? sum / float(n) : 1e9;
}

[numthreads(256, 1, 1)]
void main(uint index : SV_GroupIndex)
{
    histogramPrevious[index] = 0;
    histogramCurrent[index] = 0;
    if (index < 16) { histNPrevious[index] = 0; histNCurrent[index] = 0; }
    if (index < kProfX) colMaxB[index] = 0;
    if (index < kProfY) rowMaxB[index] = 0;
    if (index < kBlocks) { blockMin[index] = 0xFFFFFFFFu; blockZero[index] = 0; }
    if (index < 64) { chromaA[index] = 0; chromaB[index] = 0; }
    if (index < 32) { edgeHA[index] = 0; edgeHB[index] = 0; lmHist[index] = 0; }
    if (index < 8) { fmSum[index] = 0; fmN[index] = 0; }
    if (index < kBlocks) { lmMin[index] = 0xFFFFFFFFu; lmDev[index] = 0; }
    const bool flowOn = (features & 128u) != 0 && (flowFlags & 1u) != 0;
    const bool barsOn = (features & 1u) != 0, warpOn = (features & 2u) != 0, darkOn = (features & 4u) != 0;
    const bool mcOn = (features & 16u) != 0, barCutOn = (features & 32u) != 0, scoreOn = (features & 64u) != 0;
    if (index == 0) {
        sadSum = 0; histogramL1 = 0; histogram16L1 = 0; histogramN16L1 = 0;
        sumPrevious = 0; sumCurrent = 0; sqPrevious = 0; sqCurrent = 0;
        centredSadSum = 0; boxSadSum = 0; warpSadSum = 0; warpCount = 0; mcSum = 0; zeroSum = 0;
        bestX = 0xFFFFFFFFu; bestY = 0xFFFFFFFFu; warpValid = 0;
        flowLapGood = 0; flowFbGood = 0; flowCostSum = 0; flowCostHigh = 0; flowResSum = 0; flowResN = 0; flowLatN = 0;
        chromaL1 = 0; zoomCoarse = 0xFFFFFFFFu; zoomFine = 0xFFFFFFFFu; zoomSum = 0; zoomN = 0; zoomZeroSum = 0; zoomDevSum = 0; zoomDevSumA = 0;
        // the active area: fixed by the caller, or the bar geometry in use (the whole frame until bars are found)
        uint x0 = 0, x1 = width, y0 = 0, y1 = height;
        if (areaX1 > areaX0 && areaY1 > areaY0) {
            x0 = uint(saturate(areaX0) * width); x1 = max(x0 + 64, uint(saturate(areaX1) * width));
            y0 = uint(saturate(areaY0) * height); y1 = max(y0 + 36, uint(saturate(areaY1) * height));
        } else if (barsOn && flags.Load(kBarsOffset + 4) != 0) {
            // a bin partly in the picture counts as bar: keep a bin's margin inside the picture
            const uint4 g = unpackGeom(flags.Load(kBarsOffset));
            const uint4 r = uint4(((g.x + (g.x ? 1u : 0u)) * width) / kProfX, width - ((g.y + (g.y ? 1u : 0u)) * width) / kProfX,
                                  ((g.z + (g.z ? 1u : 0u)) * height) / kProfY, height - ((g.w + (g.w ? 1u : 0u)) * height) / kProfY);
            if (r.y > r.x + width / 4 && r.w > r.z + height / 4) { x0 = r.x; x1 = r.y; y0 = r.z; y1 = r.w; }
        }
        rect[0] = x0; rect[1] = min(x1, width); rect[2] = y0; rect[3] = min(y1, height);
    }
    GroupMemoryBarrierWithGroupSync();
    const uint rx0 = rect[0], rw = rect[1] - rect[0], ry0 = rect[2], rh = rect[3] - rect[2];

    // --- pass 1: the 64x36 grid (histograms, sums), the profiles and the bar maxima
    uint a[kPerThread], b[kPerThread];
    uint count = 0;
    if (compare != 0) {
        for (uint i = index; i < kSamples; i += 256) {
            const uint x = rx0 + (((i % kColumns) * 2u + 1u) * rw) / (2u * kColumns);
            const uint y = ry0 + (((i / kColumns) * 2u + 1u) * rh) / (2u * kRows);
            const uint pa = lumaAt(previousFrame, x, y);
            const uint pb = lumaAt(currentFrame, x, y);
            a[count] = pa;
            b[count] = pb;
            InterlockedAdd(histogramPrevious[pa], 1);
            InterlockedAdd(histogramCurrent[pb], 1);
            InterlockedAdd(sadSum, pa > pb ? pa - pb : pb - pa);
            InterlockedAdd(sumPrevious, pa);
            InterlockedAdd(sumCurrent, pb);
            InterlockedAdd(sqPrevious, pa * pa);
            InterlockedAdd(sqCurrent, pb * pb);
            if (scoreOn) { InterlockedAdd(chromaA[chromaBin(previousFrame, x, y)], 1); InterlockedAdd(chromaB[chromaBin(currentFrame, x, y)], 1); }
            ++count;
        }
    }
    // profiles inside the active area; the bar maxima of B over the whole frame (every frame)
    const bool findBars = barsOn && !(areaX1 > areaX0);
    // the block-matching image of both frames inside the active area (each texel = 4 samples of its cell)
    if (compare != 0 && (mcOn || scoreOn)) {
        for (uint k = index; k < kLowTexels / 2; k += 256) {
            uint pa = 0, pb = 0;
            [unroll] for (uint h = 0; h < 2; ++h) {
                const uint tx = (2u * k + h) % kLowW, ty = (2u * k + h) / kLowW;
                const uint xa = rx0 + ((4u * tx + 1u) * rw) / (4u * kLowW), xb = rx0 + ((4u * tx + 3u) * rw) / (4u * kLowW);
                const uint ya = ry0 + ((4u * ty + 1u) * rh) / (4u * kLowH), yb = ry0 + ((4u * ty + 3u) * rh) / (4u * kLowH);
                const uint sa = lumaAt(previousFrame, xa, ya) + lumaAt(previousFrame, xb, ya) + lumaAt(previousFrame, xa, yb) + lumaAt(previousFrame, xb, yb);
                const uint sb = lumaAt(currentFrame, xa, ya) + lumaAt(currentFrame, xb, ya) + lumaAt(currentFrame, xa, yb) + lumaAt(currentFrame, xb, yb);
                pa |= sa << (16u * h); pb |= sb << (16u * h);
            }
            lowA[k] = pa; lowB[k] = pb;
        }
    }
    const uint gridW = flowGrid & 0xFFFFu, gridH = flowGrid >> 16, flowW = flowExtent & 0xFFFFu, flowH = flowExtent >> 16;
    if (compare != 0 && flowOn) {
        for (uint li = index; li < kLat; li += 256) {
            // the cell under this lattice point (frame-normalised position u inside the active area)
            const float2 u = float2((float(rx0) + (float(li % kLatX) + 0.5) * float(rw) / float(kLatX)) / float(width),
                                    (float(ry0) + (float(li / kLatX) + 0.5) * float(rh) / float(kLatY)) / float(height));
            const uint gx = min(uint(u.x * float(gridW)), gridW - 1), gy = min(uint(u.y * float(gridH)), gridH - 1);
            const float2 v = flowAt(0, gx, gy);
            flowLat[li] = f32tof16(v.x / float(flowW)) | (f32tof16(v.y / float(flowH)) << 16);
            if ((flowFlags & 2u) != 0) { const uint c = costAt(gx, gy); InterlockedAdd(flowCostSum, c); InterlockedAdd(flowCostHigh, c >= 128u ? 1u : 0u); }
            if ((flowFlags & 4u) != 0) {
                // backward vector where the forward one lands; consistent when they cancel within 1.5 flow px + 5 %
                const float2 q = u * float2(flowW, flowH) + v;
                const uint bx = uint(clamp(q.x / float(flowW) * float(gridW), 0.0, float(gridW - 1))), by = uint(clamp(q.y / float(flowH) * float(gridH), 0.0, float(gridH - 1)));
                const float2 r = flowAt(1, bx, by);
                InterlockedAdd(flowFbGood, length(v + r) < 1.5 + 0.05 * length(v) ? 1u : 0u);
            }
            InterlockedAdd(flowLatN, 1u);
        }
    }
    if (index < kProfX) {
        const float x = float(rx0) + (float(index) + 0.5) * float(rw) / float(kProfX);
        float sa = 0.0, sb = 0.0;
        if (compare != 0 && warpOn)
            for (uint k = 0; k < kProfTaps; ++k) {
                const uint y = ry0 + ((k * 2u + 1u) * rh) / (2u * kProfTaps);
                sa += float(lumaAt(previousFrame, uint(x), y));
                sb += float(lumaAt(currentFrame, uint(x), y));
            }
        profAx[index] = sa / float(kProfTaps);
        profBx[index] = sb / float(kProfTaps);
        if (findBars) {
            uint m = 0;
            const uint fx = ((index * 2u + 1u) * width) / (2u * kProfX);
            uint n = 0;
            for (uint k = 0; k < kProfTaps; ++k) { const uint v = lumaAt(currentFrame, fx, ((k * 2u + 1u) * height) / (2u * kProfTaps)); m = max(m, v); n += v > uint(barLuma * 255.0) ? 1u : 0u; }
            colMaxB[index] = m | (n << 16);
        }
    } else if (index < kProfX + kProfY) {
        const uint r = index - kProfX;
        const float y = float(ry0) + (float(r) + 0.5) * float(rh) / float(kProfY);
        float sa = 0.0, sb = 0.0;
        if (compare != 0 && warpOn)
            for (uint k = 0; k < kProfTaps; ++k) {
                const uint x = rx0 + ((k * 2u + 1u) * rw) / (2u * kProfTaps);
                sa += float(lumaAt(previousFrame, x, uint(y)));
                sb += float(lumaAt(currentFrame, x, uint(y)));
            }
        profAy[r] = sa / float(kProfTaps);
        profBy[r] = sb / float(kProfTaps);
        if (findBars) {
            uint m = 0;
            const uint fy = ((r * 2u + 1u) * height) / (2u * kProfY);
            uint n = 0;
            for (uint k = 0; k < kProfTaps; ++k) { const uint v = lumaAt(currentFrame, ((k * 2u + 1u) * width) / (2u * kProfTaps), fy); m = max(m, v); n += v > uint(barLuma * 255.0) ? 1u : 0u; }
            rowMaxB[r] = m | (n << 16);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // --- pass 2: histogram distances, frame statistics, profile means
    if (compare != 0 && flowOn) {
        for (uint li = index; li < kLat; li += 256) {
            const uint lx = li % kLatX, ly = li / kLatX;
            const float2 v = float2(f16tof32(flowLat[li] & 0xFFFFu), f16tof32(flowLat[li] >> 16));
            float lap = 0.0; uint terms = 0;
            if (lx > 0 && lx + 1 < kLatX) {
                const uint a = flowLat[li - 1], b = flowLat[li + 1];
                lap += length(float2(f16tof32(a & 0xFFFFu), f16tof32(a >> 16)) + float2(f16tof32(b & 0xFFFFu), f16tof32(b >> 16)) - 2.0 * v); ++terms;
            }
            if (ly > 0 && ly + 1 < kLatY) {
                const uint a = flowLat[li - kLatX], b = flowLat[li + kLatX];
                lap += length(float2(f16tof32(a & 0xFFFFu), f16tof32(a >> 16)) + float2(f16tof32(b & 0xFFFFu), f16tof32(b >> 16)) - 2.0 * v); ++terms;
            }
            if (terms > 0) InterlockedAdd(flowLapGood, lap / float(terms) < 0.004 + 0.1 * length(v) ? 1u : 0u);
        }
    }
    if (scoreOn && index < 64) { const uint ca = chromaA[index], cb = chromaB[index]; InterlockedAdd(chromaL1, ca > cb ? ca - cb : cb - ca); }
    if (scoreOn && compare != 0) {
        for (uint ti = index; ti < kLowTexels / 4; ti += 256) {   // every other texel in x and y
            const uint ex = (ti % (kLowW / 2)) * 2 + 1, ey = (ti / (kLowW / 2)) * 2 + 1, te = ey * kLowW + ex;
            if (ex == 0 || ey == 0 || ex == kLowW - 1 || ey == kLowH - 1) continue;
            const uint q = (ex >= kLowW / 2 ? 8u : 0u) + (ey >= kLowH / 2 ? 16u : 0u);
            {
                const float gx = lowLog(0, te + 1) - lowLog(0, te - 1), gy = lowLog(0, te + kLowW) - lowLog(0, te - kLowW), m = abs(gx) + abs(gy);
                if (m >= 0.05) { float ang = atan2(gy, gx); if (ang < 0.0) ang += 3.14159265; InterlockedAdd(edgeHA[q + min(uint(ang * (8.0 / 3.14159265)), 7u)], uint(m * 64.0 + 0.5)); }
            }
            {
                const float gx = lowLog(1, te + 1) - lowLog(1, te - 1), gy = lowLog(1, te + kLowW) - lowLog(1, te - kLowW), m = abs(gx) + abs(gy);
                if (m >= 0.05) { float ang = atan2(gy, gx); if (ang < 0.0) ang += 3.14159265; InterlockedAdd(edgeHB[q + min(uint(ang * (8.0 / 3.14159265)), 7u)], uint(m * 64.0 + 0.5)); }
            }
        }
    }
    const uint p = histogramPrevious[index], c = histogramCurrent[index];
    InterlockedAdd(histogramL1, p > c ? p - c : c - p);
    if (index < 16) {
        uint p16 = 0, c16 = 0;
        for (uint k = 0; k < 16; ++k) { p16 += histogramPrevious[index * 16 + k]; c16 += histogramCurrent[index * 16 + k]; }
        InterlockedAdd(histogram16L1, p16 > c16 ? p16 - c16 : c16 - p16);
    }
    if (index == 16 || index == 17) {
        // mean, deviation and the 2 % / 98 % points of one frame
        const bool first = index == 16;
        const float n = float(kSamples);
        const float mean = float(first ? sumPrevious : sumCurrent) / n;
        const float var = max(0.0, float(first ? sqPrevious : sqCurrent) / n - mean * mean);
        uint acc = 0, lo = 0, hi = 255;
        const uint loCount = kSamples / 50, hiCount = kSamples - kSamples / 50;
        bool loFound = false, hiFound = false;
        for (uint k = 0; k < 256; ++k) {
            acc += first ? histogramPrevious[k] : histogramCurrent[k];
            if (!loFound && acc > loCount) { lo = k; loFound = true; }
            if (!hiFound && acc >= hiCount) { hi = k; hiFound = true; }
        }
        stats[first ? 0 : 1] = mean;
        stats[first ? 2 : 3] = sqrt(var);
        stats[first ? 4 : 6] = float(lo);
        stats[first ? 5 : 7] = float(hi);
    }
    if (index >= 32 && index < 36) {
        const uint which = index - 32;
        const uint n = which < 2 ? kProfX : kProfY;
        float s = 0.0;
        for (uint k = 0; k < n; ++k) s += which == 0 ? profAx[k] : which == 1 ? profBx[k] : which == 2 ? profAy[k] : profBy[k];
        profMeans[which] = s / float(n);
    }
    GroupMemoryBarrierWithGroupSync();

    // --- pass 3: mean-removed SAD, the stretched 16-bin histograms, the motion fit
    const float meanShift = stats[1] - stats[0];
    {
        uint centred = 0;
        const float loA = stats[4], spanA = max(stats[5] - stats[4], 8.0);
        const float loB = stats[6], spanB = max(stats[7] - stats[6], 8.0);
        for (uint j = 0; j < count; ++j) {
            centred += uint(abs(float(b[j]) - float(a[j]) - meanShift) * 16.0 + 0.5);
            InterlockedAdd(histNPrevious[uint(clamp((float(a[j]) - loA) * 16.0 / spanA, 0.0, 15.0))], 1);
            InterlockedAdd(histNCurrent[uint(clamp((float(b[j]) - loB) * 16.0 / spanB, 0.0, 15.0))], 1);
        }
        InterlockedAdd(centredSadSum, centred);
    }
    if (compare != 0 && warpOn) {
        for (uint cand = index; cand < kScales * kShiftsX; cand += 256) {
            const float e = profileError(0, kScale[cand / kShiftsX], float(int(cand % kShiftsX) - int(kShiftsX / 2)));
            InterlockedMin(bestX, keyOf(e, cand));
        }
        for (uint cand = index; cand < kScales * kShiftsY; cand += 256) {
            const float e = profileError(1, kScale[cand / kShiftsY], float(int(cand % kShiftsY) - int(kShiftsY / 2)));
            InterlockedMin(bestY, keyOf(e, cand));
        }
    }
    if (compare != 0 && mcOn) {
        const float shift4 = meanShift * 4.0;   // texels are sums of 4 samples
        for (uint w = index; w < kBlocks * kCandidates; w += 256) {
            const uint blk = w / kCandidates, cand = w % kCandidates;
            const int dx = int(cand % kSearchX) - int(kSearchX / 2), dy = int(cand / kSearchX) - int(kSearchY / 2);
            const int bx = int((blk % kBlocksX) * kBlockW), by = int((blk / kBlocksX) * kBlockH);
            const int ax = bx + dx, ay = by + dy;
            if (ax < 0 || ay < 0 || ax + int(kBlockW) > int(kLowW) || ay + int(kBlockH) > int(kLowH)) continue;
            float s = 0.0;
            for (uint y = 0; y < kBlockH; ++y)
                for (uint x = 0; x < kBlockW; ++x)
                    s += abs(float(lowAt(1, uint(by + int(y)) * kLowW + uint(bx) + x)) - float(lowAt(0, uint(ay + int(y)) * kLowW + uint(ax) + x)) - shift4);
            const uint q = uint(s + 0.5);
            InterlockedMin(blockMin[blk], q);
            if (dx == 0 && dy == 0) blockZero[blk] = q;
        }
    }
    if (compare != 0 && scoreOn) {
        const float shift4 = meanShift * 4.0;
        for (uint cand = index; cand < kZoomCoarse; cand += 256) {
            const uint si = cand / (kZx * kZy), r = cand % (kZx * kZy);
            const float e = zoomError(zoomScaleAt(float(si)), (float(r / kZy) - 0.5 * float(kZx - 1)) * kZoomStep, (float(r % kZy) - 0.5 * float(kZy - 1)) * kZoomStep, 6, 3, meanShift * 4.0);
            InterlockedMin(zoomCoarse, zoomKey(e / 1020.0, cand));
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (compare != 0 && scoreOn && zoomCoarse != 0xFFFFFFFFu) {
        const uint c0 = zoomCoarse & 4095u, si = c0 / (kZx * kZy), r = c0 % (kZx * kZy);
        const float tx0 = (float(r / kZy) - 0.5 * float(kZx - 1)) * kZoomStep, ty0 = (float(r % kZy) - 0.5 * float(kZy - 1)) * kZoomStep;
        for (uint cand = index; cand < kZoomFine; cand += 256) {
            const float s = zoomScaleAt(float(si) + (float(cand / 25) - 2.0) * 0.25);
            const float e = zoomError(s, tx0 + (float((cand / 5) % 5) - 2.0) * 0.375 * kZoomStep, ty0 + (float(cand % 5) - 2.0) * 0.375 * kZoomStep, 3, 1, meanShift * 4.0);
            InterlockedMin(zoomFine, zoomKey(e / 1020.0, cand));
        }
    }
    if (index < 16) {
        const uint pn = histNPrevious[index], cn = histNCurrent[index];
        InterlockedAdd(histogramN16L1, pn > cn ? pn - cn : cn - pn);
    }
    if (index == 0 && compare != 0 && warpOn && bestX != 0xFFFFFFFFu && bestY != 0xFFFFFFFFu) {
        // decode, refine the shift to a fraction of a bin (parabola through the neighbours), to pixels
        const uint cx = bestX & 1023u, cy = bestY & 1023u;
        const float sx = kScale[cx / kShiftsX], sy = kScale[cy / kShiftsY];
        float tx = float(int(cx % kShiftsX) - int(kShiftsX / 2)), ty = float(int(cy % kShiftsY) - int(kShiftsY / 2));
        {
            const float e0 = profileError(0, sx, tx - 1.0), e1 = profileError(0, sx, tx), e2 = profileError(0, sx, tx + 1.0);
            const float d = e0 - 2.0 * e1 + e2;
            if (d > 1e-6) tx += clamp(0.5 * (e0 - e2) / d, -0.5, 0.5);
        }
        {
            const float e0 = profileError(1, sy, ty - 1.0), e1 = profileError(1, sy, ty), e2 = profileError(1, sy, ty + 1.0);
            const float d = e0 - 2.0 * e1 + e2;
            if (d > 1e-6) ty += clamp(0.5 * (e0 - e2) / d, -0.5, 0.5);
        }
        warp[0] = sx; warp[1] = tx * float(rw) / float(kProfX);
        warp[2] = sy; warp[3] = ty * float(rh) / float(kProfY);
        warpValid = 1;
    }
    GroupMemoryBarrierWithGroupSync();

    // --- pass 4: the box and warped residuals on the grid
    if (compare != 0 && warpOn && warpValid != 0) {
        const float d = max(1.0, float(rw) / 512.0);
        const float cxp = float(rx0) + 0.5 * float(rw), cyp = float(ry0) + 0.5 * float(rh);
        uint boxSum = 0, warpSum = 0, n = 0;
        for (uint i = index; i < kSamples; i += 256) {
            const float x = float(rx0) + (float(i % kColumns) + 0.5) * float(rw) / float(kColumns);
            const float y = float(ry0) + (float(i / kColumns) + 0.5) * float(rh) / float(kRows);
            const float bb = boxAt(currentFrame, x, y, d);
            const float ab = boxAt(previousFrame, x, y, d);
            boxSum += uint(abs(bb - ab - meanShift) * 16.0 + 0.5);
            const float xa = cxp + warp[0] * (x - cxp) + warp[1];
            const float ya = cyp + warp[2] * (y - cyp) + warp[3];
            if (xa >= float(rx0) && xa <= float(rx0 + rw - 1) && ya >= float(ry0) && ya <= float(ry0 + rh - 1)) {
                warpSum += uint(abs(bb - boxAt(previousFrame, xa, ya, d) - meanShift) * 16.0 + 0.5);
                ++n;
            }
        }
        InterlockedAdd(boxSadSum, boxSum);
        InterlockedAdd(warpSadSum, warpSum);
        InterlockedAdd(warpCount, n);
    }
    if (compare != 0 && mcOn && index < kBlocks && blockMin[index] != 0xFFFFFFFFu) { InterlockedAdd(mcSum, blockMin[index]); InterlockedAdd(zeroSum, blockZero[index]); }
    if (compare != 0 && flowOn && scoreOn) {
        const float shift4 = meanShift * 4.0;
        uint fs = 0, fn = 0;
        for (uint tf = index; tf < kLowTexels; tf += 256) {
            const float2 u = float2((float(rx0) + (float(tf % kLowW) + 0.5) * float(rw) / float(kLowW)) / float(width),
                                    (float(ry0) + (float(tf / kLowW) + 0.5) * float(rh) / float(kLowH)) / float(height));
            const uint gx = min(uint(u.x * float(gridW)), gridW - 1), gy = min(uint(u.y * float(gridH)), gridH - 1);
            const float2 ua = u + flowAt(0, gx, gy) / float2(flowW, flowH);
            const float xa = (ua.x * float(width) - float(rx0)) / float(rw) * float(kLowW) - 0.5, ya = (ua.y * float(height) - float(ry0)) / float(rh) * float(kLowH) - 0.5;
            if (xa < 0.0 || ya < 0.0 || xa > float(kLowW - 1) || ya > float(kLowH - 1)) continue;
            fs += uint(abs(float(lowAt(1, tf)) - lowA2(xa, ya) - shift4) + 0.5);
            ++fn;
        }
        InterlockedAdd(flowResSum, fs); InterlockedAdd(flowResN, fn);
    }
    const bool fmOn = (features & 256u) != 0 && scoreOn && compare != 0 && !(flowOn && (flowFlags & 6u) == 6u);   // as PushMpcCutGuard runs it
    if (fmOn) {
        // the similarity: B(x) ~ A(c + R(-phi) (x + t - c) / s), in pixels (the FM grid is 64 x 64 over the area)
        const float lnS = asfloat(flags.Load(kFmOffset)), phi = asfloat(flags.Load(kFmOffset + 4));
        const float2 tFm = float2(asfloat(flags.Load(kFmOffset + 12)), asfloat(flags.Load(kFmOffset + 16)));
        const float cx = 0.5 * float(kLowW - 1), cy = 0.5 * float(kLowH - 1), shift4 = meanShift * 4.0;
        for (uint w = index; w < 8 * 24 * 13; w += 256) {
            const uint v = w / (24 * 13), p = w % (24 * 13);
            const float x = float((p % 24) * 4 + 2), y = float((p / 24) * 4 + 2);
            const float s = exp((v & 1u) ? -lnS : lnS), ph = (v & 2u) ? -phi : phi;
            const float2 tp = ((v & 4u) ? -tFm : tFm) * float2(float(rw), float(rh)) / 64.0;
            const float2 q = (float2((x - cx) * float(rw) / float(kLowW), (y - cy) * float(rh) / float(kLowH)) + tp) / s;
            const float2 a = float2(cos(ph) * q.x + sin(ph) * q.y, -sin(ph) * q.x + cos(ph) * q.y);
            const float xa = cx + a.x * float(kLowW) / float(rw), ya = cy + a.y * float(kLowH) / float(rh);
            if (xa < 0.0 || ya < 0.0 || xa > float(kLowW - 1) || ya > float(kLowH - 1)) continue;
            InterlockedAdd(fmSum[v], uint(abs(float(lowAt(1, uint(y) * kLowW + uint(x))) - lowA2(xa, ya) - shift4) + 0.5));
            InterlockedAdd(fmN[v], 1u);
        }
    }
    float zS = 1.0, zTx = 0.0, zTy = 0.0;
    if (compare != 0 && scoreOn && zoomFine != 0xFFFFFFFFu) {
        const uint c0 = zoomCoarse & 4095u, si = c0 / (kZx * kZy), r = c0 % (kZx * kZy), cf = zoomFine & 4095u;
        zS = zoomScaleAt(float(si) + (float(cf / 25) - 2.0) * 0.25);
        zTx = (float(r / kZy) - 0.5 * float(kZx - 1)) * kZoomStep + (float((cf / 5) % 5) - 2.0) * 0.375 * kZoomStep;
        zTy = (float(r % kZy) - 0.5 * float(kZy - 1)) * kZoomStep + (float(cf % 5) - 2.0) * 0.375 * kZoomStep;
        const float cx = 0.5 * float(kLowW - 1), cy = 0.5 * float(kLowH - 1), shift4 = meanShift * 4.0, meanB4 = stats[1] * 4.0, meanA4 = stats[0] * 4.0;
        uint zs = 0, zn = 0, z0 = 0, dv = 0, dva = 0;
        for (uint tz = index; tz < kLowTexels; tz += 256) {
            const float x = float(tz % kLowW), y = float(tz / kLowW), bz = float(lowAt(1, tz));
            z0 += uint(abs(bz - float(lowAt(0, tz)) - shift4) + 0.5);
            dv += uint(abs(bz - meanB4) + 0.5);
            dva += uint(abs(float(lowAt(0, tz)) - meanA4) + 0.5);
            const float xa = cx + zS * (x - cx) + zTx, ya = cy + zS * (y - cy) + zTy;
            if (xa < 0.0 || ya < 0.0 || xa > float(kLowW - 1) || ya > float(kLowH - 1)) continue;
            zs += uint(abs(bz - lowA2(xa, ya) - shift4) + 0.5);
            ++zn;
        }
        InterlockedAdd(zoomSum, zs); InterlockedAdd(zoomN, zn); InterlockedAdd(zoomZeroSum, z0); InterlockedAdd(zoomDevSum, dv); InterlockedAdd(zoomDevSumA, dva);
        for (uint w = index; w < kBlocks * kLmCandidates; w += 256) {
            const uint blk = w / kLmCandidates, cand = w % kLmCandidates;
            const bool viaZoom = cand < kLmX * kLmY;
            const uint cc = viaZoom ? cand : cand - kLmX * kLmY;
            const float dx = viaZoom ? float(int(cc % kLmX) - int(kLmX / 2)) : float(int(cc % 3) - 1);
            const float dy = viaZoom ? float(int(cc / kLmX) - int(kLmY / 2)) : float(int(cc / 3) - 1);
            const uint bx = (blk % kBlocksX) * kBlockW, by = (blk / kBlocksX) * kBlockH;
            const float s = viaZoom ? zS : 1.0, ox = viaZoom ? zTx : 0.0, oy = viaZoom ? zTy : 0.0;
            float sum = 0.0; bool inside = true;
            for (uint y = 0; y < kBlockH && inside; ++y)
                for (uint x = 0; x < kBlockW; x += 2) {
                    const float xb = float(bx + x), yb = float(by + y);
                    const float xa = cx + s * (xb - cx) + ox + dx, ya = cy + s * (yb - cy) + oy + dy;
                    if (xa < 0.0 || ya < 0.0 || xa > float(kLowW - 1) || ya > float(kLowH - 1)) { inside = false; break; }
                    sum += abs(float(lowAt(1, (by + y) * kLowW + bx + x)) - lowA2(xa, ya) - shift4);
                }
            if (inside) InterlockedMin(lmMin[blk], uint(sum + 0.5));
        }
        if (index < kBlocks) {
            const uint bx = (index % kBlocksX) * kBlockW, by = (index / kBlocksX) * kBlockH;
            float m = 0.0;
            for (uint y = 0; y < kBlockH; ++y) for (uint x = 0; x < kBlockW; ++x) m += float(lowAt(1, (by + y) * kLowW + bx + x));
            m /= float(kBlockW * kBlockH);
            float d = 0.0;
            for (uint y = 0; y < kBlockH; ++y) for (uint x = 0; x < kBlockW; x += 2) d += abs(float(lowAt(1, (by + y) * kLowW + bx + x)) - m);
            lmDev[index] = uint(d + 0.5);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    DeviceMemoryBarrierWithGroupSync();

    if (index == 0) {
        const float sad = float(sadSum) / (255.0 * kSamples);
        const float histogram = float(histogramL1) / (2.0 * kSamples);
        const float histogram16 = float(histogram16L1) / (2.0 * kSamples);
        const float histogramN16 = float(histogramN16L1) / (2.0 * kSamples);
        const float centredSad = float(centredSadSum) / (16.0 * 255.0 * kSamples);
        const float contrast = max(0.02, 0.5 * (stats[2] + stats[3]) / 255.0);
        const float centredSadN = centredSad / contrast;
        const bool haveWarp = warpOn && warpValid != 0 && warpCount * 10 >= kSamples * 6;
        const float boxSad = float(boxSadSum) / (16.0 * 255.0 * kSamples);
        const float warpSad = haveWarp ? float(warpSadSum) / (16.0 * 255.0 * float(warpCount)) : 0.0;
        const float warpRatioNow = haveWarp ? warpSad / max(boxSad, 1e-4) : 1.0;

        const bool flash = sad > flashSad && histogram < flashHistogram;
        const bool fixedRule = (sad > cutSad && histogram > cutHistogram) ||
                               (sad > replacementSad && histogram > replacementHistogram);
        const bool hold = sad < holdSad && centredSad < holdSad;
        const bool initialised = flags.Load(kStateOffset + 4) != 0;
        float baseline = initialised ? asfloat(flags.Load(kStateOffset)) : kInitialBaseline;
        float baselineN = initialised ? asfloat(flags.Load(kStateOffset + 8)) : kInitialBaselineN;
        float baselineW = initialised ? asfloat(flags.Load(kStateOffset + 12)) : kInitialBaselineW;
        if (!(baseline >= 0.0 && baseline < 1.0)) baseline = kInitialBaseline;   // NaN guards
        if (!(baselineN >= 0.0 && baselineN < 50.0)) baselineN = kInitialBaselineN;
        if (!(baselineW >= 0.0 && baselineW < 1.0)) baselineW = kInitialBaselineW;
        const float threshold = max(adaptiveMin, adaptiveRatio * baseline);
        const bool adaptiveRule = adaptiveMin > 0.0 && centredSad > threshold && histogram16 > adaptiveHistogram16;
        const bool dark = darkOn && max(stats[0], stats[1]) < darkMean * 255.0;
        const bool darkRule = dark && centredSadN > max(darkMin, darkRatio * baselineN) && histogramN16 > darkHistogram;
        const bool warpVetoed = haveWarp && warpRatioNow < warpVeto;   // the warp explains the difference: motion
        const bool warpCut = haveWarp && warpRatioNow >= warpVeto && warpSad > max(warpMin, warpRatio * baselineW) &&
                             (histogram16 > warpHistogram || histogramN16 > darkHistogram);
        // spike rule: a sudden change against the last pair that moved (cuts inside fast motion, where the
        // baselines are high; held pairs of animation on twos are skipped, so the next drawing is not a spike)
        const bool refValid = flags.Load(kSpikeOffset + 8) != 0;
        const float refMrSad = asfloat(flags.Load(kSpikeOffset)), refHistogram16 = asfloat(flags.Load(kSpikeOffset + 4));
        const bool spikeRule = (features & 8u) != 0 && refValid && centredSad > max(spikeMin, spikeRatio * refMrSad) &&
                               (histogram16 > max(spikeHistogram, spikeHistogramRatio * refHistogram16) || (dark && histogramN16 > darkHistogram));
        // block matching
        const float mcNorm = 1.0 / (float(kBlocks * kBlockW * kBlockH) * 4.0 * 255.0);
        const float mcSad = float(mcSum) * mcNorm, zeroSad = float(zeroSum) * mcNorm;
        const bool haveMc = mcOn && compare != 0 && zeroSad > 0.002;
        const float mcRatioNow = haveMc ? mcSad / zeroSad : 1.0;
        float baselineMc = asfloat(flags.Load(96));
        if (!(baselineMc > 0.0 && baselineMc < 1.0)) baselineMc = kInitialBaseline;
        const bool mcVetoed = haveMc && !fixedRule && mcRatioNow < mcVeto;
        const bool mcCut = haveMc && mcRatioNow >= mcVeto && mcSad > max(mcMin, mcRatio * baselineMc) &&
                           (histogram16 > adaptiveHistogram16 || histogramN16 > darkHistogram);
        // scene measures: chroma histogram distance, orientation histogram distance (normalised), edge energy change,
        // the zoom-aware residual (best similarity fit) and the zero-motion residual, both over B's mean deviation
        float chromaHist = 0.0, edgeHist = 0.0, edgeEnergy = 0.0, zoomRes = 0.0, zoomRes0 = 0.0, devB = 0.0;
        float lmFrac = 0.0, lmQ25 = 2.0; uint lmTex = 0;
        // flow measures: residual after warping A by the flow (over the frames' mean deviation), the share of lattice
        // points in a smooth (affine) field, the share with forward-backward agreement, the mean cost and its high share
        float flowRes = -1.0, flowLap = -1.0, flowFb = -1.0, flowCostMean = -1.0, flowCostHi = -1.0;
        const bool haveFlow = compare != 0 && flowOn && flowLatN > 0;
        // Fourier-Mellin measures: the best residual over the frames' mean deviation, the two peak heights, |ln scale|
        float fmRes = -1.0, fmPeak = -1.0, fmPeakT = -1.0, fmLnS = 0.0;
        if (fmOn) {
            const float devAB = max(0.5 * float(zoomDevSum + zoomDevSumA) / float(kLowTexels), 4.0);
            fmRes = 2.0;
            for (uint v = 0; v < 8; ++v) if (fmN[v] * 20 >= 24 * 13 * 11) fmRes = min(fmRes, float(fmSum[v]) / float(fmN[v]) / devAB);
            fmPeak = asfloat(flags.Load(kFmOffset + 8)); fmPeakT = asfloat(flags.Load(kFmOffset + 20)); fmLnS = abs(asfloat(flags.Load(kFmOffset)));
        }
        if (haveFlow) {
            const float devAB = max(0.5 * float(zoomDevSum + zoomDevSumA) / float(kLowTexels), 4.0);
            flowRes = flowResN * 20 >= kLowTexels * 11 ? float(flowResSum) / float(flowResN) / devAB : 2.0;
            flowLap = float(flowLapGood) / float(flowLatN);
            if ((flowFlags & 4u) != 0) flowFb = float(flowFbGood) / float(flowLatN);
            if ((flowFlags & 2u) != 0) { flowCostMean = float(flowCostSum) / (255.0 * float(flowLatN)); flowCostHi = float(flowCostHigh) / float(flowLatN); }
        }
        if (scoreOn && compare != 0) {
            // textured blocks: mean deviation >= 3 code values (x4 texel sums, x48 texels); r = best residual / deviation
            uint hist[16];
            for (uint k = 0; k < 16; ++k) hist[k] = 0;
            uint good = 0;
            for (uint k = 0; k < kBlocks; ++k) {
                if (lmDev[k] < 288u || lmMin[k] == 0xFFFFFFFFu) continue;
                const float r = float(lmMin[k]) / float(lmDev[k]);
                ++lmTex; good += r < 0.5 ? 1u : 0u;
                ++hist[min(uint(r * 8.0), 15u)];
            }
            if (lmTex >= 8) {
                lmFrac = float(good) / float(lmTex);
                uint acc = 0, k = 0;
                for (; k < 16; ++k) { acc += hist[k]; if (acc * 4 >= lmTex) break; }
                lmQ25 = (float(k) + 0.5) / 8.0;
            } else { lmFrac = 1.0; lmQ25 = 0.0; }   // too flat to say
        }
        if (scoreOn && compare != 0) {
            chromaHist = float(chromaL1) / (2.0 * kSamples);
            float ta = 0.0, tb = 0.0;
            for (uint k = 0; k < 32; ++k) { ta += float(edgeHA[k]); tb += float(edgeHB[k]); }
            float l1 = 0.0;
            for (uint k = 0; k < 32; ++k) l1 += abs(float(edgeHA[k]) / max(ta, 1.0) - float(edgeHB[k]) / max(tb, 1.0));
            edgeHist = ta > 64.0 && tb > 64.0 ? 0.5 * l1 : (ta > 64.0 || tb > 64.0 ? 1.0 : 0.0);
            edgeEnergy = abs(log2((tb + 64.0) / (ta + 64.0)));
            devB = max(float(zoomDevSum) / float(kLowTexels), 4.0);
            const float devAB = max(0.5 * float(zoomDevSum + zoomDevSumA) / float(kLowTexels), 4.0);
            zoomRes0 = float(zoomZeroSum) / float(kLowTexels) / devAB;
            zoomRes = zoomN * 20 >= kLowTexels * 11 ? min(zoomRes0, float(zoomSum) / float(zoomN) / devAB) : zoomRes0;
        }
        // bar geometry of B (this frame) and A (the previous real frame, stored by the last pair)
        // bar: at most barTaps of 32 taps above barLuma; picture: at least a quarter of them, the brightest at barPicture
        const uint picture = uint(barPicture * 255.0);
        uint4 gB = uint4(0, 0, 0, 0); bool darkB = true, validA = false, darkA = true; uint4 gA = uint4(0, 0, 0, 0);
        bool barCut = false;
        if (findBars) {
            while (gB.x < kProfX / 2 && (colMaxB[gB.x] >> 16) <= barTaps) ++gB.x;
            while (gB.y < kProfX / 2 && (colMaxB[kProfX - 1 - gB.y] >> 16) <= barTaps) ++gB.y;
            while (gB.z < kProfY / 2 && (rowMaxB[gB.z] >> 16) <= barTaps) ++gB.z;
            while (gB.w < kProfY / 2 && (rowMaxB[kProfY - 1 - gB.w] >> 16) <= barTaps) ++gB.w;
            darkB = gB.x >= kProfX / 2 || gB.z >= kProfY / 2;
            // a bar needs a partner of about the same width (letterbox, pillarbox, a centred windowed insert)
            if (max(gB.x, gB.y) - min(gB.x, gB.y) > 2) { gB.x = 0; gB.y = 0; }
            if (max(gB.z, gB.w) - min(gB.z, gB.w) > 2) { gB.z = 0; gB.w = 0; }
            validA = flags.Load(104) != 0;
            const uint pA = flags.Load(100);
            darkA = (pA & 0x80808080u) != 0; gA = unpackGeom(pA & 0x7F7F7F7Fu);
            const bool establishedA = flags.Load(kBarsOffset + 4) != 0 && nearGeom(gA, unpackGeom(flags.Load(kBarsOffset)));
            if (barCutOn && compare != 0 && validA && !darkA && !darkB && establishedA) {
                // the bar side of a real letterbox is black: most band bins stay under half of barLuma
                const uint black0 = uint(barLuma * 127.5);
                // a band that is bar in one frame and clearly picture in the other, on both opposite edges
                const uint step = uint(barChangeBins);
                bool rows = abs(int(gA.z) - int(gB.z)) >= int(step) && abs(int(gA.w) - int(gB.w)) >= int(step);
                bool cols = abs(int(gA.x) - int(gB.x)) >= int(step) && abs(int(gA.y) - int(gB.y)) >= int(step);
                if (rows) {
                    uint n = 0, bright = 0, black = 0;
                    for (uint k = min(gA.z, gB.z); k < max(gA.z, gB.z); ++k) { const uint v = gA.z > gB.z ? rowMaxB[k] : flags.Load(kRowMaxOffset + k * 4); const uint u = gA.z > gB.z ? flags.Load(kRowMaxOffset + k * 4) : rowMaxB[k]; ++n; bright += ((v >> 16) >= kProfTaps / 4 && (v & 0xFFFFu) >= picture) ? 1u : 0u; black += (u & 0xFFFFu) <= black0 ? 1u : 0u; }
                    for (uint k = min(gA.w, gB.w); k < max(gA.w, gB.w); ++k) { const uint r2 = kProfY - 1 - k; const uint v = gA.w > gB.w ? rowMaxB[r2] : flags.Load(kRowMaxOffset + r2 * 4); const uint u = gA.w > gB.w ? flags.Load(kRowMaxOffset + r2 * 4) : rowMaxB[r2]; ++n; bright += ((v >> 16) >= kProfTaps / 4 && (v & 0xFFFFu) >= picture) ? 1u : 0u; black += (u & 0xFFFFu) <= black0 ? 1u : 0u; }
                    rows = bright * 2 >= n && black * 4 >= n * 3;
                }
                if (cols) {
                    uint n = 0, bright = 0, black = 0;
                    for (uint k = min(gA.x, gB.x); k < max(gA.x, gB.x); ++k) { const uint v = gA.x > gB.x ? colMaxB[k] : flags.Load(kColMaxOffset + k * 4); const uint u = gA.x > gB.x ? flags.Load(kColMaxOffset + k * 4) : colMaxB[k]; ++n; bright += ((v >> 16) >= kProfTaps / 4 && (v & 0xFFFFu) >= picture) ? 1u : 0u; black += (u & 0xFFFFu) <= black0 ? 1u : 0u; }
                    for (uint k = min(gA.y, gB.y); k < max(gA.y, gB.y); ++k) { const uint c2 = kProfX - 1 - k; const uint v = gA.y > gB.y ? colMaxB[c2] : flags.Load(kColMaxOffset + c2 * 4); const uint u = gA.y > gB.y ? flags.Load(kColMaxOffset + c2 * 4) : colMaxB[c2]; ++n; bright += ((v >> 16) >= kProfTaps / 4 && (v & 0xFFFFu) >= picture) ? 1u : 0u; black += (u & 0xFFFFu) <= black0 ? 1u : 0u; }
                    cols = bright * 2 >= n && black * 4 >= n * 3;
                }
                barCut = rows || cols;
            }
        }
        // scene score (bit 6): the pair's measures and their rise over the last kScoreHist moving pairs, combined
        float sceneScore = -100.0;
        bool scoreCut = false;
        if (scoreOn && compare != 0 && !hold) {
            const uint n = min(flags.Load(kScoreOffset + 96), kScoreHist);
            float4 mx = float4(0.3, 0.05, 0.05, 0.05);
            if (n > 0) {
                mx = float4(0.0, 0.0, 0.0, 0.0);
                for (uint k = 0; k < n; ++k) mx = max(mx, asfloat(flags.Load4(kScoreOffset + k * 16)));
            }
            const float baseNow = baseline + (centredSad > baseline ? baselineAttack : baselineRelease) * (centredSad - baseline);
            const float x[11] = { zoomRes, chromaHist, histogram16, edgeHist, histogramN16, zoomRes - mx.x, chromaHist - mx.y,
                                  histogram16 - mx.z, edgeHist - mx.w, log(max(centredSad, 0.002) / max(baseNow, 0.01)), lmQ25 };
            if (haveFlow && (flowFlags & 6u) == 6u) {
                const float xf[5] = { flowRes, flowLap, flowFb, flowCostMean, flowCostHi };
                sceneScore = kScoreWF[16];
                for (uint k = 0; k < 11; ++k) sceneScore += kScoreWF[k] * x[k];
                for (uint k = 0; k < 5; ++k) sceneScore += kScoreWF[11 + k] * xf[k];
                scoreCut = sceneScore > scoreThresholdFlow;
            } else if (fmOn && fmRes >= 0.0) {
                const float xm[4] = { fmRes, fmPeak, fmPeakT, fmLnS };
                sceneScore = kScoreWM[15];
                for (uint k = 0; k < 11; ++k) sceneScore += kScoreWM[k] * x[k];
                for (uint k = 0; k < 4; ++k) sceneScore += kScoreWM[11 + k] * xm[k];
                scoreCut = sceneScore > kScoreThresholdFm;
            } else {
                sceneScore = kScoreW[11];
                for (uint k = 0; k < 11; ++k) sceneScore += kScoreW[k] * x[k];
                scoreCut = sceneScore > scoreThreshold;
            }
        }
        if (scoreOn && compare != 0 && !hold) {
            const uint head = flags.Load(kScoreOffset + 100) % kScoreHist;
            flags.Store4(kScoreOffset + head * 16, asuint(float4(zoomRes, chromaHist, histogram16, edgeHist)));
            flags.Store2(kScoreOffset + 96, uint2(min(flags.Load(kScoreOffset + 96) + 1, kScoreHist), (head + 1) % kScoreHist));
        } else if (compare == 0) flags.Store2(kScoreOffset + 96, uint2(0u, 0u));
        const bool flowVeto = haveFlow && (flowFlags & 4u) != 0 && flowFb > kFlowVetoFb;
        const bool cut = compare != 0 && (barCut || (!flash && !hold &&
                         (fixedRule || ((((adaptiveRule || darkRule || spikeRule) && !warpVetoed && !mcVetoed) || warpCut || mcCut || scoreCut) && !flowVeto))));
        if (compare != 0 && !hold) flags.Store3(kSpikeOffset, uint3(asuint(centredSad), asuint(histogram16), 1u));
        else if (compare == 0) flags.Store(kSpikeOffset + 8, 0u);
        flags.Store2(parity * 8, uint2(cut ? 1u : 0u, 0u));
        const bool soft = compare != 0 && !cut && !hold && !flash && !flowVeto && haveFlow && (flowFlags & 6u) == 6u && sceneScore > kSoftThresholdFlow;
        flags.Store4(kSoftOffset + parity * 16, uint4((cut || soft) ? 1u : 0u, 0u, soft ? 1u : 0u, 0u));
        // Diagnostics for tests: last scores per parity and a running cut count.
        flags.Store3(16 + parity * 16, uint3(asuint(sad), asuint(histogram), compare));
        uint previousCount;
        if (cut) flags.InterlockedAdd(48, 1, previousCount);
        flags.InterlockedAdd(52, 1, previousCount);
        if (compare != 0 && !cut && !hold) {
            const float w = centredSad > baseline ? baselineAttack : baselineRelease;
            baseline += w * (centredSad - baseline);
            const float wn = centredSadN > baselineN ? baselineAttack : baselineRelease;
            baselineN += wn * (centredSadN - baselineN);
            if (haveWarp) {
                const float ww = warpSad > baselineW ? baselineAttack : baselineRelease;
                baselineW += ww * (warpSad - baselineW);
            }
            flags.Store4(kStateOffset, uint4(asuint(baseline), 1u, asuint(baselineN), asuint(baselineW)));
            if (haveMc) {
                const float wm = mcSad > baselineMc ? baselineAttack : baselineRelease;
                flags.Store(96, asuint(baselineMc + wm * (mcSad - baselineMc)));
            }
        }
        // the bar geometry the measures use, with hysteresis: a new geometry is taken when it has lasted barPersist
        // frames (a dark frame proves nothing and is skipped), at once after a bar-change cut; then this frame's
        // maxima and geometry become A's for the next pair
        if (findBars) {
            if (!darkB) {
                const bool have = flags.Load(kBarsOffset + 4) != 0;
                const uint4 g = unpackGeom(flags.Load(kBarsOffset)), cand = unpackGeom(flags.Load(kBarsOffset + 8));
                uint n = flags.Load(kBarsOffset + 12);
                if (!have || barCut) { flags.Store2(kBarsOffset, uint2(packGeom(gB), 1u)); n = 0; }
                else if (nearGeom(gB, g)) n = 0;
                else if (nearGeom(gB, cand)) { if (++n >= barPersist) { flags.Store(kBarsOffset, packGeom(gB)); n = 0; } }
                else { flags.Store(kBarsOffset + 8, packGeom(gB)); n = 1; }
                flags.Store(kBarsOffset + 12, n);
            }
            for (uint k = 0; k < kProfX; ++k) flags.Store(kColMaxOffset + k * 4, colMaxB[k]);
            for (uint k = 0; k < kProfY; ++k) flags.Store(kRowMaxOffset + k * 4, rowMaxB[k]);
            flags.Store2(100, uint2(packGeom(gB) | (darkB ? 0x80808080u : 0u), 1u));
        }
        // trace record (PushMpcCutGuard reads it back when PUSHMPC_CUT_TRACE is set)
        const uint rules = (fixedRule ? 1u : 0u) | (adaptiveRule ? 2u : 0u) | (darkRule ? 4u : 0u) | (warpCut ? 8u : 0u) |
                           (warpVetoed ? 16u : 0u) | (flash ? 32u : 0u) | (hold ? 64u : 0u) | (dark ? 128u : 0u) | (haveWarp ? 256u : 0u) | (spikeRule ? 512u : 0u) | (barCut ? 1024u : 0u) | (mcCut ? 2048u : 0u) | (mcVetoed ? 4096u : 0u) | (haveMc ? 8192u : 0u) | (scoreCut ? 16384u : 0u) | (flowVeto ? 32768u : 0u) | (soft ? 65536u : 0u);
        uint pairs = flags.Load(52);
        flags.Store4(kTraceOffset, uint4(pairs, compare, asuint(sad), asuint(histogram)));
        flags.Store4(kTraceOffset + 16, uint4(asuint(histogram16), asuint(histogramN16), asuint(centredSad), asuint(centredSadN)));
        flags.Store4(kTraceOffset + 32, uint4(asuint(warpSad), asuint(baseline), asuint(baselineN), asuint(baselineW)));
        flags.Store4(kTraceOffset + 48, uint4(asuint(stats[0] / 255.0), asuint(stats[1] / 255.0), asuint(stats[2] / 255.0), asuint(stats[3] / 255.0)));
        flags.Store4(kTraceOffset + 64, uint4(asuint(haveWarp ? warp[0] : 1.0), asuint(haveWarp ? warp[1] : 0.0), asuint(haveWarp ? warp[2] : 1.0), asuint(haveWarp ? warp[3] : 0.0)));
        flags.Store4(kTraceOffset + 80, uint4(rules, cut ? 1u : 0u, rect[0] | (rect[1] << 16), rect[2] | (rect[3] << 16)));
        flags.Store4(kTraceOffset + 96, uint4(asuint(boxSad), asuint(warpRatioNow), asuint(threshold), packGeom(gA)));
        flags.Store4(kTraceOffset + 112, uint4(asuint(mcSad), asuint(mcRatioNow), asuint(baselineMc), packGeom(gB) | (darkB ? 0x80808080u : 0u)));
        flags.Store4(kTraceOffset + 128, uint4(asuint(zoomRes), asuint(zoomRes0), asuint(zS), asuint(zTx)));
        flags.Store4(kTraceOffset + 144, uint4(asuint(zTy), asuint(chromaHist), asuint(edgeHist), asuint(edgeEnergy)));
        flags.Store4(kTraceOffset + 160, uint4(asuint(devB / 1020.0), asuint(float(zoomDevSumA) / float(kLowTexels) / 1020.0), asuint(lmFrac), asuint(lmQ25)));
        flags.Store4(kTraceOffset + 176, uint4(lmTex, asuint(sceneScore), asuint(flowRes), asuint(flowLap)));
        flags.Store4(kTraceOffset + 192, uint4(asuint(flowFb), asuint(flowCostMean), asuint(flowCostHi), haveFlow ? flowFlags : 0u));
        flags.Store4(kTraceOffset + 208, uint4(asuint(fmRes), asuint(fmPeak), asuint(fmPeakT), asuint(fmLnS)));
    }
}
