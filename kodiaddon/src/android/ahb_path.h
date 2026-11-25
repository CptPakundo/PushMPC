// ahb_path: Android hardware decoding with zero copy into the GPU engine.
//   AMediaCodec (NDK, no Java VM) decodes into an AImageReader's surface; each frame is an AHardwareBuffer, imported
//   into Vulkan with its external format and a YCbCr conversion sampler; a compute pass (ahb2yuv.comp) writes it into
//   the engine's source slot. No CPU copy between the decoder and the engine.
// Fallbacks (in the add-on): FFmpeg's MediaCodec decoders (NDK mode, frames to the CPU), then software decoding.
// Built only for Android with PM_AHB (the shader compiled by glslang at build time).
#pragma once
#if defined(__ANDROID__) && defined(PM_AHB)
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct AMediaCodec;
struct AImageReader;
struct AImage;
struct AHardwareBuffer;
struct AVBSFContext;
struct AVCodecParameters;
namespace pmvs::gpu { class Engine; }

namespace pmk {

class AhbDecoder {
public:
    ~AhbDecoder();
    // codecpar: the demuxer's stream (H.264, HEVC, VP9, AV1); false with the reason when this device cannot
    bool Open(const AVCodecParameters* par, std::string& why);
    // one packet (pts in microseconds); false: the decoder has no input buffer free, offer it again later
    bool Send(const uint8_t* data, int size, int64_t pts);
    void SendEnd();   // drain
    // the next decoded frame, if one is ready: its hardware buffer (valid until Release) and pts in microseconds
    bool Receive(AHardwareBuffer*& buf, int64_t& pts, bool& eof);
    void Release();   // the frame from Receive is no longer used (the GPU finished with it)
    void Flush();
    int Width() const { return m_w; }
    int Height() const { return m_h; }
    int Bits() const { return m_bits; }

private:
    void Pump();
    AMediaCodec* m_codec = nullptr;
    AImageReader* m_reader = nullptr;
    AImage* m_held = nullptr;
    AVBSFContext* m_bsf = nullptr;
    int m_w = 0, m_h = 0, m_bits = 8;
    bool m_eosIn = false, m_eosOut = false;
};

// imports AHardwareBuffers into the engine's source slots (on the engine's Vulkan device)
class AhbImporter {
public:
    ~AhbImporter();
    // fill a free source slot of the engine from buf; the slot, or -1 with the reason. Waits for the GPU (so the
    // decoder's buffer can go back right after).
    int Fill(pmvs::gpu::Engine& e, AHardwareBuffer* buf, std::string& err);

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace pmk
#endif
