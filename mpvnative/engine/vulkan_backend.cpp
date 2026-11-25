// SPDX-License-Identifier: GPL-3.0-or-later
// VulkanBackend: the portable GPU engine (vsplugin/engines/gpu) on the decoder's own Vulkan device, zero copy.
//   in:  the FFmpeg Vulkan frame's planes -> import.comp -> the engine's source slot (one GPU pass, no host memory)
//   work: motion both ways, PushMPC's cut guard, k - 1 generated frames, optional NIS / FSR1 upscaling
//   out: the engine's output (device memory) -> export.comp -> a Vulkan frame of the filter's pool, which mpv shows
// Every step is ordered on the engine's timeline; the owner's per-image timeline semaphores are waited for and
// signalled (+1) around each import and export, so mpv / FFmpeg see the frames as they expect.
// Not here (the VapourSynth route has them): Dedup re-timing and Subtitle Hold (they need the CPU picture).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <tuple>
#include "backend.h"
#include "engine.h"
#include "gpu_backend.h"
#include "gpu_engine.h"
#include "platform.h"
#include "vk_context.h"
#include "pme_import.h"
#include "pme_export.h"

namespace pme {
namespace {

using namespace pmvs;
using namespace pmvs::gpu;

bool Ten(uint32_t f) { return f == PMENG_FMT_P010 || f == PMENG_FMT_YUV420P10; }
bool TwoPlane(uint32_t f) { return f == PMENG_FMT_NV12 || f == PMENG_FMT_P010; }

struct Pipe { VkDescriptorSetLayout dsl = VK_NULL_HANDLE; VkPipelineLayout pl = VK_NULL_HANDLE; VkPipeline p = VK_NULL_HANDLE; };

class VulkanBackend : public Backend {
public:
    ~VulkanBackend() override
    {
        std::string e;
        for (auto& q : m_q) m_eng.ReleaseOut(q.out);
        m_q.clear();
        if (!m_ctx) return;
        // our own work first (never vkDeviceWaitIdle on the owner's device)
        for (auto& dp : m_pools) if (dp.ticket) m_eng.Wait(dp.ticket, e);
        const VkDevice d = m_ctx->Dev();
        for (auto& kv : m_views) vkDestroyImageView(d, kv.second, nullptr);
        for (auto& dp : m_pools) if (dp.pool) vkDestroyDescriptorPool(d, dp.pool, nullptr);
        for (Pipe* p : { &m_import, &m_export }) {
            if (p->p) vkDestroyPipeline(d, p->p, nullptr);
            if (p->pl) vkDestroyPipelineLayout(d, p->pl, nullptr);
            if (p->dsl) vkDestroyDescriptorSetLayout(d, p->dsl, nullptr);
        }
        if (m_sampler) vkDestroySampler(d, m_sampler, nullptr);
    }

    bool Init(const pmeng_device& dev, const pmeng_stream& st, const Options& o, pmeng_output& out, std::string& why)
    {
        if (st.fmt != PMENG_FMT_NV12 && st.fmt != PMENG_FMT_P010 && st.fmt != PMENG_FMT_YUV420P && st.fmt != PMENG_FMT_YUV420P10) {
            why = "the Vulkan engine takes NV12 / P010 / YUV 4:2:0 frames"; return false;
        }
        if (!dev.vk_device || !dev.vk_get_instance_proc_addr) { why = "no Vulkan device"; return false; }
        m_fmt = st.fmt; m_w = int(st.width); m_h = int(st.height); m_bits = Ten(st.fmt) ? 10 : 8;
        ExternalDevice ext;
        ext.instance = static_cast<VkInstance>(dev.vk_instance);
        ext.phys = static_cast<VkPhysicalDevice>(dev.vk_physical_device);
        ext.device = static_cast<VkDevice>(dev.vk_device);
        ext.getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dev.vk_get_instance_proc_addr);
        ext.family = dev.vk_queue_family; ext.queueIndex = dev.vk_queue_index;
        ext.queueLock = dev.vk_lock_queue; ext.queueUnlock = dev.vk_unlock_queue; ext.lockCtx = dev.vk_lock_ctx;
        m_ctx = Context::Adopt(ext, why);
        if (!m_ctx) return false;

        // settings: the registry / ini, then the options
        PortableSettings s = LoadPortableSettings([](const char* n) { return ReadSetting(n); });
        if (!Opt(o, "smoother").empty()) s.smoother = ParseSmoother(Opt(o, "smoother"), s.smoother);
        if (!Opt(o, "enable").empty()) s.enable = ParseBool(Opt(o, "enable"), s.enable);
        const double srcFps = st.fps_num > 0 && st.fps_den > 0 ? double(st.fps_num) / st.fps_den : 0;
        const double displayHz = std::atof(Opt(o, "display_fps", "0").c_str());
        const int dispW = std::atoi(Opt(o, "display_w", "0").c_str()), dispH = std::atoi(Opt(o, "display_h", "0").c_str());
        std::string up = Opt(o, "upscaler", ReadSetting("Upscaler")), target = Opt(o, "target", ReadSetting("UpscaleTarget"));
        if (up.empty() || Opt(o, "sharper") == "off") up = "off";   // no upscaling unless asked for
        const UpscaleChoice uc = DecideUpscale(up, target, dispW, dispH, m_w, m_h);

        Config c;
        c.width = m_w; c.height = m_h; c.bits = m_bits;
        c.deviceReadback = true;
        c.upscaler = uc.kind == GpuUpscaler::Nis ? Upscaler::Nis : uc.kind == GpuUpscaler::Fsr ? Upscaler::Fsr : Upscaler::None;
        if (c.upscaler != Upscaler::None) { c.outWidth = uc.w; c.outHeight = uc.h; }
        const std::string q = Opt(o, "portable_quality", ReadSetting("PortableQuality"));
        c.quality = q == "fast" || q == "0" ? 1 : 2;
        if (!m_eng.Configure(c, m_ctx, why)) return false;
        if (!MakePipes(why)) return false;

        const GpuCalibration cal = s.enable && s.smoother == 0 ? CalibrateGpu(m_eng) : GpuCalibration{};
        const PortableK pk = s.enable ? DecidePortableK(s.smoother, srcFps, displayHz, m_h, cal.ok ? cal.meMs : 1.0, cal.ok ? cal.genMs : 1.0, true)
                                      : PortableK{ 1, 0, 0, "PushMPC is off (Enable = 0)" };
        m_k = std::clamp(pk.k, 1, 8);

        out.mem = PMENG_MEM_VULKAN;
        out.fmt = m_fmt;
        out.width = uint32_t(m_eng.OutWidth()); out.height = uint32_t(m_eng.OutHeight());
        out.multiplier = uint32_t(m_k);
        out.range = st.range; out.matrix = st.matrix; out.primaries = st.primaries; out.transfer = st.transfer;
        out.keep_dynamic_hdr = 1;
        std::snprintf(out.engine, sizeof(out.engine), "portable-vulkan");
        std::snprintf(out.description, sizeof(out.description), "PushFPS portable engine on %s (the decoder's device), %dx%d %d-bit -> %dx%d (%s); %s",
                      m_ctx->Info().Describe().c_str(), m_w, m_h, m_bits, m_eng.OutWidth(), m_eng.OutHeight(), UpscalerName(c.upscaler), pk.why.c_str());
        return true;
    }

    int Submit(const pmeng_image& src, int64_t, uint32_t flags) override
    {
        if (!m_q.empty()) return PMENG_BUSY;
        if (src.mem != PMENG_MEM_VULKAN || src.fmt != m_fmt) { err = "Vulkan engine: unexpected input memory / format"; return PMENG_ERROR; }
        const bool two = TwoPlane(m_fmt);
        const uint32_t np = two ? 2 : 3;
        if (src.vk_planes < np) { err = "Vulkan engine: missing planes"; return PMENG_ERROR; }
        ExternalSync sync;
        Semaphores(src, np, sync);
        VkImageView in[3] = {};
        for (uint32_t p = 0; p < np; ++p)
            if (!(in[p] = View(src.vk[p], PlaneFormat(p, two), VK_IMAGE_USAGE_SAMPLED_BIT))) return PMENG_ERROR;
        VkDescriptorSet set = VK_NULL_HANDLE;
        Pool* pool = NextPool();
        if (!pool) return PMENG_ERROR;
        auto rec = [&](VkCommandBuffer cb, const SlotTarget& t, std::string& e) -> bool {
            ToGeneral(cb, src, np);
            if (!Alloc(*pool, m_import, set, e)) return false;
            VkDescriptorImageInfo ii[6] = {};
            for (int i = 0; i < 3; ++i) { ii[i].sampler = m_sampler; ii[i].imageView = in[i < int(np) ? i : 1]; ii[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL; }
            ii[3].imageView = t.yv; ii[4].imageView = t.uv; ii[5].imageView = t.vv;
            for (int i = 3; i < 6; ++i) ii[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            VkWriteDescriptorSet w[6] = {};
            for (int i = 0; i < 6; ++i) {
                w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = set; w[i].dstBinding = uint32_t(i); w[i].descriptorCount = 1;
                w[i].descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[i].pImageInfo = &ii[i];
            }
            vkUpdateDescriptorSets(m_ctx->Dev(), 6, w, 0, nullptr);
            struct { int w, h, cw, ch, twoPlane, shift; float inScale, outScale; } pc{ t.w, t.h, t.cw, t.ch, two ? 1 : 0, m_fmt == PMENG_FMT_P010 ? 6 : 0,
                m_bits > 8 ? 65535.f : 255.f, m_bits > 8 ? 65535.f : 255.f };
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m_import.p);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m_import.pl, 0, 1, &set, 0, nullptr);
            vkCmdPushConstants(cb, m_import.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            vkCmdDispatch(cb, uint32_t((t.w + 15) / 16), uint32_t((t.h + 7) / 8), 1);
            return true;
        };
        uint64_t itk = 0;
        const int slot = m_eng.Import(rec, sync, err, &itk);
        if (slot < 0) return PMENG_ERROR;
        pool->ticket = itk;
        m_last = itk;
        if (m_hostSync && !m_eng.Wait(itk, err)) return PMENG_ERROR;   // diagnostics: PUSHMPC_VK_SYNC=1

        const bool contiguous = !(flags & PMENG_SUBMIT_DISCONTINUITY) && m_prevSlot >= 0;
        if (!contiguous) ResetHistory();
        if (contiguous && m_k > 1) {
            const int pair = m_eng.EstimateBegin(m_prevSlot, slot, m_prevPair, err);
            if (pair < 0) return PMENG_ERROR;
            PairStats ps;
            if (!m_eng.EstimateEnd(pair, ps, err)) return PMENG_ERROR;
            const int cut = m_guard.Decide(ps);
            if (cut) ++m_cuts;
            for (int j = 1; j < m_k; ++j) {
                const int mode = cut ? (2 * j < m_k ? 1 : 2) : 0;   // a cut: the nearer real frame, never a morph
                int o = -1; uint64_t tk = 0;
                if (!m_eng.Generate(pair, float(j) / float(m_k), mode, o, tk, err)) return PMENG_ERROR;
                m_q.push_back({ { m_seq, uint32_t(j), uint32_t(m_k), 1 }, o });
            }
            if (m_prevPair >= 0) m_eng.ReleasePair(m_prevPair);
            m_prevPair = pair;
        }
        int o = -1; uint64_t tk = 0;
        if (!m_eng.Real(slot, o, tk, err)) return PMENG_ERROR;
        m_q.push_back({ { m_seq, 0, uint32_t(m_k), 0 }, o });
        if (m_prevSlot >= 0) m_eng.ReleaseSlot(m_prevSlot);   // its outputs are recorded; the queue keeps the order
        m_prevSlot = slot;
        ++m_seq;
        return PMENG_OK;
    }

    int Peek(pmeng_out_info& info, uint32_t) override
    {
        if (m_q.empty()) return 0;
        info = m_q.front().info;
        return 1;
    }

    int Write(const pmeng_image& dst) override
    {
        if (m_q.empty()) { err = "write without an output"; return PMENG_ERROR; }
        const Item it = m_q.front();
        m_q.pop_front();
        const bool two = TwoPlane(m_fmt);
        const uint32_t np = two ? 2 : 3;
        if (dst.mem != PMENG_MEM_VULKAN || dst.vk_planes < np) { m_eng.ReleaseOut(it.out); err = "Vulkan engine: unexpected output frame"; return PMENG_ERROR; }
        ExternalSync sync;
        Semaphores(dst, np, sync);
        VkImageView ov[3] = {};
        for (uint32_t p = 0; p < np; ++p)
            if (!(ov[p] = View(dst.vk[p], PlaneFormat(p, two), VK_IMAGE_USAGE_STORAGE_BIT))) { m_eng.ReleaseOut(it.out); return PMENG_ERROR; }
        Pool* pool = NextPool();
        if (!pool) { m_eng.ReleaseOut(it.out); return PMENG_ERROR; }
        auto rec = [&](VkCommandBuffer cb, const OutSource& s, std::string& e) -> bool {
            ToGeneral(cb, dst, np);
            VkDescriptorSet set = VK_NULL_HANDLE;
            if (!Alloc(*pool, m_export, set, e)) return false;
            VkDescriptorBufferInfo bi{ s.buffer, 0, VK_WHOLE_SIZE };
            VkDescriptorImageInfo ii[3] = {};
            for (int i = 0; i < 3; ++i) { ii[i].imageView = ov[i < int(np) ? i : 1]; ii[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL; }
            VkWriteDescriptorSet w[4] = {};
            for (int i = 0; i < 4; ++i) { w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = set; w[i].dstBinding = uint32_t(i); w[i].descriptorCount = 1; }
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[0].pBufferInfo = &bi;
            for (int i = 1; i < 4; ++i) { w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[i].pImageInfo = &ii[i - 1]; }
            vkUpdateDescriptorSets(m_ctx->Dev(), 4, w, 0, nullptr);
            struct { uint32_t offY, offU, offV, pitchY, pitchC; int w, h, cw, ch, bps, twoPlane, shift; float outScale; } pc{
                uint32_t(s.offset), uint32_t(s.offset + s.offU), uint32_t(s.offset + s.offV), uint32_t(s.pitchY), uint32_t(s.pitchC),
                s.w, s.h, s.cw, s.ch, s.bits > 8 ? 2 : 1, two ? 1 : 0, m_fmt == PMENG_FMT_P010 ? 6 : 0, s.bits > 8 ? 65535.f : 255.f };
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m_export.p);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m_export.pl, 0, 1, &set, 0, nullptr);
            vkCmdPushConstants(cb, m_export.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            vkCmdDispatch(cb, uint32_t((s.w + 15) / 16), uint32_t((s.h + 7) / 8), 1);
            return true;
        };
        uint64_t tk = 0;
        if (!m_eng.Export(it.out, rec, sync, tk, err)) { m_eng.ReleaseOut(it.out); return PMENG_ERROR; }
        pool->ticket = tk;
        m_last = tk;
        if (m_hostSync && !m_eng.Wait(tk, err)) return PMENG_ERROR;
        ++m_written;
        if (it.info.generated) ++m_generated;
        return PMENG_OK;
    }

    int Skip() override { if (!m_q.empty()) { m_eng.ReleaseOut(m_q.front().out); m_q.pop_front(); } return PMENG_OK; }
    uint32_t Pending() override { return uint32_t(m_q.size()); }
    int Flush() override
    {
        for (auto& q : m_q) m_eng.ReleaseOut(q.out);
        m_q.clear();
        ResetHistory();
        return PMENG_OK;
    }
    std::string Stats() override
    {
        char b[320];
        const auto& t = m_eng.Times();
        std::snprintf(b, sizeof(b), "portable-vulkan: %llu sources, %llu outputs (%llu generated), %llu cuts; import %.2f ms, motion %.2f ms (means)",
                      (unsigned long long)m_seq, (unsigned long long)m_written, (unsigned long long)m_generated, (unsigned long long)m_cuts,
                      t.uploads ? t.uploadMs / double(t.uploads) : 0.0, t.pairs ? t.meMs / double(t.pairs) : 0.0);
        return b;
    }

private:
    struct Item { pmeng_out_info info; int out; };
    struct Pool { VkDescriptorPool pool = VK_NULL_HANDLE; uint64_t ticket = 0; };

    void ResetHistory()
    {
        if (m_prevPair >= 0) m_eng.ReleasePair(m_prevPair);
        if (m_prevSlot >= 0) m_eng.ReleaseSlot(m_prevSlot);
        m_prevPair = m_prevSlot = -1;
    }

    static VkFormat PlaneFormatOf(uint32_t p, bool two, bool ten)
    {
        if (p == 0) return ten ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM;
        if (two) return ten ? VK_FORMAT_R16G16_UNORM : VK_FORMAT_R8G8_UNORM;
        return ten ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM;
    }
    VkFormat PlaneFormat(uint32_t p, bool two) const { return PlaneFormatOf(p, two, m_bits > 8); }

    // one view per (image, aspect, usage), kept while the owner's pool reuses its images
    VkImageView View(const pmeng_image::pmeng_vk_plane& pl, VkFormat f, VkImageUsageFlags usage)
    {
        const auto key = std::make_tuple(reinterpret_cast<uintptr_t>(pl.image), pl.aspect, uint32_t(usage));
        auto it = m_views.find(key);
        if (it != m_views.end()) return it->second;
        if (m_views.size() > 256) {   // a new pool (seek, size change): drop the old views once our work is done
            std::string e;
            if (m_last) m_eng.Wait(m_last, e);
            for (auto& kv : m_views) vkDestroyImageView(m_ctx->Dev(), kv.second, nullptr);
            m_views.clear();
        }
        VkImageViewUsageCreateInfo uci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO };
        uci.usage = usage;
        VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.pNext = &uci;
        vi.image = static_cast<VkImage>(pl.image); vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = f;
        vi.subresourceRange = { VkImageAspectFlags(pl.aspect ? pl.aspect : VK_IMAGE_ASPECT_COLOR_BIT), 0, 1, 0, 1 };
        VkImageView v = VK_NULL_HANDLE;
        const VkResult r = vkCreateImageView(m_ctx->Dev(), &vi, nullptr, &v);
        if (r != VK_SUCCESS) { err = "Vulkan engine: vkCreateImageView on the frame failed (" + std::to_string(int(r)) + ")"; return VK_NULL_HANDLE; }
        m_views[key] = v;
        return v;
    }
    void Semaphores(const pmeng_image& im, uint32_t np, ExternalSync& s) const
    {
        for (uint32_t p = 0; p < np; ++p) {
            auto sem = static_cast<VkSemaphore>(im.vk[p].semaphore);
            if (!sem || std::find(s.wait.begin(), s.wait.end(), sem) != s.wait.end()) continue;
            s.wait.push_back(sem); s.waitValue.push_back(im.vk[p].value);
            s.signal.push_back(sem); s.signalValue.push_back(im.vk[p].value + 1);
        }
    }
    void ToGeneral(VkCommandBuffer cb, const pmeng_image& im, uint32_t np) const
    {
        VkImageMemoryBarrier b[3] = {};
        uint32_t n = 0;
        for (uint32_t p = 0; p < np; ++p) {
            bool dup = false;
            for (uint32_t q = 0; q < n; ++q) if (b[q].image == static_cast<VkImage>(im.vk[p].image)) dup = true;
            if (dup) continue;
            VkImageMemoryBarrier& x = b[n++];
            x.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            x.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
            x.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            x.oldLayout = VkImageLayout(im.vk[p].layout); x.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            x.srcQueueFamilyIndex = x.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;   // FFmpeg's pools: concurrent sharing
            x.image = static_cast<VkImage>(im.vk[p].image);
            const bool multi = im.vk[p].aspect & (VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT | VK_IMAGE_ASPECT_PLANE_2_BIT);
            x.subresourceRange = { VkImageAspectFlags(multi ? (np == 2 ? VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT
                                                                       : VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT | VK_IMAGE_ASPECT_PLANE_2_BIT)
                                                         : VK_IMAGE_ASPECT_COLOR_BIT), 0, 1, 0, 1 };
        }
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, n, b);
    }

    bool MakePipe(Pipe& p, const uint32_t* code, size_t bytes, const std::vector<VkDescriptorType>& types, uint32_t push, std::string& e)
    {
        const VkDevice d = m_ctx->Dev();
        std::vector<VkDescriptorSetLayoutBinding> b(types.size());
        for (size_t k = 0; k < b.size(); ++k) { b[k].binding = uint32_t(k); b[k].descriptorType = types[k]; b[k].descriptorCount = 1; b[k].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; }
        VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = uint32_t(b.size()); li.pBindings = b.data();
        if (vkCreateDescriptorSetLayout(d, &li, nullptr, &p.dsl) != VK_SUCCESS) { e = "vkCreateDescriptorSetLayout failed"; return false; }
        VkPushConstantRange pr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, push };
        VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pli.setLayoutCount = 1; pli.pSetLayouts = &p.dsl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
        if (vkCreatePipelineLayout(d, &pli, nullptr, &p.pl) != VK_SUCCESS) { e = "vkCreatePipelineLayout failed"; return false; }
        VkShaderModuleCreateInfo si{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        si.codeSize = bytes; si.pCode = code;
        VkShaderModule mod = VK_NULL_HANDLE;
        if (vkCreateShaderModule(d, &si, nullptr, &mod) != VK_SUCCESS) { e = "vkCreateShaderModule failed"; return false; }
        VkComputePipelineCreateInfo ci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        ci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr };
        ci.layout = p.pl;
        const VkResult r = vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, &p.p);
        vkDestroyShaderModule(d, mod, nullptr);
        if (r != VK_SUCCESS) { e = "vkCreateComputePipelines failed (" + std::to_string(int(r)) + ")"; return false; }
        return true;
    }
    bool MakePipes(std::string& e)
    {
        const VkDescriptorType CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, SB = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        if (!MakePipe(m_import, kSpv_pme_import, sizeof(kSpv_pme_import), { CIS, CIS, CIS, SI, SI, SI }, 32, e)) return false;
        if (!MakePipe(m_export, kSpv_pme_export, sizeof(kSpv_pme_export), { SB, SI, SI, SI }, 52, e)) return false;
        VkSamplerCreateInfo s{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        s.magFilter = s.minFilter = VK_FILTER_NEAREST; s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        s.addressModeU = s.addressModeV = s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(m_ctx->Dev(), &s, nullptr, &m_sampler) != VK_SUCCESS) { e = "vkCreateSampler failed"; return false; }
        m_pools.resize(24);
        for (auto& p : m_pools) {
            const VkDescriptorPoolSize sizes[] = { { CIS, 6 }, { SI, 6 }, { SB, 2 } };
            VkDescriptorPoolCreateInfo dpi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            dpi.maxSets = 2; dpi.poolSizeCount = 3; dpi.pPoolSizes = sizes;
            if (vkCreateDescriptorPool(m_ctx->Dev(), &dpi, nullptr, &p.pool) != VK_SUCCESS) { e = "vkCreateDescriptorPool failed"; return false; }
        }
        return true;
    }
    // a descriptor pool whose last use the GPU has finished (ring)
    Pool* NextPool()
    {
        Pool& p = m_pools[m_nextPool];
        m_nextPool = (m_nextPool + 1) % m_pools.size();
        if (p.ticket && !m_eng.Wait(p.ticket, err)) return nullptr;
        vkResetDescriptorPool(m_ctx->Dev(), p.pool, 0);
        p.ticket = 0;
        return &p;
    }
    bool Alloc(Pool& p, const Pipe& pipe, VkDescriptorSet& set, std::string& e)
    {
        VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = p.pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &pipe.dsl;
        if (vkAllocateDescriptorSets(m_ctx->Dev(), &ai, &set) != VK_SUCCESS) { e = "vkAllocateDescriptorSets failed"; return false; }
        return true;
    }

    bool m_hostSync = std::getenv("PUSHMPC_VK_SYNC") && std::getenv("PUSHMPC_VK_SYNC")[0] == '1';
    std::shared_ptr<Context> m_ctx;
    Engine m_eng;
    CutGuard m_guard;
    uint32_t m_fmt = 0;
    int m_w = 0, m_h = 0, m_bits = 8, m_k = 1;
    int m_prevSlot = -1, m_prevPair = -1;
    uint64_t m_seq = 0, m_written = 0, m_generated = 0, m_cuts = 0, m_last = 0;
    std::deque<Item> m_q;
    Pipe m_import, m_export;
    VkSampler m_sampler = VK_NULL_HANDLE;
    std::vector<Pool> m_pools;
    size_t m_nextPool = 0;
    std::map<std::tuple<uintptr_t, uint32_t, uint32_t>, VkImageView> m_views;
};

} // namespace

Backend* OpenVulkan(const pmeng_device& dev, const pmeng_stream& st, const Options& o, pmeng_output& out, std::string& why)
{
    auto* b = new VulkanBackend();
    if (!b->Init(dev, st, o, out, why)) { delete b; return nullptr; }
    return b;
}

} // namespace pme
