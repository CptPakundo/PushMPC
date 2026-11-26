#include "pushmpc/PushCore.h"

#include "veyra/engine/EngineController.h"   // PlayerOptions
#include "veyra/engine/GraphDescription.h"   // StageRequest, describeStages
#include "veyra/engine/EffectChain.h"
#include "veyra/engine/PresetLibrary.h"      // ChainSessionStore
#include "veyra/pipeline/EnhanceGraph.h"
#include "veyra/gfx/D3D12DeviceContext.h"
#include "veyra/gfx/CommandSlotRing.h"
#include "veyra/sink/ImageExportSink.h"      // readRgba8
#include "veyra/Log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace pushmpc {
namespace {
using clock = std::chrono::steady_clock;
double msSince(clock::time_point t) { return std::chrono::duration<double, std::milli>(clock::now() - t).count(); }
std::string narrow(const std::wstring& s) { return std::filesystem::path(s).string(); }
}  // namespace

struct Core::Impl {
    veyra::gfx::D3D12DeviceContext ctx;
    veyra::gfx::CommandSlotRing ring;
    std::unique_ptr<veyra::pipeline::EnhanceGraph> graph;
    veyra::pipeline::EnhanceGraphDesc gd;
    veyra::engine::PlayerOptions options;
    StreamInfo stream;
    AVFrame* frame = nullptr;
    uint32_t multiplier = 1;
    bool fg = false;
    bool haveReal = false;
    int64_t previousPts = 0;
    uint64_t sequence = 0;
    std::vector<uint8_t> lastReal;  // for holds
    CoreStats stats;
    std::string error, description;
    bool opened = false;

    bool fail(std::string message) {
        error = std::move(message);
        veyra::log::error("pushcore", error);
        return false;
    }

    bool loadSettings(const CoreConfig& cfg, veyra::engine::EnhancementSettings& s,
                      std::optional<veyra::engine::ChainRuntimeOrder>& order) {
        if (cfg.dataDir.empty()) return true;
        auto session = veyra::engine::ChainSession::initial(s);
        veyra::engine::ChainSessionStore store(std::filesystem::path(cfg.dataDir) / L"chain-session.v1");
        if (!store.load(session)) return fail("chain session not loaded: " + narrow(store.error()));
        const auto& configuration = session.configurations[size_t(session.active)];
        configuration.apply(s);
        order = veyra::engine::runtimeOrder(configuration.chain);
        return true;
    }

    bool open(const CoreConfig& cfg, const StreamInfo& info) {
        using namespace veyra;
        stream = info;
        if (!info.width || !info.height) return fail("no source size");
        Status st = Status::Ok;
        gfx::DeviceContextDesc dd;
        dd.commandSlotCount = 6;
        if (!ctx.initialize(dd, st) || !ring.initialize(ctx.device(), ctx.directQueue(), ctx.fence(), ctx.fenceEvent(), 6, st))
            return fail("no D3D12 device the engine accepts");
        const bool nvidia = ctx.adapter().isNvidia;

        engine::EnhancementSettings s;
        std::optional<engine::ChainRuntimeOrder> order;
        if (!loadSettings(cfg, s, order)) return false;
        options = engine::PlayerOptions::from(s, order);
        if (cfg.sr >= 0) options.sr = cfg.sr != 0;
        if (cfg.fg >= 0) options.fg = cfg.fg != 0;
        if (cfg.multiplier >= 2) options.fgMultiplier = uint32_t(std::clamp(cfg.multiplier, 2, int(veyra::pipeline::FrameBatch::Capacity)));
        if (cfg.vfgQuality >= 0) options.settings.vfgQuality = uint32_t(cfg.vfgQuality);
        if (!nvidia) return fail("pushcore M1 expects an NVIDIA adapter");

        // Source format, as the exporter fills it from the opened file.
        const bool hdrTransfer = info.transfer == AVCOL_TRC_SMPTE2084 || info.transfer == AVCOL_TRC_ARIB_STD_B67;
        gd = {};
        gd.hdrInput = hdrTransfer;
        gd.videoHdr = options.settings.videoHdr;
        gd.videoHdr.enabled = false;  // M1: SDR in, SDR out; RTX Video HDR and HDR10 come in M4
        gd.hdrOutput = gd.hdrInput;
        if (gd.hdrOutput) return fail("pushcore M1 handles SDR sources only");
        gd.captureBitDepth = info.format == InputFormat::P010 ? 10 : 8;

        engine::StageRequest stages;
        stages.nr = options.nr; stages.sr = options.sr; stages.fg = options.fg; stages.fgMultiplier = options.fgMultiplier;
        stages.width = info.width; stages.height = info.height;
        stages.exportJob = true;       // file rules: no present-sink FG, selected NR size
        stages.nvidiaAdapter = nvidia;
        stages.nodeOrder = options.nodeOrder ? &*options.nodeOrder : nullptr;
        engine::describeStages(stages, options.snapshot(), gd);
        gd.dlssgLite = false;
        gd.runtimeAbsPath = cfg.runtimeDir;

        if (options.fg && engine::presentSinkFrameGeneration(options.settings.frameGenerationBackend))
            return fail("the saved frame generation method presents on its own (XeSS) and cannot feed a filter");

        graph = std::make_unique<pipeline::EnhanceGraph>(ctx, ring);
        fg = options.fg;
        multiplier = fg ? options.fgMultiplier : 1;
        gd.enableFg = fg;
        gd.fgMultiplier = multiplier;
        if (!graph->initialize(gd) || !graph->createViews())
            return fail(std::format("enhancement graph did not start (failed backend {})", int(graph->failedBackend())));

        frame = av_frame_alloc();
        if (!frame) return fail("no AVFrame");
        frame->format = info.format == InputFormat::P010 ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
        frame->width = int(info.width); frame->height = int(info.height);
        frame->color_range = AVColorRange(info.range);
        frame->colorspace = AVColorSpace(info.matrix);
        frame->color_primaries = AVColorPrimaries(info.primaries);
        frame->color_trc = AVColorTransferCharacteristic(info.transfer);

        description = std::format(
            "source {}x{} {} -> output {}x{}; SR {} (videoSrQuality {}); FG {} {} {}X (vfgQuality {}); NR {}",
            info.width, info.height, info.format == InputFormat::P010 ? "P010" : "NV12", gd.workWidth, gd.workHeight,
            gd.enableSr ? "on" : "off", gd.videoSrQuality, fg ? "on" : "off",
            engine::frameGenerationBackendName(options.settings.frameGenerationBackend), multiplier, gd.vfgQuality,
            gd.enableNr ? "on" : "off");
        veyra::log::info("pushcore", description);
        opened = true;
        return true;
    }

    bool read(ID3D12Resource* texture, OutputFrame& o) {
        const auto t = clock::now();
        veyra::sink::RgbaImage image;
        if (!texture || !veyra::sink::readRgba8(ctx, ring, texture, image)) return fail("readback failed");
        o.width = image.width; o.height = image.height; o.format = OutputFormat::RGBA8;
        o.pixels = std::move(image.pixels);
        stats.readbackMs += msSince(t);
        return true;
    }

    bool push(const InputFrame& in, std::vector<OutputFrame>& out) {
        using namespace veyra;
        if (!opened) return fail("not open");
        for (int p = 0; p < 2; ++p) {
            frame->data[p] = const_cast<uint8_t*>(in.planes[p]);
            frame->linesize[p] = in.strides[p];
        }
        const bool reset = in.discontinuity || !haveReal;
        const auto t = clock::now();
        pipeline::EnhanceGraph::FrameOutputs result;
        if (!graph->process(frame, double(in.pts100ns) / 10000.0, reset, result, ++sequence, nullptr, nullptr, false))
            return fail(std::format("graph process failed (failed backend {})", int(graph->failedBackend())));
        if (!ctx.waitForFenceValue(std::max(result.videoFenceValue, result.genFenceValue), 30000) || !graph->resolveGeneration(result))
            return fail("GPU did not finish the frame within 30 s");
        const double ms = msSince(t);
        stats.processMs += ms;
        stats.slowestProcessMs = std::max(stats.slowestProcessMs, ms);

        if (!reset && fg) {
            for (uint32_t j = 1; j < multiplier; ++j) {
                OutputFrame o;
                o.pts100ns = previousPts + (in.pts100ns - previousPts) * j / multiplier;
                const pipeline::BatchFrame* item = nullptr;
                for (uint32_t k = 0; k < result.batch.count; ++k)
                    if (result.batch.frames[k].subframe == j && result.batch.frames[k].validity == pipeline::GenerationValidity::Valid)
                        item = &result.batch.frames[k];
                if (item) {
                    o.kind = OutputFrame::Kind::Generated;
                    if (!read(item->lease->texture.Get(), o)) return false;
                    ++stats.generated;
                } else {
                    o.kind = OutputFrame::Kind::Hold;
                    o.width = gd.workWidth; o.height = gd.workHeight;
                    o.pixels = lastReal;
                    ++stats.holds;
                }
                out.push_back(std::move(o));
            }
        }
        const auto& real = result.batch.frames[result.batch.count - 1];
        OutputFrame o;
        o.pts100ns = in.pts100ns;
        o.kind = OutputFrame::Kind::Real;
        if (!read(real.lease->texture.Get(), o)) return false;
        lastReal = o.pixels;
        out.push_back(std::move(o));
        previousPts = in.pts100ns;
        haveReal = true;
        ++stats.source;
        return true;
    }

    void close() {
        if (graph) { ring.drainQueue(); graph->shutdown(); graph.reset(); }
        if (ctx.initialized()) { ring.shutdown(); ctx.shutdown(); }
        if (frame) { for (auto& d : frame->data) d = nullptr; av_frame_free(&frame); }
        opened = false;
    }
};

Core::Core() : impl_(std::make_unique<Impl>()) {}
Core::~Core() { impl_->close(); }
bool Core::open(const CoreConfig& c, const StreamInfo& s) { return impl_->open(c, s); }
bool Core::push(const InputFrame& f, std::vector<OutputFrame>& o) { return impl_->push(f, o); }
void Core::close() { impl_->close(); }
std::string Core::describe() const { return impl_->description; }
uint32_t Core::outputWidth() const { return impl_->gd.workWidth; }
uint32_t Core::outputHeight() const { return impl_->gd.workHeight; }
uint32_t Core::multiplier() const { return impl_->multiplier; }
const CoreStats& Core::stats() const { return impl_->stats; }
const std::string& Core::error() const { return impl_->error; }

}  // namespace pushmpc
