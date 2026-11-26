// win_anime_route: Auto's anime upscaler in the Kodi add-on as pure logic (no Kodi, FFmpeg or D3D), so it is tested on
// the CPU (tests/win_anime_route_test.cpp). Two parts:
//   AnimeAutoSwitch   every OS: which anime model runs, from the class ContentDetect measures in the decoded picture and
//                     its DrawnPolicy (pushmpc/content_detect.h), never from names; the route is changed at most once per
//                     30 s and never to a denoise + sharpen (_DS) model unless the policy allows it.
//   PlanWindowsAnime  Windows: where Auto's anime upscaler can run without losing Video Super Resolution.
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
// the _DS variant only where the policy allows it (hand-drawn) and the source is low bitrate
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

} // namespace pmk
