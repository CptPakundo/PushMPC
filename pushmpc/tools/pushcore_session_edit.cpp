#include <cmath>
// pushcore_session_edit: test tool for the renderer's live settings reload.
//
//   pushcore_session_edit <data dir> [--multiplier N] [--sr 0|1] [--color 0|1] [--exposure X]
//
// Loads <data dir>\chain-session.v1 (with PushFPS's own sidecar resolution, e.g. .vfg), prints the active
// configuration, optionally changes the frame generation multiplier or the SR switch, and saves it back with
// PushFPS's ChainSessionStore, i.e. the way PushFPS itself writes it. Only ever run it on a COPY of a data folder:
// it refuses the user's installed PushFPS folder.
#include "veyra/engine/EngineController.h"   // PlayerOptions
#include "veyra/engine/EffectChain.h"
#include "veyra/engine/PresetLibrary.h"      // ChainSessionStore

#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <string>

using namespace veyra::engine;

static void describe(const wchar_t* what, const ChainSession& session) {
    EnhancementSettings s;
    session.configurations[size_t(session.active)].apply(s);
    const auto o = PlayerOptions::from(s);
    std::wprintf(L"%ls: mode %d sr %d fg %d multiplier %u (settings.multiplier %u) backend %ls vfgQuality %u videoSrQuality %u\n", what,
                 int(session.active), o.sr ? 1 : 0, o.fg ? 1 : 0, o.fgMultiplier, s.multiplier,
                 std::filesystem::path(frameGenerationBackendName(s.frameGenerationBackend)).wstring().c_str(), s.vfgQuality, s.videoSrQuality);
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fwprintf(stderr, L"usage: pushcore_session_edit <data dir> [--multiplier N] [--sr 0|1]\n");
        return 2;
    }
    const std::filesystem::path dir = std::filesystem::absolute(argv[1]);
    std::wstring lower = dir.wstring();
    for (auto& c : lower) c = wchar_t(towlower(c));
    if (lower.find(L"\\appdata\\local\\programs\\pushfps") != std::wstring::npos) {
        std::fwprintf(stderr, L"refusing to edit the installed PushFPS data folder: %ls\n", dir.c_str());
        return 3;
    }
    int multiplier = -1, sr = -1, color = -1, srq = -1, backend = -1, vfgq = -1, flowb = -1, target = -1;
    double exposure = std::nan("");
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!wcscmp(argv[i], L"--multiplier")) multiplier = _wtoi(argv[i + 1]);
        else if (!wcscmp(argv[i], L"--sr")) sr = _wtoi(argv[i + 1]);
        else if (!wcscmp(argv[i], L"--color")) color = _wtoi(argv[i + 1]);
        else if (!wcscmp(argv[i], L"--exposure")) exposure = _wtof(argv[i + 1]);
        else if (!wcscmp(argv[i], L"--srq")) srq = _wtoi(argv[i + 1]);   // videoSrQuality (1..4 = RTX VSR tiers)
        else if (!wcscmp(argv[i], L"--backend")) backend = _wtoi(argv[i + 1]);   // FrameGenerationBackend value (0 DLSS, 4 VFG, 7 PushFlow)
        else if (!wcscmp(argv[i], L"--vfgq")) vfgq = _wtoi(argv[i + 1]);
        else if (!wcscmp(argv[i], L"--flow")) flowb = _wtoi(argv[i + 1]);   // OpticalFlowBackend (0 NVOF, 1 FidelityFX, 2 GPU DIS)
        else if (!wcscmp(argv[i], L"--target")) target = _wtoi(argv[i + 1]);   // pipeline::SrTarget (0 1440p, 1 4K)
    }
    EnhancementSettings base;
    auto session = ChainSession::initial(base);
    ChainSessionStore store(dir / L"chain-session.v1");
    if (!store.load(session)) {
        std::fwprintf(stderr, L"load failed: %ls\n", store.error().c_str());
        return 1;
    }
    describe(L"before", session);
    if (multiplier < 0 && sr < 0 && color < 0 && srq < 0 && backend < 0 && vfgq < 0 && flowb < 0 && target < 0 && std::isnan(exposure)) return 0;
    auto& c = session.configurations[size_t(session.active)];
    EnhancementSettings s;
    c.apply(s);
    if (multiplier >= 1) s.multiplier = uint32_t(multiplier);
    if (sr >= 0) s.sr = sr != 0;
    if (color >= 0) s.color.enabled = color != 0;
    if (!std::isnan(exposure)) s.color.exposure = float(exposure);
    if (srq >= 0) s.videoSrQuality = uint32_t(srq);
    if (backend >= 0) s.frameGenerationBackend = FrameGenerationBackend(backend);
    if (vfgq >= 0) s.vfgQuality = uint32_t(vfgq);
    if (flowb >= 0) s.opticalFlowBackend = OpticalFlowBackend(flowb);
    if (target >= 0) s.srTarget = veyra::pipeline::SrTarget(target);
    std::wprintf(L"colour enabled %d exposure %.3f\n", s.color.enabled ? 1 : 0, s.color.exposure);
    auto next = ChainConfiguration::capture(toChain(s), s, c.selectedNr, c.selectedColour);
    next.editor = c.editor;
    c = next;
    if (!store.save(session)) {
        std::fwprintf(stderr, L"save failed: %ls\n", store.error().c_str());
        return 1;
    }
    ChainSession check = ChainSession::initial(base);
    ChainSessionStore again(dir / L"chain-session.v1");
    if (!again.load(check)) {
        std::fwprintf(stderr, L"reload failed: %ls\n", again.error().c_str());
        return 1;
    }
    describe(L"after", check);
    return 0;
}
