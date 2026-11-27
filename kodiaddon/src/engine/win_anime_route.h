// win_anime_route: Auto's anime upscaler in the Kodi add-on as pure logic (no Kodi, FFmpeg or D3D), so it is tested on
// the CPU (tests/win_anime_route_test.cpp). Two parts:
//   AnimeAutoSwitch   every OS: which anime model runs, from the class ContentDetect measures in the decoded picture and
//                     its DrawnPolicy (pushmpc/content_detect.h), never from names; the route is changed at most once per
//                     30 s and never to a denoise + sharpen (_DS) model unless the policy allows it.
//   PlanWindowsAnime  Windows: where Auto's anime upscaler can run without losing Video Super Resolution.
// Next to the driver's Video Super Resolution (RTX / Intel VSR) Auto's anime upscaler waits for the phase-4 verdict, as
// in every player (content_detect.h kAnimeSrBeforeDriverVsr / AutoAnimeSrBesideVsr). Which upscaler pushcore runs is
// known once it is open: where that is a driver VSR the model is not armed (PushcoreAnimeAllowed, the add-on's TryPushcore).
//
// Why Windows differs. Kodi's DXVA renderer runs RTX / Intel Video Super Resolution only on a picture at most 2560 wide
// and narrower than its back buffer, decided when the renderer is configured for the stream (Kodi's
// CProcessorHD::IsSuperResolutionSuitable, called from CRendererDXVA's configure step). So:
//   - the anime upscaler in the add-on at x2, the rest left to Kodi: a 1080p source becomes 3840 wide, and Kodi turns
//     Video Super Resolution off for the whole stream, the live-action shots too; making the picture size follow the
//     class instead would reconfigure Kodi's renderer mid-stream;
//   - pushcore's own upscaler slot at a fixed output size: Video Super Resolution (RTX VSR; Intel VSR / FSR on other
//     GPUs) while the picture is not drawn, the anime model (PUSHCORE_SR_METHOD_PMML) while it is, at the same output
//     size (pushcore rebuilds its graph for a new SR method at the same SR target). This keeps both, and is the plan
//     here. It needs the pushcore route to run pushcore's upscaler (WinAnimeIn::pushcoreSr); a route that keeps the
//     source size keeps Kodi's scaler, and the anime upscaler stays a setting there.
#pragma once
#include <cmath>
#include <string>
#include "../../../pushmpc/include/pushmpc/content_detect.h"

namespace pmk {

// ---------------------------------------------------------------------------------------------------- models
// pmml's model ids (filter/src/PushMpcAnimeSr.h): the denoise + sharpen variants end in "-ds"; only the two ArtCNN C
// models have one
inline bool AnimeModelIsDs(const std::string& id) { return id.size() > 3 && id.compare(id.size() - 3, 3, "-ds") == 0; }
inline std::string AnimeModelPlain(const std::string& id) { return AnimeModelIsDs(id) ? id.substr(0, id.size() - 3) : id; }
inline bool AnimeModelHasDs(const std::string& plain) { return plain == "artcnn-c4f16" || plain == "artcnn-c4f32"; }

// the model Auto runs for this measured picture: "" (the plain route) unless the policy turns the anime upscaler on;
// the _DS variant only where the policy allows it and the source is low bitrate (never today: kAnimeUpscalerDsAuto)
inline std::string AnimeModelFor(const std::string& autoModel, int cls, bool cg, bool lowBitrate)
{
    const pushmpc::content::DrawnPolicy& p = pushmpc::content::PolicyFor(cls, cg);
    if (!p.animeUpscaler || autoModel.empty()) return {};
    const std::string plain = AnimeModelPlain(autoModel);
    return p.animeUpscalerDs && lowBitrate && AnimeModelHasDs(plain) ? plain + "-ds" : plain;
}

// ---------------------------------------------------------------------------------------------------- the switch
// Follows the measured class: Update with every state ContentDetect reports; when it returns true the route should run
// Next() ("" = the plain route); then Commit (switched) or Disarm (could not: no more switches this stream). A switch
// waits while the last one is less than kMinGapS old (the engine or pushcore's graph is rebuilt each time); the first
// one is at once. While the class is still being measured nothing changes.
class AnimeAutoSwitch {
public:
    static constexpr double kMinGapS = 30.0;

    // the model prepared for this stream ("" = no automatic anime upscaler); lowBitrate: the source's measured bits per
    // pixel are low (the _DS variant may then run on hand-drawn pictures)
    void Arm(const std::string& autoModel, bool lowBitrate = false) { m_auto = AnimeModelPlain(autoModel); m_low = lowBitrate; m_running.clear(); m_next.clear(); m_switched = false; }
    void Disarm() { m_auto.clear(); m_next = m_running; }
    // the anime model stopped for the stream (it could not keep up): nothing runs, nothing is armed
    void Stop() { m_auto.clear(); m_running.clear(); m_next.clear(); }
    bool Armed() const { return !m_auto.empty(); }
    const std::string& Running() const { return m_running; }
    const std::string& Next() const { return m_next; }

    bool Update(int cls, bool cg, double nowS)
    {
        if (m_auto.empty() || cls == pushmpc::content::kUnknown) return false;
        const std::string want = AnimeModelFor(m_auto, cls, cg, m_low);
        if (want == m_running) { m_next = m_running; return false; }
        if (m_switched && nowS - m_at < kMinGapS) return false;   // rate limit: asked again by the next state
        m_next = want;
        return true;
    }
    void Commit(double nowS) { m_running = m_next; m_switched = true; m_at = nowS; }

private:
    std::string m_auto, m_running, m_next;
    bool m_low = false, m_switched = false;
    double m_at = 0;
};

// ---------------------------------------------------------------------------------------------------- Windows plan
// Kodi's own rule for Video Super Resolution on a picture of this width (CProcessorHD::IsSuperResolutionSuitable; HDR on
// Intel aside): at most 2560 wide and narrower than Kodi's back buffer
inline bool KodiVsrSuitable(int pictureW, int backBufferW) { return pictureW <= 2560 && backBufferW > pictureW; }

enum class WinUpscale { KodiScaler, PushcoreSr };

struct WinAnimeIn {
    int srcW = 0, srcH = 0;        // the stored picture
    double dar = 0;                // display aspect ratio (0: srcW / srcH)
    int screenW = 0, screenH = 0;  // Kodi's screen (0: not known)
    int upscale = 0;               // the Upscaling setting (0 Auto; the others are the user's and decided elsewhere)
    std::string autoModel;         // Auto's anime model for this source ("" none: no runtime, none fits, too large)
    bool pushcore = false;         // the pushcore route plays this stream
    bool pushcoreSr = false;       // ... and runs pushcore's own upscaler at a fixed output size, SR method switchable mid-stream
};

struct WinAnimePlan {
    WinUpscale route = WinUpscale::KodiScaler;
    int outW = 0, outH = 0;        // PushcoreSr: the fixed output size (the screen, aspect kept; even)
    std::string autoModel;         // PushcoreSr: the plain model AnimeAutoSwitch is armed with
    bool vsr = true;               // Video Super Resolution stays for the pictures that are not drawn
    std::string why;
};

inline WinAnimePlan PlanWindowsAnime(const WinAnimeIn& in)
{
    WinAnimePlan p;
    if (in.upscale != 0) { p.why = "upscaling: your setting"; return p; }
    if (in.srcW <= 0 || in.srcH <= 0 || in.screenW <= 0 || in.screenH <= 0) {
        p.why = "upscaling: Kodi's video scaler (screen size not known)"; return p;
    }
    const double dar = in.dar > 0 ? in.dar : double(in.srcW) / in.srcH;
    int ow = in.screenW, oh = int(std::lround(in.screenW / dar));
    if (oh > in.screenH) { oh = in.screenH; ow = int(std::lround(in.screenH * dar)); }
    ow &= ~1; oh &= ~1;
    if (ow <= in.srcW * 11 / 10 && oh <= in.srcH * 11 / 10) { p.why = "upscaling: the screen is not larger than the video: none"; return p; }
    const std::string base = "upscaling: Kodi's video scaler (RTX / Intel Video Super Resolution when enabled in Kodi)";
    if (in.autoModel.empty()) { p.why = base; return p; }
    if (!in.pushcore) {
        p.why = base + "; the anime upscaler is a setting on this route (in the add-on it would turn Kodi's Video Super Resolution off)";
        return p;
    }
    if (!in.pushcoreSr) {
        p.why = base + "; the anime upscaler is a setting here (pushcore's bridge keeps the source size, so pushcore's own upscaler does not run)";
        return p;
    }
    p.route = WinUpscale::PushcoreSr;
    p.outW = ow; p.outH = oh;
    p.autoModel = AnimeModelPlain(in.autoModel);
    p.why = "upscaling: pushcore to " + std::to_string(ow) + "x" + std::to_string(oh) +
            ": Video Super Resolution, the anime upscaler " + p.autoModel + " once the picture is measured as drawn animation";
    return p;
}

// pushcore's SR method (pm_pc_info.sr_method, PUSHCORE_SR_METHOD_*) is the driver's Video Super Resolution: RTX VSR (0),
// Intel VSR (4), RTX VSR through ZLUDA (5)
inline bool PushcoreSrDriverVsr(bool sr, unsigned method) { return sr && (method == 0 || method == 4 || method == 5); }
// after pushcore opened its upscaler: whether Auto's anime model may be armed beside what it runs (the shared rule)
inline bool PushcoreAnimeAllowed(bool sr, unsigned method) { return pushmpc::content::AutoAnimeSrBesideVsr(PushcoreSrDriverVsr(sr, method)); }

// the pushcore route is worth opening: for frame generation, or for its upscaler (then also at x1, Smoother off: it is the
// only place the anime upscaler runs without taking Video Super Resolution away)
// Auto's anime upscaler keeps up: it is given up for the stream when it took longer than a whole source frame for
// kAnimeLateFrames frames in a row (the cost Auto picked it by is a prior: card 1.0, nothing learned on these routes).
// Feed it every source frame's upscaler time (ms, < 0 = none); true once it is to be stopped.
inline constexpr int kAnimeLateFrames = 12;   // half a second at 24 fps: a run, not a single slow frame
class AnimeLateness {
public:
    bool Update(double animeMs, double frameMs)
    {
        if (animeMs < 0 || frameMs <= 0) { m_run = 0; return false; }
        m_run = animeMs > frameMs ? m_run + 1 : 0;
        return m_run >= kAnimeLateFrames;
    }
    void Reset() { m_run = 0; }
private:
    int m_run = 0;
};

// The same on the pushcore route, where the anime model runs inside pushcore: of the pictures expected from pushcore,
// more than half missed over a window of kAnimeMissWindow means it does not keep up (VLC's output upscaler uses the same
// rule, pm_pcsr_core.h). Feed every picture that was to come from pushcore; true once the anime model is to be stopped.
inline constexpr int kAnimeMissWindow = 48;
class AnimeMisses {
public:
    bool Update(bool delivered)
    {
        ++m_seen; if (!delivered) ++m_missed;
        if (m_seen < kAnimeMissWindow) return false;
        const bool stop = m_missed * 2 > m_seen;
        m_seen = m_missed = 0;
        return stop;
    }
    void Reset() { m_seen = m_missed = 0; }
private:
    int m_seen = 0, m_missed = 0;
};

inline bool PushcoreWorth(int smoother, const WinAnimePlan& plan) { return smoother != 1 || plan.route == WinUpscale::PushcoreSr; }

// ---------------------------------------------------------------------------------------------------- pushcore updates
// The pushcore route's settings follow the measured class through the bridge's update (pm_pc_update: a warm reload
// each, the frames in flight dropped): the anime model through AnimeAutoSwitch (at most once per 30 s), the drawn
// treatment's settings (line art, sign hold, the anime deband preset: Auto in the bridge) whenever the class or its
// CG flag changes, at most once per kMinGapS. Held frames themselves (re-timing: every picture then comes from pushcore,
// up to 14 source frames late, in a ring about 3.5 times larger) run only while the picture is measured as drawn
// (DrawnPolicy holdRepair: hand-drawn and drawn CG), switched at most once per kDrawnGapS (the first time at once);
// the override (PUSHMPC_KODI_DRAWN: 0 off, 1 on from the open) replaces the measurement. While the class is still
// being measured nothing changes.
// modelIndex: the model id as the bridge takes it (PUSHCORE_ANIME_SR_*, the order of PushMpcAnimeSr.h), -1 = none.
class PushcoreContentGate {
public:
    static constexpr double kMinGapS = 10.0;
    static constexpr double kDrawnGapS = 30.0;
    struct Ask { bool update = false; bool switched = false; int cls = 0; bool cg = false; std::string model; bool drawn = false; bool drawnChanged = false; };

    // running: whether the route runs the drawn treatment now; drawnOverride: -1 follow the measured class, 0 off, 1 on
    void SetDrawn(bool running, int drawnOverride) { m_drawn = running; m_override = drawnOverride; }
    bool Drawn() const { return m_drawn; }
    // the drawn treatment wanted for a measured class
    bool WantDrawn(int cls, bool cg) const { return m_override >= 0 ? m_override != 0 : pushmpc::content::PolicyFor(cls, cg).holdRepair; }

    // upscales: the route runs pushcore's upscaler (the anime model can change); running: the model pushcore runs ("")
    Ask Update(int cls, bool cg, double nowS, AnimeAutoSwitch& sw, bool upscales, const std::string& running)
    {
        Ask a;
        if (cls == pushmpc::content::kUnknown) return a;
        a.cls = cls; a.cg = cg; a.model = running; a.drawn = m_drawn;
        if (upscales && sw.Armed() && sw.Update(cls, cg, nowS)) { a.switched = true; a.model = sw.Next(); }
        const bool want = WantDrawn(cls, cg);
        if (want != m_drawn && (!m_drawnTried || nowS - m_drawnAt >= kDrawnGapS)) { a.drawn = want; a.drawnChanged = true; }
        const bool changed = !m_have || cls != m_cls || cg != m_cg;
        a.update = a.switched || a.drawnChanged || (changed && (!m_have || nowS - m_at >= kMinGapS));
        return a;
    }
    // after the update was tried (ok: pushcore runs it); a refused switch disarms the anime switch for the stream; a
    // refused drawn change is tried again kDrawnGapS later
    void Done(const Ask& a, bool ok, double nowS, AnimeAutoSwitch& sw)
    {
        if (a.switched) { if (ok) sw.Commit(nowS); else sw.Disarm(); }
        if (a.drawnChanged) { if (ok) m_drawn = a.drawn; m_drawnTried = true; m_drawnAt = nowS; }
        m_have = true; m_cls = a.cls; m_cg = a.cg; m_at = nowS;
    }

private:
    bool m_have = false;
    int m_cls = 0;
    bool m_cg = false;
    double m_at = 0;
    bool m_drawn = false, m_drawnTried = false;
    int m_override = -1;
    double m_drawnAt = 0;
};

// PUSHMPC_KODI_DRAWN: unset -1 (the measured class decides), "0" off, anything else on
inline int KodiDrawnOverride(const char* env) { return !env || !*env ? -1 : env[0] == '0' ? 0 : 1; }

} // namespace pmk
