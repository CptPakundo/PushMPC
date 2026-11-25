// ahb_path: see ahb_path.h.
#include "ahb_path.h"
#if defined(__ANDROID__) && defined(PM_AHB)
#include "gpu_engine.h"
#include "vk_context.h"

#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

extern "C" {
#include <libavcodec/bsf.h>
#include <libavcodec/codec_id.h>
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavutil/pixdesc.h>
}

#include "ahb2yuv_spv.h"   // kSpv_ahb2yuv, compiled by glslang at build time

namespace pmk {

// ------------------------------------------------------------------------------------------------------- decoder
AhbDecoder::~AhbDecoder()
{
    if (m_held) AImage_delete(m_held);
    if (m_codec) { AMediaCodec_stop(m_codec); AMediaCodec_delete(m_codec); }
    if (m_reader) AImageReader_delete(m_reader);
    if (m_bsf) av_bsf_free(&m_bsf);
}

bool AhbDecoder::Open(const AVCodecParameters* par, std::string& why)
{
    if (__builtin_available(android 26, *)) {
    } else {
        why = "Android 8 or later needed (AImageReader hardware buffers)";
        return false;
    }
    const char* mime = nullptr;
    const char* bsf = nullptr;
    switch (par->codec_id) {
    case AV_CODEC_ID_H264: mime = "video/avc"; bsf = "h264_mp4toannexb"; break;
    case AV_CODEC_ID_HEVC: mime = "video/hevc"; bsf = "hevc_mp4toannexb"; break;
    case AV_CODEC_ID_VP9: mime = "video/x-vnd.on2.vp9"; break;
    case AV_CODEC_ID_AV1: mime = "video/av01"; break;
    default: why = std::string("no MediaCodec path for ") + avcodec_get_name(par->codec_id); return false;
    }
    m_w = par->width; m_h = par->height;
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(AVPixelFormat(par->format));
    m_bits = d && d->comp[0].depth > 8 ? 10 : 8;
    // MediaCodec wants Annex B (start codes, parameter sets in band) where the container stores length-prefixed NALs
    if (bsf && par->extradata && par->extradata_size > 0 && par->extradata[0] == 1) {
        const AVBitStreamFilter* f = av_bsf_get_by_name(bsf);
        if (!f || av_bsf_alloc(f, &m_bsf) < 0 || avcodec_parameters_copy(m_bsf->par_in, par) < 0 || av_bsf_init(m_bsf) < 0) {
            why = std::string("bitstream filter ") + bsf + " unavailable";
            return false;
        }
    }
    if (AImageReader_newWithUsage(m_w, m_h, AIMAGE_FORMAT_PRIVATE, AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 6, &m_reader) != AMEDIA_OK) {
        why = "AImageReader_newWithUsage failed";
        return false;
    }
    ANativeWindow* win = nullptr;
    if (AImageReader_getWindow(m_reader, &win) != AMEDIA_OK || !win) { why = "AImageReader_getWindow failed"; return false; }
    m_codec = AMediaCodec_createDecoderByType(mime);
    if (!m_codec) { why = std::string("no MediaCodec decoder for ") + mime; return false; }
    AMediaFormat* fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, mime);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, m_w);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, m_h);
    const media_status_t st = AMediaCodec_configure(m_codec, fmt, win, nullptr, 0);
    AMediaFormat_delete(fmt);
    if (st != AMEDIA_OK || AMediaCodec_start(m_codec) != AMEDIA_OK) { why = "MediaCodec configure/start failed"; return false; }
    return true;
}

bool AhbDecoder::Send(const uint8_t* data, int size, int64_t pts)
{
    std::vector<uint8_t> annexB;
    if (m_bsf) {
        AVPacket* in = av_packet_alloc();
        AVPacket* out = av_packet_alloc();
        av_new_packet(in, size);
        std::memcpy(in->data, data, size_t(size));
        in->pts = pts;
        if (av_bsf_send_packet(m_bsf, in) >= 0)
            while (av_bsf_receive_packet(m_bsf, out) >= 0) { annexB.insert(annexB.end(), out->data, out->data + out->size); av_packet_unref(out); }
        av_packet_free(&in);
        av_packet_free(&out);
        data = annexB.data(); size = int(annexB.size());
    }
    const ssize_t idx = AMediaCodec_dequeueInputBuffer(m_codec, 0);
    if (idx < 0) { Pump(); return false; }
    size_t cap = 0;
    uint8_t* buf = AMediaCodec_getInputBuffer(m_codec, size_t(idx), &cap);
    const size_t n = std::min(cap, size_t(size));
    if (buf && n) std::memcpy(buf, data, n);
    AMediaCodec_queueInputBuffer(m_codec, size_t(idx), 0, n, uint64_t(pts < 0 ? 0 : pts), 0);
    Pump();
    return true;
}

void AhbDecoder::SendEnd()
{
    if (m_eosIn) return;
    const ssize_t idx = AMediaCodec_dequeueInputBuffer(m_codec, 2000);
    if (idx < 0) return;
    AMediaCodec_queueInputBuffer(m_codec, size_t(idx), 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
    m_eosIn = true;
}

// decoded output buffers go to the reader's surface (render = true); the reader then has the frame as an image
void AhbDecoder::Pump()
{
    for (;;) {
        AMediaCodecBufferInfo info{};
        const ssize_t idx = AMediaCodec_dequeueOutputBuffer(m_codec, &info, 0);
        if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED || idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
        if (idx < 0) return;
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) m_eosOut = true;
        AMediaCodec_releaseOutputBuffer(m_codec, size_t(idx), info.size > 0);
    }
}

bool AhbDecoder::Receive(AHardwareBuffer*& buf, int64_t& pts, bool& eof)
{
    eof = false;
    Pump();
    AImage* img = nullptr;
    if (AImageReader_acquireNextImage(m_reader, &img) != AMEDIA_OK || !img) { eof = m_eosOut; return false; }
    if (AImage_getHardwareBuffer(img, &buf) != AMEDIA_OK || !buf) { AImage_delete(img); return false; }
    int64_t ns = 0;
    AImage_getTimestamp(img, &ns);
    pts = ns / 1000;
    if (m_held) AImage_delete(m_held);
    m_held = img;
    return true;
}

void AhbDecoder::Release()
{
    if (m_held) { AImage_delete(m_held); m_held = nullptr; }
}

void AhbDecoder::Flush()
{
    Release();
    AImage* img = nullptr;
    while (AImageReader_acquireNextImage(m_reader, &img) == AMEDIA_OK && img) { AImage_delete(img); img = nullptr; }
    if (m_codec) AMediaCodec_flush(m_codec);
    if (m_bsf) av_bsf_flush(m_bsf);
    m_eosIn = m_eosOut = false;
}

// ------------------------------------------------------------------------------------------------------ importer
struct AhbImporter::Impl {
    std::shared_ptr<pmvs::gpu::Context> ctx;
    VkDevice dev = VK_NULL_HANDLE;
    uint64_t extFormat = 0;
    VkSamplerYcbcrConversion conv = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    // the frame being converted (freed after the GPU finished)
    VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE;

    void FreeFrame()
    {
        if (view) vkDestroyImageView(dev, view, nullptr);
        if (img) vkDestroyImage(dev, img, nullptr);
        if (mem) vkFreeMemory(dev, mem, nullptr);
        view = VK_NULL_HANDLE; img = VK_NULL_HANDLE; mem = VK_NULL_HANDLE;
        if (pool) vkResetDescriptorPool(dev, pool, 0);
    }
    void FreePipeline()
    {
        if (pipe) vkDestroyPipeline(dev, pipe, nullptr);
        if (pl) vkDestroyPipelineLayout(dev, pl, nullptr);
        if (dsl) vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
        if (sampler) vkDestroySampler(dev, sampler, nullptr);
        if (conv) vkDestroySamplerYcbcrConversion(dev, conv, nullptr);
        if (module) vkDestroyShaderModule(dev, module, nullptr);
        pipe = VK_NULL_HANDLE; pl = VK_NULL_HANDLE; dsl = VK_NULL_HANDLE; sampler = VK_NULL_HANDLE; conv = VK_NULL_HANDLE; module = VK_NULL_HANDLE;
    }
    ~Impl()
    {
        if (!dev) return;
        FreeFrame();
        FreePipeline();
        if (pool) vkDestroyDescriptorPool(dev, pool, nullptr);
    }

    // the conversion, sampler and pipeline for one external format (they are immutable per format)
    bool Pipeline(const VkAndroidHardwareBufferFormatPropertiesANDROID& fp, std::string& err)
    {
        if (pipe && fp.externalFormat == extFormat) return true;
        FreePipeline();
        extFormat = fp.externalFormat;
        VkExternalFormatANDROID ef{ VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID };
        ef.externalFormat = fp.externalFormat;
        const bool linear = (fp.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT) != 0;
        VkSamplerYcbcrConversionCreateInfo ci{ VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO };
        ci.pNext = &ef;
        ci.format = VK_FORMAT_UNDEFINED;
        ci.ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_IDENTITY;   // no matrix: the stored Y, Cb, Cr
        ci.ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL;                    // no range expansion: the stored codes
        ci.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
        ci.xChromaOffset = fp.suggestedXChromaOffset;
        ci.yChromaOffset = fp.suggestedYChromaOffset;
        ci.chromaFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        if (vkCreateSamplerYcbcrConversion(dev, &ci, nullptr, &conv) != VK_SUCCESS) { err = "vkCreateSamplerYcbcrConversion failed"; return false; }
        VkSamplerYcbcrConversionInfo cinfo{ VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO };
        cinfo.conversion = conv;
        VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        si.pNext = &cinfo;
        si.magFilter = si.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(dev, &si, nullptr, &sampler) != VK_SUCCESS) { err = "vkCreateSampler (YCbCr) failed"; return false; }
        VkDescriptorSetLayoutBinding b[4] = {};
        b[0].binding = 0; b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b[0].descriptorCount = 1; b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; b[0].pImmutableSamplers = &sampler;
        for (int i = 1; i < 4; ++i) { b[i].binding = uint32_t(i); b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; }
        VkDescriptorSetLayoutCreateInfo dl{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        dl.bindingCount = 4; dl.pBindings = b;
        if (vkCreateDescriptorSetLayout(dev, &dl, nullptr, &dsl) != VK_SUCCESS) { err = "vkCreateDescriptorSetLayout failed"; return false; }
        VkPushConstantRange pr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 16 };
        VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pli.setLayoutCount = 1; pli.pSetLayouts = &dsl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
        if (vkCreatePipelineLayout(dev, &pli, nullptr, &pl) != VK_SUCCESS) { err = "vkCreatePipelineLayout failed"; return false; }
        VkShaderModuleCreateInfo mi{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        mi.codeSize = sizeof(kSpv_ahb2yuv); mi.pCode = kSpv_ahb2yuv;
        if (vkCreateShaderModule(dev, &mi, nullptr, &module) != VK_SUCCESS) { err = "vkCreateShaderModule failed"; return false; }
        VkComputePipelineCreateInfo cp{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        cp.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr };
        cp.layout = pl;
        if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cp, nullptr, &pipe) != VK_SUCCESS) { err = "vkCreateComputePipelines failed"; return false; }
        if (!pool) {
            VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 12 } };
            VkDescriptorPoolCreateInfo pci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            pci.maxSets = 4; pci.poolSizeCount = 2; pci.pPoolSizes = ps;
            if (vkCreateDescriptorPool(dev, &pci, nullptr, &pool) != VK_SUCCESS) { err = "vkCreateDescriptorPool failed"; return false; }
        }
        return true;
    }

    // the hardware buffer as a sampled image with its external format
    bool ImportImage(AHardwareBuffer* buf, int& w, int& h, std::string& err)
    {
        VkAndroidHardwareBufferFormatPropertiesANDROID fp{ VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID };
        VkAndroidHardwareBufferPropertiesANDROID props{ VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID };
        props.pNext = &fp;
        if (vkGetAndroidHardwareBufferPropertiesANDROID(dev, buf, &props) != VK_SUCCESS) { err = "vkGetAndroidHardwareBufferPropertiesANDROID failed"; return false; }
        if (!Pipeline(fp, err)) return false;
        AHardwareBuffer_Desc desc{};
        AHardwareBuffer_describe(buf, &desc);
        w = int(desc.width); h = int(desc.height);
        VkExternalFormatANDROID ef{ VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID };
        ef.externalFormat = fp.externalFormat;
        VkExternalMemoryImageCreateInfo em{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
        em.pNext = &ef;
        em.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
        VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ii.pNext = &em;
        ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_UNDEFINED; ii.extent = { desc.width, desc.height, 1 };
        ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT; ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(dev, &ii, nullptr, &img) != VK_SUCCESS) { err = "vkCreateImage (hardware buffer) failed"; return false; }
        VkImportAndroidHardwareBufferInfoANDROID imp{ VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID };
        imp.buffer = buf;
        VkMemoryDedicatedAllocateInfo ded{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
        ded.image = img; ded.pNext = &imp;
        uint32_t type = 0;
        while (type < 32 && !(props.memoryTypeBits & (1u << type))) ++type;
        VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.pNext = &ded; ai.allocationSize = props.allocationSize; ai.memoryTypeIndex = type;
        if (type >= 32 || vkAllocateMemory(dev, &ai, nullptr, &mem) != VK_SUCCESS) { err = "importing the hardware buffer failed"; return false; }
        if (vkBindImageMemory(dev, img, mem, 0) != VK_SUCCESS) { err = "vkBindImageMemory (hardware buffer) failed"; return false; }
        VkSamplerYcbcrConversionInfo cinfo{ VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO };
        cinfo.conversion = conv;
        VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.pNext = &cinfo;
        vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_UNDEFINED;
        vi.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(dev, &vi, nullptr, &view) != VK_SUCCESS) { err = "vkCreateImageView (hardware buffer) failed"; return false; }
        return true;
    }
};

AhbImporter::AhbImporter() = default;
AhbImporter::~AhbImporter() = default;

int AhbImporter::Fill(pmvs::gpu::Engine& e, AHardwareBuffer* buf, std::string& err)
{
    if (!m) { m = std::make_unique<Impl>(); m->ctx = e.Ctx(); m->dev = m->ctx->Dev(); }
    Impl& I = *m;
    if (!I.ctx->AndroidHardwareBuffer()) { err = "the Vulkan device cannot import Android hardware buffers"; return -1; }
    I.FreeFrame();
    int w = 0, h = 0;
    if (!I.ImportImage(buf, w, h, err)) { I.FreeFrame(); return -1; }
    auto rec = [&I, w, h](VkCommandBuffer cb, const pmvs::gpu::SlotTarget& t, std::string& e2) -> bool {
        // the decoder's buffer comes from outside Vulkan: acquire it from the foreign queue family
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT; b.dstQueueFamilyIndex = I.ctx->Family();
        b.image = I.img; b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkDescriptorSetAllocateInfo da{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        da.descriptorPool = I.pool; da.descriptorSetCount = 1; da.pSetLayouts = &I.dsl;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(I.dev, &da, &set) != VK_SUCCESS) { e2 = "vkAllocateDescriptorSets failed"; return false; }
        VkDescriptorImageInfo src{ VK_NULL_HANDLE, I.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo dst[3] = { { VK_NULL_HANDLE, t.yv, VK_IMAGE_LAYOUT_GENERAL }, { VK_NULL_HANDLE, t.uv, VK_IMAGE_LAYOUT_GENERAL }, { VK_NULL_HANDLE, t.vv, VK_IMAGE_LAYOUT_GENERAL } };
        VkWriteDescriptorSet wr[4] = {};
        wr[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &src, nullptr, nullptr };
        for (int i = 0; i < 3; ++i) wr[i + 1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, uint32_t(i + 1), 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &dst[i], nullptr, nullptr };
        vkUpdateDescriptorSets(I.dev, 4, wr, 0, nullptr);
        // the slot's planes into GENERAL for the stores (their contents are replaced)
        VkImageMemoryBarrier sb[3] = {};
        const VkImage planes[3] = { t.y, t.u, t.v };
        for (int i = 0; i < 3; ++i) {
            sb[i] = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            sb[i].srcAccessMask = 0; sb[i].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            sb[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; sb[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            sb[i].srcQueueFamilyIndex = sb[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            sb[i].image = planes[i]; sb[i].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        }
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 3, sb);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, I.pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, I.pl, 0, 1, &set, 0, nullptr);
        const int pc[4] = { std::min(w, t.w), std::min(h, t.h), t.cw, t.ch };
        vkCmdPushConstants(cb, I.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
        vkCmdDispatch(cb, uint32_t((t.w + 15) / 16), uint32_t((t.h + 7) / 8), 1);
        return true;
    };
    uint64_t ticket = 0;
    const int slot = e.Import(rec, pmvs::gpu::ExternalSync{}, err, &ticket);
    if (slot >= 0 && !e.Wait(ticket, err)) return -1;
    return slot;
}

} // namespace pmk
#endif
