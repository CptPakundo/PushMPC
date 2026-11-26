// PushMPC: the image model behind ContentDetect's drawn-vs-not evidence (optional). CLIP ViT-B/32's image encoder
// (OpenAI, MIT; Setup fetches the pinned ONNX file into <pmml runtime>\content-model\) runs through pmml on the GPU
// (DirectML; TensorRT-RTX where installed) on a 224x224 thumbnail about once a second, on its own thread: the streaming
// thread only hands over the thumbnail and picks up the probability. No model, no runtime, or a failure: the
// hand-built cues alone decide (ContentDetector without SetImageEvidence).
#pragma once
#if defined(PUSHMPC_CONTENT_MODEL) && PUSHMPC_CONTENT_MODEL
#include <windows.h>
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

namespace pm {

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
    // streaming thread: the newest result (true once per result)
    bool Take(double& pDrawn, double& pts)
    {
        std::lock_guard<std::mutex> lk(m_mx);
        if (!m_have) return false;
        m_have = false; pDrawn = m_p; pts = m_pPts; return true;
    }
    bool Failed() const { std::lock_guard<std::mutex> lk(m_mx); return m_failed; }
    double LastMs() const { return m_ms.load(); }

private:
    static std::filesystem::path Runtime()
    {
        namespace fs = std::filesystem;
        auto ok = [](const fs::path& p) { std::error_code ec; return !p.empty() && fs::exists(p / L"content-model" / L"vision_model_fp16.onnx", ec) &&
                                                                 (fs::exists(p / L"ort" / L"directml" / L"onnxruntime.dll", ec) || fs::exists(p / L"ort" / L"core" / L"onnxruntime.dll", ec)); };
        if (const wchar_t* e = _wgetenv(L"PMML_RUNTIME"); e && *e && ok(e)) return e;
        wchar_t buf[1024]{}; DWORD size = sizeof(buf);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\PushMPC", L"MlRuntime", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS && ok(buf)) return buf;
        if (const wchar_t* la = _wgetenv(L"LOCALAPPDATA"); la && ok(fs::path(la) / L"PushMPC" / L"pmml")) return fs::path(la) / L"PushMPC" / L"pmml";
        return {};
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
                const char* backend = fs::exists(rt / L"ort" / L"directml" / L"onnxruntime.dll", ec) ? "ort:directml:dml:0" : "ort:core:cpu:0";
                pmml::Backend b; pmml::Backend::parse(backend, &b);
                s = pmml::RawSession::create(rt.string(), b, (rt / L"content-model" / L"vision_model_fp16.onnx").string(), {}, (rt / L"cache").string(), &err);
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
            m_p = pushmpc::content::ImageDrawnProbability(emb.data()); m_pPts = pts; m_have = true;
        }
    }
    Log m_log;
    mutable std::mutex m_mx;
    std::condition_variable m_cv;
    std::thread m_thread;
    std::vector<float> m_in;
    double m_inPts = 0, m_p = 0.5, m_pPts = 0;
    bool m_busy = false, m_have = false, m_failed = false, m_stop = false;
    std::atomic<double> m_ms{ -1.0 };
};

} // namespace pm
#endif
