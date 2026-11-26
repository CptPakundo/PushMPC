// SPDX-License-Identifier: GPL-3.0-or-later
// PushMPC capture mode: the engines that take frames in host memory, and the pipeline that feeds them.
//   cpu    PushFPS's portable engine on the CPU (vsplugin/engines/portable): every OS, any t
//   ml     PushFlow (RIFE) through pmml on the GPU (ONNX Runtime / ncnn), when the pmml runtime is installed: any t
//   apple  VideoToolbox frame interpolation (macOS 15.4+ / 26): k outputs per source frame
// The Vulkan GPU engine keeps its frames on the GPU (cap_gpu.h); these get I420 pictures from the converter.
#pragma once
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include "cap_core.h"

namespace pmc {

struct EngineRequest {
    int w = 0, h = 0;
    double srcFps = 0, displayHz = 0;
    int smoother = 0;            // 0 Auto, 1 off, 2..8
};

class HostEngine {
public:
    virtual ~HostEngine() = default;
    virtual std::string Describe() const = 0;
    // 0: any t (the pipeline asks for each screen refresh's own phase); k: only the phases j / k exist
    virtual int Steps() const { return 0; }
    // a new newest frame b; the engine's pair becomes (the previous frame, b). contiguous = false: no pair yet
    virtual bool Push(const I420& f, bool contiguous, std::string& err) = 0;
    // the frame at phase t (0 < t < 1) of the current pair
    virtual bool Render(float t, I420& out, std::string& err) = 0;
};

// the engine by name ("cpu", "ml", "apple"); nullptr + why when it cannot run here
std::unique_ptr<HostEngine> OpenHostEngine(const std::string& name, const EngineRequest& r, std::string& why);
// whether the pmml runtime is installed (ml)
bool MlInstalled();
// whether this Mac offers VideoToolbox frame interpolation (apple)
bool AppleAvailable(std::string* why);

// frames in host memory -> the picture of one screen refresh
class HostPipeline {
public:
    explicit HostPipeline(std::unique_ptr<HostEngine> e) : m_eng(std::move(e)) {}
    HostEngine* Engine() const { return m_eng.get(); }
    void Add(uint64_t idx, bool discontinuity, I420&& pic);
    std::vector<uint64_t> Frames() const;
    std::vector<uint64_t> Discontinuities() const;
    // the picture for this pick (one of the stored frames, or the engine's), nullptr + err on failure
    const I420* Get(const Pick& p, std::string& err);
    uint64_t Generated() const { return m_generated; }
    uint64_t Pushes() const { return m_pushes; }
private:
    struct Entry { uint64_t idx = 0; bool disc = false; I420 pic; };
    Entry* Find(uint64_t idx);
    std::unique_ptr<HostEngine> m_eng;
    std::deque<Entry> m_q;
    bool m_havePair = false;
    uint64_t m_pa = 0, m_pb = 0;             // the engine's pair
    bool m_haveOut = false;
    uint64_t m_oa = 0, m_ob = 0; float m_ot = -1;   // what m_out holds
    I420 m_out;
    uint64_t m_generated = 0, m_pushes = 0;
};

} // namespace pmc
