// SPDX-License-Identifier: GPL-3.0-or-later
// pushmpc.Enhance, portable GPU engine: Subtitle Hold on planar YUV. PushFPS portable 0.3.0's Subtitle Hold
// (CrosshairHold.h with HoldKind::Subtitles; the rule is PushFPS's include/veyra/pipeline/PushOverlay.h, used as it
// is): in the band where subtitles sit (the middle 62.5 % of the width, the bottom 400/2160 of the height, from 4 %
// above the bottom edge), thin strokes that stand still while the picture behind them moves are found, filled in
// from their surroundings in a copy of the band that frame generation takes, and laid back over each generated frame
// with what the background did behind them. Burned-in subtitles (and any static caption there) then stay sharp and in
// place instead of being dragged along with the picture.
//
// PushFPS runs the rule on BGRA; here the rule's luma is the video's Y (normalised as limited range), and the fill
// and the "shows through" measure take Y, U and V as their three channels (chroma repeated over its 2x2 luma pixels).
// The result for a pair is the buffer interp.comp reads (code values; layout in its Hold binding).
#pragma once
#include <memory>
#include <vector>
#include "portable_engine.h"
#include "veyra/pipeline/PushOverlay.h"

namespace pmvs::gpu {

struct HoldFrameYuv {
    int x = 0, y = 0, w = 0, h = 0;                        // the band in luma pixels (even, whole 16 px blocks)
    veyra::pipeline::overlay::Plane mask;                  // 1 where frame generation is not to see the picture
    veyra::pipeline::overlay::Picture zone, filled;        // (Y, U, V) normalised; filled empty when nothing is masked
    int masked = 0;
};
struct HoldPairYuv {
    int x = 0, y = 0, w = 0, h = 0;
    std::vector<float> data;   // interp.comp's Hold buffer
    int held = 0;              // luma pixels held
};

class SubtitleHoldYuv {
public:
    void Reset();
    // the next frame frame generation sees (a distinct frame), in order; null when the picture is too small
    std::shared_ptr<const HoldFrameYuv> Frame(const pmvs::portable::YuvView& v);
    // null when neither frame holds a pixel (then nothing is done to the generated frames)
    static std::shared_ptr<const HoldPairYuv> Pair(const HoldFrameYuv& a, const HoldFrameYuv& b, int bits);

private:
    veyra::pipeline::overlay::State m_state{0};
    int m_width = 0, m_height = 0;
};

} // namespace pmvs::gpu
