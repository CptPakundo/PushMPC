// SPDX-License-Identifier: GPL-3.0-or-later
// PushMPC capture mode on the portable GPU engine (cap_gpu.h).
#include "cap_gpu.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#ifndef _WIN32
#include <sys/stat.h>
#endif
#include "backend.h"     // DecideUpscale
#include "platform.h"    // the settings
#include "pmc_rgb2yuv.h" // kSpv_pmc_rgb2yuv (shaders/rgb2yuv.comp)

namespace pmc {
namespace {
struct ConvPush { int plane, w, h, srcW, srcH, srcPitch, srcOff, dstWordOff, nwords, rgbx, rgb10, bits; };
}

GpuPipeline::~GpuPipeline()
{
    if (!m_vk) return;
    std::string e;
    for (auto& p : m_pending) { if (m_eng) m_eng->Wait(p.ticket, e); if (p.release) p.release(); }
    m_pending.clear();
    for (auto& u : m_up) { if (m_eng && u.ticket) m_eng->Wait(u.ticket, e); m_vk->Free(u.staging); }
    if (m_eng) {
        for (auto& x : m_q) { if (x.pair >= 0) m_eng->ReleasePair(x.pair); if (x.slot >= 0) m_eng->ReleaseSlot(x.slot); }
    }
    m_q.clear();
    {
        std::lock_guard<std::mutex> lk(m_vk->qlock);
        vkQueueWaitIdle(m_vk->queue);
    }
    for (auto& d : m_dmabufs) m_vk->Free(d.second);
    m_eng.reset();
    if (m_dpool) vkDestroyDescriptorPool(m_vk->dev, m_dpool, nullptr);
    m_vk->Free(m_conv);
}

bool GpuPipeline::Open(Vk& vk, const GpuRequest& r, std::string& desc, std::string& err)
{
    m_vk = &vk; m_w = r.w; m_h = r.h; m_hostOut = r.hostOut; m_bits = r.bits; m_pq = r.pq;
    std::string why;
    auto ctx = vk.EngineContext(why);
    if (!ctx) { err = "the GPU engine cannot run on " + vk.name + ": " + why; return false; }
    const pmvs::PortableSettings s = pmvs::LoadPortableSettings([](const char* n) { return pmvs::ReadSetting(n); });
    const pmvs::UpscaleChoice uc = pmvs::DecideUpscale(r.upscaler.empty() ? s.upscaler : r.upscaler, "screen", r.outW, r.outH, r.w, r.h);
    pmvs::gpu::Config c;
    c.width = r.w; c.height = r.h; c.bits = r.bits;   // 10: the engine's 16-bit planes (as P010 / 10-bit video in mpv)
    if (uc.kind != pmvs::GpuUpscaler::None) {
        c.outWidth = uc.w; c.outHeight = uc.h;
        c.upscaler = uc.kind == pmvs::GpuUpscaler::Nis ? pmvs::gpu::Upscaler::Nis : pmvs::gpu::Upscaler::Fsr;
        c.sharpness = float(uc.kind == pmvs::GpuUpscaler::Nis ? pmvs::kDefaultSharpnessNis : pmvs::kDefaultSharpnessFsr);
    }
    c.quality = s.quality == 0 ? 1 : 2;
    c.deviceReadback = !r.hostOut;
    c.sourceSlots = 8; c.outSlots = 6;
    // motion: the plugin's rule (portable_vs.cpp): Auto = hybrid where the device has NVIDIA optical flow (the flow is one
    // more block-matcher candidate and the block matcher's SAD picks: measured per block, every pair), else the block
    // matcher; PortableMotion blocks / nvof / hybrid choose by hand
    const bool hybrid = s.motion == "hybrid" || s.motion == "auto" || s.motion.empty();
    c.blockMatcherOnly = s.motion != "nvof" && !hybrid;
    c.nvofHybrid = hybrid;
    m_eng = std::make_shared<pmvs::gpu::Engine>();
    if (!m_eng->Configure(c, ctx, err)) { err = "GPU engine: " + err; m_eng.reset(); return false; }
    m_outW = m_eng->OutWidth(); m_outH = m_eng->OutHeight();
    if (!vk.Pipe(kSpv_pmc_rgb2yuv, sizeof(kSpv_pmc_rgb2yuv), { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER }, sizeof(ConvPush), m_conv, err)) return false;
    VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6 };
    VkDescriptorPoolCreateInfo di{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    di.maxSets = 3; di.poolSizeCount = 1; di.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(vk.dev, &di, nullptr, &m_dpool) != VK_SUCCESS) { err = "vkCreateDescriptorPool failed"; return false; }
    for (auto& u : m_up) {
        VkDescriptorSetAllocateInfo sa{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        sa.descriptorPool = m_dpool; sa.descriptorSetCount = 1; sa.pSetLayouts = &m_conv.dsl;
        if (vkAllocateDescriptorSets(vk.dev, &sa, &u.set) != VK_SUCCESS) { err = "vkAllocateDescriptorSets failed"; return false; }
    }
    char b[512];
    std::snprintf(b, sizeof(b), "the portable GPU engine on %s: %s, the cut guard on the GPU, %dx%d%s%s", vk.name.c_str(),
                  !m_eng->OpticalFlowActive() ? "PushFPS's block matcher" : c.nvofHybrid ? "the block matcher with NVIDIA optical flow as a candidate (hybrid)" : "NVIDIA optical flow",
                  r.w, r.h,
                  uc.kind != pmvs::GpuUpscaler::None ? ", upscaled " : "", uc.kind != pmvs::GpuUpscaler::None ? uc.why.c_str() : "");
    desc = b;
    return true;
}

bool GpuPipeline::RecordConvert(VkCommandBuffer cb, const pmvs::gpu::Engine::PlanarDesc& d, VkBuffer src, VkDeviceSize srcBytes, int pitchPx, int offPx, int w, int h, PixFmt fmt,
                                Upload& u, std::string& err)
{
    if (d.bps != (m_bits > 8 ? 2 : 1)) { err = "GPU engine: unexpected staging depth"; return false; }
    const int per = d.bps == 2 ? 2 : 4;   // samples per 32-bit word
    if ((d.offU & 3) || (d.offV & 3)) { err = "GPU engine: unaligned planes"; return false; }
    VkDescriptorBufferInfo bs{ src, 0, srcBytes ? srcBytes : VK_WHOLE_SIZE }, bd{ d.buffer, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet wr[2]{};
    for (int i = 0; i < 2; ++i) { wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[i].dstSet = u.set; wr[i].dstBinding = uint32_t(i); wr[i].descriptorCount = 1; wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; }
    wr[0].pBufferInfo = &bs; wr[1].pBufferInfo = &bd;
    vkUpdateDescriptorSets(m_vk->dev, 2, wr, 0, nullptr);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m_conv.p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, m_conv.pl, 0, 1, &u.set, 0, nullptr);
    for (int plane = 0; plane < 3; ++plane) {
        const int pw = plane ? d.cw : d.width, ph = plane ? d.ch : d.height;
        const VkDeviceSize off = plane == 0 ? 0 : plane == 1 ? d.offU : d.offV;
        ConvPush p{ plane, pw, ph, std::min(w, d.width), std::min(h, d.height), pitchPx, offPx, int(off / 4), int((int64_t(pw) * ph + per - 1) / per),
                    fmt == PixFmt::RGBX ? 1 : 0, fmt == PixFmt::RGB10 ? 1 : fmt == PixFmt::XBGR10 ? 2 : 0, m_bits };
        vkCmdPushConstants(cb, m_conv.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        const uint32_t groups = uint32_t((p.nwords + 63) / 64);
        const uint32_t gx = std::min<uint32_t>(groups, 32768u), gy = (groups + gx - 1) / gx;
        vkCmdDispatch(cb, gx, gy, 1);
    }
    return true;
}

bool GpuPipeline::Add(uint64_t idx, bool discontinuity, SourceFrame& f, std::string& err)
{
    Trim();
    Upload& u = m_up[m_upNext];
    m_upNext = (m_upNext + 1) % 3;
    if (u.ticket && !m_eng->Wait(u.ticket, err)) return false;
    VkBuffer src = VK_NULL_HANDLE;
    VkDeviceSize srcBytes = 0;
    int pitchPx = 0, offPx = 0;
    bool held = false;
#ifndef _WIN32
    if (f.dmabufFd >= 0 && m_vk->dmabuf && !m_dmabufFailed && (f.pitch & 3) == 0 && (f.dmabufOffset & 3) == 0) {
        uint64_t key = f.dmabufInode;
        if (!key) { struct stat st {}; if (fstat(f.dmabufFd, &st) == 0) key = uint64_t(st.st_ino); }
        auto it = m_dmabufs.find(key);
        if (it == m_dmabufs.end()) {
            VBuf b;
            std::string e;
            if (key && m_vk->ImportDmabuf(f.dmabufFd, f.dmabufSize, b, e)) it = m_dmabufs.emplace(key, b).first;
            else { m_dmabufFailed = true; std::fprintf(stderr, "pushmpc-capture: the GPU cannot read the shared picture buffers directly (%s): copying them\n", e.c_str()); }
        }
        if (it != m_dmabufs.end()) {
            src = it->second.b; srcBytes = it->second.size;
            pitchPx = int(f.pitch / 4); offPx = int(f.dmabufOffset / 4);
            held = true;
        }
    }
#endif
    if (!src) {
        if (!f.ptr) { f.Release(); err = "the captured picture is not readable"; return false; }
        const VkDeviceSize need = VkDeviceSize(m_w) * VkDeviceSize(m_h) * 4;
        if (u.staging.size < need) {
            m_vk->Free(u.staging);
            if (!m_vk->Buffer(need, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, u.staging, err)) { f.Release(); return false; }
        }
        uint8_t* dst = static_cast<uint8_t*>(u.staging.map);
        const int w = std::min(m_w, f.width), h = std::min(m_h, f.height);
        for (int y = 0; y < h; ++y) std::memcpy(dst + size_t(y) * size_t(m_w) * 4, f.ptr + ptrdiff_t(y) * f.pitch, size_t(w) * 4);
        f.Release();
        src = u.staging.b; srcBytes = need; pitchPx = m_w; offPx = 0;
        ++m_hostFrames;
    } else {
        ++m_dmabufFrames;
    }
    const PixFmt fmt = f.fmt;
    const int fw = f.width, fh = f.height;
    uint64_t ticket = 0;
    const int slot = m_eng->UploadWith([&](VkCommandBuffer cb, const pmvs::gpu::Engine::PlanarDesc& d, std::string& e) {
        return RecordConvert(cb, d, src, srcBytes, pitchPx, offPx, fw, fh, fmt, u, e);
    }, ticket, err);
    u.ticket = ticket;
    if (held) m_pending.push_back({ ticket, std::move(f.release) });
    f.release = nullptr;
    if (slot < 0) return false;
    Entry e;
    e.idx = idx; e.disc = discontinuity; e.slot = slot;
    if (!discontinuity && !m_q.empty() && m_q.back().slot >= 0) {
        e.pair = m_eng->EstimateBegin(m_q.back().slot, slot, m_q.back().pair, err);
        if (e.pair < 0) { m_eng->ReleaseSlot(slot); return false; }
    }
    m_q.push_back(e);
    return true;
}

void GpuPipeline::Trim()
{
    std::string e;
    while (m_q.size() > 4) {
        Entry& a = m_q.front();
        if (a.pair >= 0) { if (!a.estimated) { pmvs::gpu::PairStats ps; m_eng->EstimateEnd(a.pair, ps, e); } m_eng->ReleasePair(a.pair); }
        if (m_q.size() > 1 && m_q[1].pair >= 0) {   // the next pair reads this frame
            Entry& b = m_q[1];
            if (!b.estimated) { pmvs::gpu::PairStats ps; m_eng->EstimateEnd(b.pair, ps, e); }
            m_eng->ReleasePair(b.pair);
            b.pair = -1;
        }
        if (a.slot >= 0) m_eng->ReleaseSlot(a.slot);
        m_q.pop_front();
    }
}

void GpuPipeline::Poll()
{
    while (!m_pending.empty() && m_eng && m_eng->Done(m_pending.front().ticket)) {
        if (m_pending.front().release) m_pending.front().release();
        m_pending.pop_front();
    }
}

std::vector<uint64_t> GpuPipeline::Frames() const
{
    std::vector<uint64_t> f;
    for (auto& e : m_q) f.push_back(e.idx);
    return f;
}

std::vector<uint64_t> GpuPipeline::Discontinuities() const
{
    std::vector<uint64_t> f;
    for (auto& e : m_q) if (e.disc || e.pair < 0) f.push_back(e.idx);
    return f;
}

GpuPipeline::Entry* GpuPipeline::Find(uint64_t idx)
{
    for (auto& e : m_q) if (e.idx == idx) return &e;
    return nullptr;
}

bool GpuPipeline::Settle(Entry& e, std::string& err)
{
    if (e.pair < 0 || e.estimated) return true;
    pmvs::gpu::PairStats ps;
    if (!m_eng->EstimateEnd(e.pair, ps, err)) return false;
    e.estimated = true;
    e.cut = m_guard.Decide(ps) != 0;
    if (e.cut) ++m_cuts;
    return true;
}

bool GpuPipeline::Show(const Pick& p, Screen* screen, I420* host, std::string& err)
{
    Entry* a = Find(p.a);
    if (!a) { err = "frame gone"; return false; }
    int out = -1;
    uint64_t tk = 0;
    bool gen = false;
    if (p.generated()) {
        Entry* b = Find(p.b);
        if (b && b->pair >= 0) {
            if (!Settle(*b, err)) return false;
            if (b->cut) a = p.t < 0.5f ? a : b;   // never morph a cut: the nearer real frame
            else gen = true;
        }
        if (gen) { if (!m_eng->Generate(b->pair, p.t, 0, out, tk, err)) return false; ++m_generated; }
    }
    if (!gen && !m_eng->Real(a->slot, out, tk, err)) return false;
    if (host) {
        if (!m_eng->Wait(tk, err)) return false;
        if (!host->Same(m_outW, m_outH, m_bits)) host->Resize(m_outW, m_outH, m_bits);
        host->pq = m_pq;
        pmvs::gpu::YuvOut o;
        o.data[0] = host->y.data(); o.data[1] = host->u.data(); o.data[2] = host->v.data();
        o.stride[0] = host->w * host->bps(); o.stride[1] = o.stride[2] = host->cw() * host->bps();
        m_eng->Read(out, o);
        return true;
    }
    pmvs::gpu::ExternalSync sync;
    sync.wait = { screen->AcquireSem() }; sync.waitValue = { 0 };
    sync.signal = { screen->RenderSem() }; sync.signalValue = { 0 };
    uint64_t done = 0;
    if (!m_eng->Export(out, [&](VkCommandBuffer cb, const pmvs::gpu::OutSource& s, std::string& e) {
            YuvLayout L;
            L.offY = s.offset; L.offU = s.offset + s.offU; L.offV = s.offset + s.offV;
            L.pitchY = s.pitchY; L.pitchC = s.pitchC; L.w = s.w; L.h = s.h; L.cw = s.cw; L.ch = s.ch; L.bits = s.bits; L.pq = m_pq;
            return screen->Record(cb, s.buffer, L, e);
        }, sync, done, err)) return false;
    std::shared_ptr<pmvs::gpu::Engine> eng = m_eng;
    screen->SetWait([eng, done] { std::string e; eng->Wait(done, e); });
    return true;
}

std::string GpuPipeline::Stats() const
{
    if (!m_eng) return {};
    const auto& t = m_eng->Times();
    char b[256];
    std::snprintf(b, sizeof(b), "GPU engine: %llu generated, %llu cuts, upload %.2f ms, motion %.2f ms a pair; frames %s",
                  (unsigned long long)m_generated, (unsigned long long)m_cuts, t.uploads ? t.uploadMs / double(t.uploads) : 0.0, t.pairs ? t.meMs / double(t.pairs) : 0.0,
                  m_dmabufFrames ? "read in place (dma-buf)" : "copied from shared memory");
    return b;
}

} // namespace pmc
