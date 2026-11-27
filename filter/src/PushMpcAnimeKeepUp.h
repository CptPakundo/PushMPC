#pragma once
// PushMPC Auto: whether Auto's anime upscaler keeps up where its cost is only a prior (card 1.0: the Kodi add-on, the
// VapourSynth plugin; VLC's output upscaler once pm_pcsr_core.h takes this header), and what runs when it does not.
// Portable (no Windows types), pure logic with the clock passed in: tested on the CPU
// (kodiaddon/tests/win_anime_route_test.cpp). The rule and its names are the ones of VLC's PcsrLateRule.
//
// AnimeKeepUp, one per stream. Feed one sample per source frame (the upscaler's time against the frame's) or per
// picture expected from pushcore (on time or missed), with its source time:
//  - grace: the first kGracePictures after an open, a model switch, a settings update (a reload drops the pictures in
//    flight), a flush or a seek are not judged (a route that holds pictures longer asks for more). A route whose model
//    may still be loading after an update (pushcore) starts that grace only at the first on-time sample (at most
//    kFirstPictureMaxS of source time later);
//  - sustained: a verdict covers at least kWindowS of SOURCE time and kMinPictures samples with no step larger than
//    kMaxGapS, and trips when more than half of them were late;
//  - no window decides the rest of the stream: every trip is followed by the model again. Transient (a whole window
//    was on time before, the late samples' median within kSlowFactor x the frame time, or no time known: pushcore's
//    pictures) waits kRearmFirstS, doubling per trip; from the kMaxTrips-th trip on, or real (never on time and far
//    over), it waits kRealRearmS. Meanwhile the route runs a cheaper ranked model (a real trip, AnimeSrStepDown priced
//    on the model's on-time speed, never on the late window) or the plain route;
//  - trips are forgotten after an on-time window kTripDecayS after the last one;
//  - a change the route could not make (the engine or pushcore refused it; the model keeps running) does not count
//    the trip: no verdict for the same back-off, none at all after kMaxTrips refusals (Defer).
// The constants are design choices, not measurements (none was tuned on a loaded PC); they are on the free-PC list.
#include <algorithm>
#include <cmath>
#include <vector>
#include "PushMpcAnimeSr.h"

namespace pm {

class AnimeKeepUp {
public:
    static constexpr int kGracePictures = 12;
    static constexpr double kWindowS = 3.0;
    static constexpr double kRearmFirstS = 60.0;
    static constexpr int kMaxTrips = 3;
    static constexpr double kSlowFactor = 2.0;
    static constexpr int kMinPictures = 24;          // a verdict needs this many samples too (sparse samples never decide)
    static constexpr double kMaxGapS = 1.0;          // a larger step between two samples starts the window over
    static constexpr double kRealRearmS = 600.0;     // a real trip (or the kMaxTrips-th): the model again after this
    static constexpr double kTripDecayS = 600.0;     // an on-time window this long after the last trip forgets the trips
    static constexpr double kFirstPictureMaxS = 10.0;   // WaitFirst: the grace starts at the latest after this source time

    enum class Trip { None, Transient, Real };

    // a new stream: no trips, no back-off, nothing known about the model; the grace
    void NewModel()
    {
        m_trips = 0; m_retryAt = -1; m_defers = 0; m_deferUntil = -1; m_lastTripAt = -1e300; m_tripPending = false;
        ModelChanged();
    }
    // another model runs (a step down, the stream's own again after the back-off): what was known about the running
    // model goes (on time, its speed); the trips and the back-off stay; the grace
    void ModelChanged() { m_onTime = false; m_onTimeMs = -1; Restart(); }
    // an open, a switch, a settings update, a flush or a seek: the grace again, the window starts over (the trips and
    // "was on time" stay). grace: more samples than kGracePictures where the route holds pictures longer. waitFirst: the
    // grace starts only at the first on-time sample (pushcore after an update: its model may load on the first frames)
    void Restart(int grace = 0, bool waitFirst = false)
    {
        m_grace = std::max(kGracePictures, grace);
        m_waitFirst = waitFirst; m_waitStart = -1e300;
        m_seen = m_late = 0; m_start = m_last = 0; m_delays.clear(); m_ms.clear();
    }

    // the upscaler's time for the source frame at tS (s) (ms, < 0 = none ran) against the frame's (ms)
    Trip FeedMs(double ms, double frameMs, double tS, double nowS)
    {
        if (ms < 0 || frameMs <= 0) return Trip::None;
        return Sample(ms <= frameMs, tS, frameMs, nowS, ms);
    }
    // a picture at source time tS (s) expected from pushcore: on time (delivered) or not; no time is known for it
    Trip FeedPicture(bool onTime, double tS, double frameMs, double nowS) { return Sample(onTime, tS, frameMs, nowS, -1.0); }

    // the back-off after a trip has run out: the stream's own model may run again (once; the grace starts over; a route
    // that ran a cheaper model meanwhile calls ModelChanged too, so what it learned about that one goes)
    bool RetryDue(double nowS)
    {
        if (m_retryAt < 0 || nowS < m_retryAt) return false;
        m_retryAt = -1;
        Restart();
        return true;
    }
    bool Waiting() const { return m_retryAt >= 0; }

    // the change the last trip asked for was refused (the model keeps running): the trip does not count, no verdict
    // for the back-off, none after kMaxTrips refusals
    void Defer(double nowS)
    {
        if (m_tripPending) { --m_trips; m_retryAt = -1; m_tripPending = false; }
        ++m_defers;
        double s = kRearmFirstS;
        for (int i = 1; i < m_defers; ++i) s *= 2;
        m_deferUntil = nowS + s;
        m_lastWait = s;
        Restart();
    }
    bool GaveUp() const { return m_defers >= kMaxTrips; }

    // the running model's best on-time speed (median ms of an on-time window), < 0 = not known: a step down is priced on it
    double OnTimeMs() const { return m_onTimeMs; }
    // the last verdict, for the log
    int LastLate() const { return m_lastLate; }
    int LastSeen() const { return m_lastSeen; }
    double LastWindowS() const { return m_lastS; }
    double LastMedianMs() const { return m_lastMedian; }   // < 0: no late time known (pictures)
    double LastWaitS() const { return m_lastWait; }        // the back-off of the last trip or refusal
    int Trips() const { return m_trips; }
    bool WasOnTime() const { return m_onTime; }

private:
    static double Median(std::vector<double> v)
    {
        if (v.empty()) return -1;
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return v[v.size() / 2];
    }
    Trip Sample(bool onTime, double tS, double frameMs, double nowS, double ms)
    {
        if (GaveUp() || nowS < m_deferUntil) return Trip::None;
        m_tripPending = false;
        if (m_waitFirst) {
            if (m_waitStart < -1e299 || tS < m_waitStart) m_waitStart = tS;
            if (!onTime && tS - m_waitStart < kFirstPictureMaxS) return Trip::None;
            m_waitFirst = false;
            return Trip::None;   // the grace (set by Restart) runs from the next sample
        }
        if (m_grace > 0) { --m_grace; return Trip::None; }
        // a step back, or a gap (a stall or a pause without a flush): the window starts over
        if (m_seen == 0 || tS < m_last || tS - m_last > kMaxGapS) { m_start = tS; m_seen = m_late = 0; m_delays.clear(); m_ms.clear(); }
        m_last = tS;
        ++m_seen;
        if (ms >= 0 && m_ms.size() < 4096) m_ms.push_back(ms);
        if (!onTime) { ++m_late; if (ms >= 0 && m_delays.size() < 4096) m_delays.push_back(ms); }
        if (tS - m_start < kWindowS || m_seen < kMinPictures) return Trip::None;
        m_lastSeen = m_seen; m_lastLate = m_late; m_lastS = tS - m_start;
        const bool trip = m_late * 2 > m_seen;
        m_start = tS; m_seen = m_late = 0;
        if (!trip) {
            m_onTime = true;
            const double med = Median(m_ms);
            if (med >= 0) m_onTimeMs = m_onTimeMs < 0 ? med : std::min(m_onTimeMs, med);
            if (m_trips > 0 && nowS - m_lastTripAt >= kTripDecayS) m_trips = 0;
            m_delays.clear(); m_ms.clear();
            return Trip::None;
        }
        ++m_trips;
        m_lastMedian = Median(m_delays);
        m_delays.clear(); m_ms.clear();
        const bool transient = m_onTime || m_lastMedian < 0 || m_lastMedian <= kSlowFactor * frameMs;
        const bool real = !transient || m_trips >= kMaxTrips;
        double s = kRealRearmS;
        if (!real) { s = kRearmFirstS; for (int i = 1; i < m_trips; ++i) s *= 2; }
        m_lastWait = s;
        m_retryAt = nowS + s;
        m_lastTripAt = nowS;
        m_tripPending = true;
        Restart();
        return real ? Trip::Real : Trip::Transient;
    }

    int m_grace = kGracePictures, m_seen = 0, m_late = 0, m_trips = 0, m_lastSeen = 0, m_lastLate = 0, m_defers = 0;
    double m_start = 0, m_last = 0, m_lastS = 0, m_lastMedian = -1, m_lastWait = 0, m_retryAt = -1, m_deferUntil = -1;
    double m_lastTripAt = -1e300, m_onTimeMs = -1, m_waitStart = -1e300;
    bool m_onTime = false, m_tripPending = false, m_waitFirst = false;
    std::vector<double> m_delays, m_ms;
};

// The next cheaper ranked anime model that fits Auto's share of the frame, -1 = none. onTimeMs: what the running model
// took per source frame here while it kept up (host transfer included; AnimeKeepUp::OnTimeMs): it sets this card's
// factor against the running model's prior; < 0 (never on time here): the prior (card 1.0), one rank at a time. Never
// a load-inflated time. Only models of the same scale (the output size stays) and cheaper by their prior; the priors
// are not changed.
inline int AnimeSrStepDown(int running, double onTimeMs, unsigned srcW, unsigned srcH, unsigned outW, unsigned outH, double fps)
{
    if (running < 0 || running >= kAnimeSrModels) return -1;
    const int base = AnimeSrModel(running).variantOf >= 0 ? AnimeSrModel(running).variantOf : running;
    const double inMpx = double(srcW) * srcH / 1e6, outMpx = double(outW) * outH / 1e6;
    const double prior = AnimeSrModel(base).msPerMpx * inMpx;
    const double net = onTimeMs - kAnimeSrTransferMsPerMpx * (inMpx + outMpx);
    const double card = onTimeMs > 0 && prior > 0 && net > 0 ? std::max(1.0, net / prior) : 1.0;
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
