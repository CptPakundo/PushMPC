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
// With the bridge's ABI 3 (pm_pushcore.h) two more modes:
//   - sr: pushcore's own upscaler to a fixed output size (Auto's anime upscaler next to Video Super Resolution,
//     win_anime_route.h); every picture, real ones too, then comes from pushcore at that size, and the decoded frames
//     are kept at the source size only to stand in (upscaled on the CPU) for one pushcore did not deliver;
//   - drawn: Auto's drawn treatment from the measured class (held-frame re-timing, the animation mode, line art, sign
//     hold, the anime deband preset); pushcore then hands a pair's frames out up to a dozen source frames later, so
//     the decoder keeps the pictures pending (CollectInto) instead of waiting for each pair.
//
// Why a device of ours and not Kodi's own (kodi::gui::GetHWContext): Kodi renders on that device's immediate context
// from its render thread and exposes no lock for it, so pushcore's and the bridge's pipeline work would race Kodi's
// draws; and Kodi takes no textures from an add-on anyway (the videocodec API's D3D11 handle type is reserved). The
// device is made on the high-performance GPU (on a laptop that is the discrete one, where the RTX features are, even
// when Kodi itself runs on the integrated GPU); Kodi's adapter is logged next to it.
#pragma once
#include <cstdint>
#include <functional>
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
    // ABI 3 (an older bridge: ignored, the source size)
    bool sr = false;                                          // pushcore's upscaler to outW x outH (the screen fit)
    int outW = 0, outH = 0;
    int animeModel = -1;                                      // while sr: PUSHCORE_ANIME_SR_* (the caller's switch), -1 none
    bool drawn = false;                                       // Auto's drawn treatment (held frames, holds, anime deband)
    int contentClass = 0; bool contentCg = false;             // ContentDetect's measurement so far
};

class PushcoreRoute {
public:
    PushcoreRoute();
    ~PushcoreRoute();
    PushcoreRoute(const PushcoreRoute&) = delete;
    PushcoreRoute& operator=(const PushcoreRoute&) = delete;

    // false with the reason: no pm_pushcore.dll, no pushcore install, no device, pushcore refused the stream, or x1
    // without its upscaler (nothing for pushcore to do)
    bool Open(const PcConfig& cfg, std::string& why);
    int K() const { return m_k; }
    const std::string& Describe() const { return m_desc; }
    bool TenBit() const { return m_bits > 8; }
    // pushcore's own upscaler at a fixed output size with its SR method switchable mid-stream (Auto's anime upscaler
    // next to Video Super Resolution, win_anime_route.h): with the bridge's ABI 3 (pm_pc_update)
    bool Upscales() const { return m_sr; }
    int OutWidth() const { return m_ow; }
    int OutHeight() const { return m_oh; }
    // every picture comes from pushcore (sr, or the drawn treatment)
    bool AllOut() const { return m_allOut; }
    // pushcore re-times held frames: a pair's frames may come up to Lag() source frames late
    bool Held() const { return m_held; }
    int Lag() const { return m_held ? 14 : 0; }
    int AnimeModel() const { return m_anime; }
    // PUSHCORE_SR_METHOD_* pushcore's upscaler runs (with ABI 3; meaningful while Upscales())
    unsigned SrMethod() const { return m_srMethod; }
    // new measured content / anime model for the running stream (pm_pc_update: setSettings + reload, warm); false with
    // the reason when the bridge refused or cannot (the open settings keep running)
    // drawn: Auto's drawn treatment (held frames, the anime holds and deband) on or off; turning it on also makes every
    // picture pushcore's (AllOut, kept from then on) and grows the ring for the held pictures
    bool Update(int contentClass, bool contentCg, int animeModel, bool drawn, std::string& why);
    bool Drawn() const { return m_cfg.drawn; }
    // textures in the route's rings (output and decoded); tests
    size_t RingSize() const;
    static bool BridgeHasAbi3();
    // runs fn(ID3D11Device*, ID3D11DeviceContext*) of the route's device with its context lock held (the decoder's own
    // GPU work there, e.g. ContentDetect's pass on the decoded textures)
    void WithDevice(const std::function<void(void* device, void* context)>& fn);
    // ContentDetect on a decoded texture of this device (the engine module's GPU tap, mpvnative/engine/d3d11_content.h:
    // the cell grid and, about once a second, the image model's thumbnail made on the GPU; only those are read back, a
    // few frames later; the detector runs on its own thread). false while nothing is measured; else the class now.
    bool MeasureTexture(void* texture, unsigned slice, int64_t ptsUs, bool disc, bool limited, int& cls, bool& cg);
    // where the route's own log lines go (the add-on's log)
    static void SetLog(std::function<void(const std::string&)> fn);
    // the FFmpeg D3D11VA device on our device (a new reference), so decoded frames are already on pushcore's device
    AVBufferRef* HwDevice() const;

    // a decoded frame: a D3D11 texture slice (AV_PIX_FMT_D3D11) or planar 4:2:0 planes (8-bit, or 10-bit in 16-bit
    // words). The frame is stored in a ring slice (its real picture, returned) and submitted to pushcore.
    int PushTexture(void* texture, unsigned slice, int64_t ptsUs, bool contiguous, std::string& err);
    int PushPlanes(const pm_yuv& src, int64_t ptsUs, bool contiguous, std::string& err);
    // the generated frames of the pair that ended with the last push, wanted at these timestamps (us): the ring slice
    // of each, or -1 for one pushcore did not deliver within the wait (the caller shows the real frame there)
    void Collect(const std::vector<int64_t>& wantUs, double frameUs, std::vector<int>& slots);
    // pending pictures (AllOut, and with Held over several frames): slots[i] < 0 are still wanted at wantUs[i] (within
    // tolUs); what pushcore has finished is routed into them; waits up to waitMs for the first still missing one.
    // Delivered / Missed are counted by the caller (Count) when it gives a picture up or out.
    void CollectInto(const std::vector<int64_t>& wantUs, int64_t tolUs, double waitMs, std::vector<int>& slots);
    void Count(bool delivered) { if (delivered) ++m_delivered; else ++m_missed; }
    // a ring slice into Kodi's buffer at the output size: semi = NV12 / P010 (Y plane, interleaved UV in dst[1]); else
    // planar (10-bit in the low bits of 16-bit words). A decoded frame kept at the source size (AllOut with sr) is
    // enlarged on the CPU (nearest pixel: it only stands in for a picture pushcore did not deliver)
    bool Read(int slot, uint8_t* const dst[3], const int stride[3], bool semi, std::string& err);
    void Flush();
    uint64_t Delivered() const { return m_delivered; }
    uint64_t Missed() const { return m_missed; }

    struct Impl;
private:
    bool Rings(std::string& why);
    Impl* m = nullptr;
    int m_k = 1, m_bits = 8, m_w = 0, m_h = 0, m_ow = 0, m_oh = 0;
    bool m_sr = false, m_allOut = false, m_held = false;
    int m_anime = -1;
    unsigned m_srMethod = 0;
    PcConfig m_cfg;
    std::string m_desc;
    uint64_t m_delivered = 0, m_missed = 0;
};

// is pm_pushcore.dll next to the add-on and pushcore installed (a cheap check for the settings / log)
bool PushcoreAvailable(std::string& why);
// the GPU behind a D3D11 device context (Kodi's, from kodi::gui::GetHWContext), for the log
std::string AdapterOfContext(void* d3d11Context);

} // namespace pmk
