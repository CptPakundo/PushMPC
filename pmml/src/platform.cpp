// SPDX-License-Identifier: GPL-3.0-or-later
#include "internal.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <cstdlib>

namespace pmml {
namespace detail {

#ifdef _WIN32
static std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    for (auto& ch : w) if (ch == L'/') ch = L'\\';
    return w;
}
#endif

Library::~Library() {
    // Never unloaded: ONNX Runtime and its EPs keep threads and static state alive until the process ends.
}

bool Library::open(const std::string& path, std::string* err) {
#ifdef _WIN32
    HMODULE h = LoadLibraryExW(widen(path).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) { if (err) *err = "cannot load " + path + " (error " + std::to_string(GetLastError()) + ")"; return false; }
    handle_ = h;
#else
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) { if (err) { const char* e = dlerror(); *err = "cannot load " + path + ": " + (e ? e : "?"); } return false; }
    handle_ = h;
#endif
    return true;
}

void* Library::symbol(const char* name) const {
    if (!handle_) return nullptr;
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle_), name));
#else
    return dlsym(handle_, name);
#endif
}

const char* ortLibraryName() {
#if defined(_WIN32)
    return "onnxruntime.dll";
#elif defined(__APPLE__)
    return "libonnxruntime.dylib";
#else
    return "libonnxruntime.so";
#endif
}

bool findModel(const Options& o, ModelFiles* m, std::string* err) {
    const std::string role = modelRole(o.tier);
    std::string file = joinPath(o.modelsDir, "pushflow-" + role + (o.fp16 ? "-fp16" : "") + ".onnx");
    if (o.fp16 && !fileExists(file)) file = joinPath(o.modelsDir, "pushflow-" + role + ".onnx");
    if (!fileExists(file)) { if (err) *err = "no PushFlow model " + file; return false; }
    m->path = file;
    const auto facts = readFacts(joinPath(o.modelsDir, "pushflow-" + role + ".onnx.txt"));
    if (facts.count("pad")) m->pad = std::atoi(facts.at("pad").c_str());
    if (facts.count("mask_for_first")) m->maskForFirst = facts.at("mask_for_first") == "1";
    // models.json sits next to models/ in PushFPS's layout and inside it in ours
    std::string json = readText(joinPath(o.modelsDir, "models.json"));
    if (json.empty()) json = readText(joinPath(o.modelsDir, "../models.json"));
    m->dims = jsonStrings(json, "models." + role + ".input_dims");
    m->channels = int(jsonNumber(json, "models." + role + ".channels", 7));
    if (m->channels != 7) { if (err) *err = "PushFlow model with " + std::to_string(m->channels) + " input channels (pmml packs 7)"; return false; }
    return true;
}

}  // namespace detail

std::unique_ptr<Interpolator> Interpolator::create(const Options& o, std::string* err) {
    if (o.width <= 0 || o.height <= 0 || (o.format == PlaneFormat::Yuv420 && ((o.width | o.height) & 1))) {
        if (err) *err = "frame size must be positive (and even for 4:2:0)";
        return nullptr;
    }
    if (o.backend.runtime == "ort") return detail::createOrt(o, err);
#if PMML_WITH_NCNN
    if (o.backend.runtime == "ncnn") return detail::createNcnn(o, err);
#endif
    if (err) *err = "runtime '" + o.backend.runtime + "' is not built into this pmml";
    return nullptr;
}

std::vector<DeviceInfo> listDevices(const std::string& runtimeDir, std::string* log) {
    auto v = detail::listOrt(runtimeDir, log);
#if PMML_WITH_NCNN
    auto n = detail::listNcnn(runtimeDir, log);
    v.insert(v.end(), n.begin(), n.end());
#endif
    return v;
}

}  // namespace pmml
