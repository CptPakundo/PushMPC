// pushcore_gpu_harness: the GPU path of pushcore.dll, end to end, with no window.
//  - loads pushcore.dll by full path (LOAD_WITH_ALTERED_SEARCH_PATH), as the MPC-VR fork will;
//  - creates its own D3D11 device on the NVIDIA adapter and decodes the clip with FFmpeg D3D11VA
//    into decoder texture arrays (what LAV Video hands MPC-VR);
//  - submits each decoded slice, polls outputs, and "presents" each one by waiting on its fence and
//    copying it into a D3D11 backbuffer-sized texture, then returns it (pushcore_presented);
//  - reads pixels back only for the frames picked with --save (PNG via WIC), never otherwise;
//  - checks counts, kinds and timestamps, logs per-stage GPU times and throughput.
//
// usage: pushcore_gpu_harness <video> <outdir> [--pushfps DIR] [--frames N] [--start S] [--paced 0|1]
//            [--copy 0|1] [--save 0,1,2,60] [--data DIR] [--sr 0|1] [--fg 0|1] [--multiplier N] [--warmup N]
//            [--shaders DIR] [--subtitles 0|1] [--held 0|1] [--gray W]
// --held 1: held-frame re-timing (PushCoreSettings.heldFrames; implies the PushMPC settings route). The per-source count
// check then becomes: real outputs at their source time, generated ones strictly between the real output before them
// and their batch's real frame, all in order; the output steps are reported on the 1/k grid (EVENNESS line).
// --gray W: every output's luma, box-downscaled to W pixels wide, appended to <outdir>\gray.raw (header: width,
// height as uint32; then one byte per pixel per output, in output order; outputs.csv gives the times) for a motion
// and judder analysis.
// --deband off|low|medium|high|anime, --srfamily standard|highbitrate|streaming, --srtarget WxH (the target class by
// its height: 1440 / 2160 / 2880; the source's shape is kept): test switches of the settings route
// --anime N: the anime upscaler (srMethod PMML) with model PUSHCORE_ANIME_SR_* N (settings route; the pmml runtime
// comes from PMML_RUNTIME, the registry or the install, PMML_ANIME_SR_BACKEND picks the backend)
// --subtitles 1: PushFPS's Subtitle Hold (PushCoreSettings.subtitleHold; implies the PushMPC settings route)
// --shaders: the compiled shaders pushcore uses. Default: the build tree's own (<harness folder>\..\shaders, the
// shaders built from the patched PushFPS source) when that folder exists, else <pushfps>\shaders. A copy of an
// installed PushFPS carries its upstream shaders, without PushMPC's (VfgFourierMellin, the patched
// VfgShotChange and YuvToLinearRgb), so the cut guard and the DV P5 path would not be what the filter runs.
// --start defaults to 1752 s (clip A's motion scene): a shorter clip needs --start 0, else "no first frame".
#include "pushmpc/pushcore_abi.h"

#define NOMINMAX
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
}

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

namespace {
std::string utf8(const std::wstring& s) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string r(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n, nullptr, nullptr);
    return r;
}
double msBetween(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

struct Agg { double mean = -1, median = -1, p95 = -1; size_t n = 0; };
Agg aggregate(std::vector<double> v) {
    Agg a;
    v.erase(std::remove_if(v.begin(), v.end(), [](double x) { return x < 0; }), v.end());
    if (v.empty()) return a;
    std::sort(v.begin(), v.end());
    a.n = v.size();
    double s = 0; for (double x : v) s += x;
    a.mean = s / double(v.size());
    a.median = v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
    a.p95 = v[std::min(v.size() - 1, size_t(std::ceil(0.95 * double(v.size()))) - 1)];
    return a;
}

AVPixelFormat pickD3D11(AVCodecContext*, const AVPixelFormat* formats) {
    for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == AV_PIX_FMT_D3D11) return *p;
    std::cout << "decoder offers no D3D11 format\n";
    return AV_PIX_FMT_NONE;
}

bool savePng(const std::wstring& path, const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t pitch) {
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    ComPtr<IWICStream> stream; ComPtr<IWICBitmapEncoder> enc; ComPtr<IWICBitmapFrameEncode> fr;
    if (FAILED(f->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
    if (FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) || FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
    if (FAILED(enc->CreateNewFrame(&fr, nullptr)) || FAILED(fr->Initialize(nullptr)) || FAILED(fr->SetSize(w, h))) return false;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    if (FAILED(fr->SetPixelFormat(&fmt)) || fmt != GUID_WICPixelFormat24bppBGR) return false;
    std::vector<uint8_t> bgr(size_t(w) * h * 3);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* s = rgba + size_t(y) * pitch;
        uint8_t* d = bgr.data() + size_t(y) * w * 3;
        for (uint32_t x = 0; x < w; ++x) { d[3 * x] = s[4 * x + 2]; d[3 * x + 1] = s[4 * x + 1]; d[3 * x + 2] = s[4 * x]; }
    }
    if (FAILED(fr->WritePixels(h, w * 3, UINT(bgr.size()), bgr.data())) || FAILED(fr->Commit()) || FAILED(enc->Commit())) return false;
    return true;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    bool hdrDisplay = false;   // --hdr-display 1: an HDR presenter (HDR sources stay HDR10, PQ BT.2020)
    if (argc < 3) {
        std::cout << "usage: pushcore_gpu_harness <video> <outdir> [--pushfps DIR] [--frames N] [--start S] [--paced 0|1]"
                     " [--copy 0|1] [--save 0,1,2,60] [--data DIR] [--sr 0|1] [--fg 0|1] [--multiplier N] [--warmup N] [--shaders DIR]\n";
        return 2;
    }
    const std::wstring input = argv[1];
    const std::filesystem::path outDir = argv[2];
    std::wstring pushfps = L"C:\\pm\\run", data, shaderDir;
    bool haveData = false;
    unsigned maxFrames = 120, warmup = 10, idleMs = 0;
    double start = 1752;
    bool paced = false, copy = false;
    std::set<uint64_t> save{0, 1, 2, 60};
    int sr = -1, fgOpt = -1, mult = -1;
    uint32_t grayWidth = 0;
    // PushMPC's own settings instead of a chain session (--backend vfg|dlssg|pushflow|fsr|frc|nvof|xefg|vpl|none, --srmethod rtx|fsr|nis|xess|ivsr): the filter's route
    bool useSettings = false;
    bool animeWanted = false;
    // G10: --adapter nvidia (default) | intel | amd | any | warp (Windows' software GPU, sets PUSHMPC_TEST_WARP=1 for pushcore);
    // <video> = synthetic:WxH = no file: a textured square moving over a textured background, a hard cut at --cut N
    // (default 0 = none), 30 fps; --check 1 reads every output back and checks the square's position in the generated
    // frames and the hold on the cut pair; --selftest xefg|xess|ivsr|vpl|bridge|all runs pushcore_selfTest on the adapter
    // (--record 1 stores the hardware results; tools/pushcore_selftest is the stand-alone version)
    std::wstring adapterPick = L"nvidia", selfTest;
    uint64_t cutAt = 0; bool checkMotion = false, selfTestRecord = false;
    std::wstring nrLiveSpec;   // --nrlive
    PushCoreSettings ps{};
    ps.structSize = sizeof(ps); ps.source = PUSHCORE_SETTINGS_PUSHMPC; ps.sr = 1; ps.srQuality = 4; ps.srTarget = PUSHCORE_SR_TARGET_4K;
    ps.multiplier = 4; ps.vfgQuality = 1; ps.nrTone = ps.nrStructure = 1; ps.fgBackend = PUSHCORE_FG_VFG; ps.fgTier = 1;
    ps.srMethod = PUSHCORE_SR_METHOD_RTX; ps.cutGuard = 1; ps.debandStrength = PUSHCORE_DEBAND_MEDIUM;
    for (int i = 3; i + 1 < argc; i += 2) {
        const std::wstring k = argv[i], v = argv[i + 1];
        if (k == L"--backend") {
            useSettings = true;
            ps.fgBackend = v == L"dlssg" ? PUSHCORE_FG_DLSSG : v == L"pushflow" ? PUSHCORE_FG_PUSHFLOW : v == L"fsr" ? PUSHCORE_FG_FSR
                         : v == L"frc" ? PUSHCORE_FG_AMDFRC : v == L"nvof" ? PUSHCORE_FG_NVOF : v == L"xefg" ? PUSHCORE_FG_XEFG : v == L"vpl" ? PUSHCORE_FG_VPL : PUSHCORE_FG_VFG;
            if (v == L"none") ps.multiplier = 1;
            continue;
        }
        if (k == L"--q") { ps.vfgQuality = uint32_t(_wtoi(v.c_str())); ps.fgTier = ps.vfgQuality; continue; }
        if (k == L"--srq") { const int q = _wtoi(v.c_str()); ps.sr = q > 0; ps.srQuality = q > 0 ? uint32_t(q) : 2u; continue; }
        if (k == L"--srmethod") { ps.srMethod = v == L"fsr" ? PUSHCORE_SR_METHOD_FSR : v == L"nis" ? PUSHCORE_SR_METHOD_NIS : v == L"xess" ? PUSHCORE_SR_METHOD_XESS
                                                       : v == L"ivsr" ? PUSHCORE_SR_METHOD_IVSR : PUSHCORE_SR_METHOD_RTX; continue; }
        if (k == L"--adapter" || k == L"--vendor") { adapterPick = v; continue; }
        if (k == L"--cut") { cutAt = uint64_t(_wtoi64(v.c_str())); continue; }
        if (k == L"--check") { checkMotion = _wtoi(v.c_str()) != 0; continue; }
        if (k == L"--selftest") { selfTest = v; continue; }
        if (k == L"--record") { selfTestRecord = _wtoi(v.c_str()) != 0; continue; }
        if (k == L"--target") { ps.srTarget = v == L"1440p" ? PUSHCORE_SR_TARGET_1440P : v == L"5k" ? PUSHCORE_SR_TARGET_5K : PUSHCORE_SR_TARGET_4K; continue; }
        if (k == L"--cutguard") { ps.cutGuard = _wtoi(v.c_str()); continue; }
        // test switches (settings route): deband, the RTX VSR model family, the SR output height
        if (k == L"--deband") {
            useSettings = true;
            ps.deband = v == L"off" ? 0 : 1;
            ps.debandStrength = v == L"low" ? PUSHCORE_DEBAND_LOW : v == L"high" ? PUSHCORE_DEBAND_HIGH : v == L"anime" ? PUSHCORE_DEBAND_ANIME : PUSHCORE_DEBAND_MEDIUM;
            continue;
        }
        if (k == L"--srfamily") {
            useSettings = true;
            ps.srFamily = v == L"highbitrate" ? PUSHCORE_SR_FAMILY_HIGH_BITRATE : v == L"streaming" ? PUSHCORE_SR_FAMILY_STREAMING : PUSHCORE_SR_FAMILY_STANDARD;
            continue;
        }
        if (k == L"--srtarget") {   // WxH: pushcore's ABI takes a target class (1440p / 4K / 5K) and keeps the source's shape
            useSettings = true;
            const size_t x = v.find(L'x');
            const int tw = _wtoi(v.c_str()), th = x == std::wstring::npos ? 0 : _wtoi(v.c_str() + x + 1);
            ps.srTarget = th == 1440 ? PUSHCORE_SR_TARGET_1440P : th == 2880 ? PUSHCORE_SR_TARGET_5K : PUSHCORE_SR_TARGET_4K;
            if (th != 1440 && th != 2160 && th != 2880)
                std::cout << "--srtarget " << tw << "x" << th << ": no such class; 4K (2160 lines) is used\n";
            std::cout << "--srtarget " << tw << "x" << th << ": the output is the source's shape fitted to " << (th == 1440 ? 1440 : th == 2880 ? 2880 : 2160)
                      << " lines (a 16:9 source: exactly " << (th == 1440 ? "2560x1440" : th == 2880 ? "5120x2880" : "3840x2160") << ")\n";
            continue;
        }
        if (k == L"--anime") {   // test switch: the anime upscaler (srMethod PMML), model by PUSHCORE_ANIME_SR_* index
            useSettings = true;
            ps.sr = 1; ps.srMethod = PUSHCORE_SR_METHOD_PMML; ps.animeSrModel = uint32_t(_wtoi(v.c_str())); animeWanted = true;
            continue;
        }
        if (k == L"--subtitles") { useSettings = true; ps.subtitleHold = _wtoi(v.c_str()); continue; }
        if (k == L"--held") { useSettings = true; ps.heldFrames = _wtoi(v.c_str()); continue; }
        // Neural Rendering (settings route): --nr 1; --nrlive N:intensity=0.6,tone=0.5,structure=0.5,skin=0.3,automask=1,style=1
        // changes those dials before source frame N with setSettings + applySettings (the no-reset live path; pushcore.log
        // shows "nr live:" lines)
        if (k == L"--nr") { useSettings = true; ps.nr = _wtoi(v.c_str()); continue; }
        if (k == L"--nrlive") { useSettings = true; nrLiveSpec = v; continue; }
        if (k == L"--gray") { grayWidth = uint32_t(_wtoi(v.c_str())); continue; }
        if (k == L"--pushfps") pushfps = v;
        else if (k == L"--shaders") shaderDir = v;
        else if (k == L"--frames") maxFrames = unsigned(_wtoi(v.c_str()));
        else if (k == L"--start") start = _wtof(v.c_str());
        else if (k == L"--paced") paced = _wtoi(v.c_str()) != 0;
        else if (k == L"--copy") copy = _wtoi(v.c_str()) != 0;
        else if (k == L"--hdr-display") hdrDisplay = _wtoi(v.c_str()) != 0;
        else if (k == L"--data") { data = v; haveData = true; }
        else if (k == L"--sr") sr = _wtoi(v.c_str());
        else if (k == L"--fg") fgOpt = _wtoi(v.c_str());
        else if (k == L"--multiplier") mult = _wtoi(v.c_str());
        else if (k == L"--warmup") warmup = unsigned(_wtoi(v.c_str()));
        else if (k == L"--idle") idleMs = unsigned(_wtoi(v.c_str()));   // keep the open core idle this long before the close (idle cost)
        else if (k == L"--save") {
            save.clear();
            size_t p = 0;
            while (p < v.size()) { const size_t e = v.find(L',', p); const auto s = v.substr(p, e == std::wstring::npos ? std::wstring::npos : e - p); if (!s.empty()) save.insert(uint64_t(_wtoi64(s.c_str()))); if (e == std::wstring::npos) break; p = e + 1; }
        } else { std::cout << "unknown option " << utf8(k) << "\n"; return 2; }
    }
    std::filesystem::create_directories(outDir);
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 2;

    // --- pushcore.dll by full path; its FFmpeg/FidelityFX imports resolve from its own folder ---
    const std::wstring dllPath = (std::filesystem::path(pushfps) / L"pushcore.dll").wstring();
    HMODULE dll = LoadLibraryExW(dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll) { std::cout << "cannot load " << utf8(dllPath) << " error " << GetLastError() << "\n"; return 1; }
    SetDllDirectoryW(pushfps.c_str());   // the harness's own delay-loaded FFmpeg (same files)
    auto getApi = reinterpret_cast<PushCoreGetApiFn>(GetProcAddress(dll, "pushcore_getApi"));
    const PushCoreApi* api = getApi ? getApi(PUSHCORE_ABI_VERSION) : nullptr;
    if (!api) { std::cout << "pushcore_getApi missing or ABI mismatch\n"; return 1; }
    {
        wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::cout << "harness " << utf8(exe) << "\nloaded  " << utf8(dllPath) << " (ABI " << api->abiVersionFn() << ")\n";
    }

    // --- D3D11 device on the NVIDIA adapter (or --adapter's), as LAV/MPC-VR would have it ---
    const bool warpAdapter = adapterPick == L"warp";
    const bool synthetic = input.rfind(L"synthetic:", 0) == 0;
    if (warpAdapter) SetEnvironmentVariableW(L"PUSHMPC_TEST_WARP", L"1");   // pushcore's D3D12 side may then take WARP (by LUID)
    ComPtr<IDXGIFactory1> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    ComPtr<IDXGIAdapter1> adapter, best;
    SIZE_T bestMem = 0;
    if (warpAdapter) { ComPtr<IDXGIFactory4> f4; if (SUCCEEDED(factory.As(&f4))) f4->EnumWarpAdapter(IID_PPV_ARGS(&best)); }
    else for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{}; adapter->GetDesc1(&d);
        const uint32_t want = adapterPick == L"intel" ? 0x8086u : adapterPick == L"amd" ? 0x1002u : adapterPick == L"any" ? 0u : 0x10DEu;
        if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || (want && d.VendorId != want)) continue;
        // strictly more memory: DXGI can list one GPU twice (equal memory, another LUID where NGX does not init); the first
        // such entry is the one the player and the filter use
        if (!best || d.DedicatedVideoMemory > bestMem) { best = adapter; bestMem = d.DedicatedVideoMemory; }
    }
    if (!best) { std::cout << "no " << utf8(adapterPick) << " adapter\n"; return 1; }
    uint64_t bestLuid = 0;
    { DXGI_ADAPTER_DESC1 d{}; best->GetDesc1(&d); bestLuid = (uint64_t(uint32_t(d.AdapterLuid.HighPart)) << 32) | d.AdapterLuid.LowPart; }
    // --selftest: pushcore_selfTest on this adapter, then exit (0 = passed)
    if (!selfTest.empty()) {
        auto fn = reinterpret_cast<PushCoreSelfTestFn>(GetProcAddress(dll, "pushcore_selfTest"));
        if (!fn) { std::cout << "pushcore_selfTest missing\n"; return 1; }
        const uint32_t which = selfTest == L"xess" ? PUSHCORE_SELFTEST_XESS_SR : selfTest == L"ivsr" ? PUSHCORE_SELFTEST_IVSR
                             : selfTest == L"vpl" ? PUSHCORE_SELFTEST_VPL_FI : selfTest == L"bridge" ? PUSHCORE_SELFTEST_BRIDGE
                             : selfTest == L"all" ? 31u : PUSHCORE_SELFTEST_XEFG;
        if (mult >= 2) SetEnvironmentVariableW(L"PUSHMPC_SELFTEST_XEFG_MULTIPLIER", std::to_wstring(mult).c_str());
        if (maxFrames != 120) SetEnvironmentVariableW(L"PUSHMPC_SELFTEST_FRAMES", std::to_wstring(maxFrames).c_str());
        std::vector<char> report(32768);
        const uint32_t flags = (warpAdapter ? PUSHCORE_SELFTEST_FLAG_WARP : 0u) | (selfTestRecord ? PUSHCORE_SELFTEST_FLAG_RECORD : 0u);
        const int r = fn(warpAdapter ? 0 : bestLuid, pushfps.c_str(), which, flags, report.data(), uint32_t(report.size()));
        std::cout << "SELFTEST " << utf8(selfTest) << ": " << report.data() << "\n" << (r == PUSHCORE_OK ? "PASS" : "FAIL") << "\n";
        return r == PUSHCORE_OK ? 0 : 1;
    }
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> imm;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(best.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, (synthetic ? 0 : D3D11_CREATE_DEVICE_VIDEO_SUPPORT) | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                 levels, 2, D3D11_SDK_VERSION, &device, nullptr, &imm))) { std::cout << "D3D11CreateDevice failed\n"; return 1; }
    { ComPtr<ID3D10Multithread> mt; if (SUCCEEDED(device.As(&mt))) mt->SetMultithreadProtected(TRUE); }
    ComPtr<ID3D11DeviceContext4> imm4; imm.As(&imm4);
    { DXGI_ADAPTER_DESC1 d{}; best->GetDesc1(&d); std::cout << "D3D11 device on " << utf8(d.Description) << std::format(" LUID {:08X}:{:08X}\n", unsigned(d.AdapterLuid.HighPart), d.AdapterLuid.LowPart); }

    // --- D3D11VA decode on that device ---
    // G10: the synthetic source (no decoder): NV12 frames made on the CPU, BT.709 limited range, 30 fps. A red textured
    // square moves by (vx, vy) per frame over a textured grey background; at frame --cut the background and the square's
    // path change completely (a hard cut).
    struct Synth {
        uint32_t W = 0, H = 0, S = 0; double vx = 0, vy = 0; uint64_t cut = 0;
        double px(double t) const { return W * 0.15 + vx * t; }
        double py(double t) const { return H * 0.30 + vy * t; }
        // RGB (8-bit) of frame k at continuous time t (t = k for real frames; scene = 0 before the cut, 1 after)
        void rgb(double t, int scene, std::vector<uint8_t>& img) const {
            img.assign(size_t(W) * H * 3, 0);
            const double sx = scene ? W * 0.6 - vx * (t - double(cut)) : px(t), sy = scene ? H * 0.55 : py(t);
            for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < W; ++x) {
                uint8_t* p = &img[(size_t(y) * W + x) * 3];
                const bool c = ((x / 16) + (y / 16)) & 1;
                if (!scene) { p[0] = uint8_t(70 + (c ? 30 : 0) + (x * 37 % 19)); p[1] = uint8_t(80 + (y * 13 % 23)); p[2] = uint8_t(90 + ((x + y) % 29)); }
                else { p[0] = uint8_t(20 + (y * 7 % 31)); p[1] = uint8_t(120 + (c ? 50 : 0)); p[2] = uint8_t(60 + (x * 11 % 37)); }
                const double qx = x + 0.5 - sx, qy = y + 0.5 - sy;
                if (qx >= 0 && qy >= 0 && qx < S && qy < S) {
                    const bool cc = ((int(qx) / 6) + (int(qy) / 6)) & 1;
                    p[0] = uint8_t(cc ? 240 : 210); p[1] = uint8_t(cc ? 40 : 70); p[2] = uint8_t((int(qx) * 9 + int(qy) * 5) & 63);
                }
            }
        }
        void nv12(const std::vector<uint8_t>& rgb, uint8_t* dst, uint32_t pitch) const {   // BT.709 limited range
            auto Y = [&](const uint8_t* p) { return 16.0 + 219.0 * (0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]) / 255.0; };
            for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < W; ++x) dst[size_t(y) * pitch + x] = uint8_t(std::lround(Y(&rgb[(size_t(y) * W + x) * 3])));
            uint8_t* uv = dst + size_t(pitch) * H;
            for (uint32_t y = 0; y < H / 2; ++y) for (uint32_t x = 0; x < W / 2; ++x) {
                double r = 0, g = 0, b = 0;
                for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) { const uint8_t* p = &rgb[(size_t(2 * y + dy) * W + 2 * x + dx) * 3]; r += p[0]; g += p[1]; b += p[2]; }
                r /= 4 * 255.0; g /= 4 * 255.0; b /= 4 * 255.0;
                const double yy = 0.2126 * r + 0.7152 * g + 0.0722 * b;
                uv[size_t(y) * pitch + 2 * x] = uint8_t(std::clamp(std::lround(128 + 224 * (b - yy) / 1.8556), 0L, 255L));
                uv[size_t(y) * pitch + 2 * x + 1] = uint8_t(std::clamp(std::lround(128 + 224 * (r - yy) / 1.5748), 0L, 255L));
            }
        }
    } syn;
    ComPtr<ID3D11Texture2D> synTex, synStaging;
    uint64_t synIndex = 0;
    AVFormatContext* fmt = nullptr; AVStream* st = nullptr; AVCodecContext* dec = nullptr; AVBufferRef* hwdev = nullptr;
    AVPacket* pkt = nullptr; AVFrame* decoded = nullptr;
    int vs = -1;
    AVPixelFormat sw = AV_PIX_FMT_NV12;
    if (synthetic) {
        unsigned w = 640, h = 360;
        swscanf_s(input.c_str() + 10, L"%ux%u", &w, &h);
        syn.W = std::max(64u, w) & ~1u; syn.H = std::max(64u, h) & ~1u; syn.S = std::max(16u, syn.H / 5);
        syn.vx = std::max(2.0, syn.W / 80.0); syn.vy = std::max(0.5, syn.H / 240.0); syn.cut = cutAt;
        D3D11_TEXTURE2D_DESC d{}; d.Width = syn.W; d.Height = syn.H; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_NV12; d.SampleDesc = {1, 0};
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&d, nullptr, &synTex))) { std::cout << "no NV12 texture on this device\n"; return 1; }
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateTexture2D(&d, nullptr, &synStaging))) { std::cout << "no NV12 staging texture\n"; return 1; }
        std::cout << std::format("synthetic source {}x{} NV12 30 fps, square {} px moving {:.1f},{:.1f} px per frame, cut at {}\n", syn.W, syn.H, syn.S, syn.vx, syn.vy, cutAt);
    } else {
    if (avformat_open_input(&fmt, utf8(input).c_str(), nullptr, nullptr) < 0 || avformat_find_stream_info(fmt, nullptr) < 0) { std::cout << "cannot open input\n"; return 1; }
    vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs < 0) { std::cout << "no video stream\n"; return 1; }
    st = fmt->streams[vs];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec, st->codecpar);
    hwdev = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    auto* hwctx = reinterpret_cast<AVD3D11VADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(hwdev->data)->hwctx);
    hwctx->device = device.Get(); device->AddRef();   // FFmpeg releases it
    if (av_hwdevice_ctx_init(hwdev) < 0) { std::cout << "D3D11VA device init failed\n"; return 1; }
    dec->hw_device_ctx = av_buffer_ref(hwdev);
    dec->get_format = pickD3D11;
    dec->extra_hw_frames = 4;
    if (!codec || avcodec_open2(dec, codec, nullptr) < 0) { std::cout << "no decoder\n"; return 1; }
    if (start > 0) av_seek_frame(fmt, -1, int64_t(start * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
    pkt = av_packet_alloc();
    decoded = av_frame_alloc();
    }
    bool eof = false;
    double decodeMs = 0; uint64_t decodedCount = 0;
    auto decodeNext = [&]() -> bool {   // leaves a D3D11 frame in `decoded` (synthetic: makes the next frame)
        if (synthetic) {
            std::vector<uint8_t> rgb;
            syn.rgb(double(synIndex), cutAt && synIndex >= cutAt ? 1 : 0, rgb);
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(imm->Map(synStaging.Get(), 0, D3D11_MAP_WRITE, 0, &m))) return false;
            syn.nv12(rgb, static_cast<uint8_t*>(m.pData), m.RowPitch);
            imm->Unmap(synStaging.Get(), 0);
            imm->CopyResource(synTex.Get(), synStaging.Get());
            ++decodedCount;
            return true;
        }
        const auto t = Clock::now();
        for (;;) {
            const int r = avcodec_receive_frame(dec, decoded);
            if (r == 0) {
                if (start > 0 && decoded->best_effort_timestamp != AV_NOPTS_VALUE &&
                    decoded->best_effort_timestamp * av_q2d(st->time_base) < start - 0.001) { av_frame_unref(decoded); continue; }
                decodeMs += msBetween(t, Clock::now()); ++decodedCount;
                return true;
            }
            if (r != AVERROR(EAGAIN) || eof) return false;
            for (;;) {
                if (av_read_frame(fmt, pkt) < 0) { eof = true; avcodec_send_packet(dec, nullptr); break; }
                const bool mine = pkt->stream_index == vs;
                if (mine) avcodec_send_packet(dec, pkt);
                av_packet_unref(pkt);
                if (mine) break;
            }
        }
    };
    if (!decodeNext()) { std::cout << "no first frame\n"; return 1; }
    PushCoreStreamInfo si{};
    si.structSize = sizeof(si);
    if (synthetic) {
        si.width = syn.W; si.height = syn.H; si.format = PUSHCORE_INPUT_NV12; si.fpsNum = 30; si.fpsDen = 1;
        si.range = AVCOL_RANGE_MPEG; si.matrix = AVCOL_SPC_BT709; si.primaries = AVCOL_PRI_BT709; si.transfer = AVCOL_TRC_BT709;
    } else {
    if (decoded->format != AV_PIX_FMT_D3D11) { std::cout << "decoder did not produce D3D11 frames\n"; return 1; }
    sw = AVPixelFormat(reinterpret_cast<AVHWFramesContext*>(decoded->hw_frames_ctx->data)->sw_format);
    {
        D3D11_TEXTURE2D_DESC d{}; reinterpret_cast<ID3D11Texture2D*>(decoded->data[0])->GetDesc(&d);
        std::cout << std::format("decoder: D3D11VA {}x{} texture array {}x{}x{} format {} sw {}\n", decoded->width, decoded->height, d.Width, d.Height,
                                 d.ArraySize, int(d.Format), av_get_pix_fmt_name(sw));
    }
    si.width = uint32_t(decoded->width); si.height = uint32_t(decoded->height);
    si.format = sw == AV_PIX_FMT_P010 ? PUSHCORE_INPUT_P010 : PUSHCORE_INPUT_NV12;
    si.fpsNum = st->avg_frame_rate.num; si.fpsDen = st->avg_frame_rate.den ? st->avg_frame_rate.den : 1;
    si.range = decoded->color_range; si.matrix = decoded->colorspace; si.primaries = decoded->color_primaries; si.transfer = decoded->color_trc;
    }
    si.displayHdr = hdrDisplay ? 1 : 0;

    const std::wstring logFile = (outDir / L"pushcore.log").wstring();
    PushCoreConfig cfg{};
    cfg.structSize = sizeof(cfg); cfg.abiVersion = PUSHCORE_ABI_VERSION;
    cfg.pushfpsDir = pushfps.c_str();
    if (shaderDir.empty()) {
        wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const auto own = std::filesystem::path(exe).parent_path().parent_path() / L"shaders";
        std::error_code ec;
        if (std::filesystem::exists(own / L"VfgFourierMellin.dxil", ec)) shaderDir = own.wstring();
    }
    if (!shaderDir.empty()) cfg.shaderDir = shaderDir.c_str();
    std::cout << "shaders " << (shaderDir.empty() ? utf8(pushfps) + "\\shaders" : utf8(shaderDir)) << "\n";
    cfg.dataDir = haveData ? data.c_str() : nullptr;
    cfg.logFile = logFile.c_str();
    cfg.sr = sr; cfg.fg = fgOpt; cfg.multiplier = mult; cfg.vfgQuality = -1;
    cfg.flags = copy ? PUSHCORE_FLAG_EGRESS_COPY : 0;
    if (useSettings) {
        if (mult >= 1) ps.multiplier = uint32_t(mult);
        cfg.settings = &ps; cfg.dataDir = L""; cfg.sr = cfg.fg = cfg.multiplier = -1;
        std::cout << std::format("settings: backend {} x{} quality/tier {} SR {} (method {}, level {}) cut guard {} subtitle hold {}\n", ps.fgBackend, ps.multiplier,
                                 ps.vfgQuality, ps.sr, ps.srMethod, ps.srQuality, ps.cutGuard, ps.subtitleHold);
    }
    char err[1024]{};
    const auto openStart = Clock::now();
    PushCore* core = api->open(&cfg, &si, device.Get(), err, sizeof(err));
    const double openMs = msBetween(openStart, Clock::now());
    if (!core) { std::cout << "OPEN FAILED: " << err << "\n"; return 1; }
    PushCoreOutputInfo oi{};
    api->outputInfo(core, &oi);
    std::cout << "pushcore: " << oi.description << std::format("\nopen {:.0f} ms, egress {}, output format {}\n", openMs,
                                                                 oi.egressMode == PUSHCORE_EGRESS_DIRECT ? "direct" : "copy", oi.dxgiFormat);
    const uint32_t m = oi.multiplier;

    // "Backbuffer": what MPC-VR's presenter would draw into.
    D3D11_TEXTURE2D_DESC bd{};
    bd.Width = oi.width; bd.Height = oi.height; bd.MipLevels = 1; bd.ArraySize = 1; bd.Format = DXGI_FORMAT(oi.dxgiFormat);
    bd.SampleDesc = {1, 0}; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> backbuffer, staging;
    device->CreateTexture2D(&bd, nullptr, &backbuffer);
    bd.Usage = D3D11_USAGE_STAGING; bd.BindFlags = 0; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    device->CreateTexture2D(&bd, nullptr, &staging);

    // --- run ---
    std::deque<PushCoreFrame> queue;
    std::vector<int64_t> sourcePts;
    std::vector<uint32_t> perSource;
    uint64_t submitted = 0, presentedCount = 0, real = 0, generated = 0, orderErrors = 0, ptsErrors = 0, sizeErrors = 0, busy = 0;
    uint64_t late = 0, pngs = 0, pngFail = 0;
    int64_t lastPts = INT64_MIN, lastRealPts = INT64_MIN, prevOutPts = INT64_MIN;
    std::vector<int64_t> steps;   // --held: output pts steps
    const bool heldOn = useSettings && ps.heldFrames != 0;
    bool haveFrame = true, failed = false;
    std::vector<double> lateness;
    Clock::time_point clockStart{}; int64_t clockOrigin = 0; bool clockRunning = false;
    const double leadMs = 60;    // paced: first paced output is due this long after the clock starts
    const uint64_t pacedFrom = 4; // paced: sources before this (graph and VFG warm-up) are shown at once, then the clock starts
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const double outIntervalMs = si.fpsNum > 0 ? 1000.0 * si.fpsDen / si.fpsNum / m : 10;
    FILE* gray = nullptr;
    if (grayWidth >= 16 && grayWidth <= oi.width) {
        gray = _wfopen((outDir / L"gray.raw").wstring().c_str(), L"wb");
        if (gray) { const uint32_t dims[2] = { grayWidth, std::max<uint32_t>(1, uint32_t(uint64_t(oi.height) * grayWidth / oi.width)) }; fwrite(dims, 4, 2, gray); }
    }
    std::ofstream csv(outDir / L"outputs.csv");
    csv << "output,source,kind,subframe,pts100ns,presentMs,latenessMs\n";
    uint32_t subtitleMax = 0; uint64_t subtitleSamples = 0;   // Subtitle Hold pixels seen in the stats during the run
    const auto runStart = Clock::now();
    Clock::time_point steadyStart{}; uint64_t steadyBase = 0; bool steady = false;
    const uint64_t lastSave = save.empty() ? 0 : *save.rbegin();
    const uint64_t steadyFrom = std::max<uint64_t>(warmup, lastSave + 4);
    double submitMs = 0;

    // G10 --check: the square's position in each generated frame against the interpolation of its two real neighbours'
    // positions (measured in the real outputs), and on the cut pair the hold (each generated frame = one of the pair's real
    // frames, not a blend across the cut)
    struct CheckState {
        std::map<uint64_t, std::vector<uint8_t>> real;              // source index -> real output
        std::vector<std::pair<PushCoreFrame, std::vector<uint8_t>>> pendingGen;
        std::vector<double> posErr; uint64_t gens = 0, holdsOk = 0, holdsBad = 0, noSquare = 0;
    } chk;
    auto centroid = [&](const std::vector<uint8_t>& img, double& cx, double& cy) {
        double sx = 0, sy = 0, n = 0;
        for (uint32_t y = 0; y < oi.height; ++y) for (uint32_t x = 0; x < oi.width; ++x) {
            const uint8_t* p = &img[(size_t(y) * oi.width + x) * 4];
            if (p[0] >= 170 && p[0] > p[1] + 90 && p[0] > p[2] + 90) { sx += x; sy += y; n += 1; }
        }
        if (n < 16) return false;
        cx = sx / n; cy = sy / n; return true;
    };
    auto psnr8 = [&](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
        double se = 0; size_t n = 0;
        for (size_t i = 0; i < a.size(); i += 4) for (int k = 0; k < 3; ++k) { const double d = double(a[i + k]) - b[i + k]; se += d * d; ++n; }
        const double mse = se / double(n ? n : 1);
        return mse < 1e-9 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
    };
    auto checkOutput = [&](const PushCoreFrame& f, std::vector<uint8_t> img) {
        if (f.kind != PUSHCORE_FRAME_REAL) { chk.pendingGen.push_back({f, std::move(img)}); return; }
        const uint64_t k = f.sourceIndex;
        auto prev = chk.real.find(k - 1);
        for (auto& [g, gi] : chk.pendingGen) {
            if (g.sourceIndex != k || prev == chk.real.end()) continue;
            ++chk.gens;
            if (cutAt && k == cutAt) {   // the cut pair: held (A or B), never a blend of the two shots
                const double pa = psnr8(gi, prev->second), pb = psnr8(gi, img);
                if (std::max(pa, pb) >= 35.0) ++chk.holdsOk; else { ++chk.holdsBad; std::cout << std::format("CHECK cut pair {} subframe {}: PSNR vs A {:.1f} dB, vs B {:.1f} dB (not held)\n", k, g.subframe, pa, pb); }
                continue;
            }
            if (cutAt && k > cutAt && k <= cutAt + 1) continue;   // the first pair after the cut: history restarts
            double ax, ay, bx, by, gx, gy;
            if (!centroid(prev->second, ax, ay) || !centroid(img, bx, by) || !centroid(gi, gx, gy)) { ++chk.noSquare; continue; }
            const double ph = double(g.subframe) / double(m);
            chk.posErr.push_back(std::hypot(gx - (ax + (bx - ax) * ph), gy - (ay + (by - ay) * ph)));
        }
        chk.pendingGen.clear();
        chk.real[k] = std::move(img);
        if (chk.real.size() > 4) chk.real.erase(chk.real.begin());
    };

    auto present = [&](const PushCoreFrame& f) -> bool {
        if (f.width != oi.width || f.height != oi.height || !f.texture) ++sizeErrors;
        imm4->Wait(f.readyFence, f.readyValue);           // GPU-side wait for the graph's output
        imm->CopyResource(backbuffer.Get(), f.texture);   // the presenter's one pass
        const bool snap = save.count(f.sourceIndex) != 0;
        if (grayWidth && gray) {   // motion analysis: the output's luma, box-downscaled
            imm->CopyResource(staging.Get(), backbuffer.Get());
            D3D11_MAPPED_SUBRESOURCE map{};
            if (SUCCEEDED(imm->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
                const uint32_t gw = grayWidth, gh = std::max<uint32_t>(1, uint32_t(uint64_t(oi.height) * gw / oi.width)), bx = oi.width / gw, by = oi.height / gh;
                std::vector<uint8_t> row(gw);
                const bool r10 = oi.dxgiFormat == 24;
                for (uint32_t y = 0; y < gh; ++y) {
                    for (uint32_t x = 0; x < gw; ++x) {
                        uint64_t acc = 0;
                        for (uint32_t yy = 0; yy < by; ++yy) {
                            const uint8_t* p = static_cast<const uint8_t*>(map.pData) + size_t(y * by + yy) * map.RowPitch + size_t(x * bx) * 4;
                            for (uint32_t xx = 0; xx < bx; ++xx, p += 4) {
                                if (r10) { const uint32_t v = *reinterpret_cast<const uint32_t*>(p); acc += (((v & 1023) + 2 * ((v >> 10) & 1023) + ((v >> 20) & 1023)) >> 2) >> 2; }
                                else acc += (uint32_t(p[0]) + 2u * p[1] + p[2]) >> 2;
                            }
                        }
                        row[x] = uint8_t(acc / (uint64_t(bx) * by));
                    }
                    fwrite(row.data(), 1, gw, gray);
                }
                imm->Unmap(staging.Get(), 0);
            }
        }
        if (api->presented(core, f.lease, imm.Get()) != PUSHCORE_OK) { std::cout << "presented failed: " << api->lastError(core) << "\n"; return false; }
        if (snap) {   // picture check only: CPU readback of the backbuffer copy
            imm->CopyResource(staging.Get(), backbuffer.Get());
            D3D11_MAPPED_SUBRESOURCE map{};
            if (SUCCEEDED(imm->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
                const auto name = std::format(L"frame{:05}-{}-{}.png", f.sourceIndex, f.subframe ? f.subframe - 1 : (f.sourceIndex ? m - 1 : 0),
                                              f.kind == PUSHCORE_FRAME_REAL ? L"real" : L"generated");
                if (oi.dxgiFormat == 24) {   // R10G10B10A2 (HDR10): the raw 10-bit words, for precision checks
                    std::wstring raw = (outDir / name).wstring(); raw.replace(raw.size() - 3, 3, L"r10");
                    if (FILE* fp = _wfopen(raw.c_str(), L"wb")) {
                        const uint32_t dims[2] = { oi.width, oi.height }; fwrite(dims, 4, 2, fp);
                        for (uint32_t y = 0; y < oi.height; ++y) fwrite(static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch, 4, oi.width, fp);
                        fclose(fp); ++pngs;
                    } else ++pngFail;
                } else if (savePng((outDir / name).wstring(), static_cast<const uint8_t*>(map.pData), oi.width, oi.height, map.RowPitch)) ++pngs; else ++pngFail;
                imm->Unmap(staging.Get(), 0);
            } else ++pngFail;
        }
        if (checkMotion && oi.dxgiFormat == 28) {   // G10 synthetic check: every output read back (RGBA8)
            imm->CopyResource(staging.Get(), backbuffer.Get());
            D3D11_MAPPED_SUBRESOURCE map{};
            if (SUCCEEDED(imm->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) {
                std::vector<uint8_t> img(size_t(oi.width) * oi.height * 4);
                for (uint32_t y = 0; y < oi.height; ++y) memcpy(&img[size_t(y) * oi.width * 4], static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch, size_t(oi.width) * 4);
                imm->Unmap(staging.Get(), 0);
                checkOutput(f, std::move(img));
            }
        }
        imm->Flush();
        const auto now = Clock::now();
        double lateMs = 0;
        if (paced && clockRunning) { lateMs = msBetween(clockStart, now) - (leadMs + double(f.pts100ns - clockOrigin) / 1e4); lateness.push_back(lateMs); if (lateMs > outIntervalMs / 2) ++late; }
        // checks
        if (f.pts100ns <= lastPts) ++orderErrors;
        lastPts = f.pts100ns;
        if (heldOn && f.sourceIndex < sourcePts.size()) {
            // re-timing: a real output at its source time; a generated one between the real output before it and its
            // batch's real frame (the pair may span held frames)
            const int64_t b = sourcePts[f.sourceIndex];
            if (f.kind == PUSHCORE_FRAME_REAL) { if (f.pts100ns != b) ++ptsErrors; lastRealPts = b; }
            else if (lastRealPts == INT64_MIN || f.pts100ns <= lastRealPts || f.pts100ns >= b) ++ptsErrors;
            ++perSource[f.sourceIndex];
            if (prevOutPts != INT64_MIN) steps.push_back(f.pts100ns - prevOutPts);
            prevOutPts = f.pts100ns;
        } else if (f.sourceIndex < sourcePts.size()) {
            const int64_t b = sourcePts[f.sourceIndex];
            if (f.kind == PUSHCORE_FRAME_REAL) { if (f.pts100ns != b) ++ptsErrors; }
            else if (f.sourceIndex == 0) ++ptsErrors;
            else {
                const int64_t a = sourcePts[f.sourceIndex - 1], d = b - a;
                const int64_t expect = a + (d / m) * f.subframe + ((d % m) * f.subframe + m / 2) / m;   // FrameBatch::interpolate
                if (f.pts100ns != expect) ++ptsErrors;
            }
            ++perSource[f.sourceIndex];
        } else ++ptsErrors;
        (f.kind == PUSHCORE_FRAME_REAL ? real : generated)++;
        csv << std::format("{},{},{},{},{},{:.3f},{:.3f}\n", presentedCount, f.sourceIndex, f.kind == PUSHCORE_FRAME_REAL ? "real" : "generated",
                           f.subframe, f.pts100ns, msBetween(runStart, now), lateMs);
        ++presentedCount;
        if (!steady && f.sourceIndex >= steadyFrom) { steady = true; steadyStart = now; steadyBase = presentedCount; }
        return true;
    };

    PushCoreFrame got[32];
    while (!failed) {
        // ingest: one decoded frame at a time, as soon as the graph has a free slot
        if (!nrLiveSpec.empty() && submitted == uint64_t(_wtoi64(nrLiveSpec.c_str())) && submitted > 0) {
            // --nrlive: the dials change mid-stream; the graph must keep NR's history (no rebuild, no reset)
            const size_t colon = nrLiveSpec.find(L':');
            std::wstring list = colon == std::wstring::npos ? std::wstring() : nrLiveSpec.substr(colon + 1);
            for (size_t p = 0; p < list.size();) {
                const size_t e = list.find(L',', p), q = list.find(L'=', p);
                const std::wstring item = list.substr(p, e == std::wstring::npos ? std::wstring::npos : e - p);
                if (q != std::wstring::npos && q < (e == std::wstring::npos ? list.size() : e)) {
                    const std::wstring name = list.substr(p, q - p), value = list.substr(q + 1, (e == std::wstring::npos ? list.size() : e) - q - 1);
                    const float f = float(_wtof(value.c_str()));
                    if (name == L"intensity") { ps.nrDials |= PUSHCORE_NR_DIAL_INTENSITY; ps.nrIntensity = f; }
                    else if (name == L"tone") ps.nrTone = f;
                    else if (name == L"structure") ps.nrStructure = f;
                    else if (name == L"skin") { ps.nrDials |= PUSHCORE_NR_DIAL_SKIN; ps.nrSkin = f; }
                    else if (name == L"automask") { ps.nrDials |= PUSHCORE_NR_DIAL_AUTO_MASK; ps.nrAutoMask = _wtoi(value.c_str()); }
                    else if (name == L"style") { ps.nrDials |= PUSHCORE_NR_DIAL_STYLE; ps.nrStyle = _wtoi(value.c_str()); }
                    else std::cout << "--nrlive: unknown dial " << utf8(item) << "\n";
                }
                if (e == std::wstring::npos) break;
                p = e + 1;
            }
            const int sr2 = api->setSettings ? api->setSettings(core, &ps) : PUSHCORE_ERROR;
            const int ar = sr2 == PUSHCORE_OK && api->applySettings ? api->applySettings(core) : PUSHCORE_ERROR;
            std::cout << std::format("NRLIVE before source frame {}: setSettings {} applySettings {} ({}; dials 0x{:X} intensity {:.2f} tone {:.2f} structure {:.2f} skin {:.2f} autoMask {} style {})\n",
                                     submitted, sr2, ar, ar == PUSHCORE_OK ? "live" : ar == PUSHCORE_NEEDS_REBUILD ? "needs a rebuild: the live path refused" : "failed",
                                     ps.nrDials, ps.nrIntensity, ps.nrTone, ps.nrStructure, ps.nrSkin, ps.nrAutoMask, ps.nrStyle);
            nrLiveSpec.clear();
        }
        if (haveFrame && submitted < maxFrames && api->canSubmit(core)) {
            const int64_t pts = synthetic ? int64_t(synIndex) * 333333 + 10000000 : av_rescale_q(decoded->best_effort_timestamp, st->time_base, AVRational{1, 10000000});
            const auto t = Clock::now();
            const int r = synthetic ? api->submit(core, synTex.Get(), 0, pts, submitted == 0 ? PUSHCORE_SUBMIT_DISCONTINUITY : 0)
                                    : api->submit(core, reinterpret_cast<ID3D11Texture2D*>(decoded->data[0]), uint32_t(reinterpret_cast<intptr_t>(decoded->data[1])),
                                                  pts, submitted == 0 ? PUSHCORE_SUBMIT_DISCONTINUITY : 0);
            submitMs += msBetween(t, Clock::now());
            if (r == PUSHCORE_ERROR) { std::cout << "SUBMIT FAILED at " << submitted << ": " << api->lastError(core) << "\n"; failed = true; break; }
            if (r == PUSHCORE_OK) {
                sourcePts.push_back(pts); perSource.push_back(0); ++submitted;
                if (synthetic) ++synIndex; else av_frame_unref(decoded);
                haveFrame = submitted < maxFrames && decodeNext();
                continue;
            }
            ++busy;
        }
        const bool moreToSubmit = haveFrame && submitted < maxFrames;
        uint32_t n = 0;
        if (api->poll(core, got, 32, &n, queue.empty() ? 2 : 0) != PUSHCORE_OK) { std::cout << "POLL FAILED: " << api->lastError(core) << "\n"; failed = true; break; }
        for (uint32_t i = 0; i < n; ++i) queue.push_back(got[i]);
        // present
        while (!queue.empty()) {
            if (paced && !clockRunning && queue.front().sourceIndex >= pacedFrom) { clockRunning = true; clockStart = Clock::now(); clockOrigin = queue.front().pts100ns; }
            if (paced && clockRunning) {
                const double due = leadMs + double(queue.front().pts100ns - clockOrigin) / 1e4;
                const double now = msBetween(clockStart, Clock::now());
                if (now < due) {
                    if (due - now > 0.3) {   // high-resolution timer, not Sleep
                        LARGE_INTEGER t; t.QuadPart = -int64_t((due - now - 0.2) * 10000);
                        SetWaitableTimer(timer, &t, 0, nullptr, nullptr, FALSE);
                        WaitForSingleObject(timer, INFINITE);
                    }
                    break;
                }
            }
            if (!present(queue.front())) { failed = true; break; }
            queue.pop_front();
        }
        PushCoreStats s{}; s.structSize = sizeof(s); api->stats(core, &s);
        subtitleMax = std::max(subtitleMax, s.subtitleHoldPixels); subtitleSamples += s.subtitleHoldPixels > 0 ? 1 : 0;
        if (!moreToSubmit && queue.empty() && s.pendingFrames == 0 && s.leasesOut == 0) break;
        if (msBetween(runStart, Clock::now()) > 600000) { std::cout << "TIMEOUT\n"; failed = true; break; }
    }
    const auto runEnd = Clock::now();
    const double wallS = msBetween(runStart, runEnd) / 1000;
    const double steadyS = steady ? msBetween(steadyStart, runEnd) / 1000 : 0;
    const double steadyFps = steadyS > 0 ? double(presentedCount - steadyBase) / steadyS : 0;

    std::vector<PushCoreGpuTiming> timings(16384);
    uint32_t tn = 0;
    api->takeGpuTimings(core, timings.data(), uint32_t(timings.size()), &tn);
    timings.resize(tn);
    std::vector<double> srV, fgV, graphV;
    std::ofstream tcsv(outDir / L"gpu-timings.csv");
    tcsv << "sourceFrameId,colorMs,srMs,fgBatchMs,fg1Ms,graphMs\n";
    for (const auto& t : timings) {
        tcsv << std::format("{},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f}\n", t.sourceFrameId, t.colorMs, t.srMs, t.fgBatchMs, t.fg1Ms, t.graphMs);
        if (t.sourceFrameId <= warmup) continue;   // ids are 1-based; skip warm-up
        srV.push_back(t.srMs); fgV.push_back(t.fgBatchMs); graphV.push_back(t.graphMs);
    }
    const Agg srA = aggregate(srV), fgA = aggregate(fgV), grA = aggregate(graphV);
    // animeSrModel preset to none: a pushcore without the field (or without the anime upscaler) leaves it so
    PushCoreStats s{}; s.structSize = sizeof(s); s.animeSrModel = ~0u; api->stats(core, &s);

    if (gray) fclose(gray);
    uint64_t countMismatch = 0;
    if (!heldOn) for (size_t i = 0; i < perSource.size(); ++i) if (perSource[i] != (i == 0 ? 1u : m)) ++countMismatch;
    const uint64_t expected = heldOn ? presentedCount : submitted ? 1 + (submitted - 1) * m : 0;
    if (heldOn) {
        // output steps on the 1/k grid of the source interval: on-grid share, the step histogram
        const double grid = sourcePts.size() > 1 ? double(sourcePts.back() - sourcePts.front()) / double(sourcePts.size() - 1) / m : 1;
        std::map<long, uint64_t> hist; uint64_t onGrid = 0;
        for (int64_t d : steps) { const double u = double(d) / grid; const long r = std::lround(u); if (std::fabs(u - double(r)) < 0.05) ++onGrid; ++hist[r]; }
        std::string h;
        for (auto& kv : hist) h += std::format(" {}:{}", kv.first, kv.second);
        uint64_t zeroSources = 0; for (size_t i = 1; i < perSource.size(); ++i) zeroSources += perSource[i] == 0;
        std::cout << std::format("EVENNESS grid {:.3f} ms; steps {} on grid {:.1f} %; steps in grid units (units:count){}; source frames with no output (held, re-timed) {}; held stats: found {} dropped {} spans {} ratio {:.1f} % of {} graph x{}; source repair {} (frames {}, parts {})\n",
                                 grid / 1e4, steps.size(), steps.empty() ? 0.0 : 100.0 * onGrid / steps.size(), h, zeroSources,
                                 s.heldFrames, s.heldDropped, s.heldSpans, s.heldRatioPermille / 10.0, s.heldTested, s.heldGraphMultiplier, s.repairActive ? "on" : "off", s.repairedFrames, s.repairedParts);
    }
    std::cout << std::format(
        "RESULT source={} outputs={} real={} generated={} skippedGenerated={} expectedOutputs={} countMismatches={} orderErrors={} ptsErrors={} sizeErrors={} pngs={} pngFailures={}\n",
        submitted, presentedCount, real, generated, s.skippedGenerated, expected, countMismatch, orderErrors, ptsErrors, sizeErrors, pngs, pngFail);
    std::cout << std::format("CUTS held by the cut guard (pushcore stats, read back): {}\n", s.cutGuardCuts);
    std::cout << std::format("SUBTITLE HOLD state {} (0 off, 1 running, 2 unavailable, 3 not in this graph); pixels held: last {}, max {}; loop samples with pixels {}\n",
                             s.subtitleHoldState, s.subtitleHoldPixels, subtitleMax, subtitleSamples);
    std::cout << std::format(
        "TIMING gpu (D3D12 timestamps, source frames > {}): VSR mean {:.2f} median {:.2f} p95 {:.2f} ms (n={}); VFG batch mean {:.2f} median {:.2f} p95 {:.2f} ms (n={}); graph span median {:.2f} p95 {:.2f} ms\n",
        warmup, srA.mean, srA.median, srA.p95, srA.n, fgA.mean, fgA.median, fgA.p95, fgA.n, grA.median, grA.p95);
    std::cout << std::format(
        "TIMING throughput: {} steady state (outputs of source >= {}): {:.1f} output fps ({:.2f} source fps); wall {:.1f} s; open {:.0f} ms; "
        "submit CPU mean {:.2f} ms; decode mean {:.2f} ms; busy={}; egress={}\n",
        paced ? "paced" : "unpaced", steadyFrom, steadyFps, steadyFps / m, wallS, openMs, submitted ? submitMs / submitted : 0,
        decodedCount ? decodeMs / decodedCount : 0, busy, oi.egressMode == PUSHCORE_EGRESS_DIRECT ? "direct" : "copy");
    if (paced) {
        const Agg l = aggregate([&] { std::vector<double> v; for (double x : lateness) v.push_back(x + 1000); return v; }());
        std::cout << std::format("TIMING paced: lateness median {:.2f} p95 {:.2f} ms (target 0, interval {:.2f} ms); late>half-interval {}\n",
                                 l.median - 1000, l.p95 - 1000, outIntervalMs, late);
    }
    bool checkPass = true;
    if (checkMotion) {
        std::vector<double> e = chk.posErr; std::sort(e.begin(), e.end());
        const double med = e.empty() ? -1 : e[e.size() / 2], p90 = e.empty() ? -1 : e[std::min(e.size() - 1, size_t(0.9 * double(e.size())))];
        const double tol = 2.0 * std::max(1.0, double(oi.width) / double(si.width));   // 2 source pixels
        checkPass = m == 1 || (!e.empty() && med <= tol && (!cutAt || cutAt >= submitted || (chk.holdsBad == 0 && chk.holdsOk > 0)));
        std::cout << std::format("CHECK generated frames checked {}: square position error median {:.2f} p90 {:.2f} px (tolerance {:.1f}), no square {}; cut pair {}: held {} not held {}; "
                                 "cuts counted {}\n", chk.gens, med, p90, tol, chk.noSquare, cutAt, chk.holdsOk, chk.holdsBad, s.cutGuardCuts);
    }
    std::cout << std::format("METHOD frame generation {} (fallback {}: {}), upscaler {}\n", s.fgBackend, s.fgFallback, s.fgNote, s.srMethod);
    const bool animeOk = !animeWanted || (s.srMethod == PUSHCORE_SR_METHOD_PMML && s.animeSrModel == ps.animeSrModel && s.animeSrMs > 0);
    if (animeWanted)
        std::cout << std::format("ANIME requested model {}: running model {} (upscaler {}){}\n", ps.animeSrModel, int(s.animeSrModel), s.srMethod,
                                 animeOk ? "" : ": NOT the anime upscaler");
    const bool pass = !failed && submitted > 1 && countMismatch == 0 && orderErrors == 0 && ptsErrors == 0 && sizeErrors == 0 &&
                      presentedCount == expected && (m == 1 || generated > 0) && checkPass && animeOk;
    std::cout << (pass ? "PASS" : "FAIL") << "\n";

    if (idleMs) { std::cout << "IDLE " << idleMs << " ms (core open, no frames)" << std::endl; Sleep(idleMs); std::cout << "IDLE done" << std::endl; }
    api->close(core);
    av_frame_free(&decoded); av_packet_free(&pkt);
    avcodec_free_context(&dec); av_buffer_unref(&hwdev); avformat_close_input(&fmt);
    CoUninitialize();
    return pass ? 0 : 1;
}
