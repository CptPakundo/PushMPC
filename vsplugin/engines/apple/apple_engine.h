// pushmpc.Enhance, backend "apple": frame interpolation and super resolution with Apple's VideoToolbox ML effects
// (VTFrameProcessor) on Apple silicon. C++ interface; the implementation is Objective-C++ (apple_engine.mm, macOS only).
//
// What runs, per machine (Probe() decides at run time; nothing is assumed from the OS version alone):
//   interpolation   VTLowLatencyFrameInterpolation (macOS 26+; "for real-time processing"): k - 1 frames per source
//                   pair from one call, on its 2^x phase grid (apple_convert.h MakePlan); it lists '420v' only. Else
//                   VTFrameRateConversion (macOS 15.4+; any phase, editing grade, quality "normal" so it has a chance
//                   to keep up); it lists RGhA only, so frames go YUV -> RGhA -> YUV through VTPixelTransferSession.
//   self-test       Configure runs each effect once on synthetic frames: a Mac (or VM) that lists an effect but cannot
//                   run it makes Configure fail, and the plugin falls back to the portable backend.
//   super res.      VTLowLatencySuperResolutionScaler (macOS 26+), factor 2 (or 4 when asked), applied to every output
//                   frame after interpolation (interpolating at the source size is cheaper, and the low-latency
//                   interpolator's size limit is the tighter one).
//   cuts            our thumbnail guard (apple_convert.h): a cut pair is never interpolated.
//   real time       apple_convert.h Ladder: steps down (super resolution -> plain VTPixelTransfer scale, then fewer
//                   phases, then none) when a pair keeps taking longer than the source frame interval.
// Frames: VapourSynth planar YUV 4:2:0 8..16-bit in, the same format out (at the super-resolution size). Uploaded
// into IOSurface-backed CVPixelBuffers from pools ('420v'/'420f' or 'x420'/'xf20', whichever the processor lists),
// tagged with the source's matrix, transfer and primaries so HDR (PQ, HLG) and the range pass through untouched.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "apple_convert.h"

namespace pmvs::apple {

// what this Mac offers (Probe() runs once per process; the per-size checks run in Configure)
struct Caps {
    bool built = false;            // compiled with VideoToolbox frame processing (macOS builds)
    bool frameProcessor = false;   // the VTFrameProcessor class exists (macOS 15.4+)
    bool lowLatencyFI = false;     // VTLowLatencyFrameInterpolationConfiguration.isSupported (macOS 26+)
    bool lowLatencySR = false;     // VTLowLatencySuperResolutionScalerConfiguration.isSupported (macOS 26+)
    bool frc = false;              // VTFrameRateConversionConfiguration.isSupported (macOS 15.4+)
    bool superResolution = false;  // VTSuperResolutionScalerConfiguration.isSupported (macOS 26+; editing grade, not used yet)
    bool sizeQueries = false;      // maximumDimensionForSpatialScaleFactor: exists (macOS 27+)
    std::string os, chip, sdk;     // "macOS 26.1 (25B78)", "Apple M4 Pro", the SDK the build used
    std::string detail;            // one line per item, for logs and the CI probe
    bool AnyInterpolation() const { return lowLatencyFI || frc; }
};
const Caps& Probe();
// a longer report for the CI probe: per configuration, the pixel formats, scale factors and size limits it lists for a
// few common sizes
std::string ProbeReport();

struct Colour { int matrix = 1, transfer = 1, primaries = 1; bool fullRange = false; };   // VapourSynth codes

struct Config {
    int width = 0, height = 0, bits = 8;
    int k = 2;                     // output frames per source frame (1 = super resolution only)
    std::string sr = "auto";       // super resolution: auto | off | 2 | 4
    std::string fi = "auto";       // interpolator: auto (low latency, else FRC) | lowlatency | frc
    double srcFps = 23.976;
    Colour colour;                 // a guess until the first frame says otherwise
};
struct Stats { uint64_t pairs = 0, cuts = 0, generated = 0, vtCalls = 0, vtErrors = 0, fiErrors = 0, srErrors = 0; double pairMs = 0; int ladder = 0; };

class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    // false + err when this Mac cannot run the configuration (the plugin then falls back to the portable backend);
    // info: what was chosen and why
    bool Configure(const Config& c, std::string& err, std::string& info);
    int OutWidth() const;
    int OutHeight() const;
    int K() const;
    // one source frame (the view is only read during the call). contiguous: it follows the previous one. Then, for a
    // pair, the k - 1 generated frames into gen (k - 1 outputs at the output size), and the real frame into cur.
    // pts: the source frame's time in 100 ns units (only has to increase)
    bool Process(const PlanarView& v, const Colour& colour, int64_t pts, bool contiguous, const std::vector<PlanarOut>& gen, const PlanarOut& cur,
                 bool& havePair, std::string& err);
    void Forget();                 // a seek
    bool LastPairWasCut() const;
    const Stats& Stat() const;
    std::string Describe() const;  // the active chain, the ladder level
    std::function<void(const std::string&)> log;   // ladder steps, VideoToolbox errors (optional)
private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace pmvs::apple
