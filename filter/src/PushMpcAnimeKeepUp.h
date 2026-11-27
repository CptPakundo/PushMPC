#pragma once
// PushMPC Auto: whether Auto's anime upscaler keeps up where its cost is only a prior (card 1.0: the Kodi add-on, the
// VapourSynth plugin), and what runs when it does not. Portable (no Windows types), pure logic: tested on the CPU
// (kodiaddon/tests/win_anime_route_test.cpp). The rule and its constants are the ones of VLC's output upscaler
// (vlcplugin/pushcore_bridge/pm_pcsr_core.h PcsrLateRule): one rule, one list for the free-PC perf run.
//
// AnimeKeepUp, one per stream. Feed one sample per source frame (the upscaler's time against the frame's) or per
// picture expected from pushcore (on time or missed), with its source time:
//  - grace: the first kGracePictures after an open, a model switch, a settings update (a reload drops the pictures in
//    flight), a flush or a seek are not judged (a route that holds pictures longer asks for more);
//  - sustained: a verdict covers at least kWindowS of SOURCE time and kMinPictures samples with no step larger than
//    kMaxGapS, and trips when more than half of them were late;
//  - after a trip, transient (a whole window was on time before, or the late samples' median stayed within kSlowFactor
//    x the frame time): the plain route for kRearmFirstS, doubling per trip, then the model again; the kMaxTrips-th trip
//    in a stream, or a real one (never on time, far over): a cheaper ranked model that fits at the measured speed
//    (AnimeSrStepDown), else the plain route for the stream;
//  - a change the route could not make (the engine or pushcore refused it; the model keeps running): no verdict for the
//    same back-off, none at all after kMaxTrips refusals (Defer).
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
    static constexpr int kMinPictures = 24;    // a verdict needs this many samples too (sparse samples never decide)
    static constexpr double kMaxGapS = 1.0;    // a larger step between two samples starts the window over

    enum class Trip { None, Transient, Real };

    // a new model (a step down, a new stream): its own trips, back-off and "was on time", and the grace
    void NewModel()
    {
        m_trips = 0; m_onTime = false; m_retryAt = -1; m_defers = 0; m_deferUntil = -1;
        Restart();
    }
    // an open, a switch, a settings update, a flush or a seek: the grace again, the window starts over (the trips and
    // "was on time" stay). grace: more samples than kGracePictures where the route holds pictures longer
    void Restart(int grace = 0)
    {
        m_grace = std::max(kGracePictures, grace);
        m_seen = m_late = 0; m_start = m_last = 0; m_delays.clear();
    }

    // the upscaler's time for the source frame at tS (s) (ms, < 0 = none ran) against the frame's (ms)
    Trip FeedMs(double ms, double frameMs, double tS, double nowS)
    {
        if (ms < 0 || frameMs <= 0) return Trip::None;
        return Sample(ms <= frameMs, tS, frameMs, nowS, ms);
    }
    // a picture at source time tS (s) expected from pushcore: on time (delivered) or not; no time is known for it
    Trip FeedPicture(bool onTime, double tS, double frameMs, double nowS) { return Sample(onTime, tS, frameMs, nowS, -1.0); }

    // the back-off after a transient trip has run out: the model may run again (once; the grace starts over)
    bool RetryDue(double nowS)
    {
        if (m_retryAt < 0 || nowS < m_retryAt) return false;
        m_retryAt = -1;
        Restart();
        return true;
    }
    bool Waiting() const { return m_retryAt >= 0; }

    // the change a trip asked for was refused (the model keeps running): no verdict for the back-off, none after kMaxTrips
    void Defer(double nowS)
    {
        ++m_defers;
        double s = kRearmFirstS;
        for (int i = 1; i < m_defers; ++i) s *= 2;
        m_deferUntil = nowS + s;
        m_lastWait = s;
        Restart();
    }
    bool GaveUp() const { return m_defers >= kMaxTrips; }

    // the last verdict, for the log
    int LastLate() const { return m_lastLate; }
    int LastSeen() const { return m_lastSeen; }
    double LastWindowS() const { return m_lastS; }
    double LastMedianMs() const { return m_lastMedian; }   // < 0: no late time known (pictures)
    double LastWaitS() const { return m_lastWait; }        // the back-off of the last transient trip or refusal
    int Trips() const { return m_trips; }
    bool WasOnTime() const { return m_onTime; }

private:
    Trip Sample(bool onTime, double tS, double frameMs, double nowS, double ms)
    {
        if (GaveUp() || nowS < m_deferUntil) return Trip::None;
        if (m_grace > 0) { --m_grace; return Trip::None; }
        // a step back, or a gap (a stall or a pause without a flush): the window starts over
        if (m_seen == 0 || tS < m_last || tS - m_last > kMaxGapS) { m_start = tS; m_seen = m_late = 0; m_delays.clear(); }
        m_last = tS;
        ++m_seen;
        if (!onTime) { ++m_late; if (ms >= 0 && m_delays.size() < 4096) m_delays.push_back(ms); }
        if (tS - m_start < kWindowS || m_seen < kMinPictures) return Trip::None;
        m_lastSeen = m_seen; m_lastLate = m_late; m_lastS = tS - m_start;
        const bool trip = m_late * 2 > m_seen;
        m_start = tS; m_seen = m_late = 0;
        if (!trip) { m_onTime = true; m_delays.clear(); return Trip::None; }
        ++m_trips;
        m_lastMedian = -1;
        if (!m_delays.empty()) {
            std::nth_element(m_delays.begin(), m_delays.begin() + m_delays.size() / 2, m_delays.end());
            m_lastMedian = m_delays[m_delays.size() / 2];
        }
        const bool transient = m_onTime || (m_lastMedian >= 0 && m_lastMedian <= kSlowFactor * frameMs);
        Restart();
        if (!transient || m_trips >= kMaxTrips) return Trip::Real;
        double s = kRearmFirstS;
        for (int i = 1; i < m_trips; ++i) s *= 2;
        m_lastWait = s;
        m_retryAt = nowS + s;
        return Trip::Transient;
    }

    int m_grace = kGracePictures, m_seen = 0, m_late = 0, m_trips = 0, m_lastSeen = 0, m_lastLate = 0, m_defers = 0;
    double m_start = 0, m_last = 0, m_lastS = 0, m_lastMedian = -1, m_lastWait = 0, m_retryAt = -1, m_deferUntil = -1;
    bool m_onTime = false;
    std::vector<double> m_delays;
};

// The next cheaper ranked anime model that fits Auto's share of the frame at the measured speed, -1 = none. measuredMs:
// what the running model takes per source frame here (host transfer included); it sets this card's factor against the
// running model's prior, and the cheaper models are costed with that factor (PickAnimeSr's own cost and share; the
// priors are not changed). Only models of the same scale (the output size stays) and cheaper by their prior.
inline int AnimeSrStepDown(int running, double measuredMs, unsigned srcW, unsigned srcH, unsigned outW, unsigned outH, double fps)
{
    if (running < 0 || running >= kAnimeSrModels || measuredMs <= 0) return -1;
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
