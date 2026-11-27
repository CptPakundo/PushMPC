#pragma once
// PushMPC Auto: whether Auto's anime upscaler keeps up where its cost is only a prior (card 1.0: the Kodi add-on, the
// VapourSynth plugin), and what runs when it does not. Portable (no Windows types), pure logic: tested on the CPU
// (kodiaddon/tests/win_anime_route_test.cpp).
//
// AnimeKeepUp      one guard per stream. Feed one sample per source frame (the upscaler's time against the frame's) or
//                  per picture expected from pushcore (delivered or missed). Nothing counts during a warm-up after an
//                  open, a model switch, a settings update or a flush / seek (a cold model, the pictures a reload
//                  dropped). It trips only when most of a sliding window of about kAnimeSustainS is late (a median over
//                  the window, not a short run), so a short load spike on a shared GPU does not decide a whole film.
//   A trip is
//     transient    the model kept up here before (a full window mostly on time), or its median cost is under
//                  kAnimeTransientRatio x the frame: the plain route for a while, then the model again (kAnimeRetryFirstS,
//                  doubling up to kAnimeRetryMaxS); after kAnimeMaxTrips trips in one stream it counts as real;
//     real         never on time here, or far over: a cheaper ranked model when one fits at the measured speed
//                  (AnimeSrStepDown), else the plain route for the stream.
// The constants are design values, not measurements (none of them was tuned on a loaded PC); they are on the track
// note's "perf re-run on a free PC" list.
#include <algorithm>
#include <cmath>
#include <vector>
#include "PushMpcAnimeSr.h"

namespace pm {

constexpr int kAnimeWarmupFrames = 24;        // source frames not counted after an open / switch / update / flush (~1 s)
constexpr double kAnimeSustainS = 3.0;        // the sliding window: most of it late = a trip
constexpr int kAnimeMinWindowFrames = 24;     // ... at least this many source frames
constexpr double kAnimeOnTimeShare = 0.25;    // a full window with at most this share late: the model keeps up here
constexpr double kAnimeTransientRatio = 2.0;  // median cost under this x the frame: load, not the card
constexpr double kAnimeRetryFirstS = 60.0;    // the model again after this long, doubling per trip ...
constexpr double kAnimeRetryMaxS = 600.0;     // ... up to this
constexpr int kAnimeMaxTrips = 3;             // transient trips in one stream; the next one counts as real

class AnimeKeepUp {
public:
    enum class Trip { None, Transient, Real };

    // fps: the source rate; perFrame: samples per source frame (pictures expected from pushcore: k, else 1). Starts a
    // new model (trips, back-off and the on-time record cleared) with its warm-up.
    void Configure(double fps, int perFrame = 1)
    {
        m_per = std::max(1, perFrame);
        const double f = fps > 0 ? fps : 23.976;
        m_windowFrames = std::max(kAnimeMinWindowFrames, int(std::lround(kAnimeSustainS * f)));
        NewModel();
    }
    // a new model (a step down, or a new stream): its own record
    void NewModel()
    {
        m_trips = 0; m_backoffS = kAnimeRetryFirstS; m_onTime = false; m_retryAt = -1;
        Restart();
    }
    // an open, a switch, a settings update, a flush or a seek: the warm-up again, the window cleared. graceFrames: more
    // source frames than kAnimeWarmupFrames when the route holds pictures that long (pushcore's held re-timing);
    // perFrame > 0: the samples per source frame from now on (pushcore's pictures change with its settings)
    void Restart(int graceFrames = 0, int perFrame = 0)
    {
        if (perFrame > 0) m_per = perFrame;
        m_grace = std::max(kAnimeWarmupFrames, graceFrames) * m_per;
        m_late.clear(); m_value.clear(); m_pos = 0; m_lateCount = 0;
    }

    // the upscaler's time for this source frame (ms, < 0 = none ran) against the frame's (ms)
    Trip FeedMs(double ms, double frameMs, double nowS)
    {
        if (ms < 0 || frameMs <= 0) return Trip::None;
        return Feed(ms > frameMs, ms / frameMs, nowS);
    }
    // a picture expected from pushcore: delivered or missed
    Trip FeedDelivered(bool delivered, double nowS) { return Feed(!delivered, -1.0, nowS); }

    // the back-off after a transient trip has run out: the model may run again (once; the warm-up starts over)
    bool RetryDue(double nowS)
    {
        if (m_retryAt < 0 || nowS < m_retryAt) return false;
        m_retryAt = -1;
        Restart();
        return true;
    }
    bool Waiting() const { return m_retryAt >= 0; }

    // the last trip, for the log: late samples of the window, its size, the cost against the frame (the median for
    // times; window / delivered for pictures), the trips so far, the wait before the model runs again (s)
    int LastLate() const { return m_lastLate; }
    int LastSeen() const { return m_lastSeen; }
    double LastRatio() const { return m_lastRatio; }
    int Trips() const { return m_trips; }
    double LastWaitS() const { return m_lastWaitS; }
    bool OnTime() const { return m_onTime; }
    int WindowSamples() const { return m_windowFrames * m_per; }

private:
    Trip Feed(bool late, double value, double nowS)
    {
        if (m_grace > 0) { --m_grace; return Trip::None; }
        const int n = WindowSamples();
        if (int(m_late.size()) < n) { m_late.push_back(late); m_value.push_back(value); m_lateCount += late; }
        else {
            m_lateCount += int(late) - int(m_late[m_pos]);
            m_late[m_pos] = late; m_value[m_pos] = value;
            m_pos = (m_pos + 1) % n;
        }
        if (int(m_late.size()) < n) return Trip::None;
        if (m_lateCount * 2 <= n) {
            if (m_lateCount <= kAnimeOnTimeShare * n) m_onTime = true;
            return Trip::None;
        }
        // most of the window late: a trip
        m_lastLate = m_lateCount; m_lastSeen = n;
        if (m_value[0] >= 0) {
            std::vector<double> v = m_value;
            std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
            m_lastRatio = v[v.size() / 2];
        } else m_lastRatio = double(n) / std::max(1, n - m_lateCount);
        const bool transient = (m_onTime || m_lastRatio < kAnimeTransientRatio) && m_trips < kAnimeMaxTrips;
        Restart();
        if (!transient) { m_lastWaitS = 0; return Trip::Real; }
        ++m_trips;
        m_lastWaitS = m_backoffS;
        m_retryAt = nowS + m_backoffS;
        m_backoffS = std::min(kAnimeRetryMaxS, m_backoffS * 2);
        return Trip::Transient;
    }

    int m_per = 1, m_windowFrames = kAnimeMinWindowFrames, m_grace = 0;
    std::vector<bool> m_late;
    std::vector<double> m_value;
    int m_pos = 0, m_lateCount = 0;
    bool m_onTime = false;
    int m_trips = 0;
    double m_backoffS = kAnimeRetryFirstS, m_retryAt = -1;
    int m_lastLate = 0, m_lastSeen = 0;
    double m_lastRatio = 0, m_lastWaitS = 0;
};

// The next cheaper ranked anime model that fits Auto's share of the frame at the measured speed, -1 = none. measuredMs:
// what the running model takes per source frame here (host transfer included); it sets this card's factor against the
// running model's prior, and the cheaper models are costed with that factor (PickAnimeSr's own cost and share; the
// priors are not changed). Only models of the same scale (the output size stays) and cheaper by their prior.
inline int AnimeSrStepDown(int running, double measuredMs, unsigned srcW, unsigned srcH, unsigned outW, unsigned outH, double fps)
{
    if (running < 0 || running >= kAnimeSrModels) return -1;
    const int base = AnimeSrModel(running).variantOf >= 0 ? AnimeSrModel(running).variantOf : running;
    const double inMpx = double(srcW) * srcH / 1e6, outMpx = double(outW) * outH / 1e6;
    const double prior = AnimeSrModel(base).msPerMpx * inMpx;
    const double net = measuredMs - kAnimeSrTransferMsPerMpx * (inMpx + outMpx);
    const double card = prior > 0 && net > 0 ? std::max(1.0, net / prior) : 1.0;
    const double budget = kAnimeSrShare * 1000.0 / std::max(1.0, fps);
    const bool dvd = srcH <= kDvdClassLines;
    for (int i : kAnimeSrRank) {
        const auto& m = AnimeSrModel(i);
        if (m.msPerMpx >= AnimeSrModel(base).msPerMpx || m.scale != AnimeSrModel(base).scale || (m.dvdOnly && !dvd)) continue;
        if (AnimeSrCostMs(i, srcW, srcH, outW, outH, card, -1.0) <= budget) return i;
    }
    return -1;
}

}  // namespace pm
