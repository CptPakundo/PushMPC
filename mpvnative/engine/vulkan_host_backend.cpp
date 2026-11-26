// SPDX-License-Identifier: GPL-3.0-or-later
// VulkanHostBackend: the portable GPU engine (vsplugin/engines/gpu) on its OWN Vulkan device, for software frames.
// For players whose frames reach the filter in host memory (Plex for Mac renders through Qt / OpenGL, so mpv has no
// Vulkan device to share): upload, motion both ways and PushMPC's cut guard on the GPU, the k - 1 generated frames read
// back; real frames are the source frame itself (copied byte for byte). On macOS the device is MoltenVK (bundled next to
// the module, see vk_context.cpp LoadVulkan). Auto picks it on macOS after the apple engine, before the CPU engine.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>
#include "backend.h"
#include "engine.h"
#include "gpu_backend.h"
#include "gpu_engine.h"
#include "platform.h"
#include "vk_context.h"

namespace pme {
namespace {

using namespace pmvs;
using namespace pmvs::gpu;

bool Ten(uint32_t f) { return f == PMENG_FMT_P010 || f == PMENG_FMT_YUV420P10; }
bool Semi(uint32_t f) { return f == PMENG_FMT_NV12 || f == PMENG_FMT_P010; }

class VulkanHostBackend : public Backend {
public:
    ~VulkanHostBackend() override { ReleaseAll(); }

    bool Init(const pmeng_stream& st, const Options& o, pmeng_output& out, std::string& why)
    {
        m_fmt = st.fmt;
        if (m_fmt != PMENG_FMT_YUV420P && m_fmt != PMENG_FMT_YUV420P10 && m_fmt != PMENG_FMT_NV12 && m_fmt != PMENG_FMT_P010) {
            why = "the Vulkan engine takes YUV 4:2:0 8 / 10 bit (planar, NV12, P010)";
            return false;
        }
        m_w = int(st.width); m_h = int(st.height); m_bits = Ten(m_fmt) ? 10 : 8;
        PortableSettings s = LoadPortableSettings([](const char* n) { return ReadSetting(n); });
        if (!Opt(o, "smoother").empty()) s.smoother = ParseSmoother(Opt(o, "smoother"), s.smoother);
        if (!Opt(o, "enable").empty()) s.enable = ParseBool(Opt(o, "enable"), s.enable);
        Config c; c.width = m_w; c.height = m_h; c.bits = m_bits;
        // the zero-copy backend's settings (vulkan_backend.cpp): 8 px blocks on the full-size picture unless "fast"; motion Auto =
        // hybrid where the device has NVIDIA's optical flow
        const std::string q = Opt(o, "portable_quality", ReadSetting("PortableQuality"));
        c.quality = q == "fast" || q == "0" ? 1 : 2;
        const std::string mo = Opt(o, "motion", ReadSetting("PortableMotion"));
        c.nvofHybrid = mo.empty() || mo == "auto" || mo == "hybrid";
        c.blockMatcherOnly = mo != "nvof" && !c.nvofHybrid;
        DeviceRequest req; req.device = Opt(o, "vulkan_device", "auto"); req.allowCpu = Opt(o, "vulkan_allow_cpu") == "1";
        if (!m_eng.Configure(c, req, why)) { why = "Vulkan engine: " + why; return false; }
        const double srcFps = st.fps_num > 0 && st.fps_den > 0 ? double(st.fps_num) / st.fps_den : 0;
        const double displayHz = std::atof(Opt(o, "display_fps", "0").c_str());
        const GpuCalibration cal = s.enable && s.smoother == 0 ? CalibrateGpu(m_eng) : GpuCalibration{};
        const PortableK pk = s.enable ? DecidePortableK(s.smoother, srcFps, displayHz, m_h, cal.ok ? cal.meMs : 1.0, cal.ok ? cal.genMs : 1.0, true,
                                                       Opt(o, "display_fs") == "1" ? 0.0 : -1.0, VrrActive(Opt(o, "display_fs").empty() ? -1 : std::atoi(Opt(o, "display_fs").c_str())))   // pushmpc-native.lua; vrr_probe.h
                                      : PortableK{ 1, 0, 0, "PushMPC is off (Enable = 0)" };
        m_k = std::clamp(pk.k, 1, 8);
        const size_t bps = m_bits > 8 ? 2 : 1;
        m_sy = size_t(m_w) * bps; m_sc = size_t((m_w + 1) / 2) * bps;
        const size_t ch = size_t((m_h + 1) / 2);
        for (int i = 0; i < 2; ++i) { m_src[i][0].resize(m_sy * m_h); m_src[i][1].resize(m_sc * ch); m_src[i][2].resize(m_sc * ch); }
        m_gen[0].resize(m_sy * m_h); m_gen[1].resize(m_sc * ch); m_gen[2].resize(m_sc * ch);
        out.mem = PMENG_MEM_CPU; out.fmt = m_fmt; out.width = st.width; out.height = st.height;
        out.multiplier = uint32_t(m_k);
        out.range = st.range; out.matrix = st.matrix; out.primaries = st.primaries; out.transfer = st.transfer;
        out.keep_dynamic_hdr = 1;
        std::snprintf(out.engine, sizeof(out.engine), "portable-vulkan");
        std::snprintf(out.description, sizeof(out.description), "PushFPS portable engine on %s (own device, frames in host memory), %dx%d %d-bit; %s",
                      m_eng.Device().Describe().c_str(), m_w, m_h, m_bits, pk.why.c_str());
        return true;
    }

    int Submit(const pmeng_image& src, int64_t, uint32_t flags) override
    {
        if (!m_q.empty()) return PMENG_BUSY;
        if (src.mem != PMENG_MEM_CPU || src.fmt != m_fmt) { err = "Vulkan engine: unexpected input memory / format"; return PMENG_ERROR; }
        const int cur = int(m_seq & 1);
        ToPlanar(src, m_src[cur]);
        YuvView v; v.width = m_w; v.height = m_h; v.bits = m_bits;
        for (int i = 0; i < 3; ++i) { v.data[i] = m_src[cur][i].data(); v.stride[i] = ptrdiff_t(i ? m_sc : m_sy); }
        const bool contiguous = !(flags & PMENG_SUBMIT_DISCONTINUITY) && m_seq > 0 && m_slot >= 0;
        if (!contiguous) ReleaseAll();
        if (m_prevSlot >= 0) { m_eng.ReleaseSlot(m_prevSlot); m_prevSlot = -1; }   // the previous pair's first frame
        const int slot = m_eng.Upload(v, err);
        if (slot < 0) return PMENG_ERROR;
        m_havePair = false;
        if (m_slot >= 0) {
            PairStats ps;
            const int pair = m_eng.Estimate(m_slot, slot, m_pair, ps, err);
            if (pair < 0) { m_eng.ReleaseSlot(slot); return PMENG_ERROR; }
            if (m_pair >= 0) m_eng.ReleasePair(m_pair);
            m_prevSlot = m_slot;   // still read by this pair's outputs
            m_pair = pair; m_havePair = true;
            m_cut = m_guard.Decide(ps) != 0;
            if (m_cut) ++m_cuts;
            for (int j = 1; j < m_k; ++j) m_q.push_back({ m_seq, uint32_t(j), uint32_t(m_k), 1 });
        }
        m_slot = slot;
        m_q.push_back({ m_seq, 0, uint32_t(m_k), 0 });
        ++m_seq;
        return PMENG_OK;
    }
    int Peek(pmeng_out_info& info, uint32_t) override
    {
        if (m_q.empty()) return 0;
        info = m_q.front();
        return 1;
    }
    int Write(const pmeng_image& dst) override
    {
        if (m_q.empty()) { err = "write without an output"; return PMENG_ERROR; }
        const pmeng_out_info oi = m_q.front();
        m_q.pop_front();
        if (dst.mem != PMENG_MEM_CPU || dst.fmt != m_fmt) { err = "Vulkan engine: unexpected output memory / format"; return PMENG_ERROR; }
        const int cur = int((m_seq - 1) & 1), prev = cur ^ 1;
        if (oi.subframe == 0) { FromPlanar(m_src[cur], dst); ++m_written; return PMENG_OK; }
        const float t = float(oi.subframe) / float(oi.multiplier);
        if (m_cut) { FromPlanar(t < 0.5f ? m_src[prev] : m_src[cur], dst); ++m_written; return PMENG_OK; }   // never morph a cut
        int o = -1; uint64_t tk = 0;
        if (!m_eng.Generate(m_pair, t, 0, o, tk, err) || !m_eng.Wait(tk, err)) return PMENG_ERROR;
        YuvOut yo;
        for (int i = 0; i < 3; ++i) { yo.data[i] = m_gen[i].data(); yo.stride[i] = ptrdiff_t(i ? m_sc : m_sy); }
        m_eng.Read(o, yo);
        FromPlanar(m_gen, dst);
        ++m_generated; ++m_written;
        return PMENG_OK;
    }
    int Skip() override { if (!m_q.empty()) m_q.pop_front(); return PMENG_OK; }
    uint32_t Pending() override { return uint32_t(m_q.size()); }
    int Flush() override { m_q.clear(); ReleaseAll(); return PMENG_OK; }
    std::string Stats() override
    {
        char b[320];
        const auto& t = m_eng.Times();
        std::snprintf(b, sizeof(b), "portable-vulkan (%s): %llu sources, %llu outputs (%llu generated), %llu scene cuts; upload %.2f ms, motion %.2f ms a pair",
                      m_eng.Device().name.c_str(), (unsigned long long)m_seq, (unsigned long long)m_written, (unsigned long long)m_generated,
                      (unsigned long long)m_cuts, t.uploads ? t.uploadMs / double(t.uploads) : 0.0, t.pairs ? t.meMs / double(t.pairs) : 0.0);
        return b;
    }

private:
    void ReleaseAll()
    {
        if (m_pair >= 0) m_eng.ReleasePair(m_pair);
        if (m_slot >= 0) m_eng.ReleaseSlot(m_slot);
        if (m_prevSlot >= 0) m_eng.ReleaseSlot(m_prevSlot);
        m_pair = m_slot = m_prevSlot = -1; m_havePair = m_cut = false;
    }
    // the caller's frame -> planar, low-bit samples (the engine's layout)
    void ToPlanar(const pmeng_image& s, std::vector<uint8_t> (&p)[3]) const
    {
        const int ch = (m_h + 1) / 2, cw = (m_w + 1) / 2;
        if (!Semi(m_fmt)) {
            for (int r = 0; r < m_h; ++r) std::memcpy(&p[0][r * m_sy], s.data[0] + r * s.linesize[0], m_sy);
            for (int i = 1; i < 3; ++i) for (int r = 0; r < ch; ++r) std::memcpy(&p[i][r * m_sc], s.data[i] + r * s.linesize[i], m_sc);
        } else if (m_bits == 8) {
            for (int r = 0; r < m_h; ++r) std::memcpy(&p[0][r * m_sy], s.data[0] + r * s.linesize[0], m_sy);
            for (int r = 0; r < ch; ++r) {
                const uint8_t* q = s.data[1] + r * s.linesize[1];
                uint8_t* u = &p[1][r * m_sc]; uint8_t* v = &p[2][r * m_sc];
                for (int x = 0; x < cw; ++x) { u[x] = q[2 * x]; v[x] = q[2 * x + 1]; }
            }
        } else {
            for (int r = 0; r < m_h; ++r) {
                const uint16_t* q = reinterpret_cast<const uint16_t*>(s.data[0] + r * s.linesize[0]);
                uint16_t* y = reinterpret_cast<uint16_t*>(&p[0][r * m_sy]);
                for (int x = 0; x < m_w; ++x) y[x] = uint16_t(q[x] >> 6);
            }
            for (int r = 0; r < ch; ++r) {
                const uint16_t* q = reinterpret_cast<const uint16_t*>(s.data[1] + r * s.linesize[1]);
                uint16_t* u = reinterpret_cast<uint16_t*>(&p[1][r * m_sc]); uint16_t* v = reinterpret_cast<uint16_t*>(&p[2][r * m_sc]);
                for (int x = 0; x < cw; ++x) { u[x] = uint16_t(q[2 * x] >> 6); v[x] = uint16_t(q[2 * x + 1] >> 6); }
            }
        }
    }
    void FromPlanar(const std::vector<uint8_t> (&p)[3], const pmeng_image& d) const
    {
        const int ch = (m_h + 1) / 2, cw = (m_w + 1) / 2;
        if (!Semi(m_fmt)) {
            for (int r = 0; r < m_h; ++r) std::memcpy(d.data[0] + r * d.linesize[0], &p[0][r * m_sy], m_sy);
            for (int i = 1; i < 3; ++i) for (int r = 0; r < ch; ++r) std::memcpy(d.data[i] + r * d.linesize[i], &p[i][r * m_sc], m_sc);
        } else if (m_bits == 8) {
            for (int r = 0; r < m_h; ++r) std::memcpy(d.data[0] + r * d.linesize[0], &p[0][r * m_sy], m_sy);
            for (int r = 0; r < ch; ++r) {
                uint8_t* q = d.data[1] + r * d.linesize[1];
                const uint8_t* u = &p[1][r * m_sc]; const uint8_t* v = &p[2][r * m_sc];
                for (int x = 0; x < cw; ++x) { q[2 * x] = u[x]; q[2 * x + 1] = v[x]; }
            }
        } else {
            for (int r = 0; r < m_h; ++r) {
                uint16_t* q = reinterpret_cast<uint16_t*>(d.data[0] + r * d.linesize[0]);
                const uint16_t* y = reinterpret_cast<const uint16_t*>(&p[0][r * m_sy]);
                for (int x = 0; x < m_w; ++x) q[x] = uint16_t(y[x] << 6);
            }
            for (int r = 0; r < ch; ++r) {
                uint16_t* q = reinterpret_cast<uint16_t*>(d.data[1] + r * d.linesize[1]);
                const uint16_t* u = reinterpret_cast<const uint16_t*>(&p[1][r * m_sc]); const uint16_t* v = reinterpret_cast<const uint16_t*>(&p[2][r * m_sc]);
                for (int x = 0; x < cw; ++x) { q[2 * x] = uint16_t(u[x] << 6); q[2 * x + 1] = uint16_t(v[x] << 6); }
            }
        }
    }

    Engine m_eng;
    CutGuard m_guard;
    uint32_t m_fmt = 0;
    int m_w = 0, m_h = 0, m_bits = 8, m_k = 1;
    int m_slot = -1, m_prevSlot = -1, m_pair = -1;
    bool m_havePair = false, m_cut = false;
    uint64_t m_seq = 0, m_written = 0, m_generated = 0, m_cuts = 0;
    size_t m_sy = 0, m_sc = 0;
    std::vector<uint8_t> m_src[2][3], m_gen[3];
    std::deque<pmeng_out_info> m_q;
};

} // namespace

Backend* OpenVulkanHost(const pmeng_stream& st, const Options& o, pmeng_output& out, std::string& why)
{
    auto* b = new VulkanHostBackend();
    if (!b->Init(st, o, out, why)) { delete b; return nullptr; }
    return b;
}

} // namespace pme
