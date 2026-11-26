// SPDX-License-Identifier: GPL-3.0-or-later
// PushMPC capture mode: the Vulkan device capture mode owns (instance with the window system's surface, device with the
// swapchain and dma-buf import), shared with the portable GPU engine (pmvs::gpu::Context::Adopt), and the screen: a
// swapchain the present shader fills from a YUV buffer (the engine's output, or a picture from host memory), scaled to
// the window with the aspect ratio kept.
#pragma once
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include "volk.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "cap_core.h"
#include "vk_context.h"

namespace pmc {

struct VBuf { VkBuffer b = VK_NULL_HANDLE; VkDeviceMemory m = VK_NULL_HANDLE; VkDeviceSize size = 0; void* map = nullptr; };
struct VImg { VkImage i = VK_NULL_HANDLE; VkDeviceMemory m = VK_NULL_HANDLE; VkImageView v = VK_NULL_HANDLE; int w = 0, h = 0; };
struct VPipe { VkDescriptorSetLayout dsl = VK_NULL_HANDLE; VkPipelineLayout pl = VK_NULL_HANDLE; VkPipeline p = VK_NULL_HANDLE; };

class Vk {
public:
    ~Vk();
    // surfaceExts: the instance extensions of the window system (empty: no screen, tests)
    bool CreateInstance(const std::vector<const char*>& surfaceExts, std::string& err);
    bool CreateDevice(VkSurfaceKHR surface, const std::string& want, bool allowCpu, std::string& err);
    // the engine's context on this device (one per process)
    std::shared_ptr<pmvs::gpu::Context> EngineContext(std::string& why);

    bool Buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host, VBuf& out, std::string& err);
    void Free(VBuf& b);
    bool Image(int w, int h, VkFormat f, VkImageUsageFlags usage, VImg& out, std::string& err);
    void Free(VImg& i);
    bool Pipe(const uint32_t* code, size_t bytes, const std::vector<VkDescriptorType>& types, uint32_t push, VPipe& out, std::string& err);
    void Free(VPipe& p);
    VkResult Submit(const VkSubmitInfo& si, VkFence fence);
    // a linear dma-buf as a storage buffer the shaders read (the fd stays the caller's)
    bool ImportDmabuf(int fd, VkDeviceSize size, VBuf& out, std::string& err);
    int MemType(uint32_t bits, VkMemoryPropertyFlags want) const;

    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    std::mutex qlock;
    std::string name;
    bool cpuDevice = false, dmabuf = false, surfaces = false;
    VkPhysicalDeviceLimits limits{};
private:
    VkPhysicalDeviceMemoryProperties m_mem{};
    std::shared_ptr<pmvs::gpu::Context> m_ctx;
};

// where a planar 8-bit YUV 4:2:0 picture sits in a buffer (byte offsets and pitches)
struct YuvLayout { VkDeviceSize offY = 0, offU = 0, offV = 0; int pitchY = 0, pitchC = 0, w = 0, h = 0, cw = 0, ch = 0; };

// the swapchain and the present shader
class Screen {
public:
    ~Screen();
    bool Init(Vk* vk, VkSurfaceKHR surface, int w, int h, std::string& err);
    bool Resize(int w, int h, std::string& err);
    int Width() const { return m_w; }
    int Height() const { return m_h; }
    // waits until this frame's resources are free (the previous use of the slot), then acquires the next image (the
    // screen's refresh). false + err on a failure; 0 = acquired, 1 = the swapchain must be made again (Resize)
    int Begin(std::string& err);
    // records the picture (yuv in buffer b, layout L) into cb: the present shader into this frame's image, its copy into
    // the swapchain image
    bool Record(VkCommandBuffer cb, VkBuffer b, const YuvLayout& L, std::string& err);
    // the semaphores of this frame for a submission the caller makes (the engine's Export)
    VkSemaphore AcquireSem() const;
    VkSemaphore RenderSem() const;
    // after the caller's submission: what to wait for before the slot is used again
    void SetWait(std::function<void()> wait);
    // a picture from host memory: upload, record, submit, present
    bool ShowHost(const I420& pic, std::string& err);
    // presents the acquired image (after the caller's submission signalled RenderSem). 1 = make the swapchain again
    int Present(std::string& err);
    VkFormat Format() const { return m_format; }
private:
    bool MakeSwapchain(std::string& err);
    void DropSwapchain();
    struct Slot {
        VkSemaphore acquire = VK_NULL_HANDLE;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VImg img;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VBuf upload;
        std::function<void()> wait;
    };
    Vk* m_vk = nullptr;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkSwapchainKHR m_sc = VK_NULL_HANDLE;
    VkFormat m_format = VK_FORMAT_UNDEFINED;
    bool m_swapRB = false;
    int m_w = 0, m_h = 0;
    std::vector<VkImage> m_images;
    std::vector<VkSemaphore> m_render;   // per swapchain image
    Slot m_slot[2];
    int m_cur = 0;
    uint32_t m_image = 0;
    VkCommandPool m_pool = VK_NULL_HANDLE;
    VkDescriptorPool m_dpool = VK_NULL_HANDLE;
    VPipe m_present;
};

} // namespace pmc
