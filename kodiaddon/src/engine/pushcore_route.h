// pushcore_route: PushMPC's Windows engine (pushcore: NVIDIA VFG / DLSS-G, PushFlow, FSR FG, AMD FRC and PushFPS's
// scene-cut guard) inside Kodi, through the VLC module's bridge pm_pushcore.dll (vlcplugin/pushcore_bridge), which ships
// in the add-on folder. pushcore.dll comes from the PushMPC install (%LOCALAPPDATA%\PushMPC\core).
//
// Pipeline (one copy, Kodi's add-on API has no texture handle):
//   FFmpeg D3D11VA decoding on a D3D11 device of ours -> pushcore on that same device (pm_pc_open_d3d11: it reads the
//   decoder's texture slices) -> generated frames as NV12 / P010 texture slices of our ring (the bridge converts them
//   on the GPU) -> one readback of each picture into Kodi's system-memory frame buffer (NV12 / P010 on Kodi 22, the DXVA
//   renderer's copy-only formats; planar on Kodi 21).
// Software-decoded frames are uploaded into a ring slice first. Real frames are the decoder's own (a GPU copy).
//
// Why a device of ours and not Kodi's own (kodi::gui::GetHWContext): Kodi renders on that device's immediate context
// from its render thread and exposes no lock for it, so pushcore's and the bridge's pipeline work would race Kodi's
// draws; and Kodi takes no textures from an add-on anyway (the videocodec API's D3D11 handle type is reserved). The
// device is made on the high-performance GPU (on a laptop that is the discrete one, where the RTX features are, even
// when Kodi itself runs on the integrated GPU); Kodi's adapter is logged next to it.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "pm_engine.h"

struct AVBufferRef;

namespace pmk {

struct PcConfig {
    int width = 0, height = 0, bits = 8;
    int fpsNum = 24000, fpsDen = 1001;
    int matrix = 1, transfer = 1, primaries = 1, range = 1;   // H.273 codes; range 1 limited, 2 full
    int smoother = 0;                                         // 0 Auto (PushMPC's own setting), 1 off, 2..16
    double displayHz = 0;
    bool warp = false;                                        // WARP adapter (CI with the stub core)
};

class PushcoreRoute {
public:
    PushcoreRoute();
    ~PushcoreRoute();
    PushcoreRoute(const PushcoreRoute&) = delete;
    PushcoreRoute& operator=(const PushcoreRoute&) = delete;

    // false with the reason: no pm_pushcore.dll, no pushcore install, no device, pushcore refused the stream
    bool Open(const PcConfig& cfg, std::string& why);
    int K() const { return m_k; }
    const std::string& Describe() const { return m_desc; }
    bool TenBit() const { return m_bits > 8; }
    // the FFmpeg D3D11VA device on our device (a new reference), so decoded frames are already on pushcore's device
    AVBufferRef* HwDevice() const;

    // a decoded frame: a D3D11 texture slice (AV_PIX_FMT_D3D11) or planar 4:2:0 planes (8-bit, or 10-bit in 16-bit
    // words). The frame is stored in a ring slice (its real picture, returned) and submitted to pushcore.
    int PushTexture(void* texture, unsigned slice, int64_t ptsUs, bool contiguous, std::string& err);
    int PushPlanes(const pm_yuv& src, int64_t ptsUs, bool contiguous, std::string& err);
    // the generated frames of the pair that ended with the last push, wanted at these timestamps (us): the ring slice
    // of each, or -1 for one pushcore did not deliver within the wait (the caller shows the real frame there)
    void Collect(const std::vector<int64_t>& wantUs, double frameUs, std::vector<int>& slots);
    // a ring slice into Kodi's buffer: semi = NV12 / P010 (Y plane, interleaved UV in dst[1]); else planar (10-bit in
    // the low bits of 16-bit words)
    bool Read(int slot, uint8_t* const dst[3], const int stride[3], bool semi, std::string& err);
    void Flush();
    uint64_t Delivered() const { return m_delivered; }
    uint64_t Missed() const { return m_missed; }

    struct Impl;
private:
    Impl* m = nullptr;
    int m_k = 1, m_bits = 8, m_w = 0, m_h = 0;
    std::string m_desc;
    uint64_t m_delivered = 0, m_missed = 0;
};

// is pm_pushcore.dll next to the add-on and pushcore installed (a cheap check for the settings / log)
bool PushcoreAvailable(std::string& why);
// the GPU behind a D3D11 device context (Kodi's, from kodi::gui::GetHWContext), for the log
std::string AdapterOfContext(void* d3d11Context);

} // namespace pmk
