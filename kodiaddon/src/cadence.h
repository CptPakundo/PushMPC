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

} // namespace pmk
