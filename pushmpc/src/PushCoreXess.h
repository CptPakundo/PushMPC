// pushcore: Intel XeSS methods (G10). Internal to pushcore.dll.
//
//  - XeFG (XeSS frame generation, PUSHCORE_FG_XEFG): Intel's frame generation runs on a headless swap chain shim (the
//    proven xefg_headless_spike design: ShimSwapChain / ShimFactory, a few import hooks, a fake window and a virtual
//    monitor). The graph runs in PushFPS's XeSS present-sink mode: it produces the real frames at the output size (after
//    the upscaler) plus the motion PushFPS's XeSS presenter gives XeFG (the graph's refined optical flow, current ->
//    previous in output pixels) and constant depth. XefgStage copies each real frame into XeFG's proxy back buffer,
//    tags motion and depth and presents; every buffer XeFG presents is captured by the shim into an output texture
//    shared with D3D11, and pushcore hands those out with its own timestamps (XeFG's pacing is never used).
//  - XeSS SR (PUSHCORE_SR_METHOD_XESS): libxess.dll's D3D12 upscaler in the graph's temporal upscaler slot (the slot FSR
//    uses: FsrSrBackend's provider override), so it gets the same flow motion, depth, reset and frame time FSR gets.
//
// Runtime: libxess_fg.dll, libxell.dll and libxess.dll from Intel's XeSS SDK release (scripts\xess-fetch.ps1 installs
// them into <root>\runtime_local\intel\experimental, the folder PushFPS's own XeSS presenter loads from).
#pragma once
#include <d3d12.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pushmpc::xess {

// <root>\runtime_local\intel\experimental
std::filesystem::path runtimeFolder();

// One runtime file: present, Intel's Authenticode signature, the pinned build (SHA-256 + size), its version.
struct FileCheck {
    bool present = false, signedByIntel = false, pinned = false;
    std::string version, sha256, why;
    uint64_t size = 0;
};
FileCheck checkFile(const std::filesystem::path& path);
// XeFG's two files (libxess_fg.dll + libxell.dll), XeSS SR's one (libxess.dll); why = what is missing or wrong
bool fgFilesUsable(std::string* why);
bool srFileUsable(std::string* why);

// ---------------------------------------------------------------------------------------------------------- XeFG
struct XefgConfig {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;   // the graph's queue: XeFG's proxy swap chain presents on it
    ID3D11Device1* d3d11 = nullptr;        // outputs are opened here (nullptr: no D3D11 side, the self-test)
    uint32_t width = 0, height = 0;        // the output (work) size
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    bool hdr10 = false;                    // R10G10B10A2, PQ BT.2020
    uint32_t multiplier = 2;               // 2.. (generated per real = multiplier - 1)
    uint32_t outSlots = 0;                 // output textures (0 = 3 groups + 2)
};

class XefgStage {
public:
    XefgStage();
    ~XefgStage();
    XefgStage(const XefgStage&) = delete;
    XefgStage& operator=(const XefgStage&) = delete;

    bool init(const XefgConfig& config, std::string& why);
    void shutdown();
    // One real frame: `color` (COMMON, the output format and size) goes into XeFG's back buffer, `motion` (COMMON,
    // R16G16_FLOAT at the output size, current -> previous in pixels; nullptr = zero) and constant depth are tagged, then
    // XeFG presents. reset = no valid history (first frame, seek, graph reset). frameMs = the source frame interval
    // (0 = unknown). Records and executes on the queue; never waits for the GPU (XeLL may sleep, see the log).
    bool submit(ID3D12Resource* color, ID3D12Resource* motion, bool reset, float frameMs, int64_t pts100ns, uint64_t sourceIndex, std::string& why);
    // The next captured output in presentation order, once its kind is known.
    struct Output {
        uint32_t slot = 0;
        ID3D12Resource* resource = nullptr;
        ID3D11Texture2D* texture = nullptr;   // the slot opened in D3D11 (nullptr without a D3D11 device)
        ID3D12Fence* fence = nullptr; uint64_t fenceValue = 0;   // the capture copy is done at this value (shim queue)
        bool generated = false;
        uint32_t subframe = 0, groupSize = 1;  // generated: 1..groupSize-1; real: 0
        int64_t pts100ns = 0;
        uint64_t sourceIndex = 0;              // the real frame the output belongs to (generated: the later one of its pair)
        bool dropped = false;                  // no free slot when XeFG presented: nothing was captured
    };
    bool take(Output& out);
    // The caller is done with a slot: XeFG may capture into it once `fence` reaches `value` (nullptr = at once)
    void release(uint32_t slot, ID3D12Fence* fence, uint64_t value);
    // Room for another real frame's group (free slots for a whole group, nothing unmatched piling up)
    bool canSubmit() const;
    // Outputs XeFG still owes for the frames submitted so far (presented by its own thread later). Frames whose presents
    // never come (nothing captured for 15 s) are given up and counted as dropped.
    uint64_t pending();
    // Seek / flush: drops everything captured or expected; the next submit should carry reset
    void flush();
    uint32_t multiplier() const { return generated_ + 1; }
    uint32_t slotCount() const;
    ID3D12Resource* slotResource(uint32_t slot) const;
    const std::string& description() const { return description_; }
    struct Counters { uint64_t presents = 0, captures = 0, real = 0, generated = 0, dropped = 0, statusErrors = 0; double sleepMsTotal = 0, presentMsTotal = 0; };
    Counters counters() const;

    struct Impl;
private:
    std::unique_ptr<Impl> p_;
    uint32_t generated_ = 1;
    std::string description_;
};

// XeFG self-test (unlock XeFg): its own D3D12 device on the adapter (WARP with warp = true), XefgStage at the asked
// multiplier on N synthetic frames (a textured square moving at a known speed over a textured background, exact motion
// vectors, constant depth); passes when every init call succeeds, >= 90 % of the expected generated frames come out,
// the square in the generated frames sits within 2 px (median) of where it should be and the generated frames are closer
// to the ideal in-between frame than a blend of the two real ones (PSNR in the square's area).
struct XefgSelfTestResult {
    bool ran = false, passed = false;
    uint32_t multiplier = 0, realOut = 0, generatedOut = 0, expectedGenerated = 0;
    double medianPositionErrorPx = -1, medianPsnr = -1, medianPsnrBlend = -1, msPerGenerated = -1;
    std::string adapter, note;
    uint32_t vendor = 0, device = 0;
    std::wstring driver;
};
XefgSelfTestResult xefgSelfTest(uint64_t adapterLuid, bool warp, uint32_t multiplier, uint32_t width, uint32_t height, uint32_t frames);

// ---------------------------------------------------------------------------------------------------------- XeSS SR
// The provider for veyra::gfx::FsrSrBackend::setProviderOverride (an opaque pointer to its ProviderOverride struct).
// quality 1..4 = PushMPC's sharpening levels: Performance, Balanced, Quality, Ultra Quality (the XeSS preset whose input
// range holds the real scale; Native AA when the source is at the output size).
const void* srProvider(uint32_t quality);
// this build has FsrSrBackend's provider slot (PushFPS with patches/pushfps-4.5.4-xess.patch)
bool srProviderBuilt();
struct XessSrSelfTestResult {
    bool ran = false, passed = false;
    double psnrXess = -1, psnrBilinear = -1, msPerFrame = -1;
    std::string adapter, note, version;
    uint32_t vendor = 0, device = 0;
    std::wstring driver;
};
// XeSS SR self-test (unlock XessSr): 2x upscale of a synthetic panning scene with exact motion; passes when XeSS
// initialises and executes on this adapter, the output is not blank and its PSNR against the ground truth is at least
// that of a bilinear upscale minus 0.5 dB.
XessSrSelfTestResult xessSrSelfTest(uint64_t adapterLuid, bool warp, uint32_t width, uint32_t height, uint32_t frames);


// ---------------------------------------------------------------------------------------------------- capabilities
// PushCoreXessCaps.cpp: the hardware rule, the files, the unlock decisions (XessFg / XessSr; a missing self-test result is
// measured once per adapter + driver when runTests), the measured XeSS SR cost (4K output) for Auto
struct XessCaps {
    bool fgHw = false, fgOn = false;
    std::string fgWhy;            // the decision line (or why not)
    bool srHw = false, srOn = false;
    std::string srWhy;
    double srMs4k = -1;           // XeSS SR ms per frame for a 4K output (self-test), -1 = unknown
};
XessCaps capabilities(const DXGI_ADAPTER_DESC1& adapter, const std::wstring& driver, bool runTests);

}  // namespace pushmpc::xess
