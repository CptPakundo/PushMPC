#pragma once
// pushcore: PushFPS's enhancement graph (RTX Video SR, NVIDIA VFG and the rest of the chain)
// driven headlessly, one decoded frame in, the real frame plus its in-between frames out.
// No window, swapchain, clock, Qt or capture source: the caller owns timing, as a DirectShow
// filter does. Modelled on PushFPS's own exporter (src/engine/VideoExportJob.cpp).
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pushmpc {

enum class InputFormat { NV12, P010 };
enum class OutputFormat { RGBA8 };  // M1: SDR only; HDR (R10G10B10A2) comes in M4

struct StreamInfo {
    uint32_t width = 0, height = 0;
    InputFormat format = InputFormat::NV12;
    int fpsNum = 0, fpsDen = 1;          // nominal source rate; 0 = unknown
    // FFmpeg enum values (AVColorRange, AVColorSpace, AVColorPrimaries, AVColorTransferCharacteristic);
    // 0 / 2 = unspecified, resolved the way PushFPS resolves a file's frames.
    int range = 0, matrix = 2, primaries = 2, transfer = 2;
};

struct CoreConfig {
    std::wstring runtimeDir;   // NVIDIA/AMD runtimes: <PushFPS>\runtime\experimental
    std::wstring dataDir;      // PushFPS user data holding chain-session.v1; empty = built-in defaults
    // Overrides on top of the saved chain (negative / empty = keep what is saved).
    int sr = -1, fg = -1;      // 0 off, 1 on
    int multiplier = -1;       // 2..8
    int vfgQuality = -1;
};

struct InputFrame {
    const uint8_t* planes[2]{};   // Y, interleaved UV
    int strides[2]{};
    int64_t pts100ns = 0;
    bool discontinuity = false;   // first frame after open / seek / flush
};

struct OutputFrame {
    int64_t pts100ns = 0;
    enum class Kind { Real, Generated, Hold } kind = Kind::Real;
    uint32_t width = 0, height = 0;
    OutputFormat format = OutputFormat::RGBA8;
    std::vector<uint8_t> pixels;  // tightly packed rows
};

struct CoreStats {
    uint64_t source = 0, generated = 0, holds = 0;
    double processMs = 0, readbackMs = 0;      // totals
    double slowestProcessMs = 0;
};

class Core {
public:
    Core();
    ~Core();
    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;

    bool open(const CoreConfig&, const StreamInfo&);
    // One source frame in; appends (multiplier - 1) in-between frames and then the real frame,
    // in presentation order, to `out`. The first frame after a discontinuity yields the real frame only.
    bool push(const InputFrame&, std::vector<OutputFrame>& out);
    void close();

    // What the graph was built with, for logs and the property page.
    std::string describe() const;
    uint32_t outputWidth() const;
    uint32_t outputHeight() const;
    uint32_t multiplier() const;
    const CoreStats& stats() const;
    const std::string& error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pushmpc
