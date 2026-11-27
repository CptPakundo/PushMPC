// SPDX-License-Identifier: GPL-3.0-or-later
// pmml's anime upscalers (PushMPC's "Anime upscaler"): ArtCNN (MIT), CuNNy (LGPL-3.0, a separate replaceable component:
// its weights file + graph), AnimeSR v2 (Apache-2.0) on ONNX Runtime, the same execution providers as PushFlow.
// Three graphs per picture, intermediates in device memory: sr_in_<kind>-<io> (picture -> the model's input), the
// model, sr_out_<kind>-<io> (-> picture). The weights are the pinned upstream files in <graphs>/anime-sr
// (scripts/anime-models-pin.json; Setup fetches them, scripts/anime-models-fetch.ps1); see
// pmml/scripts/anime_sr_graphs.py.
#pragma once
#include "pmml/pmml.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pmml {

struct UpscaleModel {
    const char* id;          // setting value, e.g. "artcnn-c4f32"
    const char* family;      // artcnn | cunny | animesr
    const char* file;        // the pinned upstream weights file
    const char* graph;       // the graph pmml runs (ArtCNN: the upstream file itself)
    const char* kind;        // luma | rgb | animesr (recurrent, RGB)
    int scale;               // 2 or 4
    const char* licence;
};
const std::vector<UpscaleModel>& upscaleModels();
const UpscaleModel* findUpscaleModel(const std::string& id);

enum class UpscaleIo {
    Yuv420,   // 16-bit planes (any bit depth by Colour), output = scale x input
    Rgba8,    // R8G8B8A8 rows (pushcore's encoded picture, BT.709 full range), output resized to outWidth x outHeight
};

struct UpscaleOptions {
    std::string runtimeDir;       // ort/<flavor>/, eps/
    std::string graphsDir;        // sr_in_* / sr_out_* (make_graphs.py)
    std::string modelsDir;        // the model graphs + weights; default <graphsDir>/anime-sr
    std::string cacheDir;
    Backend backend;
    std::string model = "artcnn-c4f16";
    UpscaleIo io = UpscaleIo::Yuv420;
    Colour colour;                // Yuv420
    int width = 0, height = 0;    // input (even for Yuv420)
    int outWidth = 0, outHeight = 0;   // Rgba8 only: the final size (0 = width * scale)
    int inPitchPx = 0, outPitchPx = 0; // Rgba8 only: row pitch of the input / output in pixels (0 = the width)
    bool syncTiming = false;
};

struct UpscaleStats { double inMs = 0, netMs = 0, outMs = 0; uint64_t runs = 0; };

class Upscaler {
public:
    virtual ~Upscaler() = default;
    static std::unique_ptr<Upscaler> create(const UpscaleOptions& o, std::string* err);
    virtual int outWidth() const = 0;
    virtual int outHeight() const = 0;
    // Yuv420: out planes outWidth x outHeight (chroma half)
    virtual bool run(const Frame& in, const OutFrame& out, std::string* err) = 0;
    // Rgba8: pitches in bytes
    virtual bool runRgba(const uint8_t* in, ptrdiff_t inPitch, uint8_t* out, ptrdiff_t outPitch, std::string* err) = 0;
    // AnimeSR is recurrent (it carries the previous output and a hidden state): call at a cut or a seek. It runs
    // causally here (the next frame is the current one, as the reference does for a clip's last frame): no added delay.
    virtual void reset() = 0;
    virtual UpscaleStats stats() const = 0;
    virtual std::string describe() const = 0;
    // Zero copy (Rgba8): the picture stays in GPU memory the caller shares with pmml's device.
    //   "dml":  ONNX Runtime with DirectML on pmml's own D3D12 device and queue (dmlDevice / dmlQueue): the caller opens
    //           its shared buffers and fence there; wrapDml turns such a buffer (UAV-capable, COMMON state) into an
    //           allocation for runRgbaDevice; the work is recorded on dmlQueue (the caller orders it with fence waits).
    //   "cuda": TensorRT-RTX: runRgbaDevice takes CUDA device pointers (the caller's D3D12 buffers imported into CUDA);
    //           it returns when the result is written.
    //   "host": no zero copy (runRgba only).
    virtual std::string deviceKind() const { return "host"; }
    virtual void* dmlDevice() const { return nullptr; }   // ID3D12Device*
    virtual void* dmlQueue() const { return nullptr; }    // ID3D12CommandQueue*
    virtual void* wrapDml(void* d3d12Resource, std::string* err) { (void)d3d12Resource; if (err) *err = "no DirectML here"; return nullptr; }
    virtual void freeDml(void* allocation) { (void)allocation; }
    // in: [h][inPitchPx][4] bytes, out: [outHeight][outPitchPx][4] (as the options say), both in the EP's device memory
    virtual bool runRgbaDevice(void* in, void* out, std::string* err) { (void)in; (void)out; if (err) *err = "no zero copy on this backend"; return false; }
};

// The files a model needs that are not there ("" = all present): the weights, the model graph, the wrappers.
std::string upscaleFilesMissing(const UpscaleOptions& o);

// The bytes pmml binds for an Rgba8 picture of `height` rows at `pitchPx` pixels a row: every row whole, the last one
// too. A buffer given to runRgba / runRgbaDevice must hold this many; a D3D12 copyable footprint's total does not (its
// last row stops at the width), which TensorRT-RTX refused at 720 wide (pitch 768): cudaMemcpy "invalid argument".
inline size_t rgbaTensorBytes(int pitchPx, int height) { return size_t(pitchPx) * size_t(height) * 4; }

}  // namespace pmml
