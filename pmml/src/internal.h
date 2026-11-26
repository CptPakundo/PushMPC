// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "pmml/pmml.h"
#include "pmml/upscale.h"

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace pmml::detail {

std::string readText(const std::string& path);
bool fileExists(const std::string& path);
std::string joinPath(const std::string& a, const std::string& b);
std::map<std::string, std::string> readFacts(const std::string& path);
bool jsonRaw(const std::string& s, const std::string& path, std::string* raw);
std::string jsonString(const std::string& s, const std::string& path, const std::string& def = {});
double jsonNumber(const std::string& s, const std::string& path, double def = 0);
std::vector<std::string> jsonStrings(const std::string& s, const std::string& path);
std::vector<std::string> jsonKeys(const std::string& s, const std::string& path);
std::string jsonEscape(const std::string& v);

inline double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// A shared library loaded by full path (its own folder first for its dependencies).
class Library {
public:
    Library() = default;
    ~Library();
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;
    bool open(const std::string& path, std::string* err);
    void* symbol(const char* name) const;
    bool loaded() const { return handle_ != nullptr; }
private:
    void* handle_ = nullptr;
};

// The file name of the ONNX Runtime library on this OS.
const char* ortLibraryName();

// The model of a tier in a models folder: file, facts (.onnx.txt) and the free dimension names (models.json).
struct ModelFiles {
    std::string path;             // pushflow-<role>[-fp16].onnx
    int pad = 64;
    bool maskForFirst = true;
    int channels = 7;
    std::vector<std::string> dims;   // e.g. batch, 7, height, width
};
bool findModel(const Options& o, ModelFiles* m, std::string* err);

// Backends.
std::unique_ptr<Interpolator> createOrt(const Options& o, std::string* err);
std::vector<DeviceInfo> listOrt(const std::string& runtimeDir, std::string* log);
std::unique_ptr<RawSession> createOrtRaw(const std::string& runtimeDir, const Backend& b, const std::string& model,
                                         const std::vector<std::pair<std::string, int64_t>>& dims, const std::string& cacheDir, std::string* err);
std::unique_ptr<Upscaler> createOrtUpscaler(const UpscaleOptions& o, std::string* err);   // the anime upscalers
#if PMML_WITH_NCNN
std::unique_ptr<Interpolator> createNcnn(const Options& o, std::string* err);
std::vector<DeviceInfo> listNcnn(const std::string& runtimeDir, std::string* log);
#endif

// CPU reference of the synth graph (used by the ncnn backend and the tests): rebuild at full size from grids/mask.
//   frames: RGB float planar [3][h][w]; grids [ph][pw][2]; mask [ph][pw]
void synthCpu(const float* fa, const float* fb, int w, int h, const float* g0, const float* g1, const float* mask,
              const NetworkSize& n, const std::array<float, 4>& affine, float s0, float s1, float* out, int threads,
              bool lineArt = false);
// line-art protection (synth_line, make_graphs.py): a line pixel has a 3x3 black top-hat of luma above kLineTau, and the
// two warped frames differ there by more than kLineDelta (luma, 0..1)
inline constexpr float kLineTau = 0.05f, kLineDelta = 0.04f;
void planesToRgb(const Frame& f, PlaneFormat fmt, const Colour& c, int w, int h, float* rgb);
void rgbToPlanes(const float* rgb, PlaneFormat fmt, const Colour& c, int w, int h, const OutFrame& out);
void resizeLinear(const float* src, int c, int h, int w, int oh, int ow, float* dst);

}  // namespace pmml::detail
