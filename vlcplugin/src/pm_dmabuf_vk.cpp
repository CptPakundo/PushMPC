// PushMPC for VLC, zero copy on Linux (see pm_dmabuf_vk.h). GPL-3.0-or-later.
#include "pm_dmabuf_vk.h"
#include <cstring>
#include <unistd.h>
#include "pm_dmabuf_spv.h"   // kSpv_pm_dmabuf_in, kSpv_pm_dmabuf_out8, kSpv_pm_dmabuf_out16 (vk/*.comp, built by CMake)

namespace pmdb {

namespace {
constexpr uint32_t Fourcc(char a, char b, char c, char d) { return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24; }
VkFormat LayerFormat(uint32_t drm)
{
    if (drm == Fourcc('R', '8', ' ', ' ')) return VK_FORMAT_R8_UNORM;
    if (drm == Fourcc('G', 'R', '8', '8')) return VK_FORMAT_R8G8_UNORM;
    if (drm == Fourcc('R', '1', '6', ' ')) return VK_FORMAT_R16_UNORM;
    if (drm == Fourcc('G', 'R', '3', '2')) return VK_FORMAT_R16G16_UNORM;   // DRM_FORMAT_GR1616
    return VK_FORMAT_UNDEFINED;
}
} // namespace

DmabufIO::~DmabufIO()
{
    if (!m_ctx) return;
    const VkDevice d = m_ctx->Dev();
    for (Pipe* p : { &m_in, &m_out8, &m_out16 }) {
        if (p->p) vkDestroyPipeline(d, p->p, nullptr);
        if (p->pl) vkDestroyPipelineLayout(d, p->pl, nullptr);
        if (p->dsl) vkDestroyDescriptorSetLayout(d, p->dsl, nullptr);
    }
    if (m_pool) vkDestroyDescriptorPool(d, m_pool, nullptr);
    if (m_sampler) vkDestroySampler(d, m_sampler, nullptr);
}

bool DmabufIO::MakePipe(const uint32_t* code, size_t bytes, const VkDescriptorType* types, int n, uint32_t push, Pipe& out, std::string& err)
{
    const VkDevice d = m_ctx->Dev();
    VkDescriptorSetLayoutBinding b[4]{};
    for (int i = 0; i < n; ++i) { b[i].binding = uint32_t(i); b[i].descriptorType = types[i]; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; }
    VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = uint32_t(n); li.pBindings = b;
    if (vkCreateDescriptorSetLayout(d, &li, nullptr, &out.dsl) != VK_SUCCESS) { err = "vkCreateDescriptorSetLayout failed"; return false; }
    VkPushConstantRange pr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, push };
    VkPipelineLayoutCreateInfo pi{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pi.setLayoutCount = 1; pi.pSetLayouts = &out.dsl; pi.pushConstantRangeCount = 1; pi.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(d, &pi, nullptr, &out.pl) != VK_SUCCESS) { err = "vkCreatePipelineLayout failed"; return false; }
    VkShaderModuleCreateInfo si{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    si.codeSize = bytes; si.pCode = code;
    VkShaderModule sm = VK_NULL_HANDLE;
    if (vkCreateShaderModule(d, &si, nullptr, &sm) != VK_SUCCESS) { err = "vkCreateShaderModule failed"; return false; }
    VkComputePipelineCreateInfo ci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    ci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", nullptr };
    ci.layout = out.pl;
    const VkResult r = vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, &out.p);
    vkDestroyShaderModule(d, sm, nullptr);
    if (r != VK_SUCCESS) { err = "vkCreateComputePipelines failed"; return false; }
    return true;
}

bool DmabufIO::Init(const std::shared_ptr<pmvs::gpu::Context>& ctx, std::string& why)
{
    m_ctx = ctx;
    if (!ctx) { why = "no Vulkan device"; return false; }
    if (!ctx->Dmabuf()) { why = "the Vulkan driver cannot import dmabufs (VK_EXT_external_memory_dma_buf / VK_EXT_image_drm_format_modifier)"; return false; }
    if (!vkGetMemoryFdPropertiesKHR) { why = "vkGetMemoryFdPropertiesKHR missing"; return false; }
    const VkDescriptorType tin[3] = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER };
    const VkDescriptorType tout[3] = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE };
    if (!MakePipe(kSpv_pm_dmabuf_in, sizeof(kSpv_pm_dmabuf_in), tin, 3, 32, m_in, why) ||
        !MakePipe(kSpv_pm_dmabuf_out8, sizeof(kSpv_pm_dmabuf_out8), tout, 3, 36, m_out8, why) ||
        !MakePipe(kSpv_pm_dmabuf_out16, sizeof(kSpv_pm_dmabuf_out16), tout, 3, 36, m_out16, why)) return false;
    VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST; sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(ctx->Dev(), &sci, nullptr, &m_sampler) != VK_SUCCESS) { why = "vkCreateSampler failed"; return false; }
    const VkDescriptorPoolSize ps[3] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64 } };
    VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = 32; pci.poolSizeCount = 3; pci.pPoolSizes = ps;
    if (vkCreateDescriptorPool(ctx->Dev(), &pci, nullptr, &m_pool) != VK_SUCCESS) { why = "vkCreateDescriptorPool failed"; return false; }
    return true;
}

void DmabufIO::Recycle() { if (m_pool) vkResetDescriptorPool(m_ctx->Dev(), m_pool, 0); }

VkDescriptorSet DmabufIO::Alloc(const Pipe& p, std::string& err)
{
    VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = m_pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &p.dsl;
    VkDescriptorSet s = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(m_ctx->Dev(), &ai, &s) != VK_SUCCESS) { err = "vkAllocateDescriptorSets failed"; return VK_NULL_HANDLE; }
    return s;
}

bool DmabufIO::Import(const pm_dmabuf& d, bool output, Imported& out, std::string& err)
{
    out = {};
    out.output = output; out.bits = d.bits; out.w = d.width; out.h = d.height; out.cw = (d.width + 1) / 2; out.ch = (d.height + 1) / 2;
    if (d.nlayers != 2) { err = "dmabuf: two layers expected (luma, chroma)"; return false; }
    const VkDevice dev = m_ctx->Dev();
    for (int l = 0; l < 2; ++l) {
        const auto& L = d.layer[l];
        const VkFormat f = LayerFormat(L.drm_format);
        if (f == VK_FORMAT_UNDEFINED) { err = "dmabuf: unknown layer format"; Release(out); return false; }
        if (L.object < 0 || L.object >= d.nobjects || d.fd[L.object] < 0) { err = "dmabuf: bad object index"; Release(out); return false; }
        VkSubresourceLayout pl{};
        pl.offset = L.offset; pl.rowPitch = L.pitch;
        VkImageDrmFormatModifierExplicitCreateInfoEXT mod{ VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT };
        mod.drmFormatModifier = d.modifier[L.object]; mod.drmFormatModifierPlaneCount = 1; mod.pPlaneLayouts = &pl;
        VkExternalMemoryImageCreateInfo ext{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
        ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT; ext.pNext = &mod;
        VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ii.pNext = &ext;
        ii.imageType = VK_IMAGE_TYPE_2D; ii.format = f;
        ii.extent = { uint32_t(l ? out.cw : out.w), uint32_t(l ? out.ch : out.h), 1 };
        ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        ii.usage = output ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(dev, &ii, nullptr, &out.img[l]) != VK_SUCCESS) { err = "dmabuf: the driver cannot make this image (format, modifier or usage)"; Release(out); return false; }
        VkMemoryRequirements mr{};
        vkGetImageMemoryRequirements(dev, out.img[l], &mr);
        const int fd = dup(d.fd[L.object]);
        if (fd < 0) { err = "dmabuf: dup failed"; Release(out); return false; }
        VkMemoryFdPropertiesKHR fp{ VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
        if (vkGetMemoryFdPropertiesKHR(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp) != VK_SUCCESS) { close(fd); err = "dmabuf: the driver does not take this fd"; Release(out); return false; }
        const uint32_t bits = mr.memoryTypeBits & fp.memoryTypeBits;
        int type = -1;
        for (int i = 0; i < 32 && type < 0; ++i) if (bits & (1u << i)) type = i;
        if (type < 0) { close(fd); err = "dmabuf: no memory type fits"; Release(out); return false; }
        const off_t total = lseek(fd, 0, SEEK_END);
        lseek(fd, 0, SEEK_SET);
        VkMemoryDedicatedAllocateInfo ded{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
        ded.image = out.img[l];
        VkImportMemoryFdInfoKHR imp{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR };
        imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT; imp.fd = fd; imp.pNext = &ded;
        VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.pNext = &imp;
        ai.allocationSize = total > 0 && VkDeviceSize(total) >= mr.size ? VkDeviceSize(total) : mr.size;
        ai.memoryTypeIndex = uint32_t(type);
        if (vkAllocateMemory(dev, &ai, nullptr, &out.mem[l]) != VK_SUCCESS) { close(fd); err = "dmabuf: import failed"; Release(out); return false; }
        // the fd now belongs to the driver
        if (vkBindImageMemory(dev, out.img[l], out.mem[l], 0) != VK_SUCCESS) { err = "dmabuf: bind failed"; Release(out); return false; }
        VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = out.img[l]; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = f;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(dev, &vi, nullptr, &out.view[l]) != VK_SUCCESS) { err = "dmabuf: view failed"; Release(out); return false; }
    }
    return true;
}

void DmabufIO::Release(Imported& im)
{
    const VkDevice dev = m_ctx->Dev();
    for (int l = 0; l < 2; ++l) {
        if (im.view[l]) vkDestroyImageView(dev, im.view[l], nullptr);
        if (im.img[l]) vkDestroyImage(dev, im.img[l], nullptr);
        if (im.mem[l]) vkFreeMemory(dev, im.mem[l], nullptr);
        im.view[l] = VK_NULL_HANDLE; im.img[l] = VK_NULL_HANDLE; im.mem[l] = VK_NULL_HANDLE;
    }
}

// the surfaces come from (and go back to) the decoder's side: a queue family ownership transfer from the foreign queue
void DmabufIO::Acquire(VkCommandBuffer cb, const Imported& im, bool output) const
{
    VkImageMemoryBarrier b[2]{};
    for (int l = 0; l < 2; ++l) {
        b[l].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b[l].srcAccessMask = 0;
        b[l].dstAccessMask = output ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
        b[l].oldLayout = output ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;   // outputs are overwritten whole
        b[l].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b[l].srcQueueFamilyIndex = m_ctx->QueueFamilyForeign() ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL;
        b[l].dstQueueFamilyIndex = m_ctx->Family();
        b[l].image = im.img[l];
        b[l].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    }
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
}

void DmabufIO::ReleaseToForeign(VkCommandBuffer cb, const Imported& im) const
{
    VkImageMemoryBarrier b[2]{};
    for (int l = 0; l < 2; ++l) {
        b[l].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b[l].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b[l].dstAccessMask = 0;
        b[l].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        b[l].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b[l].srcQueueFamilyIndex = m_ctx->Family();
        b[l].dstQueueFamilyIndex = m_ctx->QueueFamilyForeign() ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL;
        b[l].image = im.img[l];
        b[l].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    }
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
}

bool DmabufIO::RecordIn(VkCommandBuffer cb, const Imported& im, const pmvs::gpu::Engine::PlanarDesc& d, std::string& err)
{
    if (d.width > im.w || d.height > im.h) { err = "dmabuf: the surface is smaller than the picture"; return false; }
    Acquire(cb, im, false);
    const VkDescriptorSet s = Alloc(m_in, err);
    if (!s) return false;
    VkDescriptorImageInfo ii[2] = { { m_sampler, im.view[0], VK_IMAGE_LAYOUT_GENERAL }, { m_sampler, im.view[1], VK_IMAGE_LAYOUT_GENERAL } };
    VkDescriptorBufferInfo bi{ d.buffer, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w[3]{};
    for (int i = 0; i < 3; ++i) { w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = s; w[i].dstBinding = uint32_t(i); w[i].descriptorCount = 1; }
    w[0].descriptorType = w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &ii[0]; w[1].pImageInfo = &ii[1];
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[2].pBufferInfo = &bi;
    vkUpdateDescriptorSets(m_ctx->Dev(), 3, w, 0, nullptr);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m_in.p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m_in.pl, 0, 1, &s, 0, nullptr);
    for (int plane = 0; plane < 3; ++plane) {
        const int w2 = plane ? d.cw : d.width, h2 = plane ? d.ch : d.height;
        const VkDeviceSize off = plane == 0 ? 0 : plane == 1 ? d.offU : d.offV;
        const int64_t bytes = int64_t(w2) * h2 * d.bps;
        struct { int plane, w, h, bps; uint32_t wordOff; int shift; float scale; int nwords; } pc{
            plane, w2, h2, d.bps, uint32_t(off / 4), im.bits > 8 ? 6 : 0, im.bits > 8 ? 65535.f : 255.f, int((bytes + 3) / 4) };
        vkCmdPushConstants(cb, m_in.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cb, uint32_t((pc.nwords + 63) / 64), 1, 1);
    }
    return true;
}

bool DmabufIO::RecordOut(VkCommandBuffer cb, const Imported& im, const pmvs::gpu::Engine::PackedDesc& d, std::string& err)
{
    if (d.width > im.w || d.height > im.h) { err = "dmabuf: the surface is smaller than the picture"; return false; }
    Acquire(cb, im, true);
    const Pipe& p = im.bits > 8 ? m_out16 : m_out8;
    const VkDescriptorSet s = Alloc(p, err);
    if (!s) return false;
    VkDescriptorBufferInfo bi{ d.buffer, 0, VK_WHOLE_SIZE };
    VkDescriptorImageInfo ii[2] = { { VK_NULL_HANDLE, im.view[0], VK_IMAGE_LAYOUT_GENERAL }, { VK_NULL_HANDLE, im.view[1], VK_IMAGE_LAYOUT_GENERAL } };
    VkWriteDescriptorSet w[3]{};
    for (int i = 0; i < 3; ++i) { w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = s; w[i].dstBinding = uint32_t(i); w[i].descriptorCount = 1; }
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[0].pBufferInfo = &bi;
    w[1].descriptorType = w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[1].pImageInfo = &ii[0]; w[2].pImageInfo = &ii[1];
    vkUpdateDescriptorSets(m_ctx->Dev(), 3, w, 0, nullptr);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p.p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p.pl, 0, 1, &s, 0, nullptr);
    const int shift = im.bits > 8 ? 6 : 0;
    const float scale = im.bits > 8 ? 65535.f : 255.f;
    struct { int plane, w, h, bps; uint32_t off0, off1; int pitch, shift; float scale; } py{
        0, d.width, d.height, d.bps, uint32_t(d.offset), 0u, d.pitchY, shift, scale };
    vkCmdPushConstants(cb, p.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(py), &py);
    vkCmdDispatch(cb, uint32_t((d.width + 15) / 16), uint32_t((d.height + 7) / 8), 1);
    struct { int plane, w, h, bps; uint32_t off0, off1; int pitch, shift; float scale; } pu{
        1, d.cw, d.ch, d.bps, uint32_t(d.offset + d.offU), uint32_t(d.offset + d.offV), d.pitchC, shift, scale };
    vkCmdPushConstants(cb, p.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pu), &pu);
    vkCmdDispatch(cb, uint32_t((d.cw + 15) / 16), uint32_t((d.ch + 7) / 8), 1);
    ReleaseToForeign(cb, im);
    return true;
}

} // namespace pmdb
