// SPDX-License-Identifier: GPL-3.0-or-later
// pushmpc.Enhance, portable GPU engine: the Vulkan device (see vk_context.h).
#include "vk_context.h"
#if !defined(_WIN32) && !defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifndef _WIN32
#include <dlfcn.h>
#endif

namespace pmvs::gpu {

namespace {
std::atomic<uint64_t> g_validationErrors{0};
std::mutex g_ctxLock;
std::weak_ptr<Context> g_ctx;

std::string Lower(std::string s) { for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }
const char* TypeName(VkPhysicalDeviceType t)
{
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "software";
    default: return "other";
    }
}
int Score(const DeviceInfo& d)
{
    switch (d.type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return 1;
    default: return 0;
    }
}
VKAPI_ATTR VkBool32 VKAPI_CALL OnDebug(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* d, void*)
{
    if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        g_validationErrors++;
        std::fprintf(stderr, "vulkan validation: %s\n", d && d->pMessage ? d->pMessage : "?");
    }
    return VK_FALSE;
}
bool HasLayer(const char* name)
{
    uint32_t n = 0;
    if (vkEnumerateInstanceLayerProperties(&n, nullptr) != VK_SUCCESS) return false;
    std::vector<VkLayerProperties> v(n);
    vkEnumerateInstanceLayerProperties(&n, v.data());
    for (auto& l : v) if (std::strcmp(l.layerName, name) == 0) return true;
    return false;
}
bool HasInstanceExt(const char* name)
{
    uint32_t n = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr) != VK_SUCCESS) return false;
    std::vector<VkExtensionProperties> v(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, v.data());
    for (auto& e : v) if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}
bool HasDeviceExt(VkPhysicalDevice p, const char* name)
{
    uint32_t n = 0;
    if (vkEnumerateDeviceExtensionProperties(p, nullptr, &n, nullptr) != VK_SUCCESS) return false;
    std::vector<VkExtensionProperties> v(n);
    vkEnumerateDeviceExtensionProperties(p, nullptr, &n, v.data());
    for (auto& e : v) if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}
DeviceInfo InfoOf(VkPhysicalDevice p)
{
    VkPhysicalDeviceDriverProperties drv{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
    VkPhysicalDeviceProperties2 pr{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    VkPhysicalDeviceProperties base{};
    vkGetPhysicalDeviceProperties(p, &base);
    DeviceInfo d;
    d.name = base.deviceName; d.vendorId = base.vendorID; d.deviceId = base.deviceID; d.apiVersion = base.apiVersion; d.type = base.deviceType;
    d.cpu = base.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
    if (VK_API_VERSION_MINOR(base.apiVersion) >= 2 || VK_API_VERSION_MAJOR(base.apiVersion) > 1) {
        pr.pNext = &drv;
        vkGetPhysicalDeviceProperties2(p, &pr);
        d.driver = drv.driverName;
        if (drv.driverInfo[0]) d.driver += std::string(" ") + drv.driverInfo;
    }
    return d;
}
// the instance every query and the device share
VkInstance g_inst = VK_NULL_HANDLE;
bool g_portEnum = false, g_validation = false, g_debugUtils = false;
bool MakeInstance(bool validation, std::string& err)
{
    if (g_inst) return true;
    if (const char* v = std::getenv("PUSHMPC_VK_VALIDATION")) validation = validation || (v[0] == '1');
    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "PushMPC"; app.pEngineName = "PushMPC portable GPU engine"; app.apiVersion = VK_API_VERSION_1_2;
    std::vector<const char*> exts, layers;
    VkInstanceCreateInfo ci{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    if (HasInstanceExt(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {   // MoltenVK through the loader
        exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        ci.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        g_portEnum = true;
    }
    if (HasInstanceExt(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) exts.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    if (validation && HasLayer("VK_LAYER_KHRONOS_validation")) { layers.push_back("VK_LAYER_KHRONOS_validation"); g_validation = true; }
    if (g_validation && HasInstanceExt(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) { exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME); g_debugUtils = true; }
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = uint32_t(exts.size()); ci.ppEnabledExtensionNames = exts.data();
    ci.enabledLayerCount = uint32_t(layers.size()); ci.ppEnabledLayerNames = layers.data();
    const VkResult r = vkCreateInstance(&ci, nullptr, &g_inst);
    if (r != VK_SUCCESS) { err = "vkCreateInstance failed (" + std::to_string(int(r)) + ")"; g_inst = VK_NULL_HANDLE; return false; }
    volkLoadInstance(g_inst);
    return true;
}
} // namespace

std::string DeviceInfo::Describe() const
{
    char b[512];
    std::snprintf(b, sizeof(b), "%s (%s, Vulkan %u.%u.%u%s%s)", name.c_str(), TypeName(type), VK_API_VERSION_MAJOR(apiVersion), VK_API_VERSION_MINOR(apiVersion),
                  VK_API_VERSION_PATCH(apiVersion), driver.empty() ? "" : ", ", driver.c_str());
    return b;
}

bool LoadVulkan(std::string& why)
{
    static std::mutex m;
    static int state = 0;   // 0 not tried, 1 loaded, -1 missing
    static std::string reason;
    std::lock_guard<std::mutex> lk(m);
    if (state == 0) {
        const char* env = std::getenv("PUSHMPC_VULKAN_LIBRARY");
#ifndef _WIN32
        if (env && *env) {
            if (void* h = dlopen(env, RTLD_NOW | RTLD_LOCAL)) {
                if (auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(h, "vkGetInstanceProcAddr"))) { volkInitializeCustom(gipa); state = 1; }
            }
            if (state != 1) reason = std::string("PUSHMPC_VULKAN_LIBRARY=") + env + " could not be loaded";
        }
#else
        (void)env;
#endif
        if (state != 1 && volkInitialize() == VK_SUCCESS) state = 1;
#if defined(__APPLE__)
        // Homebrew (vulkan-loader + molten-vk, or molten-vk alone) is not on dyld's default search path
        if (state != 1) {
            for (const char* p : { "/opt/homebrew/lib/libvulkan.1.dylib", "/usr/local/lib/libvulkan.1.dylib", "/opt/homebrew/lib/libMoltenVK.dylib", "/usr/local/lib/libMoltenVK.dylib" }) {
                void* h = dlopen(p, RTLD_NOW | RTLD_LOCAL);
                if (!h) continue;
                if (auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(h, "vkGetInstanceProcAddr"))) { volkInitializeCustom(gipa); state = 1; break; }
            }
        }
#endif
        if (state != 1) {
            state = -1;
            if (reason.empty())
#if defined(_WIN32)
                reason = "no Vulkan driver (vulkan-1.dll not found)";
#elif defined(__APPLE__)
                reason = "no Vulkan library (install MoltenVK: brew install molten-vk)";
#else
                reason = "no Vulkan loader (libvulkan.so.1 not found)";
#endif
        }
    }
    why = reason;
    return state == 1;
}

std::vector<DeviceInfo> Context::List(std::string& why)
{
    std::vector<DeviceInfo> out;
    std::lock_guard<std::mutex> lk(g_ctxLock);
    if (!LoadVulkan(why) || !MakeInstance(false, why)) return out;
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_inst, &n, nullptr);
    std::vector<VkPhysicalDevice> v(n);
    if (n) vkEnumeratePhysicalDevices(g_inst, &n, v.data());
    for (auto p : v) out.push_back(InfoOf(p));
    if (out.empty()) why = "Vulkan lists no device";
    return out;
}

std::shared_ptr<Context> Context::Get(const DeviceRequest& req, std::string& why)
{
    std::lock_guard<std::mutex> lk(g_ctxLock);
    if (auto c = g_ctx.lock()) {
        if (c->m_req.device == req.device && c->m_req.allowCpu == req.allowCpu) return c;
    }
    std::shared_ptr<Context> c(new Context());
    if (!c->Open(req, why)) return nullptr;
    g_ctx = c;
    return c;
}

std::shared_ptr<Context> Context::Adopt(const ExternalDevice& ext, std::string& why)
{
    std::lock_guard<std::mutex> lk(g_ctxLock);
    if (!ext.instance || !ext.phys || !ext.device || !ext.getInstanceProcAddr) { why = "external Vulkan device: missing handles"; return nullptr; }
    if (auto c = g_ctx.lock()) {
        if (c->m_external && c->m_dev == ext.device && c->m_family == ext.family && c->m_ext.queueIndex == ext.queueIndex) return c;
        if (!c->m_external) { why = "the engine already runs on its own Vulkan device in this process"; return nullptr; }
    }
    // the function pointers come from the owner's loader and instance (volk keeps one set per process)
    volkInitializeCustom(ext.getInstanceProcAddr);
    volkLoadInstanceOnly(ext.instance);
    volkLoadDevice(ext.device);
    std::shared_ptr<Context> c(new Context());
    c->m_external = true; c->m_ext = ext;
    c->m_inst = ext.instance; c->m_phys = ext.phys; c->m_dev = ext.device; c->m_family = ext.family;
    c->m_info = InfoOf(ext.phys);
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(ext.phys, &props);
    c->m_limits = props.limits;
    vkGetPhysicalDeviceMemoryProperties(ext.phys, &c->m_mem);
    if (VK_API_VERSION_MAJOR(props.apiVersion) == 1 && VK_API_VERSION_MINOR(props.apiVersion) < 2) { why = c->m_info.name + ": Vulkan < 1.2"; return nullptr; }
    // the storage formats the engine writes (pyramid R8_UINT, planes R8 / R16, intermediates R16)
    for (VkFormat f : { VK_FORMAT_R8_UINT, VK_FORMAT_R8_UNORM, VK_FORMAT_R16_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R16G16_UNORM })
        if (!c->FormatOk(f, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) { why = c->m_info.name + ": no storage support for a plane format the engine writes"; return nullptr; }
    vkGetDeviceQueue(ext.device, ext.family, ext.queueIndex, &c->m_queue);
    if (!c->m_queue) { why = "external Vulkan device: no queue " + std::to_string(ext.family) + "/" + std::to_string(ext.queueIndex); return nullptr; }
    g_ctx = c;
    return c;
}

bool Context::Open(const DeviceRequest& req, std::string& err)
{
    m_req = req;
    if (!LoadVulkan(err)) return false;
    if (!MakeInstance(req.validation, err)) return false;
    m_inst = g_inst;
    if (g_debugUtils && vkCreateDebugUtilsMessengerEXT) {
        VkDebugUtilsMessengerCreateInfoEXT mi{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        mi.pfnUserCallback = OnDebug;
        vkCreateDebugUtilsMessengerEXT(m_inst, &mi, nullptr, &m_messenger);
    }
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(m_inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    if (n) vkEnumeratePhysicalDevices(m_inst, &n, devs.data());
    if (devs.empty()) { err = "Vulkan lists no device"; return false; }

    // what the engine needs: Vulkan 1.2 (timeline semaphores), compute, the storage formats it writes
    std::string rejected;
    auto usable = [&](VkPhysicalDevice p, std::string& why) {
        const DeviceInfo d = InfoOf(p);
        if (VK_API_VERSION_MAJOR(d.apiVersion) == 1 && VK_API_VERSION_MINOR(d.apiVersion) < 2) { why = d.name + ": Vulkan " + std::to_string(VK_API_VERSION_MINOR(d.apiVersion)) + " < 1.2"; return false; }
        VkPhysicalDeviceVulkan12Features f12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        VkPhysicalDeviceFeatures2 f2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        f2.pNext = &f12;
        vkGetPhysicalDeviceFeatures2(p, &f2);
        if (!f12.timelineSemaphore) { why = d.name + ": no timeline semaphores"; return false; }
        if (!f2.features.shaderStorageImageExtendedFormats) { why = d.name + ": no extended storage image formats"; return false; }
        auto fmt = [&](VkFormat f, VkFormatFeatureFlags need) { VkFormatProperties fp{}; vkGetPhysicalDeviceFormatProperties(p, f, &fp); return (fp.optimalTilingFeatures & need) == need; };
        const VkFormatFeatureFlags sampled = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if (!fmt(VK_FORMAT_R8_UNORM, sampled) || !fmt(VK_FORMAT_R16_UNORM, sampled | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
            || !fmt(VK_FORMAT_R8_UINT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) || !fmt(VK_FORMAT_R16G16B16A16_SFLOAT, sampled)) {
            why = d.name + ": a needed image format is missing"; return false;
        }
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(p, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> q(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(p, &qn, q.data());
        for (uint32_t i = 0; i < qn; ++i) if (q[i].queueFlags & VK_QUEUE_COMPUTE_BIT) return true;
        why = d.name + ": no compute queue";
        return false;
    };
    int pick = -1, bestScore = -1;
    const std::string want = Lower(req.device);
    char* endp = nullptr;
    const long index = want.empty() ? -1 : std::strtol(want.c_str(), &endp, 10);
    const bool byIndex = !want.empty() && endp && *endp == 0;
    for (size_t i = 0; i < devs.size(); ++i) {
        const DeviceInfo d = InfoOf(devs[i]);
        std::string why;
        const bool named = !want.empty() && want != "auto" && (byIndex ? long(i) == index : Lower(d.name).find(want) != std::string::npos);
        if (!want.empty() && want != "auto" && !named) continue;
        if (!usable(devs[i], why)) { rejected += (rejected.empty() ? "" : "; ") + why; continue; }
        if (d.cpu && !named && !req.allowCpu) { rejected += (rejected.empty() ? "" : "; ") + d.name + ": software rasteriser (the CPU engine is faster)"; continue; }
        const int s = Score(d);
        if (s > bestScore) { bestScore = s; pick = int(i); }
    }
    if (pick < 0) {
        err = want.empty() || want == "auto" ? "no usable Vulkan device" : "no usable Vulkan device matches '" + req.device + "'";
        if (!rejected.empty()) err += " (" + rejected + ")";
        return false;
    }
    m_phys = devs[size_t(pick)];
    m_info = InfoOf(m_phys);
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(m_phys, &props);
    m_limits = props.limits;
    vkGetPhysicalDeviceMemoryProperties(m_phys, &m_mem);

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> q(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(m_phys, &qn, q.data());
    // a universal queue (graphics + compute) where there is one: it runs the transfers too, and every driver has it
    int fam = -1;
    for (uint32_t i = 0; i < qn && fam < 0; ++i) if ((q[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && (q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) fam = int(i);
    for (uint32_t i = 0; i < qn && fam < 0; ++i) if (q[i].queueFlags & VK_QUEUE_COMPUTE_BIT) fam = int(i);
    m_family = uint32_t(fam);

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qi.queueFamilyIndex = m_family; qi.queueCount = 1; qi.pQueuePriorities = &prio;
    VkPhysicalDeviceVulkan12Features f12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    f12.timelineSemaphore = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    f2.features.shaderStorageImageExtendedFormats = VK_TRUE;
    f2.pNext = &f12;
    std::vector<const char*> exts;
    if (HasDeviceExt(m_phys, "VK_KHR_portability_subset")) exts.push_back("VK_KHR_portability_subset");
    // NVIDIA's optical-flow engine through VK_NV_optical_flow (it needs synchronization2 and format_feature_flags2),
    // on its own queue family; PUSHMPC_VK_NO_OPTICAL_FLOW=1 leaves it out
    VkPhysicalDeviceOpticalFlowFeaturesNV fOf{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV };
    VkPhysicalDeviceSynchronization2FeaturesKHR fSync2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR };
    int ofFam = -1;
    const char* noOf = std::getenv("PUSHMPC_VK_NO_OPTICAL_FLOW");
    if (!(noOf && noOf[0] == '1') && HasDeviceExt(m_phys, VK_NV_OPTICAL_FLOW_EXTENSION_NAME) && HasDeviceExt(m_phys, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)
        && HasDeviceExt(m_phys, VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME)) {
        VkPhysicalDeviceOpticalFlowFeaturesNV qOf{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV };
        VkPhysicalDeviceFeatures2 q2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        q2.pNext = &qOf;
        vkGetPhysicalDeviceFeatures2(m_phys, &q2);
        for (uint32_t i = 0; i < qn && ofFam < 0; ++i) if (q[i].queueFlags & VK_QUEUE_OPTICAL_FLOW_BIT_NV) ofFam = int(i);
        if (qOf.opticalFlow && ofFam >= 0) {
            exts.push_back(VK_NV_OPTICAL_FLOW_EXTENSION_NAME);
            exts.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
            exts.push_back(VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME);
            fOf.opticalFlow = VK_TRUE; fSync2.synchronization2 = VK_TRUE;
            fOf.pNext = &fSync2; f12.pNext = &fOf;
        } else ofFam = -1;
    }
#if !defined(_WIN32) && !defined(__APPLE__)
    if (HasDeviceExt(m_phys, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) && HasDeviceExt(m_phys, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME)) {
        exts.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
        exts.push_back(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
        m_dmabuf = true;
    }
#endif
#if defined(__ANDROID__)
    // decoder frames from AImageReader: VK_ANDROID_external_memory_android_hardware_buffer with the YCbCr conversion
    VkPhysicalDeviceSamplerYcbcrConversionFeatures fY{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES };
    if (HasDeviceExt(m_phys, VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME) && HasDeviceExt(m_phys, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME)) {
        VkPhysicalDeviceSamplerYcbcrConversionFeatures qY{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES };
        VkPhysicalDeviceFeatures2 q2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        q2.pNext = &qY;
        vkGetPhysicalDeviceFeatures2(m_phys, &q2);
        if (qY.samplerYcbcrConversion) {
            exts.push_back(VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
            exts.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
            fY.samplerYcbcrConversion = VK_TRUE;
            fY.pNext = f12.pNext; f12.pNext = &fY;
            m_ahb = true;
        }
    }
#endif
    bool hostImport = false;
    if (HasDeviceExt(m_phys, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) { exts.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME); hostImport = true; }
    VkDeviceQueueCreateInfo qis[2] = { qi, qi };
    uint32_t qCount = 1;
    if (ofFam >= 0 && uint32_t(ofFam) != m_family) { qis[1].queueFamilyIndex = uint32_t(ofFam); qCount = 2; }
    VkDeviceCreateInfo di{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    di.pNext = &f2;
    di.queueCreateInfoCount = qCount; di.pQueueCreateInfos = qis;
    di.enabledExtensionCount = uint32_t(exts.size()); di.ppEnabledExtensionNames = exts.data();
    const VkResult r = vkCreateDevice(m_phys, &di, nullptr, &m_dev);
    if (r != VK_SUCCESS) { m_dmabuf = false; m_ahb = false; err = "vkCreateDevice failed (" + std::to_string(int(r)) + ") on " + m_info.name; m_dev = VK_NULL_HANDLE; return false; }
    volkLoadDevice(m_dev);
    if (hostImport && vkGetMemoryHostPointerPropertiesEXT) {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT };
        VkPhysicalDeviceProperties2 p2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        p2.pNext = &hp;
        vkGetPhysicalDeviceProperties2(m_phys, &p2);
        m_hostAlign = hp.minImportedHostPointerAlignment ? hp.minImportedHostPointerAlignment : 4096;
    }
    vkGetDeviceQueue(m_dev, m_family, 0, &m_queue);
    if (ofFam >= 0 && vkCreateOpticalFlowSessionNV) { m_ofFamily = uint32_t(ofFam); vkGetDeviceQueue(m_dev, m_ofFamily, 0, &m_ofQueue); }
    return true;
}

Context::~Context()
{
    if (m_external) return;   // the owner's device: never destroyed here (the engine waited for its own work)
    if (m_dev) { vkDeviceWaitIdle(m_dev); vkDestroyDevice(m_dev, nullptr); }
    if (m_messenger && vkDestroyDebugUtilsMessengerEXT) vkDestroyDebugUtilsMessengerEXT(m_inst, m_messenger, nullptr);
    // the instance stays for the process (Context::List and the next device use it)
}

uint64_t Context::ValidationErrors() const { return g_validationErrors.load(); }

VkResult Context::Submit(const VkSubmitInfo& si, VkFence fence)
{
    std::lock_guard<std::mutex> lk(m_queueLock);
    if (m_external && m_ext.queueLock) m_ext.queueLock(m_ext.lockCtx, m_family, m_ext.queueIndex);
    const VkResult r = vkQueueSubmit(m_queue, 1, &si, fence);
    if (m_external && m_ext.queueUnlock) m_ext.queueUnlock(m_ext.lockCtx, m_family, m_ext.queueIndex);
    return r;
}
VkResult Context::SubmitOf(const VkSubmitInfo& si)
{
    std::lock_guard<std::mutex> lk(m_ofQueue == m_queue ? m_queueLock : m_ofLock);
    return vkQueueSubmit(m_ofQueue, 1, &si, VK_NULL_HANDLE);
}
VkResult Context::WaitIdle()
{
    std::lock_guard<std::mutex> lk(m_queueLock);
    if (m_external && m_ext.queueLock) m_ext.queueLock(m_ext.lockCtx, m_family, m_ext.queueIndex);
    const VkResult r = vkQueueWaitIdle(m_queue);
    if (m_external && m_ext.queueUnlock) m_ext.queueUnlock(m_ext.lockCtx, m_family, m_ext.queueIndex);
    return r;
}

int Context::MemoryType(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) const
{
    for (uint32_t i = 0; i < m_mem.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (m_mem.memoryTypes[i].propertyFlags & want) == want && !(m_mem.memoryTypes[i].propertyFlags & avoid)) return int(i);
    return -1;
}

bool Context::FormatOk(VkFormat f, VkFormatFeatureFlags need) const
{
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(m_phys, f, &fp);
    return (fp.optimalTilingFeatures & need) == need;
}

bool Context::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host, Buffer& out, std::string& err)
{
    out = {};
    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = std::max<VkDeviceSize>(size, 16); bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_dev, &bi, nullptr, &out.buf) != VK_SUCCESS) { err = "vkCreateBuffer failed"; return false; }
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(m_dev, out.buf, &mr);
    int type = -1;
    if (host) {
        // the CPU reads the readback buffers: cached memory when there is some, else plain host-visible
        type = MemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (type < 0) type = MemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    } else {
        type = MemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type < 0) type = MemoryType(mr.memoryTypeBits, 0);
    }
    if (type < 0) { err = "no memory type for a buffer"; Destroy(out); return false; }
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = mr.size; ai.memoryTypeIndex = uint32_t(type);
    if (vkAllocateMemory(m_dev, &ai, nullptr, &out.mem) != VK_SUCCESS) { err = "out of GPU memory (" + std::to_string(mr.size >> 20) + " MB buffer)"; Destroy(out); return false; }
    vkBindBufferMemory(m_dev, out.buf, out.mem, 0);
    out.size = size;
    if (host && vkMapMemory(m_dev, out.mem, 0, VK_WHOLE_SIZE, 0, &out.map) != VK_SUCCESS) { err = "vkMapMemory failed"; Destroy(out); return false; }
    return true;
}

bool Context::CreateImage(int w, int h, VkFormat format, VkImageUsageFlags usage, bool rrrView, Image& out, std::string& err, uint32_t ofUsage)
{
    out = {};
    VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = format; ii.extent = { uint32_t(w), uint32_t(h), 1 }; ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.usage = usage; ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkOpticalFlowImageFormatInfoNV of{ VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_INFO_NV };
    const uint32_t fams[2] = { m_family, m_ofFamily };
    if (ofUsage) {
        of.usage = VkOpticalFlowUsageFlagsNV(ofUsage);
        ii.pNext = &of;
        if (m_ofFamily != m_family) { ii.sharingMode = VK_SHARING_MODE_CONCURRENT; ii.queueFamilyIndexCount = 2; ii.pQueueFamilyIndices = fams; }
    }
    if (vkCreateImage(m_dev, &ii, nullptr, &out.img) != VK_SUCCESS) { err = "vkCreateImage failed"; return false; }
    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(m_dev, out.img, &mr);
    int type = MemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) type = MemoryType(mr.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = mr.size; ai.memoryTypeIndex = uint32_t(type);
    if (type < 0 || vkAllocateMemory(m_dev, &ai, nullptr, &out.mem) != VK_SUCCESS) { err = "out of GPU memory (" + std::to_string(w) + "x" + std::to_string(h) + " image)"; Destroy(out); return false; }
    vkBindImageMemory(m_dev, out.img, out.mem, 0);
    VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = out.img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = format;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(m_dev, &vi, nullptr, &out.view) != VK_SUCCESS) { err = "vkCreateImageView failed"; Destroy(out); return false; }
    if (rrrView) {
        // the luma repeated in R, G and B (NIS reads colour); a view for sampling only
        VkImageViewUsageCreateInfo uv{ VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO };
        uv.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        vi.pNext = &uv;
        vi.components = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ONE };
        if (vkCreateImageView(m_dev, &vi, nullptr, &out.viewRRR) != VK_SUCCESS) { err = "vkCreateImageView (RRR) failed"; Destroy(out); return false; }
    }
    out.format = format; out.w = w; out.h = h;
    return true;
}

bool Context::ImportDmaBuf(int fd, VkDeviceSize size, VkBufferUsageFlags usage, Buffer& out, std::string& err)
{
    out = {};
#if !defined(_WIN32) && !defined(__APPLE__)
    if (!m_dmabuf || !vkGetMemoryFdPropertiesKHR) { err = "dma-buf import not supported by " + m_info.name; return false; }
    VkExternalMemoryBufferCreateInfo eb{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
    eb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.pNext = &eb; bi.size = size; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_dev, &bi, nullptr, &out.buf) != VK_SUCCESS) { err = "vkCreateBuffer (dma-buf) failed"; return false; }
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(m_dev, out.buf, &mr);
    VkMemoryFdPropertiesKHR fp{ VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
    if (vkGetMemoryFdPropertiesKHR(m_dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp) != VK_SUCCESS) {
        err = "vkGetMemoryFdPropertiesKHR failed"; Destroy(out); return false;
    }
    int type = MemoryType(mr.memoryTypeBits & fp.memoryTypeBits, 0);
    if (type < 0) { err = "no memory type can import this dma-buf"; Destroy(out); return false; }
    const int dupFd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (dupFd < 0) { err = "dup failed"; Destroy(out); return false; }
    VkImportMemoryFdInfoKHR imp{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR };
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT; imp.fd = dupFd;
    VkMemoryDedicatedAllocateInfo ded{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    ded.buffer = out.buf; imp.pNext = &ded;
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.pNext = &imp; ai.allocationSize = mr.size; ai.memoryTypeIndex = uint32_t(type);
    if (vkAllocateMemory(m_dev, &ai, nullptr, &out.mem) != VK_SUCCESS) {
        close(dupFd);   // not consumed on failure
        err = "vkAllocateMemory (dma-buf import, " + std::to_string(mr.size) + " bytes) failed"; Destroy(out); return false;
    }
    if (vkBindBufferMemory(m_dev, out.buf, out.mem, 0) != VK_SUCCESS) { err = "vkBindBufferMemory (dma-buf) failed"; Destroy(out); return false; }
    out.size = size;
    return true;
#else
    (void)fd; (void)size; (void)usage;
    err = "dma-buf import is Linux only";
    return false;
#endif
}

bool Context::ImportHost(void* ptr, VkDeviceSize size, VkBufferUsageFlags usage, Buffer& out, std::string& err)
{
    out = {};
    if (!m_hostAlign || !vkGetMemoryHostPointerPropertiesEXT) { err = "host memory import not supported by " + m_info.name; return false; }
    if ((reinterpret_cast<uintptr_t>(ptr) % m_hostAlign) || (size % m_hostAlign)) { err = "host range not aligned to " + std::to_string(m_hostAlign); return false; }
    VkExternalMemoryBufferCreateInfo eb{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
    eb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.pNext = &eb; bi.size = size; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(m_dev, &bi, nullptr, &out.buf) != VK_SUCCESS) { err = "vkCreateBuffer (host import) failed"; return false; }
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(m_dev, out.buf, &mr);
    VkMemoryHostPointerPropertiesEXT hp{ VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
    if (vkGetMemoryHostPointerPropertiesEXT(m_dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, ptr, &hp) != VK_SUCCESS) {
        err = "vkGetMemoryHostPointerPropertiesEXT failed"; Destroy(out); return false;
    }
    const int type = MemoryType(mr.memoryTypeBits & hp.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (type < 0 || mr.size > size) { err = "no memory type can import this host range"; Destroy(out); return false; }
    VkImportMemoryHostPointerInfoEXT imp{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT };
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT; imp.pHostPointer = ptr;
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.pNext = &imp; ai.allocationSize = size; ai.memoryTypeIndex = uint32_t(type);
    if (vkAllocateMemory(m_dev, &ai, nullptr, &out.mem) != VK_SUCCESS) { err = "vkAllocateMemory (host import) failed"; Destroy(out); return false; }
    if (vkBindBufferMemory(m_dev, out.buf, out.mem, 0) != VK_SUCCESS) { err = "vkBindBufferMemory (host import) failed"; Destroy(out); return false; }
    out.size = size;
    return true;
}

void Context::Destroy(Buffer& b)
{
    if (b.map) vkUnmapMemory(m_dev, b.mem);
    if (b.buf) vkDestroyBuffer(m_dev, b.buf, nullptr);
    if (b.mem) vkFreeMemory(m_dev, b.mem, nullptr);
    b = {};
}
void Context::Destroy(Image& i)
{
    if (i.viewRRR) vkDestroyImageView(m_dev, i.viewRRR, nullptr);
    if (i.view) vkDestroyImageView(m_dev, i.view, nullptr);
    if (i.img) vkDestroyImage(m_dev, i.img, nullptr);
    if (i.mem) vkFreeMemory(m_dev, i.mem, nullptr);
    i = {};
}

} // namespace pmvs::gpu
