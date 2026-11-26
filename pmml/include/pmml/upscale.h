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
};

// The files a model needs that are not there ("" = all present): the weights, the model graph, the wrappers.
std::string upscaleFilesMissing(const UpscaleOptions& o);

}  // namespace pmml
