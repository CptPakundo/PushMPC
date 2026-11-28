// Neural Rendering's no-reset live path, without a GPU (CI: intel-video.yml, core job, against the patched PushFPS tree).
//
//   1. the field classification (PushMpcNrLive.h, patches/pushfps-4.5.8-pushmpc.patch): live / live-reset-at-cut / rebuild;
//   2. the decision EnhanceGraph::applySettings takes from it: a change confined to NR's live dials -> no inputRevision
//      bump (the NGX evaluate then passes Reset 0, the AMD runtimes do not reset) and no anti-flicker reset; anything
//      else -> the bump and the reset, as PushFPS does;
//   3. pushcore's ABI mapping (PushCoreNrDials.h): nrDials 0 (a zeroed or a smaller struct) gives exactly the evaluate
//      values NR got before the new fields existed; set dials arrive; out-of-range ones are ignored.
// NR itself cannot run on WARP: the NGX-level proof is a GPU run (the "nr live" lines in pushcore.log).
//
// build: cl /std:c++20 /EHsc /utf-8 /DNOMINMAX /I <PushFPS>\include /I pushmpc\include /I pushmpc\src pushmpc\tools\nr_live_test.cpp
#include "veyra/pipeline/PushMpcNrLive.h"
#include "PushCoreNrDials.h"
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

using veyra::engine::EnhancementSettings;
using veyra::pipeline::NrField;
using veyra::pipeline::NrFieldClass;

static int failures = 0, checks = 0;
#define CHECK(cond, what) do { ++checks; if (!(cond)) { ++failures; std::printf("FAIL %s (line %d)\n", what, __LINE__); } } while (0)

static_assert(offsetof(PushCoreSettings, nrDials) == offsetof(PushCoreSettings, holdFlags) + sizeof(uint32_t), "the NR dials are appended");
static_assert(offsetof(PushCoreSettings, nrStyle) + sizeof(int32_t) == sizeof(PushCoreSettings), "the NR dials end the struct");

// PushMPC's NR settings as pushcore builds them (buildFromPushMpc: EnhancementSettings{}, runtime Ampere, NR on)
static EnhancementSettings base() {
    EnhancementSettings s{};
    s.nrRuntime = veyra::engine::NrRuntime::Ampere;
    s.nr = true;
    s.model.tone = 0.7f; s.model.structure = 0.8f;
    return s;
}

// EnhanceGraph::applySettings' bookkeeping for one layer (the patch: `if(nrLive.live)continue;` before the reset and the bump)
struct Layer { uint64_t inputRevision = 1; bool temporalReset = false; };
static void apply(Layer& l, const veyra::pipeline::NrLiveDecision& d) {
    if (d.live) return;
    l.temporalReset = true;
    ++l.inputRevision;
}
// the evaluate: NGX Reset = reset || inputRevision != 1; AMD the same with !historyValid
static bool ngxReset(const Layer& l, bool frameReset) { return frameReset || l.inputRevision != 1; }

static bool contains(const std::vector<std::string>& lines, const char* text) {
    for (const auto& l : lines) if (l.find(text) != std::string::npos) return true;
    return false;
}

template <class F>
static void expectLive(const char* name, F change, const char* line) {
    const auto a = base(); auto b = a; b.revision = a.revision + 1; change(b);
    CHECK(b.validate().empty(), name);
    const auto d = veyra::pipeline::nrLiveDecision(a, b);
    Layer l; apply(l, d);
    const bool ok = d.live && !l.temporalReset && !ngxReset(l, false) && contains(d.changes, line) && contains(d.changes, "(no reset");
    std::printf("%-44s live=%d bump=%d ngxReset=%d  %s\n", name, d.live, int(l.inputRevision - 1), ngxReset(l, false),
                d.changes.empty() ? "(no line)" : d.changes.front().c_str());
    CHECK(ok, name);
}
template <class F>
static void expectReset(const char* name, F change) {
    const auto a = base(); auto b = a; b.revision = a.revision + 1; change(b);
    const auto d = veyra::pipeline::nrLiveDecision(a, b);
    Layer l; apply(l, d);
    std::printf("%-44s live=%d bump=%d ngxReset=%d\n", name, d.live, int(l.inputRevision - 1), ngxReset(l, false));
    CHECK(!d.live && l.temporalReset && ngxReset(l, false), name);
}

int main() {
    std::printf("== 1. classification\n");
    struct Row { NrField f; NrFieldClass c; };
    const Row rows[] = {
        {NrField::Intensity, NrFieldClass::Live}, {NrField::LocalTone, NrFieldClass::Live}, {NrField::LocalStructure, NrFieldClass::Live},
        {NrField::Skin, NrFieldClass::Live}, {NrField::Style, NrFieldClass::LiveResetAtCut}, {NrField::UseAutoMask, NrFieldClass::LiveResetAtCut},
        {NrField::UICorrection, NrFieldClass::LiveResetAtCut}, {NrField::ResidualGains, NrFieldClass::Live},
        {NrField::CorrectionValues, NrFieldClass::Live}, {NrField::CorrectionEnabled, NrFieldClass::Live}, {NrField::Protection, NrFieldClass::Live},
        {NrField::NetworkPreset, NrFieldClass::Rebuild}, {NrField::Runtime, NrFieldClass::Rebuild}, {NrField::Motion, NrFieldClass::Rebuild},
        {NrField::LayerSet, NrFieldClass::Rebuild}, {NrField::SizePolicy, NrFieldClass::Rebuild}, {NrField::AntiFlicker, NrFieldClass::Rebuild},
        {NrField::NrOnOff, NrFieldClass::Rebuild}};
    static_assert(sizeof(rows) / sizeof(rows[0]) == size_t(NrField::Count), "every field has a row");
    for (const auto& r : rows) {
        const auto c = veyra::pipeline::nrFieldClass(r.f);
        std::printf("  %-18s %s\n", std::string(veyra::pipeline::nrFieldName(r.f)).c_str(),
                    c == NrFieldClass::Live ? "live" : c == NrFieldClass::LiveResetAtCut ? "live, reset at next cut" : "rebuild");
        CHECK(c == r.c, std::string(veyra::pipeline::nrFieldName(r.f)).c_str());
    }

    std::printf("== 2. applySettings decision (live: no inputRevision bump, no anti-flicker reset)\n");
    expectLive("intensity 1 -> 0.6", [](EnhancementSettings& s) { s.model.intensity = 0.6f; }, "layer 1 intensity 1.00->0.60 (no reset)");
    expectLive("tone 0.7 -> 0.4", [](EnhancementSettings& s) { s.model.tone = 0.4f; }, "layer 1 tone 0.70->0.40");
    expectLive("structure 0.8 -> 0.5", [](EnhancementSettings& s) { s.model.structure = 0.5f; }, "layer 1 structure 0.80->0.50");
    expectLive("skin -1 -> 0.3", [](EnhancementSettings& s) { s.model.skin = 0.3f; }, "layer 1 skin -1.00->0.30");
    expectLive("style 0 -> 1", [](EnhancementSettings& s) { s.model.style = 1; }, "layer 1 style 0->1 (no reset; history renews");
    expectLive("autoMask 0 -> 1", [](EnhancementSettings& s) { s.model.autoMask = 1; }, "layer 1 autoMask 0->1");
    expectLive("uiCorrection 0 -> 1", [](EnhancementSettings& s) { s.model.uiCorrection = 1; }, "layer 1 uiCorrection 0->1");
    expectLive("residual total 1 -> 0.8", [](EnhancementSettings& s) { s.residual.total = 0.8f; }, "layer 1 residual total 1.00->0.80");
    expectLive("residual darken 1 -> 0.5", [](EnhancementSettings& s) { s.residual.darken = 0.5f; }, "residual darken");
    expectLive("protection region on", [](EnhancementSettings& s) { s.protection.enabled = true; s.protection.regions[0] = {0.1f, 0.8f, 0.9f, 0.95f}; }, "protection off -> on");
    expectLive("intensity + tone + style together", [](EnhancementSettings& s) { s.model.intensity = 0.5f; s.model.tone = 0.3f; s.model.style = 2; }, "style 0->2");
    {   // revision only: nothing NR sees changed -> no reset, no line
        const auto a = base(); auto b = a; b.revision = 7;
        const auto d = veyra::pipeline::nrLiveDecision(a, b);
        std::printf("%-44s live=%d lines=%zu\n", "revision only", d.live, d.changes.size());
        CHECK(d.live && d.changes.empty(), "revision only");
    }
    {   // a non-video field (audio offset) beside a dial: still live, as sameActiveVideoParameters ignores it
        const auto a = base(); auto b = a; b.audioOffsetMs = 40; b.model.intensity = 0.9f;
        CHECK(veyra::pipeline::nrLiveDecision(a, b).live, "audio offset + intensity");
    }
    {   // the correction block, on in both: its values are shader constants
        auto a = base(); a.residual.correction.enabled = true; a.residual.correction.automatic = false; a.nrTemporal = true;
        auto b = a; b.residual.correction.hue = 0.5f;
        const auto d = veyra::pipeline::nrLiveDecision(a, b);
        CHECK(d.live && contains(d.changes, "correction values"), "correction values");
    }
    {   // two layers: a dial of layer 2
        auto a = base(); const auto flat = a.nrLayer(0); a.nrLayerCount = 2;
        for (int i = 0; i < 2; ++i) { a.nrLayers[i] = flat; a.nrLayers[i].enabled = true; }
        auto b = a; b.nrLayers[1].model.intensity = 0.4f;
        CHECK(a.validate().empty() && b.validate().empty(), "two layers valid");
        const auto d = veyra::pipeline::nrLiveDecision(a, b);
        std::printf("%-44s live=%d  %s\n", "two layers, layer 2 intensity", d.live, d.changes.empty() ? "(no line)" : d.changes.front().c_str());
        CHECK(d.live && contains(d.changes, "layer 2 intensity 1.00->0.40"), "layer 2 intensity");
        auto c = a; c.nrLayers[1].model.performance = !a.nrLayers[1].model.performance;
        CHECK(!veyra::pipeline::nrLiveDecision(a, c).live, "layer 2 network preset");
        auto e = a; e.nrLayers[1].enabled = false;
        CHECK(!veyra::pipeline::nrLiveDecision(a, e).live, "layer 2 off");
    }
    std::printf("== 2b. not live (bump + reset, as PushFPS)\n");
    expectReset("network preset (model.performance)", [](EnhancementSettings& s) { s.model.performance = !s.model.performance; });
    expectReset("runtime Ampere -> NVIDIA original", [](EnhancementSettings& s) { s.nrRuntime = veyra::engine::NrRuntime::NvidiaOriginal; });
    expectReset("anti-flicker on", [](EnhancementSettings& s) { s.nrTemporal = true; });
    expectReset("NR size policy", [](EnhancementSettings& s) { s.nrPolicy = veyra::pipeline::NrSizePolicy::Auto; });
    expectReset("NR off", [](EnhancementSettings& s) { s.nr = false; });
    expectReset("a second NR layer", [](EnhancementSettings& s) { const auto flat = s.nrLayer(0); s.nrLayerCount = 2; s.nrLayers[0] = flat; s.nrLayers[0].enabled = true; s.nrLayers[1] = s.nrLayers[0]; });
    expectReset("RTX VSR on (outside NR)", [](EnhancementSettings& s) { s.sr = true; });
    expectReset("colour grade on (outside NR)", [](EnhancementSettings& s) { s.color.enabled = true; });
    expectReset("intensity + VSR together", [](EnhancementSettings& s) { s.model.intensity = 0.6f; s.sr = true; });
    {   // NR off before: switching it on resets (stale history)
        auto a = base(); a.nr = false; auto b = base();
        CHECK(!veyra::pipeline::nrLiveDecision(a, b).live, "NR on");
    }
    {   // a frame reset (seek, flush, stream change, a cut the graph detects) still resets after a live change
        const auto a = base(); auto b = a; b.model.intensity = 0.6f;
        Layer l; apply(l, veyra::pipeline::nrLiveDecision(a, b));
        CHECK(!ngxReset(l, false) && ngxReset(l, true), "frame reset after a live change");
    }

    std::printf("== 3. pushcore ABI -> NrSettings (PUSHCORE_FEATURE_NR_DIALS)\n");
    const veyra::engine::NrSettings defaults{};
    auto mapped = [](const PushCoreSettings& in) { EnhancementSettings s{}; s.nrRuntime = veyra::engine::NrRuntime::Ampere; s.nr = true; pushmpc::applyNrDials(in, s.model); return s; };
    {   // a zeroed full-size struct with today's tone / structure: everything else as before
        PushCoreSettings in{}; in.structSize = sizeof(in); in.nrTone = 0.7f; in.nrStructure = 0.8f;
        const auto s = mapped(in);
        std::printf("  zeroed struct: intensity %.2f tone %.2f structure %.2f skin %.2f style %d autoMask %d uiCorrection %d performance %d\n",
                    s.model.intensity, s.model.tone, s.model.structure, s.model.skin, s.model.style, s.model.autoMask, s.model.uiCorrection, int(s.model.performance));
        auto want = defaults; want.tone = 0.7f; want.structure = 0.8f;
        CHECK(s.model == want && s.validate().empty(), "zeroed struct = today's values");
        CHECK(s.model.intensity == 1.0f && s.model.skin == -1.0f && s.model.style == 0 && s.model.autoMask == 0 && s.model.uiCorrection == 0, "today's evaluate values");
    }
    {   // a struct as an older caller builds it: only up to holdFlags copied (pushcore's copySettings), the rest zero
        PushCoreSettings full{}; std::memset(&full, 0xA5, sizeof(full)); full.nrTone = 1; full.nrStructure = 1;
        PushCoreSettings in{}; std::memcpy(&in, &full, offsetof(PushCoreSettings, nrDials)); in.structSize = offsetof(PushCoreSettings, nrDials);
        auto want = defaults;
        CHECK(mapped(in).model == want, "older-size struct = today's values");
    }
    {   // values without their bits are not read
        PushCoreSettings in{}; in.nrTone = 1; in.nrStructure = 1; in.nrIntensity = 0.2f; in.nrSkin = 0.5f; in.nrAutoMask = 1; in.nrStyle = 2;
        CHECK(mapped(in).model == defaults, "dials without nrDials bits ignored");
    }
    {   // set dials arrive
        PushCoreSettings in{}; in.nrTone = 0.5f; in.nrStructure = 0.6f;
        in.nrDials = PUSHCORE_NR_DIAL_INTENSITY | PUSHCORE_NR_DIAL_SKIN | PUSHCORE_NR_DIAL_AUTO_MASK | PUSHCORE_NR_DIAL_STYLE;
        in.nrIntensity = 0.6f; in.nrSkin = 0.3f; in.nrAutoMask = 1; in.nrStyle = 1;
        const auto s = mapped(in);
        CHECK(s.model.intensity == 0.6f && s.model.skin == 0.3f && s.model.autoMask == 1 && s.model.style == 1 &&
              s.model.tone == 0.5f && s.model.structure == 0.6f && s.model.performance == defaults.performance && s.validate().empty(), "set dials arrive");
        in.nrSkin = -1; CHECK(mapped(in).model.skin == -1.0f, "skin -1 (follows structure)");
    }
    {   // out of range: ignored, the open still validates
        PushCoreSettings in{}; in.nrTone = 2; in.nrStructure = -1;
        in.nrDials = PUSHCORE_NR_DIAL_INTENSITY | PUSHCORE_NR_DIAL_SKIN | PUSHCORE_NR_DIAL_AUTO_MASK | PUSHCORE_NR_DIAL_STYLE;
        in.nrIntensity = std::numeric_limits<float>::quiet_NaN(); in.nrSkin = -0.5f; in.nrAutoMask = 2; in.nrStyle = 3;
        const auto s = mapped(in);
        CHECK(s.model == defaults && s.validate().empty(), "out-of-range dials ignored");
    }
    {   // the ABI round trip into the live decision: intensity through nrDials keeps the history
        PushCoreSettings in{}; in.nrTone = 0.7f; in.nrStructure = 0.8f;
        const auto a = mapped(in);
        in.nrDials = PUSHCORE_NR_DIAL_INTENSITY; in.nrIntensity = 0.6f;
        const auto b = mapped(in);
        const auto d = veyra::pipeline::nrLiveDecision(a, b);
        CHECK(d.live && contains(d.changes, "nr live: layer 1 intensity 1.00->0.60 (no reset)"), "ABI intensity -> live, no reset");
    }

    std::printf("nr live: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
