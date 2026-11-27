// SPDX-License-Identifier: GPL-3.0-or-later
// PushMPC capture mode: the Vulkan device and the screen (cap_vk.h).
#include "cap_vk.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#ifndef _WIN32
#include <unistd.h>
#endif
#include "pmc_present.h"   // kSpv_pmc_present (shaders/present.comp, built by CMake)
#if defined(__APPLE__)
#include <dlfcn.h>
#include <cstdio>
#include "mac_vk_probe.h"  // MoltenVK's instance tried in a process of its own first (it can abort the process)
#endif

namespace pmc {
namespace {
void QLock(void* c, uint32_t, uint32_t) { static_cast<std::mutex*>(c)->lock(); }
void QUnlock(void* c, uint32_t, uint32_t) { static_cast<std::mutex*>(c)->unlock(); }
std::string Lower(std::string s) { for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }
bool HasDevExt(VkPhysicalDevice p, const char* n)
{
    uint32_t c = 0;
    vkEnumerateDeviceExtensionProperties(p, nullptr, &c, nullptr);
    std::vector<VkExtensionProperties> e(c);
    if (c) vkEnumerateDeviceExtensionProperties(p, nullptr, &c, e.data());
    for (auto& x : e) if (std::strcmp(x.extensionName, n) == 0) return true;
    return false;
}
struct PresentPush { int w, h, cw, ch, offY, offU, offV, pitchY, pitchC, dw, dh, vx, vy, vw, vh, swapRB, bits, pq, outMode; float sdrWhite; };
} // namespace

Vk::~Vk()
{
    m_ctx.reset();
    if (dev) { vkDeviceWaitIdle(dev); vkDestroyDevice(dev, nullptr); }
    if (inst) vkDestroyInstance(inst, nullptr);
}

bool Vk::CreateInstance(const std::vector<const char*>& surfaceExts, std::string& err)
{
    std::string why;
    if (!pmvs::gpu::LoadVulkan(why)) { err = "Vulkan is not installed (" + why + ")"; return false; }
#if defined(__APPLE__)
    {   // MoltenVK's first instance in pushmpc-vulkan-probe (next to pushmpc-capture) first: Metal can abort the process inside
        // vkCreateInstance on some GPUs (mac_vk_probe.h). A probe that died keeps Vulkan out: capture runs the host engines
        Dl_info di;
        std::string lib, note;
        if (vkGetInstanceProcAddr && dladdr(reinterpret_cast<void*>(vkGetInstanceProcAddr), &di) && di.dli_fname) lib = di.dli_fname;
        const bool ok = pmvs::gpu::MacVulkanProbe(lib, why, note);
        if (!note.empty()) std::fprintf(stderr, "pushmpc-capture: %s\n", note.c_str());
        if (!ok) { err = "Vulkan is not usable on this Mac (" + why + ")"; return false; }
    }
#endif
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> have(n);
    if (n) vkEnumerateInstanceExtensionProperties(nullptr, &n, have.data());
    auto has = [&](const char* e) { for (auto& x : have) if (std::strcmp(x.extensionName, e) == 0) return true; return false; };
    std::vector<const char*> exts;
    surfaces = !surfaceExts.empty();
    for (const char* e : surfaceExts) if (!has(e)) { surfaces = false; err = std::string("the Vulkan driver has no ") + e; }
    if (!surfaceExts.empty() && !surfaces) return false;
    for (const char* e : surfaceExts) exts.push_back(e);
    VkInstanceCreateInfo ci{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    if (has(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) { exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME); ci.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR; }
    if (has(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) exts.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    // HDR swapchains (HDR10 PQ): MoltenVK, Mesa / NVIDIA on a Wayland compositor with the colour-management protocol
    if (surfaces && has(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME)) { exts.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME); colorspaces = true; }
    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "PushMPC capture"; app.pEngineName = "PushMPC"; app.apiVersion = VK_API_VERSION_1_2;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = uint32_t(exts.size()); ci.ppEnabledExtensionNames = exts.data();
    const VkResult r = vkCreateInstance(&ci, nullptr, &inst);
    if (r != VK_SUCCESS) { err = "vkCreateInstance failed (" + std::to_string(int(r)) + ")"; inst = VK_NULL_HANDLE; return false; }
    volkLoadInstance(inst);
    return true;
}

bool Vk::CreateDevice(VkSurfaceKHR surface, const std::string& want, bool allowCpu, std::string& err)
{
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    if (n) vkEnumeratePhysicalDevices(inst, &n, devs.data());
    if (devs.empty()) { err = "Vulkan lists no graphics card"; return false; }
    const std::string w = Lower(want);
    char* endp = nullptr;
    const long index = w.empty() ? -1 : std::strtol(w.c_str(), &endp, 10);
    const bool byIndex = !w.empty() && endp && *endp == 0;
    int best = -1, bestScore = -1, bestFam = -1;
    std::string rejected;
    for (size_t i = 0; i < devs.size(); ++i) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(devs[i], &p);
        const std::string nm = p.deviceName;
        const bool named = !w.empty() && w != "auto" && (byIndex ? long(i) == index : Lower(nm).find(w) != std::string::npos);
        if (!w.empty() && w != "auto" && !named) continue;
        auto reject = [&](const std::string& why) { rejected += (rejected.empty() ? "" : "; ") + nm + ": " + why; };
        if (VK_API_VERSION_MAJOR(p.apiVersion) == 1 && VK_API_VERSION_MINOR(p.apiVersion) < 2) { reject("Vulkan < 1.2"); continue; }
        VkPhysicalDeviceVulkan12Features f12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        VkPhysicalDeviceFeatures2 f2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        f2.pNext = &f12;
        vkGetPhysicalDeviceFeatures2(devs[i], &f2);
        if (!f12.timelineSemaphore || !f2.features.shaderStorageImageExtendedFormats) { reject("missing features"); continue; }
        const bool cpu = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
        if (cpu && !named && !allowCpu) { reject("a software rasteriser"); continue; }
        if (surface && !HasDevExt(devs[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME)) { reject("no swapchain"); continue; }
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, nullptr);
        std::vector<VkQueueFamilyProperties> q(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, q.data());
        int fam = -1;
        for (int pass = 0; pass < 2 && fam < 0; ++pass)
            for (uint32_t k = 0; k < qn && fam < 0; ++k) {
                if (!(q[k].queueFlags & VK_QUEUE_COMPUTE_BIT)) continue;
                if (pass == 0 && !(q[k].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
                VkBool32 ok = VK_TRUE;
                if (surface) vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], k, surface, &ok);
                if (ok) fam = int(k);
            }
        if (fam < 0) { reject(surface ? "cannot show on this window" : "no compute queue"); continue; }
        const int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 4 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 3
                        : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU ? 2 : 1;
        if (score > bestScore) { bestScore = score; best = int(i); bestFam = fam; }
    }
    if (best < 0) { err = "no usable Vulkan graphics card" + (rejected.empty() ? std::string() : " (" + rejected + ")"); return false; }
    phys = devs[size_t(best)];
    family = uint32_t(bestFam);
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys, &props);
    name = props.deviceName;
    cpuDevice = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    limits = props.limits;
    vkGetPhysicalDeviceMemoryProperties(phys, &m_mem);

    std::vector<const char*> exts;
    if (surface) exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (HasDevExt(phys, "VK_KHR_portability_subset")) exts.push_back("VK_KHR_portability_subset");
#ifdef __linux__
    if (HasDevExt(phys, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) && HasDevExt(phys, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME)) {
        exts.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
        exts.push_back(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
        dmabuf = true;
    }
#endif
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qi.queueFamilyIndex = family; qi.queueCount = 1; qi.pQueuePriorities = &prio;
    VkPhysicalDeviceVulkan12Features f12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    f12.timelineSemaphore = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    f2.features.shaderStorageImageExtendedFormats = VK_TRUE;
    f2.pNext = &f12;
    // NVIDIA's optical-flow engine (as vk_context.cpp's own device does: VK_NV_optical_flow with synchronization2 and
    // format_feature_flags2, on an optical-flow queue family of its own); PUSHMPC_VK_NO_OPTICAL_FLOW=1 leaves it out
    VkPhysicalDeviceOpticalFlowFeaturesNV fOf{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV };
    VkPhysicalDeviceSynchronization2FeaturesKHR fSync2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR };
    int ofFam = -1;
    {
        const char* noOf = std::getenv("PUSHMPC_VK_NO_OPTICAL_FLOW");
        if (!(noOf && noOf[0] == '1') && HasDevExt(phys, VK_NV_OPTICAL_FLOW_EXTENSION_NAME) && HasDevExt(phys, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)
            && HasDevExt(phys, VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME)) {
            VkPhysicalDeviceOpticalFlowFeaturesNV qOf{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV };
            VkPhysicalDeviceFeatures2 q2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            q2.pNext = &qOf;
            vkGetPhysicalDeviceFeatures2(phys, &q2);
            uint32_t qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> q(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, q.data());
            for (uint32_t i = 0; i < qn && ofFam < 0; ++i) if ((q[i].queueFlags & VK_QUEUE_OPTICAL_FLOW_BIT_NV) && i != family) ofFam = int(i);
            if (qOf.opticalFlow && ofFam >= 0) {
                exts.push_back(VK_NV_OPTICAL_FLOW_EXTENSION_NAME);
                exts.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
                exts.push_back(VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME);
                fOf.opticalFlow = VK_TRUE; fSync2.synchronization2 = VK_TRUE;
                fOf.pNext = &fSync2; f12.pNext = &fOf;
            } else ofFam = -1;
        }
    }
    VkDeviceQueueCreateInfo qis[2] = { qi, qi };
    uint32_t qCount = 1;
    if (ofFam >= 0) { qis[1].queueFamilyIndex = uint32_t(ofFam); qCount = 2; }
    VkDeviceCreateInfo di{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    di.pNext = &f2;
    di.queueCreateInfoCount = qCount; di.pQueueCreateInfos = qis;
    di.enabledExtensionCount = uint32_t(exts.size()); di.ppEnabledExtensionNames = exts.data();
    const VkResult r = vkCreateDevice(phys, &di, nullptr, &dev);
    if (r != VK_SUCCESS) { err = "vkCreateDevice failed (" + std::to_string(int(r)) + ") on " + name; dev = VK_NULL_HANDLE; return false; }
    volkLoadDevice(dev);
    vkGetDeviceQueue(dev, family, 0, &queue);
    if (ofFam >= 0 && vkCreateOpticalFlowSessionNV) { ofFamily = uint32_t(ofFam); vkGetDeviceQueue(dev, ofFamily, 0, &ofQueue); opticalFlow = ofQueue != VK_NULL_HANDLE; }
    return true;
}

std::shared_ptr<pmvs::gpu::Context> Vk::EngineContext(std::string& why)
{
    if (m_ctx) return m_ctx;
    pmvs::gpu::ExternalDevice e;
    e.instance = inst; e.phys = phys; e.device = dev; e.getInstanceProcAddr = vkGetInstanceProcAddr;
    e.family = family; e.queueIndex = 0;
    e.queueLock = QLock; e.queueUnlock = QUnlock; e.lockCtx = &qlock;
    if (opticalFlow) { e.ofFamily = ofFamily; e.ofQueue = ofQueue; }
    m_ctx = pmvs::gpu::Context::Adopt(e, why);
    return m_ctx;
}

int Vk::MemType(uint32_t bits, VkMemoryPropertyFlags want) const
{
    for (uint32_t i = 0; i < m_mem.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (m_mem.memoryTypes[i].propertyFlags & want) == want) return int(i);
    return -1;
}

bool Vk::Buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host, VBuf& out, std::string& err)
{
    out = {};
    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev, &bi, nullptr, &out.b) != VK_SUCCESS) { err = "vkCreateBuffer failed"; return false; }
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(dev, out.b, &mr);
    int t = host ? MemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                 : MemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t < 0 && !host) t = MemType(mr.memoryTypeBits, 0);
    if (t < 0) { err = "no memory type for a buffer"; Free(out); return false; }
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = mr.size; ai.memoryTypeIndex = uint32_t(t);
    if (vkAllocateMemory(dev, &ai, nullptr, &out.m) != VK_SUCCESS) { err = "out of GPU memory (buffer)"; Free(out); return false; }
    vkBindBufferMemory(dev, out.b, out.m, 0);
    out.size = size;
    if (host && vkMapMemory(dev, out.m, 0, VK_WHOLE_SIZE, 0, &out.map) != VK_SUCCESS) { err = "vkMapMemory failed"; Free(out); return false; }
    return true;
}

void Vk::Free(VBuf& b)
{
    if (b.map) vkUnmapMemory(dev, b.m);
    if (b.b) vkDestroyBuffer(dev, b.b, nullptr);
    if (b.m) vkFreeMemory(dev, b.m, nullptr);
    b = {};
}

bool Vk::Image(int w, int h, VkFormat f, VkImageUsageFlags usage, VImg& out, std::string& err)
{
    out = {};
    VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = f; ii.extent = { uint32_t(w), uint32_t(h), 1 };
    ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage; ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &ii, nullptr, &out.i) != VK_SUCCESS) { err = "vkCreateImage failed"; return false; }
    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(dev, out.i, &mr);
    int t = MemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t < 0) t = MemType(mr.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = mr.size; ai.memoryTypeIndex = uint32_t(t);
    if (t < 0 || vkAllocateMemory(dev, &ai, nullptr, &out.m) != VK_SUCCESS) { err = "out of GPU memory (image)"; Free(out); return false; }
    vkBindImageMemory(dev, out.i, out.m, 0);
    VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = out.i; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = f; vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(dev, &vi, nullptr, &out.v) != VK_SUCCESS) { err = "vkCreateImageView failed"; Free(out); return false; }
    out.w = w; out.h = h;
    return true;
}

void Vk::Free(VImg& i)
{
    if (i.v) vkDestroyImageView(dev, i.v, nullptr);
    if (i.i) vkDestroyImage(dev, i.i, nullptr);
    if (i.m) vkFreeMemory(dev, i.m, nullptr);
    i = {};
}

bool Vk::Pipe(const uint32_t* code, size_t bytes, const std::vector<VkDescriptorType>& types, uint32_t push, VPipe& out, std::string& err)
{
    std::vector<VkDescriptorSetLayoutBinding> b(types.size());
    for (size_t i = 0; i < types.size(); ++i) { b[i] = {}; b[i].binding = uint32_t(i); b[i].descriptorType = types[i]; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; }
    VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = uint32_t(b.size()); li.pBindings = b.data();
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &out.dsl) != VK_SUCCESS) { err = "vkCreateDescriptorSetLayout failed"; return false; }
    VkPushConstantRange pr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, push };
    VkPipelineLayoutCreateInfo pi{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pi.setLayoutCount = 1; pi.pSetLayouts = &out.dsl; pi.pushConstantRangeCount = 1; pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(dev, &pi, nullptr, &out.pl) != VK_SUCCESS) { err = "vkCreatePipelineLayout failed"; return false; }
    VkShaderModuleCreateInfo si{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    si.codeSize = bytes; si.pCode = code;
    VkShaderModule sm = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &si, nullptr, &sm) != VK_SUCCESS) { err = "vkCreateShaderModule failed"; return false; }
    VkComputePipelineCreateInfo ci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    ci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", nullptr };
    ci.layout = out.pl;
    const VkResult r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &out.p);
    vkDestroyShaderModule(dev, sm, nullptr);
    if (r != VK_SUCCESS) { err = "vkCreateComputePipelines failed"; return false; }
    return true;
}

void Vk::Free(VPipe& p)
{
    if (p.p) vkDestroyPipeline(dev, p.p, nullptr);
    if (p.pl) vkDestroyPipelineLayout(dev, p.pl, nullptr);
    if (p.dsl) vkDestroyDescriptorSetLayout(dev, p.dsl, nullptr);
    p = {};
}

VkResult Vk::Submit(const VkSubmitInfo& si, VkFence fence)
{
    std::lock_guard<std::mutex> lk(qlock);
    return vkQueueSubmit(queue, 1, &si, fence);
}

bool Vk::ImportDmabuf(int fd, VkDeviceSize size, VBuf& out, std::string& err)
{
#ifdef __linux__
    out = {};
    if (!dmabuf) { err = "the Vulkan driver imports no dma-buf"; return false; }
    VkExternalMemoryBufferCreateInfo eb{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
    eb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.pNext = &eb; bi.size = size; bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev, &bi, nullptr, &out.b) != VK_SUCCESS) { err = "dma-buf: vkCreateBuffer failed"; return false; }
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(dev, out.b, &mr);
    const int d = dup(fd);
    if (d < 0) { err = "dma-buf: dup failed"; Free(out); return false; }
    VkMemoryFdPropertiesKHR fp{ VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
    if (!vkGetMemoryFdPropertiesKHR || vkGetMemoryFdPropertiesKHR(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, d, &fp) != VK_SUCCESS) { close(d); err = "dma-buf: the driver does not take this buffer"; Free(out); return false; }
    const uint32_t bits = mr.memoryTypeBits & fp.memoryTypeBits;
    int t = -1;
    for (int i = 0; i < 32 && t < 0; ++i) if (bits & (1u << i)) t = i;
    if (t < 0) { close(d); err = "dma-buf: no memory type fits"; Free(out); return false; }
    const off_t total = lseek(d, 0, SEEK_END);
    lseek(d, 0, SEEK_SET);
    VkImportMemoryFdInfoKHR imp{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR };
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT; imp.fd = d;
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.pNext = &imp;
    ai.allocationSize = total > 0 && VkDeviceSize(total) >= mr.size ? VkDeviceSize(total) : mr.size;
    ai.memoryTypeIndex = uint32_t(t);
    if (vkAllocateMemory(dev, &ai, nullptr, &out.m) != VK_SUCCESS) { close(d); err = "dma-buf: import failed"; Free(out); return false; }
    if (vkBindBufferMemory(dev, out.b, out.m, 0) != VK_SUCCESS) { err = "dma-buf: bind failed"; Free(out); return false; }
    out.size = size;
    return true;
#else
    (void)fd; (void)size; (void)out;
    err = "dma-buf: Linux only";
    return false;
#endif
}

// ------------------------------------------------------------------------------------------------ the screen
Screen::~Screen()
{
    if (!m_vk || !m_vk->dev) return;
    {
        std::lock_guard<std::mutex> lk(m_vk->qlock);
        vkQueueWaitIdle(m_vk->queue);
    }
    DropSwapchain();
    for (auto& s : m_slot) {
        if (s.acquire) vkDestroySemaphore(m_vk->dev, s.acquire, nullptr);
        if (s.fence) vkDestroyFence(m_vk->dev, s.fence, nullptr);
        m_vk->Free(s.upload);
    }
    if (m_dpool) vkDestroyDescriptorPool(m_vk->dev, m_dpool, nullptr);
    if (m_pool) vkDestroyCommandPool(m_vk->dev, m_pool, nullptr);
    m_vk->Free(m_present);
}

bool Screen::Init(Vk* vk, VkSurfaceKHR surface, int w, int h, bool hdr, std::string& err)
{
    m_vk = vk; m_surface = surface; m_w = w; m_h = h; m_hdrWanted = hdr;
    const VkDevice d = vk->dev;
    VkCommandPoolCreateInfo pi{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pi.queueFamilyIndex = vk->family;
    if (vkCreateCommandPool(d, &pi, nullptr, &m_pool) != VK_SUCCESS) { err = "vkCreateCommandPool failed"; return false; }
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 } };
    VkDescriptorPoolCreateInfo di{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    di.maxSets = 2; di.poolSizeCount = 2; di.pPoolSizes = ps;
    if (vkCreateDescriptorPool(d, &di, nullptr, &m_dpool) != VK_SUCCESS) { err = "vkCreateDescriptorPool failed"; return false; }
    if (!vk->Pipe(kSpv_pmc_present, sizeof(kSpv_pmc_present), { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE }, sizeof(PresentPush), m_present, err)) return false;
    for (auto& s : m_slot) {
        VkSemaphoreCreateInfo si{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (vkCreateSemaphore(d, &si, nullptr, &s.acquire) != VK_SUCCESS) { err = "vkCreateSemaphore failed"; return false; }
        VkFenceCreateInfo fi{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(d, &fi, nullptr, &s.fence) != VK_SUCCESS) { err = "vkCreateFence failed"; return false; }
        VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = m_pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(d, &ai, &s.cb) != VK_SUCCESS) { err = "vkAllocateCommandBuffers failed"; return false; }
        VkDescriptorSetAllocateInfo sa{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        sa.descriptorPool = m_dpool; sa.descriptorSetCount = 1; sa.pSetLayouts = &m_present.dsl;
        if (vkAllocateDescriptorSets(d, &sa, &s.set) != VK_SUCCESS) { err = "vkAllocateDescriptorSets failed"; return false; }
    }
    return MakeSwapchain(err);
}

bool Screen::MakeSwapchain(std::string& err)
{
    const VkDevice d = m_vk->dev;
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_vk->phys, m_surface, &caps);
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(m_vk->phys, m_surface, &nf, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(nf);
    if (nf) vkGetPhysicalDeviceSurfaceFormatsKHR(m_vk->phys, m_surface, &nf, fmts.data());
    if (fmts.empty()) { err = "the window offers no picture format"; return false; }
    VkSurfaceFormatKHR f = fmts[0];
    for (auto& x : fmts) if ((x.format == VK_FORMAT_B8G8R8A8_UNORM || x.format == VK_FORMAT_R8G8B8A8_UNORM) && x.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { f = x; break; }
    m_hdr = false;
    if (!m_hdrWanted) m_hdrWhy = "not asked for (an SDR display or desktop)";
    else if (!m_vk->colorspaces) m_hdrWhy = "the Vulkan driver has no VK_EXT_swapchain_colorspace";
    else {
        m_hdrWhy = "the window offers no HDR10 format (the compositor / display does not take HDR here)";
        for (auto& x : fmts)
            if (x.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT && (x.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || x.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32)) {
                f = x; m_hdr = true; m_hdrWhy.clear(); break;
            }
    }
    m_format = f.format;
    // the shader packs R in the low bits: a BGRA (or A2R10G10B10) swapchain gets B, G, R
    m_swapRB = f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { err = "the window's images take no copies"; return false; }
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) {
        ext.width = uint32_t(std::clamp<int>(m_w, int(caps.minImageExtent.width), int(std::max(caps.minImageExtent.width, caps.maxImageExtent.width))));
        ext.height = uint32_t(std::clamp<int>(m_h, int(caps.minImageExtent.height), int(std::max(caps.minImageExtent.height, caps.maxImageExtent.height))));
    }
    if (ext.width == 0 || ext.height == 0) { err = "the window has no size"; return false; }
    m_w = int(ext.width); m_h = int(ext.height);
    uint32_t count = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha))
        for (VkCompositeAlphaFlagBitsKHR a : { VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR })
            if (caps.supportedCompositeAlpha & a) { alpha = a; break; }
    VkSwapchainCreateInfoKHR ci{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface = m_surface; ci.minImageCount = count; ci.imageFormat = f.format; ci.imageColorSpace = f.colorSpace;
    ci.imageExtent = ext; ci.imageArrayLayers = 1; ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.preTransform = caps.currentTransform; ci.compositeAlpha = alpha;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR; ci.clipped = VK_TRUE;
    const VkResult r = vkCreateSwapchainKHR(d, &ci, nullptr, &m_sc);
    if (r != VK_SUCCESS) { err = "vkCreateSwapchainKHR failed (" + std::to_string(int(r)) + ")"; m_sc = VK_NULL_HANDLE; return false; }
    uint32_t ni = 0;
    vkGetSwapchainImagesKHR(d, m_sc, &ni, nullptr);
    m_images.resize(ni);
    vkGetSwapchainImagesKHR(d, m_sc, &ni, m_images.data());
    m_render.resize(ni);
    for (auto& s : m_render) { VkSemaphoreCreateInfo si{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO }; vkCreateSemaphore(d, &si, nullptr, &s); }
    // r32ui: the shader packs the swapchain's own 32-bit texel; the copy is raw (size-compatible formats)
    for (auto& s : m_slot) if (!m_vk->Image(m_w, m_h, VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, s.img, err)) return false;
    return true;
}

void Screen::DropSwapchain()
{
    const VkDevice d = m_vk->dev;
    for (auto& s : m_render) if (s) vkDestroySemaphore(d, s, nullptr);
    m_render.clear();
    m_images.clear();
    for (auto& s : m_slot) m_vk->Free(s.img);
    if (m_sc) vkDestroySwapchainKHR(d, m_sc, nullptr);
    m_sc = VK_NULL_HANDLE;
}

bool Screen::Resize(int w, int h, std::string& err)
{
    for (auto& s : m_slot) { if (s.wait) { s.wait(); s.wait = nullptr; } vkWaitForFences(m_vk->dev, 1, &s.fence, VK_TRUE, UINT64_MAX); }
    {
        std::lock_guard<std::mutex> lk(m_vk->qlock);
        vkQueueWaitIdle(m_vk->queue);
    }
    DropSwapchain();
    m_w = w; m_h = h;
    return MakeSwapchain(err);
}

int Screen::Begin(std::string& err)
{
    Slot& s = m_slot[m_cur];
    if (s.wait) { s.wait(); s.wait = nullptr; }
    vkWaitForFences(m_vk->dev, 1, &s.fence, VK_TRUE, UINT64_MAX);
    const VkResult r = vkAcquireNextImageKHR(m_vk->dev, m_sc, 2000000000ull, s.acquire, VK_NULL_HANDLE, &m_image);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_TIMEOUT || r == VK_NOT_READY) return 1;
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { err = "vkAcquireNextImageKHR failed (" + std::to_string(int(r)) + ")"; return -1; }
    return 0;
}

VkSemaphore Screen::AcquireSem() const { return m_slot[m_cur].acquire; }
VkSemaphore Screen::RenderSem() const { return m_render[m_image]; }
void Screen::SetWait(std::function<void()> wait) { m_slot[m_cur].wait = std::move(wait); }

bool Screen::Record(VkCommandBuffer cb, VkBuffer b, const YuvLayout& L, std::string& err)
{
    Slot& s = m_slot[m_cur];
    const VkDevice d = m_vk->dev;
    VkDescriptorBufferInfo bi{ b, 0, VK_WHOLE_SIZE };
    VkDescriptorImageInfo ii{ VK_NULL_HANDLE, s.img.v, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w[2]{};
    for (int i = 0; i < 2; ++i) { w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = s.set; w[i].dstBinding = uint32_t(i); w[i].descriptorCount = 1; }
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[0].pBufferInfo = &bi;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[1].pImageInfo = &ii;
    vkUpdateDescriptorSets(d, 2, w, 0, nullptr);
    auto barrier = [&](VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
        VkImageMemoryBarrier m{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        m.oldLayout = from; m.newLayout = to; m.srcAccessMask = sa; m.dstAccessMask = da;
        m.srcQueueFamilyIndex = m.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        m.image = img; m.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cb, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &m);
    };
    barrier(s.img.i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    // the picture fills the window with its aspect ratio kept (bars where they differ)
    const double sc = std::min(double(m_w) / double(L.w), double(m_h) / double(L.h));
    const int vw = std::max(1, std::min(m_w, int(std::lround(L.w * sc)))), vh = std::max(1, std::min(m_h, int(std::lround(L.h * sc))));
    PresentPush p{ L.w, L.h, L.cw, L.ch, int(L.offY), int(L.offU), int(L.offV), L.pitchY, L.pitchC, m_w, m_h, (m_w - vw) / 2, (m_h - vh) / 2, vw, vh, m_swapRB ? 1 : 0,
                   L.bits, L.pq ? 1 : 0, m_hdr ? 1 : 0, m_sdrWhite };
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m_present.p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m_present.pl, 0, 1, &s.set, 0, nullptr);
    vkCmdPushConstants(cb, m_present.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(cb, uint32_t((m_w + 15) / 16), uint32_t((m_h + 7) / 8), 1);
    barrier(s.img.i, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    const VkImage dst = m_images[m_image];
    barrier(dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageCopy c{};
    c.srcSubresource = c.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    c.extent = { uint32_t(m_w), uint32_t(m_h), 1 };
    vkCmdCopyImage(cb, s.img.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    (void)err;
    return true;
}

bool Screen::ShowHost(const I420& pic, std::string& err)
{
    Slot& s = m_slot[m_cur];
    YuvLayout L;
    L.w = pic.w; L.h = pic.h; L.cw = pic.cw(); L.ch = pic.ch(); L.pitchY = pic.w * pic.bps(); L.pitchC = pic.cw() * pic.bps();
    L.bits = pic.bits; L.pq = pic.pq;
    L.offY = 0;
    L.offU = (VkDeviceSize(pic.y.size()) + 255) & ~VkDeviceSize(255);
    L.offV = (L.offU + VkDeviceSize(pic.u.size()) + 255) & ~VkDeviceSize(255);
    const VkDeviceSize need = L.offV + VkDeviceSize(pic.v.size()) + 4;
    if (s.upload.size < need) {
        m_vk->Free(s.upload);
        if (!m_vk->Buffer(need, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, s.upload, err)) return false;
    }
    uint8_t* base = static_cast<uint8_t*>(s.upload.map);
    std::memcpy(base + L.offY, pic.y.data(), pic.y.size());
    std::memcpy(base + L.offU, pic.u.data(), pic.u.size());
    std::memcpy(base + L.offV, pic.v.data(), pic.v.size());
    vkResetFences(m_vk->dev, 1, &s.fence);
    vkResetCommandBuffer(s.cb, 0);
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s.cb, &bi);
    if (!Record(s.cb, s.upload.b, L, err)) return false;
    vkEndCommandBuffer(s.cb);
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = 1; si.pWaitSemaphores = &s.acquire; si.pWaitDstStageMask = &stage;
    si.commandBufferCount = 1; si.pCommandBuffers = &s.cb;
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &m_render[m_image];
    const VkResult r = m_vk->Submit(si, s.fence);
    if (r != VK_SUCCESS) { err = "vkQueueSubmit failed (" + std::to_string(int(r)) + ")"; return false; }
    return true;
}

int Screen::Present(std::string& err)
{
    VkPresentInfoKHR pi{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &m_render[m_image];
    pi.swapchainCount = 1; pi.pSwapchains = &m_sc; pi.pImageIndices = &m_image;
    VkResult r;
    {
        std::lock_guard<std::mutex> lk(m_vk->qlock);
        r = vkQueuePresentKHR(m_vk->queue, &pi);
    }
    m_cur ^= 1;
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) return 1;
    if (r != VK_SUCCESS) { err = "vkQueuePresentKHR failed (" + std::to_string(int(r)) + ")"; return -1; }
    return 0;
}

} // namespace pmc
