// PushMPC: the image model behind ContentDetect's drawn-vs-not evidence (optional; every route that links pmml: the
// MPC-HC filter, the Kodi add-on, the VapourSynth plugin, the IINA bridge). CLIP ViT-B/32's image encoder (OpenAI, MIT;
// Setup fetches the pinned ONNX file into <pmml runtime>/content-model/) runs through pmml (DirectML on Windows, CoreML
// on macOS, ONNX Runtime CPU otherwise) on a 224x224 thumbnail about once a second, on its own thread: the caller
// thread only hands over the thumbnail and picks up the probability. No model, no runtime, or a failure: the
// hand-built cues alone decide (ContentDetector without SetImageEvidence).
#pragma once
#if defined(PUSHMPC_CONTENT_MODEL) && PUSHMPC_CONTENT_MODEL
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "pmml/pmml.h"
#include "pushmpc/content_detect.h"

namespace pushmpc::content {

class ContentModel {
public:
    using Log = std::function<void(const std::string&)>;
    explicit ContentModel(Log log) : m_log(std::move(log)) { m_thread = std::thread([this] { Loop(); }); }
    ~ContentModel() { { std::lock_guard<std::mutex> lk(m_mx); m_stop = true; } m_cv.notify_all(); if (m_thread.joinable()) m_thread.join(); }
    // streaming thread: a thumbnail (ClipInput) of the frame at pts; dropped when the previous one is still running
    void Offer(std::vector<float> input, double pts)
    {
        std::lock_guard<std::mutex> lk(m_mx);
        if (m_busy || m_failed) return;
        m_in = std::move(input); m_inPts = pts; m_busy = true;
        m_cv.notify_all();
    }
    // streaming thread: the newest result (true once per result); pCg: rendered 3D CG among the drawn looks
    bool Take(double& pDrawn, double& pts, double& pCg)
    {
        std::lock_guard<std::mutex> lk(m_mx);
        if (!m_have) return false;
        m_have = false; pDrawn = m_p; pts = m_pPts; pCg = m_pCg; return true;
    }
    bool Take(double& pDrawn, double& pts) { double c; return Take(pDrawn, pts, c); }
    bool Failed() const { std::lock_guard<std::mutex> lk(m_mx); return m_failed; }
    double LastMs() const { return m_ms.load(); }

private:
    static std::filesystem::path Runtime()
    {
        namespace fs = std::filesystem;
        auto ok = [](const fs::path& p) {
            std::error_code ec;
            return !p.empty() && fs::exists(p / "content-model" / "vision_model_fp16.onnx", ec) && fs::exists(p / "ort", ec);
        };
        if (const char* e = std::getenv("PMML_RUNTIME"); e && *e && ok(e)) return e;
#if defined(_WIN32)
        // (no windows.h here: it is included by hosts whose own APIs its macros break, e.g. Kodi's vfs::CreateDirectory)
        if (const char* la = std::getenv("LOCALAPPDATA"); la && ok(fs::path(la) / "PushMPC" / "pmml")) return fs::path(la) / "PushMPC" / "pmml";
#elif defined(__APPLE__)
        if (const char* h = std::getenv("HOME"); h && ok(fs::path(h) / "Library/Application Support/PushMPC/pmml")) return fs::path(h) / "Library/Application Support/PushMPC/pmml";
        // Homebrew's pushmpc-setup keeps its pmml runtime in the XDG data folder
        if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x && ok(fs::path(x) / "pushmpc/pmml")) return fs::path(x) / "pushmpc/pmml";
        if (const char* h = std::getenv("HOME"); h && ok(fs::path(h) / ".local/share/pushmpc/pmml")) return fs::path(h) / ".local/share/pushmpc/pmml";
#else
        if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x && ok(fs::path(x) / "pushmpc/pmml")) return fs::path(x) / "pushmpc/pmml";
        if (const char* h = std::getenv("HOME"); h && ok(fs::path(h) / ".local/share/pushmpc/pmml")) return fs::path(h) / ".local/share/pushmpc/pmml";
#endif
        return {};
    }
    static const char* BackendFor(const std::filesystem::path& rt)
    {
        std::error_code ec;
#if defined(_WIN32)
        if (std::filesystem::exists(rt / "ort" / "directml", ec)) return "ort:directml:dml:0";
#elif defined(__APPLE__)
        return "ort:core:coreml:0";
#endif
        return "ort:core:cpu:0";
    }
    void Loop()
    {
        namespace fs = std::filesystem;
        std::unique_ptr<pmml::RawSession> s;
        {
            const fs::path rt = Runtime();
            std::string err;
            if (rt.empty()) err = "no image model installed (Setup: the content model) or no pmml runtime";
            else {
                std::error_code ec;
                pmml::Backend b; pmml::Backend::parse(BackendFor(rt), &b);
                s = pmml::RawSession::create(rt.string(), b, (rt / "content-model" / "vision_model_fp16.onnx").string(), {}, (rt / "cache").string(), &err);
                if (!s && b.ep != "cpu") { pmml::Backend::parse("ort:core:cpu:0", &b); err.clear(); s = pmml::RawSession::create(rt.string(), b, (rt / "content-model" / "vision_model_fp16.onnx").string(), {}, (rt / "cache").string(), &err); }
                if (s) m_log("content: image model " + s->describe());
            }
            if (!s) { m_log("content: image model not used (" + err + "): the hand-built cues decide"); std::lock_guard<std::mutex> lk(m_mx); m_failed = true; return; }
        }
        std::vector<float> emb(512);
        while (true) {
            std::vector<float> in; double pts;
            {
                std::unique_lock<std::mutex> lk(m_mx);
                m_cv.wait(lk, [&] { return m_stop || (m_busy && !m_in.empty()); });
                if (m_stop) return;
                in.swap(m_in); pts = m_inPts;
            }
            const auto t0 = std::chrono::steady_clock::now();
            std::string err;
            const bool ok = s->run(in.data(), { 1, 3, 224, 224 }, { emb.data() }, { emb.size() * sizeof(float) }, &err);
            m_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::lock_guard<std::mutex> lk(m_mx);
            m_busy = false;
            if (!ok) { m_failed = true; m_log("content: image model failed (" + err + "): the hand-built cues decide"); return; }
            m_p = ImageDrawnProbability(emb.data()); m_pCg = ImageCgProbability(emb.data()); m_pPts = pts; m_have = true;
        }
    }
    Log m_log;
    mutable std::mutex m_mx;
    std::condition_variable m_cv;
    std::thread m_thread;
    std::vector<float> m_in;
    double m_inPts = 0, m_p = 0.5, m_pPts = 0, m_pCg = 0.5;
    bool m_busy = false, m_have = false, m_failed = false, m_stop = false;
    std::atomic<double> m_ms{ -1.0 };
};

} // namespace pm
#endif
