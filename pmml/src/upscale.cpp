// SPDX-License-Identifier: GPL-3.0-or-later
// pmml's anime upscalers: the model table (mirrors scripts/anime-models-pin.json; pmml_unit checks the two agree) and
// the backend dispatch. The ONNX Runtime implementation is in ort.cpp (OrtUpscaler).
#include "internal.h"
#include "pmml/upscale.h"

namespace pmml {

const std::vector<UpscaleModel>& upscaleModels() {
    static const std::vector<UpscaleModel> m = {
        {"artcnn-c4f16", "artcnn", "ArtCNN_C4F16.onnx", "ArtCNN_C4F16.onnx", "luma", 2, "MIT"},
        {"artcnn-c4f16-ds", "artcnn", "ArtCNN_C4F16_DS.onnx", "ArtCNN_C4F16_DS.onnx", "luma", 2, "MIT"},
        {"artcnn-c4f16-dn", "artcnn", "ArtCNN_C4F16_DN.onnx", "ArtCNN_C4F16_DN.onnx", "luma", 2, "MIT"},
        {"artcnn-c4f32", "artcnn", "ArtCNN_C4F32.onnx", "ArtCNN_C4F32.onnx", "luma", 2, "MIT"},
        {"artcnn-c4f32-ds", "artcnn", "ArtCNN_C4F32_DS.onnx", "ArtCNN_C4F32_DS.onnx", "luma", 2, "MIT"},
        {"artcnn-c4f32-dn", "artcnn", "ArtCNN_C4F32_DN.onnx", "ArtCNN_C4F32_DN.onnx", "luma", 2, "MIT"},
        {"artcnn-r8f64", "artcnn", "ArtCNN_R8F64.onnx", "ArtCNN_R8F64.onnx", "luma", 2, "MIT"},
        {"cunny-fast", "cunny", "CuNNy-fast-NVL.pickle", "sr-cunny-fast.onnx", "luma", 2, "LGPL-3.0"},
        {"cunny-4x16", "cunny", "CuNNy-4x16-NVL.pickle", "sr-cunny-4x16.onnx", "rgb", 2, "LGPL-3.0"},
        {"cunny-8x32", "cunny", "CuNNy-8x32-NVL.pickle", "sr-cunny-8x32.onnx", "rgb", 2, "LGPL-3.0"},
        {"animesr-v2", "animesr", "AnimeSR_v2.pth", "sr-animesr-v2.onnx", "animesr", 4, "Apache-2.0"},
    };
    return m;
}

const UpscaleModel* findUpscaleModel(const std::string& id) {
    for (const auto& m : upscaleModels())
        if (id == m.id) return &m;
    return nullptr;
}

std::string upscaleFilesMissing(const UpscaleOptions& o) {
    const UpscaleModel* m = findUpscaleModel(o.model);
    if (!m) return "unknown model " + o.model;
    const std::string dir = o.modelsDir.empty() ? detail::joinPath(o.graphsDir, "anime-sr") : o.modelsDir;
    const std::string kind = std::string(m->kind) == "luma" ? "luma" : "rgb";
    const std::string io = o.io == UpscaleIo::Rgba8 ? "rgba8" : "yuv";
    std::string missing;
    for (const std::string& p : {detail::joinPath(dir, m->file), detail::joinPath(dir, m->graph),
                                 detail::joinPath(o.graphsDir, "sr_in_" + kind + "-" + io + ".onnx"),
                                 detail::joinPath(o.graphsDir, "sr_out_" + kind + "-" + io + ".onnx")})
        if (!detail::fileExists(p)) missing += (missing.empty() ? "" : ", ") + p;
    return missing;
}

std::unique_ptr<Upscaler> Upscaler::create(const UpscaleOptions& o, std::string* err) {
    if (o.backend.runtime != "ort") {
        if (err) *err = "the anime upscalers run on ONNX Runtime only (backend " + o.backend.key() + ")";
        return nullptr;
    }
    if (const std::string miss = upscaleFilesMissing(o); !miss.empty()) {
        if (err) *err = "anime upscaler files missing: " + miss;
        return nullptr;
    }
    return detail::createOrtUpscaler(o, err);
}

}  // namespace pmml
