// win_anime_route_test: Auto's anime upscaler decisions of the add-on (src/engine/win_anime_route.h), CPU only, no Kodi,
// no FFmpeg, no GPU. The measured class flipping between drawn, not drawn and drawn rendered CG gives the expected model
// (never a denoise + sharpen one on CG or live action), the route is changed at most once per 30 s, the output size
// stays fixed, and on Windows the plan never takes Video Super Resolution away.
#include "win_anime_route.h"
#include "kodi_engine.h"
#include "../../filter/src/PushMpcAnimeSr.h"
#include <cstdio>
#include <string>
#include <tuple>

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace pushmpc::content;

static std::string Id(int m) { const std::wstring w = pm::AnimeSrModel(m).id; return std::string(w.begin(), w.end()); }

// the model helpers agree with the model table: every "-ds" id is a DS variant of a model AnimeModelHasDs names
static void Models()
{
    int ds = 0;
    for (int i = 0; i < pm::kAnimeSrModels; ++i) {
        const std::string id = Id(i);
        const std::wstring label = pm::AnimeSrModel(i).label;
        const bool tableDs = label.size() > 3 && label.compare(label.size() - 3, 3, L" DS") == 0;
        CHECK(pmk::AnimeModelIsDs(id) == tableDs);
        if (tableDs) { ++ds; CHECK(pmk::AnimeModelHasDs(pmk::AnimeModelPlain(id))); CHECK(!pmk::AnimeModelIsDs(pmk::AnimeModelPlain(id))); }
        if (pmk::AnimeModelHasDs(id)) CHECK(pm::AnimeSrModelIndex(std::wstring(pm::AnimeSrModel(i).id) + L"-ds") >= 0);
    }
    CHECK(ds == 2);
    // the DrawnPolicy per measured kind
    const std::string m = "artcnn-c4f16";
    CHECK(pmk::AnimeModelFor(m, kDrawn, false, false) == m);            // hand-drawn
    CHECK(pmk::AnimeModelFor(m, kDrawn, false, true) == m + "-ds");     // hand-drawn, low bitrate: DS allowed
    CHECK(pmk::AnimeModelFor(m, kDrawn, true, true) == m);              // rendered CG: plain model, never DS
    CHECK(pmk::AnimeModelFor(m + "-ds", kDrawn, true, false) == m);     // a DS pick is made plain for CG
    for (int cls : {int(kUnknown), int(kCg), int(kLive), int(kGame)})
        for (bool low : {false, true}) CHECK(pmk::AnimeModelFor(m, cls, false, low).empty());   // not drawn: no anime upscaler
    CHECK(pmk::AnimeModelFor("cunny-fast", kDrawn, false, true) == "cunny-fast");                 // no DS variant to pick
    CHECK(pmk::AnimeModelFor("", kDrawn, false, false).empty());
}

// the switch over a stream: drawn -> live -> drawn CG -> hand-drawn, with the 30 s limit and the engine config each
// step gives (the output size never changes)
static void Switch()
{
    pmk::EngineCfg cfg; cfg.width = 1280; cfg.height = 720; cfg.upscaler = 1; cfg.outWidth = 2560; cfg.outHeight = 1440;
    cfg.animeAutoModel = "artcnn-c4f16"; cfg.animeAutoOutW = 2560; cfg.animeAutoOutH = 1440;
    pmk::AnimeAutoSwitch s;
    s.Arm("artcnn-c4f16-ds", true);   // armed with a DS id: kept plain, DS only where the policy allows
    CHECK(s.Armed() && s.Running().empty());
    int switches = 0;
    auto step = [&](int cls, bool cg, double t) {
        if (!s.Update(cls, cg, t)) return false;
        pmk::EngineCfg c2 = pmk::AnimeAutoConfig(cfg, !s.Next().empty());
        c2.animeModel = s.Next();
        CHECK(c2.outWidth == 2560 && c2.outHeight == 1440 && c2.upscaler == 1);   // fixed output size in both directions
        CHECK(!pmk::AnimeModelIsDs(c2.animeModel) || PolicyFor(cls, cg).animeUpscalerDs);   // DS only where the policy allows
        cfg = c2; s.Commit(t); ++switches;
        return true;
    };
    CHECK(!step(kUnknown, false, 0.5));                    // still measuring: nothing changes
    CHECK(step(kDrawn, false, 1.5));                       // first decision: at once
    CHECK(s.Running() == "artcnn-c4f16-ds" && cfg.animeModel == "artcnn-c4f16-ds");   // hand-drawn, low bitrate
    CHECK(!step(kDrawn, false, 5.0));                      // same class: no reopen
    CHECK(!step(kLive, false, 20.0));                      // live action within 30 s: held back
    CHECK(s.Running() == "artcnn-c4f16-ds");
    CHECK(step(kLive, false, 31.6));                       // asked again by a later state: off now
    CHECK(s.Running().empty() && cfg.animeModel.empty());
    CHECK(!step(kDrawn, true, 40.0));                      // rendered CG within 30 s: held back
    CHECK(step(kDrawn, true, 62.0));                       // then the plain model (never DS on CG)
    CHECK(s.Running() == "artcnn-c4f16" && cfg.animeModel == "artcnn-c4f16");
    CHECK(!step(kDrawn, false, 80.0));                     // hand-drawn again within 30 s: held back
    CHECK(step(kDrawn, false, 92.5));                      // the DS variant may run now
    CHECK(s.Running() == "artcnn-c4f16-ds");
    CHECK(step(kGame, false, 130.0));                      // game footage: off
    CHECK(s.Running().empty());
    CHECK(switches == 5);

    // a normal bitrate source never gets DS; a failed reopen stops further switches
    pmk::AnimeAutoSwitch n;
    n.Arm("artcnn-c4f32");
    CHECK(n.Update(kDrawn, false, 2.0) && n.Next() == "artcnn-c4f32");
    n.Disarm();
    CHECK(!n.Armed() && !n.Update(kLive, false, 100.0) && !n.Update(kDrawn, false, 200.0));
    pmk::AnimeAutoSwitch none;
    none.Arm("");
    CHECK(!none.Armed() && !none.Update(kDrawn, false, 1.0));
}

// Windows: the plan keeps Video Super Resolution and a fixed output size
static void Windows()
{
    // Kodi's rule: the add-on's x2 anime picture of a 1080p source turns Video Super Resolution off on a 4K screen (Option
    // B), the source itself keeps it
    CHECK(!pmk::KodiVsrSuitable(1920 * 2, 3840));
    CHECK(pmk::KodiVsrSuitable(1920, 3840));
    CHECK(pmk::KodiVsrSuitable(1280 * 2, 3840) && !pmk::KodiVsrSuitable(3840, 3840));

    pmk::WinAnimeIn in;
    in.srcW = 1920; in.srcH = 1080; in.screenW = 3840; in.screenH = 2160; in.autoModel = "artcnn-c4f16-ds";
    in.pushcore = true; in.pushcoreSr = true;
    pmk::WinAnimePlan p = pmk::PlanWindowsAnime(in);
    CHECK(p.route == pmk::WinUpscale::PushcoreSr && p.outW == 3840 && p.outH == 2160 && p.vsr);
    CHECK(p.autoModel == "artcnn-c4f16");                  // armed plain
    // the class flips: pushcore's output size is the plan's, whatever runs in its upscaler slot
    pmk::AnimeAutoSwitch s;
    s.Arm(p.autoModel);
    for (auto [cls, cg, t] : {std::tuple{int(kDrawn), false, 2.0}, {int(kLive), false, 40.0}, {int(kDrawn), true, 80.0}, {int(kLive), false, 120.0}}) {
        CHECK(s.Update(cls, cg, t));
        s.Commit(t);
        const bool anime = !s.Running().empty();
        CHECK(anime == (cls == kDrawn));                   // the anime model while drawn, Video Super Resolution otherwise
        CHECK(!pmk::AnimeModelIsDs(s.Running()));          // normal bitrate: never DS
        CHECK(p.outW == 3840 && p.outH == 2160);
    }
    // anamorphic 4:3 on 16:9: aspect kept, even
    in.dar = 4.0 / 3.0;
    p = pmk::PlanWindowsAnime(in);
    CHECK(p.outW == 2880 && p.outH == 2160);
    in.dar = 0;

    // a route that keeps the source size: Kodi's scaler (Video Super Resolution), no automatic anime upscaler
    in.pushcoreSr = false;
    p = pmk::PlanWindowsAnime(in);
    CHECK(p.route == pmk::WinUpscale::KodiScaler && p.autoModel.empty() && p.vsr && p.outW == 0);
    CHECK(pmk::KodiVsrSuitable(in.srcW, in.screenW));      // what Kodi then gets: the source, Video Super Resolution on
    in.pushcore = false;
    p = pmk::PlanWindowsAnime(in);
    CHECK(p.route == pmk::WinUpscale::KodiScaler && p.autoModel.empty() && p.vsr);
    // nothing to do: no model, the user's setting, the screen not larger, the screen not known
    in.pushcore = in.pushcoreSr = true;
    pmk::WinAnimeIn x = in; x.autoModel.clear();
    CHECK(pmk::PlanWindowsAnime(x).route == pmk::WinUpscale::KodiScaler);
    x = in; x.upscale = 1;
    CHECK(pmk::PlanWindowsAnime(x).route == pmk::WinUpscale::KodiScaler);
    x = in; x.screenW = 1920; x.screenH = 1080;
    CHECK(pmk::PlanWindowsAnime(x).route == pmk::WinUpscale::KodiScaler);
    x = in; x.screenW = 0;
    CHECK(pmk::PlanWindowsAnime(x).route == pmk::WinUpscale::KodiScaler);
}

int main()
{
    Models();
    Switch();
    Windows();
    std::printf(g_fail ? "win_anime_route_test: %d of %d check(s) failed\n" : "win_anime_route_test: all %d checks passed\n", g_fail ? g_fail : g_checks, g_checks);
    return g_fail ? 1 : 0;
}
