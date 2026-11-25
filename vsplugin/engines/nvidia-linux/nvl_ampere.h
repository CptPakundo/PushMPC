// VFG on RTX 30 (SM86) under Linux: the port of PushFPS's VfgAmpereCompat (Windows: IAT rewrite of the VFG DLL's two
// nvcuda imports). The Linux VFG library (libnvVFXVideoFrameGeneration.so) imports cuDeviceGetAttribute and
// cuModuleLoadData from libcuda.so.1 directly (checked on the pinned wheel by the structural CI test), so the same two
// hooks go into ITS global offset table only (its JUMP_SLOT / GLOB_DAT relocations; RELRO pages are made writable for
// the write and read-only again); every other caller of the driver is unchanged:
//   cuDeviceGetAttribute  attribute 76 (compute minor) reads 9 instead of 6 on the selected device;
//   cuModuleLoadData      a fatbin is retargeted SM89 -> SM86 into a private copy (nvl_fatbin.h), then loaded.
// The library is identified by SHA-256 and size first (the pin in scripts/nvidia-vfx-pin.json); an unknown build is
// left alone. Auto enables this only after the self-test (pushmpc-nvvfx-selftest) passed on this machine.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pmvs::nvl {

struct GotSlot { std::string symbol; uintptr_t address = 0; bool relro = false; };
// The GOT slots of `symbols` in the loaded object whose link map is `handle` (from dlopen). false + why when a symbol
// has no slot, or more than one per symbol kind; slots are reported for the tests.
bool FindGotSlots(void* handle, const std::vector<std::string>& symbols, std::vector<GotSlot>& slots, std::string& why);

class AmpereCompat {
public:
    // the identity of the Linux VFG library this was audited against (scripts/nvidia-vfx-pin.json)
    static constexpr uint64_t kKnownSize = 218148697;
    static constexpr const char* kKnownSha256 = "9CEEF5EEE4A61ED7C7F27D48AEAF7CF46D7DBF5D6E4318D591BC5375996422D3";

    // vfgHandle: dlopen of libnvVFXVideoFrameGeneration.so (absolute path); cudaLib: dlopen of libcuda.so.1;
    // device: the CUDA device VFG runs on. checkIdentity=false only for the tests.
    bool Install(void* vfgHandle, const std::string& vfgPath, void* cudaLib, int device, std::string& why, bool checkIdentity = true);
    bool Installed() const { return m_installed; }
    bool Restore();
    ~AmpereCompat() { Restore(); }
    // counters of the hooks (process-wide)
    static unsigned PatchedLoads();
    static unsigned AttributeSpoofs();
    static unsigned RejectedLoads();

private:
    bool m_installed = false;
    std::vector<GotSlot> m_slots;
    std::vector<uintptr_t> m_original;
};

} // namespace pmvs::nvl
