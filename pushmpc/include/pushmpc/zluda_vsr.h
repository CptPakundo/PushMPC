#pragma once
// RTX Video Super Resolution on AMD RX 7000 / RX 9000 through ZLUDA (unlock ZludaVsr, PUSHCORE_SR_METHOD_ZLUDA_VSR).
//
// The route: NVIDIA's nvngx_vsr.dll (the RTX Video SDK feature DLL, here the NVIDIA Video Effects build from NVIDIA's
// PyPI wheel) runs through its CUDA entry points (NVSDK_NGX_CUDA_*) on ZLUDA, a CUDA driver on AMD's HIP. pushcore never
// loads any of that: a worker process (pushcore_zluda_vsr_worker.exe) does, as PushFPS's RX 7000 NR worker does for NR,
// with the same ZLUDA files (runtime\amd-nr-rdna3\zluda: nvapi64.dll -> zluda_nvapi.dll, zluda_real.dll, the nvcuda.dll
// gate, the nvngx.dll NGX host). Pictures move as D3D12 shared textures imported into CUDA (no pixel copies through the
// CPU); fences are shared D3D12 fences imported as CUDA external semaphores.
//
// Evidence this header is built on (pushmpc tools and notes in the report of the change):
//   - the NGX CUDA image contract: NVIDIA's own CUDA helpers pass CUtexObject* / void* (nvsdk_ngx_helpers.h
//     NVSDK_NGX_CUDA_Feature_Eval_Params, nvsdk_ngx_helpers_dlssd_cuda.h); the CUDA VSR caller NVIDIA ships
//     (nvVFXVideoSuperRes.dll) creates cuArray + cuTexObject (input) + cuSurfObject (output) and sets "Input1",
//     "Output", "Rect.*", "OutRect.*", "VSR.QualityLevel"; the RedDuke NR runtime (MIT, the one PushFPS ships) hands the
//     snippet a pointer to { object, width, height, pitch, format } whose first 8 bytes are the texture / surface object;
//     nvngx_vsr.dll's own PTX reads its input with tex.2d (texture object) and writes with sust.2d (surface object);
//   - CUDA context / stream: NVIDIA's CUDA feature helpers set "Input1" = CUcontext and "Input2" = CUstream at
//     CreateFeature (NGX_CUDA_CREATE_DLSSD_EXT); Init_Ext1 takes the CUcontext;
//   - the NGX snippets only accept calls from a module named nvngx.dll (RedDuke: "the network only accepts calls that come
//     from a module named nvngx.dll"): the worker calls through RedDuke's NGX host (zluda\nvngx.dll, ngxrt_* exports);
//   - nvngx_vsr.dll (VFX build, 59,120,752 B): 79 fatbins, every one with sm_80 PTX (PTX ISA 8.6); 100 standalone CUDA
//     ELF cubins (69 sm_80, 31 sm_75); every sm_80 cubin kernel has a PTX twin of the same name (5 kernels without PTX are
//     sm_75 only, the Turing path). ZLUDA runs PTX, not SASS, so the worker swaps each standalone cubin the DLL loads for
//     the PTX module that holds the same kernels (PtxIndex below; the RedDuke gate does the same for DLSS-G with
//     cubin_fatbin_map.txt). Instructions in that PTX: mma.sync m16n8k32 s8 (7,720), m16n8k16 f16 (3,264) / s8 (1,248) /
//     bf16 (32), ldmatrix x4/x2 (8,613), cp.async (833), tex.2d, suld/sust.2d: ZLUDA implements mma (PRs 546/551/571),
//     ldmatrix (503), cp.async (427) and surfaces (203, 688).
//
// Header-only and Win32 only; the parts without a GPU (packing, protocol, the PTX index, the picture check, the
// refusals) are checked by pushmpc\tools\zluda_vsr_test.cpp.
#include <windows.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

namespace pmz {

// ------------------------------------------------------------------------------------------------ files
// runtimeDir = pushcore's runtime directory (<root>\runtime\experimental, as PushCoreGpu.cpp uses it)
inline const wchar_t* const kZludaFiles[] = { L"nvapi64.dll", L"zluda_nvapi.dll", L"zluda_real.dll", L"nvcuda.dll", L"nvngx.dll" };
inline std::wstring ZludaDir(const std::wstring& runtimeDir) { return runtimeDir + L"\\..\\amd-nr-rdna3\\zluda"; }
// the nvngx_vsr.dll the worker uses, best first: PushMPC's own copy for AMD (NVIDIA's wheel, scripts\nvidia-vfx-fetch.ps1
// -Only nvngx_vsr.dll into runtime\zluda-vsr), then the NVIDIA package's Video Effects copy, then the RTX Video SDK one
inline std::vector<std::wstring> VsrCandidates(const std::wstring& runtimeDir)
{
    return { runtimeDir + L"\\..\\zluda-vsr\\nvngx_vsr.dll", runtimeDir + L"\\..\\nvidia-vfg\\nvngx_vsr.dll", runtimeDir + L"\\nvngx_vsr.dll" };
}
inline bool FileThere(const std::wstring& p) { const DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
inline std::wstring FindVsr(const std::wstring& runtimeDir)
{
    for (const auto& c : VsrCandidates(runtimeDir)) if (FileThere(c)) return c;
    return {};
}
// the first missing ZLUDA file (empty = all there)
inline std::wstring MissingZluda(const std::wstring& runtimeDir)
{
    for (const wchar_t* f : kZludaFiles) if (!FileThere(ZludaDir(runtimeDir) + L"\\" + f)) return f;
    return {};
}
// AMD's HIP runtime, which ZLUDA runs on: in the driver (System32) or the HIP SDK (HIP_PATH\bin)
inline std::wstring FindHip()
{
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring s = std::wstring(sys) + L"\\amdhip64_7.dll";
    if (FileThere(s)) return s;
    wchar_t sdk[1024] = {};
    const DWORD n = GetEnvironmentVariableW(L"HIP_PATH", sdk, 1024);
    if (n && n < 1024) { const std::wstring h = std::wstring(sdk) + L"\\bin\\amdhip64_7.dll"; if (FileThere(h)) return h; }
    return {};
}

// ------------------------------------------------------------------------------------------------ eligibility (pure)
// family = PUSHCORE_FAMILY_* (6 RX 9000, 7 RX 7000)
struct Eligibility {
    bool hardware = false;     // the GPU could run it (an AMD RX 7000 / 9000)
    bool files = false;        // ZLUDA, HIP and nvngx_vsr.dll are there
    std::string why;           // the first reason it cannot run (empty when both hold)
};
inline Eligibility Check(uint32_t vendor, uint32_t family, bool hip, const std::wstring& missingZluda, bool vsr)
{
    Eligibility e;
    if (vendor != 0x1002) { e.why = "needs an AMD RX 7000 or RX 9000 (no AMD GPU)"; return e; }
    if (family != 6u && family != 7u) { e.why = "needs an AMD RX 7000 or RX 9000 (RDNA3 / RDNA4)"; return e; }
    e.hardware = true;
    if (!hip) { e.why = "AMD's HIP runtime (amdhip64_7.dll) is not installed"; return e; }
    if (!missingZluda.empty()) {
        char b[160]; std::snprintf(b, sizeof(b), "ZLUDA is not installed (runtime\\amd-nr-rdna3\\zluda\\%ls)", missingZluda.c_str());
        e.why = b; return e;
    }
    if (!vsr) { e.why = "nvngx_vsr.dll is not installed (runtime\\zluda-vsr)"; return e; }
    e.files = true;
    return e;
}

// ------------------------------------------------------------------------------------------------ worker protocol
// Parent (pushcore) and worker talk over two anonymous pipes; every message is a fixed-size struct.
//   Init     -> Reply (0 = ready: VSR created and warmed up on the shared textures)
//   Evaluate -> Reply (after the done fence was signalled for this sequence)
//   Stop     -> the worker releases everything and exits 0
// GPU order per frame: parent queue Signal(producer, n) after its copy into the shared input; worker: CUDA wait
// (producer >= n), VSR, CUDA signal (done = n); parent queue Wait(done, n) before its copy out of the shared output.
constexpr uint32_t kMagic = 0x5253565Au /* "ZVSR" */, kVersion = 1;
enum class Op : uint32_t { Init = 1, Evaluate = 2, Stop = 3, Reply = 4 };
struct Header { uint32_t magic = kMagic, version = kVersion, bytes = 0; Op op = Op::Init; uint64_t sequence = 0; };
struct Init {
    Header h{ kMagic, kVersion, 0, Op::Init, 0 };
    uint64_t adapterLuid = 0;
    uint32_t inW = 0, inH = 0, outW = 0, outH = 0;
    uint32_t quality = 1;          // VSR.QualityLevel 1..4
    uint32_t layout = 0;           // Layout bits (the one the self-test passed with)
    // shared NT handles, duplicated into the worker: input RGBA8 texture, output RGBA8 texture, producer fence, done fence
    std::array<uint64_t, 4> handles{};
    std::array<wchar_t, 512> zluda{}, vsr{}, cache{}, log{};
};
struct Evaluate { Header h{ kMagic, kVersion, 0, Op::Evaluate, 0 }; };
struct Reply {
    Header h{ kMagic, kVersion, 0, Op::Reply, 0 };
    int32_t result = 0;            // 0 ok
    float ms = -1;                 // worker time of the evaluate (wait + VSR + signal), < 0 unknown
    std::array<char, 512> message{};
};
template <class T> constexpr uint32_t Bytes() { return uint32_t(sizeof(T)); }
inline Init MakeInit() { Init i; i.h.bytes = Bytes<Init>(); return i; }
inline Evaluate MakeEvaluate(uint64_t seq) { Evaluate e; e.h.bytes = Bytes<Evaluate>(); e.h.sequence = seq; return e; }
inline Header MakeStop(uint64_t seq) { Header h; h.bytes = uint32_t(sizeof(Header)); h.op = Op::Stop; h.sequence = seq; return h; }
inline Reply MakeReply(uint64_t seq, int32_t result, const std::string& msg, float ms = -1)
{
    Reply r; r.h.bytes = Bytes<Reply>(); r.h.sequence = seq; r.result = result; r.ms = ms;
    std::memcpy(r.message.data(), msg.data(), std::min(msg.size(), r.message.size() - 1));
    return r;
}
constexpr bool ValidHeader(const Header& h, Op op, uint32_t bytes) { return h.magic == kMagic && h.version == kVersion && h.op == op && h.bytes == bytes; }
// RTX VSR's sizes: input at least 64x64 and at most 4096x2304 (4K DCI), output >= input, at most 4x and 8K
constexpr bool ValidExtent(uint32_t w, uint32_t h, uint32_t ow, uint32_t oh)
{
    return w >= 64 && h >= 64 && w <= 4096 && h <= 2304 && ow >= w && oh >= h && ow <= 7680 && oh <= 4320 && ow <= 4 * w && oh <= 4 * h;
}
template <size_t N> bool TerminatedPath(const std::array<wchar_t, N>& s) { return s[0] != 0 && s[N - 1] == 0; }
template <size_t N> void PutPath(std::array<wchar_t, N>& d, const std::wstring& s) { d.fill(0); wcsncpy_s(d.data(), N, s.c_str(), _TRUNCATE); }
// empty = valid
inline std::string ValidateInit(const Init& i)
{
    if (!ValidHeader(i.h, Op::Init, Bytes<Init>()) || i.h.sequence != 0) return "bad header";
    if (!ValidExtent(i.inW, i.inH, i.outW, i.outH)) return "bad extent";
    if (i.quality < 1 || i.quality > 4) return "bad quality";
    if (i.layout > 3) return "bad layout";
    for (auto v : i.handles) if (!v) return "missing handle";
    if (!TerminatedPath(i.zluda) || !TerminatedPath(i.vsr) || !TerminatedPath(i.cache) || !TerminatedPath(i.log)) return "bad path";
    return {};
}
static_assert(std::is_trivially_copyable_v<Init> && sizeof(Init) < 8192);
static_assert(std::is_trivially_copyable_v<Reply> && std::is_trivially_copyable_v<Evaluate>);

// ------------------------------------------------------------------------------------------------ NGX CUDA packing
// The image parameter (RedDuke's reverse-engineered descriptor; NGX reads the first 8 bytes and asks CUDA about them)
struct CudaImage { uint64_t object = 0; uint32_t width = 0, height = 0, pitch = 0, format = 0; };
static_assert(sizeof(CudaImage) == 24 && offsetof(CudaImage, object) == 0);
// Layout bits: the self-test tries 0, 2, 1, 3 and keeps the first that passes
constexpr uint32_t kLayoutHandleValue = 1u;   // the image parameter is the object value itself (void*), not a pointer to CudaImage
constexpr uint32_t kLayoutNoContext = 2u;     // no CUcontext / CUstream as Input1 / Input2 at CreateFeature
inline const uint32_t kLayoutOrder[4] = { 0u, kLayoutNoContext, kLayoutHandleValue, kLayoutHandleValue | kLayoutNoContext };
constexpr int kFeatureVsr = 16;                // NVSDK_NGX_Feature_Reserved16 (RTX VSR; PushFPS VideoSrBackend uses the same id on D3D12)
constexpr unsigned long long kAppId = 0x1ull;  // as vsr_gate_probe
constexpr int kNgxVersionApi = 0x15;           // NVSDK_NGX_Version_API (SDK 310.x)
inline void* ImageParam(CudaImage& img, uint32_t layout) { return (layout & kLayoutHandleValue) ? reinterpret_cast<void*>(uintptr_t(img.object)) : static_cast<void*>(&img); }
// P: anything with Set(const char*, unsigned) and Set(const char*, void*) (NVSDK_NGX_Parameter, or the test's recorder)
template <class P> void PackCreate(P& p, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, uint32_t quality, uint32_t layout, void* cuContext, void* cuStream)
{
    p.Set("CreationNodeMask", 1u); p.Set("VisibilityNodeMask", 1u);
    p.Set("Width", w); p.Set("Height", h); p.Set("OutWidth", ow); p.Set("OutHeight", oh);
    p.Set("VSR.QualityLevel", quality);
    if (!(layout & kLayoutNoContext)) { p.Set("Input1", cuContext); p.Set("Input2", cuStream); }
}
template <class P> void PackEvaluate(P& p, CudaImage& in, CudaImage& out, uint32_t quality, uint32_t layout)
{
    p.Set("Input1", ImageParam(in, layout)); p.Set("Output", ImageParam(out, layout));
    p.Set("Rect.X", 0u); p.Set("Rect.Y", 0u); p.Set("Rect.W", in.width); p.Set("Rect.H", in.height);
    p.Set("OutRect.X", 0u); p.Set("OutRect.Y", 0u); p.Set("OutRect.W", out.width); p.Set("OutRect.H", out.height);
    p.Set("VSR.QualityLevel", quality);
}

// ------------------------------------------------------------------------------------------------ cubin -> PTX
// LZ4 block format (fatbin entries with flag 0x2000 hold their payload LZ4-compressed). false on malformed input.
inline bool Lz4Block(const uint8_t* src, size_t n, std::vector<uint8_t>& dst, size_t outSize)
{
    dst.clear(); dst.reserve(outSize);
    size_t i = 0;
    while (i < n) {
        const uint8_t tok = src[i++];
        size_t lit = tok >> 4;
        if (lit == 15) { uint8_t b; do { if (i >= n) return false; b = src[i++]; lit += b; } while (b == 255); }
        if (i + lit > n || dst.size() + lit > outSize) return false;
        dst.insert(dst.end(), src + i, src + i + lit); i += lit;
        if (i >= n || dst.size() == outSize) break;
        if (i + 2 > n) return false;
        const size_t off = size_t(src[i]) | (size_t(src[i + 1]) << 8); i += 2;
        size_t ml = tok & 15;
        if (ml == 15) { uint8_t b; do { if (i >= n) return false; b = src[i++]; ml += b; } while (b == 255); }
        ml += 4;
        if (!off || off > dst.size() || dst.size() + ml > outSize) return false;
        const size_t start = dst.size() - off;
        for (size_t k = 0; k < ml; ++k) dst.push_back(dst[start + k]);
    }
    return dst.size() == outSize;
}
constexpr uint16_t kEmCuda = 190;
inline bool IsCudaElf(const void* p)
{
    const auto* b = static_cast<const uint8_t*>(p);
    if (!b || b[0] != 0x7F || b[1] != 'E' || b[2] != 'L' || b[3] != 'F' || b[4] != 2 /*64-bit*/) return false;
    uint16_t em = 0; std::memcpy(&em, b + 18, 2);
    return em == kEmCuda;
}
// the size of an ELF image from its header (section header table end, or the furthest section), 0 = malformed
inline size_t ElfSize(const uint8_t* b, size_t limit = size_t(1) << 31)
{
    uint64_t shoff = 0; uint16_t shentsize = 0, shnum = 0;
    std::memcpy(&shoff, b + 40, 8); std::memcpy(&shentsize, b + 58, 2); std::memcpy(&shnum, b + 60, 2);
    if (shentsize != 64 || !shnum || shoff > limit) return 0;
    uint64_t end = shoff + uint64_t(shnum) * 64;
    for (uint16_t i = 0; i < shnum; ++i) {
        uint64_t off = 0, size = 0; uint32_t type = 0;
        std::memcpy(&type, b + shoff + i * 64 + 4, 4);
        std::memcpy(&off, b + shoff + i * 64 + 24, 8); std::memcpy(&size, b + shoff + i * 64 + 32, 8);
        if (type != 8 /*SHT_NOBITS*/) end = std::max(end, off + size);
    }
    return end > limit ? 0 : size_t(end);
}
// kernel names of a CUDA ELF (sections ".text.<kernel>")
inline std::vector<std::string> ElfKernels(const uint8_t* b, size_t n)
{
    std::vector<std::string> k;
    if (n < 64 || !IsCudaElf(b)) return k;
    uint64_t shoff = 0; uint16_t shentsize = 0, shnum = 0, shstrndx = 0;
    std::memcpy(&shoff, b + 40, 8); std::memcpy(&shentsize, b + 58, 2); std::memcpy(&shnum, b + 60, 2); std::memcpy(&shstrndx, b + 62, 2);
    if (shentsize != 64 || shstrndx >= shnum || shoff + uint64_t(shnum) * 64 > n) return k;
    uint64_t stroff = 0, strsize = 0;
    std::memcpy(&stroff, b + shoff + shstrndx * 64 + 24, 8); std::memcpy(&strsize, b + shoff + shstrndx * 64 + 32, 8);
    if (stroff + strsize > n) return k;
    for (uint16_t i = 0; i < shnum; ++i) {
        uint32_t nameOff = 0; std::memcpy(&nameOff, b + shoff + i * 64, 4);
        if (nameOff >= strsize) continue;
        const char* s = reinterpret_cast<const char*>(b + stroff + nameOff);
        const size_t len = strnlen(s, size_t(strsize - nameOff));
        if (len > 6 && std::strncmp(s, ".text.", 6) == 0) k.emplace_back(s + 6, len - 6);
    }
    return k;
}
// .entry names of a PTX module
inline std::vector<std::string> PtxEntries(const std::string& ptx)
{
    std::vector<std::string> e;
    for (size_t p = ptx.find(".entry"); p != std::string::npos; p = ptx.find(".entry", p + 6)) {
        size_t a = p + 6;
        while (a < ptx.size() && (ptx[a] == ' ' || ptx[a] == '\t')) ++a;
        size_t z = a;
        while (z < ptx.size() && (isalnum(static_cast<unsigned char>(ptx[z])) || ptx[z] == '_' || ptx[z] == '$')) ++z;
        if (z > a) e.emplace_back(ptx.substr(a, z - a));
    }
    return e;
}
// Every PTX module of a DLL image (all fatbins, LZ4 payloads expanded) and which module holds which kernel.
class PtxIndex {
public:
    struct Stats { size_t fatbins = 0, ptx = 0, compressed = 0, bad = 0, elfInFatbin = 0; };
    bool Build(const uint8_t* img, size_t n)
    {
        modules_.clear(); entries_.clear(); byEntry_.clear(); stats_ = {};
        for (size_t pos = 0; pos + 16 <= n;) {
            const uint8_t magic[4] = { 0x50, 0xED, 0x55, 0xBA };
            const uint8_t* f = std::search(img + pos, img + n, magic, magic + 4);
            if (f == img + n) break;
            pos = size_t(f - img);
            uint16_t hsz = 0; uint64_t fsz = 0;
            if (pos + 16 > n) break;
            std::memcpy(&hsz, img + pos + 6, 2); std::memcpy(&fsz, img + pos + 8, 8);
            if (hsz != 16 || fsz > n - pos - 16) { pos += 4; continue; }
            ++stats_.fatbins;
            size_t e = pos + 16; const size_t end = e + size_t(fsz);
            while (e + 64 <= end) {
                uint16_t kind = 0; uint32_t ehs = 0, csz = 0; uint64_t size = 0, flags = 0, dsz = 0;
                std::memcpy(&kind, img + e, 2); std::memcpy(&ehs, img + e + 4, 4); std::memcpy(&size, img + e + 8, 8);
                std::memcpy(&csz, img + e + 16, 4); std::memcpy(&flags, img + e + 40, 8); std::memcpy(&dsz, img + e + 56, 8);
                if (ehs < 64 || ehs > 512 || e + ehs + size > end) break;
                const uint8_t* payload = img + e + ehs;
                if (kind == 1) {
                    std::string text;
                    if ((flags & 0x2000) && dsz) {
                        ++stats_.compressed;
                        std::vector<uint8_t> out;
                        if (csz <= size && dsz < (size_t(256) << 20) && Lz4Block(payload, csz, out, size_t(dsz))) text.assign(out.begin(), out.end());
                        else ++stats_.bad;
                    } else {
                        text.assign(reinterpret_cast<const char*>(payload), size_t(size));
                    }
                    while (!text.empty() && text.back() == 0) text.pop_back();
                    if (!text.empty()) Add(std::move(text));
                } else if (kind == 2) {
                    ++stats_.elfInFatbin;
                }
                e += ehs + size_t(size);
            }
            pos = end;
        }
        stats_.ptx = modules_.size();
        return !modules_.empty();
    }
    void Add(std::string ptx)
    {
        const size_t i = modules_.size();
        auto names = PtxEntries(ptx);
        for (const auto& k : names) byEntry_.emplace(k, i);
        entries_.emplace_back(names.begin(), names.end());
        modules_.push_back(std::move(ptx));
    }
    // the PTX module with every kernel of this cubin; nullptr when there is none
    const std::string* ForKernels(const std::vector<std::string>& kernels) const
    {
        if (kernels.empty()) return nullptr;
        const auto range = byEntry_.equal_range(kernels.front());
        for (auto it = range.first; it != range.second; ++it) {
            const auto& set = entries_[it->second];
            if (std::all_of(kernels.begin(), kernels.end(), [&](const std::string& k) { return set.count(k) != 0; })) return &modules_[it->second];
        }
        return nullptr;
    }
    const std::string* ForCubin(const void* elf) const
    {
        if (!IsCudaElf(elf)) return nullptr;
        const auto* b = static_cast<const uint8_t*>(elf);
        const size_t n = ElfSize(b);
        return n ? ForKernels(ElfKernels(b, n)) : nullptr;
    }
    size_t Modules() const { return modules_.size(); }
    const Stats& stats() const { return stats_; }
private:
    std::vector<std::string> modules_;
    std::vector<std::set<std::string>> entries_;
    std::multimap<std::string, size_t> byEntry_;
    Stats stats_;
};
// Standalone cubins of a DLL image (CUDA ELFs outside any fatbin) and how many map to a PTX module, per SM version
struct CubinCoverage { std::map<int, size_t> cubins, mapped; std::vector<std::string> unmapped; };
inline CubinCoverage Coverage(const uint8_t* img, size_t n, const PtxIndex& index)
{
    CubinCoverage c;
    for (size_t pos = 0; pos + 64 <= n; ++pos) {
        if (img[pos] != 0x7F || !IsCudaElf(img + pos)) continue;
        const size_t sz = ElfSize(img + pos, n - pos);
        if (!sz) continue;
        uint32_t flags = 0; std::memcpy(&flags, img + pos + 48, 4);
        const int sm = int(flags & 0xFF);
        const auto k = ElfKernels(img + pos, sz);
        if (k.empty()) continue;
        ++c.cubins[sm];
        if (index.ForKernels(k)) ++c.mapped[sm]; else c.unmapped.push_back("sm_" + std::to_string(sm) + " " + k.front());
        pos += sz - 1;
    }
    return c;
}

// ------------------------------------------------------------------------------------------------ the picture check
// 8-bit RGBA test picture (vsr_gate_probe's): smooth gradients, hard edges at several angles, fine strokes, noise
inline std::vector<uint8_t> MakeInput(uint32_t w, uint32_t h)
{
    std::vector<uint8_t> p(size_t(w) * h * 4);
    uint32_t seed = 12345;
    for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
        seed = seed * 1664525u + 1013904223u;
        const double fx = double(x) / w, fy = double(y) / h;
        double r = 40 + 150 * fx, g = 60 + 120 * fy, b = 90 + 60 * std::sin(fx * 9 + fy * 5);
        if (((x / 24) + (y / 24)) % 2 == 0 && fx > 0.55 && fy < 0.45) { r = 230; g = 230; b = 230; }
        const double ring = std::hypot(fx - 0.25, fy - 0.65) * 40; if (std::fmod(ring, 2.0) < 1.0) { r *= 0.4; g *= 0.4; }
        if (fy > 0.80 && fy < 0.90 && (x % 7 == 0 || (y % 9 == 0 && x % 3 != 0))) { r = g = b = 250; }
        const double n = double(seed >> 24) / 255.0 * 12 - 6;
        auto c = [&](double v) { return uint8_t(std::clamp(v + n, 0.0, 255.0)); };
        uint8_t* o = &p[(size_t(y) * w + x) * 4]; o[0] = c(r); o[1] = c(g); o[2] = c(b); o[3] = 255;
    }
    return p;
}
inline double Cubic(double x) { x = std::fabs(x); const double a = -0.5; return x <= 1 ? (a + 2) * x * x * x - (a + 3) * x * x + 1 : x < 2 ? a * x * x * x - 5 * a * x * x + 8 * a * x - 4 * a : 0; }
inline std::vector<uint8_t> Bicubic(const std::vector<uint8_t>& in, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh)
{
    std::vector<uint8_t> o(size_t(ow) * oh * 4);
    for (uint32_t y = 0; y < oh; ++y) for (uint32_t x = 0; x < ow; ++x) {
        const double sx = (x + 0.5) * w / ow - 0.5, sy = (y + 0.5) * h / oh - 0.5;
        const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
        for (int c = 0; c < 4; ++c) {
            double acc = 0, wsum = 0;
            for (int j = -1; j <= 2; ++j) for (int i = -1; i <= 2; ++i) {
                const int px = std::clamp(ix + i, 0, int(w) - 1), py = std::clamp(iy + j, 0, int(h) - 1);
                const double wt = Cubic(sx - (ix + i)) * Cubic(sy - (iy + j));
                acc += wt * in[(size_t(py) * w + px) * 4 + c]; wsum += wt;
            }
            o[(size_t(y) * ow + x) * 4 + c] = uint8_t(std::clamp(acc / wsum, 0.0, 255.0));
        }
    }
    return o;
}
inline double Psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    double se = 0; size_t n = 0;
    for (size_t i = 0; i + 3 < a.size() && i + 3 < b.size(); i += 4) for (int c = 0; c < 3; ++c) { const double d = double(a[i + c]) - b[i + c]; se += d * d; ++n; }
    const double mse = se / double(std::max<size_t>(n, 1));
    return mse <= 1e-9 ? 99.0 : 10 * std::log10(255.0 * 255.0 / mse);
}
inline double GradientEnergy(const std::vector<uint8_t>& p, uint32_t w, uint32_t h)
{
    double e = 0;
    for (uint32_t y = 1; y < h; ++y) for (uint32_t x = 1; x < w; ++x) {
        const size_t i = (size_t(y) * w + x) * 4;
        const double gx = double(p[i + 1]) - p[i + 1 - 4], gy = double(p[i + 1]) - p[i + 1 - size_t(w) * 4];
        e += gx * gx + gy * gy;
    }
    return e / (double(w) * h);
}
struct Verdict { bool pass = false; double psnr = 0, sharp = 0; std::string why; };
// vsr_gate_probe's sanity: the device is alive, the output is not blank or constant, it keeps the picture (>= 24 dB
// against a bicubic upscale) and it is at least as sharp (gradient energy >= 0.9 x bicubic)
inline Verdict Judge(const std::vector<uint8_t>& in, uint32_t w, uint32_t h, const std::vector<uint8_t>& out, uint32_t ow, uint32_t oh, bool deviceAlive)
{
    Verdict v;
    if (!deviceAlive) { v.why = "the device was removed"; return v; }
    if (out.size() != size_t(ow) * oh * 4) { v.why = "no output read back"; return v; }
    uint8_t mn = 255, mx = 0;
    for (size_t i = 0; i < out.size(); i += 4) { mn = std::min(mn, out[i + 1]); mx = std::max(mx, out[i + 1]); }
    if (mx - mn < 16) { v.why = "the output is blank or constant (green " + std::to_string(mn) + ".." + std::to_string(mx) + ")"; return v; }
    const auto ref = Bicubic(in, w, h, ow, oh);
    v.psnr = Psnr(out, ref);
    v.sharp = GradientEnergy(out, ow, oh) / std::max(1e-9, GradientEnergy(ref, ow, oh));
    char b[160];
    if (v.psnr < 24.0) { std::snprintf(b, sizeof(b), "the output does not match the picture (PSNR %.2f dB against bicubic, needs 24)", v.psnr); v.why = b; return v; }
    if (v.sharp < 0.9) { std::snprintf(b, sizeof(b), "the output is softer than bicubic (gradient energy x%.2f)", v.sharp); v.why = b; return v; }
    v.pass = true;
    std::snprintf(b, sizeof(b), "output sane (%.2f dB against bicubic, sharpness x%.2f)", v.psnr, v.sharp);
    v.why = b;
    return v;
}

// ------------------------------------------------------------------------------------------------ the self-test record
// SelfTest\ZludaVsr Reason: "pass layout=<n> ms=<median ms> at <w>x<h> x<scale> ..." (what pushcore reads back)
inline uint32_t LayoutFromReason(const std::wstring& r)
{
    const size_t p = r.find(L"layout=");
    if (p == std::wstring::npos) return 0;
    const int v = _wtoi(r.c_str() + p + 7);
    return v >= 0 && v <= 3 ? uint32_t(v) : 0;
}
inline double MsFromReason(const std::wstring& r)
{
    const size_t p = r.find(L"ms=");
    if (p == std::wstring::npos) return -1;
    const double v = _wtof(r.c_str() + p + 3);
    return v > 0 && v < 1000 ? v : -1;
}

} // namespace pmz
