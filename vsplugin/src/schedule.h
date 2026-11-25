// pushmpc.Enhance: the frame scheduling, rate and warm-core reuse rules, free of VapourSynth and D3D11 so the unit tests
// (tests\sched_test.cpp) compile them directly.
//
// Pull model: VapourSynth asks for output frame n. With multiplier k, n = s * k + sub belongs to source frame s; sub 0 is
// s's real frame, sub 1..k-1 are generated between s and s + 1, so they need s + 1 submitted. pushcore hands out frames
// in presentation order (for each submitted source s: the generated frames of the pair (s - 1, s), then s's real frame);
// every output lands in a small cache keyed by its output index, which comes from the frame's pts (we submit
// pts(s) = s x source duration, so a generated frame's pts sits on the k-th grid). A generated frame pushcore did not
// produce (skippedGenerated) is replaced by the nearest real frame instead of stalling.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <numeric>
#include <string>
#include <vector>

namespace pmvs {

// ------------------------------------------------------------------------------------------------ rates
struct Rational { int64_t num = 0, den = 1; };
inline Rational Reduce(int64_t num, int64_t den)
{
    if (den < 0) { num = -num; den = -den; }
    if (den == 0) return { 0, 1 };
    const int64_t g = std::gcd(num < 0 ? -num : num, den);
    return g ? Rational{ num / g, den / g } : Rational{ num, den };
}
// output rate = source rate x k (0/x stays 0: variable rate, the frame durations carry the timing)
inline Rational OutputRate(int64_t fpsNum, int64_t fpsDen, int k) { return fpsNum <= 0 || fpsDen <= 0 ? Rational{ 0, 1 } : Reduce(fpsNum * k, fpsDen); }
// _DurationNum / _DurationDen of an output frame: the source frame's duration / k
inline Rational OutputDuration(int64_t durNum, int64_t durDen, int k) { return durNum <= 0 || durDen <= 0 ? Rational{ 0, 1 } : Reduce(durNum, durDen * k); }

struct Pos { int64_t src = 0; int sub = 0; };
inline Pos Map(int64_t n, int k) { return { n / k, int(n % k) }; }
inline int64_t OutputFrames(int64_t srcFrames, int k) { return srcFrames * k; }
// the source range output n needs: s alone for the real frame, s .. s + 1 for a generated one (s alone at the clip's end)
inline void Needs(int64_t n, int k, int64_t srcFrames, int64_t& lo, int64_t& hi)
{
    const Pos p = Map(n, k);
    lo = p.src;
    hi = p.sub == 0 ? p.src : std::min(p.src + 1, srcFrames - 1);
}
// the nearest real frame for a generated one that is missing
inline int64_t NearestReal(int64_t n, int k, int64_t srcFrames)
{
    const Pos p = Map(n, k);
    return std::min(p.sub * 2 <= k ? p.src : p.src + 1, srcFrames - 1);
}

// ------------------------------------------------------------------------------------------------ submission plan
inline constexpr int64_t kMaxGap = 4; // a forward jump of at most this many source frames is submitted through, else reset

struct Plan { bool discontinuity = false; int64_t from = 0, to = -1; };
// the source frames to request in arInitial: from what is already submitted (a short gap is filled in) up to hi
inline int64_t RequestFrom(int64_t nextSubmit, int64_t s, int64_t hi)
{
    return nextSubmit >= s - kMaxGap && nextSubmit <= hi ? std::min(nextSubmit, s) : s;
}
// what to submit for output n: nothing (already submitted), the frames from nextSubmit (in order), or a reset at s.
// requestedFrom = what arInitial requested (only those source frames are available now).
// evicted = the output was cached once and dropped (a jump back): it has to be made again. Not evicted and already
// submitted = pushcore skipped it (or it is still on its way): no submit, the wait and the fallback handle it.
inline Plan PlanSubmit(int64_t nextSubmit, int64_t s, int64_t hi, int64_t requestedFrom, bool cached, bool evicted)
{
    Plan p;
    if (cached) return p;
    if (nextSubmit > hi) {
        if (evicted) { p.discontinuity = true; p.from = s; p.to = hi; }
        return p;
    }
    if (nextSubmit >= requestedFrom && nextSubmit >= s - kMaxGap) { p.from = nextSubmit; p.to = hi; return p; }
    p.discontinuity = true; p.from = s; p.to = hi;
    return p;
}

// ------------------------------------------------------------------------------------------------ pts grid
struct Grid {
    double srcDur100ns = 417083.3; // one source frame, 100 ns
    int k = 1;
    int64_t Pts(int64_t s) const { return int64_t(std::llround(double(s) * srcDur100ns)); }
    int64_t Index(int64_t pts) const { return int64_t(std::llround(double(pts) * k / srcDur100ns)); }
};
inline double SrcDur100ns(int64_t fpsNum, int64_t fpsDen) { return fpsNum > 0 && fpsDen > 0 ? 1e7 * double(fpsDen) / double(fpsNum) : 1e7 / 23.976; }

// ------------------------------------------------------------------------------------------------ the pipeline
// Frame: what the cache holds (a VSFrame* in the plugin, an int in the tests). The backend submits source frames and
// polls finished outputs; the pipeline owns the order, the cache and the fallback.
template <class Frame> struct Out { int64_t pts = 0; bool generated = false; Frame frame{}; };
template <class Frame> struct Backend {
    virtual ~Backend() = default;
    virtual bool CanSubmit() = 0;
    virtual bool Submit(int64_t s, int64_t pts, bool discontinuity, std::string& err) = 0;
    virtual bool Poll(uint32_t timeoutMs, std::vector<Out<Frame>>& out, std::string& err) = 0; // appends
    virtual bool Flush(std::string& err) = 0;
    virtual void Release(Frame f) = 0;
    virtual uint64_t NowMs() = 0;
};

template <class Frame> class Pipeline {
public:
    struct Result { bool ok = false; Frame frame{}; bool repeated = false; int64_t repeatOf = -1; std::string err; };
    struct Counters { uint64_t submitted = 0, outputs = 0, generated = 0, repeats = 0, discontinuities = 0, offGrid = 0, evicted = 0; };

    void Init(int k, int64_t srcFrames, double srcDur100ns) { m_grid.k = std::max(1, k); m_grid.srcDur100ns = srcDur100ns; m_srcFrames = srcFrames; }
    int K() const { return m_grid.k; }
    const Grid& GridOf() const { return m_grid; }
    int64_t NextSubmit() const { return m_nextSubmit; }
    const Counters& Count() const { return m_count; }
    size_t Cached() const { return m_cache.size(); }

    void Requests(int64_t n, int64_t& from, int64_t& to) const
    {
        int64_t lo, hi; Needs(n, m_grid.k, m_srcFrames, lo, hi);
        from = Has(n) ? lo : RequestFrom(m_nextSubmit, lo, hi);
        to = hi;
    }
    // produce output n; the frame stays owned by the cache (the caller adds its own reference)
    Result Produce(int64_t n, int64_t requestedFrom, Backend<Frame>& b, uint32_t waitMs, bool retry = true)
    {
        Result r;
        int64_t lo, hi; Needs(n, m_grid.k, m_srcFrames, lo, hi);
        const Pos pos = Map(n, m_grid.k);
        const Plan plan = PlanSubmit(m_nextSubmit, pos.src, hi, requestedFrom, Has(n), n < m_evictedBelow);
        if (plan.discontinuity) {
            if (!b.Flush(r.err)) return r;
            m_count.discontinuities++;
            m_realSeen = pos.src - 1;
            m_evictedBelow = std::min(m_evictedBelow, pos.src * m_grid.k);
        }
        for (int64_t i = plan.from; i <= plan.to; ++i) {
            const uint64_t until = b.NowMs() + waitMs;
            while (!b.CanSubmit()) {
                if (!Drain(b, 5, r.err)) return r;
                if (b.NowMs() > until) { r.err = "pushcore stays busy"; return r; }
            }
            if (!b.Submit(i, m_grid.Pts(i), plan.discontinuity && i == plan.from, r.err)) return r;
            m_count.submitted++;
            m_nextSubmit = i + 1;
            if (!Drain(b, 0, r.err)) return r;
        }
        // wait until n is there, or until what it needs has come out without it
        const int64_t needReal = pos.sub == 0 ? pos.src : hi;
        const uint64_t deadline = b.NowMs() + waitMs;
        while (!Has(n) && m_realSeen < needReal) {
            if (!Drain(b, 10, r.err)) return r;
            if (b.NowMs() > deadline) break;
        }
        if (Has(n)) { r.ok = true; r.frame = m_cache[n]; }
        else {
            const int64_t real = NearestReal(n, m_grid.k, m_srcFrames) * m_grid.k;
            const int64_t other = (pos.src) * m_grid.k;
            const int64_t pick = Has(real) ? real : Has(other) ? other : -1;
            if (pick < 0 && retry && !plan.discontinuity) { m_evictedBelow = std::max(m_evictedBelow, n + 1); return Produce(n, requestedFrom, b, waitMs, false); }
            if (pick < 0) { r.err = "pushcore delivered neither the frame nor a real neighbour"; Evict(b, pos.src); return r; }
            r.ok = true; r.frame = m_cache[pick]; r.repeated = true; r.repeatOf = pick;
            m_count.repeats++;
        }
        Evict(b, pos.src);
        return r;
    }
    void Clear(Backend<Frame>& b)
    {
        for (auto& [i, f] : m_cache) b.Release(f);
        m_cache.clear();
        m_nextSubmit = -1000000; m_realSeen = -1; m_evictedBelow = 0;
    }
    template <class F> void ClearWith(F releaseFn)
    {
        for (auto& [i, f] : m_cache) releaseFn(f);
        m_cache.clear();
        m_nextSubmit = -1000000; m_realSeen = -1; m_evictedBelow = 0;
    }
    // the pipeline is new after a reopen or reuse: the next submit must be a discontinuity
    void Reset() { m_nextSubmit = -1000000; m_realSeen = -1; m_evictedBelow = 0; }

private:
    bool Has(int64_t n) const { return m_cache.count(n) != 0; }
    bool Drain(Backend<Frame>& b, uint32_t timeoutMs, std::string& err)
    {
        std::vector<Out<Frame>> out;
        if (!b.Poll(timeoutMs, out, err)) return false;
        for (auto& o : out) {
            const int64_t idx = m_grid.Index(o.pts);
            const double exact = double(o.pts) * m_grid.k / m_grid.srcDur100ns;
            if (std::fabs(exact - double(idx)) > 0.25) m_count.offGrid++;
            m_count.outputs++;
            if (o.generated) m_count.generated++;
            else m_realSeen = std::max(m_realSeen, idx / m_grid.k);
            auto it = m_cache.find(idx);
            if (it != m_cache.end()) { b.Release(it->second); it->second = o.frame; }
            else m_cache.emplace(idx, o.frame);
        }
        return true;
    }
    // keep the previous source's outputs (a fallback and a short step back), drop older ones; cap the size
    // (never s's own outputs: the frame just handed out is among them). Frames ahead of s stay unless the cache is full.
    void Evict(Backend<Frame>& b, int64_t s)
    {
        const int64_t keepFrom = (s - 1) * m_grid.k, current = s * m_grid.k;
        while (!m_cache.empty()) {
            auto it = m_cache.begin();
            if (it->first >= current || (it->first >= keepFrom && m_cache.size() <= kMaxCache)) break;
            m_evictedBelow = std::max(m_evictedBelow, it->first + 1); b.Release(it->second); m_cache.erase(it); m_count.evicted++;
        }
        while (m_cache.size() > kMaxCache) { // far ahead of s (a jump back): drop the newest
            auto it = std::prev(m_cache.end());
            if (it->first < current + m_grid.k) break;
            b.Release(it->second); m_cache.erase(it); m_count.evicted++;
        }
    }
    static constexpr size_t kMaxCache = 96;
    Grid m_grid;
    int64_t m_srcFrames = 0;
    int64_t m_nextSubmit = -1000000;
    int64_t m_realSeen = -1;
    int64_t m_evictedBelow = 0;   // outputs below this were cached once and dropped
    std::map<int64_t, Frame> m_cache;
    Counters m_count;
};

// ------------------------------------------------------------------------------------------------ warm core reuse
// mpv reloads the script on every seek: the next instance reuses the process's open core when the stream matches.
struct CoreKey {
    uint32_t width = 0, height = 0, format = 0;
    int32_t fpsNum = 0, fpsDen = 0, range = 0, matrix = 0, primaries = 0, transfer = 0;
    uint32_t displayHdr = 0;
    uint64_t adapterLuid = 0;
    std::vector<uint8_t> settings; // the PushCoreSettings bytes (zero-initialised before filling)
    bool SameStream(const CoreKey& o) const
    {
        return width == o.width && height == o.height && format == o.format && fpsNum == o.fpsNum && fpsDen == o.fpsDen && range == o.range
            && matrix == o.matrix && primaries == o.primaries && transfer == o.transfer && displayHdr == o.displayHdr && adapterLuid == o.adapterLuid;
    }
};
enum class Reuse { Open, Flush, Reload, Reopen };
inline const char* ReuseName(Reuse r) { return r == Reuse::Flush ? "flush (warm core reused)" : r == Reuse::Reload ? "setSettings + reload (warm core, new settings)" : r == Reuse::Reopen ? "reopen (stream changed)" : "open (cold)"; }
inline Reuse DecideReuse(bool haveCore, const CoreKey& cur, const CoreKey& next)
{
    if (!haveCore) return Reuse::Open;
    if (!cur.SameStream(next)) return Reuse::Reopen;
    return cur.settings == next.settings ? Reuse::Flush : Reuse::Reload;
}

// ------------------------------------------------------------------------------------------------ readback bandwidth
// 4:4:4 readback above this (GB/s, one direction; mpv's re-upload costs the same again) goes 4:2:0 on the GPU first
// (4K at <= 120 fps is fine in 4:4:4, 4K HDR at 240 is not).
inline constexpr double kReadbackLimitGBs = 6.0;
enum class Chroma { Auto = 0, C444 = 1, C420 = 2 };
inline double ReadbackGBs(uint32_t w, uint32_t h, bool tenBit, bool c444, double outFps)
{
    const double bytesPerSample = tenBit ? 2.0 : 1.0;
    const double samples = double(w) * h * (c444 ? 3.0 : 1.5);
    return samples * bytesPerSample * outFps / 1e9;
}
struct ChromaDecision { bool use420 = false; double gbs444 = 0, gbs420 = 0; std::string why; };
inline ChromaDecision DecideChroma(Chroma mode, uint32_t w, uint32_t h, bool tenBit, double outFps)
{
    ChromaDecision d;
    d.gbs444 = ReadbackGBs(w, h, tenBit, true, outFps);
    d.gbs420 = ReadbackGBs(w, h, tenBit, false, outFps);
    char b[200];
    if (mode == Chroma::C444) { d.use420 = false; snprintf(b, sizeof(b), "4:4:4 (your setting; %.2f GB/s readback)", d.gbs444); }
    else if (mode == Chroma::C420) { d.use420 = true; snprintf(b, sizeof(b), "4:2:0 (your setting; %.2f GB/s readback)", d.gbs420); }
    else if (d.gbs444 > kReadbackLimitGBs) { d.use420 = true; snprintf(b, sizeof(b), "Auto: 4:2:0 on the GPU before readback (4:4:4 would be %.2f GB/s > %.1f; 4:2:0 %.2f GB/s)", d.gbs444, kReadbackLimitGBs, d.gbs420); }
    else { d.use420 = false; snprintf(b, sizeof(b), "Auto: 4:4:4 (%.2f GB/s readback <= %.1f)", d.gbs444, kReadbackLimitGBs); }
    d.why = b;
    return d;
}

// ------------------------------------------------------------------------------------------------ colour props
// VapourSynth's _Matrix/_Primaries/_Transfer are ITU-T H.273 codes, the same numbers as FFmpeg's enums that
// PushCoreStreamInfo uses; only the range differs (VS: 0 full, 1 limited; FFmpeg: 1 limited, 2 full, 0 unspecified).
inline int32_t FfRangeFromVs(int64_t vsRange, bool present) { return !present ? 1 : vsRange == 0 ? 2 : 1; }
inline bool IsHdrTransfer(int32_t t) { return t == 16 || t == 18; }
// mpv hands out no frame while the script loads ("Frame requested during init" gives a dummy), so the core opens with a
// guess and is reopened at the first real frame when its colour differs; the guess comes from the previous instance
// of the same stream (mpv's reload on a seek) or from the format: 10-bit UHD is almost always PQ BT.2020.
struct Colour {
    int32_t matrix = 1, transfer = 1, primaries = 1, range = 1;   // FFmpeg / H.273 codes, range 1 limited 2 full
    bool operator==(const Colour&) const = default;
};
inline Colour GuessColour(uint32_t w, uint32_t h, int bits)
{
    Colour c;
    if (bits > 8 && (w >= 3200 || h >= 1800)) { c.matrix = 9; c.transfer = 16; c.primaries = 9; }
    else if (w < 1024 && h < 600) { c.matrix = 6; }   // SD: BT.601
    return c;
}
// the colour of a frame from its properties; absent or unspecified (2) values keep the fallback's
inline Colour ColourFromProps(const Colour& fallback, int64_t matrix, bool hasMatrix, int64_t transfer, bool hasTransfer, int64_t primaries, bool hasPrimaries, int64_t vsRange, bool hasRange)
{
    Colour c = fallback;
    if (hasMatrix && matrix != 2) c.matrix = int32_t(matrix);
    if (hasTransfer && transfer != 2) c.transfer = int32_t(transfer);
    if (hasPrimaries && primaries != 2) c.primaries = int32_t(primaries);
    if (hasRange) c.range = FfRangeFromVs(vsRange, true);
    return c;
}

} // namespace pmvs
