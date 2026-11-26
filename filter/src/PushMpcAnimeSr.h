#pragma once
// PushMPC Auto: the anime upscalers (pushcore srMethod PUSHCORE_SR_METHOD_PMML; PushCoreSettings.animeSrModel) as
// candidates next to RTX VSR / FSR / NIS for animation. ArtCNN (MIT), CuNNy (LGPL-3.0), AnimeSR v2 (Apache-2.0).
//
// Quality order: the offline check (pmml/scripts/anime_sr_quality.py, CPU, 14 clips x 3 frames, crops reduced 2x / 4x
// and upscaled again, luma PSNR and line-pixel PSNR against bicubic; 2026-11-26):
//   2x, clean 1080p anime: R8F64 +7.0 dB, C4F32 +5.3, C4F16 +4.7, CuNNy 8x32 +4.6, 4x16 +3.5, fast +2.3 (line pixels alike);
//   DVD anime: R8F64 +4.4, C4F32 +3.0, CuNNy 8x32 +2.8, C4F16 +2.7, 4x16 +2.0, fast +1.6;
//   4x, DVD anime: R8F64 twice +3.3, C4F32 twice +2.2, CuNNy 8x32 twice +1.9, AnimeSR v2 -2.0 (a restoration GAN: it
//   redraws, which a fidelity score cannot credit; the user's verdict pairs decide it);
//   the _DS (denoise + sharpen) variants cost 0.1-0.6 dB on clean sources: only for low-bitrate sources.
// The user's verdict (side-by-side pairs at the local test) may reorder kAnimeSrRank and set kAnimeSrBeforeRtxVsr.
//
// Cost: a prior per input megapixel on an RTX 5090-class card (calibrated on a published RealESRGAN-compact speed figure; DirectML /
// TensorRT-RTX FP16) times the card factor, plus the host round trip of this version (readback of the source picture,
// upload of the output); the running core's measurement (PushCoreStats.animeSrMs) replaces it per model on this card.
// Portable (no Windows types): the VapourSynth ml route uses it too.
// Fit: the upscaler may take at most kAnimeSrShare of a source frame's time (frame generation keeps the rest).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <cwchar>
#include <cwctype>
#include <string>

namespace pm {

struct AnimeSrModelInfo {
    const wchar_t* id;      // setting value (as pmml's model id)
    const wchar_t* label;
    int scale;
    double msPerMpx;        // prior: ms per input megapixel, RTX 5090 class
    bool dvdOnly;           // heavy: only on DVD-class sources (<= 576 lines)
    int variantOf;          // the DS / DN variants: the base model's index, else -1
};
constexpr int kAnimeSrModels = 11;   // PUSHCORE_ANIME_SR_COUNT, same order as PUSHCORE_ANIME_SR_*
inline const AnimeSrModelInfo& AnimeSrModel(int i) {
    static const AnimeSrModelInfo m[kAnimeSrModels] = {
        {L"artcnn-c4f16", L"ArtCNN C4F16", 2, 0.5, false, -1},
        {L"artcnn-c4f16-ds", L"ArtCNN C4F16 DS", 2, 0.5, false, 0},
        {L"artcnn-c4f16-dn", L"ArtCNN C4F16 DN", 2, 0.5, false, 0},
        {L"artcnn-c4f32", L"ArtCNN C4F32", 2, 1.5, false, -1},
        {L"artcnn-c4f32-ds", L"ArtCNN C4F32 DS", 2, 1.5, false, 3},
        {L"artcnn-c4f32-dn", L"ArtCNN C4F32 DN", 2, 1.5, false, 3},
        {L"artcnn-r8f64", L"ArtCNN R8F64", 2, 29.0, true, -1},
        {L"cunny-fast", L"CuNNy fast", 2, 0.3, false, -1},
        {L"cunny-4x16", L"CuNNy 4x16", 2, 0.5, false, -1},
        {L"cunny-8x32", L"CuNNy 8x32", 2, 2.5, false, -1},
        {L"animesr-v2", L"AnimeSR v2", 4, 25.0, true, -1},
    };
    return m[std::clamp(i, 0, kAnimeSrModels - 1)];
}
inline int AnimeSrModelIndex(const std::wstring& id) {
    for (int i = 0; i < kAnimeSrModels; ++i)
        if (std::equal(id.begin(), id.end(), AnimeSrModel(i).id, AnimeSrModel(i).id + std::wcslen(AnimeSrModel(i).id),
                       [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); })) return i;
    return -1;
}
// best first (offline evidence above); the DS / DN variants are chosen from their base, never ranked themselves
constexpr int kAnimeSrRank[] = {6, 3, 0, 9, 8, 7, 10};
constexpr bool kAnimeSrBeforeRtxVsr = false;   // RTX VSR stays Auto's first on RTX cards until the verdict (V8 / V9)
constexpr double kAnimeSrShare = 0.35;         // the most of a source frame's time the upscaler may take
constexpr double kAnimeSrTransferMsPerMpx = 0.35;   // host round trip per megapixel (source in + output out), PCIe 4/5
constexpr unsigned kDvdClassLines = 576;

// the cost of a model for a source of srcW x srcH upscaled to outW x outH: learned (ms per input Mpx on this card, < 0
// unknown) or the prior times the card factor, plus the transfer
inline double AnimeSrCostMs(int model, unsigned srcW, unsigned srcH, unsigned outW, unsigned outH, double card, double learnedMsPerMpx)
{
    const double inMpx = double(srcW) * srcH / 1e6, outMpx = double(outW) * outH / 1e6;
    const double net = learnedMsPerMpx > 0 ? learnedMsPerMpx * inMpx : AnimeSrModel(model).msPerMpx * inMpx * card;
    return net + kAnimeSrTransferMsPerMpx * (inMpx + outMpx);
}

struct AnimeSrPick {
    int model = -1;          // AnimeSrModel index, -1 = none fits / not wanted
    double ms = 0;
    std::wstring note;
};

// Auto's anime upscaler for this source: the best-ranked candidate that is allowed at this source size and fits the
// share of the frame time. lowBitrate: the DS variant of an ArtCNN C model. forced >= 0: the user's model (kept even
// when over the share: the user asked for it; the note says so). learned[i]: ms per input Mpx on this card (< 0 unknown).
inline AnimeSrPick PickAnimeSr(unsigned srcW, unsigned srcH, unsigned outW, unsigned outH, double fps, bool lowBitrate, double card, const double* learned, int forced)
{
    AnimeSrPick p;
    const double frameMs = 1000.0 / std::max(1.0, fps), budget = kAnimeSrShare * frameMs;
    const bool dvd = srcH <= kDvdClassLines;
    auto cost = [&](int i) { return AnimeSrCostMs(i, srcW, srcH, outW, outH, card, learned ? learned[i] : -1.0); };
    if (forced >= 0 && forced < kAnimeSrModels) {
        p.model = forced;
        p.ms = cost(forced);
        p.note = std::format(L"{} (your choice; {:.1f} ms a frame{})", AnimeSrModel(forced).label, p.ms, p.ms > budget ? L", more than Auto would give it" : L"");
        return p;
    }
    std::wstring skipped;
    for (int i : kAnimeSrRank) {
        const auto& m = AnimeSrModel(i);
        if (m.dvdOnly && !dvd) continue;
        const double ms = cost(i);
        if (ms > budget) { skipped += std::format(L"{}{} {:.1f} ms", skipped.empty() ? L"" : L", ", m.label, ms); continue; }
        int use = i;
        if (lowBitrate && (i == 0 || i == 3)) use = i + 1;   // _DS
        p.model = use;
        p.ms = cost(use);
        p.note = std::format(L"{} ({:.1f} ms a frame of {:.1f} allowed{}{})", AnimeSrModel(use).label, p.ms, budget, use != i ? L", low bitrate: denoise + sharpen" : L"",
                             skipped.empty() ? L"" : L"; too slow here: " + skipped);
        return p;
    }
    p.note = L"no anime upscaler fits (" + skipped + L")";
    return p;
}

}  // namespace pm
