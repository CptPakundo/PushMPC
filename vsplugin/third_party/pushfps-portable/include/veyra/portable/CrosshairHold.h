// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
#pragma once
// Crosshair Hold in the Linux and macOS engine.
//
// A crosshair stands still on the screen while the world behind it moves. Frame generation moves every
// place by the world's motion, so in a generated frame the crosshair is gone from the middle and shows
// up beside it. The rule that finds a crosshair (and any small overlay made of thin strokes: a dot, a
// ring) is the Windows build's, used as it is: include/veyra/pipeline/PushOverlay.h. This file is how
// this engine uses it:
//
//   - for every real frame, the rule looks at a square round the screen centre and says which pixels
//     are the overlay's (`mask`); those are filled in from their surroundings in a copy of the square
//     (`filled`), which is what frame generation samples;
//   - for a pair of real frames, `shows` says for each held pixel how much of the background shows
//     through it, worked out from the two frames themselves;
//   - a generated pixel in the square is then
//         real(A, B, t) + shows * (generated - filled(A, B, t))
//     (Interpolate.cpp and shaders/frame.frag, the same statement): the crosshair's own pixels, with
//     whatever the background did behind them.
//
// With no crosshair on screen no pair is made and every frame is drawn exactly as without the switch.
#include "veyra/pipeline/PushOverlay.h"
#include "veyra/portable/Image.h"

#include <memory>

namespace veyra::portable {

// One real frame's part.
struct HoldFrame {
    int x = 0, y = 0, size = 0;                    // the zone in the picture (size = w; the crosshair's is square)
    int w = 0, h = 0;
    pipeline::overlay::Plane mask;                 // 1 where frame generation is not to see the picture
    Bgra zone;                                     // the square as the frame has it
    pipeline::overlay::Picture filled;             // the square with the mask filled in; empty when nothing is masked
    int masked = 0;                                // pixels of the mask
};

// What a generated frame between two real frames needs.
struct HoldPair {
    int x = 0, y = 0, size = 0, w = 0, h = 0;   // size = w
    Bgra filledA, filledB;   // the square of each real frame as frame generation takes it
    // Per pixel of the square: how many 255ths of the background's change show through (kShowsFull = all of
    // it). A pixel that is not held has 255 and its filled pixels are its real ones, so the statement above
    // leaves it as generated; drawn larger or smaller than the picture, the three blend like any picture.
    Gray shows;
    int held = 0;            // pixels held in this pair
};
inline constexpr int kShowsFull = 255;

// The rule's step() with the same result and less work (CrosshairHold.cpp says where; the tests hold the
// two against each other).
pipeline::overlay::Plane stepFast(pipeline::overlay::State& state, const pipeline::overlay::Plane& luma, const pipeline::overlay::Rule& rule);

// Which zone the rule looks at: the crosshair's square round the centre, or the subtitle band (Subtitle Hold).
enum class HoldKind { Crosshair, Subtitles };

class CrosshairHold {
public:
    explicit CrosshairHold(HoldKind kind = HoldKind::Crosshair) : kind_(kind) {}
    void reset();
    // The next real frame, in order. Null when the picture is too small for a zone.
    std::shared_ptr<const HoldFrame> frame(const Bgra& image);
    // Null when neither frame has a held pixel (then nothing is done to the generated frames).
    static std::shared_ptr<const HoldPair> pair(const HoldFrame& a, const HoldFrame& b);

private:
    HoldKind kind_ = HoldKind::Crosshair;
    pipeline::overlay::State state_{0};
    int width_ = 0, height_ = 0;
};

}  // namespace veyra::portable
