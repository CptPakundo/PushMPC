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
//            [--shaders DIR]
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
    unsigned maxFrames = 120, warmup = 10;
    double start = 1752;
    bool paced = false, copy = false;
    std::set<uint64_t> save{0, 1, 2, 60};
    int sr = -1, fgOpt = -1, mult = -1;
    // PushMPC's own settings instead of a chain session (--backend vfg|dlssg|pushflow|fsr|frc|none): the filter's route
    bool useSettings = false;
    PushCoreSettings ps{};
    ps.structSize = sizeof(ps); ps.source = PUSHCORE_SETTINGS_PUSHMPC; ps.sr = 1; ps.srQuality = 4; ps.srTarget = PUSHCORE_SR_TARGET_4K;
    ps.multiplier = 4; ps.vfgQuality = 1; ps.nrTone = ps.nrStructure = 1; ps.fgBackend = PUSHCORE_FG_VFG; ps.fgTier = 1;
    ps.srMethod = PUSHCORE_SR_METHOD_RTX; ps.cutGuard = 1; ps.debandStrength = PUSHCORE_DEBAND_MEDIUM;
    for (int i = 3; i + 1 < argc; i += 2) {
        const std::wstring k = argv[i], v = argv[i + 1];
        if (k == L"--backend") {
            useSettings = true;
            ps.fgBackend = v == L"dlssg" ? PUSHCORE_FG_DLSSG : v == L"pushflow" ? PUSHCORE_FG_PUSHFLOW : v == L"fsr" ? PUSHCORE_FG_FSR
                         : v == L"frc" ? PUSHCORE_FG_AMDFRC : PUSHCORE_FG_VFG;
            if (v == L"none") ps.multiplier = 1;
            continue;
        }
        if (k == L"--q") { ps.vfgQuality = uint32_t(_wtoi(v.c_str())); ps.fgTier = ps.vfgQuality; continue; }
        if (k == L"--srq") { const int q = _wtoi(v.c_str()); ps.sr = q > 0; ps.srQuality = q > 0 ? uint32_t(q) : 2u; continue; }
        if (k == L"--srmethod") { ps.srMethod = v == L"fsr" ? PUSHCORE_SR_METHOD_FSR : v == L"nis" ? PUSHCORE_SR_METHOD_NIS : PUSHCORE_SR_METHOD_RTX; continue; }
        if (k == L"--target") { ps.srTarget = v == L"1440p" ? PUSHCORE_SR_TARGET_1440P : v == L"5k" ? PUSHCORE_SR_TARGET_5K : PUSHCORE_SR_TARGET_4K; continue; }
        if (k == L"--cutguard") { ps.cutGuard = _wtoi(v.c_str()); continue; }
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

    // --- D3D11 device on the NVIDIA adapter, as LAV/MPC-VR would have it ---
    ComPtr<IDXGIFactory1> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    ComPtr<IDXGIAdapter1> adapter, best;
    SIZE_T bestMem = 0;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{}; adapter->GetDesc1(&d);
        if (d.VendorId == 0x10DE && d.DedicatedVideoMemory > bestMem) { best = adapter; bestMem = d.DedicatedVideoMemory; }
    }
    if (!best) { std::cout << "no NVIDIA adapter\n"; return 1; }
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> imm;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(best.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                 levels, 2, D3D11_SDK_VERSION, &device, nullptr, &imm))) { std::cout << "D3D11CreateDevice failed\n"; return 1; }
    { ComPtr<ID3D10Multithread> mt; if (SUCCEEDED(device.As(&mt))) mt->SetMultithreadProtected(TRUE); }
    ComPtr<ID3D11DeviceContext4> imm4; imm.As(&imm4);
    { DXGI_ADAPTER_DESC1 d{}; best->GetDesc1(&d); std::cout << "D3D11 device on " << utf8(d.Description) << std::format(" LUID {:08X}:{:08X}\n", unsigned(d.AdapterLuid.HighPart), d.AdapterLuid.LowPart); }

    // --- D3D11VA decode on that device ---
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, utf8(input).c_str(), nullptr, nullptr) < 0 || avformat_find_stream_info(fmt, nullptr) < 0) { std::cout << "cannot open input\n"; return 1; }
    const int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs < 0) { std::cout << "no video stream\n"; return 1; }
    AVStream* st = fmt->streams[vs];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext* dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec, st->codecpar);
    AVBufferRef* hwdev = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    auto* hwctx = reinterpret_cast<AVD3D11VADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(hwdev->data)->hwctx);
    hwctx->device = device.Get(); device->AddRef();   // FFmpeg releases it
    if (av_hwdevice_ctx_init(hwdev) < 0) { std::cout << "D3D11VA device init failed\n"; return 1; }
    dec->hw_device_ctx = av_buffer_ref(hwdev);
    dec->get_format = pickD3D11;
    dec->extra_hw_frames = 4;
    if (!codec || avcodec_open2(dec, codec, nullptr) < 0) { std::cout << "no decoder\n"; return 1; }
    if (start > 0) av_seek_frame(fmt, -1, int64_t(start * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* decoded = av_frame_alloc();
    bool eof = false;
    double decodeMs = 0; uint64_t decodedCount = 0;
    auto decodeNext = [&]() -> bool {   // leaves a D3D11 frame in `decoded`
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
    if (decoded->format != AV_PIX_FMT_D3D11) { std::cout << "decoder did not produce D3D11 frames\n"; return 1; }
    const AVPixelFormat sw = AVPixelFormat(reinterpret_cast<AVHWFramesContext*>(decoded->hw_frames_ctx->data)->sw_format);
    {
        D3D11_TEXTURE2D_DESC d{}; reinterpret_cast<ID3D11Texture2D*>(decoded->data[0])->GetDesc(&d);
        std::cout << std::format("decoder: D3D11VA {}x{} texture array {}x{}x{} format {} sw {}\n", decoded->width, decoded->height, d.Width, d.Height,
                                 d.ArraySize, int(d.Format), av_get_pix_fmt_name(sw));
    }

    PushCoreStreamInfo si{};
    si.structSize = sizeof(si);
    si.width = uint32_t(decoded->width); si.height = uint32_t(decoded->height);
    si.format = sw == AV_PIX_FMT_P010 ? PUSHCORE_INPUT_P010 : PUSHCORE_INPUT_NV12;
    si.fpsNum = st->avg_frame_rate.num; si.fpsDen = st->avg_frame_rate.den ? st->avg_frame_rate.den : 1;
    si.range = decoded->color_range; si.matrix = decoded->colorspace; si.primaries = decoded->color_primaries; si.transfer = decoded->color_trc;
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
        std::cout << std::format("settings: backend {} x{} quality/tier {} SR {} (method {}, level {}) cut guard {}\n", ps.fgBackend, ps.multiplier,
                                 ps.vfgQuality, ps.sr, ps.srMethod, ps.srQuality, ps.cutGuard);
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
    int64_t lastPts = INT64_MIN;
    bool haveFrame = true, failed = false;
    std::vector<double> lateness;
    Clock::time_point clockStart{}; int64_t clockOrigin = 0; bool clockRunning = false;
    const double leadMs = 60;    // paced: first paced output is due this long after the clock starts
    const uint64_t pacedFrom = 4; // paced: sources before this (graph and VFG warm-up) are shown at once, then the clock starts
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const double outIntervalMs = si.fpsNum > 0 ? 1000.0 * si.fpsDen / si.fpsNum / m : 10;
    std::ofstream csv(outDir / L"outputs.csv");
    csv << "output,source,kind,subframe,pts100ns,presentMs,latenessMs\n";
    const auto runStart = Clock::now();
    Clock::time_point steadyStart{}; uint64_t steadyBase = 0; bool steady = false;
    const uint64_t lastSave = save.empty() ? 0 : *save.rbegin();
    const uint64_t steadyFrom = std::max<uint64_t>(warmup, lastSave + 4);
    double submitMs = 0;

    auto present = [&](const PushCoreFrame& f) -> bool {
        if (f.width != oi.width || f.height != oi.height || !f.texture) ++sizeErrors;
        imm4->Wait(f.readyFence, f.readyValue);           // GPU-side wait for the graph's output
        imm->CopyResource(backbuffer.Get(), f.texture);   // the presenter's one pass
        const bool snap = save.count(f.sourceIndex) != 0;
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
        imm->Flush();
        const auto now = Clock::now();
        double lateMs = 0;
        if (paced && clockRunning) { lateMs = msBetween(clockStart, now) - (leadMs + double(f.pts100ns - clockOrigin) / 1e4); lateness.push_back(lateMs); if (lateMs > outIntervalMs / 2) ++late; }
        // checks
        if (f.pts100ns <= lastPts) ++orderErrors;
        lastPts = f.pts100ns;
        if (f.sourceIndex < sourcePts.size()) {
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
        if (haveFrame && submitted < maxFrames && api->canSubmit(core)) {
            const int64_t pts = av_rescale_q(decoded->best_effort_timestamp, st->time_base, AVRational{1, 10000000});
            const auto t = Clock::now();
            const int r = api->submit(core, reinterpret_cast<ID3D11Texture2D*>(decoded->data[0]), uint32_t(reinterpret_cast<intptr_t>(decoded->data[1])),
                                      pts, submitted == 0 ? PUSHCORE_SUBMIT_DISCONTINUITY : 0);
            submitMs += msBetween(t, Clock::now());
            if (r == PUSHCORE_ERROR) { std::cout << "SUBMIT FAILED at " << submitted << ": " << api->lastError(core) << "\n"; failed = true; break; }
            if (r == PUSHCORE_OK) {
                sourcePts.push_back(pts); perSource.push_back(0); ++submitted;
                av_frame_unref(decoded);
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
    PushCoreStats s{}; s.structSize = sizeof(s); api->stats(core, &s);

    uint64_t countMismatch = 0;
    for (size_t i = 0; i < perSource.size(); ++i) if (perSource[i] != (i == 0 ? 1u : m)) ++countMismatch;
    const uint64_t expected = submitted ? 1 + (submitted - 1) * m : 0;
    std::cout << std::format(
        "RESULT source={} outputs={} real={} generated={} skippedGenerated={} expectedOutputs={} countMismatches={} orderErrors={} ptsErrors={} sizeErrors={} pngs={} pngFailures={}\n",
        submitted, presentedCount, real, generated, s.skippedGenerated, expected, countMismatch, orderErrors, ptsErrors, sizeErrors, pngs, pngFail);
    std::cout << std::format("CUTS held by the cut guard (pushcore stats, read back): {}\n", s.cutGuardCuts);
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
    const bool pass = !failed && submitted > 1 && countMismatch == 0 && orderErrors == 0 && ptsErrors == 0 && sizeErrors == 0 &&
                      presentedCount == expected && (m == 1 || generated > 0);
    std::cout << (pass ? "PASS" : "FAIL") << "\n";

    api->close(core);
    av_frame_free(&decoded); av_packet_free(&pkt);
    avcodec_free_context(&dec); av_buffer_unref(&hwdev); avformat_close_input(&fmt);
    CoUninitialize();
    return pass ? 0 : 1;
}
