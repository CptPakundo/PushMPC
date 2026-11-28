#pragma once
// PushMPC: Intel Video Super Resolution (PUSHCORE_SR_METHOD_IVSR) as pushcore's upscaler stage.
//
// Where it sits: on the D3D11 bridge (PushCoreD3D11Bridge), between the ingress and the graph. The source frame (NV12 /
// P010, copied into the ingress ring as always) goes through the driver's video processor with Intel's VPE extension
// set to super resolution, into an NV12 / P010 texture at the output size; the graph then takes that frame as its
// source with its own upscaler off, so colour, Neural Rendering, frame generation and the cut guard run at the output size as
// after any other upscaler. The bridge signals a fence the graph's queue waits on (no CPU wait).
//
// Self-test (unlock IntelVsr, cached in SelfTest\IntelVsr per vendor:device:driver): the extension calls return S_OK and
// a synthetic 960x540 -> 1920x1080 frame comes out not blank, at least as sharp as bilinear and within a PSNR floor of
// the truth (>= 22 dB and >= bilinear - 1 dB); its cost (1080p -> 4K blit) is measured and recorded.
#include "PushCoreD3D11Bridge.h"
#include <deque>
#include <string>
#include <vector>

namespace pushmpc {

struct IvsrSelfTest {
    bool ran = false, pass = false;
    std::string reason;                // one line: the verdict and the numbers
    double psnr = -1, psnrBilinear = -1, sharp = -1, sharpBilinear = -1, ms4k = -1;
};
// D3D11 only (no D3D12 device needed); the bridge must be initialised on the adapter to test
IvsrSelfTest intelVsrSelfTest(D3D11Bridge& b, bool requireExtension = true);
// the per-adapter cost recorded by the self-test ("ms4k=1.23" in its Reason), < 0 = unknown
double intelVsrCostFromReason(const std::wstring& reason);

class IntelVsrStage {
public:
    // in: the ingress format and size; out: the upscaled size. ring = textures handed to the graph (one per ingress slot).
    bool init(D3D11Bridge& b, uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH, DXGI_FORMAT format, bool bt709, bool limited,
              uint32_t ring, std::string& why);
    void reset();
    bool ready() const { return blit_.ready(); }
    bool superResolution() const { return srOn_; }
    // The source (a bridge-side view of an ingress slot) into ring slot `slot`: the bridge waits for `waitFence`/`waitValue`
    // (the caller's ingress copy) and for `freeFence`/`freeValue` (the graph consumed the slot), then blits and signals.
    // Returns the bridge fence value the graph waits for (0 = failed; why says it). A failing extension mid-stream turns
    // it off and keeps blitting with the plain video processor (never a black frame).
    uint64_t run(ID3D11Texture2D* source, uint32_t slot, ID3D11Fence* waitFence, uint64_t waitValue, ID3D11Fence* freeFence, uint64_t freeValue,
                 std::string& why);
    D3D11Bridge::Texture& slot(uint32_t i) { return ring_[i]; }
    D3D11Bridge::Fence11& fence() { return fence_; }
    // GPU ms of the recent blits (D3D11 timestamps), oldest first; drained by the caller
    std::vector<double> takeTimings();

private:
    D3D11Bridge* b_ = nullptr;
    VideoBlit blit_;
    std::vector<D3D11Bridge::Texture> ring_;
    D3D11Bridge::Fence11 fence_;
    bool srOn_ = false;
    struct Q { Microsoft::WRL::ComPtr<ID3D11Query> disjoint, t0, t1; };
    std::deque<Q> pending_;
    std::vector<Q> freeQ_;
    std::vector<double> done_;
};

}  // namespace pushmpc
