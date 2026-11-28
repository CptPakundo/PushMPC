// engine_test: the Vulkan zero-copy route (vulkan_backend.cpp) with frames on a Vulkan device this test makes, as FFmpeg's
// hwupload would hand them to vf_pushmpc: NV12 as two images (R8 luma, R8G8 chroma), one timeline semaphore per frame.
// A moving box at x4 with a timestamp discontinuity: every output comes out (Dedup's held-back lookahead included), in
// pushcore order, real frames exact, no pair across the gap. Included by engine_test.cpp (Pic, Box, BoxX, CHECK); runs
// only when asked (PUSHMPC_TEST_GPU=1, or PUSHMPC_TEST_VULKAN=1 with a CPU Vulkan device such as lavapipe).
#pragma once
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <mutex>
#ifndef _WIN32
#include <dlfcn.h>
#endif

namespace vkzc {

struct Vk {
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mem{};
    std::string name;
#define PFN(n) PFN_##n n = nullptr;
    PFN(vkDestroyInstance) PFN(vkEnumeratePhysicalDevices) PFN(vkGetPhysicalDeviceProperties) PFN(vkGetPhysicalDeviceQueueFamilyProperties)
    PFN(vkGetPhysicalDeviceMemoryProperties) PFN(vkGetPhysicalDeviceFeatures2) PFN(vkCreateDevice) PFN(vkGetDeviceProcAddr)
    PFN(vkDestroyDevice) PFN(vkGetDeviceQueue) PFN(vkCreateImage) PFN(vkDestroyImage) PFN(vkGetImageMemoryRequirements) PFN(vkAllocateMemory)
    PFN(vkFreeMemory) PFN(vkBindImageMemory) PFN(vkCreateBuffer) PFN(vkDestroyBuffer) PFN(vkGetBufferMemoryRequirements) PFN(vkBindBufferMemory)
    PFN(vkMapMemory) PFN(vkUnmapMemory) PFN(vkCreateSemaphore) PFN(vkDestroySemaphore) PFN(vkWaitSemaphores) PFN(vkCreateCommandPool)
    PFN(vkDestroyCommandPool) PFN(vkAllocateCommandBuffers) PFN(vkBeginCommandBuffer) PFN(vkEndCommandBuffer) PFN(vkResetCommandBuffer)
    PFN(vkCmdPipelineBarrier) PFN(vkCmdCopyBufferToImage) PFN(vkCmdCopyImageToBuffer) PFN(vkQueueSubmit) PFN(vkQueueWaitIdle) PFN(vkDeviceWaitIdle)
#undef PFN
};

inline std::mutex& QueueLock() { static std::mutex m; return m; }
inline void LockQ(void*, uint32_t, uint32_t) { QueueLock().lock(); }
inline void UnlockQ(void*, uint32_t, uint32_t) { QueueLock().unlock(); }

inline uint32_t MemType(const Vk& v, uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < v.mem.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (v.mem.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

// the device: Vulkan 1.2, a graphics + compute queue, every feature it has enabled (as FFmpeg enables what it can)
inline bool Open(Vk& v, bool allowCpu, std::string& why)
{
#ifdef _WIN32
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    if (lib) v.gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(lib, "vkGetInstanceProcAddr")));
#else
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (lib) v.gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
#endif
    if (!v.gipa) { why = "no Vulkan loader"; return false; }
    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(v.gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    VkApplicationInfo ai{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.pApplicationName = "engine_test"; ai.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &ai;
    if (!createInstance || createInstance(&ici, nullptr, &v.inst) != VK_SUCCESS) { why = "vkCreateInstance failed"; return false; }
#define LI(n) v.n = reinterpret_cast<PFN_##n>(v.gipa(v.inst, #n));
    LI(vkDestroyInstance) LI(vkEnumeratePhysicalDevices) LI(vkGetPhysicalDeviceProperties) LI(vkGetPhysicalDeviceQueueFamilyProperties)
    LI(vkGetPhysicalDeviceMemoryProperties) LI(vkGetPhysicalDeviceFeatures2) LI(vkCreateDevice) LI(vkGetDeviceProcAddr)
#undef LI
    uint32_t n = 0;
    v.vkEnumeratePhysicalDevices(v.inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    if (n) v.vkEnumeratePhysicalDevices(v.inst, &n, devs.data());
    int best = -1, bestScore = -1;
    for (uint32_t i = 0; i < n; ++i) {
        VkPhysicalDeviceProperties p{};
        v.vkGetPhysicalDeviceProperties(devs[i], &p);
        if (VK_API_VERSION_MINOR(p.apiVersion) < 2 && VK_API_VERSION_MAJOR(p.apiVersion) == 1) continue;
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU && !allowCpu) continue;
        const int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (score > bestScore) { bestScore = score; best = int(i); }
    }
    if (best < 0) { why = "no usable Vulkan device"; return false; }
    v.phys = devs[size_t(best)];
    { VkPhysicalDeviceProperties p{}; v.vkGetPhysicalDeviceProperties(v.phys, &p); v.name = p.deviceName; }
    v.vkGetPhysicalDeviceMemoryProperties(v.phys, &v.mem);
    uint32_t qn = 0;
    v.vkGetPhysicalDeviceQueueFamilyProperties(v.phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> q(qn);
    v.vkGetPhysicalDeviceQueueFamilyProperties(v.phys, &qn, q.data());
    int fam = -1;
    for (uint32_t i = 0; i < qn && fam < 0; ++i) if ((q[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && (q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) fam = int(i);
    if (fam < 0) { why = v.name + ": no graphics + compute queue"; return false; }
    v.family = uint32_t(fam);
    VkPhysicalDeviceVulkan12Features f12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceVulkan11Features f11{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
    f11.pNext = &f12;
    VkPhysicalDeviceFeatures2 f2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    f2.pNext = &f11;
    v.vkGetPhysicalDeviceFeatures2(v.phys, &f2);
    if (!f12.timelineSemaphore) { why = v.name + ": no timeline semaphores"; return false; }
    const float prio = 1.f;
    VkDeviceQueueCreateInfo qi{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qi.queueFamilyIndex = v.family; qi.queueCount = 1; qi.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = &f2; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qi;
    if (v.vkCreateDevice(v.phys, &dci, nullptr, &v.dev) != VK_SUCCESS) { why = v.name + ": vkCreateDevice failed"; return false; }
#define LD(n) v.n = reinterpret_cast<PFN_##n>(v.vkGetDeviceProcAddr(v.dev, #n));
    LD(vkDestroyDevice) LD(vkGetDeviceQueue) LD(vkCreateImage) LD(vkDestroyImage) LD(vkGetImageMemoryRequirements) LD(vkAllocateMemory)
    LD(vkFreeMemory) LD(vkBindImageMemory) LD(vkCreateBuffer) LD(vkDestroyBuffer) LD(vkGetBufferMemoryRequirements) LD(vkBindBufferMemory)
    LD(vkMapMemory) LD(vkUnmapMemory) LD(vkCreateSemaphore) LD(vkDestroySemaphore) LD(vkWaitSemaphores) LD(vkCreateCommandPool)
    LD(vkDestroyCommandPool) LD(vkAllocateCommandBuffers) LD(vkBeginCommandBuffer) LD(vkEndCommandBuffer) LD(vkResetCommandBuffer)
    LD(vkCmdPipelineBarrier) LD(vkCmdCopyBufferToImage) LD(vkCmdCopyImageToBuffer) LD(vkQueueSubmit) LD(vkQueueWaitIdle) LD(vkDeviceWaitIdle)
#undef LD
    v.vkGetDeviceQueue(v.dev, v.family, 0, &v.queue);
    VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = v.family;
    if (v.vkCreateCommandPool(v.dev, &pci, nullptr, &v.pool) != VK_SUCCESS) { why = "vkCreateCommandPool failed"; return false; }
    VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = v.pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    if (v.vkAllocateCommandBuffers(v.dev, &cai, &v.cb) != VK_SUCCESS) { why = "vkAllocateCommandBuffers failed"; return false; }
    return true;
}

// one NV12 frame: two images (R8 w x h, R8G8 w/2 x h/2), device memory, a timeline semaphore
struct Frame {
    VkImage img[2] = {};
    VkDeviceMemory mem[2] = {};
    VkSemaphore sem = VK_NULL_HANDLE;
    uint64_t value = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};
inline bool MakeFrame(Vk& v, int w, int h, Frame& f)
{
    for (int p = 0; p < 2; ++p) {
        VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ii.imageType = VK_IMAGE_TYPE_2D; ii.format = p ? VK_FORMAT_R8G8_UNORM : VK_FORMAT_R8_UNORM;
        ii.extent = { uint32_t(p ? w / 2 : w), uint32_t(p ? h / 2 : h), 1 }; ii.mipLevels = 1; ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (v.vkCreateImage(v.dev, &ii, nullptr, &f.img[p]) != VK_SUCCESS) return false;
        VkMemoryRequirements mr{};
        v.vkGetImageMemoryRequirements(v.dev, f.img[p], &mr);
        VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = mr.size; mai.memoryTypeIndex = MemType(v, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mai.memoryTypeIndex == UINT32_MAX) mai.memoryTypeIndex = MemType(v, mr.memoryTypeBits, 0);
        if (v.vkAllocateMemory(v.dev, &mai, nullptr, &f.mem[p]) != VK_SUCCESS || v.vkBindImageMemory(v.dev, f.img[p], f.mem[p], 0) != VK_SUCCESS) return false;
    }
    VkSemaphoreTypeCreateInfo st{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    st.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE; st.initialValue = 0;
    VkSemaphoreCreateInfo sci{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    sci.pNext = &st;
    return v.vkCreateSemaphore(v.dev, &sci, nullptr, &f.sem) == VK_SUCCESS;
}
inline void FreeFrame(Vk& v, Frame& f)
{
    for (int p = 0; p < 2; ++p) { if (f.img[p]) v.vkDestroyImage(v.dev, f.img[p], nullptr); if (f.mem[p]) v.vkFreeMemory(v.dev, f.mem[p], nullptr); }
    if (f.sem) v.vkDestroySemaphore(v.dev, f.sem, nullptr);
    f = Frame{};
}

// a host-visible staging buffer of NV12's size (tight rows)
struct Staging { VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; uint8_t* map = nullptr; size_t size = 0; };
inline bool MakeStaging(Vk& v, size_t size, Staging& s)
{
    VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size; bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (v.vkCreateBuffer(v.dev, &bi, nullptr, &s.buf) != VK_SUCCESS) return false;
    VkMemoryRequirements mr{};
    v.vkGetBufferMemoryRequirements(v.dev, s.buf, &mr);
    VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size; mai.memoryTypeIndex = MemType(v, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX || v.vkAllocateMemory(v.dev, &mai, nullptr, &s.mem) != VK_SUCCESS) return false;
    if (v.vkBindBufferMemory(v.dev, s.buf, s.mem, 0) != VK_SUCCESS) return false;
    void* p = nullptr;
    if (v.vkMapMemory(v.dev, s.mem, 0, size, 0, &p) != VK_SUCCESS) return false;
    s.map = static_cast<uint8_t*>(p); s.size = size;
    return true;
}

// buffer <-> frame on our queue (under the queue lock the engine shares), waited for. up: upload, and the frame's
// semaphore set to value + 1; down: read back (the frame is in GENERAL, its last writer already finished)
inline bool Transfer(Vk& v, Frame& f, Staging& s, int w, int h, bool up)
{
    std::lock_guard<std::mutex> lk(QueueLock());
    v.vkResetCommandBuffer(v.cb, 0);
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    v.vkBeginCommandBuffer(v.cb, &bi);
    VkImageMemoryBarrier b[2] = {};
    for (int p = 0; p < 2; ++p) {
        b[p].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b[p].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; b[p].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        b[p].oldLayout = up ? VK_IMAGE_LAYOUT_UNDEFINED : f.layout; b[p].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b[p].srcQueueFamilyIndex = b[p].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b[p].image = f.img[p]; b[p].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    }
    v.vkCmdPipelineBarrier(v.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
    VkBufferImageCopy r[2] = {};
    r[0].bufferOffset = 0; r[0].imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }; r[0].imageExtent = { uint32_t(w), uint32_t(h), 1 };
    r[1].bufferOffset = VkDeviceSize(w) * h; r[1].imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }; r[1].imageExtent = { uint32_t(w / 2), uint32_t(h / 2), 1 };
    for (int p = 0; p < 2; ++p) {
        if (up) v.vkCmdCopyBufferToImage(v.cb, s.buf, f.img[p], VK_IMAGE_LAYOUT_GENERAL, 1, &r[p]);
        else v.vkCmdCopyImageToBuffer(v.cb, f.img[p], VK_IMAGE_LAYOUT_GENERAL, s.buf, 1, &r[p]);
    }
    VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_HOST_READ_BIT;
    v.vkCmdPipelineBarrier(v.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    v.vkEndCommandBuffer(v.cb);
    VkTimelineSemaphoreSubmitInfo ts{ VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    const uint64_t sig = f.value + 1;
    ts.signalSemaphoreValueCount = 1; ts.pSignalSemaphoreValues = &sig;
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &v.cb;
    if (up) { si.pNext = &ts; si.signalSemaphoreCount = 1; si.pSignalSemaphores = &f.sem; }
    if (v.vkQueueSubmit(v.queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) return false;
    if (v.vkQueueWaitIdle(v.queue) != VK_SUCCESS) return false;
    if (up) f.value = sig;
    f.layout = VK_IMAGE_LAYOUT_GENERAL;
    return true;
}

inline pmeng_image Image(const Frame& f, int w, int h)
{
    pmeng_image im{};
    im.mem = PMENG_MEM_VULKAN; im.fmt = PMENG_FMT_NV12; im.width = uint32_t(w); im.height = uint32_t(h);
    im.range = 1; im.matrix = 1; im.primaries = 1; im.transfer = 1;
    for (int p = 0; p < 2; ++p) {
        im.vk[p].image = reinterpret_cast<void*>(f.img[p]);
        im.vk[p].aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        im.vk[p].layout = int32_t(f.layout);
        im.vk[p].semaphore = reinterpret_cast<void*>(f.sem);
        im.vk[p].value = f.value;
    }
    im.vk_planes = 2;
    return im;
}

} // namespace vkzc

static void TestVulkanZeroCopy(const pmeng_api* api, bool allowCpu)
{
    using namespace vkzc;
    const int W = 320, H = 180, N = 48, cutAt = 30, discAt = 40;
    Vk v;
    std::string why;
    if (!Open(v, allowCpu, why)) { std::printf("vulkan zero copy: not run: %s\n", why.c_str()); if (v.dev) v.vkDestroyDevice(v.dev, nullptr); if (v.inst) v.vkDestroyInstance(v.inst, nullptr); return; }
    std::printf("vulkan zero copy: test device %s\n", v.name.c_str());
    Staging stg;
    CHECK(MakeStaging(v, size_t(W) * H * 3 / 2, stg), "vulkan zero copy: staging buffer");
    pmeng_device dev{}; dev.struct_size = sizeof(dev); dev.mem = PMENG_MEM_VULKAN;
    dev.vk_instance = v.inst; dev.vk_physical_device = v.phys; dev.vk_device = v.dev;
    dev.vk_get_instance_proc_addr = reinterpret_cast<void*>(v.gipa);
    dev.vk_queue_family = v.family; dev.vk_queue_index = 0;
    dev.vk_lock_queue = LockQ; dev.vk_unlock_queue = UnlockQ;
    pmeng_stream st{}; st.struct_size = sizeof(st); st.width = W; st.height = H; st.fmt = PMENG_FMT_NV12; st.fps_num = 24000; st.fps_den = 1001;
    pmeng_output out{}; out.struct_size = sizeof(out);
    char err[512] = {};
    void* h = api->open(&dev, &st, "smoother=4\n", &out, err, sizeof(err));
    CHECK(h && std::strcmp(out.engine, "portable-vulkan") == 0, "vulkan zero copy: open: %s %s", h ? out.engine : "", err);
    std::vector<Pic> srcs;
    std::vector<Frame> frames;
    Frame dst;
    CHECK(MakeFrame(v, W, H, dst), "vulkan zero copy: output frame");
    int outputs = 0, orderErr = 0, realErr = 0, cutBlend = 0, motionErr = 0;
    uint64_t lastSrc = 0; uint32_t lastSub = 0; bool first = true;
    if (h) {
        std::printf("vulkan zero copy: %s | %s\n", out.engine, out.description);
        auto drain = [&]() {
            pmeng_out_info oi{};
            while (api->peek(h, &oi, 0) == 1) {
                const pmeng_image di = Image(dst, W, H);
                if (api->write(h, &di) != PMENG_OK) { CHECK(false, "vulkan zero copy: write: %s", api->last_error(h)); return; }
                // the engine signalled value + 1 and left the planes in GENERAL
                dst.value += 1; dst.layout = VK_IMAGE_LAYOUT_GENERAL;
                VkSemaphoreWaitInfo wi{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
                wi.semaphoreCount = 1; wi.pSemaphores = &dst.sem; wi.pValues = &dst.value;
                CHECK(v.vkWaitSemaphores(v.dev, &wi, 5000000000ull) == VK_SUCCESS, "vulkan zero copy: output %d not signalled", outputs);
                CHECK(Transfer(v, dst, stg, W, H, false), "vulkan zero copy: read back");
                Pic got(W, H, PMENG_FMT_NV12);
                std::memcpy(got.p[0].data(), stg.map, size_t(W) * H);
                std::memcpy(got.p[1].data(), stg.map + size_t(W) * H, size_t(W) * H / 2);
                ++outputs;
                const bool inOrder = first || oi.source > lastSrc || (oi.source == lastSrc && lastSub != 0 && (oi.subframe == 0 || oi.subframe > lastSub));
                if (!inOrder || oi.source >= srcs.size()) ++orderErr;
                first = false; lastSrc = oi.source; lastSub = oi.subframe;
                if (oi.source >= srcs.size()) continue;
                const int s = int(oi.source);
                if (oi.subframe == 0) {
                    if (srcs[s].p[0] != got.p[0] && realErr++ < 6) std::printf("vulkan zero copy: real frame %d differs from its source\n", s);
                } else if (s == cutAt) {
                    const int bgv = got.Y(2, 2);
                    if (bgv < 37 || (bgv > 43 && bgv < 227) || bgv > 233) ++cutBlend;
                } else if (s < cutAt) {
                    const int want = 4 * (s - 1) + int(oi.subframe), bx = BoxX(got);
                    if (bx < 0 || std::abs(bx - want) > 2) { if (motionErr++ < 6) std::printf("vulkan zero copy: pair %d phase %u: box at %d, expected %d\n", s, oi.subframe, bx, want); }
                }
            }
        };
        for (int s = 0; s < N; ++s) {
            srcs.emplace_back(W, H, PMENG_FMT_NV12);
            Pic& src = srcs.back();
            if (s >= cutAt && s < discAt) Box(src, 4 * s, 230); else Box(src, 4 * s);
            frames.emplace_back();
            Frame& f = frames.back();
            CHECK(MakeFrame(v, W, H, f), "vulkan zero copy: frame %d", s);
            std::memcpy(stg.map, src.p[0].data(), size_t(W) * H);
            std::memcpy(stg.map + size_t(W) * H, src.p[1].data(), size_t(W) * H / 2);
            CHECK(Transfer(v, f, stg, W, H, true), "vulkan zero copy: upload %d", s);
            const pmeng_image im = Image(f, W, H);
            const int r = api->submit(h, &im, int64_t(s) * 41708, s == discAt ? PMENG_SUBMIT_DISCONTINUITY : 0u);
            CHECK(r == PMENG_OK, "vulkan zero copy: submit %d -> %d %s", s, r, api->last_error(h));
            f.value += 1;   // the engine signalled value + 1 when its import was done
            drain();
        }
        if (api->struct_size >= offsetof(pmeng_api, finish) + sizeof(api->finish) && api->finish) CHECK(api->finish(h) == PMENG_OK, "vulkan zero copy: finish");
        drain();
        CHECK(api->pending(h) == 0, "vulkan zero copy: pending %u", api->pending(h));
        const int expect = 1 + (N - 1) * 4 - 3;   // minus the discontinuity's group
        CHECK(outputs == expect, "vulkan zero copy: %d outputs, expected %d (Dedup's held-back outputs at the discontinuity)", outputs, expect);
        CHECK(orderErr == 0, "vulkan zero copy: %d outputs out of order", orderErr);
        CHECK(realErr == 0, "vulkan zero copy: %d real frames differ from their source", realErr);
        CHECK(cutBlend == 0, "vulkan zero copy: %d generated frames blended across the cut", cutBlend);
        CHECK(motionErr == 0, "vulkan zero copy: %d generated frames with the box in the wrong place", motionErr);
        char sbuf[512]; api->stats(h, sbuf, sizeof(sbuf));
        std::printf("vulkan zero copy: %d outputs; %s\n", outputs, sbuf);
        api->close(h);
    }
    v.vkDeviceWaitIdle(v.dev);
    FreeFrame(v, dst);
    for (Frame& f : frames) FreeFrame(v, f);
    if (stg.map) v.vkUnmapMemory(v.dev, stg.mem);
    if (stg.buf) v.vkDestroyBuffer(v.dev, stg.buf, nullptr);
    if (stg.mem) v.vkFreeMemory(v.dev, stg.mem, nullptr);
    v.vkDestroyCommandPool(v.dev, v.pool, nullptr);
    v.vkDestroyDevice(v.dev, nullptr);
    v.vkDestroyInstance(v.inst, nullptr);
}
