// SPDX-License-Identifier: GPL-3.0-or-later
// pushmpc.Enhance, portable GPU engine: the Vulkan device, one per process (mpv reloads the script on every seek; the
// device stays open). Vulkan 1.2 compute: Linux (any vendor's driver, Mesa lavapipe for tests), macOS through MoltenVK
// (Metal), Windows on any GPU whose driver ships Vulkan. The Vulkan library is loaded at run time (volk): a machine
// without one gets the CPU engine instead, with the reason in the log.
#pragma once
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include "volk.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pmvs::gpu {

struct DeviceInfo {
    std::string name, driver;
    uint32_t vendorId = 0, deviceId = 0, apiVersion = 0;
    VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    bool cpu = false;              // a software rasteriser (lavapipe, SwiftShader): correct, not fast
    std::string Describe() const;  // "AMD Radeon RX 7800 XT (discrete, Vulkan 1.3.280, RADV)"
};

// which device: "" or "auto" (the best hardware device: discrete, then integrated, then virtual), a number (the
// index in the list), or a part of the name. allowCpu: a software device may be picked by Auto (tests); asked for by
// name or number it always may.
struct DeviceRequest { std::string device; bool allowCpu = false; bool validation = false; };

// a device someone else owns (FFmpeg's AVVulkanDeviceContext in mpv / libavfilter): the engine runs on it, so its
// frames need no copy between devices. queueLock / queueUnlock guard the shared queue (FFmpeg's lock_queue); the
// device and the instance are never destroyed here.
struct ExternalDevice {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
    uint32_t family = 0, queueIndex = 0;        // a compute-capable queue
    void (*queueLock)(void* ctx, uint32_t family, uint32_t index) = nullptr;
    void (*queueUnlock)(void* ctx, uint32_t family, uint32_t index) = nullptr;
    void* lockCtx = nullptr;
};

struct Buffer { VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkDeviceSize size = 0; void* map = nullptr; };
struct Image { VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE, viewRRR = VK_NULL_HANDLE; VkFormat format = VK_FORMAT_UNDEFINED; int w = 0, h = 0; };

class Context {
public:
    // the process's device, opened on first use with this request (later requests reuse it when they match, else the
    // device is reopened once nothing uses it); null with the reason when there is none
    static std::shared_ptr<Context> Get(const DeviceRequest& req, std::string& why);
    // the engine on an external device (see ExternalDevice); null with the reason when it lacks what the engine needs
    static std::shared_ptr<Context> Adopt(const ExternalDevice& ext, std::string& why);
    bool External() const { return m_external; }
    // every device Vulkan lists (for the log and tests); empty with the reason when Vulkan is missing
    static std::vector<DeviceInfo> List(std::string& why);
    ~Context();

    const DeviceInfo& Info() const { return m_info; }
    VkDevice Dev() const { return m_dev; }
    VkPhysicalDevice Phys() const { return m_phys; }
    uint32_t Family() const { return m_family; }
    const VkPhysicalDeviceLimits& Limits() const { return m_limits; }
    uint64_t ValidationErrors() const;   // with validation on: errors the layer reported (tests fail on any)

    // the queue is shared by every engine of the process
    VkResult Submit(const VkSubmitInfo& si, VkFence fence);
    // NVIDIA's optical-flow engine (VK_NV_optical_flow, Turing and later except TU117): its own queue family
    bool OpticalFlow() const { return m_ofQueue != VK_NULL_HANDLE; }
    uint32_t OfFamily() const { return m_ofFamily; }
    VkResult SubmitOf(const VkSubmitInfo& si);
    VkResult WaitIdle();

    bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host, Buffer& out, std::string& err);
    bool CreateImage(int w, int h, VkFormat format, VkImageUsageFlags usage, bool rrrView, Image& out, std::string& err, uint32_t ofUsage = 0);   // ofUsage: VkOpticalFlowUsageFlagsNV (shared with the optical-flow queue)
    void Destroy(Buffer& b);
    void Destroy(Image& i);
    bool FormatOk(VkFormat f, VkFormatFeatureFlags need) const;

private:
    Context() = default;
    bool Open(const DeviceRequest& req, std::string& err);
    int MemoryType(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid = 0) const;

    VkInstance m_inst = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;
    VkPhysicalDevice m_phys = VK_NULL_HANDLE;
    VkDevice m_dev = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    uint32_t m_family = 0, m_ofFamily = 0;
    VkQueue m_ofQueue = VK_NULL_HANDLE;
    std::mutex m_ofLock;
    VkPhysicalDeviceLimits m_limits{};
    VkPhysicalDeviceMemoryProperties m_mem{};
    DeviceInfo m_info;
    DeviceRequest m_req;
    std::mutex m_queueLock;
    bool m_external = false;
    ExternalDevice m_ext;
};

// load the Vulkan library once per process; false with the reason when there is none
bool LoadVulkan(std::string& why);

} // namespace pmvs::gpu
