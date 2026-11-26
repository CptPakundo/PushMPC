// SPDX-License-Identifier: GPL-3.0-or-later
// VulkanBackend: the portable GPU engine (vsplugin/engines/gpu) on the decoder's own Vulkan device, zero copy.
//   in:  the FFmpeg Vulkan frame's planes -> import.comp -> the engine's source slot (one GPU pass, no host memory)
//   work: motion both ways, PushMPC's cut guard, k - 1 generated frames, optional NIS / FSR1 upscaling
//   out: the engine's output (device memory) -> export.comp -> a Vulkan frame of the filter's pool, which mpv shows
// Every step is ordered on the engine's timeline; the owner's per-image timeline semaphores are waited for and
// signalled (+1) around each import and export, so mpv / FFmpeg see the frames as they expect.
// Not here (the VapourSynth route has them): Dedup re-timing and Subtitle Hold (they need the CPU picture).
#include <algorithm>
#include <array>
#include <future>
#include <iterator>
#include <memory>
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
#include "cadence.h"
#include "subtitle_hold.h"
#include "vk_context.h"
#include "pme_import.h"
#include "pme_export.h"
#include "pme_thumb.h"
#include "transitions.h"

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
        for (auto& tb : m_thumbRing) m_ctx->Destroy(tb.buf);
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
        // motion: Auto = hybrid (NVIDIA optical flow as one more block-matcher candidate) where the device has the flow
        // hardware, as the VapourSynth route; blocks / nvof / hybrid by hand (PortableMotion)
        const std::string mo = Opt(o, "motion", ReadSetting("PortableMotion"));
        c.nvofHybrid = mo.empty() || mo == "auto" || mo == "hybrid";
        c.blockMatcherOnly = mo != "nvof" && !c.nvofHybrid;
        if (!m_eng.Configure(c, m_ctx, why)) return false;
        if (!MakePipesOnce(why)) return false;

        const GpuCalibration cal = s.enable && s.smoother == 0 ? CalibrateGpu(m_eng) : GpuCalibration{};
        const PortableK pk = s.enable ? DecidePortableK(s.smoother, srcFps, displayHz, m_h, cal.ok ? cal.meMs : 1.0, cal.ok ? cal.genMs : 1.0, true)
                                      : PortableK{ 1, 0, 0, "PushMPC is off (Enable = 0)" };
        m_k = std::clamp(pk.k, 1, 8);
        m_dedup = m_k > 1 && ParseBool(Opt(o, "dedup", ReadSetting("Dedup")), true);
        m_holdOn = m_k > 1 && ParseBool(Opt(o, "subtitle_hold", ReadSetting("SubtitleHold")), false);
        m_transitions = m_k > 1 && ParseBool(Opt(o, "transitions", ReadSetting("TransitionRules")), true);
        // lookahead 3 (twos, threes and fours) where the output slots allow it, else 2
        const int look = m_dedup ? ((3 + 1) * m_k + 2 <= 28 ? 3 : 2) : 0;
        if (const int needOuts = (look + 1) * m_k + 2; needOuts > c.outSlots) {   // a settle can release (lookahead + 1) x k outputs
            c.outSlots = std::min(needOuts, 28);
            if (!m_eng.Configure(c, m_ctx, why) || !MakePipesOnce(why)) return false;
        }
        m_cadence.Configure(m_k, look, 0);

        out.mem = PMENG_MEM_VULKAN;
        out.fmt = m_fmt;
        out.width = uint32_t(m_eng.OutWidth()); out.height = uint32_t(m_eng.OutHeight());
        out.multiplier = uint32_t(m_k);
        out.range = st.range; out.matrix = st.matrix; out.primaries = st.primaries; out.transfer = st.transfer;
        out.keep_dynamic_hdr = 1;
        std::snprintf(out.engine, sizeof(out.engine), "portable-vulkan");
        std::snprintf(out.description, sizeof(out.description), "PushFPS portable engine on %s (the decoder's device), %dx%d %d-bit -> %dx%d (%s); %s",
                      m_ctx->Info().Describe().c_str(), m_w, m_h, m_bits, m_eng.OutWidth(), m_eng.OutHeight(), UpscalerName(c.upscaler), pk.why.c_str());
        std::snprintf(out.description + std::strlen(out.description), sizeof(out.description) - std::strlen(out.description), "; Dedup %s, Subtitle Hold %s",
                      m_dedup ? "on" : "off", m_holdOn ? "on" : "off");
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
        ThumbBuf* thumb = m_transitions ? NextThumb() : nullptr;
        if (m_transitions && !thumb) return PMENG_ERROR;
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
            if (thumb && !RecordThumb(cb, *pool, t, *thumb, e)) return false;
            return true;
        };
        uint64_t itk = 0;
        const int slot = m_eng.Import(rec, sync, err, &itk);
        if (slot < 0) return PMENG_ERROR;
        pool->ticket = itk;
        if (thumb) { thumb->src = int64_t(m_seq); thumb->ticket = itk; }
        m_lastTicket = itk;
        if (m_hostSync && !m_eng.Wait(itk, err)) return PMENG_ERROR;   // diagnostics: PUSHMPC_VK_SYNC=1

        const int64_t s = int64_t(m_seq++);
        const bool contiguous = !(flags & PMENG_SUBMIT_DISCONTINUITY) && m_last == s - 1;
        if (!contiguous) Drop();
        m_last = s;
        // Dedup (PushFPS portable's rule on the GPU: the quarter-size luma against the last distinct picture)
        bool dup = false;
        if (m_dedup && m_lastDistinctSlot >= 0) {
            DedupStats ds;
            if (!m_eng.Duplicate(m_lastDistinctSlot, slot, ds, err)) return PMENG_ERROR;
            dup = ds.duplicate;
        }
        if (dup) { m_eng.ReleaseSlot(slot); ++m_dups; }
        else {
            m_slotOf[s] = slot;
            m_lastDistinctSlot = slot;
            if (m_prevDistinct >= 0 && !StartPair(m_prevDistinct, s)) return PMENG_ERROR;
            m_prevDistinct = s;
            if (m_holdOn && !HoldFrame(s, slot)) return PMENG_ERROR;
        }
        m_cadence.Push(s, dup);
        if (!SettleAndEmit()) return PMENG_ERROR;
        Prune();
        return PMENG_OK;
    }

    int Finish() override
    {
        m_cadence.Finish();
        return SettleAndEmit() ? PMENG_OK : PMENG_ERROR;
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
        m_lastTicket = tk;
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
        Drop();
        return PMENG_OK;
    }
    std::string Stats() override
    {
        char b[600];
        const auto& t = m_eng.Times();
        std::snprintf(b, sizeof(b), "portable-vulkan: %llu sources (%llu duplicates), %llu outputs (%llu generated, %llu held), %llu cuts, %llu flashes held, %llu dissolves blended, %llu pairs with Subtitle Hold; import %.2f ms, motion %.2f ms (means)",
                      (unsigned long long)m_seq, (unsigned long long)m_dups, (unsigned long long)m_written, (unsigned long long)m_generated, (unsigned long long)m_holds,
                      (unsigned long long)m_cuts, (unsigned long long)m_flashes, (unsigned long long)m_linear, (unsigned long long)m_heldPairs,
                      t.uploads ? t.uploadMs / double(t.uploads) : 0.0, t.pairs ? t.meMs / double(t.pairs) : 0.0);
        return b;
    }

private:
    struct Item { pmeng_out_info info; int out; };
    struct Pool { VkDescriptorPool pool = VK_NULL_HANDLE; uint64_t ticket = 0; };
    struct ThumbBuf { Buffer buf; int64_t src = -1; uint64_t ticket = 0; int tw = 0, th = 0, rowWords = 0; };

    struct PairInfo { int id = -1; int cut = 0; bool pending = false; };

    // the next thumbnail buffer of the ring (its last use finished; sized at the first use)
    ThumbBuf* NextThumb()
    {
        ThumbBuf& tb = m_thumbRing[m_nextThumb];
        m_nextThumb = (m_nextThumb + 1) % m_thumbRing.size();
        if (tb.ticket && !m_eng.Wait(tb.ticket, err)) return nullptr;
        if (tb.src >= 0 && !m_thumbs.count(tb.src)) ThumbFromBuf(tb);   // keep what an older pair may still ask for
        tb.src = -1; tb.ticket = 0;
        if (!tb.buf.buf) {
            tb.tw = std::max(1, m_w / 8); tb.th = std::max(1, m_h / 8); tb.rowWords = (tb.tw + 3) / 4;
            if (!m_ctx->CreateBuffer(VkDeviceSize(tb.rowWords) * VkDeviceSize(tb.th) * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, tb.buf, err)) return nullptr;
        }
        return &tb;
    }
    bool RecordThumb(VkCommandBuffer cb, Pool& pool, const SlotTarget& t, const ThumbBuf& tb, std::string& e)
    {
        VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (!Alloc(pool, m_thumbPipe, set, e)) return false;
        VkDescriptorImageInfo ii{ m_sampler, t.yv, VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorBufferInfo bi{ tb.buf.buf, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet w[2] = {};
        for (int i = 0; i < 2; ++i) { w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = set; w[i].dstBinding = uint32_t(i); w[i].descriptorCount = 1; }
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &ii;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &bi;
        vkUpdateDescriptorSets(m_ctx->Dev(), 2, w, 0, nullptr);
        struct { int tw, th, rowWords, shift; float kRaw; } pc{ tb.tw, tb.th, tb.rowWords, std::max(0, m_bits - 8), m_bits > 8 ? 65535.f : 255.f };
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m_thumbPipe.p);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m_thumbPipe.pl, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cb, m_thumbPipe.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cb, uint32_t((tb.rowWords * tb.th + 63) / 64), 1, 1);
        return true;
    }
    void ThumbFromBuf(const ThumbBuf& tb)
    {
        Thumb t;
        t.w = tb.tw; t.h = tb.th;
        t.px.resize(size_t(t.w) * size_t(t.h));
        const uint8_t* p = static_cast<const uint8_t*>(tb.buf.map);
        double sum = 0;
        for (int y = 0; y < t.h; ++y)
            for (int x = 0; x < t.w; ++x) { const uint8_t v = p[size_t(y) * size_t(tb.rowWords) * 4 + size_t(x)]; t.px[size_t(y) * size_t(t.w) + size_t(x)] = v; sum += v; }
        t.mean = sum / double(t.px.size());
        m_thumbs[tb.src] = std::move(t);
    }
    // the thumbnail of source frame s, or null (not imported yet, or too old)
    const Thumb* ThumbAt(int64_t s)
    {
        if (auto it = m_thumbs.find(s); it != m_thumbs.end()) return &it->second;
        for (auto& tb : m_thumbRing)
            if (tb.src == s) {
                if (tb.ticket && !m_eng.Wait(tb.ticket, err)) return nullptr;
                ThumbFromBuf(tb);
                return &m_thumbs[s];
            }
        return nullptr;
    }
    // a seek or gap: every queued output and picture forgotten
    void Drop()
    {
        for (auto& q : m_q) m_eng.ReleaseOut(q.out);
        m_q.clear();
        if (m_holdChain.valid()) m_holdChain.wait();
        m_holdChain = {};
        m_holdOf.clear();
        m_hold.Reset();
        m_thumbs.clear();
        for (auto& [key, p] : m_pairs) m_eng.ReleasePair(p.id);
        m_pairs.clear();
        m_lastPair = -1; m_lastPairKey = { -1, -1 };
        for (auto& [s, slot] : m_slotOf) m_eng.ReleaseSlot(slot);
        m_slotOf.clear();
        m_lastDistinctSlot = -1; m_prevDistinct = -1;
        m_cadence.Reset();
        m_last = -1000000;
    }
    // the GPU picture of source frame s: its own, or the distinct frame it repeats
    int SlotAt(int64_t s) const
    {
        auto it = m_slotOf.upper_bound(s);
        if (it == m_slotOf.begin()) return m_slotOf.empty() ? -1 : m_slotOf.begin()->second;
        return std::prev(it)->second;
    }
    bool StartPair(int64_t d0, int64_t d1)
    {
        const auto key = std::make_pair(d0, d1);
        if (m_pairs.count(key)) return true;
        auto a = m_slotOf.find(d0), b = m_slotOf.find(d1);
        if (a == m_slotOf.end() || b == m_slotOf.end()) { err = "Vulkan engine: a picture of the pair is gone"; return false; }
        const int prev = m_lastPairKey.second == d0 ? m_lastPair : -1;
        const int id = m_eng.EstimateBegin(a->second, b->second, prev, err);
        if (id < 0) return false;
        PairInfo pi; pi.id = id; pi.pending = true;
        m_pairs[key] = pi;
        m_lastPair = id; m_lastPairKey = key;
        return true;
    }
    bool PairFor(int64_t d0, int64_t d1, PairInfo& out)
    {
        const auto key = std::make_pair(d0, d1);
        auto it = m_pairs.find(key);
        if (it == m_pairs.end()) { if (!StartPair(d0, d1)) return false; it = m_pairs.find(key); }
        if (!it->second.pending) { out = it->second; return true; }
        PairStats st;
        if (!m_eng.EstimateEnd(it->second.id, st, err)) return false;
        PairInfo pi; pi.id = it->second.id;
        pi.cut = m_transitions ? DecidePair(m_guard, st, m_rules, ThumbAt(d0 - 1), ThumbAt(d0), ThumbAt(d1), ThumbAt(d1 + 1)) : m_guard.Decide(st);
        if (pi.cut == 3) ++m_flashes;
        if (pi.cut == 4) ++m_linear;
        if (pi.cut == 1 || pi.cut == 2) ++m_cuts;
        if (m_holdOn && !pi.cut) {
            auto ha = m_holdOf.find(d0), hb = m_holdOf.find(d1);
            if (ha != m_holdOf.end() && hb != m_holdOf.end()) {
                auto fa = ha->second.get(), fb = hb->second.get();
                if (fa && fb) {
                    auto hp = SubtitleHoldYuv::Pair(*fa, *fb, m_bits);
                    if (!m_eng.SetHold(pi.id, hp.get(), err)) return false;
                    if (hp && hp->held > 0) ++m_heldPairs;
                }
            }
        }
        it->second = pi;
        out = pi;
        return true;
    }
    // Subtitle Hold: only the subtitle band comes back to the host (about 1.4 MB at 4K 8-bit), the rule runs on a worker
    bool HoldFrame(int64_t s, int slot)
    {
        const veyra::pipeline::overlay::Zone z = veyra::pipeline::overlay::subtitleZoneFor(uint32_t(m_w), uint32_t(m_h));
        if (z.size == 0) return true;
        auto band = std::make_shared<std::array<std::vector<uint8_t>, 3>>();
        std::vector<uint8_t> planes[3];
        if (!m_eng.ReadRect(slot, int(z.x), int(z.y), int(z.w), int(z.h), planes, err)) return false;
        for (int p = 0; p < 3; ++p) (*band)[size_t(p)] = std::move(planes[p]);
        const int bps = m_bits > 8 ? 2 : 1, zx = int(z.x), zy = int(z.y), zw = int(z.w), w = m_w, h = m_h, bits = m_bits;
        std::shared_future<std::shared_ptr<const HoldFrameYuv>> prev = m_holdChain;
        SubtitleHoldYuv* hold = &m_hold;
        m_holdChain = std::async(std::launch::async, [prev, hold, band, bps, zx, zy, zw, w, h, bits]() {
            if (prev.valid()) prev.wait();
            // a full-size view whose rows inside the band point into the band copy (the rule reads only the band)
            portable::YuvView v;
            v.width = w; v.height = h; v.bits = bits;
            v.stride[0] = ptrdiff_t(zw) * bps; v.stride[1] = v.stride[2] = ptrdiff_t(zw / 2) * bps;
            v.data[0] = (*band)[0].data() - (ptrdiff_t(zy) * v.stride[0] + ptrdiff_t(zx) * bps);
            v.data[1] = (*band)[1].data() - (ptrdiff_t(zy / 2) * v.stride[1] + ptrdiff_t(zx / 2) * bps);
            v.data[2] = (*band)[2].data() - (ptrdiff_t(zy / 2) * v.stride[2] + ptrdiff_t(zx / 2) * bps);
            return hold->Frame(v);
        }).share();
        m_holdOf[s] = m_holdChain;
        return true;
    }
    // the outputs Cadence settles: GPU work recorded now, exported into the filter's frames by Write
    bool SettleAndEmit()
    {
        std::vector<Settled> settled;
        m_cadence.Settle(settled);
        for (const Settled& st : settled) {
            const int64_t pos = st.n / m_k + (st.n % m_k ? 1 : 0);
            pmeng_out_info info{ uint64_t(pos), uint32_t(st.n % m_k), uint32_t(m_k), 0 };
            const Shot& sh = st.shot;
            int o = -1; uint64_t tk = 0;
            if (sh.kind != Shot::Generated) {
                const int slot = SlotAt(sh.src);
                if (slot < 0) { err = "Vulkan engine: no picture for frame " + std::to_string(sh.src); return false; }
                if (!m_eng.Real(slot, o, tk, err)) return false;
                info.generated = sh.kind == Shot::Hold ? 1 : 0;
                if (sh.kind == Shot::Hold) ++m_holds;
            } else {
                PairInfo pi;
                if (!PairFor(sh.d0, sh.d1, pi)) return false;
                // 1, 2 cuts and 3 flashes: the nearer real frame (never a morph); 4 a dissolve or fade: the plain linear blend
                const int mode = pi.cut == 4 ? 3 : pi.cut ? (sh.t < 0.5f ? 1 : 2) : 0;
                if (!m_eng.Generate(pi.id, sh.t, mode, o, tk, err)) return false;
                info.generated = 1;
            }
            m_q.push_back({ info, o });
        }
        return true;
    }
    // forget what no later output can need (as the VapourSynth GPU backend)
    void Prune()
    {
        const auto& dist = m_cadence.Distinct();
        const int64_t front = dist.empty() ? m_last : dist.front();
        for (auto it = m_pairs.begin(); it != m_pairs.end();) {
            if (it->first.first < front && it->second.id != m_lastPair) { m_eng.ReleasePair(it->second.id); it = m_pairs.erase(it); }
            else ++it;
        }
        for (auto it = m_slotOf.begin(); it != m_slotOf.end();) {
            bool used = false;
            for (auto& [key, p] : m_pairs) used = used || key.first == it->first || key.second == it->first;
            if (it->first < front && !used && it->second != m_lastDistinctSlot) { m_eng.ReleaseSlot(it->second); it = m_slotOf.erase(it); }
            else ++it;
        }
        for (auto it = m_thumbs.begin(); it != m_thumbs.end();) { if (it->first < front - 2) it = m_thumbs.erase(it); else ++it; }
        for (auto it = m_holdOf.begin(); it != m_holdOf.end();) {
            if (it->first < front - 1) { it->second.wait(); it = m_holdOf.erase(it); }
            else ++it;
        }
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
            if (m_lastTicket) m_eng.Wait(m_lastTicket, e);
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
    bool MakePipesOnce(std::string& e) { return m_import.p ? true : MakePipes(e); }
    bool MakePipes(std::string& e)
    {
        const VkDescriptorType CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, SB = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        if (!MakePipe(m_import, kSpv_pme_import, sizeof(kSpv_pme_import), { CIS, CIS, CIS, SI, SI, SI }, 32, e)) return false;
        if (!MakePipe(m_export, kSpv_pme_export, sizeof(kSpv_pme_export), { SB, SI, SI, SI }, 52, e)) return false;
        if (!MakePipe(m_thumbPipe, kSpv_pme_thumb, sizeof(kSpv_pme_thumb), { CIS, SB }, 20, e)) return false;
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
    // Dedup / Cadence / Subtitle Hold (the VapourSynth GPU backend's scheduling)
    Cadence m_cadence;
    bool m_dedup = false, m_holdOn = false;
    int64_t m_last = -1000000, m_prevDistinct = -1;
    int m_lastDistinctSlot = -1, m_lastPair = -1;
    std::pair<int64_t, int64_t> m_lastPairKey{ -1, -1 };
    std::map<int64_t, int> m_slotOf;
    std::map<std::pair<int64_t, int64_t>, PairInfo> m_pairs;
    SubtitleHoldYuv m_hold;
    std::shared_future<std::shared_ptr<const HoldFrameYuv>> m_holdChain;
    std::map<int64_t, std::shared_future<std::shared_ptr<const HoldFrameYuv>>> m_holdOf;
    uint64_t m_dups = 0, m_holds = 0, m_heldPairs = 0, m_flashes = 0, m_linear = 0;
    // transition rules (dissolves / fades blended, flashes held) on GPU thumbnails read back, a few KB per frame
    bool m_transitions = false;
    TransitionRules m_rules;
    std::array<ThumbBuf, 12> m_thumbRing;
    size_t m_nextThumb = 0;
    std::map<int64_t, Thumb> m_thumbs;
    uint64_t m_seq = 0, m_written = 0, m_generated = 0, m_cuts = 0, m_lastTicket = 0;
    std::deque<Item> m_q;
    Pipe m_import, m_export, m_thumbPipe;
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
