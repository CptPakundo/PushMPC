// SPDX-License-Identifier: GPL-3.0-or-later
// VulkanHostBackend: the portable GPU engine (vsplugin/engines/gpu) on its OWN Vulkan device, for software frames.
// For players whose frames reach the filter in host memory (Plex for Mac renders through Qt / OpenGL, so mpv has no
// Vulkan device to share): upload, motion both ways and PushMPC's cut guard on the GPU, the k - 1 generated frames read
// back; real frames are the source frame itself (copied byte for byte). On macOS the device is MoltenVK (bundled next to
// the module, see vk_context.cpp LoadVulkan). Auto picks it on macOS after the apple engine, before the CPU engine.
// Dedup (as the zero-copy backend and the VapourSynth GPU route, cadence.h): a frame that repeats the last distinct
// picture (animation drawn on twos, threes, fours) gets no pair of its own; the outputs between two distinct pictures
// are spread over the whole held span (lookahead 3 source frames), so a drawing held for several frames moves smoothly
// instead of moving in one step and stopping. Dedup=0 (setting) or dedup=no (option) turns it off.
#include <algorithm>
#include <map>
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
        const PortableK pk = s.enable ? DecidePortableK(s.smoother, srcFps, displayHz, m_h, cal.ok ? cal.meMs : 1.0, cal.ok ? cal.genMs : 1.0, true)
                                      : PortableK{ 1, 0, 0, "PushMPC is off (Enable = 0)" };
        m_k = std::clamp(pk.k, 1, 8);
        m_dedup = m_k > 1 && ParseBool(Opt(o, "dedup", ReadSetting("Dedup")), true);
        m_cadence.Configure(m_k, m_dedup ? kDedupLookahead : 0, 0);
        const size_t bps = m_bits > 8 ? 2 : 1;
        m_sy = size_t(m_w) * bps; m_sc = size_t((m_w + 1) / 2) * bps;
        const size_t ch = size_t((m_h + 1) / 2);
        m_ch = ch;
        m_gen[0].resize(m_sy * m_h); m_gen[1].resize(m_sc * ch); m_gen[2].resize(m_sc * ch);
        out.mem = PMENG_MEM_CPU; out.fmt = m_fmt; out.width = st.width; out.height = st.height;
        out.multiplier = uint32_t(m_k);
        out.range = st.range; out.matrix = st.matrix; out.primaries = st.primaries; out.transfer = st.transfer;
        out.keep_dynamic_hdr = 1;
        std::snprintf(out.engine, sizeof(out.engine), "portable-vulkan");
        std::snprintf(out.description, sizeof(out.description), "PushFPS portable engine on %s (own device, frames in host memory), %dx%d %d-bit; %s; Dedup %s",
                      m_eng.Device().Describe().c_str(), m_w, m_h, m_bits, pk.why.c_str(), m_dedup ? "on" : "off");
        return true;
    }

    int Submit(const pmeng_image& src, int64_t, uint32_t flags) override
    {
        if (!m_q.empty()) return PMENG_BUSY;
        if (src.mem != PMENG_MEM_CPU || src.fmt != m_fmt) { err = "Vulkan engine: unexpected input memory / format"; return PMENG_ERROR; }
        const int64_t s = int64_t(m_seq++);
        const bool contiguous = !(flags & PMENG_SUBMIT_DISCONTINUITY) && m_last == s - 1;
        if (!contiguous) ReleaseAll();
        m_last = s;
        Planes& pl = m_src[s];
        for (int i = 0; i < 3; ++i) pl.p[i].resize(i ? m_sc * m_ch : m_sy * size_t(m_h));
        ToPlanar(src, pl.p);
        YuvView v; v.width = m_w; v.height = m_h; v.bits = m_bits;
        for (int i = 0; i < 3; ++i) { v.data[i] = pl.p[i].data(); v.stride[i] = ptrdiff_t(i ? m_sc : m_sy); }
        const int slot = m_eng.Upload(v, err);
        if (slot < 0) return PMENG_ERROR;
        bool dup = false;
        if (m_dedup && m_lastDistinctSlot >= 0) {
            DedupStats ds;
            if (!m_eng.Duplicate(m_lastDistinctSlot, slot, ds, err)) { m_eng.ReleaseSlot(slot); return PMENG_ERROR; }
            dup = ds.duplicate;
        }
        if (dup) { m_eng.ReleaseSlot(slot); ++m_dups; }
        else {
            m_slotOf[s] = slot;
            m_lastDistinctSlot = slot;
            if (m_prevDistinct >= 0 && !StartPair(m_prevDistinct, s)) return PMENG_ERROR;
            m_prevDistinct = s;
        }
        m_cadence.Push(s, dup);
        Settle();
        Prune();
        return PMENG_OK;
    }
    int Finish() override
    {
        m_cadence.Finish();
        Settle();
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
        if (dst.mem != PMENG_MEM_CPU || dst.fmt != m_fmt) { err = "Vulkan engine: unexpected output memory / format"; return PMENG_ERROR; }
        const Shot& sh = it.shot;
        auto real = [&](int64_t f) -> int {
            auto p = m_src.find(f);
            if (p == m_src.end()) { err = "Vulkan engine: no picture for frame " + std::to_string(f); return PMENG_ERROR; }
            FromPlanar(p->second.p, dst); ++m_written;
            return PMENG_OK;
        };
        if (sh.kind != Shot::Generated) return real(sh.src);
        auto pi = m_pairs.find({ sh.d0, sh.d1 });
        if (pi == m_pairs.end()) { err = "Vulkan engine: the pair is gone"; return PMENG_ERROR; }
        if (pi->second.cut) return real(sh.t < 0.5f ? sh.d0 : sh.d1);   // never morph a cut
        int o = -1; uint64_t tk = 0;
        if (!m_eng.Generate(pi->second.id, sh.t, 0, o, tk, err) || !m_eng.Wait(tk, err)) return PMENG_ERROR;
        YuvOut yo;
        for (int i = 0; i < 3; ++i) { yo.data[i] = m_gen[i].data(); yo.stride[i] = ptrdiff_t(i ? m_sc : m_sy); }
        m_eng.Read(o, yo);
        FromPlanar(m_gen, dst);
        ++m_generated; ++m_written;
        return PMENG_OK;
    }
    int Skip() override { if (!m_q.empty()) m_q.pop_front(); return PMENG_OK; }
    uint32_t Pending() override { return uint32_t(m_q.size()); }
    int Flush() override { m_q.clear(); ReleaseAll(); return PMENG_OK; }    std::string Stats() override
    {
        char b[320];
        const auto& t = m_eng.Times();
        std::snprintf(b, sizeof(b), "portable-vulkan (%s): %llu sources (%llu repeats, Dedup), %llu outputs (%llu generated), %llu scene cuts; upload %.2f ms, motion %.2f ms a pair",
                      m_eng.Device().name.c_str(), (unsigned long long)m_seq, (unsigned long long)m_dups, (unsigned long long)m_written, (unsigned long long)m_generated,
                      (unsigned long long)m_cuts, t.uploads ? t.uploadMs / double(t.uploads) : 0.0, t.pairs ? t.meMs / double(t.pairs) : 0.0);
        return b;
    }

private:
    static constexpr int kDedupLookahead = 3;   // twos, threes and fours (HeldFrames.h kLookahead)
    struct Planes { std::vector<uint8_t> p[3]; };
    struct PairInfo { int id = -1; bool cut = false; };
    struct Item { pmeng_out_info info; Shot shot; };
    void ReleaseAll()
    {
        for (auto& [key, p] : m_pairs) m_eng.ReleasePair(p.id);
        m_pairs.clear();
        for (auto& [f, slot] : m_slotOf) m_eng.ReleaseSlot(slot);
        m_slotOf.clear();
        m_src.clear();
        m_lastDistinctSlot = -1; m_prevDistinct = -1; m_lastPair = -1; m_last = -1000000;
        m_cadence.Reset();
    }
    bool StartPair(int64_t d0, int64_t d1)
    {
        auto a = m_slotOf.find(d0), b = m_slotOf.find(d1);
        if (a == m_slotOf.end() || b == m_slotOf.end()) { err = "Vulkan engine: a picture of the pair is gone"; return false; }
        PairStats ps;
        const int id = m_eng.Estimate(a->second, b->second, m_lastPair, ps, err);
        if (id < 0) return false;
        PairInfo pi; pi.id = id; pi.cut = m_guard.Decide(ps) != 0;
        if (pi.cut) ++m_cuts;
        m_pairs[{ d0, d1 }] = pi;
        m_lastPair = id;
        return true;
    }
    // the outputs Cadence settles now (output n = p k + j: tagged with position p, subframe j, as the zero-copy backend)
    void Settle()
    {
        std::vector<Settled> settled;
        m_cadence.Settle(settled);
        for (const Settled& st : settled) {
            const int64_t pos = st.n / m_k + (st.n % m_k ? 1 : 0);
            pmeng_out_info info{ uint64_t(pos), uint32_t(st.n % m_k), uint32_t(m_k), st.shot.kind == Shot::Real ? 0u : 1u };
            if (st.shot.kind == Shot::Hold) ++m_holds;
            m_q.push_back({ info, st.shot });
        }
    }
    // forget what no later output can need
    void Prune()
    {
        const auto& dist = m_cadence.Distinct();
        const int64_t front = dist.empty() ? m_last : dist.front();
        int64_t oldestQueued = m_last;
        for (const Item& it : m_q) oldestQueued = std::min(oldestQueued, it.shot.kind == Shot::Generated ? it.shot.d0 : it.shot.src);
        const int64_t keep = std::min(front, oldestQueued);
        for (auto it = m_pairs.begin(); it != m_pairs.end();) {
            if (it->first.first < keep && it->second.id != m_lastPair) { m_eng.ReleasePair(it->second.id); it = m_pairs.erase(it); }
            else ++it;
        }
        for (auto it = m_slotOf.begin(); it != m_slotOf.end();) {
            bool used = false;
            for (auto& [key, p] : m_pairs) used = used || key.first == it->first || key.second == it->first;
            if (it->first < keep && !used && it->second != m_lastDistinctSlot) { m_eng.ReleaseSlot(it->second); it = m_slotOf.erase(it); }
            else ++it;
        }
        for (auto it = m_src.begin(); it != m_src.end();) { if (it->first < keep) it = m_src.erase(it); else ++it; }
    }    // the caller's frame -> planar, low-bit samples (the engine's layout)
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
    Cadence m_cadence;
    bool m_dedup = false;
    uint32_t m_fmt = 0;
    int m_w = 0, m_h = 0, m_bits = 8, m_k = 1;
    int m_lastDistinctSlot = -1, m_lastPair = -1;
    int64_t m_prevDistinct = -1, m_last = -1000000;
    uint64_t m_seq = 0, m_written = 0, m_generated = 0, m_cuts = 0, m_dups = 0, m_holds = 0;
    size_t m_sy = 0, m_sc = 0, m_ch = 0;
    std::map<int64_t, Planes> m_src;                         // source pictures still shown (real frames, holds, cut frames)
    std::map<int64_t, int> m_slotOf;                         // distinct frame -> engine slot
    std::map<std::pair<int64_t, int64_t>, PairInfo> m_pairs; // (d0, d1) -> motion pair
    std::vector<uint8_t> m_gen[3];
    std::deque<Item> m_q;
};

} // namespace

Backend* OpenVulkanHost(const pmeng_stream& st, const Options& o, pmeng_output& out, std::string& why)
{
    auto* b = new VulkanHostBackend();
    if (!b->Init(st, o, out, why)) { delete b; return nullptr; }
    return b;
}

} // namespace pme
