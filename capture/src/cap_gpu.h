// SPDX-License-Identifier: GPL-3.0-or-later
// PushMPC capture mode on the portable GPU engine (vsplugin/engines/gpu, Vulkan): the captured picture goes to the GPU
// once (host memory: one copy into a staging buffer; a linear dma-buf: imported, no copy), the conversion to YUV, the
// motion search and the cut guard run there, generated (and NIS / FSR1 upscaled) frames go from the engine's buffer
// straight into the window (Export), never through host memory.
#pragma once
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "cap_core.h"
#include "cap_source.h"
#include "cap_vk.h"
#include "gpu_backend.h"
#include "gpu_engine.h"

namespace pmc {

struct GpuRequest {
    int w = 0, h = 0;              // the captured picture (even)
    int outW = 0, outH = 0;        // the window (the upscaling target); 0 = no upscaling
    std::string upscaler = "auto"; // auto | nis | fsr | off
    bool hostOut = false;          // outputs read back into host memory (the dump, tests) instead of Export
};

class GpuPipeline {
public:
    ~GpuPipeline();
    bool Open(Vk& vk, const GpuRequest& r, std::string& desc, std::string& err);
    // a captured frame; its release runs once the GPU has read it
    bool Add(uint64_t idx, bool discontinuity, SourceFrame& f, std::string& err);
    std::vector<uint64_t> Frames() const;
    std::vector<uint64_t> Discontinuities() const;
    // the picture of this pick into the screen's acquired image (the engine's submission waits for the acquire and
    // signals the screen's render semaphore), or into host memory (hostOut)
    bool Show(const Pick& p, Screen* screen, I420* host, std::string& err);
    // runs the releases of frames the GPU has read
    void Poll();
    int OutW() const { return m_outW; }
    int OutH() const { return m_outH; }
    uint64_t Generated() const { return m_generated; }
    uint64_t Cuts() const { return m_cuts; }
    std::string Stats() const;
private:
    struct Entry { uint64_t idx = 0; bool disc = false; int slot = -1, pair = -1; bool estimated = false, cut = false; };
    struct Upload { VBuf staging; VkDescriptorSet set = VK_NULL_HANDLE, setImg = VK_NULL_HANDLE; uint64_t ticket = 0; };
    struct Pending { uint64_t ticket = 0; std::function<void()> release; };
    Entry* Find(uint64_t idx);
    bool Settle(Entry& e, std::string& err);
    void Trim();
    bool RecordConvert(VkCommandBuffer cb, const pmvs::gpu::Engine::PlanarDesc& d, VkBuffer src, VkDeviceSize srcBytes, int pitchPx, int offPx, int w, int h, bool rgbx, Upload& u, std::string& err);
    Vk* m_vk = nullptr;
    std::shared_ptr<pmvs::gpu::Engine> m_eng;
    pmvs::gpu::CutGuard m_guard;
    VPipe m_conv;
    VkDescriptorPool m_dpool = VK_NULL_HANDLE;
    Upload m_up[3];
    int m_upNext = 0;
    std::map<uint64_t, VBuf> m_dmabufs;   // imported dma-bufs by inode (PipeWire reuses a few buffers)
    std::deque<Entry> m_q;
    std::deque<Pending> m_pending;
    int m_w = 0, m_h = 0, m_outW = 0, m_outH = 0;
    bool m_hostOut = false, m_dmabufFailed = false;
    uint64_t m_generated = 0, m_cuts = 0, m_dmabufFrames = 0, m_hostFrames = 0;
};

} // namespace pmc
