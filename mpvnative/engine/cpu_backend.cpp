// CpuBackend: PushFPS's portable engine (vsplugin/engines/portable) behind pmeng. Planar YUV 4:2:0 goes in as it is;
// NV12 / P010 (the formats hardware frames download to) are de-interleaved into scratch planes, and the output is
// written back in the caller's format. The multiplier is the VapourSynth route's rule (backend.h DecidePortableK: the
// display's multiple within the CPU budget this machine measures); smoother=N fixes it.
// Dedup (as the GPU routes, vsplugin/engines/gpu/cadence.h, with the same rules): a frame that repeats the last distinct
// picture (PushFPS's duplicate test: no 4x4 block of the quarter-size luma changed by 3 codes, mean under 0.8) is not
// given to the engine; when the next distinct frame comes (within the lookahead of 3: twos, threes, fours), the outputs
// of the whole held span are made from that one pair at their own phases, so a held drawing moves smoothly over its
// span. A longer run is a still: shown as it is, only its last interval moves. Dedup=0 / dedup=no turns it off.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>
#include "backend.h"
#include "pushmpc/pushcore_abi.h"
static_assert(pmvs::kPortableMaxK == int(PUSHCORE_MAX_MULTIPLIER), "the portable ceiling follows PUSHCORE_MAX_MULTIPLIER");
#include "engine.h"
#include "platform.h"
#include "portable_engine.h"

namespace pme {
namespace {

using namespace pmvs;

portable::Calibration CalibrateCached(int w, int h, int bits, int threads, int quality)
{
    static std::mutex m;
    static std::map<std::tuple<int, int, int, int, int>, portable::Calibration> cache;
    std::lock_guard<std::mutex> lk(m);
    const auto key = std::make_tuple(w, h, bits, threads, quality);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    const portable::Calibration c = portable::Calibrate(w, h, bits, threads, quality);
    cache[key] = c;
    return c;
}

bool Is10(uint32_t f) { return f == PMENG_FMT_YUV420P10 || f == PMENG_FMT_P010; }
bool IsSemi(uint32_t f) { return f == PMENG_FMT_NV12 || f == PMENG_FMT_P010; }

class CpuBackend : public Backend {
public:
    bool Init(const pmeng_stream& st, const Options& o, pmeng_output& out, std::string& why)
    {
        m_fmt = st.fmt;
        if (m_fmt != PMENG_FMT_YUV420P && m_fmt != PMENG_FMT_YUV420P10 && m_fmt != PMENG_FMT_NV12 && m_fmt != PMENG_FMT_P010) {
            why = "the CPU engine takes YUV 4:2:0 8 / 10 bit (planar, NV12, P010)";
            return false;
        }
        m_w = int(st.width); m_h = int(st.height);
        m_bits = Is10(m_fmt) ? 10 : 8;
        PortableSettings s = LoadPortableSettings([](const char* n) { return ReadSetting(n); });
        if (!Opt(o, "smoother").empty()) s.smoother = ParseSmoother(Opt(o, "smoother"), s.smoother);
        if (!Opt(o, "portable_quality").empty()) s.quality = Opt(o, "portable_quality") == "fast" ? 0 : 1;
        s.quality = s.quality < 0 ? 1 : std::min(s.quality, 1);   // Auto: balanced (16 px vectors), as the VapourSynth route; the CPU engine has no high
        if (!Opt(o, "portable_threads").empty()) { const int t = std::atoi(Opt(o, "portable_threads").c_str()); s.threads = t > 0 && t <= 64 ? t : 0; }
        if (!Opt(o, "enable").empty()) s.enable = ParseBool(Opt(o, "enable"), s.enable);
        const double srcFps = st.fps_num > 0 && st.fps_den > 0 ? double(st.fps_num) / st.fps_den : 0;
        const double displayHz = std::atof(Opt(o, "display_fps", "0").c_str());

        const portable::Calibration cal = CalibrateCached(m_w, m_h, m_bits, s.threads, s.quality);
        const PortableK pk = s.enable ? DecidePortableK(s.smoother, srcFps, displayHz, m_h, cal.meMs, cal.genMs, false, -1, VrrActive(Opt(o, "display_fs").empty() ? -1 : std::atoi(Opt(o, "display_fs").c_str())))   // vrr_probe.h
                                      : PortableK{ 1, 0, 0, "PushMPC is off (Enable = 0)" };
        m_k = std::clamp(pk.k, 1, kPortableMaxK);
        m_dedup = m_k > 1 && ParseBool(Opt(o, "dedup", ReadSetting("Dedup")), true);
        portable::Config c; c.width = m_w; c.height = m_h; c.bits = m_bits; c.threads = s.threads; c.quality = s.quality;
        if (!m_eng.Configure(c, why)) return false;
        if (IsSemi(m_fmt)) {
            const size_t bps = m_bits > 8 ? 2 : 1;
            m_strideY = size_t(m_w) * bps; m_strideC = size_t((m_w + 1) / 2) * bps;
            m_y.resize(m_strideY * m_h); m_u.resize(m_strideC * ((m_h + 1) / 2)); m_v.resize(m_u.size());
            m_oy.resize(m_y.size()); m_ou.resize(m_u.size()); m_ov.resize(m_v.size());
        }
        out.mem = PMENG_MEM_CPU;
        out.fmt = m_fmt;
        out.width = st.width; out.height = st.height;
        out.multiplier = uint32_t(m_k);
        out.range = st.range; out.matrix = st.matrix; out.primaries = st.primaries; out.transfer = st.transfer;
        out.keep_dynamic_hdr = 1;
        std::snprintf(out.engine, sizeof(out.engine), "portable-cpu");
        std::snprintf(out.description, sizeof(out.description), "PushFPS portable engine (CPU, %d threads, quality %s), %dx%d %d-bit; %s",
                      m_eng.Threads(), s.quality ? "balanced" : "fast", m_w, m_h, m_bits, pk.why.c_str());
        return true;
    }

    int Submit(const pmeng_image& src, int64_t, uint32_t flags) override
    {
        if (!m_q.empty()) return PMENG_BUSY;
        if (src.mem != PMENG_MEM_CPU || src.fmt != m_fmt) { err = "CPU engine: unexpected input memory / format"; return PMENG_ERROR; }
        portable::YuvView v;
        v.width = m_w; v.height = m_h; v.bits = m_bits;
        const int ch = (m_h + 1) / 2, cw = (m_w + 1) / 2;
        if (!IsSemi(m_fmt)) {
            for (int i = 0; i < 3; ++i) { v.data[i] = src.data[i]; v.stride[i] = src.linesize[i]; }
        } else if (m_bits == 8) {
            for (int r = 0; r < m_h; ++r) std::memcpy(&m_y[r * m_strideY], src.data[0] + r * src.linesize[0], size_t(m_w));
            for (int r = 0; r < ch; ++r) {
                const uint8_t* p = src.data[1] + r * src.linesize[1];
                uint8_t* u = &m_u[r * m_strideC]; uint8_t* vv = &m_v[r * m_strideC];
                for (int x = 0; x < cw; ++x) { u[x] = p[2 * x]; vv[x] = p[2 * x + 1]; }
            }
            Planes(v);
        } else {
            for (int r = 0; r < m_h; ++r) {
                const uint16_t* p = reinterpret_cast<const uint16_t*>(src.data[0] + r * src.linesize[0]);
                uint16_t* y = reinterpret_cast<uint16_t*>(&m_y[r * m_strideY]);
                for (int x = 0; x < m_w; ++x) y[x] = uint16_t(p[x] >> 6);
            }
            for (int r = 0; r < ch; ++r) {
                const uint16_t* p = reinterpret_cast<const uint16_t*>(src.data[1] + r * src.linesize[1]);
                uint16_t* u = reinterpret_cast<uint16_t*>(&m_u[r * m_strideC]);
                uint16_t* vv = reinterpret_cast<uint16_t*>(&m_v[r * m_strideC]);
                for (int x = 0; x < cw; ++x) { u[x] = uint16_t(p[2 * x] >> 6); vv[x] = uint16_t(p[2 * x + 1] >> 6); }
            }
            Planes(v);
        }
        const bool contiguous = !(flags & PMENG_SUBMIT_DISCONTINUITY) && m_seq > 0;
        if (!contiguous) { m_eng.Forget(); m_pending.clear(); m_still = false; m_haveDistinct = false; }
        const uint64_t s = m_seq++;
        // Dedup: the quarter-size luma against the last distinct frame's
        bool dup = false;
        if (m_dedup) {
            Quarter(v, m_cur);
            dup = m_haveDistinct && Duplicate(m_lastQ, m_cur);
        }
        if (dup) {
            ++m_dups;
            m_pending.push_back(s);
            // a run longer than the lookahead is a still: its frames are shown as they are (the last distinct picture)
            if (!m_still && m_pending.size() > size_t(kLookahead)) m_still = true;
            if (m_still) { for (uint64_t p : m_pending) HoldPosition(p); m_pending.clear(); }
            return PMENG_OK;
        }
        if (!m_eng.Push(v, contiguous, err)) return PMENG_ERROR;
        if (m_eng.HavePair() && m_eng.PairIsCut()) ++m_cuts;
        if (m_dedup) { std::swap(m_lastQ, m_cur); m_haveDistinct = true; }
        if (!m_eng.HavePair()) { m_q.push_back({ { s, 0, uint32_t(m_k), 0 }, -1.f }); m_pending.clear(); m_still = false; return PMENG_OK; }
        // the outputs from the last distinct frame d0 to this one: g source intervals (the held span), each output at its
        // own phase; after a still only the last interval moves
        const uint64_t g = m_still ? 1 : uint64_t(m_pending.size()) + 1, d0 = s - g;
        for (uint64_t p = d0 + 1; p <= s; ++p) {
            for (int j = 1; j < m_k; ++j) {
                const double tau = double(p - 1 - d0) + double(j) / double(m_k);
                m_q.push_back({ { p, uint32_t(j), uint32_t(m_k), 1 }, float(tau / double(g)) });
            }
            if (p < s) m_q.push_back({ { p, 0, uint32_t(m_k), 1 }, float(double(p - d0) / double(g)) });
            else m_q.push_back({ { p, 0, uint32_t(m_k), 0 }, -1.f });
        }
        m_pending.clear(); m_still = false;
        return PMENG_OK;
    }
    int Finish() override
    {
        for (uint64_t p : m_pending) HoldPosition(p);   // the end of the stream: what waited for a next drawing is shown as it is
        m_pending.clear();
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
        const Item item = m_q.front();
        const pmeng_out_info oi = item.info;
        m_q.pop_front();
        if (dst.mem != PMENG_MEM_CPU || dst.fmt != m_fmt) { err = "CPU engine: unexpected output memory / format"; return PMENG_ERROR; }
        portable::YuvOut o;
        if (!IsSemi(m_fmt)) {
            for (int i = 0; i < 3; ++i) { o.data[i] = dst.data[i]; o.stride[i] = dst.linesize[i]; }
        } else {
            o.data[0] = m_oy.data(); o.data[1] = m_ou.data(); o.data[2] = m_ov.data();
            o.stride[0] = ptrdiff_t(m_strideY); o.stride[1] = o.stride[2] = ptrdiff_t(m_strideC);
        }
        // t < 0: the current real picture (a real frame, or a held one: the last distinct frame); else the pair at t
        if (item.t > 0.f && item.t < 1.f && m_eng.HavePair()) { m_eng.Generate(item.t, o); ++m_generated; }
        else m_eng.Current(o);
        if (IsSemi(m_fmt)) Interleave(dst);
        ++m_written;
        return PMENG_OK;
    }
    int Skip() override { if (!m_q.empty()) m_q.pop_front(); return PMENG_OK; }
    uint32_t Pending() override { return uint32_t(m_q.size()); }
    int Flush() override { m_q.clear(); m_eng.Forget(); m_pending.clear(); m_still = false; m_haveDistinct = false; return PMENG_OK; }
    std::string Stats() override
    {
        char b[256];
        const auto& s = m_eng.Stat();
        std::snprintf(b, sizeof(b), "portable-cpu: %llu sources (%llu repeats, Dedup %s), %llu outputs (%llu generated), %llu scene cuts; motion %.2f ms, each generated frame %.2f ms",
                      (unsigned long long)m_seq, (unsigned long long)m_dups, m_dedup ? "on" : "off", (unsigned long long)m_written, (unsigned long long)m_generated, (unsigned long long)s.cuts, s.meMs, s.genMs);
        return b;
    }

private:
    static constexpr int kLookahead = 3;   // twos, threes and fours (HeldFrames.h kLookahead)
    struct Item { pmeng_out_info info; float t; };   // t < 0: the current real picture
    // a held position: all its outputs show the last distinct picture
    void HoldPosition(uint64_t p)
    {
        for (int j = 1; j < m_k; ++j) m_q.push_back({ { p, uint32_t(j), uint32_t(m_k), 1 }, -1.f });
        m_q.push_back({ { p, 0, uint32_t(m_k), 1 }, -1.f });
    }
    // PushFPS's duplicate test (portable Dedup.h compareForDuplicate): the luma at quarter size (4x4 means, 8 bits)
    void Quarter(const portable::YuvView& v, std::vector<uint8_t>& q) const
    {
        const int qw = m_w / 4, qh = m_h / 4;
        q.resize(size_t(qw) * qh);
        const int shift = m_bits > 8 ? m_bits - 8 : 0;
        for (int y = 0; y < qh; ++y)
            for (int x = 0; x < qw; ++x) {
                uint32_t s = 0;
                for (int j = 0; j < 4; ++j) {
                    const uint8_t* row = v.data[0] + (y * 4 + j) * v.stride[0];
                    for (int i = 0; i < 4; ++i) s += m_bits > 8 ? uint32_t(reinterpret_cast<const uint16_t*>(row)[x * 4 + i] >> shift) : row[x * 4 + i];
                }
                q[size_t(y) * qw + x] = uint8_t((s + 8) / 16);
            }
    }
    bool Duplicate(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) const
    {
        const int qw = m_w / 4, qh = m_h / 4;
        if (a.size() != b.size() || !qw || !qh) return false;
        double total = 0; float worst = 0;
        for (int by = 0; by < (qh + 3) / 4; ++by)
            for (int bx = 0; bx < (qw + 3) / 4; ++bx) {
                int sum = 0, n = 0;
                for (int y = by * 4; y < std::min(by * 4 + 4, qh); ++y)
                    for (int x = bx * 4; x < std::min(bx * 4 + 4, qw); ++x) { sum += std::abs(int(a[size_t(y) * qw + x]) - int(b[size_t(y) * qw + x])); ++n; }
                total += sum; worst = std::max(worst, float(sum) / float(n));
            }
        return worst < 3.0f && total / double(a.size()) < 0.8;   // PushFPS's DedupSettings
    }
    void Planes(portable::YuvView& v)
    {
        v.data[0] = m_y.data(); v.data[1] = m_u.data(); v.data[2] = m_v.data();
        v.stride[0] = ptrdiff_t(m_strideY); v.stride[1] = v.stride[2] = ptrdiff_t(m_strideC);
    }
    void Interleave(const pmeng_image& dst)
    {
        const int ch = (m_h + 1) / 2, cw = (m_w + 1) / 2;
        if (m_bits == 8) {
            for (int r = 0; r < m_h; ++r) std::memcpy(dst.data[0] + r * dst.linesize[0], &m_oy[r * m_strideY], size_t(m_w));
            for (int r = 0; r < ch; ++r) {
                uint8_t* p = dst.data[1] + r * dst.linesize[1];
                const uint8_t* u = &m_ou[r * m_strideC]; const uint8_t* v = &m_ov[r * m_strideC];
                for (int x = 0; x < cw; ++x) { p[2 * x] = u[x]; p[2 * x + 1] = v[x]; }
            }
        } else {
            for (int r = 0; r < m_h; ++r) {
                uint16_t* p = reinterpret_cast<uint16_t*>(dst.data[0] + r * dst.linesize[0]);
                const uint16_t* y = reinterpret_cast<const uint16_t*>(&m_oy[r * m_strideY]);
                for (int x = 0; x < m_w; ++x) p[x] = uint16_t(y[x] << 6);
            }
            for (int r = 0; r < ch; ++r) {
                uint16_t* p = reinterpret_cast<uint16_t*>(dst.data[1] + r * dst.linesize[1]);
                const uint16_t* u = reinterpret_cast<const uint16_t*>(&m_ou[r * m_strideC]);
                const uint16_t* v = reinterpret_cast<const uint16_t*>(&m_ov[r * m_strideC]);
                for (int x = 0; x < cw; ++x) { p[2 * x] = uint16_t(u[x] << 6); p[2 * x + 1] = uint16_t(v[x] << 6); }
            }
        }
    }

    portable::Engine m_eng;
    uint32_t m_fmt = 0;
    int m_w = 0, m_h = 0, m_bits = 8, m_k = 1;
    uint64_t m_seq = 0, m_written = 0, m_generated = 0, m_cuts = 0, m_dups = 0;
    std::deque<Item> m_q;
    bool m_dedup = false, m_still = false, m_haveDistinct = false;
    std::deque<uint64_t> m_pending;                // repeats waiting for the next distinct frame
    std::vector<uint8_t> m_lastQ, m_cur;           // quarter-size luma: the last distinct frame, the new one
    size_t m_strideY = 0, m_strideC = 0;
    std::vector<uint8_t> m_y, m_u, m_v, m_oy, m_ou, m_ov;
};

} // namespace

Backend* OpenCpu(const pmeng_stream& st, const Options& o, pmeng_output& out, std::string& why)
{
    auto* b = new CpuBackend();
    if (!b->Init(st, o, out, why)) { delete b; return nullptr; }
    return b;
}

} // namespace pme
