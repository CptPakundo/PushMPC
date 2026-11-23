// pushcore_harness: decodes a video file to NV12/P010 in system memory (what LAV Video Decoder hands a
// DirectShow filter), pushes every frame through pushmpc::Core, and checks what comes out:
// frame counts, frame kinds, strictly rising timestamps at source rate x multiplier, timing.
// Writes a few frames as PNG so the picture can be looked at.
//
// usage: pushcore_harness <video> <outdir> [--frames N] [--start SECONDS] [--data DIR] [--runtime DIR]
//                         [--sr 0|1] [--fg 0|1] [--multiplier N] [--vfg-quality N] [--save-every N]
#include "pushmpc/PushCore.h"
#include "veyra/sink/ImageExportSink.h"
#include "veyra/RuntimePaths.h"
#include "veyra/Log.h"

#include <windows.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

namespace {
std::string utf8(const std::wstring& s) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string r(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n, nullptr, nullptr);
    return r;
}
const char* kindName(pushmpc::OutputFrame::Kind k) {
    return k == pushmpc::OutputFrame::Kind::Real ? "real" : k == pushmpc::OutputFrame::Kind::Generated ? "generated" : "hold";
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::cout << "usage: pushcore_harness <video> <outdir> [--frames N] [--start S] [--data DIR] [--runtime DIR]"
                     " [--sr 0|1] [--fg 0|1] [--multiplier N] [--vfg-quality N] [--save-every N]\n";
        return 2;
    }
    const std::wstring input = argv[1];
    const std::filesystem::path outDir = argv[2];
    unsigned maxFrames = 240, saveEvery = 0;
    double start = 0;
    pushmpc::CoreConfig cfg;
    cfg.runtimeDir = veyra::runtime::localRuntimeDirectory().wstring();
    cfg.dataDir = (veyra::runtime::localDataDirectory() / L"user-data-2.0.0").wstring();
    for (int i = 3; i + 1 < argc; i += 2) {
        const std::wstring k = argv[i], v = argv[i + 1];
        if (k == L"--frames") maxFrames = unsigned(_wtoi(v.c_str()));
        else if (k == L"--start") start = _wtof(v.c_str());
        else if (k == L"--data") cfg.dataDir = v;
        else if (k == L"--runtime") cfg.runtimeDir = v;
        else if (k == L"--sr") cfg.sr = _wtoi(v.c_str());
        else if (k == L"--fg") cfg.fg = _wtoi(v.c_str());
        else if (k == L"--multiplier") cfg.multiplier = _wtoi(v.c_str());
        else if (k == L"--vfg-quality") cfg.vfgQuality = _wtoi(v.c_str());
        else if (k == L"--save-every") saveEvery = unsigned(_wtoi(v.c_str()));
        else { std::cout << "unknown option " << utf8(k) << "\n"; return 2; }
    }
    std::filesystem::create_directories(outDir);
    veyra::Logger::instance().openFile((outDir / L"pushcore.log").wstring());
    veyra::Logger::instance().setConsoleEnabled(false);
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 2;

    // --- decode setup ---
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, utf8(input).c_str(), nullptr, nullptr) < 0 || avformat_find_stream_info(fmt, nullptr) < 0) {
        std::cout << "cannot open " << utf8(input) << "\n"; return 1;
    }
    const int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs < 0) { std::cout << "no video stream\n"; return 1; }
    AVStream* st = fmt->streams[vs];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext* dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec, st->codecpar);
    dec->thread_count = 0;
    if (!codec || avcodec_open2(dec, codec, nullptr) < 0) { std::cout << "no decoder\n"; return 1; }
    if (start > 0) av_seek_frame(fmt, -1, int64_t(start * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);

    const bool tenBit = av_pix_fmt_desc_get(AVPixelFormat(st->codecpar->format))->comp[0].depth > 8;
    const AVPixelFormat target = tenBit ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
    pushmpc::StreamInfo info;
    info.width = uint32_t(dec->width); info.height = uint32_t(dec->height);
    info.format = tenBit ? pushmpc::InputFormat::P010 : pushmpc::InputFormat::NV12;
    info.fpsNum = st->avg_frame_rate.num; info.fpsDen = st->avg_frame_rate.den ? st->avg_frame_rate.den : 1;
    info.range = dec->color_range; info.matrix = dec->colorspace; info.primaries = dec->color_primaries; info.transfer = dec->color_trc;

    pushmpc::Core core;
    if (!core.open(cfg, info)) { std::cout << "OPEN FAILED: " << core.error() << "\n"; return 1; }
    std::cout << "pushcore: " << core.describe() << "\n";
    const unsigned m = core.multiplier();

    AVFrame* decoded = av_frame_alloc();
    AVFrame* converted = av_frame_alloc();
    converted->format = target; converted->width = dec->width; converted->height = dec->height;
    av_frame_get_buffer(converted, 64);
    SwsContext* sws = nullptr;
    AVPacket* pkt = av_packet_alloc();

    std::vector<pushmpc::OutputFrame> out;
    uint64_t pushed = 0, outputs = 0, real = 0, generated = 0, holds = 0, badOrder = 0, badCount = 0, sizeWrong = 0;
    int64_t lastPts = INT64_MIN;
    bool failed = false, first = true;
    const auto t0 = std::chrono::steady_clock::now();
    double pushMs = 0;

    auto handle = [&](AVFrame* f) -> bool {
        if (start > 0 && f->best_effort_timestamp != AV_NOPTS_VALUE &&
            f->best_effort_timestamp * av_q2d(st->time_base) < start - 0.001) return true;
        sws = sws_getCachedContext(sws, f->width, f->height, AVPixelFormat(f->format), dec->width, dec->height, target,
                                   SWS_POINT, nullptr, nullptr, nullptr);
        av_frame_make_writable(converted);
        sws_scale(sws, f->data, f->linesize, 0, f->height, converted->data, converted->linesize);
        pushmpc::InputFrame in;
        in.planes[0] = converted->data[0]; in.planes[1] = converted->data[1];
        in.strides[0] = converted->linesize[0]; in.strides[1] = converted->linesize[1];
        in.pts100ns = av_rescale_q(f->best_effort_timestamp, st->time_base, AVRational{1, 10000000});
        in.discontinuity = first;
        out.clear();
        const auto tp = std::chrono::steady_clock::now();
        if (!core.push(in, out)) { std::cout << "PUSH FAILED at frame " << pushed << ": " << core.error() << "\n"; failed = true; return false; }
        pushMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tp).count();
        const size_t expected = first ? 1 : m;
        if (out.size() != expected) ++badCount;
        for (size_t k = 0; k < out.size(); ++k) {
            const auto& o = out[k];
            if (o.pts100ns <= lastPts) ++badOrder;
            lastPts = o.pts100ns;
            if (o.width != core.outputWidth() || o.height != core.outputHeight() || o.pixels.size() != size_t(o.width) * o.height * 4) ++sizeWrong;
            real += o.kind == pushmpc::OutputFrame::Kind::Real;
            generated += o.kind == pushmpc::OutputFrame::Kind::Generated;
            holds += o.kind == pushmpc::OutputFrame::Kind::Hold;
            if (pushed < 3 || (saveEvery && pushed % saveEvery == 0)) {
                veyra::sink::RgbaImage img{o.width, o.height, o.pixels};
                const auto name = std::format(L"frame{:05}-{}-{}.png", pushed, k, std::wstring(kindName(o.kind), kindName(o.kind) + strlen(kindName(o.kind))));
                veyra::sink::saveImage((outDir / name).wstring(), img);
            }
            if (pushed < 3) std::cout << std::format("  source {} out {} {} pts {:.4f}s\n", pushed, k, kindName(o.kind), o.pts100ns / 1e7);
        }
        outputs += out.size();
        first = false;
        ++pushed;
        return pushed < maxFrames;
    };

    bool more = true;
    while (more && !failed && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == vs && avcodec_send_packet(dec, pkt) >= 0)
            while (more && avcodec_receive_frame(dec, decoded) >= 0) more = handle(decoded);
        av_packet_unref(pkt);
    }
    const double wallS = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const auto& s = core.stats();
    const double srcFps = info.fpsNum > 0 ? double(info.fpsNum) / info.fpsDen : 0;
    std::cout << std::format(
        "RESULT source={} outputs={} real={} generated={} holds={} expectedOutputs={} countMismatches={} orderErrors={} sizeErrors={}\n",
        pushed, outputs, real, generated, holds, pushed ? 1 + (pushed - 1) * m : 0, badCount, badOrder, sizeWrong);
    std::cout << std::format(
        "TIMING per source frame: push {:.1f} ms (GPU work+wait {:.1f} ms, readback {:.1f} ms), slowest GPU {:.1f} ms; "
        "real-time budget at {:.3f} fps = {:.1f} ms; wall {:.1f} s\n",
        pushed ? pushMs / pushed : 0, s.source ? s.processMs / s.source : 0, s.source ? s.readbackMs / s.source : 0,
        s.slowestProcessMs, srcFps, srcFps > 0 ? 1000.0 / srcFps : 0, wallS);
    const bool pass = !failed && pushed > 1 && badCount == 0 && badOrder == 0 && sizeWrong == 0 && (m == 1 || generated > 0);
    std::cout << (pass ? "PASS" : "FAIL") << "\n";

    core.close();
    sws_freeContext(sws);
    av_packet_free(&pkt); av_frame_free(&decoded); av_frame_free(&converted);
    avcodec_free_context(&dec); avformat_close_input(&fmt);
    CoUninitialize();
    return pass ? 0 : 1;
}
