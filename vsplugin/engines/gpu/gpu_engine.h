// SPDX-License-Identifier: GPL-3.0-or-later
// pushmpc.Enhance, portable GPU engine: PushFPS's portable frame generation on any Vulkan 1.2 GPU (Linux: AMD, Intel,
// NVIDIA, Mesa; macOS through MoltenVK; Windows without pushcore), with NIS or FSR1 upscaling. Compute shaders in
// engines/gpu/shaders (SPIR-V embedded from engines/gpu/spv):
//   upload      the source's Y, U and V planes (8 or 16 bits) through a host-visible staging buffer into images; the
//               8-bit luma pyramid (pyr0, pyrdown);
//   dedup       PushFPS's duplicate test on the quarter-size luma;
//   motion      PushFPS's hierarchical block matching on the pyramid, both directions (me, mefinal), the scene-cut
//               counts and the cut guard's measures (mad, fbcheck);
//   generate    the planar YUV interpolation (interp), Subtitle Hold inside it;
//   upscale     NIS 1.0.3 or FSR1 EASU + RCAS on luma, bicubic chroma (nis, easu, rcas, pack);
//   readback    the shaders write the finished frame straight into a host-visible buffer (packed rows): the only
//               copies are the upload and the copy out into VapourSynth's frame.
// Submissions are ordered on one queue with a timeline semaphore; each output has its own ticket so the CPU copies
// one frame out while the GPU makes the next.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "portable_engine.h"
#include "subtitle_hold.h"
#include "veyra/portable/Motion.h"
#include "vk_context.h"

namespace pmvs::gpu {

using pmvs::portable::YuvOut;
using pmvs::portable::YuvView;

enum class Upscaler { None = 0, Nis = 1, Fsr = 2 };
inline const char* UpscalerName(Upscaler u) { return u == Upscaler::Nis ? "NIS" : u == Upscaler::Fsr ? "FSR1 (EASU + RCAS)" : "none"; }

struct Config {
    int width = 0, height = 0, bits = 8;
    int outWidth = 0, outHeight = 0;    // 0: the source's size (no upscaling)
    Upscaler upscaler = Upscaler::None;
    float sharpness = 0.5f;             // 0..1 (NIS's slider; FSR1: RCAS 2 x (1 - sharpness) stops)
    int quality = 1;                    // motion vectors from: 0 the quarter-size picture (32 px blocks), 1 the half-size
                                        // one (16 px, PushFPS's default; with the refined interpolation and the global
                                        // candidate it led on 4 of 6 benchmark clips and on average), 2 the full-size one
                                        // (8 px: ahead on fast pans and CG by 0.1-0.2 dB)
    int sweeps = 2;                     // passes per pyramid level (me.comp)
    bool extended = true;               // sweep 2 also tries its own, right and lower neighbours' vectors
    int levels = 0;                     // pyramid levels; 0: 4 up to 1200 lines (PushFPS), 5 above (the coarsest
                                        // search keeps covering a fast pan at 4K)
    int searchRange = 8;
    float smoothness = 3.0f;
    int sourceSlots = 8, outSlots = 10;
    bool blockMatcherOnly = true;       // false: NVIDIA's optical flow (VK_NV_optical_flow) where the driver has it. Off by
                                        // default: on an RTX 5090 it was slower (4K: 17.0 vs 6.8 ms a pair) and not better
                                        // (synthetic zoom: 37.85 vs 38.06 dB) than the block matcher at quality 2
    float errorLow = 24.f, errorHigh = 48.f;
    bool importable = false;            // source slots writable by Import (a decoder's GPU frames: Android hardware buffers)
    bool deviceReadback = false;        // outputs stay in device memory for Export (frames of an external device: no Read)
    // the packed output's layout: semiPlanarOut puts U and V interleaved (NV12 / P010); msbOut stores samples in the high
    // bits of 16 (P010). Both default off: planar, low bits (the VapourSynth and Kodi planar formats)
    bool semiPlanarOut = false, msbOut = false;
    bool refined = true;                // InterpolatePlanesRefined's rule (portable_engine.h); false: PushFPS's own
    bool regionRepair = false;          // the held-drawing repair (Cells, RepairSlot): source slots also storage images
    float boundaryPx = 1.0f;
    // the block matcher at quality 2 (8 px blocks on the full-size picture): search on a 3x3 binomial-filtered copy; double
    // the smoothness penalty at the full-size level; the equiangular sub-pixel fit; the quadtree merge to 16 px where
    // the finer detail is noise (mefinal.comp); the previous pair's global affine motion as one more candidate
    // Defaults from the CI ablation (6 clips, held-out frames): the global candidate helps zooms (+0.34 dB in_to_tree); the
    // prefilter cost 0.1-0.2 dB on water and CG; lambda scaling, the equiangular fit and the merge changed < 0.05 dB: off.
    bool prefilter = false, lambdaScale = false, equiangular = false, merge = false, globalCandidate = true;
    float mergeSpread = 0.75f, mergeGain = 1.0f;
    // NVIDIA optical flow (when not blockMatcherOnly): preset 0 slow, 1 medium, 2 fast; input 0 Auto (half size above 1440
    // lines), 1 full, 2 half; dense: the field at the flow's own 4x4-cell resolution; cost: cells above nvofMaxCost are
    // replaced by their neighbours' median; hybrid: the flow becomes one more candidate of the block matcher instead of
    // the field (the block matcher's SAD picks)
    int nvofPreset = 1, nvofInput = 0;
    bool nvofDense = true, nvofCost = true, nvofHybrid = false;
    float nvofMaxCost = 64.f;
};

struct PairStats {
    int blocks = 0;
    float unexplainedF = 0, unexplainedB = 0;   // share of blocks motion does not explain (PushFPS's cut measure)
    float meanCostF = 0, meanCostB = 0;         // matched error per pixel, 0..255
    float zeroCost = 0;                         // mean change without motion, 0..255
    float consistent = 0;                       // share of blocks whose forward and backward vectors cancel
    float moving = 0;                           // share of blocks moving >= 1 px
    bool cutRule = false;                       // PushFPS's rule (MotionField::sceneCut) in either direction
    double ms = 0;
};
struct DedupStats { float maxBlock = 0, mean = 0; bool duplicate = false; };

// ---- the seams for more motion sources and a learned synthesis stage ("flow once, synthesize many": e.g. splat to
// phase t + a small fusion CNN). Motion is estimated once per pair of real frames (Estimate), into one field format
// whatever the source; every output phase of the pair reuses it (Generate). A Synthesizer replaces interp.comp for
// generated frames; without one (the default) interp.comp runs. Cut, Dedup, Subtitle Hold, upscaling and readback stay
// the engine's.
enum class MotionSource { BlockMatcher = 0, NvOpticalFlow = 1 };   // later: D3D12 Video Motion Estimator (via interop), others
inline const char* MotionSourceName(MotionSource s) { return s == MotionSource::NvOpticalFlow ? "NVIDIA optical flow (VK_NV_optical_flow)" : "PushFPS's block matcher (GPU)"; }

// one pair's GPU data after Estimate (valid until ReleasePair). Images: R8 / R16 UNORM planes holding the source's code
// values (layout GENERAL). Fields: vec2 per block (blocksX x blocksY, row-major), full-size pixels, a point at p in A is at
// p + F(p) in B (K the other way). Cost: the matched error per block (0..255 per pixel), the confidence measure.
struct PairGpu {
    VkImageView aY = VK_NULL_HANDLE, aU = VK_NULL_HANDLE, aV = VK_NULL_HANDLE, bY = VK_NULL_HANDLE, bU = VK_NULL_HANDLE, bV = VK_NULL_HANDLE;
    VkBuffer fieldF = VK_NULL_HANDLE, fieldK = VK_NULL_HANDLE, costF = VK_NULL_HANDLE, costK = VK_NULL_HANDLE;
    int blocksX = 0, blocksY = 0, blockSize = 0, width = 0, height = 0, bits = 8;
};
// where a synthesized frame goes: without upscaling the packed readback slot (rows of 4 / 2 samples per 32-bit word,
// the pitches and plane offsets below, from `offset`), with upscaling the three R16 UNORM intermediate images (values
// normalised to 0..1 of the source's range) that the upscaler reads.
struct SynthTarget {
    bool packed = true;
    VkBuffer buffer = VK_NULL_HANDLE; VkDeviceSize offset = 0; int pitchY = 0, pitchC = 0; VkDeviceSize offU = 0, offV = 0;
    VkImageView midY = VK_NULL_HANDLE, midU = VK_NULL_HANDLE, midV = VK_NULL_HANDLE;
};
class Synthesizer {
public:
    virtual ~Synthesizer() = default;
    virtual const char* Name() const = 0;
    // record the generated frame at phase t into cb (the engine has put a full barrier before; it puts one after)
    virtual bool Record(VkCommandBuffer cb, const PairGpu& pair, float t, const SynthTarget& target, std::string& err) = 0;
};

// where an output frame lies in its buffer (packed rows): Y at 0, U at offU, V at offV; one output = stride bytes
struct OutputLayout { int pitchY = 0, pitchC = 0; uint64_t offU = 0, offV = 0, stride = 0; };
// ---- frames of an external device (FFmpeg's Vulkan frames in mpv): the caller records the copy in (Import) or out
// (Export) into the engine's command buffer, with the owner's timeline semaphores to wait for and to signal.
struct ExternalSync { std::vector<VkSemaphore> wait, signal; std::vector<uint64_t> waitValue, signalValue; };
// a source slot to fill: R8 / R16 UNORM planes (layout GENERAL, storage usage) holding the source's code values / maxRaw
struct SlotTarget { VkImage y = VK_NULL_HANDLE, u = VK_NULL_HANDLE, v = VK_NULL_HANDLE; VkImageView yv = VK_NULL_HANDLE, uv = VK_NULL_HANDLE, vv = VK_NULL_HANDLE; int w = 0, h = 0, cw = 0, ch = 0, bits = 8; };
// a finished output in device memory: the packed readback layout (rows of 4 / 2 samples per 32-bit word)
struct OutSource { VkBuffer buffer = VK_NULL_HANDLE; VkDeviceSize offset = 0, offU = 0, offV = 0; int pitchY = 0, pitchC = 0, w = 0, h = 0, cw = 0, ch = 0, bits = 8; };
using ImportRecorder = std::function<bool(VkCommandBuffer cb, const SlotTarget& t, std::string& err)>;
using ExportRecorder = std::function<bool(VkCommandBuffer cb, const OutSource& s, std::string& err)>;

class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    bool Configure(const Config& c, const DeviceRequest& req, std::string& err);
    // the same on a given context (Context::Adopt: an external device)
    bool Configure(const Config& c, std::shared_ptr<Context> ctx, std::string& err);
    // a source picture recorded by the caller into a free slot (and its pyramid); the slot index, or -1
    int Import(const ImportRecorder& rec, const ExternalSync& sync, std::string& err, uint64_t* ticket = nullptr);
    // an output (Generate / Real with deviceReadback) recorded by the caller into its frame; frees the readback slot
    bool Export(int out, const ExportRecorder& rec, const ExternalSync& sync, uint64_t& ticket, std::string& err);
    // a rectangle of an imported picture back to the host (Subtitle Hold's band): luma x, y, w, h (even), chroma at half;
    // planes packed (rows of w / w / 2 samples, source bits). Waits for the GPU.
    bool ReadRect(int slot, int x, int y, int w, int h, std::vector<uint8_t> (&planes)[3], std::string& err);
    const Config& Cfg() const;
    const DeviceInfo& Device() const;
    bool OpticalFlowActive() const;     // the motion comes from VK_NV_optical_flow
    MotionSource Motion() const { return OpticalFlowActive() ? MotionSource::NvOpticalFlow : MotionSource::BlockMatcher; }
    std::shared_ptr<Context> Ctx() const;
    bool PairResources(int pair, PairGpu& out) const;
    void SetSynthesizer(std::shared_ptr<Synthesizer> s);   // null: interp.comp (the default)
    int OutWidth() const;
    int OutHeight() const;

    // a source picture into a free slot (and its pyramid); the slot index, or -1
    int Upload(const YuvView& v, std::string& err);
    // PushFPS's duplicate test between two uploaded pictures (waits for the GPU)
    bool Duplicate(int prevSlot, int slot, DedupStats& st, std::string& err);
    void ReleaseSlot(int slot);
    int FreeSlots() const;

    // motion between two uploaded pictures, both directions (waits for the GPU: the cut decision needs its measures);
    // prevPair: the pair before (its fields are the temporal predictors), or -1. The pair's index, or -1.
    int Estimate(int slotA, int slotB, int prevPair, PairStats& st, std::string& err);
    // the same in two steps: Begin submits the work and returns the pair at once, End waits for it and gives the measures
    // (the backend starts a pair when its second frame arrives and ends it when its outputs are due)
    int EstimateBegin(int slotA, int slotB, int prevPair, std::string& err);
    bool EstimateEnd(int pair, PairStats& st, std::string& err);
    void ReleasePair(int pair);
    bool SetHold(int pair, const HoldPairYuv* hold, std::string& err);

    // an output frame into a free readback slot. mode 0: generated at phase t; 1 / 2: the pair's real frame A / B
    // (a cut's nearer frame). Real: an uploaded picture as it is (through the upscaler).
    bool Generate(int pair, float t, int mode, int& out, uint64_t& ticket, std::string& err);
    bool Real(int slot, int& out, uint64_t& ticket, std::string& err);
    int FreeOuts() const;
    bool Done(uint64_t ticket) const;
    bool Wait(uint64_t ticket, std::string& err) const;
    // after Wait: the frame into the caller's planes (the output size); the readback slot is free again
    void Read(int out, const YuvOut& o);
    void ReleaseOut(int out);

    // zero copy (Linux): a player's dma-buf frame buffer imported as an output target laid out as Layout() says; the
    // frame is written straight into it (no readback, no copy). ImportOutput returns an id, or -1 with the reason.
    bool DmaBufImport() const;
    OutputLayout Layout() const;
    int ImportOutput(int fd, uint64_t size, std::string& err);
    // the same for a host memory range (VK_EXT_external_memory_host): ptr and size aligned to HostImportAlignment()
    uint64_t HostImportAlignment() const;   // 0: not supported
    int ImportHostOutput(void* ptr, uint64_t size, std::string& err);
    void ReleaseOutput(int id);
    bool GenerateInto(int pair, float t, int mode, int output, uint64_t& ticket, std::string& err);
    bool RealInto(int slot, int output, uint64_t& ticket, std::string& err);
    // ---- zero-copy hosts (VA-API dmabufs imported in VLC): the host records its own commands into the engine's command
    // buffer, ordered on the engine's timeline. UploadWith: rec writes the source into `planar` (device memory, Upload's
    // staging layout: rows of width x bytes per sample, packed; U at offU, V at offV, all 4-byte aligned), then the
    // engine takes it as Upload does (slot, pyramid). GenerateWith: Generate, then rec reads the frame from its packed
    // readback slot (as Read sees it) in the same submission; the caller frees the slot with ReleaseOut after Wait.
    struct PlanarDesc { VkBuffer buffer = VK_NULL_HANDLE; VkDeviceSize offU = 0, offV = 0; int width = 0, height = 0, cw = 0, ch = 0, bps = 1; };
    struct PackedDesc { VkBuffer buffer = VK_NULL_HANDLE; VkDeviceSize offset = 0, offU = 0, offV = 0; int pitchY = 0, pitchC = 0, width = 0, height = 0, cw = 0, ch = 0, bps = 1; };
    int UploadWith(const std::function<bool(VkCommandBuffer, const PlanarDesc&, std::string&)>& rec, uint64_t& ticket, std::string& err);
    bool GenerateWith(int pair, float t, int mode, const std::function<bool(VkCommandBuffer, const PackedDesc&, std::string&)>& rec,
                      int& out, uint64_t& ticket, std::string& err);

    // ---- the held-drawing repair (pushmpc/src/HeldFrames.h on the portable routes; Config::regionRepair) ----
    // Cells: the cell grid of an uploaded picture (f x f luma cells, f = width / 480, at least 2: means in 8-bit codes,
    // and per cell the packed sub-cell change against prevSlot (-1: none) and Cb / Cr; CellGrid::unpackAux). Waits.
    // RepairSlot: each part's pair (slotA = its pair start, slotB = its next drawing) generated at its phase t at the
    // source size, laid into `slot` inside the part's cell mask (feathered, camera residual shiftX / Y in pixels, detail
    // matching on luma), then the slot's pyramid again. Up to 3 parts, disjoint masks. Waits.
    struct CellGridOut { int w = 0, h = 0, cell = 0; std::vector<float> v; std::vector<uint32_t> aux; };
    struct RepairPart { int slotA = -1, slotB = -1; float t = 0.5f, shiftX = 0, shiftY = 0; const std::vector<uint8_t>* mask = nullptr; };
    bool RepairAvailable() const;       // regionRepair set and this build has the shaders
    bool Cells(int slot, int prevSlot, CellGridOut& out, std::string& err);
    bool RepairSlot(int slot, const RepairPart* parts, int n, float featherCells, bool detail, std::string& err);
    // ContentDetect's GPU tap (tap.comp, the words pushmpc/content_detect.h PackedCellsRef makes on the CPU): the packed cells
    // of an uploaded picture, kCellsW (480) x CellsHeight words; with clip also the image model's 224 x 224 thumbnail (224 x
    // 224 more words after them). Waits. False with no tap shader in this build (TapAvailable).
    bool TapAvailable() const;
    bool Tap(int slot, bool clip, std::vector<uint32_t>& words, int& cw, int& ch, std::string& err);

    // gpu_test: a pair's fields from / to the CPU reference's MotionField
    bool InjectFields(int pair, const veyra::portable::MotionField& f, const veyra::portable::MotionField& k, std::string& err);
    bool ReadFields(int pair, veyra::portable::MotionField& f, veyra::portable::MotionField& k, std::string& err);

    struct Timing { double uploadMs = 0, meMs = 0, waitMs = 0; uint64_t uploads = 0, pairs = 0, outputs = 0; };
    const Timing& Times() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace pmvs::gpu
