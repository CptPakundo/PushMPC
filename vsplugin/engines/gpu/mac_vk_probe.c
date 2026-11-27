// SPDX-License-Identifier: GPL-3.0-or-later
// pushmpc-vulkan-probe <Vulkan library>: macOS only. Does in a process of its own what the portable GPU engine does
// first in the player's process: loads the library (MoltenVK, or the loader), creates an instance with the same
// extensions and flags as vk_context.cpp MakeInstance, opens each device with a compute queue and runs one empty
// submit to a fence. On some Macs Metal aborts inside MoltenVK's vkCreateInstance (the macOS Intel runner, a virtual
// GPU: abort in -[_MTLDevice newArgumentEncoderWithArguments:] from MVKPhysicalDevice::initMetalFeatures), which
// cannot be caught in-process; mac_vk_probe.h runs this first and keeps Vulkan out of the player when it dies.
// Prints one line per step and, as its very last act, "PMVKPROBE-OK" (also when a Vulkan call fails cleanly: the
// engine then gets the same error in-process and reports it). Exit 0.
#define VK_NO_PROTOTYPES 1
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#ifndef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
#define VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME "VK_KHR_portability_enumeration"
#define VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR 0x00000001
#endif

static int done(const char *what, VkResult r)
{
    printf("probe: %s failed (%d)\n", what, (int)r);
    printf("PMVKPROBE-OK\n");   /* a clean Vulkan error: the engine meets it in-process and reports it */
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv)
{
    void *lib;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkEnumerateInstanceExtensionProperties enumExt;
    PFN_vkCreateInstance createInstance;
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r;
    uint32_t n = 0, i, ndev = 0, d;
    const char *exts[2];
    uint32_t nexts = 0;
    int portEnum = 0, props2 = 0;
    VkExtensionProperties *ep;
    VkApplicationInfo app;
    VkInstanceCreateInfo ci;
    VkPhysicalDevice *devs;
    PFN_vkEnumeratePhysicalDevices enumDev;
    PFN_vkGetPhysicalDeviceProperties getProps;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties getQf;
    PFN_vkEnumerateDeviceExtensionProperties enumDevExt;
    PFN_vkCreateDevice createDevice;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkDestroyInstance destroyInstance;

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 2) { fprintf(stderr, "usage: pushmpc-vulkan-probe <Vulkan library>\n"); return 2; }
    lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { printf("probe: cannot load %s: %s\nPMVKPROBE-OK\n", argv[1], dlerror()); return 0; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (!gipa) { printf("probe: %s has no vkGetInstanceProcAddr\nPMVKPROBE-OK\n", argv[1]); return 0; }
    enumExt = (PFN_vkEnumerateInstanceExtensionProperties)gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    createInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    if (!enumExt || !createInstance) { printf("probe: no global commands\nPMVKPROBE-OK\n"); return 0; }
    /* the instance as MakeInstance makes it (no validation layer) */
    enumExt(NULL, &n, NULL);
    ep = (VkExtensionProperties *)calloc(n ? n : 1, sizeof(*ep));
    enumExt(NULL, &n, ep);
    for (i = 0; i < n; i++) {
        if (!strcmp(ep[i].extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) portEnum = 1;
        if (!strcmp(ep[i].extensionName, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) props2 = 1;
    }
    free(ep);
    if (portEnum) exts[nexts++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
    if (props2) exts[nexts++] = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    memset(&app, 0, sizeof(app));
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "PushMPC"; app.pEngineName = "PushMPC portable GPU engine"; app.apiVersion = VK_API_VERSION_1_2;
    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    if (portEnum) ci.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = nexts; ci.ppEnabledExtensionNames = exts;
    printf("probe: vkCreateInstance\n");
    r = createInstance(&ci, NULL, &inst);
    if (r != VK_SUCCESS) return done("vkCreateInstance", r);
    enumDev = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    getProps = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    getQf = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    enumDevExt = (PFN_vkEnumerateDeviceExtensionProperties)gipa(inst, "vkEnumerateDeviceExtensionProperties");
    createDevice = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
    gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
    destroyInstance = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
    if (!enumDev || !getProps || !getQf || !enumDevExt || !createDevice || !gdpa) return done("instance commands", VK_ERROR_INITIALIZATION_FAILED);
    r = enumDev(inst, &ndev, NULL);
    if (r != VK_SUCCESS || !ndev) return done("vkEnumeratePhysicalDevices", r);
    devs = (VkPhysicalDevice *)calloc(ndev, sizeof(*devs));
    enumDev(inst, &ndev, devs);
    for (d = 0; d < ndev; d++) {
        VkPhysicalDeviceProperties pr;
        VkQueueFamilyProperties *qf;
        uint32_t nq = 0, q, fam = UINT32_MAX, nde = 0;
        VkExtensionProperties *de;
        const char *dexts[1];
        uint32_t ndexts = 0;
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qi;
        VkDeviceCreateInfo dci;
        VkDevice dev = VK_NULL_HANDLE;
        getProps(devs[d], &pr);
        printf("probe: device %u: %s\n", d, pr.deviceName);
        getQf(devs[d], &nq, NULL);
        qf = (VkQueueFamilyProperties *)calloc(nq ? nq : 1, sizeof(*qf));
        getQf(devs[d], &nq, qf);
        for (q = 0; q < nq && fam == UINT32_MAX; q++)
            if (qf[q].queueFlags & VK_QUEUE_COMPUTE_BIT) fam = q;
        free(qf);
        if (fam == UINT32_MAX) { printf("probe: device %u has no compute queue\n", d); continue; }
        enumDevExt(devs[d], NULL, &nde, NULL);
        de = (VkExtensionProperties *)calloc(nde ? nde : 1, sizeof(*de));
        enumDevExt(devs[d], NULL, &nde, de);
        for (q = 0; q < nde; q++)
            if (!strcmp(de[q].extensionName, "VK_KHR_portability_subset")) dexts[ndexts++] = "VK_KHR_portability_subset";
        free(de);
        memset(&qi, 0, sizeof(qi));
        qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO; qi.queueFamilyIndex = fam; qi.queueCount = 1; qi.pQueuePriorities = &prio;
        memset(&dci, 0, sizeof(dci));
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qi;
        dci.enabledExtensionCount = ndexts; dci.ppEnabledExtensionNames = dexts;
        r = createDevice(devs[d], &dci, NULL, &dev);
        if (r != VK_SUCCESS) { printf("probe: device %u: vkCreateDevice failed (%d)\n", d, (int)r); continue; }
        {   /* one empty submit to a fence, waited for at most 5 s */
            PFN_vkGetDeviceQueue getQueue = (PFN_vkGetDeviceQueue)gdpa(dev, "vkGetDeviceQueue");
            PFN_vkCreateCommandPool createPool = (PFN_vkCreateCommandPool)gdpa(dev, "vkCreateCommandPool");
            PFN_vkAllocateCommandBuffers allocCb = (PFN_vkAllocateCommandBuffers)gdpa(dev, "vkAllocateCommandBuffers");
            PFN_vkBeginCommandBuffer beginCb = (PFN_vkBeginCommandBuffer)gdpa(dev, "vkBeginCommandBuffer");
            PFN_vkEndCommandBuffer endCb = (PFN_vkEndCommandBuffer)gdpa(dev, "vkEndCommandBuffer");
            PFN_vkCreateFence createFence = (PFN_vkCreateFence)gdpa(dev, "vkCreateFence");
            PFN_vkQueueSubmit submit = (PFN_vkQueueSubmit)gdpa(dev, "vkQueueSubmit");
            PFN_vkWaitForFences waitFences = (PFN_vkWaitForFences)gdpa(dev, "vkWaitForFences");
            PFN_vkDestroyDevice destroyDevice = (PFN_vkDestroyDevice)gdpa(dev, "vkDestroyDevice");
            VkQueue queue = VK_NULL_HANDLE;
            VkCommandPool pool = VK_NULL_HANDLE;
            VkCommandBuffer cb = VK_NULL_HANDLE;
            VkFence fence = VK_NULL_HANDLE;
            VkCommandPoolCreateInfo pci;
            VkCommandBufferAllocateInfo cai;
            VkCommandBufferBeginInfo bi;
            VkFenceCreateInfo fci;
            VkSubmitInfo si;
            if (!getQueue || !createPool || !allocCb || !beginCb || !endCb || !createFence || !submit || !waitFences) {
                printf("probe: device %u: missing device commands\n", d);
            } else {
                getQueue(dev, fam, 0, &queue);
                memset(&pci, 0, sizeof(pci)); pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; pci.queueFamilyIndex = fam;
                memset(&cai, 0, sizeof(cai)); cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
                memset(&bi, 0, sizeof(bi)); bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                memset(&fci, 0, sizeof(fci)); fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                memset(&si, 0, sizeof(si)); si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
                if ((r = createPool(dev, &pci, NULL, &pool)) == VK_SUCCESS && (cai.commandPool = pool, (r = allocCb(dev, &cai, &cb)) == VK_SUCCESS) &&
                    (r = beginCb(cb, &bi)) == VK_SUCCESS && (r = endCb(cb)) == VK_SUCCESS && (r = createFence(dev, &fci, NULL, &fence)) == VK_SUCCESS &&
                    (r = submit(queue, 1, &si, fence)) == VK_SUCCESS && (r = waitFences(dev, 1, &fence, VK_TRUE, 5000000000ull)) == VK_SUCCESS)
                    printf("probe: device %u: submit ok\n", d);
                else
                    printf("probe: device %u: submit step failed (%d)\n", d, (int)r);
            }
            if (destroyDevice) destroyDevice(dev, NULL);   /* the pool and fence go with the process */
        }
    }
    free(devs);
    if (destroyInstance) destroyInstance(inst, NULL);
    printf("PMVKPROBE-OK\n");
    fflush(stdout);
    return 0;
}
