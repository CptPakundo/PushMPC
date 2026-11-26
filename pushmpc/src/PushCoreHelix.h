#pragma once
// PushMPC: HelixSR (github.com/lonewolf0622/HelixSR, release 3.4.1, HelixSR Freeware License; community work, not
// affiliated with NVIDIA or AMD). Setup fetches the pinned release on the user's PC (scripts\helixsr-fetch.ps1) and keeps
// only what pushcore uses, under <runtime_local>\community\helixsr\:
//   anr\nvngx_dlssg.dll, anr\helixsr.ini   HelixSR frame generation (FidelityFX frame interpolation + HelixSR's own work,
//                                          2x-6x) behind the DLSS-G (NGX) provider interface; PushFPS drives it as a direct
//                                          provider (FrameGenerationBackend::Anr), no NVIDIA driver involved
//   fsr4-full\amd_fidelityfx_upscaler_dx12.dll, hlx_fsr4.ini   HLX FSR 4 Full: AMD's FSR 4.1.2 network (MIT) as compute
//                                          shaders, for RX 5000; behind the FidelityFX upscaler API
//   fsr4\amd_fidelityfx_upscaler_dx12.dll  HLX FSR 4 (the Lite network; GTX 16 / GTX 10 paths)
//   LICENSE, THIRD_PARTY_NOTICES.md, SOURCE.md
// Setup also copies AMD's pinned FidelityFX loader next to each HLX FSR 4 DLL (PushFPS's FSR upscaler loads the loader and
// the provider from one folder: PUSHMPC_FSR_SR_DIR). HLX DLSS (NVIDIA's DLSS network rebuilt from NVIDIA's DLL on the PC)
// is NOT used: PushMPC does not copy or convert NVIDIA's models.
// Nothing here is disassembled or modified (the licence forbids it): pushcore uses the two public interfaces only.
#include <cstdint>
#include <filesystem>

namespace pmhelix {

struct PinnedFile { const wchar_t* rel; uint64_t size; };   // SHA-256 pins: scripts\helixsr-fetch.ps1 (checked at install)
constexpr const char* kVersion = "3.4.1";
constexpr PinnedFile kFgFiles[] = { { L"anr\\nvngx_dlssg.dll", 1256448ull }, { L"anr\\helixsr.ini", 618ull } };
constexpr PinnedFile kFsr4FullFiles[] = { { L"fsr4-full\\amd_fidelityfx_upscaler_dx12.dll", 22876979ull }, { L"fsr4-full\\amd_fidelityfx_loader_dx12.dll", 0 } };
constexpr PinnedFile kFsr4LiteFiles[] = { { L"fsr4\\amd_fidelityfx_upscaler_dx12.dll", 58185808ull }, { L"fsr4\\amd_fidelityfx_loader_dx12.dll", 0 } };

// <runtime_local>\community\helixsr (runtime_local = veyra::runtime::localDataDirectory())
inline std::filesystem::path Root(const std::filesystem::path& localData) { return localData / L"community" / L"helixsr"; }

// the files are there with the pinned sizes (size 0 = any size: AMD's loader, pinned by fidelityfx-fetch / the package)
template <size_t N> bool Present(const std::filesystem::path& root, const PinnedFile (&files)[N])
{
    for (const auto& f : files) {
        std::error_code ec;
        const auto n = std::filesystem::file_size(root / f.rel, ec);
        if (ec || (f.size && n != f.size)) return false;
    }
    return true;
}
inline bool FgPresent(const std::filesystem::path& localData) { return Present(Root(localData), kFgFiles); }
inline bool Fsr4FullPresent(const std::filesystem::path& localData) { return Present(Root(localData), kFsr4FullFiles); }
inline bool Fsr4LitePresent(const std::filesystem::path& localData) { return Present(Root(localData), kFsr4LiteFiles); }

}  // namespace pmhelix
