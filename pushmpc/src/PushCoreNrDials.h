// PushCoreSettings' Neural Rendering dials -> PushFPS's NrSettings (the evaluate parameters NGX Feature 18 / the AMD NR
// runtimes get). Header-only so pushmpc/tools/nr_live_test.cpp checks the same mapping without a GPU.
//
// The promise of PUSHCORE_FEATURE_NR_DIALS: nrDials 0 (a smaller or a zeroed struct) leaves every field this mapping does
// not own at EnhancementSettings{}' value (intensity 1, skin -1, style 0, autoMask 0, uiCorrection 0), which is what NR
// received before these fields existed; an out-of-range value is ignored, never handed to validate() (that would fail
// the open).
#pragma once
#include "pushmpc/pushcore_abi.h"
#include "veyra/engine/EnhancementSettings.h"
#include <cmath>

namespace pushmpc {

inline void applyNrDials(const PushCoreSettings& in, veyra::engine::NrSettings& model) {
    auto unit = [](float v, float d) { return std::isfinite(v) && v >= 0 && v <= 1 ? v : d; };
    model.tone = unit(in.nrTone, 1.0f);
    model.structure = unit(in.nrStructure, 1.0f);
    if (in.nrDials & PUSHCORE_NR_DIAL_INTENSITY) model.intensity = unit(in.nrIntensity, model.intensity);
    if (in.nrDials & PUSHCORE_NR_DIAL_SKIN) model.skin = in.nrSkin == -1.0f ? -1.0f : unit(in.nrSkin, model.skin);
    if ((in.nrDials & PUSHCORE_NR_DIAL_AUTO_MASK) && (in.nrAutoMask == 0 || in.nrAutoMask == 1)) model.autoMask = in.nrAutoMask;
    if ((in.nrDials & PUSHCORE_NR_DIAL_STYLE) && in.nrStyle >= 0 && in.nrStyle <= 2) model.style = in.nrStyle;
}

}  // namespace pushmpc
