// pushmpc.Enhance, backend "portable": frame generation on the CPU with PushFPS's portable core (GPL-3; copied into
// third_party/pushfps-portable). No GPU, no vendor SDK: Windows (AMD, Intel, any GPU), Linux and macOS.
//
// Per source pair (s - 1, s):
//   - motion both ways with PushFPS's hierarchical block matcher (veyra::portable::MotionEstimator) on an 8-bit luma
//     pyramid (the source's Y plane, its top 8 bits), forward and backward in parallel; the last fields are the temporal
//     predictors, as in PushFPS;
//   - the scene-cut rule is PushFPS's own (MotionField::sceneCut: most blocks changed and motion explains little of it):
//     a cut pair generates no blend, every generated frame is the nearer real frame;
//   - the generated frames at t = j / k: a planar YUV 4:2:0 port of PushFPS's Interpolate.cpp (two candidates F and -K,
//     weights 1/(e+4)^2, the occlusion fallback to the nearer frame's sample), at the source's bit depth, so 10-bit and
//     HDR sources stay as they are. The candidates' error is taken from luma (PushFPS takes the mean of R, G, B; for a
//     grey picture the two are identical, which portable_test checks bit for bit against the core).
// The output is the source's own format (YUV 4:2:0, same bits, same colour): real frames are copied unchanged.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "veyra/portable/Image.h"
#include "veyra/portable/Interpolate.h"
#include "veyra/portable/Motion.h"

namespace pmvs::portable {

// a planar YUV 4:2:0 picture as VapourSynth holds it (bits 8: 1 byte per sample; 9..16: 2 bytes)
struct YuvView { const uint8_t* data[3] = {}; ptrdiff_t stride[3] = {}; int width = 0, height = 0, bits = 8; };
struct YuvOut { uint8_t* data[3] = {}; ptrdiff_t stride[3] = {}; };

struct Config {
    int width = 0, height = 0, bits = 8;
    int threads = 0;   // interpolation threads; 0 = hardware threads (at most 16)
    int quality = 1;   // 0 fast (vectors from the quarter-size picture), 1 balanced (half size), as PushFPS
};
struct Stats { uint64_t pushed = 0, pairs = 0, cuts = 0, generated = 0; double meMs = 0, genMs = 0; };   // ms: running means

// one plane, 16-bit samples, rows packed
struct Plane { int w = 0, h = 0; std::vector<uint16_t> px; };
struct Picture { Plane p[3]; std::shared_ptr<const veyra::portable::Pyramid> pyramid; };

class Engine {
public:
    bool Configure(const Config& c, std::string& err);
    const Config& Cfg() const { return m_cfg; }
    int Threads() const { return m_threads; }
    // a new source frame; contiguous = it follows the previous one (else the pair starts over)
    bool Push(const YuvView& v, bool contiguous, std::string& err);
    void Forget();                                   // a seek: no pair until two frames are pushed again
    bool HavePair() const { return m_havePair; }
    bool PairIsCut() const { return m_cut; }
    const veyra::portable::MotionField& Forward() const { return m_fwd; }
    const veyra::portable::MotionField& Backward() const { return m_bwd; }
    // the generated frame at phase t (0 = previous, 1 = current) of the current pair; the nearer real frame on a cut
    void Generate(float t, const YuvOut& out);
    void Current(const YuvOut& out) const;          // the current real frame, unchanged
    const Stats& Stat() const { return m_stats; }

private:
    void Write(const Picture& pic, const YuvOut& out) const;
    Config m_cfg;
    int m_threads = 1;
    veyra::portable::MotionSettings m_ms;
    Picture m_prev, m_cur;
    bool m_haveCur = false, m_havePair = false, m_cut = false;
    veyra::portable::MotionField m_fwd, m_bwd;
    Stats m_stats;
};

// the planar interpolation on its own (portable_test compares it with veyra::portable::interpolate)
// maxValue: (1 << bits) - 1; errors are scaled to 0..255 so PushFPS's thresholds apply at every bit depth
void InterpolatePlanes(const Picture& a, const Picture& b, const veyra::portable::MotionField& fwd, const veyra::portable::MotionField& bwd,
                       float t, int maxValue, Picture& out, int threads, veyra::portable::InterpolateSettings s = {});

// The refined rule (the GPU engine's default; this is its CPU reference, gpu_test compares the two):
//  - occlusion is decided geometrically: the fallback to the nearer frame's sample is weighted by how much the
//    forward and backward fields disagree at the pixel (1 - consistency), not by the photometric error alone;
//    where they agree and the error is still high (water, fire, grain: texture motion cannot predict) the two
//    motion-compensated samples are averaged, which is the lower-error choice for unpredictable texture;
//  - each candidate's error is the mean over a 3x3 patch (no pixel-to-pixel switching on texture);
//  - at motion boundaries (the field changes by more than boundaryPx within one block) the forward vectors of the
//    four neighbouring blocks are tried too, and the best by patch error replaces the worse candidate.
struct RefineOptions { float boundaryPx = 1.0f; };
// per block of a field: 1 when it and the opposite field cancel (|f + k(p + f)| = 0), falling to 0 at 1.5 px + 5 %
std::vector<float> Consistency(const veyra::portable::MotionField& f, const veyra::portable::MotionField& k);
void InterpolatePlanesRefined(const Picture& a, const Picture& b, const veyra::portable::MotionField& fwd, const veyra::portable::MotionField& bwd,
                              float t, int maxValue, Picture& out, int threads, veyra::portable::InterpolateSettings s = {}, RefineOptions o = {});

// one motion estimation (both ways) and one generated frame on a synthetic picture of this size: the CPU budget Auto uses
struct Calibration { double meMs = 0, genMs = 0; int threads = 1; };
Calibration Calibrate(int width, int height, int bits, int threads, int quality);

} // namespace pmvs::portable
