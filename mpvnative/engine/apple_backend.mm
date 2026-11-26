// SPDX-License-Identifier: GPL-3.0-or-later
// libpushmpc_engine_apple.dylib: the apple engine (vsplugin/engines/apple: VideoToolbox VTFrameProcessor frame
// interpolation / frame-rate conversion, Metal) behind pmeng, for software frames (Plex for Mac, Homebrew mpv without
// a Vulkan device). A separate module so the engine itself keeps loading on macOS 11 (Plex's minimum) while this part
// needs macOS 13.3 to load and VTFrameProcessor (macOS 15.4+) to run: the engine dlopen()s it from its own folder and
// asks it for a backend; Configure's self-test decides on this Mac (a Mac or VM that lists an effect but cannot run it
// fails here, and Auto moves on to the Vulkan engine, then the CPU engine).
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
#include "apple_engine.h"
#include "engine.h"
#include "held_dedup.h"   // held frames: PushFPS's duplicate test on the quarter-size luma
#include "platform.h"     // ReadSetting("Dedup"), ParseBool

namespace {

using namespace pmvs;

std::string O(const pme::Options& o, const char* k, const std::string& def = std::string())
{
    auto it = o.find(k);
    return it == o.end() || it->second.empty() ? def : it->second;
}
bool Ten(uint32_t f) { return f == PMENG_FMT_P010 || f == PMENG_FMT_YUV420P10; }
bool Semi(uint32_t f) { return f == PMENG_FMT_NV12 || f == PMENG_FMT_P010; }

class AppleBackend : public pme::Backend {
public:
    void (*log)(const char*) = nullptr;

    bool Init(const pmeng_stream& st, const pme::Options& o, pmeng_output& out, std::string& why)
    {
        m_fmt = st.fmt;
        if (m_fmt != PMENG_FMT_YUV420P && m_fmt != PMENG_FMT_YUV420P10 && m_fmt != PMENG_FMT_NV12 && m_fmt != PMENG_FMT_P010) {
            why = "the apple engine takes YUV 4:2:0 8 / 10 bit (planar, NV12, P010)";
            return false;
        }
        const apple::Caps& caps = apple::Probe();
        if (!caps.AnyInterpolation()) { why = "no VideoToolbox interpolator on this Mac: " + caps.detail; return false; }
        m_w = int(st.width); m_h = int(st.height); m_bits = Ten(m_fmt) ? 10 : 8;
        int smoother = 0;
        if (const std::string s = O(o, "smoother"); !s.empty()) smoother = s == "auto" ? 0 : std::atoi(s.c_str());
        const double srcFps = st.fps_num > 0 && st.fps_den > 0 ? double(st.fps_num) / st.fps_den : 23.976;
        const double displayHz = std::atof(O(o, "display_fps", "0").c_str());
        const apple::AppleK ak = apple::DecideAppleK(smoother, srcFps, displayHz);
        if (ak.k <= 1) { why = "x1: nothing for the apple engine to do"; return false; }
        apple::Config c;
        c.width = m_w; c.height = m_h; c.bits = m_bits; c.k = ak.k; c.srcFps = srcFps;
        c.sr = "off";   // pmeng outputs keep the source size (mpv scales)
        c.fi = O(o, "apple_fi", "auto");
        c.colour.matrix = st.matrix; c.colour.transfer = st.transfer; c.colour.primaries = st.primaries;
        c.colour.fullRange = st.range == 2;   // AVCOL_RANGE_JPEG
        m_col = c.colour;
        m_eng.log = [this](const std::string& m) { if (log) log(("apple: " + m).c_str()); };
        std::string info;
        if (!m_eng.Configure(c, why, info)) { why = "apple engine: " + why; return false; }
        m_k = std::max(1, m_eng.K());
        m_dedup = m_k > 1 && ParseBool(O(o, "dedup", ReadSetting("Dedup")), true);   // held frames (Dedup=0 / dedup=no: off)
        const size_t bps = m_bits > 8 ? 2 : 1;
        m_sy = size_t(m_w) * bps; m_sc = size_t((m_w + 1) / 2) * bps;
        const size_t ch = size_t((m_h + 1) / 2);
        m_bufs.resize(size_t(m_k));   // [0] the real frame, [1..k-1] the generated ones
        for (auto& b : m_bufs) { b[0].resize(m_sy * m_h); b[1].resize(m_sc * ch); b[2].resize(m_sc * ch); }
        for (auto& p : m_in) p.clear();
        m_in[0].resize(m_sy * m_h); m_in[1].resize(m_sc * ch); m_in[2].resize(m_sc * ch);
        out.mem = PMENG_MEM_CPU; out.fmt = m_fmt; out.width = st.width; out.height = st.height;
        out.multiplier = uint32_t(m_k);
        out.range = st.range; out.matrix = st.matrix; out.primaries = st.primaries; out.transfer = st.transfer;
        out.keep_dynamic_hdr = 1;
        std::snprintf(out.engine, sizeof(out.engine), "apple");
        std::snprintf(out.description, sizeof(out.description), "VideoToolbox on %s (%s), %dx%d %d-bit; %s; %s",
                      caps.chip.c_str(), caps.os.c_str(), m_w, m_h, m_bits, info.c_str(), ak.why.c_str());
        return true;
    }

    int Submit(const pmeng_image& src, int64_t pts, uint32_t flags) override
    {
        if (!m_q.empty()) return PMENG_BUSY;
        if (src.mem != PMENG_MEM_CPU || src.fmt != m_fmt) { err = "apple engine: unexpected input memory / format"; return PMENG_ERROR; }
        apple::PlanarView v; v.width = m_w; v.height = m_h; v.bits = m_bits;
        if (!Semi(m_fmt)) { for (int i = 0; i < 3; ++i) { v.data[i] = src.data[i]; v.stride[i] = src.linesize[i]; } }
        else { ToPlanar(src); for (int i = 0; i < 3; ++i) { v.data[i] = m_in[i].data(); v.stride[i] = ptrdiff_t(i ? m_sc : m_sy); } }
        const bool contiguous = !(flags & PMENG_SUBMIT_DISCONTINUITY) && m_seq > 0;
        if (!contiguous) { m_pending.clear(); m_still = false; m_haveDistinct = false; }
        const uint64_t s = m_seq;
        // held frames (as cpu_backend.cpp, eagerly): a repeat of the last distinct frame is not processed; the next
        // distinct frame's one pair makes the whole held span (VTFrameRateConversion takes any phase); a run longer than
        // the lookahead is a still (shown as it is, only its last interval moves)
        if (m_dedup) {
            held::QuarterLuma(v.data[0], v.stride[0], m_w, m_h, m_bits > 8 ? 2 : 1, m_bits > 8 ? m_bits - 8 : 0, m_curQ);
            if (contiguous && m_haveDistinct && held::QuarterDuplicate(m_lastQ, m_curQ, m_w, m_h)) {
                ++m_seq;
                ++m_dups;
                m_pending.push_back(s);
                if (!m_still && m_pending.size() > size_t(held::kLookahead)) m_still = true;
                if (m_still) { for (uint64_t p : m_pending) HoldPosition(p); m_pending.clear(); }
                return PMENG_OK;
            }
            std::swap(m_lastQ, m_curQ);
            m_haveDistinct = true;
        }
        const int g = m_still ? 1 : int(m_pending.size()) + 1;   // source intervals from the last distinct frame
        Bufs(size_t(g * m_k));
        std::vector<apple::PlanarOut> gen;
        for (int j = 1; j < g * m_k; ++j) gen.push_back(Out(size_t(j)));
        if (g > 1) m_eng.SetSpan(g);
        bool havePair = false;
        if (!m_eng.Process(v, m_col, pts * 10, contiguous, gen, Out(0), havePair, err)) { err = "apple engine: " + err; return PMENG_ERROR; }
        if (havePair) {
            // position p's outputs (its in-betweens, then p itself) from buffer (p - 1 - d0) k + j: phase j / (g k) of the span
            const uint64_t d0 = s - uint64_t(g);
            for (uint64_t p = d0 + 1; p <= s; ++p) {
                for (int j = 1; j < m_k; ++j) m_q.push_back({ { p, uint32_t(j), uint32_t(m_k), 1 }, size_t(p - 1 - d0) * size_t(m_k) + size_t(j) });
                if (p < s) m_q.push_back({ { p, 0, uint32_t(m_k), 1 }, size_t(p - d0) * size_t(m_k) });
            }
            if (m_eng.LastPairWasCut()) ++m_cuts;
        }
        m_q.push_back({ { s, 0, uint32_t(m_k), 0 }, 0 });
        m_pending.clear();
        m_still = false;
        ++m_seq;
        return PMENG_OK;
    }
    int Finish() override
    {
        for (uint64_t p : m_pending) HoldPosition(p);   // the end of the stream: what waited for a next drawing, as it is
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
        const Item it = m_q.front();
        m_q.pop_front();
        if (dst.mem != PMENG_MEM_CPU || dst.fmt != m_fmt) { err = "apple engine: unexpected output memory / format"; return PMENG_ERROR; }
        FromPlanar(m_bufs[it.buf], dst);
        ++m_written;
        if (it.buf) ++m_generated;
        return PMENG_OK;
    }
    int Skip() override { if (!m_q.empty()) m_q.pop_front(); return PMENG_OK; }
    uint32_t Pending() override { return uint32_t(m_q.size()); }
    int Flush() override { m_q.clear(); m_eng.Forget(); m_pending.clear(); m_still = false; m_haveDistinct = false; return PMENG_OK; }
    std::string Stats() override
    {
        const auto& s = m_eng.Stat();
        char b[400];
        std::snprintf(b, sizeof(b), "apple: %llu sources (%llu repeats, Dedup %s), %llu outputs (%llu generated), %llu scene cuts; %llu VideoToolbox calls (%llu errors), %.2f ms a pair; %s",
                      (unsigned long long)m_seq, (unsigned long long)m_dups, m_dedup ? "on" : "off", (unsigned long long)m_written, (unsigned long long)m_generated, (unsigned long long)m_cuts,
                      (unsigned long long)s.vtCalls, (unsigned long long)s.vtErrors, s.pairMs, m_eng.Describe().c_str());
        return b;
    }

private:
    struct Item { pmeng_out_info info; size_t buf; };   // buf: m_bufs index (0: the last real frame)
    // a held position (a still): all its outputs show the last distinct picture (m_bufs[0], the last real frame written)
    void HoldPosition(uint64_t p)
    {
        for (int j = 1; j < m_k; ++j) m_q.push_back({ { p, uint32_t(j), uint32_t(m_k), 1 }, 0 });
        m_q.push_back({ { p, 0, uint32_t(m_k), 1 }, 0 });
    }
    // n output buffers ([0] the real frame, [1..n-1] generated): a held span needs up to (lookahead + 1) k
    void Bufs(size_t n)
    {
        if (m_bufs.size() >= n) return;
        const size_t ch = size_t((m_h + 1) / 2), old = m_bufs.size();
        m_bufs.resize(n);
        for (size_t i = old; i < n; ++i) { m_bufs[i][0].resize(m_sy * m_h); m_bufs[i][1].resize(m_sc * ch); m_bufs[i][2].resize(m_sc * ch); }
    }
    apple::PlanarOut Out(size_t i)
    {
        apple::PlanarOut o;
        for (int p = 0; p < 3; ++p) { o.data[p] = m_bufs[i][p].data(); o.stride[p] = ptrdiff_t(p ? m_sc : m_sy); }
        return o;
    }
    void ToPlanar(const pmeng_image& s)
    {
        const int ch = (m_h + 1) / 2, cw = (m_w + 1) / 2;
        if (m_bits == 8) {
            for (int r = 0; r < m_h; ++r) std::memcpy(&m_in[0][r * m_sy], s.data[0] + r * s.linesize[0], m_sy);
            for (int r = 0; r < ch; ++r) {
                const uint8_t* q = s.data[1] + r * s.linesize[1];
                uint8_t* u = &m_in[1][r * m_sc]; uint8_t* v = &m_in[2][r * m_sc];
                for (int x = 0; x < cw; ++x) { u[x] = q[2 * x]; v[x] = q[2 * x + 1]; }
            }
        } else {
            for (int r = 0; r < m_h; ++r) {
                const uint16_t* q = reinterpret_cast<const uint16_t*>(s.data[0] + r * s.linesize[0]);
                uint16_t* y = reinterpret_cast<uint16_t*>(&m_in[0][r * m_sy]);
                for (int x = 0; x < m_w; ++x) y[x] = uint16_t(q[x] >> 6);
            }
            for (int r = 0; r < ch; ++r) {
                const uint16_t* q = reinterpret_cast<const uint16_t*>(s.data[1] + r * s.linesize[1]);
                uint16_t* u = reinterpret_cast<uint16_t*>(&m_in[1][r * m_sc]); uint16_t* v = reinterpret_cast<uint16_t*>(&m_in[2][r * m_sc]);
                for (int x = 0; x < cw; ++x) { u[x] = uint16_t(q[2 * x] >> 6); v[x] = uint16_t(q[2 * x + 1] >> 6); }
            }
        }
    }
    void FromPlanar(const std::array<std::vector<uint8_t>, 3>& p, const pmeng_image& d) const
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

    apple::Engine m_eng;
    apple::Colour m_col;
    uint32_t m_fmt = 0;
    int m_w = 0, m_h = 0, m_bits = 8, m_k = 1;
    uint64_t m_seq = 0, m_written = 0, m_generated = 0, m_cuts = 0;
    size_t m_sy = 0, m_sc = 0;
    std::vector<std::array<std::vector<uint8_t>, 3>> m_bufs;
    std::vector<uint8_t> m_in[3];
    std::deque<Item> m_q;
    // held frames: Dedup on, a still, the repeats waiting for the next distinct frame, quarter-size luma (last distinct, new)
    bool m_dedup = false, m_still = false, m_haveDistinct = false;
    uint64_t m_dups = 0;
    std::deque<uint64_t> m_pending;
    std::vector<uint8_t> m_lastQ, m_curQ;
};

} // namespace

// the engine module's entry into this one (same compiler and C++ library: the Backend crosses as a C++ object)
extern "C" PMENG_EXPORT pme::Backend* pme_apple_open(const pmeng_stream* st, const pme::Options* o, pmeng_output* out, std::string* why, void (*log)(const char*))
{
    auto* b = new AppleBackend();
    b->log = log;
    if (!b->Init(*st, *o, *out, *why)) { delete b; return nullptr; }
    return b;
}
