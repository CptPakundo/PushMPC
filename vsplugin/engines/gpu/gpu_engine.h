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
    int quality = 2;                    // motion vectors from: 0 the quarter-size picture (32 px blocks), 1 the half-size
                                        // one (16 px, PushFPS's default), 2 the full-size one (8 px)
    int sweeps = 2;                     // passes per pyramid level (me.comp)
    bool extended = true;               // sweep 2 also tries its own, right and lower neighbours' vectors
    int levels = 0;                     // pyramid levels; 0: 4 up to 1200 lines (PushFPS), 5 above (the coarsest
                                        // search keeps covering a fast pan at 4K)
    int searchRange = 8;
    float smoothness = 3.0f;
    int sourceSlots = 6, outSlots = 10;
    bool blockMatcherOnly = false;      // never NVIDIA's optical flow (else it is the motion source where the driver has it)
    float errorLow = 24.f, errorHigh = 48.f;
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

class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    bool Configure(const Config& c, const DeviceRequest& req, std::string& err);
    const Config& Cfg() const;
    const DeviceInfo& Device() const;
    bool OpticalFlowActive() const;     // the motion comes from VK_NV_optical_flow
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
