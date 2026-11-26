#pragma once
// PushMPC: Intel VPL AI frame interpolation (PUSHCORE_FG_VPL) on the D3D11 bridge.
//
// Intel's oneVPL runtime (part of Intel's graphics driver) interpolates between consecutive frames with its AI frame
// interpolation VPP filter (mfxExtVPPAIFrameInterpolation; libvpl's MIT dispatcher is linked into pushcore and finds the
// driver's runtime). pushcore runs its graph with frame generation off; each real output frame (RGBA8 / RGB10A2 at the
// output size, after the upscaler) is copied into a VPL input surface on the bridge device, VPL returns the frames in
// between, they are copied into shared textures, the scene-cut guard's pair test decides on the D3D12 side whether the
// pair is a cut (then the pair's first real frame is held over them, as for every other method), and pushcore hands them
// out before the real frame with its own timestamps. A discontinuity (open, seek, flush) resets VPL.
// Video memory on D3D11: the runtime's own surfaces (MFXMemory_GetSurfaceForVPP / ForVPPOut), reached through
// GetNativeHandle. Formats: the graph's own (BGR4 = RGBA8, A2RGB10) when VPL takes it, else NV12 / P010 through the
// video processor.
#include "PushCoreD3D11Bridge.h"
#include <memory>
#include <string>

namespace pushmpc {

class VplFi {
public:
    VplFi();
    ~VplFi();
    VplFi(const VplFi&) = delete;
    VplFi& operator=(const VplFi&) = delete;
    static bool built();   // this pushcore links libvpl
    // A VPL session on the bridge's D3D11 device (the bridge's adapter must be the Intel GPU), frame interpolation of
    // w x h frames of `format` at the largest multiplier <= want the runtime accepts (Query), down to 2.
    bool init(D3D11Bridge& b, uint32_t w, uint32_t h, DXGI_FORMAT format, uint32_t wantMultiplier, double fps, std::string& why);
    void close();
    bool ready() const;
    uint32_t multiplier() const;
    std::string describe() const;
    // the next real frame (a bridge texture, w x h, `format`, ready on the bridge context). discontinuity = nothing between
    // the previous real frame and this one. outputs[0..multiplier-2] receive the frames between the previous real frame and
    // this one, in time order; produced = how many were written. CPU-synchronous (waits for VPL).
    bool push(ID3D11Texture2D* real, bool discontinuity, ID3D11Texture2D* const* outputs, uint32_t& produced, std::string& why);
    double lastMs() const;   // wall time of the last push (copy in, VPL, copies out)

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

struct VplSelfTest {
    bool ran = false;
    bool pass = false;    // works on this GPU / driver: frames come out, better than a 50/50 blend
    bool gate = false;    // default-on gate (T5b, moved onto this material): psnr >= blend + 5.47 dB (28.90 - 23.43 on the probe material)
                          // and <= 20.8 ms per source frame at 1080p 2x
    std::string reason;   // one line, with psnr= blend= ms= maxM=
    double psnr = -1, psnrBlend = -1, msPerSource = -1;
    uint32_t maxMultiplier = 0;
};
// Procedural held-out test (1080p, 30 -> 60: input = the even frames of a 60 fps pan/zoom over a textured pattern,
// generated frames scored against the dropped odd ones, PSNR on luma), on its own D3D11 device on the adapter (WARP when
// warp), in a worker thread: a VPL call that hangs past timeoutMs is abandoned and reported as a failure.
VplSelfTest vplFiSelfTest(uint64_t adapterLuid, bool warp, uint32_t timeoutMs);
// The same material's baselines without VPL (CI calibration of the gate): "blend X dB, repeat Y dB"
std::string vplMaterialBaselines(uint32_t w = 1920, uint32_t h = 1080);

}  // namespace pushmpc
