// pushmpc.Enhance, backend "apple": frame interpolation and super resolution with Apple's VideoToolbox ML effects
// (VTFrameProcessor) on Apple silicon. C++ interface; the implementation is Objective-C++ (apple_engine.mm, macOS only).
//
// What runs, per machine (Probe() decides at run time; nothing is assumed from the OS version alone):
//   interpolation   VTLowLatencyFrameInterpolation (macOS 26+; "for real-time processing") gives 2^x - 1 phases
//                   per call (x = 1: 0.5, x = 2: quarters): x2 and x4 are native (it lists '420v' only). x3, x5..x7
//                   use VTFrameRateConversion (macOS 15.4+; any phase, editing grade, quality "normal"; it lists RGhA
//                   only, so frames go YUV -> RGhA -> YUV through VTPixelTransferSession). x8 tries the low-latency
//                   eighth grid (x = 3, an unlock macOS 26 accepts) first. Last resort: the low-latency grid with
//                   phases rounded to it (apple_convert.h MakePlan). Each candidate must pass its self-test.
//   self-test       Configure runs each effect once on synthetic frames: a Mac (or VM) that lists an effect but cannot
//                   run it makes Configure fail, and the plugin falls back to the portable backend.
//   super res.      applied to every output frame after interpolation, in presentation order. Candidates, best first
//                   (apple_convert.h DecideSrOrder; each must pass a self-test and a real-time benchmark in Configure):
//                   VTSuperResolutionScaler (macOS 26, editing grade, temporal, up to 1920x1080 video; it takes the
//                   pair's VTOpticalFlow, computed once per pair and shared with frame-rate conversion and the 10-bit
//                   recovery; one-time model download through Apple's API, started in the background), ArtCNN C4F32 /
//                   C4F16 on Metal (apple_metal.h; first for anime), VTLowLatencySuperResolutionScaler (x2 / x4,
//                   small sizes).
//   10-bit          when the low-latency interpolator lists only 8-bit formats and the source has more bits, the
//                   generated frames get their 2 lost bits back on Metal (apple_nn.h RecoverTenBitPlane), in the same
//                   command buffer as the VideoToolbox call. Frame-rate conversion (half float) stays the exact path.
//   denoise         VTTemporalNoiseFilter (macOS 26; CleanUp = On, off by default: apple_convert.h DecideDenoise) on each
//                   source frame before everything else, with up to the configuration's previousFrameCount earlier
//                   source frames as references (no future frames: every route is one frame in, one out). The real
//                   frames then come back denoised too. It must pass a self-test; if not, the chain runs without it.
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
    bool superResolution = false;  // VTSuperResolutionScalerConfiguration.isSupported (macOS 26+; editing grade)
    bool opticalFlow = false;      // VTOpticalFlowConfiguration.isSupported (macOS 15.4+)
    bool noiseFilter = false;      // VTTemporalNoiseFilterConfiguration.isSupported (macOS 26+)
    bool commandBuffer = false;    // VTFrameProcessor processWithCommandBuffer:parameters: exists
    std::string metal;             // the Metal device ("" = none)
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
    bool anime = false;            // content hint for Auto's upscaler order (ArtCNN first for anime)
    std::string modelDownload = "auto";   // the editing-grade super-resolution model: auto (background download) | never
    std::string tenBit = "auto";   // 10-bit recovery for an 8-bit-only interpolator: auto | off
    std::string denoise = "auto";  // the temporal noise filter (CleanUp): auto (= off) | off | on
    std::string denoiseStrength;   // CleanUpStrength: subtle | balanced | strong | 0..1 ("" = balanced)
};
struct Stats {
    uint64_t pairs = 0, cuts = 0, generated = 0, vtCalls = 0, vtErrors = 0, fiErrors = 0, srErrors = 0, flowCalls = 0, flowErrors = 0, recovered = 0;
    double pairMs = 0; int ladder = 0;
    double benchFiMs = 0, benchSrMs = 0, benchFlowMs = 0;   // Configure's measurements (per call)
    uint64_t tnfFrames = 0, tnfErrors = 0, tnfPassed = 0;    // denoised, failed, passed through (no reference yet)
    double benchTnfMs = 0;
};
// the editing-grade super-resolution model (Apple's one-time download). status: -1 not available here, 0 download
// required, 1 downloading, 2 ready. Download: starts it (asynchronous) and returns at once; wait > 0 waits up to that
// many seconds, printing progress through log.
int SrModelStatus(int width = 1920, int height = 1080, float* percent = nullptr);
bool SrModelDownload(double waitSeconds, const std::function<void(const std::string&)>& log, std::string& err);

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
    // the same on CVPixelBuffers (the IINA bridge: VideoToolbox-decoded frames, zero copy when the format matches).
    // gen / cur receive retained buffers (CVPixelBufferRelease them); cur is the input itself when nothing changes it
    bool ProcessBuffer(void* pixelBuffer, const Colour& colour, int64_t pts, bool contiguous, std::vector<void*>& gen, void*& cur, bool& havePair, std::string& err);
    void Forget();                 // a seek
    // held frames: the NEXT Process / ProcessBuffer pair spans g source intervals (the frames between were repeats of
    // its first frame): it makes g k - 1 frames at phases j / (g k) (gen must have room for them). Frame-rate
    // conversion takes any phase; the low-latency grid rounds them (MakePlan). Back to 1 after that call.
    void SetSpan(int g);
    bool LastPairWasCut() const;
    bool Denoising() const;        // the temporal noise filter is in the chain (Configure kept it)
    const Stats& Stat() const;
    std::string Describe() const;  // the active chain, the ladder level
    std::function<void(const std::string&)> log;   // ladder steps, VideoToolbox errors (optional)
    struct Impl;                   // (public only so the implementation's helpers can name it)
private:
    std::unique_ptr<Impl> m;
};

} // namespace pmvs::apple
