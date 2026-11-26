// cadence: which pictures the videocodec returns to Kodi for each new source frame, and with which timestamps.
// Pure C++ (no Kodi, no FFmpeg) so cadence_test checks every rule.
//
// One frame of latency: when source frame C arrives, the previous frame P is output as a real picture at its own pts,
// followed by k - 1 generated pictures at P + j/k * (C - P). Kodi's VideoPlayer takes several pictures per packet
// (GetPicture is polled until it returns VC_BUFFER) and its renderer picks, at each display refresh, the newest picture
// whose pts is due (RenderManager::PrepareNextRender), so the timestamps alone do the pacing.
//
// No generation across a discontinuity: a gap larger than kMaxGapFrames source frame intervals (a seek, a missing
// stretch, a timestamp jump) or a step back. Then P goes out alone and the pair starts over.
#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

namespace pmk {

inline constexpr double kMaxGapFrames = 2.5;

struct Slot {
    int64_t pts = 0;   // microseconds (Kodi's DVD_TIME_BASE)
    float t = 0.f;     // phase within the pair: 0 = the real previous frame
    bool real = true;
    bool newest = false;   // held frames: a real picture that is the engine's newest frame (emitted before the next push)
};

// frameUs: the nominal source frame interval (from the stream's frame rate); <= 0 when unknown (then any forward gap up
// to 250 ms counts as contiguous)
inline bool Contiguous(int64_t prevPts, int64_t curPts, double frameUs)
{
    const int64_t d = curPts - prevPts;
    if (d <= 0) return false;
    const double limit = frameUs > 0 ? frameUs * kMaxGapFrames : 250000.0;
    return double(d) <= limit;
}

// the pictures to emit for the previous frame now that the current one is known
inline std::vector<Slot> SlotsForPair(int64_t prevPts, int64_t curPts, int k, bool contiguous, bool engineHasPair)
{
    std::vector<Slot> out;
    out.push_back(Slot{prevPts, 0.f, true});
    if (k <= 1 || !contiguous || !engineHasPair) return out;
    const double d = double(curPts - prevPts);
    for (int j = 1; j < k; ++j) {
        Slot s;
        s.real = false;
        s.t = float(double(j) / k);
        s.pts = prevPts + int64_t(std::llround(d * j / k));
        out.push_back(s);
    }
    return out;
}

// the stream frame rate Kodi is told: k x the source rate, as a rational (so refresh-rate switching and VideoPlayer's
// frame time match what the add-on outputs). Kodi's VideoPlayerVideo::OpenStream treats rates above 120 fps as
// invalid (it then uses 25 fps and trusts the timestamps until its own detection settles), which is harmless.
inline void ReportedRate(int rate, int scale, int k, int& outRate, int& outScale)
{
    if (k < 1) k = 1;
    outRate = rate * k;
    outScale = scale;
    if (outRate <= 0 || outScale <= 0) { outRate = 0; outScale = 0; }
}

// ---- held frames (animation drawn on twos, threes, fours). With the engine's Dedup on, a frame that repeats the last
// distinct one does not make a new pair (IEngine::LastDup). Up to kHeldLookahead repeats wait (nothing is emitted for
// them); when the next distinct frame C comes, the base B (the last distinct frame) goes out as a real picture, then
// g k - 1 generated pictures at B + j / (g k) x (C - B), t = j / (g k), all from the one pair (B, C), g = repeats + 1.
// One more repeat makes a still: B and the repeats so far go out as real pictures (the same drawing) and each further
// repeat goes out as it comes; when the still ends only its last interval moves (pair (B's drawing, C), t = j / k).
// No pair across a gap: B and the waiting repeats go out as they are. Without repeats this is SlotsForPair.
inline constexpr int kHeldLookahead = 3;   // twos, threes, fours (the other routes' lookahead)

class HeldCadence {
public:
    void Reset() { m_have = false; Start(0); }
    // a source frame at pts, after the engine's push: dup = it repeated the last distinct frame (the pair stayed)
    std::vector<Slot> Next(int64_t pts, bool dup, bool contiguous, int k, bool engineHasPair)
    {
        std::vector<Slot> out;
        if (!m_have) { m_have = true; Start(pts); return out; }
        if (!contiguous) {
            out.push_back(Slot{m_base, 0.f, true, false});
            for (int64_t r : m_reps) out.push_back(Slot{r, 0.f, true, false});
            Start(pts);
            return out;
        }
        if (dup) {
            ++m_repeats;
            if (m_still) { out.push_back(Slot{m_base, 0.f, true, true}); m_base = pts; return out; }
            m_reps.push_back(pts);
            if (int(m_reps.size()) <= kHeldLookahead) return out;   // waits for the next drawing
            ++m_stills;
            m_still = true;
            out.push_back(Slot{m_base, 0.f, true, true});
            for (size_t i = 0; i + 1 < m_reps.size(); ++i) out.push_back(Slot{m_reps[i], 0.f, true, true});
            m_base = m_reps.back();
            m_reps.clear();
            return out;
        }
        const int g = m_still ? 1 : int(m_reps.size()) + 1;
        out.push_back(Slot{m_base, 0.f, true, false});
        if (k > 1 && engineHasPair) {
            const int n = g * k;
            const double d = double(pts - m_base);
            for (int j = 1; j < n; ++j) out.push_back(Slot{m_base + int64_t(std::llround(d * j / n)), float(double(j) / n), false, false});
        } else {
            for (int64_t r : m_reps) out.push_back(Slot{r, 0.f, true, false});
        }
        Start(pts);
        return out;
    }
    // the end of the stream: the base and the waiting repeats, as they are (the engine's newest frame)
    std::vector<Slot> Drain()
    {
        std::vector<Slot> out;
        if (m_have) {
            out.push_back(Slot{m_base, 0.f, true, true});
            for (int64_t r : m_reps) out.push_back(Slot{r, 0.f, true, true});
        }
        Reset();
        return out;
    }
    bool Have() const { return m_have; }
    uint64_t Repeats() const { return m_repeats; }
    uint64_t Stills() const { return m_stills; }

private:
    void Start(int64_t pts) { m_base = pts; m_reps.clear(); m_still = false; }
    bool m_have = false, m_still = false;
    int64_t m_base = 0;             // B: the last distinct frame (in a still: the last repeat)
    std::vector<int64_t> m_reps;    // the repeats waiting for the next drawing
    uint64_t m_repeats = 0, m_stills = 0;
};

} // namespace pmk
