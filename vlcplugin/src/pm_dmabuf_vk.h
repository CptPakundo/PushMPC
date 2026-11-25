// PushMPC for VLC, zero copy on Linux: VA-API surfaces exported as dmabufs, imported into the GPU engine's Vulkan device
// (VK_EXT_external_memory_dma_buf + VK_EXT_image_drm_format_modifier, one image per layer with the surface's own
// modifier and plane layout) and converted on the GPU: source surface -> the engine's planar staging (dmabuf_in.comp),
// finished frame -> output surface (dmabuf_out.comp). GPL-3.0-or-later.
#pragma once
#include <memory>
#include <string>
#include "gpu_engine.h"
#include "pm_engine.h"

namespace pmdb {

struct Imported {
    VkImage img[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkDeviceMemory mem[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkImageView view[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    int w = 0, h = 0, cw = 0, ch = 0, bits = 8;
    bool output = false;
};

class DmabufIO {
public:
    ~DmabufIO();
    // the pipelines on the engine's device; false with the reason (no dmabuf support, no 16-bit storage formats, ...)
    bool Init(const std::shared_ptr<pmvs::gpu::Context>& ctx, std::string& why);
    bool Import(const pm_dmabuf& d, bool output, Imported& out, std::string& err);
    void Release(Imported& im);
    // into the engine's command buffer (the engine orders them on its timeline)
    bool RecordIn(VkCommandBuffer cb, const Imported& im, const pmvs::gpu::Engine::PlanarDesc& d, std::string& err);
    bool RecordOut(VkCommandBuffer cb, const Imported& im, const pmvs::gpu::Engine::PackedDesc& d, std::string& err);
    // once the recorded work has finished: the descriptor sets go back
    void Recycle();

private:
    struct Pipe { VkDescriptorSetLayout dsl = VK_NULL_HANDLE; VkPipelineLayout pl = VK_NULL_HANDLE; VkPipeline p = VK_NULL_HANDLE; };
    bool MakePipe(const uint32_t* code, size_t bytes, const VkDescriptorType* types, int n, uint32_t push, Pipe& out, std::string& err);
    VkDescriptorSet Alloc(const Pipe& p, std::string& err);
    void Acquire(VkCommandBuffer cb, const Imported& im, bool output) const;
    void ReleaseToForeign(VkCommandBuffer cb, const Imported& im) const;
    std::shared_ptr<pmvs::gpu::Context> m_ctx;
    Pipe m_in, m_out8, m_out16;
    VkSampler m_sampler = VK_NULL_HANDLE;
    VkDescriptorPool m_pool = VK_NULL_HANDLE;
};

} // namespace pmdb
