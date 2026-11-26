// pushmpc.Enhance: the process-wide pushcore host. One D3D11 device and one warm PushCore per process; pushcore.dll and
// this plugin are pinned (GetModuleHandleEx PIN), so VapourSynth unloading the plugin when mpv tears down the script on
// a seek does not unload them. Each new script instance takes ownership (a token); the core is reused with
// pushcore_flush when the stream matches, re-set with setSettings + reload when only the settings differ, and reopened
// otherwise (pushcore's own NGX warm cache keeps the models across that).
//
// Fast start (as the MPC-HC filter and mpv's own route): a cold open whose full graph waits for NGX (RTX VSR / AI clean-up,
// ~2 s) opens a quick graph first (the driver's RTX VSR through the D3D11 video processor where it answers, else NVIDIA
// Image Scaling; no NGX: ~0.5 s) and the full graph on a worker thread (pushcore's GPU-only warm-up: no use of the
// immediate context, so no lock). At a submit after the full graph is ready, that source frame also goes into the full
// graph as its first one; the quick graph delivers up to and including it, the full graph from the next one on.
//
// Ingress: VapourSynth YUV420P8 / P10..P16 planes -> a staging NV12 / P010 texture (UV interleaved on the CPU) -> a
// DEFAULT texture -> pushcore_submit2 (streamNow = INT64_MIN: the full group, always).
// Egress: wait on the frame's fence, copy to a staging texture (4:4:4), or convert to 4:2:0 in a compute shader first
// (Auto, by the readback bandwidth), map, write the VapourSynth planes, return the lease.
#pragma once
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>
#include "pushmpc/pushcore_abi.h"
#include "schedule.h"

namespace pmvs {
using Microsoft::WRL::ComPtr;

// a planar YUV 4:2:0 source frame as VapourSynth holds it (bits 8 = 1 byte per sample, 9..16 = 2 bytes)
struct PlaneView { const uint8_t* data[3] = {}; ptrdiff_t stride[3] = {}; uint32_t width = 0, height = 0; int bits = 8; };
// what the output frames are in VapourSynth
enum class OutFmt { Rgb24, Rgb30, Yuv420P8, Yuv420P10 };
inline const char* OutFmtName(OutFmt f) { return f == OutFmt::Rgb24 ? "RGB24" : f == OutFmt::Rgb30 ? "RGB30" : f == OutFmt::Yuv420P8 ? "YUV420P8" : "YUV420P10"; }
inline bool OutIs420(OutFmt f) { return f == OutFmt::Yuv420P8 || f == OutFmt::Yuv420P10; }
// an output frame the caller allocated: three planes, written by the host
struct OutImage { uint8_t* data[3] = {}; ptrdiff_t stride[3] = {}; void* handle = nullptr; };

struct HostPaths { std::wstring coreDir, runtimeDir, shaderDir, logFile; };
enum class AdapterPick { Auto, Warp };

class CoreHost : public Backend<void*> {
public:
    static CoreHost& Get();          // never destroyed (the warm core lives until the process exits)
    std::mutex& Lock() { return m_lock; }

    // pushcore.dll (pinned) and its exports; idempotent
    bool Load(const HostPaths& paths, std::string& err);
    bool Loaded() const { return m_api != nullptr; }
    bool Caps(PushCoreCaps& caps);   // pushcore_capabilities for adapter 0 (false: export missing)
    bool EnsureDevice(uint64_t luid, AdapterPick pick, std::string& err);
    uint64_t DeviceLuid() const { return m_luid; }
    std::string DeviceName() const { return m_deviceName; }

    // take the core for a new instance (open, reuse or reopen); returns the owner token (0 = failed)
    uint64_t Acquire(const CoreKey& key, const PushCoreStreamInfo& si, const PushCoreSettings& ps, Reuse& how, double& ms, std::string& err);
    void Release(uint64_t token);    // the instance is gone: return everything, keep the core warm
    bool Owns(uint64_t token) const { return token && token == m_owner; }
    const PushCoreOutputInfo& OutInfo() const { return m_out; }
    bool Stats(PushCoreStats& st);
    uint32_t Features() const { return m_features; }
    // the colour the last instance of a stream found in its frames (mpv reloads the script on every seek)
    void RememberColour(uint32_t w, uint32_t h, int bits, int64_t fn, int64_t fd, const Colour& c) { m_colours[{ w, h, bits, fn, fd }] = c; }
    bool RecallColour(uint32_t w, uint32_t h, int bits, int64_t fn, int64_t fd, Colour& c) const { auto it = m_colours.find({ w, h, bits, fn, fd }); if (it == m_colours.end()) return false; c = it->second; return true; }

    // per output stream: the VapourSynth format and the 4:2:0 conversion
    bool ConfigureEgress(OutFmt fmt, bool hdrMatrix, std::string& err);

    // set by the instance around Pipeline::Produce (under Lock)
    std::function<bool(int64_t s, PlaneView& v)> source;   // the VapourSynth frame for source s
    std::function<bool(uint32_t w, uint32_t h, OutImage& img)> alloc; // a new VapourSynth output frame
    std::function<void(void* handle)> release;            // frees an output frame
    std::function<void(int64_t pts, bool generated, void* handle)> tag; // frame props after the pixels are in
    // zero copy (VLC's hardware-decoded pictures): source frames as textures on the adopted device, and the output frames
    // handed over as they come from pushcore (rawOut returns true when it read the frame on Context(); the host then
    // returns it with that context, else without)
    std::function<bool(int64_t s, ID3D11Texture2D*& tex, uint32_t& slice)> sourceTex;
    std::function<bool(const PushCoreFrame& fr, std::string& err)> rawOut;
    // use the caller's D3D11 device instead of the host's own; the core is reopened when the device changes
    bool AdoptDevice(ID3D11Device* dev, std::string& err);
    ID3D11Device* Device() const { return m_dev.Get(); }
    ID3D11DeviceContext* Context() const { return m_ctx.Get(); }
    ID3D11DeviceContext4* Context4() const { return m_ctx4.Get(); }

    // Backend
    bool CanSubmit() override;
    bool Submit(int64_t s, int64_t pts, bool discontinuity, std::string& err) override;
    bool Poll(uint32_t timeoutMs, std::vector<Out<void*>>& out, std::string& err) override;
    bool Flush(std::string& err) override;
    void Release(void* f) override { if (f && release) release(f); }
    uint64_t NowMs() override { return GetTickCount64(); }

private:
    CoreHost() = default;
    bool Readback(PushCore* core, const PushCoreFrame& fr, OutImage& img, std::string& err);
    bool EnsureStaging(DXGI_FORMAT fmt, uint32_t w, uint32_t h, std::string& err);
    bool EnsureUpload(uint32_t w, uint32_t h, bool p010, std::string& err);
    void ReturnAll();
    void CloseCore();
    // fast start
    bool QuickWanted(const PushCoreSettings& ps, std::string& why);
    void StartFull(const PushCoreStreamInfo& si, const PushCoreSettings& ps);
    void Handover(ID3D11Texture2D* tex, uint32_t slice, int64_t pts);
    void RetireOld(const char* why);
    void DropQuick();
    void CloseAsync(PushCore* core, const char* what);
    void ReturnFrom(PushCore* core);

    std::mutex m_lock;
    HMODULE m_dll = nullptr;
    const PushCoreApi* m_api = nullptr;
    PushCoreCapabilitiesFn m_caps = nullptr;
    uint32_t m_features = 0;
    HostPaths m_paths;
    ComPtr<ID3D11Device> m_dev;
    ComPtr<ID3D11DeviceContext> m_ctx;
    ComPtr<ID3D11DeviceContext4> m_ctx4;
    uint64_t m_luid = 0; bool m_warp = false; std::string m_deviceName;
    PushCore* m_core = nullptr;
    CoreKey m_key;
    // fast start: m_core is the quick graph while m_quick; the full graph arrives in m_next (m_nextLock: the worker never
    // takes m_lock); after the handover the quick graph is m_old until its last frame was taken
    bool m_quick = false, m_fullOpened = false;
    std::mutex m_nextLock;
    uint64_t m_gen = 0;
    bool m_nextDone = false;
    PushCore* m_next = nullptr;
    std::string m_nextErr;
    PushCore* m_old = nullptr;
    int64_t m_handoverPts = 0, m_discardUpTo = INT64_MIN;
    uint64_t m_quickTick = 0;
    PushCoreOutputInfo m_out{};
    uint64_t m_owner = 0, m_nextToken = 1;
    // ingress ring
    struct Up { ComPtr<ID3D11Texture2D> staging, tex; };
    std::vector<Up> m_up; size_t m_upNext = 0; uint32_t m_upW = 0, m_upH = 0; bool m_upP010 = false;
    // egress
    OutFmt m_fmt = OutFmt::Rgb24; bool m_hdrMatrix = false;
    ComPtr<ID3D11Texture2D> m_stage; DXGI_FORMAT m_stageFmt = DXGI_FORMAT_UNKNOWN; uint32_t m_stageW = 0, m_stageH = 0;
    ComPtr<ID3D11ComputeShader> m_cs; ComPtr<ID3D11Buffer> m_cb;
    ComPtr<ID3D11Texture2D> m_plane[3], m_planeStage[3]; ComPtr<ID3D11UnorderedAccessView> m_uav[3]; uint32_t m_planeW = 0, m_planeH = 0;
    std::map<ID3D11Texture2D*, ComPtr<ID3D11ShaderResourceView>> m_srv;
    bool m_csFailed = false;
    std::map<std::tuple<uint32_t, uint32_t, int, int64_t, int64_t>, Colour> m_colours;
};

} // namespace pmvs
