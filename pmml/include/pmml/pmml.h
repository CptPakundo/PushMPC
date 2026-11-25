// SPDX-License-Identifier: GPL-3.0-or-later
// pmml: PushMPC's ML runtime layer. One interface for PushFlow (RIFE) frame interpolation over
//   (a) ONNX Runtime with an execution provider: CPU, DirectML, TensorRT-RTX / OpenVINO / QNN / AMD GPU (plugin EPs),
//       CoreML, MIGraphX;
//   (b) ncnn on Vulkan (any Vulkan GPU, MoltenVK on macOS).
// Frames are 16-bit planes (RGB 4:4:4 or YUV 4:2:0, any bit depth). With ONNX Runtime the colour conversion, the
// network's downscale/pad, the network and the full-size rebuild all run on the chosen EP (pmml/scripts/make_graphs.py);
// frames cross the bus once per pair and the finished picture once per output frame.
// "Flow once, synthesize many": with more than one timestep per pair the network runs once at t_ref and every
// timestep is rebuilt from its grids, scaled (Interpolator::setPair's `phases`).
// Auto: autobench.h measures the candidates on the user's GPU at the real size and picks the best that fits.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pmml {

constexpr const char* kVersion = "0.1.0";

// Colour: how 16-bit codes map to RGB 0..1 (and back). Planes hold codes 0..maxValue.
struct Colour {
    float kr = 0.2126f, kb = 0.0722f;                      // BT.709 (BT.601: 0.299 / 0.114; BT.2020: 0.2627 / 0.0593)
    float yOff = 16.f / 255.f, yScale = 219.f / 255.f;    // limited range; full: 0 / 1
    float cOff = 128.f / 255.f, cScale = 224.f / 255.f;   // limited range; full: 0.5 / 1
    float maxValue = 255.f;                               // (1 << bits) - 1
    static Colour limited(int bits, float kr = 0.2126f, float kb = 0.0722f);
    static Colour full(int bits, float kr = 0.2126f, float kb = 0.0722f);
    std::array<float, 8> packed() const { return {kr, kb, yOff, yScale, cOff, cScale, maxValue, 0.f}; }
};

enum class PlaneFormat { Rgb, Yuv420 };

// Planes with strides in elements (uint16). For Yuv420 planes 1 and 2 are (w/2) x (h/2).
struct Frame { const uint16_t* plane[3] = {}; ptrdiff_t stride[3] = {}; };
struct OutFrame { uint16_t* plane[3] = {}; ptrdiff_t stride[3] = {}; };

// Where the network runs. runtime "ort": flavor = the ONNX Runtime build (core | directml | migraphx), ep = cpu | dml |
// trt-rtx | openvino | qnn (NPU) | qnn-gpu | amdgpu | coreml | migraphx. runtime "ncnn": ep = vulkan | cpu.
struct Backend {
    std::string runtime = "ort";
    std::string flavor = "core";
    std::string ep = "cpu";
    int device = 0;               // adapter / EP device index among the matching devices
    std::string key() const { return runtime + ":" + flavor + ":" + ep + ":" + std::to_string(device); }
    static bool parse(const std::string& text, Backend* out);   // "ort:directml:dml:0" or short "dml", "trt-rtx", "ncnn"
};

struct Options {
    std::string runtimeDir;       // ort/<flavor>/, eps/<ep>/ep.json, ncnn models
    std::string modelsDir;        // pushflow-*.onnx (+ .onnx.txt facts, models.json) (an installed PushFPS's models work)
    std::string graphsDir;        // pack-*.onnx, synth-*.onnx (make_graphs.py)
    std::string ncnnModelsDir;    // pushflow-<role>-<padW>x<padH>.ncnn.param/bin (ncnn_models.py); default <runtimeDir>/ncnn-models
    std::string cacheDir;         // engine caches (TensorRT-RTX, OpenVINO), the autobench cache
    Backend backend;
    std::string tier = "balanced";   // fast | balanced | quality (PushFlow's tiers: model and motion size)
    int motionHeight = 0;         // 0 = the tier's own (540 / 720 / 1080); else 540, 720, 1080...
    bool fp16 = false;            // the -fp16 model when the models folder has it
    float tRef = 0.5f;            // flow once: where the network runs
    bool hostIntermediates = false;   // debug: grids/mask/frames through host memory between the graphs
    bool syncTiming = false;      // wait for the device after each graph, so Stats splits the time honestly (probes)
    PlaneFormat format = PlaneFormat::Yuv420;
    Colour colour;
    int width = 0, height = 0;    // full size (even for Yuv420)
};

struct Stats {
    double packMs = 0, netMs = 0, synthMs = 0;   // wall time of the last call of each graph (synchronous)
    int netW = 0, netH = 0, padW = 0, padH = 0;   // the network's size and its padded size
    uint64_t networkRuns = 0, synthRuns = 0;
};

class Interpolator {
public:
    virtual ~Interpolator() = default;
    static std::unique_ptr<Interpolator> create(const Options& o, std::string* err);
    // A new pair. phases = how many timesteps will be rendered for it: 1 runs the network at that exact t
    // ("per t"); more runs it once at tRef and rebuilds every t from its motion ("flow once"). 0 = always per t.
    virtual bool setPair(const Frame& a, const Frame& b, int phases, std::string* err) = 0;
    virtual bool render(float t, const OutFrame& out, std::string* err) = 0;
    virtual Stats stats() const = 0;
    virtual std::string describe() const = 0;
};

// What this runtime folder can run on this machine: one line per (backend, device), with the device's name, vendor id
// and driver when the EP reports them.
struct DeviceInfo {
    Backend backend;
    std::string name, vendor, driver;
    uint32_t vendorId = 0, deviceId = 0;
    std::string type;             // GPU | NPU | CPU
    std::string note;
};
std::vector<DeviceInfo> listDevices(const std::string& runtimeDir, std::string* log);

// The PushFlow tiers: which model and how large the network runs (PushFPS docs/PUSHFLOW.md, 3.1.6+).
struct NetworkSize { int w = 0, h = 0, padW = 0, padH = 0; };
NetworkSize networkSize(int width, int height, const std::string& tier, int motionHeight, int pad);
std::array<float, 4> gridAffine(int width, int height, const NetworkSize& n);
std::string modelRole(const std::string& tier);   // "fast" | "quality"

}  // namespace pmml
