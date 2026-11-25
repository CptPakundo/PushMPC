// The SM89 -> SM86 fatbin retarget of VFG's kernels for RTX 30 (Ampere, SM86): a port of PushFPS's
// VfgAmpereCompat::retargetVfgFatbin (src/pipeline/VfgAmpereCompat.cpp, GPL-3.0; itself derived from
// Xchen991/RTX30-Broadcast-FrameGen, MIT), unchanged in substance: bounded parsing into a private copy, only the audited
// SM89 entry layout (uncompressed ELF64 CUDA entry, header 64, e_flags 0x00590559) is rewritten to SM86
// (container arch 86, e_flags 0x00560556); anything else is rejected. Retargeting metadata does not translate the
// kernels' instructions: SM86 runs SM89 SASS because the two are one binary-compatible family (Ampere GA10x / Ada).
// Pure C++, any OS: nvl_test.cpp checks it on synthetic fatbins, the structural CI test on the real Linux VFG library.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace pmvs::nvl {

inline constexpr uint32_t kFatbinMagic = 0xBA55ED50u;
inline constexpr size_t kMaxFatbin = 32u * 1024u * 1024u;
struct FatbinChanges { unsigned containers = 0, elfFlags = 0, entries = 0; };

namespace fatbin_detail {
template <class T> inline T Get(const uint8_t* s, size_t p) { T v{}; std::memcpy(&v, s + p, sizeof(v)); return v; }
template <class T> inline void Put(uint8_t* s, size_t p, T v) { std::memcpy(s + p, &v, sizeof(v)); }
inline bool Fits(size_t p, size_t n, size_t size) { return p <= size && n <= size - p; }
}

// the whole fatbin's size from its 16-byte header (0 when it is not one)
inline size_t FatbinSize(const uint8_t* header16)
{
    using namespace fatbin_detail;
    if (Get<uint32_t>(header16, 0) != kFatbinMagic || Get<uint16_t>(header16, 4) != 1 || Get<uint16_t>(header16, 6) != 16) return 0;
    const uint64_t payload = Get<uint64_t>(header16, 8);
    return payload > kMaxFatbin - 16 ? 0 : size_t(payload) + 16;
}

inline bool RetargetVfgFatbin(const uint8_t* input, size_t size, std::vector<uint8_t>& output, FatbinChanges& changed, std::string& why)
{
    using namespace fatbin_detail;
    output.clear(); changed = {}; why.clear();
    auto bad = [&](const char* t) { why = t; output.clear(); return false; };
    if (size < 16 || size > kMaxFatbin) return bad("fatbin length outside bounded range");
    if (Get<uint32_t>(input, 0) != kFatbinMagic || Get<uint16_t>(input, 4) != 1 || Get<uint16_t>(input, 6) != 16) return bad("unknown fatbin header");
    if (Get<uint64_t>(input, 8) != size - 16) return bad("fatbin payload length mismatch");
    output.assign(input, input + size);
    size_t p = 16; unsigned entries = 0;
    while (p < size) {
        if (++entries > 4096 || !Fits(p, 64, size)) return bad("truncated fatbin entry");
        const auto kind = Get<uint16_t>(input, p), version = Get<uint16_t>(input, p + 2);
        const auto header = Get<uint32_t>(input, p + 4);
        const auto bytes = Get<uint64_t>(input, p + 8);
        if ((kind != 1 && kind != 2) || version != 0x101 || header < 64 || header > 4096 || header % 8 || !Fits(p, header, size) || bytes > size - p - header || !bytes)
            return bad("invalid fatbin entry bounds or format");
        const auto sm = Get<uint32_t>(input, p + 28);
        const size_t payload = p + header;
        if (sm == 89) {
            if (kind != 2 || header != 64 || bytes < 64 || Get<uint32_t>(input, payload) != 0x464c457f || Get<uint8_t>(input, payload + 4) != 2
                || Get<uint8_t>(input, payload + 5) != 1 || Get<uint16_t>(input, payload + 18) != 190 || Get<uint32_t>(input, payload + 48) != 0x00590559)
                return bad("unrecognized SM89 CUDA ELF layout");
            Put<uint32_t>(output.data(), p + 28, 86); Put<uint32_t>(output.data(), payload + 48, 0x00560556);
            ++changed.containers; ++changed.elfFlags;
        }
        p = payload + size_t(bytes);
    }
    if (!entries) return bad("empty fatbin");
    changed.entries = entries;
    return true;
}

// The attribute spoof: CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR (76) reads 9 instead of 6 on the selected device
// (PushFPS vfgReportedAttribute).
constexpr int VfgReportedAttribute(int value, int attribute, int device, int selectedDevice, int result)
{
    return !result && attribute == 76 && device == selectedDevice && value == 6 ? 9 : value;
}

} // namespace pmvs::nvl
