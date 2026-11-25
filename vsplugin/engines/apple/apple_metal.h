// pushmpc.Enhance, backend "apple": the Metal stages (macOS; apple_metal.mm). They work on the same IOSurface-backed
// CVPixelBuffers as VideoToolbox (zero copy through CVMetalTextureCache) and can share one command buffer with a
// VideoToolbox call (VTFrameProcessor processWithCommandBuffer:parameters:):
//   - ArtCNN (C4F16 / C4F32, MIT; weights from apple_nn.h's .pmnn): the luma doubler. 3x3 convolutions over
//     rgba16f texture arrays (4 channels a slice, float accumulation), the long skip, depth-to-space x2 and the clip
//     fused into the last pass, written straight into the output buffer's Y plane; chroma is doubled bilinearly.
//   - 10-bit recovery (apple_nn.h RecoverTenBitPlane, same formula): G10 = 4 G8 + LP(warped residuals of the two
//     10-bit sources), per plane, the pair's optical flow from VTOpticalFlow (or none).
//   - flow scaling: a VTOpticalFlow result scaled by a phase step (super resolution's flow between two output frames
//     of one pair).
// Buffers: bi-planar 4:2:0, '420v' / '420f' (8-bit) or 'x420' / 'xf20' (10 bits in the high bits of 16); flow:
// '2C0h' (two half floats, pixels).
#pragma once
#include <CoreVideo/CoreVideo.h>
#include <memory>
#include <string>
#include "apple_nn.h"

namespace pmvs::apple {

class Metal {
public:
    // the process-wide context (nullptr + err when this Mac has no Metal device or the kernels do not compile)
    static Metal* Shared(std::string& err);
    std::string DeviceName() const;
    struct Net;
    std::shared_ptr<Net> LoadNet(const NnModel& m, std::string& err);
    // a new command buffer (an id<MTLCommandBuffer>, retained: release with Commit)
    void* NewCommandBuffer();
    // commits (and, if wait, waits for) a command buffer from NewCommandBuffer, then releases it
    bool Commit(void* cmd, bool wait, std::string& err);
    // dst must be 2x src in both directions, same format family (8-bit or 16-bit)
    bool EncodeArtCnn(Net& net, void* cmd, CVPixelBufferRef src, CVPixelBufferRef dst, std::string& err);
    // a16, b16: the pair at 16 bits ('x420'); g8: the interpolator's 8-bit output; flow: forward flow '2C0h' (nullable);
    // out: 'x420', the size of g8
    bool EncodeRecover(void* cmd, CVPixelBufferRef a16, CVPixelBufferRef b16, CVPixelBufferRef g8, CVPixelBufferRef flow, float t, CVPixelBufferRef out, std::string& err);
    bool EncodeFlowScale(void* cmd, CVPixelBufferRef in, CVPixelBufferRef out, float scale, std::string& err);
    // test helper: the network on a float luma picture (w x h in [0, 1]) -> 2w x 2h, synchronously
    bool RunArtCnnFloat(Net& net, const float* in, int w, int h, float* out, std::string& err);
    ~Metal();
    struct Impl;
private:
    Metal();
    std::unique_ptr<Impl> m;
};

// where the ArtCNN weights live: $PUSHMPC_MODELS, then <this library's folder>/pushmpc-models, then
// ~/.config/pushmpc/models. name: "ArtCNN_C4F16" -> "<dir>/ArtCNN_C4F16.pmnn" ("" when not found)
std::string FindModel(const std::string& name);

} // namespace pmvs::apple
