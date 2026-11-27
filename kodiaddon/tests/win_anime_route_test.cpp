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
    CHECK(pmk::AnimeModelFor(m, kDrawn, false, true) == m);            // hand-drawn, low bitrate: plain too (DS never automatic)
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
    CHECK(s.Running() == "artcnn-c4f16" && cfg.animeModel == "artcnn-c4f16");   // hand-drawn, low bitrate: the plain model
    CHECK(!step(kDrawn, false, 5.0));                      // same class: no reopen
    CHECK(!step(kLive, false, 20.0));                      // live action within 30 s: held back
    CHECK(s.Running() == "artcnn-c4f16");
    CHECK(step(kLive, false, 31.6));                       // asked again by a later state: off now
    CHECK(s.Running().empty() && cfg.animeModel.empty());
    CHECK(!step(kDrawn, true, 40.0));                      // rendered CG within 30 s: held back
    CHECK(step(kDrawn, true, 62.0));                       // then the plain model (never DS on CG)
    CHECK(s.Running() == "artcnn-c4f16" && cfg.animeModel == "artcnn-c4f16");
    CHECK(!step(kDrawn, false, 80.0));                     // hand-drawn again within 30 s: held back
    CHECK(!step(kDrawn, false, 92.5));                     // hand-drawn after CG: the same plain model, no switch
    CHECK(s.Running() == "artcnn-c4f16");
    CHECK(step(kGame, false, 130.0));                      // game footage: off
    CHECK(s.Running().empty());
    CHECK(switches == 4);   // hand-drawn after CG keeps the plain model (no DS): no switch there

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

// the pushcore route's updates (pm_pc_update) from the measured class: the anime model through AnimeAutoSwitch (30 s),
// the drawn treatment on every class / CG change (10 s apart at most), nothing while measuring; the route opens at x1
// only for its upscaler
static void PushcoreUpdates()
{
    pmk::WinAnimePlan up; up.route = pmk::WinUpscale::PushcoreSr;
    pmk::WinAnimePlan keep;
    CHECK(pmk::PushcoreWorth(0, keep) && pmk::PushcoreWorth(2, keep) && !pmk::PushcoreWorth(1, keep));   // Smoother off: only to upscale
    CHECK(pmk::PushcoreWorth(1, up));                                                                     // x1 with pushcore's upscaler

    pmk::AnimeAutoSwitch sw;
    sw.Arm("artcnn-c4f16");
    pmk::PushcoreContentGate g;
    std::string running;
    auto step = [&](int cls, bool cg, double t, bool ok = true) {
        const pmk::PushcoreContentGate::Ask a = g.Update(cls, cg, t, sw, true, running);
        if (a.update) { g.Done(a, ok, t, sw); if (ok) running = a.model; }
        return a;
    };
    CHECK(!step(kUnknown, false, 0.5).update);                     // measuring: nothing
    pmk::PushcoreContentGate::Ask a = step(kDrawn, false, 2.0);    // drawn: the anime model and the drawn treatment at once
    CHECK(a.update && a.switched && a.model == "artcnn-c4f16" && running == "artcnn-c4f16");
    CHECK(!step(kDrawn, false, 3.0).update);                       // the same class: nothing
    a = step(kDrawn, true, 5.0);                                   // CG measured 3 s later: held back (10 s), the model stays plain
    CHECK(!a.update);
    a = step(kDrawn, true, 12.5);                                  // ... then given (the treatment changes, the model does not)
    CHECK(a.update && !a.switched && a.cg && a.model == "artcnn-c4f16" && !pmk::AnimeModelIsDs(running));
    a = step(kLive, false, 20.0);                                  // live 7.5 s later: the switch waits 30 s, the class 10 s
    CHECK(!a.update);
    a = step(kLive, false, 32.6);                                  // class gap over, the anime switch not yet (30.6 s after 2.0)
    CHECK(a.update && a.switched && a.model.empty() && running.empty());   // 30 s since the model went on: both
    a = step(kDrawn, false, 40.0);
    CHECK(!a.update);                                              // 7.4 s after the last update
    a = step(kDrawn, false, 70.0);
    CHECK(a.update && a.switched && a.model == "artcnn-c4f16");
    // a refused anime model (pushcore cannot run it): the switch is disarmed, the class still goes on
    pmk::AnimeAutoSwitch sw2; sw2.Arm("cunny-fast");
    pmk::PushcoreContentGate g2;
    a = g2.Update(kDrawn, false, 1.0, sw2, true, "");
    CHECK(a.update && a.switched && a.model == "cunny-fast");
    g2.Done(a, false, 1.0, sw2);
    CHECK(!sw2.Armed());
    a = g2.Update(kLive, false, 20.0, sw2, true, "");
    CHECK(a.update && !a.switched && a.model.empty());
    // no upscaler on the route (source size): the class alone
    pmk::AnimeAutoSwitch sw3; sw3.Arm("artcnn-c4f16");
    pmk::PushcoreContentGate g3;
    a = g3.Update(kDrawn, false, 1.0, sw3, false, "");
    CHECK(a.update && !a.switched && a.model.empty());
}

// held frames on the pushcore route follow the measured class (DrawnPolicy holdRepair), at most once per 30 s, the first
// time at once; PUSHMPC_KODI_DRAWN overrides
static void PushcoreDrawn()
{
    CHECK(pmk::KodiDrawnOverride(nullptr) == -1 && pmk::KodiDrawnOverride("") == -1);
    CHECK(pmk::KodiDrawnOverride("0") == 0 && pmk::KodiDrawnOverride("1") == 1);
    pmk::AnimeAutoSwitch sw;   // not armed: the class alone
    pmk::PushcoreContentGate g;
    g.SetDrawn(false, -1);
    auto step = [&](int cls, bool cg, double t, bool ok = true) {
        const pmk::PushcoreContentGate::Ask a = g.Update(cls, cg, t, sw, false, "");
        if (a.update) g.Done(a, ok, t, sw);
        return a;
    };
    pmk::PushcoreContentGate::Ask a = step(kUnknown, false, 0.5);
    CHECK(!a.update && !a.drawnChanged && !g.Drawn());                 // measuring: nothing
    a = step(kLive, false, 1.0);
    CHECK(a.update && !a.drawnChanged && !a.drawn && !g.Drawn());       // live action: the class goes in, no held frames
    a = step(kDrawn, false, 12.0);
    CHECK(a.update && a.drawnChanged && a.drawn && g.Drawn());          // hand-drawn: held frames at once (first switch)
    a = step(kDrawn, true, 25.0);
    CHECK(a.update && !a.drawnChanged && g.Drawn());                    // drawn CG: held frames stay (holdRepair)
    a = step(kLive, false, 36.0);
    CHECK(a.update && !a.drawnChanged && a.drawn && g.Drawn());         // live 24 s after the switch: the class goes, held frames wait
    a = step(kLive, false, 42.5);
    CHECK(a.update && a.drawnChanged && !a.drawn && !g.Drawn());        // 30.5 s after the switch: off
    a = step(kDrawn, false, 50.0);
    CHECK(!a.update && !g.Drawn());                                     // drawn 7.5 s later: held back (both gaps)
    a = step(kDrawn, false, 73.0);
    CHECK(a.update && a.drawnChanged && g.Drawn());                     // 30.5 s after the last switch: on
    // a refused switch (no room for the ring): tried again 30 s later
    pmk::PushcoreContentGate f;
    f.SetDrawn(false, -1);
    a = f.Update(kDrawn, false, 1.0, sw, false, "");
    CHECK(a.drawnChanged && a.drawn);
    f.Done(a, false, 1.0, sw);
    CHECK(!f.Drawn());
    a = f.Update(kDrawn, false, 15.0, sw, false, "");
    CHECK(!a.drawnChanged && !a.update);
    a = f.Update(kDrawn, false, 31.5, sw, false, "");
    CHECK(a.drawnChanged && a.drawn && a.update);
    // the override: 0 never, 1 always (from the open)
    pmk::PushcoreContentGate off; off.SetDrawn(false, 0);
    CHECK(!off.Update(kDrawn, false, 1.0, sw, false, "").drawnChanged && !off.WantDrawn(kDrawn, false));
    pmk::PushcoreContentGate on; on.SetDrawn(true, 1);
    a = on.Update(kLive, false, 1.0, sw, false, "");
    CHECK(!a.drawnChanged && a.drawn && on.WantDrawn(kLive, false));
    // not drawn per the policy: live action, game footage
    CHECK(!g.WantDrawn(kLive, false) && !g.WantDrawn(kGame, false) && g.WantDrawn(kDrawn, true));
}

int main()
{
    Models();
    Switch();
    Windows();
    PushcoreUpdates();
    PushcoreDrawn();
    std::printf(g_fail ? "win_anime_route_test: %d of %d check(s) failed\n" : "win_anime_route_test: all %d checks passed\n", g_fail ? g_fail : g_checks, g_checks);
    return g_fail ? 1 : 0;
}
