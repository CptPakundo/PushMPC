// SPDX-License-Identifier: GPL-3.0-or-later
// libpushmpc_engine_apple.dylib: the apple engine (vsplugin/engines/apple: VideoToolbox VTFrameProcessor frame
// interpolation / frame-rate conversion, Metal) behind pmeng, for software frames (Plex for Mac, Homebrew mpv without
// a Vulkan device). A separate module so the engine itself keeps loading on macOS 11 (Plex's minimum) while this part
// needs macOS 13.3 to load and VTFrameProcessor (macOS 15.4+) to run: the engine dlopen()s it from its own folder and
// asks it for a backend; Configure's self-test decides on this Mac (a Mac or VM that lists an effect but cannot run it
// fails here, and Auto moves on to the Vulkan engine, then the CPU engine).
#import <CoreVideo/CoreVideo.h>
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
        std::vector<apple::PlanarOut> gen;
        for (int j = 1; j < m_k; ++j) gen.push_back(Out(size_t(j)));
        const bool contiguous = !(flags & PMENG_SUBMIT_DISCONTINUITY) && m_seq > 0;
        bool havePair = false;
        if (!m_eng.Process(v, m_col, pts * 10, contiguous, gen, Out(0), havePair, err)) { err = "apple engine: " + err; return PMENG_ERROR; }
        if (havePair) {
            for (int j = 1; j < m_k; ++j) m_q.push_back({ m_seq, uint32_t(j), uint32_t(m_k), 1 });
            if (m_eng.LastPairWasCut()) ++m_cuts;
        }
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
        if (dst.mem != PMENG_MEM_CPU || dst.fmt != m_fmt) { err = "apple engine: unexpected output memory / format"; return PMENG_ERROR; }
        FromPlanar(m_bufs[oi.subframe], dst);
        ++m_written;
        if (oi.subframe) ++m_generated;
        return PMENG_OK;
    }
    int Skip() override { if (!m_q.empty()) m_q.pop_front(); return PMENG_OK; }
    uint32_t Pending() override { return uint32_t(m_q.size()); }
    int Flush() override { m_q.clear(); m_eng.Forget(); return PMENG_OK; }
    std::string Stats() override
    {
        const auto& s = m_eng.Stat();
        char b[320];
        std::snprintf(b, sizeof(b), "apple: %llu sources, %llu outputs (%llu generated), %llu scene cuts; %llu VideoToolbox calls (%llu errors), %.2f ms a pair; %s",
                      (unsigned long long)m_seq, (unsigned long long)m_written, (unsigned long long)m_generated, (unsigned long long)m_cuts,
                      (unsigned long long)s.vtCalls, (unsigned long long)s.vtErrors, s.pairMs, m_eng.Describe().c_str());
        return b;
    }

private:
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
    std::deque<pmeng_out_info> m_q;
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

// ---- CVPixelBuffers in and out (VLC's VideoToolbox pictures, zero copy): a plain C interface, so the VLC module needs
// no C++ type from here. Opaque handle; every call is guarded by the caller's own checks of the buffers.
namespace {
struct CvpxEngine {
    apple::Engine eng;
    apple::Colour col;
    int k = 1;
    std::string info;
};
void SetC(char* d, size_t n, const std::string& s) { if (d && n) std::snprintf(d, n, "%s", s.c_str()); }
} // namespace

// w, h, bits (8 / 10) of the stream; smoother 0 Auto, 2..8 fixed; colour as FFmpeg / H.273 codes. *k: the multiplier.
// NULL with the reason when VideoToolbox cannot run this stream here (the caller then uses its copy path)
extern "C" PMENG_EXPORT void* pme_apple_cvpx_open(int w, int h, int bits, int smoother, double fps, double hz, int fullRange,
                                                  int matrix, int transfer, int primaries, int* k, char* why, size_t whyn)
{
    const apple::Caps& caps = apple::Probe();
    if (!caps.AnyInterpolation()) { SetC(why, whyn, "no VideoToolbox interpolator on this Mac: " + caps.detail); return nullptr; }
    const apple::AppleK ak = apple::DecideAppleK(smoother, fps > 0 ? fps : 23.976, hz);
    if (ak.k <= 1) { SetC(why, whyn, "x1: nothing to do"); return nullptr; }
    auto* c = new CvpxEngine();
    apple::Config cfg;
    cfg.width = w; cfg.height = h; cfg.bits = bits; cfg.k = ak.k; cfg.srcFps = fps > 0 ? fps : 23.976; cfg.sr = "off";
    c->col.matrix = matrix > 0 && matrix != 2 ? matrix : 1; c->col.transfer = transfer > 0 && transfer != 2 ? transfer : 1;
    c->col.primaries = primaries > 0 && primaries != 2 ? primaries : 1; c->col.fullRange = fullRange != 0;
    cfg.colour = c->col;
    std::string err;
    if (!c->eng.Configure(cfg, err, c->info)) { SetC(why, whyn, err); delete c; return nullptr; }
    c->k = std::max(1, c->eng.K());
    c->info = "VideoToolbox on " + caps.chip + " (" + caps.os + "), CVPixelBuffers in and out: " + c->info;
    if (k) *k = c->k;
    return c;
}
// one source buffer; gen receives up to maxGen retained buffers (the caller releases them), *ngen how many, *havePair
// whether they lie between the previous source and this one. 0 = ok
extern "C" PMENG_EXPORT int pme_apple_cvpx_process(void* h, void* pb, int64_t pts100ns, int contiguous, void** gen, int maxGen,
                                                   int* ngen, int* havePair, char* err, size_t errn)
{
    auto* c = static_cast<CvpxEngine*>(h);
    *ngen = 0; *havePair = 0;
    std::vector<void*> g;
    void* cur = nullptr;
    bool pair = false;
    std::string e;
    if (!c->eng.ProcessBuffer(pb, c->col, pts100ns, contiguous != 0, g, cur, pair, e)) { SetC(err, errn, e); return -1; }
    if (cur) CVPixelBufferRelease(static_cast<CVPixelBufferRef>(cur));   // the real frame: the caller shows its own picture
    for (size_t i = 0; i < g.size(); ++i) {
        if (int(i) < maxGen) gen[(*ngen)++] = g[i];
        else CVPixelBufferRelease(static_cast<CVPixelBufferRef>(g[i]));
    }
    *havePair = pair ? 1 : 0;
    return 0;
}
extern "C" PMENG_EXPORT void pme_apple_cvpx_forget(void* h) { static_cast<CvpxEngine*>(h)->eng.Forget(); }
extern "C" PMENG_EXPORT const char* pme_apple_cvpx_describe(void* h) { return static_cast<CvpxEngine*>(h)->info.c_str(); }
extern "C" PMENG_EXPORT void pme_apple_cvpx_close(void* h) { delete static_cast<CvpxEngine*>(h); }
