// Headless check of the Auto rules (no GPU): prints the decision for a few sources, displays and GPU families.
#include "../src/PushMpcAuto.h"
#include "../src/PushMpcLearned.h"
#include "../src/PushMpcExtras.h"
#include "../src/PushMpcShows.h"
#include "../../pushmpc/include/pushmpc/unlocks.h"
#include "../../pushmpc/include/pushmpc/dlssg_route.h"
#include <cstdio>
#include <random>
using namespace pm;
static void Run(const char* what, Source s, Display d, Settings st = {}, Gpu g = {}, Costs c = {})
{
    const Choice r = Decide(st, s, d, c, g);
    wprintf(L"%-40hs %ls M=%d q=%d SR=%d L%d%ls %ls style %d out %ux%u NR=%d HDR=%d%ls DB=%d  %.1f/%.1f ms\n    smoother: %ls\n    stopped: %ls\n    sharper: %ls | clean: %ls | hdr: %ls\n    cut guard: %ls\n",
        what, FgMethodLabel(r.fgMethod), r.multiplier, r.fgQuality, r.sr, r.srLevel, r.artifactReduction ? L" (AR)" : L"", UpscalerStr(r.srMethod), r.srFamily,
        r.outW, r.outH, r.nr, r.hdr, r.hdrItm ? L" (ITM)" : L"", r.deband, r.predictedMs, r.budgetMs,
        r.smootherNote.c_str(), r.stoppedAt.c_str(), r.sharperNote.c_str(), r.cleanNote.c_str(), r.hdrNote.c_str(), r.guardNote.c_str());
}
static Gpu Family(uint32_t f, const wchar_t* name)
{
    Gpu g; g.family = f; g.name = name; g.available = 0;
    auto on = [&](uint32_t c) { g.available |= 1u << c; };
    const bool rtx = f >= kRtx50 && f <= kRtx20, nv = f >= kRtx50 && f <= kGtx;
    if (f == kRtx50 || f == kRtx40 || f == kRtx30) on(kCapVfg);
    if (rtx && f != kRtx20) on(kCapDlssg);
    on(kCapPushFlow); on(kCapFsrFg); on(kCapFsrSr); on(kCapNis);
    if (f == kRdna4) on(kCapFsr4Fg);
    if (!nv && f != kIntel) on(kCapFrc);
    if (rtx) { on(kCapRtxVsr); on(kCapVsrFamilies); on(kCapRtxHdr); on(kCapNr); }
    if (f == kRdna4 || f == kRdna3) on(kCapNr);
    for (auto& r : g.reason) r = L"not on this GPU";
    return g;
}
static uint32_t CoreFgFor(FgMethod m) { return uint32_t(int(m) - 1); }   // the filter's / vsplugin's mapping onto PUSHCORE_FG_*
int main()
{
    Display d4k{ L"4K 240", 239.96, 3840, 2160, false };
    Display d4kVrr = d4k; d4kVrr.vrr = true; d4kVrr.fullscreen = true;   // G-SYNC fullscreen-only: VRR means the player is fullscreen
    Display d60{ L"60", 60.0, 1920, 1080, false };
    Display d144{ L"1440p144", 144.0, 2560, 1440, false };
    Display d120{ L"4K 120", 119.88, 3840, 2160, false };
    Run("1080p23.976 Blu-ray, 240 Hz fixed", { 1920, 1040, 23.976, false, 0.5 }, d4k);
    Run("1080p23.976 Blu-ray, 240 Hz VRR", { 1920, 1040, 23.976, false, 0.5 }, d4kVrr);
    Display d4kBattery = d4k; d4kBattery.battery = true;
    Run("on battery (Light), 240 Hz", { 1920, 1040, 23.976, false, 0.12 }, d4kBattery);
    Run("1080p23.976 web rip, 240 Hz", { 1920, 1080, 23.976, false, 0.12 }, d4k);
    Run("1080p60 -> 4K 240", { 1920, 1080, 59.94, false, 0.3 }, d4k);
    Run("4K23.976 remux", { 3840, 1608, 23.976, false, 0.45 }, d4k);
    Run("1080p23.976 on 60 Hz", { 1920, 1080, 23.976, false, 0.5 }, d60);
    Run("1080p23.976 on 1440p144", { 1920, 1080, 23.976, false, 0.5 }, d144);
    Run("1080p23.976 on 4K 120", { 1920, 1080, 23.976, false, 0.5 }, d120);
    Display hdr = d4k; hdr.hdr = true;
    Run("1080p23.976 SDR, Windows HDR on", { 1920, 1080, 23.976, false, 0.5 }, hdr);
    Run("SDR web rip 0.22, Windows HDR on", { 1920, 1080, 23.976, false, 0.22 }, hdr);
    Settings rtxHdr; rtxHdr.hdrMethod = HdrMethod::Rtx;
    Run("HDR method RTX Video HDR (user)", { 1920, 1080, 23.976, false, 0.12 }, hdr, rtxHdr);
    Settings boostOff; boostOff.hdrBoost = HdrBoost::Off;
    Run("HDR boost Off (user)", { 1920, 1080, 23.976, false, 0.5 }, hdr, boostOff);
    Settings noLimit; noLimit.hdrLimit = false;
    Run("bright-scene limit off (user)", { 1920, 1080, 23.976, false, 0.5 }, hdr, noLimit);
    Settings high; high.fgQuality = FgQuality::High;
    Run("FG quality High (user), 240 fixed", { 1920, 1040, 23.976, false, 0.5 }, d4k, high);
    Settings dl; dl.fgMethod = FgMethod::Dlssg;
    Run("method DLSS-G (user), VRR", { 1920, 1040, 23.976, false, 0.5 }, d4kVrr, dl);
    Costs costly; costly.card = 6.0;
    Run("RTX 30 (3060-class priors), 240 fixed", { 1920, 1080, 23.976, false, 0.5 }, d4k, {}, Family(kRtx30, L"RTX 3060"), costly);
    Costs slow; slow.card = 12.0;
    Run("RTX 30 too slow for VFG 2x", { 1920, 1080, 23.976, false, 0.5 }, d4k, {}, Family(kRtx30, L"RTX 3050"), slow);
    Run("RTX 20, 144 Hz", { 1920, 1080, 23.976, false, 0.5 }, d144, {}, Family(kRtx20, L"RTX 2080"), Costs{ .card = 4.0 });
    Run("GTX 1080, 60 Hz", { 1920, 1080, 23.976, false, 0.5 }, d60, {}, Family(kGtx, L"GTX 1080"), Costs{ .card = 6.0 });
    Run("RX 9070 XT, 4K 240, Windows HDR", { 1920, 1080, 23.976, false, 0.12 }, hdr, {}, Family(kRdna4, L"RX 9070 XT"), Costs{ .card = 2.2 });
    Run("RX 7900, 1440p144", { 1920, 1080, 23.976, false, 0.5 }, d144, {}, Family(kRdna3, L"RX 7900"), Costs{ .card = 2.4 });
    Run("Intel Arc, 60 Hz", { 1920, 1080, 23.976, false, 0.5 }, d60, {}, Family(kIntel, L"Arc A770"), Costs{ .card = 5.0 });
    // ---- F1: G-SYNC scope = min(application mode 0x1194F158, global mode 0x1094F1F7), missing = driver default 1
    wprintf(L"\n[VRR scope] mode/request -> windowed | fullscreen\n");
    const int cases[][2] = { { -1, -1 }, { 1, 1 }, { 2, 2 }, { 2, 1 }, { 1, 2 }, { 0, 2 }, { 2, 0 }, { 2, -1 }, { -1, 2 } };
    for (const auto& c : cases) {
        VrrProbe w; w.mode = c[0]; w.request = c[1]; VrrDecide(w, true, true, false);
        VrrProbe f = w; VrrDecide(f, true, true, true);
        wprintf(L"  %2d/%2d scope %d: windowed %-3ls | fullscreen %-3ls  (%ls)\n", c[0], c[1], w.scope, w.active ? L"VRR" : L"off", f.active ? L"VRR" : L"off", f.how.c_str());
    }
    { VrrProbe n; n.mode = 2; n.request = 2; VrrDecide(n, false, false, true); wprintf(L"  2/2 on a fixed-refresh monitor: %ls (%ls)\n", n.active ? L"VRR" : L"off", n.how.c_str()); }
    // ---- AMD FreeSync (ADL, PushFPS 4.5.3 PushFreeSync.h): the NVIDIA rule with the driver default scope (fullscreen)
    wprintf(L"\n[VRR AMD FreeSync] capStatus/caps/stateStatus/useCases -> windowed | fullscreen\n");
    const int amd[][4] = { { 0, 12, 0, 2 }, { 0, 12, 0, 0 }, { 0, 4, 0, 2 }, { 0, 0, 0, 0 }, { -1, 0, 0, 0 }, { 0, 12, -1, 2 } };
    for (const auto& c : amd) {
        const FreeSyncState f = FreeSyncFrom(c[0], c[1], c[2], c[3]);
        VrrProbe w; VrrDecideAmd(w, f, false); VrrProbe fs; VrrDecideAmd(fs, f, true);
        wprintf(L"  %2d/%2d/%2d/%d: windowed %-3ls | fullscreen %-3ls  (%ls)\n", c[0], c[1], c[2], c[3], w.active ? L"VRR" : L"off", fs.active ? L"VRR" : L"off", fs.how.c_str());
    }
    {
        // the user's state: global 1/1 inherited by mpc-hc64.exe
        VrrProbe w; w.mode = 1; w.request = 1; VrrDecide(w, true, true, false);
        VrrProbe f = w; VrrDecide(f, true, true, true);
        Display dw = d4k; dw.vrr = w.active; Display df = d4k; df.vrr = f.active;
        const Choice cw = Decide({}, { 1920, 1040, 23.976, false, 0.5 }, dw, {}, {}), cf = Decide({}, { 1920, 1040, 23.976, false, 0.5 }, df, {}, {});
        wprintf(L"  user's profile 1/1, clip A: windowed %dx %.1f fps (VRR %d), fullscreen %dx %.1f fps (VRR %d)  [expected 10x / 10x]\n",
                cw.multiplier, 23.976 * cw.multiplier, int(dw.vrr), cf.multiplier, 23.976 * cf.multiplier, int(df.vrr));
    }

    // ---- F8: first decision from the first timestamps (media type 29.97, timestamps 24.0)
    wprintf(L"\n[source interval] F8: media type 29.97 fps, timestamps 24.000 fps\n");
    {
        StepTracker t; long long pts = 0; int n = 0;
        const Choice byType = Decide({}, { 1920, 1080, 29.97, false, 0.26 }, d4k, {}, {});
        while (t.base <= 0) { t.Add(pts, n == 0, 1.0); pts += 416667; n++; }
        const double fps = 1000.0 / t.Step(1.0);
        const Choice byPts = Decide({}, { 1920, 1080, fps, false, 0.26 }, d4k, {}, {});
        wprintf(L"  measured after %d samples: %.3f ms (%.3f fps); Auto by type %dx (%.1f fps), by timestamps %dx (%.1f fps)  [expected 5x 120]\n",
                n, t.Step(1.0), fps, byType.multiplier, 29.97 * byType.multiplier, byPts.multiplier, fps * byPts.multiplier);
        // jitter (screen capture at 59.94 with +-4 ms timestamp jitter)
        StepTracker j; std::mt19937 rng(7); std::uniform_real_distribution<double> u(-40000.0, 40000.0); n = 0;
        while (j.base <= 0) { j.Add((long long)(n * 166833.0 + u(rng)), n == 0, 1.0); n++; }
        wprintf(L"  59.94 with +-4 ms jitter: %.3f ms (%.3f fps) after %d samples\n", j.Step(1.0), 1000.0 / j.Step(1.0), n);
        for (double jit : { 5000.0, 10000.0 }) { StepTracker k; std::uniform_real_distribution<double> uj(-jit, jit); int m = 0; while (k.base <= 0) { k.Add((long long)(m * 166833.0 + uj(rng)), m == 0, 1.0); m++; } wprintf(L"  59.94 with +-%.1f ms jitter: %.3f ms (%.3f fps)\n", jit / 1e4, k.Step(1.0), 1000.0 / k.Step(1.0)); }
        // a real change of frame rate (24 -> 30) is re-measured once, after ~3 windows; a short burst is not
        StepTracker r; pts = 0; int ev2 = -1;
        for (n = 0; n < 40; n++) { r.Add(pts, n == 0, 1.0); pts += 417083; }
        for (int k = 0; k < 20; k++) { r.Add(pts, false, 1.0); pts += 333667; }
        const double afterBurst = r.Step(1.0);
        for (int k = 0; k < 140 && ev2 < 0; k++) { if (r.Add(pts, false, 1.0) == StepTracker::Remeasured) ev2 = k; pts += 333667; }
        wprintf(L"  24 fps, 20 steps at 30 fps: %.3f ms kept; sustained 30 fps: re-measured %.3f ms after %d more steps\n", afterBurst, r.Step(1.0), ev2);
    }

    // ---- F9: playback rate (the interval is divided by the rate at once; one decision per change)
    wprintf(L"\n[playback rate] F9: clip A 23.976, 240 Hz fixed and VRR\n");
    {
        StepTracker t; long long pts = 0;
        for (int n = 0; n < 9; n++) { t.Add(pts, n == 0, 1.0); pts += 417083; }
        const Choice fresh = Decide({}, { 1920, 1040, 23.976, false, 0.5 }, d4k, {}, {});
        for (double rate : { 2.0, 0.5, 1.0 }) {
            const double fps = 1000.0 / t.Step(rate);
            const Choice c = Decide({}, { 1920, 1040, fps, false, 0.5 }, d4k, {}, {}), v = Decide({}, { 1920, 1040, fps, false, 0.5 }, d4kVrr, {}, {});
            wprintf(L"  %.1fx: source %.3f fps -> fixed %dx %.1f fps | VRR %dx %.1f fps  (%ls)\n", rate, fps, c.multiplier, fps * c.multiplier, v.multiplier, fps * v.multiplier, c.stoppedAt.c_str());
            // the timestamps at that rate do not move the interval (no re-decision from them)
            for (int n = 0; n < 100; n++) { if (t.Add(pts, n == 0, rate) != StepTracker::None) wprintf(L"    unexpected re-measure at %.1fx\n", rate); pts += (long long)(417083 / rate); }
            if (rate == 1.0) wprintf(L"  back at 1.0x: %dx, fresh start: %dx  [%ls]\n", c.multiplier, fresh.multiplier, c.multiplier == fresh.multiplier ? L"same" : L"DIFFERENT");
        }
    }

    // ---- F4: diagnostics anonymisation (linear scan; the RC1 crash was std::regex_error on a long log)
    wprintf(L"\n[anonymise] F4\n");
    {
        wchar_t prof[MAX_PATH] = {}, user[256] = {}; DWORD un = 256;
        GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH); GetUserNameW(user, &un);
        std::wstring log = L"PushMPC diagnostics\r\n";
        for (int i = 0; i < 400; i++)
            log += std::format(L"08:09:{:02}.{:03} [1234] opened \"E:\\Users\\{}\\Videos\\Shows, Movies and More\\Some Show\\Some Show - Episode 2   [SomeGroup].mkv\" and {}\\Videos\\clip {}.MP4; "
                               L"name OtherShow.S01E01.avi; core {}\\pushcore.dll; VSR 1.264 ms, 0.329 H.264-equivalent; C:\\Program Files\\MPC-HC\\mpc-hc64.exe; E:\\Users\\{}\\Downloads\\x; \\\\nas\\share\\movie.m2ts\r\n",
                               i % 60, i, user, prof, i, prof, user);
        log += std::wstring(20000, L'x') + L".mkv\r\n" + std::wstring(5000, L'a') + L" b.mkv c";
        const auto t0 = GetTickCount64();
        const std::wstring a = Anonymise(log);
        const auto ms = GetTickCount64() - t0;
        auto has = [&](const std::wstring& n) { return !n.empty() && a.find(n) != std::wstring::npos; };
        const size_t line2 = a.find(L"\r\n") + 2;
        wprintf(L"  %zu chars in %llu ms; user name %ls, profile %ls, 'SomeGroup' %ls, 'OtherShow' %ls, 'clip 3' %ls, 'movie' %ls, Program Files kept %ls, '1.264 ms' kept %ls, 'H.264' kept %ls\n",
                log.size(), ms, has(user) ? L"LEFT" : L"gone", has(prof) ? L"LEFT" : L"gone", has(L"SomeGroup") ? L"LEFT" : L"gone", has(L"OtherShow") ? L"LEFT" : L"gone",
                has(L"clip 3") ? L"LEFT" : L"gone", has(L"movie") ? L"LEFT" : L"gone", has(L"C:\\Program Files\\MPC-HC") ? L"yes" : L"NO", has(L"1.264 ms") ? L"yes" : L"NO", has(L"H.264-equivalent") ? L"yes" : L"NO");
        wprintf(L"  line 1: %ls\n", a.substr(line2, a.find(L"\r\n", line2) - line2).c_str());
        // round 2: the project's repository URL survives (its owner name may contain the user name); the rest does not
        const std::wstring repo = pm::anon::RepoKey();
        const std::wstring r = Anonymise(std::format(L"feed https://{}/releases/latest/download/release.json; HTTPS://{}; user {} at {}\\x; host ok", repo, repo, user, prof));
        size_t keptN = 0; for (size_t p = 0; (p = r.find(repo, p)) != std::wstring::npos; p += repo.size()) ++keptN;
        std::wstring rest = r; for (size_t p; (p = rest.find(repo)) != std::wstring::npos;) rest.erase(p, repo.size());
        wprintf(L"  repository URL kept %zu of 2 (case-insensitive match restores the canonical spelling), user name outside it %ls, profile %ls  [%ls]\n",
                keptN, rest.find(user) != std::wstring::npos ? L"LEFT" : L"gone", r.find(prof) != std::wstring::npos ? L"LEFT" : L"gone",
                keptN == 2 && rest.find(user) == std::wstring::npos && r.find(prof) == std::wstring::npos ? L"PASS" : L"FAIL");
    }

    // round 2: the host player from the exe name (MPC-BE support; window class and reopen per host)
    {
        const bool ok = HostFromExe(L"C:\\Program Files\\MPC-HC\\mpc-hc64.exe") == Host::MpcHc && HostFromExe(L"E:\\x\\MPC-BE x64\\MPC-BE64.EXE") == Host::MpcBe &&
                        HostFromExe(L"PotPlayerMini64.exe") == Host::PotPlayer && HostFromExe(L"auto_test.exe") == Host::Other &&
                        wcscmp(HostWindowClass(Host::MpcBe), L"MPC-BE") == 0 && wcscmp(HostWindowClass(Host::MpcHc), L"MediaPlayerClassicW") == 0 &&
                        HostCanReopen(Host::MpcBe) && !HostCanReopen(Host::PotPlayer) && HostWindowClass(Host::Other) == nullptr;
        wprintf(L"\n[host player] mpc-hc64 / MPC-BE64 / PotPlayerMini64 / other, window classes, reopen  [%ls]; this process: %ls\n", ok ? L"PASS" : L"FAIL", HostName());
    }
    // ---- T5b vendor paths: weak-GPU cold start, the measured card factor, the unlock gates
    wprintf(L"\n[vendor paths] T5b\n");
    {
        int fails = 0;
        auto check = [&](bool ok, const wchar_t* what, const std::wstring& detail = L"") { wprintf(L"  %-74ls [%ls]%ls%ls\n", what, ok ? L"PASS" : L"FAIL", detail.empty() ? L"" : L"  ", detail.c_str()); fails += ok ? 0 : 1; };
        const Source film{ 1920, 1080, 23.976, false, 0.5 };
        // weak GPUs: FSR FG on the cold start, then the speed test decides
        check(IsWeakGpu(kGtx, L"GTX 1060") && IsWeakGpu(kRdna1, L"RX 5700") && IsWeakGpu(kIntel, L"Intel(R) Iris(R) Xe Graphics") && IsWeakGpu(kOtherGpu, L"Qualcomm(R) Adreno(TM) X1-85 GPU")
              && !IsWeakGpu(kIntel, L"Intel(R) Arc(TM) B580 Graphics") && !IsWeakGpu(kRdna3, L"RX 7600") && !IsWeakGpu(kRtx20, L"RTX 2060"), L"IsWeakGpu: GTX, RX 5000, Xe iGPU, Adreno weak; Arc, RX 7000, RTX not");
        Costs cold; cold.card = 6.0;
        const Choice c0 = Decide({}, film, d60, cold, Family(kGtx, L"GTX 1060"));
        check(c0.fgMethod == FgMethod::Fsr && c0.multiplier == 2 && c0.stoppedAt.find(L"cold start") != std::wstring::npos, L"GTX 1060 cold start, 60 Hz: FSR FG 2x (said so)", c0.smootherNote);
        Costs fastPf = cold; fastPf.pfBenched = true; fastPf.pfFirst[0] = 9; fastPf.pfFirst[1] = 12; fastPf.pfFirst[2] = 25; fastPf.pfNext[0] = 7; fastPf.pfNext[1] = 9; fastPf.pfNext[2] = 22;
        ApplyMeasuredCard(fastPf);
        const Choice c1 = Decide({}, film, d60, fastPf, Family(kGtx, L"GTX 1660 SUPER"));
        check(c1.fgMethod == FgMethod::PushFlow && c1.multiplier >= 2 && fastPf.cardMeasured, L"benched, PushFlow fits: PushFlow wins (measured card factor)", std::format(L"{} card {:.2f}", c1.smootherNote, fastPf.card));
        Costs slowPf = cold; slowPf.pfBenched = true; for (int t = 0; t < 3; ++t) { slowPf.pfFirst[t] = 60 + 20 * t; slowPf.pfNext[t] = 50 + 20 * t; }
        ApplyMeasuredCard(slowPf);
        const Choice c2 = Decide({}, film, d60, slowPf, Family(kGtx, L"GTX 1050 Ti"));
        check(c2.fgMethod == FgMethod::Fsr && c2.multiplier == 2, L"benched, PushFlow too slow: FSR FG 2x (by the benchmark)", c2.smootherNote + L" | " + c2.stoppedAt);
        const Choice c3 = Decide({}, film, d60, cold, Family(kIntel, L"Intel(R) Arc(TM) A770 Graphics"));
        check(c3.fgMethod == FgMethod::PushFlow, L"Arc (not weak): PushFlow first on the priors as before", c3.smootherNote);
        Settings forced; forced.fgMethod = FgMethod::PushFlow;
        const Choice c4 = Decide(forced, film, d60, cold, Family(kGtx, L"GTX 1060"));
        check(c4.fgMethod == FgMethod::PushFlow || c4.multiplier < 2, L"the user's method wins over the cold-start order", c4.smootherNote);
        Costs named; named.card = CardFactor(L"RX 7600", kRdna3);
        check(!named.cardMeasured && std::fabs(named.card - 6.0) < 1e-9 && !ApplyMeasuredCard(named), L"no speed test yet: the name table stays (RX 7600 = 6.0)");
        // RTX VSR on GTX 16 through the unlock: Auto picks it when the capability is available, with the GTX VSR prior
        Gpu g16 = Family(kGtx, L"GTX 1660 Ti"); g16.available |= 1u << kCapRtxVsr;
        const Choice c5 = Decide({}, film, d144, cold, g16);
        check(c5.srMethod == Upscaler::Rtx && c5.sr, L"GTX 16 with the Gtx16Vsr capability: RTX VSR", c5.sharperNote);
        // unlock decisions (pure)
        using namespace pmu;
        SelfTest none, pass, fail; pass.known = fail.known = true; pass.passed = true; fail.reason = L"output black";
        auto d1 = DecideUnlock(Unlock::Gtx16Vsr, UnlockMode::Auto, true, L"", none);
        check(!d1.on && d1.runSelfTest, L"Auto, eligible, no result: off, run the self-test", d1.why);
        auto d2 = DecideUnlock(Unlock::Gtx16Vsr, UnlockMode::Auto, true, L"", pass);
        check(d2.on && !d2.runSelfTest, L"Auto, self-test passed: on", d2.why);
        auto d3 = DecideUnlock(Unlock::Gtx16Vsr, UnlockMode::Auto, true, L"", fail);
        check(!d3.on && d3.why.find(L"output black") != std::wstring::npos, L"Auto, self-test failed: off, with the reason", d3.why);
        auto d4 = DecideUnlock(Unlock::Gtx16Vsr, UnlockMode::Off, true, L"", pass, false, L"PUSHMPC_DISABLE_GTX16VSR=1");
        check(!d4.on && d4.why.find(L"PUSHMPC_DISABLE_GTX16VSR") != std::wstring::npos, L"off switch beats a passed self-test", d4.why);
        auto d5 = DecideUnlock(Unlock::Gtx16Vsr, UnlockMode::On, true, L"", fail);
        check(d5.on && d5.why.find(L"FAILED") != std::wstring::npos, L"forced on (tester) even after a failed self-test, and said so", d5.why);
        auto d6 = DecideUnlock(Unlock::ZludaVsr, UnlockMode::Auto, false, L"needs an RX 7000 or RX 9000", pass);
        check(!d6.on && !d6.runSelfTest, L"not eligible: never on, no self-test", d6.why);
        check(EnvName(Unlock::Gtx16Vsr) == L"PUSHMPC_DISABLE_GTX16VSR" && EnvName(Unlock::SubtitleHoldAuto) == L"PUSHMPC_DISABLE_SUBTITLEHOLDAUTO", L"off-switch environment names");
        check(IsTu116(0x10DE, 0x2182) && IsTu117(0x10DE, 0x1F82) && !IsTu116(0x10DE, 0x2684) && IsGtx16(0x10DE, 0x0000, L"NVIDIA GeForce GTX 1660 Ti") && !IsGtx16(0x1002, 0x2182, L"x"),
              L"GTX 16 by device ID (TU116 1660 Ti, TU117 1650) and by name; RTX 4090 not");
        check(HasNvofa(0x10DE, 0x2182, false, L"") && !HasNvofa(0x10DE, 0x1F82, false, L"") && HasNvofa(0x10DE, 0x2684, true, L"") && !HasNvofa(0x10DE, 0x1B80, false, L"GTX 1080"),
              L"NVOFA: TU116 yes, TU117 no, RTX yes, Pascal no");
        check(ZludaEligibleFamily(6) && ZludaEligibleFamily(7) && !ZludaEligibleFamily(8) && !ZludaEligibleFamily(1), L"ZLUDA route: RX 9000 / 7000 only");
        // the self-test cache round trip, on a scratch registry root (HKCU\Software\PushMPC-auto_test, deleted after)
        {
            RegBase() = L"Software\\PushMPC-auto_test";
            const std::wstring key = SelfTestKey(0x10DE, 0x2182, L"32.0.15.9999");
            WriteSelfTest(Unlock::Gtx16Vsr, key, true, L"auto_test");
            const SelfTest a = ReadSelfTest(Unlock::Gtx16Vsr, key), b = ReadSelfTest(Unlock::Gtx16Vsr, SelfTestKey(0x10DE, 0x2182, L"32.0.15.1"));
            RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\PushMPC-auto_test");
            RegBase() = L"Software\\PushMPC";
            check(a.known && a.passed && !b.known, L"self-test cache: same key read back, another driver is stale");
        }
        {   // NVOF FG (T5b item 6): GTX 16 with NVOFA picks it over FSR FG; a GTX 1650 (no NVOFA cap) keeps FSR FG; RTX 20 after PushFlow
            Gpu g1660 = Family(kGtx, L"NVIDIA GeForce GTX 1660 Ti"); g1660.available |= 1u << kCapNvofFg;
            const Choice n1 = Decide({}, film, d60, cold, g1660);
            check(n1.fgMethod == FgMethod::Nvof && n1.multiplier == 2, L"GTX 1660 Ti cold start: NVOF FG 2x (NVIDIA flow hardware + FSR interpolation)", n1.smootherNote);
            const Choice n2 = Decide({}, film, d60, cold, Family(kGtx, L"NVIDIA GeForce GTX 1650"));
            check(n2.fgMethod == FgMethod::Fsr, L"GTX 1650 (no NVOFA): FSR FG", n2.smootherNote);
            Gpu g2060 = Family(kRtx20, L"NVIDIA GeForce RTX 2060"); g2060.available |= 1u << kCapNvofFg;
            Costs slow2060 = cold; slow2060.card = 8.0; slow2060.pfBenched = true; for (int t = 0; t < 3; ++t) { slow2060.pfFirst[t] = 70 + 10 * t; slow2060.pfNext[t] = 60 + 10 * t; }
            const Choice n3 = Decide({}, film, d60, slow2060, g2060);
            check(n3.fgMethod == FgMethod::Nvof, L"RTX 2060, PushFlow measured too slow: NVOF FG (before FSR FG)", n3.smootherNote + L" | " + n3.stoppedAt);
            // RTX 20 with the community DLSS-G port available (self-test passed or pending): DLSS-G first, above 2x where
            // it fits; without the port (kCapDlssg off) the order is unchanged
            Gpu g2080 = Family(kRtx20, L"NVIDIA GeForce RTX 2080 Ti"); g2080.available |= 1u << kCapDlssg;
            const Choice n4 = Decide({}, film, d144, cold, g2080);
            check(n4.fgMethod == FgMethod::Dlssg && n4.multiplier >= 2, L"RTX 2080 Ti with the DLSS-G port: DLSS-G", n4.smootherNote + L" | " + n4.stoppedAt);
            const Choice n5 = Decide({}, film, d144, cold, Family(kRtx20, L"NVIDIA GeForce RTX 2080 Ti"));
            check(n5.fgMethod != FgMethod::Dlssg, L"RTX 2080 Ti without the DLSS-G port: not DLSS-G", n5.smootherNote);
            // RTX 30: Transfusion or the community port per open (pmu::ChooseDlssgRoute)
            using pmu::DlssgRoute;
            auto route = [](auto f) { pmu::DlssgRouteInputs in; in.transfusionUsable = true; in.portUsable = true; f(in); return pmu::ChooseDlssgRoute(in); };
            check(route([](auto& in) { in.portUsable = false; in.transfusion.runs = 3; in.transfusion.msPerGen = 2.0; }).route == DlssgRoute::Transfusion, L"RTX 30 without the port: Transfusion");
            check(route([](auto&) {}).route == DlssgRoute::Transfusion, L"RTX 30 with the port, nothing measured: Transfusion (default)");
            check(route([](auto& in) { in.transfusion.runs = 1; in.transfusion.msPerGen = 2.0; }).route == DlssgRoute::Port, L"RTX 30, Transfusion measured, port self-test pending: the port (its self-test)");
            check(route([](auto& in) { in.portPassed = true; in.transfusion.runs = 1; in.transfusion.msPerGen = 2.0; }).route == DlssgRoute::Port, L"RTX 30, port passed but not measured: the port (measuring it)");
            check(route([](auto& in) { in.portPassed = true; in.transfusion = { 4, 0, 0, 2.0 }; in.port = { 4, 0, 0, 1.5 }; }).route == DlssgRoute::Port, L"RTX 30, the port measured cheaper: the port");
            check(route([](auto& in) { in.portPassed = true; in.transfusion = { 4, 0, 0, 1.5 }; in.port = { 4, 0, 0, 2.0 }; }).route == DlssgRoute::Transfusion, L"RTX 30, Transfusion measured cheaper: Transfusion");
            check(route([](auto& in) { in.portPassed = true; in.transfusion = { 4, 0, 0, 2.0 }; in.port = { 4, 0, 0, 1.98 }; }).route == DlssgRoute::Transfusion, L"RTX 30, near tie (< 3 %): Transfusion stays");
            check(route([](auto& in) { in.portPassed = true; in.transfusion = { 4, 0, 0, 2.0 }; in.port = { 4, 0, 1, 1.0 }; }).route == DlssgRoute::Transfusion, L"RTX 30, the port crashed once: Transfusion");
            check(route([](auto& in) { in.portPassed = true; in.transfusion = { 4, 0, 1, 1.0 }; in.port = { 4, 0, 0, 2.0 }; }).route == DlssgRoute::Port, L"RTX 30, Transfusion crashed once: the port");
            check(route([](auto& in) { in.transfusion = { 0, 0, 1, 0 }; }).route == DlssgRoute::Port, L"RTX 30, Transfusion crashed before any measurement: the port's self-test");
            check(route([](auto& in) { in.transfusionUsable = false; }).route == DlssgRoute::Port, L"RTX 30, Transfusion not usable: the port");
            { const auto c = route([](auto& in) { in.portPassed = true; in.wantLite = true; }); check(c.route == DlssgRoute::Port && c.lite, L"RTX 30, FG Lite asked for, port passed: the port with preset C"); }
            { const auto c = route([](auto& in) { in.wantLite = true; }); check(c.route == DlssgRoute::Transfusion && !c.lite, L"RTX 30, FG Lite asked for, port not tested: Transfusion, no Lite"); }
            { const auto c = route([](auto& in) { in.portPassed = true; in.wantLite = true; in.latch = DlssgRoute::Transfusion; }); check(c.route == DlssgRoute::Transfusion && !c.lite, L"RTX 30, process latched to Transfusion: Transfusion even for FG Lite"); }
            check(route([](auto& in) { in.latch = DlssgRoute::Port; in.portPassed = true; in.transfusion = { 4, 0, 0, 1.0 }; in.port = { 4, 0, 0, 3.0 }; }).route == DlssgRoute::Port, L"RTX 30, process latched to the port: the port");
            // FG Lite as an Auto tier: only where pushcore offers it, and only when it reaches a higher multiplier
            {
                Gpu g30 = Family(kRtx30, L"NVIDIA GeForce RTX 3060"); g30.available &= ~(1u << kCapVfg);
                Gpu g30lite = g30; g30lite.available |= 1u << kCapDlssgLite;
                bool liteSeen = false, liteBad = false, liteWithoutCap = false;
                for (double card = 1.0; card <= 40.0; card += 0.5) {
                    Costs k = cold; k.card = card;
                    const Choice std2 = Decide({}, film, d144, k, g30);
                    const Choice lite2 = Decide({}, film, d144, k, g30lite);
                    if (std2.fgMethod == FgMethod::Dlssg && std2.fgQuality == kDlssgLiteQ) liteWithoutCap = true;
                    if (lite2.fgMethod == FgMethod::Dlssg && lite2.fgQuality == kDlssgLiteQ) {
                        liteSeen = true;
                        if (!(lite2.multiplier > (std2.fgMethod == FgMethod::Dlssg ? std2.multiplier : 1))) liteBad = true;
                    }
                }
                check(liteSeen && !liteBad, L"RTX 30 with FG Lite: Auto takes it on weaker cards, only for a higher multiplier");
                check(!liteWithoutCap, L"RTX 30 without FG Lite (no port self-test): never FG Lite");
                check(FgKeyOf(FgMethod::Dlssg, kDlssgLiteQ) == kKeyDlssgLite && FgKeyOf(FgMethod::Dlssg, 0) == kKeyDlssg, L"FG Lite has its own learned cost key");
            }
            {   // FSR 4.1 INT8 upscaling: RX 6000 (SDK 2.4.0 / upscaler 4.1.2) gets the same INT8 prior as RX 7000
                Costs k = cold; k.card = 3.0;
                const Source sd{ 1280, 720, 23.976, false, 0.5 };
                const Choice r6 = Decide({}, sd, d144, k, Family(kRdna2, L"AMD Radeon RX 6800 XT"));
                const Choice r7 = Decide({}, sd, d144, k, Family(kRdna3, L"AMD Radeon RX 7800 XT"));
                check(r6.sr == r7.sr && std::abs(r6.predictedMs - r7.predictedMs) < 0.01, L"RX 6000 and RX 7000: the same FSR 4.1 INT8 prior", std::format(L"{:.2f} vs {:.2f} ms", r6.predictedMs, r7.predictedMs));
            }
            {   // HelixSR frame generation (kCapHelixFg): only where pushcore offers it; after PushFlow, before FSR FG; up to 6x
                const Source sd{ 1920, 1080, 23.976, false, 0.5 };
                Costs slow = cold; slow.card = 3.0; slow.pfBenched = true; for (int t = 0; t < 3; ++t) { slow.pfFirst[t] = 90; slow.pfNext[t] = 80; }
                Gpu rx = Family(kRdna2, L"AMD Radeon RX 6600"); Gpu rxH = rx; rxH.available |= 1u << kCapHelixFg;
                const Choice h0 = Decide({}, sd, d144, slow, rx), h1 = Decide({}, sd, d144, slow, rxH);
                check(h0.fgMethod != FgMethod::HelixFg, L"RX 6600 without HelixSR: never HelixSR FG", h0.smootherNote);
                check(h1.fgMethod == FgMethod::HelixFg && h1.multiplier >= 2, L"RX 6600, PushFlow measured too slow, HelixSR offered: HelixSR FG", h1.smootherNote + L" | " + h1.stoppedAt);
                check(h1.multiplier >= h0.multiplier, L"HelixSR FG reaches at least FSR FG's multiplier", std::format(L"{} vs {}", h1.multiplier, h0.multiplier));
                Costs fast = cold; fast.card = 0.5;
                check(Decide({}, sd, d144, fast, rxH).fgMethod == FgMethod::PushFlow, L"RX 6600 where PushFlow fits: PushFlow stays first", L"");
                check(MaxMultiplier(FgMethod::HelixFg) == 6 && FgKeyOf(FgMethod::HelixFg, 1) == kKeyHelixFg && CapOf(FgMethod::HelixFg) == kCapHelixFg, L"HelixSR FG: up to 6x, own cost key, own capability bit");
                check(FgMethodStr(FgMethod::HelixFg) == std::wstring(L"HELIXFG") && int(FgMethod::HelixFg) - 1 == 8, L"HelixSR FG maps to PUSHCORE_FG_HELIXFG (8)");
                // HLX FSR 4 on RX 5000: a higher FSR upscaler prior until measured, only where pushcore offers it
                const Source sd720{ 1280, 720, 23.976, false, 0.5 };
                Gpu r5 = Family(kRdna1, L"AMD Radeon RX 5700 XT"); Gpu r5h = r5; r5h.available |= 1u << kCapHlxFsr4;
                Costs k5 = cold; k5.card = 2.0; k5.pfBenched = true;
                const Choice s0 = Decide({}, sd720, d144, k5, r5), s1 = Decide({}, sd720, d144, k5, r5h);
                check(s0.sr && s1.sr && s1.srMethod == Upscaler::Fsr && s1.predictedMs >= s0.predictedMs, L"RX 5700 XT with HLX FSR 4: FSR upscaling, costed higher until measured",
                      std::format(L"{:.2f} vs {:.2f} ms", s1.predictedMs, s0.predictedMs));
            }
            Settings fm; fm.fgMethod = FgMethod::Nvof;
            check(CoreFgFor(FgMethod::Nvof) == 5u, L"FgMethod::Nvof maps to PUSHCORE_FG_NVOF (5)");
            (void)fm;
        }
        {   // Animation: measured only (ContentDetect's class, else the held-frame share), Off / On, DLSS-G behind PushFlow
            Source s; s.fps = 23.976; Display d; d.refreshHz = 240; Gpu g = Family(kRtx40, L"NVIDIA GeForce RTX 4090"); g.available &= ~(1u << kCapVfg);
            Source held = s; held.heldShare = 0.4; Source hint = s; hint.contentClass = 1; hint.contentConf = 0.8f; Source live = s; live.heldShare = 0.01;
            Source liveHeld = held; liveHeld.contentClass = 3; liveHeld.contentConf = 0.7f;   // held frames on live action (a still shot): the class wins
            Settings off; off.animation = kOff; Settings on; on.animation = 1;
            const Choice a = Decide({}, held, d, {}, g), b = Decide({}, live, d, {}, g), c2 = Decide({}, hint, d, {}, g), e = Decide(off, held, d, {}, g), f = Decide(on, live, d, {}, g);
            const Choice lh = Decide({}, liveHeld, d, {}, g);
            check(!a.animation && !b.animation && c2.animation && !lh.animation && !e.animation && f.animation, L"Animation Auto: the measured class only (held share alone: off until measured); Off / On fixed",
                  a.animNote + L" | " + c2.animNote + L" | " + lh.animNote);
            check(c2.fgMethod == FgMethod::PushFlow && b.fgMethod == FgMethod::Dlssg, L"animation without VFG: PushFlow before DLSS-G (live action keeps DLSS-G)",
                  std::wstring(FgMethodLabel(c2.fgMethod)) + L" / " + FgMethodLabel(b.fgMethod));
            // round 5: the animation mode (Auto = Full until the verdict; the user's choice for the show; the setting)
            Source tv = hint; Source picked = hint; picked.showChoice = 1;
            Settings cam; cam.animMode = 1; Settings noTrans; noTrans.holdTransients = kOff;
            const Choice m1 = Decide({}, tv, d, {}, g), m2 = Decide({}, picked, d, {}, g), m3 = Decide(cam, picked, d, {}, g), m4 = Decide(noTrans, live, d, {}, g), m5 = Decide({}, live, d, {}, g);
            check(m1.animMode == 0 && m2.animMode == 1 && m3.animMode == 1, L"animation mode: Auto = Full until the verdict, the user's choice for a show is kept, the setting wins", m1.animModeNote + L" | " + m2.animModeNote);
            check((m1.holdFlags & 1) && (m1.holdFlags & 2) && (m5.holdFlags & 1) && (m5.holdFlags & 8) && !(m5.holdFlags & 2) && m4.holdFlags == 0,
                  L"holds: transients on for every video (flash / impact also on live action), line art on animation only; Off turns them off", std::to_wstring(m1.holdFlags) + L" / " + std::to_wstring(m5.holdFlags));
            check(ShowKeyFor(L"media/Show/Season 1/e01.mkv") == ShowKeyFor(L"Media/show/season 2/e05.mkv") && ShowKeyFor(L"media/Show/S01/e01.mkv") == ShowKeyFor(L"media/Show/e09.mkv")
                  && ShowKeyFor(L"media/Show/e01.mkv") != ShowKeyFor(L"media/Other/e01.mkv") && ShowKeyFor(L"media/Show/e01.mkv").size() == 16,
                  L"show key: one per series folder (season folders folded, case-insensitive), a 16-digit hash", ShowKeyFor(L"media/Show/e01.mkv"));
            // round 5 (item 4): sign hold on animation; line art and sign hold go before a frame generation step
            {
                bool found = false, same = true;
                Settings noLine; noLine.lineArt = kOff;
                for (double card = 1.0; card < 40.0 && !found; card += 0.05) {
                    Costs cc; cc.card = card;
                    const Choice with = Decide({}, tv, d, cc, g);
                    if (with.animModeNote.find(L"would cost a frame generation step") != std::wstring::npos) {
                        found = true; same = with.multiplier == Decide(noLine, tv, d, cc, g).multiplier && !(with.holdFlags & 6u);
                    }
                }
                check((m1.holdFlags & 4) && !(m5.holdFlags & 4) && found && same,
                      L"sign hold Auto on animation only; line art + sign hold dropped where they would cost a frame generation step (same multiplier as without)", std::to_wstring(m1.holdFlags));
            }
            // round 5 (item 5): deband Auto on 8-bit animation whatever the bitrate (anime preset; Low from Blu-ray class);
            // live action keeps the bitrate rule; 10-bit animation stays off
            Source an = hint; an.bppEq = 0.3; Source anBd = hint; anBd.bppEq = 0.6; Source lv = live; lv.bppEq = 0.3; Source an10 = an; an10.tenBit = true;
            const Choice d1 = Decide({}, an, d, {}, g), d2 = Decide({}, anBd, d, {}, g), d3 = Decide({}, lv, d, {}, g), d4 = Decide({}, an10, d, {}, g);
            check(d1.deband && d1.debandStrength == 3 && d2.deband && d2.debandStrength == 0 && !d3.deband && !d4.deband,
                  L"deband Auto: 8-bit animation on at any bitrate (anime preset, Low from 0.5 bpp), live action by bitrate, 10-bit off",
                  d1.debandNote + L" | " + d2.debandNote + L" | " + d3.debandNote + L" | " + d4.debandNote);
        }
        {   // Subtitle Hold: Auto = 2 (gated) when the core gates it and the unlock is not off; Off / On stay manual
            Settings a; Settings off; off.subtitleHold = kOff; Settings on; on.subtitleHold = 1;
            check(SubtitleHoldValue(a, true, true) == 2 && SubtitleHoldValue(a, false, true) == 0 && SubtitleHoldValue(a, true, false) == 0
                  && SubtitleHoldValue(off, true, true) == 0 && SubtitleHoldValue(on, false, false) == 1, L"Subtitle Hold: Auto gated (2) only with the core's gate and the unlock on; Off/On manual");
        }
        {   // NVIDIA VFG up to 16x (8x in the SDK multiplier mode, above in its timestep mode), the usual rules
            const Source clipA{ 1920, 1040, 23.976, false, 0.5 }, low15{ 1920, 1040, 15.0, false, 0.5 };
            const Display w240 = d4k, vrr240 = d4kVrr;
            const Display w480{ L"4K 480", 480.0, 3840, 2160, false };
            const Choice a = Decide({}, clipA, w240, {}, {}), b = Decide({}, clipA, vrr240, {}, {});
            check(a.multiplier == 10 && CadenceEven(w240.refreshHz, 23.976 * a.multiplier), L"VFG16: clip A, 240 Hz fixed (windowed): 10x, one new frame per refresh", a.smootherNote + L" | " + a.stoppedAt);
            check(b.multiplier == 10, L"VFG16: clip A, 240 Hz VRR (fullscreen): 10x (11x would pass the refresh)", b.smootherNote + L" | " + b.stoppedAt);
            const Choice c15 = Decide({}, low15, w240, {}, {});
            check(c15.multiplier == 16, L"VFG16: a 15 fps source on 240 Hz: 16x (240 fps)", c15.smootherNote + L" | " + c15.stoppedAt);
            const Choice c480 = Decide({}, clipA, w480, {}, {});
            check(c480.multiplier >= 10 && c480.multiplier <= 16 && c480.predictedMs <= c480.budgetMs, L"VFG16: 480 Hz: above 8x within the GPU budget", c480.smootherNote + L" | " + c480.stoppedAt);
            Settings f16; f16.smoother = 16; Settings f12; f12.smoother = 12;
            check(Decide(f16, clipA, w240, {}, {}).multiplier == 16, L"VFG16: Smoother 16x is honoured with VFG");
            Settings pf16 = f16; pf16.fgMethod = FgMethod::PushFlow;
            check(Decide(pf16, clipA, w240, {}, {}).multiplier == 16, L"PushFlow16: Smoother 16x with PushFlow is honoured (flow once above 4x)");
            PushFlowCeiling() = 4;   // an older core (no PUSHCORE_FEATURE_PUSHFLOW16)
            const int pfOld = Decide(pf16, clipA, w240, {}, {}).multiplier;
            PushFlowCeiling() = 16;
            check(pfOld == 4, L"PushFlow16: a core without it: Smoother 16x with PushFlow stops at 4x", std::format(L"{}x", pfOld));
            check(ParseChoice(L"16", 2, kMaxSmoother, kAuto) == 16 && ParseChoice(L"17", 2, kMaxSmoother, kAuto) == kAuto, L"VFG16: the Smoother setting reads 2..16");
            const Costs c0{};
            check(FgPrior(c0, FgMethod::Vfg, 1, 16, 1.0) > FgPrior(c0, FgMethod::Vfg, 1, 10, 1.0) && FgPrior(c0, FgMethod::Vfg, 1, 10, 1.0) > FgPrior(c0, FgMethod::Vfg, 1, 8, 1.0),
                  L"VFG16: the cost grows with the multiplier above 8x");
            // PushFlow above 4x: flow once, one network run per pair and a rebuild per further frame
            check(FgPrior(c0, FgMethod::PushFlow, 1, 5, 1.0) < FgPrior(c0, FgMethod::PushFlow, 1, 4, 1.0)
                  && FgPrior(c0, FgMethod::PushFlow, 1, 16, 1.0) > FgPrior(c0, FgMethod::PushFlow, 1, 10, 1.0)
                  && std::fabs(FgPrior(c0, FgMethod::PushFlow, 1, 10, 1.0) - (c0.pfFirst[1] + 8 * c0.pfRebuild[1]) * c0.card) < 1e-9,
                  L"PushFlow16: above 4x the cost is the first frame plus a rebuild per further frame");
            Costs pfMeasured = c0; pfMeasured.pfBenched = pfMeasured.pfRebuildBenched = true; pfMeasured.pfRebuild[1] = 0.5;
            Costs pfOldBench = pfMeasured; pfOldBench.pfRebuildBenched = false; pfOldBench.card = 2.0;
            check(std::fabs(FgPrior(pfOldBench, FgMethod::PushFlow, 1, 6, 1.0) - (c0.pfFirst[1] + 4 * 0.5 * 2.0)) < 1e-9,
                  L"PushFlow16: a speed test without the rebuild scales the rebuild prior by the card");
            const Choice rx = Decide({}, clipA, w240, pfMeasured, Family(kRdna3, L"AMD Radeon RX 7900 XTX"));
            check(rx.fgMethod == FgMethod::PushFlow && rx.multiplier > 4 && rx.predictedMs <= rx.budgetMs,
                  L"PushFlow16: Radeon, clip A on 240 Hz: PushFlow above 4x (flow once)", rx.smootherNote + L" | " + rx.stoppedAt);
            VfgCeiling() = 8;   // an older core (no PUSHCORE_FEATURE_VFG16)
            const Choice o1 = Decide({}, clipA, w240, {}, {}), o2 = Decide({}, clipA, vrr240, {}, {}), o3 = Decide(f12, clipA, w240, {}, {});
            VfgCeiling() = 16;
            check(o1.multiplier == 5 && o2.multiplier == 8 && o3.multiplier == 8, L"VFG16: a core without 16x: 5x windowed, 8x VRR, Smoother 12x stops at 8x",
                  std::format(L"{}x / {}x / {}x", o1.multiplier, o2.multiplier, o3.multiplier));
        }
        {   // the renderer's work per shown frame (ShowCost), HDR to SDR Auto, and the fixed-refresh spare (windowed 10x at the budget edge)
            const Source dd{ 3840, 2160, 23.976, true, 0.188, true };   // a 4K HDR10 web episode (not low bitrate)
            const Display w240 = d4k, vrr240 = d4kVrr;
            Display hdrOn = d4k; hdrOn.hdr = true;
            const Costs c0{};
            check(std::fabs(ShowCost(c0, w240, true, false, 10, 1.0, 1.0) - 10 * (c0.rendererTm4k + c0.compose4k)) < 1e-9
                  && std::fabs(ShowCost(c0, vrr240, true, true, 10, 1.0, 1.0) - c0.coreTm4k) < 1e-9 && ShowCost(c0, vrr240, false, false, 10, 1.0, 1.0) == 0.0,
                  L"show cost: renderer tone map + composition per shown frame; PushMPC's tone map once; fullscreen SDR nothing");
            const Choice a = Decide({}, dd, w240, {}, {});
            check(a.coreToneMap && a.multiplier == 10 && a.predictedMs <= a.budgetMs, L"HDR on an SDR desktop, windowed 240 Hz: PushMPC tone-maps, 10x fits",
                  std::format(L"{}x core {} {:.1f}/{:.1f} | {}", a.multiplier, int(a.coreToneMap), a.predictedMs, a.budgetMs, a.toneMapNote));
            Settings player; player.hdrToSdr = HdrToSdr::Player;
            const Choice b = Decide(player, dd, w240, {}, {}), bf = Decide(player, dd, vrr240, {}, {});
            check(!b.coreToneMap && b.multiplier == 5 && bf.multiplier == 10, L"HDR to SDR by the renderer: windowed 240 Hz 5x (10x would not fit), fullscreen VRR 10x",
                  std::format(L"{}x / {}x | {}", b.multiplier, bf.multiplier, b.stoppedAt));
            const Choice h = Decide({}, dd, hdrOn, {}, {});
            check(!h.coreToneMap && h.toneMapNote.find(L"Windows HDR is on") != std::wstring::npos, L"Windows HDR on: no tone map (HDR10 to the renderer)", h.toneMapNote);
            Settings forced; forced.hdrToSdr = HdrToSdr::PushMpc; forced.smoother = kOff;
            check(Decide(forced, dd, w240, {}, {}).coreToneMap && !Decide({}, { 1920, 1080, 23.976, false, 0.5 }, w240, {}, {}).coreToneMap,
                  L"HdrToSdr PushMPC is honoured (Smoother off too); an SDR source never tone-maps");
            // the fixed-refresh spare: a GPU 10 % slower than the 5090 tables
            Costs slower; slower.fgScale[kKeyVfgMed] = 1.1;
            const Source clipA{ 1920, 1040, 23.976, false, 0.5 };
            const Choice f = Decide({}, clipA, w240, slower, {}), v = Decide({}, clipA, vrr240, slower, {});
            check(EveryRefresh(w240, 239.76) && !EveryRefresh(w240, 119.88) && !EveryRefresh(vrr240, 239.76),
                  L"one new frame per refresh: 10x at 240 Hz fixed, not 5x, not with VRR");
            check(f.multiplier == 5 && f.stoppedAt.find(L"spare") != std::wstring::npos && v.multiplier == 10,
                  L"windowed 10x at the budget edge: 5x (even cadence, said so); fullscreen VRR keeps 10x", std::format(L"{}x / {}x | {}", f.multiplier, v.multiplier, f.stoppedAt));
        }
        {   // Anime upscalers (round 5 item 7): candidates for animation only, by source class and cost; RTX VSR stays first on RTX
            // until the verdict; on GPUs without RTX VSR the best fitting one is the anime default; R8F64 / AnimeSR on DVD only
            auto withAnime = [](Gpu g) { g.available |= 1u << kCapAnimeSr; g.reason[kCapAnimeSr].clear(); return g; };
            const Gpu rdna3 = withAnime(Family(kRdna3, L"AMD Radeon RX 7900 XTX")), rtx50 = withAnime(Family(kRtx50, L"NVIDIA GeForce RTX 5090"));
            const Gpu rdnaNo = Family(kRdna3, L"AMD Radeon RX 7900 XTX");   // anime models not installed
            Display d; d.refreshHz = 240; d.width = 3840; d.height = 2160;
            Source bd{ 1920, 1080, 23.976, false, 0.5 }; bd.heldShare = 0.4; bd.contentClass = 1; bd.contentConf = 0.9f;   // measured drawn
            Source dvd = bd; dvd.w = 720; dvd.h = 480;
            Source p720 = bd; p720.w = 1280; p720.h = 720;
            Source live = bd; live.heldShare = 0.01; live.contentClass = 3;
            Source uhd = bd; uhd.w = 3840; uhd.h = 2160;
            const Choice a = Decide({}, bd, d, {}, rdna3);
            check(a.srMethod == Upscaler::Anime && (a.animeModel == 3 || a.animeModel == 0), L"anime SR: 1080p animation on a GPU without RTX VSR: ArtCNN C (the best that fits)", a.animeSrNote);
            const Choice b = Decide({}, dvd, d, {}, rdna3);
            check(b.srMethod == Upscaler::Anime && b.animeModel == 6, L"anime SR: DVD animation: ArtCNN R8F64 (heavy, DVD-class only) when it fits", b.animeSrNote);
            Costs slow; slow.card = 4.0;   // a slower card: R8F64 no longer fits a DVD's frame share, a C model does
            const Choice b2 = Decide({}, dvd, d, slow, rdna3);
            check(b2.srMethod == Upscaler::Anime && b2.animeModel != 6 && b2.animeModel != 10, L"anime SR: a slow card falls back to a lighter model", b2.animeSrNote);
            const Choice c3 = Decide({}, live, d, {}, rdna3);
            check(c3.srMethod != Upscaler::Anime, L"anime SR: never for live action", c3.animeSrNote);
            const Choice e = Decide({}, uhd, d, {}, rdna3);
            check(e.srMethod != Upscaler::Anime, L"anime SR: not on a source at the output size (2x / 4x models only)");
            const Choice f = Decide({}, bd, d, {}, rtx50);
            check(f.srMethod == Upscaler::Rtx && f.animeSrNote.find(L"candidate") == 0, L"anime SR: RTX VSR stays first on RTX until the verdict; the candidate is named", f.animeSrNote);
            {   // the same rule for every driver Video Super Resolution (content_detect.h kAnimeSrBeforeDriverVsr): Intel VSR too
                Gpu ivsr = rdna3; ivsr.available |= 1u << kCapIvsr; ivsr.reason[kCapIvsr].clear();
                const Choice iv = Decide({}, bd, d, {}, ivsr);
                check(iv.srMethod != Upscaler::Anime && iv.animeSrNote.find(L"candidate") == 0, L"anime SR: Intel VSR stays first until the verdict (the shared rule)", iv.animeSrNote);
                // an Intel GPU with XeSS SR but no Intel VSR: no driver VSR, so the anime model on drawn animation (as on AMD)
                Gpu xe = withAnime(Family(kIntel, L"Intel Arc A770")); xe.available |= 1u << kCapXessSr; xe.reason[kCapXessSr].clear();
                const Choice xa = Decide({}, bd, d, {}, xe), xl = Decide({}, live, d, {}, xe);
                check(xa.srMethod == Upscaler::Anime && xl.srMethod != Upscaler::Anime, L"anime SR: XeSS-only GPU: the anime model on drawn, XeSS / FSR on live action", xa.animeSrNote + L" | " + xl.sharperNote);
                Gpu xv = xe; xv.available |= 1u << kCapIvsr; xv.reason[kCapIvsr].clear();
                check(Decide({}, bd, d, {}, xv).srMethod != Upscaler::Anime, L"anime SR: with Intel VSR too: the driver's VSR stays first");
                // HDR: pushcore feeds the model its SDR-like proxy (AnimeSrFeed::PushcoreProxy): allowed
                Source hdrBd = bd; hdrBd.hdr = true; hdrBd.tenBit = true;
                Display dh = d; dh.hdr = true;   // HDR in and out: the traced path (hdrWorking, the SDR-like proxy)
                const Choice hv = Decide({}, hdrBd, dh, {}, rdna3);
                check(hv.srMethod == Upscaler::Anime, L"anime SR: an HDR source in pushcore (SDR-like proxy, restored onto HDR) keeps the anime model", hv.animeSrNote);
            }
            Settings asked; asked.upscaler = Upscaler::Anime;
            Settings askedSr = asked; askedSr.animeModel = L"animesr-v2";
            const Choice g1 = Decide(asked, bd, d, {}, rtx50), g2 = Decide(askedSr, dvd, d, {}, rtx50);
            check(g1.srMethod == Upscaler::Anime && g1.animeModel >= 0 && g2.animeModel == 10, L"anime SR: Upscaler Anime on RTX: Auto's model, or the setting's (AnimeSR on DVD)", g2.animeSrNote);
            Settings askedR8 = asked; askedR8.animeModel = L"cunny-8x32";
            check(Decide(askedR8, bd, d, {}, rdna3).animeModel == 9, L"anime SR: the setting's CuNNy model is used");
            const Choice h = Decide({}, bd, d, {}, rdnaNo);
            check(h.srMethod == Upscaler::Fsr, L"anime SR: without its files the GPU keeps FSR");
            Source lowRate = bd; lowRate.bppEq = 0.02;
            const Choice l = Decide({}, lowRate, d, {}, rdna3);
            check(l.animeModel == 0 || l.animeModel == 3, L"anime SR: a low-bitrate source keeps the plain model (_DS never automatic: measured worse)", l.animeSrNote);
            {   // drawn + rendered 3D CG (ContentDetect's cg flag): the plain anime model even at a low bitrate, hold repair on,
                // sign hold / line art / the anime deband preset off (DrawnPolicy kPolicyDrawnCg)
                Source cg = lowRate; cg.contentCg = true;
                const Choice q = Decide({}, cg, d, {}, rdna3);
                check(q.srMethod == Upscaler::Anime && (q.animeModel == 0 || q.animeModel == 3), L"drawn CG: the plain anime model at a low bitrate (never _DS)", q.animeSrNote);
                check(q.animation && q.drawnCg && (q.holdFlags & 6u) == 0, L"drawn CG: held drawings re-timed, no line art, no sign hold", q.animNote);
                Source cg8 = cg; cg8.bppEq = 0.3; cg8.tenBit = false; cg8.hdr = false;
                Source hd8 = cg8; hd8.contentCg = false;
                const Choice r1 = Decide({}, cg8, d, {}, rdna3), r2 = Decide({}, hd8, d, {}, rdna3);
                check(r1.debandStrength != 3 && r2.debandStrength == 3, L"drawn CG: the normal deband rule, hand-drawn the anime preset", r1.debandNote + L" | " + r2.debandNote);
                check((r2.holdFlags & 6u) == 6u && !r2.drawnCg, L"hand-drawn: line art and sign hold on", r2.animNote);
                check(!(r2.holdFlags & 32u), L"hand-drawn: the sign hold's first text gate (not the strict one)", std::to_wstring(r2.holdFlags));
                Source liveCg = live; liveCg.contentCg = true;   // a stale flag on non-drawn content changes nothing
                const Choice r3 = Decide({}, liveCg, d, {}, rdna3);
                check(!r3.animation && r3.srMethod != Upscaler::Anime && (r3.holdFlags & 6u) == 0, L"not drawn: no drawn treatment whatever the CG flag");
                Settings animOn; animOn.animation = 1;   // the user's Animation On over live action: the sign hold runs with the strict gate
                const Choice r4 = Decide(animOn, live, d, {}, rdna3);
                check((r4.holdFlags & 4u) && (r4.holdFlags & 32u), L"Animation On over live action: the sign hold's strict text gate (measured class)", std::to_wstring(r4.holdFlags));
            }
            Costs learned; learned.animeSrLearned[3] = 40.0; learned.animeSrLearned[0] = 40.0;   // measured slow on this card
            const Choice m = Decide({}, bd, d, learned, rdna3);
            check(m.animeModel != 3 && m.animeModel != 0, L"anime SR: this card's measurement replaces the prior", m.animeSrNote);
            const Choice n = Decide({}, p720, d, {}, rdna3);
            check(n.animeModel != 6 && n.animeModel != 10, L"anime SR: 720p never gets the DVD-only models", n.animeSrNote);
            check(AnimeSrModelIndex(L"ARTCNN-C4F32") == 3 && AnimeSrModelIndex(L"apisr") == -1 && AnimeSrModel(10).scale == 4, L"anime SR: model table");
            check(!Decide({}, bd, d, {}, rdna3).sameGraph(Decide({}, dvd, d, {}, rdna3)), L"anime SR: another model is another graph");
        }
        check(!CoreOutputStalled(800, 40, true) && !CoreOutputStalled(5000, 10, true) && !CoreOutputStalled(5000, 40, false) && CoreOutputStalled(1600, 24, true),
              L"no-output guard: source frames shown after 1.5 s and 24 frames in, nothing out (playing)");
        {   // the delivery check: late frames at 10x cap Auto one step lower; at 240 Hz fixed that is 5x (the even cadence), VRR 9x
            check(!DeliveryOverloaded(9, 100) && !DeliveryOverloaded(20, 4000) && DeliveryOverloaded(30, 2400), L"delivery check: 1 % late (and at least 10 frames) in 10 s");
            Settings capped; capped.autoCap = 9;
            const Source clipA{ 1920, 1040, 23.976, false, 0.5 };
            const Choice w = Decide(capped, clipA, d4k, {}, {}), v = Decide(capped, clipA, d4kVrr, {}, {});
            check(w.multiplier == 5 && v.multiplier == 9 && v.stoppedAt.find(L"delivery measured") != std::wstring::npos,
                  L"delivery check: capped at 9x -> 5x windowed (even), 9x VRR (said so)", std::format(L"{}x / {}x: {}", w.multiplier, v.multiplier, v.stoppedAt));
            DeliveryCap dc;
            const bool c1 = dc.Update(true, 10, 10000) && dc.cap == 9;
            int opened = 0; for (int i = 0; i < 6; ++i) opened += dc.Update(false, 9, 10000) ? 1 : 0;   // 60 s clean
            const bool c2 = opened == 1 && dc.cap == 10;
            const bool c3 = dc.Update(true, 10, 10000) && dc.cap == 9;                                  // 10x fails again
            int n = 0; while (!dc.Update(false, 9, 10000) && n < 100) ++n;                              // now 120 s
            const bool c4 = n == 11 && dc.cap == 10;
            const bool c5 = DeliveryCap{}.Update(true, 2, 10000) == false;                              // nothing below 2x
            check(c1 && c2 && c3 && c4 && c5, L"delivery check: one step down, back up after 60 s clean, 120 s after a second failure",
                  std::format(L"{} {} {} {} {} (n {})", c1, c2, c3, c4, c5, n));
        }
        {   // bits per pixel at 1080p quality, and clean-up Auto only inside the budget
            const DWORD hvc1 = DWORD('H' | ('V' << 8) | ('C' << 16) | ('1' << 24)), h264 = DWORD('H' | ('2' << 8) | ('6' << 16) | ('4' << 24));
            const double web4k = BitsPerPixel(16.55e6, 3840, 2160, 24.0, hvc1), web1080 = BitsPerPixel(6e6, 1920, 1080, 23.976, h264), uhdbd = BitsPerPixel(60e6, 3840, 2160, 23.976, hvc1);
            check(web4k > kLowBitrateBpp && web1080 <= kLowBitrateBpp && uhdbd >= kHighBitrateBpp,
                  L"bits per pixel at 1080p quality: 4K HEVC 16.5 Mbit/s not low, 1080p H.264 6 Mbit/s low, UHD Blu-ray high",
                  std::format(L"{:.3f} / {:.3f} / {:.3f}", web4k, web1080, uhdbd));
            Settings f10; f10.smoother = 12;
            const Source low4k{ 3840, 2160, 24.0, true, 0.10, true };
            const Choice ch = Decide(f10, low4k, d4k, {}, {});
            check(!ch.nr && ch.cleanNote.find(L"would not fit") != std::wstring::npos, L"clean-up Auto stays off when the plan with it (Smoother 12x, 4K) would not fit", ch.cleanNote);
            const Choice ca = Decide({}, low4k, d4k, {}, {});
            check(ca.predictedMs <= ca.budgetMs, L"Auto's own plan for a low-bitrate 4K HDR source fits its budget",
                  std::format(L"{}x nr {} {:.1f} of {:.1f} ms", ca.multiplier, int(ca.nr), ca.predictedMs, ca.budgetMs));
        }
        wprintf(L"  vendor paths: %d failed\n", fails);
    }
    // ---- G10 Intel methods: XeSS FG / XeSS SR / Intel VSR / VPL FI in Auto, pushcore's numbering of them
    wprintf(L"\n[intel video] G10\n");
    {
        int fails = 0;
        auto check = [&](bool ok, const wchar_t* what, const std::wstring& detail = L"") { wprintf(L"  %-74ls [%ls]%ls%ls\n", what, ok ? L"PASS" : L"FAIL", detail.empty() ? L"" : L"  ", detail.c_str()); fails += ok ? 0 : 1; };
        const Source film{ 1920, 1080, 23.976, false, 0.5 };
        Costs cold; cold.card = 6.0;
        Gpu arc = Family(kIntel, L"Intel(R) Arc(TM) B580 Graphics"); arc.available |= 1u << kCapXefg;
        const Choice x1 = Decide({}, film, d60, cold, arc);
        check(x1.fgMethod == FgMethod::XeFg && x1.multiplier >= 2, L"Arc with XeSS FG (self-test passed): XeSS FG first", x1.smootherNote + L" | " + x1.stoppedAt);
        const Choice x2 = Decide({}, film, d60, cold, Family(kIntel, L"Intel(R) Arc(TM) B580 Graphics"));
        check(x2.fgMethod != FgMethod::XeFg && x2.fgMethod != FgMethod::Vpl, L"Arc without the Intel capabilities: never XeSS FG / VPL", x2.smootherNote);
        Gpu arcSr = arc; arcSr.available |= 1u << kCapXessSr; arcSr.reason[kCapXessSr] = L"ms4k=2.50"; ReadMeasured(arcSr);
        const Display d4k{ L"4K 60", 60.0, 3840, 2160, false };
        const Choice x3 = Decide({}, film, d4k, cold, arcSr);
        check(arcSr.xessMs4k > 2.4 && arcSr.xessMs4k < 2.6 && x3.sr && x3.srMethod == Upscaler::Xess,
              L"Arc with XeSS SR, 4K screen: XeSS SR (measured ms4k read from the capability)", x3.sharperNote);
        check(CoreFgFor(FgMethod::XeFg) == 6u && CoreFgFor(FgMethod::Vpl) == 7u && CoreSrMethod(Upscaler::Xess) == 3u && CoreSrMethod(Upscaler::IntelVsr) == 4u
              && UpscalerOfCore(3) == Upscaler::Xess && UpscalerOfCore(4) == Upscaler::IntelVsr, L"XeSS FG 6, VPL 7, XeSS SR 3, Intel VSR 4 (PUSHCORE_FG_* / SR_METHOD_*)");
        Gpu vplArc = Family(kIntel, L"Intel(R) Arc(TM) A770 Graphics"); vplArc.available |= 1u << kCapVplFi; vplArc.reason[kCapVplFi] = L"gate passed ms=9.50 psnr=31.0 maxM=4";
        ReadMeasured(vplArc);
        check(vplArc.vplGate && vplArc.vplMs1080 > 9.4 && vplArc.vplMs1080 < 9.6 && FgOrder(vplArc).front() == FgMethod::Vpl, L"VPL's gate and cost read from the capability; VPL first once gated");
        // XeSS on the GPUs without a better method (GTX, RX 5000, other): a measured candidate; never on RTX / RX 6000+
        {
            Gpu gtx = Family(kGtx, L"NVIDIA GeForce GTX 1080"); gtx.available |= (1u << kCapXefg) | (1u << kCapXessSr);
            gtx.reason[kCapXefg] = L"ms4k=6.00"; gtx.reason[kCapXessSr] = L"ms4k=3.00"; ReadMeasured(gtx);
            Costs slowPfGtx = cold; slowPfGtx.pfBenched = true; for (int t = 0; t < 3; ++t) { slowPfGtx.pfFirst[t] = 60 + 20 * t; slowPfGtx.pfNext[t] = 50 + 20 * t; }
            ApplyMeasuredCard(slowPfGtx);
            const Choice g1 = Decide({}, film, d60, slowPfGtx, gtx);
            check(gtx.xefgMs4k > 5.9 && g1.fgMethod == FgMethod::XeFg && g1.multiplier >= 2, L"GTX 1080, PushFlow measured too slow: XeSS FG (measured) before NVOF / FSR FG", g1.smootherNote + L" | " + g1.stoppedAt);
            const Display d4k{ L"4K 60", 60.0, 3840, 2160, false };
            const Choice g2 = Decide({}, film, d4k, slowPfGtx, gtx);
            check(g2.sr && g2.srMethod == Upscaler::Xess, L"GTX 1080, 4K screen: XeSS SR (DP4a, measured) over FSR", g2.sharperNote);
            Gpu rx5 = Family(kRdna1, L"AMD Radeon RX 5700 XT"); rx5.available |= 1u << kCapXefg;
            check(FgOrder(rx5)[1] == FgMethod::XeFg && FgOrder(Family(kOtherGpu, L"x"))[1] == FgMethod::XeFg, L"RX 5000 and other GPUs: XeSS FG right after PushFlow");
            Gpu rtx = Family(kRtx40, L"NVIDIA GeForce RTX 4070"); rtx.available |= (1u << kCapXefg) | (1u << kCapXessSr);
            const Choice r1 = Decide({}, film, d4k, cold, rtx);
            Gpu rx7 = Family(kRdna3, L"AMD Radeon RX 7800 XT"); rx7.available |= (1u << kCapXefg) | (1u << kCapXessSr);
            const Choice r2 = Decide({}, film, d4k, cold, rx7);
            check(r1.fgMethod != FgMethod::XeFg && r1.srMethod != Upscaler::Xess && r2.fgMethod != FgMethod::XeFg && r2.srMethod != Upscaler::Xess,
                  L"RTX 40 / RX 7000 with XeSS installed: their own better methods stay", r1.smootherNote + L" | " + r2.smootherNote);
        }
        wprintf(L"  intel video: %d failed\n", fails);
    }
    // ---- Learned costs across sessions (PushMpcLearned.h): foreign GPU load must not lower later picks for good ----
    // Each simulated session: a new session number, the saved values into Costs (as LoadLearned), Decide at the start of
    // the file; the running key is measured at the card's true ratio times this session's load; under load the session
    // steps down in-session (Decide again with that measurement, the new key measured too); then the session's values go
    // into the histories (as SaveLearned). "Old rule" = the 1.0 behaviour: the saved value is the session's last value.
    int learnFails = 0;
    {
        auto check = [&](bool ok, const std::wstring& what, const std::wstring& detail = L"") {
            wprintf(L"  %-96ls [%ls]  %ls\n", what.c_str(), ok ? L"PASS" : L"FAIL", detail.c_str());
            if (!ok) ++learnFails;
        };
        const Gpu g5090 = Family(kRtx50, L"NVIDIA GeForce RTX 5090");
        const Source film{ 1920, 1080, 23.976, false, 0.5 };
        const Display disp{ L"4K 240", 239.96, 3840, 2160, false };
        auto pickStr = [](const Choice& c) { return std::format(L"{} q{} x{}", FgMethodLabel(c.fgMethod), c.fgQuality, c.multiplier); };
        auto same = [](const Choice& a, const Choice& b) { return a.fgMethod == b.fgMethod && a.fgQuality == b.fgQuality && a.multiplier == b.multiplier && a.sr == b.sr && a.srLevel == b.srLevel; };
        struct Sim {
            learned::Values v;                 // the new rules (the Learned key)
            std::map<int, double> old;         // the old rule: per key the last session's value
            std::vector<Choice> starts, startsOld;
            int lowered = 0, loweredOld = 0;   // sessions whose start pick was stepped down in-session (a rebuild while playing)
        };
        // one session: trueRatio per key (default 1.0), load factor for this session's measurements
        auto session = [&](Sim& s, double load, const std::map<int, double>& trueRatio, double defaultRatio, const Source& src = Source{ 1920, 1080, 23.976, false, 0.5 }) {
            auto ratio = [&](int k) { auto it = trueRatio.find(k); return (it == trueRatio.end() ? defaultRatio : it->second) * load; };
            auto run = [&](Costs c, std::map<int, double>& measured, int& lowered) {
                Choice ch = Decide({}, src, disp, c, g5090);
                const Choice start = ch;
                for (int step = 0; step < 6; ++step) {   // in-session: the running key measured, Decide again until it stays
                    if (ch.multiplier < 2) break;
                    const int k = FgKeyOf(ch.fgMethod, ch.fgQuality);
                    measured[k] = ratio(k); c.fgScale[k] = ratio(k); c.fgLearned[k] = true;
                    const Choice next = Decide({}, src, disp, c, g5090);
                    if (same(next, ch)) break;
                    ch = next;
                }
                if (ch.multiplier < start.multiplier || ch.fgQuality < start.fgQuality) ++lowered;
                return start;
            };
            // new rules
            const uint32_t sid = learned::BeginSession(s.v);
            Costs c; learned::LoadBench(s.v, sid, c); ApplyMeasuredCard(c); learned::LoadCosts(s.v, sid, c);
            std::map<int, double> m;
            s.starts.push_back(run(c, m, s.lowered));
            std::map<std::wstring, double> named;
            for (auto& [k, x] : m) named[std::format(L"FgScale{}", k)] = x;
            learned::SaveSession(s.v, sid, named, c);
            // old rule
            Costs o; for (auto& [k, x] : s.old) { o.fgScale[k] = x; o.fgLearned[k] = true; }
            std::map<int, double> mo;
            s.startsOld.push_back(run(o, mo, s.loweredOld));
            for (auto& [k, x] : mo) s.old[k] = x;
        };
        // 1. a loaded session between clean ones: the next clean session picks what it picked before the load
        {
            Sim s;
            session(s, 1.0, {}, 1.0); session(s, 1.0, {}, 1.0);
            session(s, 2.5, {}, 1.0);   // other programs load the GPU: everything measures 2.5x slower
            for (int i = 0; i < 4; ++i) session(s, 1.0, {}, 1.0);
            const Choice& before = s.starts[1];
            bool back = true; for (size_t i = 3; i < s.starts.size(); ++i) back = back && same(s.starts[i], before);
            check(back, L"learned: a loaded session, then clean ones: the picks return to the original at once", pickStr(before) + L" -> " + pickStr(s.starts[3]));
            check(!same(s.startsOld[3], s.startsOld[1]), L"learned (control): the old rule starts the next clean file lowered", pickStr(s.startsOld[1]) + L" -> " + pickStr(s.startsOld[3]));
        }
        // 2. three loaded sessions in a row ARE how the card does then: the values rise; clean sessions bring them back
        //    for the keys that run, and a key Auto no longer runs goes back to its prior after 20 sessions
        {
            Sim s;
            session(s, 1.0, {}, 1.0);
            for (int i = 0; i < 3; ++i) session(s, 2.5, {}, 1.0);
            const Choice lowered = s.starts.back();
            session(s, 2.5, {}, 1.0);
            check(!same(s.starts.back(), s.starts[0]), L"learned: three loaded sessions in a row: the slower values are saved", pickStr(s.starts.back()));
            for (int i = 0; i < 22; ++i) session(s, 1.0, {}, 1.0);
            check(same(s.starts.back(), s.starts[0]), L"learned: ...and 22 clean sessions later the original pick is back (faster values at once, unused ones expire)", pickStr(s.starts.back()));
            (void)lowered;
        }
        // 3. a genuinely slower card (every key 1.6x the prior, every session): converges and then stays (no per-file flapping)
        {
            Sim s;
            for (int i = 0; i < 12; ++i) session(s, 1.0, {}, 1.6);
            Costs ideal; for (int k = 0; k < kFgKeys; ++k) { ideal.fgScale[k] = 1.6; ideal.fgLearned[k] = true; }
            const Choice target = Decide({}, film, disp, ideal, g5090);
            int firstStable = -1;
            for (int i = 0; i < 12; ++i) {
                bool rest = true; for (int j = i; j < 12; ++j) rest = rest && same(s.starts[j], s.starts[i]);
                if (rest) { firstStable = i; break; }
            }
            check(firstStable >= 0 && firstStable <= 6 && same(s.starts.back(), target),
                  std::format(L"learned: a slower card converges by session {} to the pick its true costs give, then stays", firstStable + 1), pickStr(s.starts.back()) + L" / " + pickStr(target));
        }
        // 3b. mixed content: two kinds of file alternate on the same frame generation key with different true ratios
        //     (the prior model is not exact across sizes); how often a file's start pick has to be stepped down while playing
        {
            Sim s;
            const Source a{ 1920, 1080, 23.976, false, 0.5 }, b{ 1280, 720, 23.976, false, 0.5 };
            for (int i = 0; i < 12; ++i) session(s, 1.0, {}, i % 2 ? 1.35 : 0.85, i % 2 ? b : a);
            check(s.lowered <= s.loweredOld + 1, std::format(L"learned: mixed content (two files alternating, true ratio 0.85 / 1.35): stepped down while playing in {} of 12 sessions (old rule {})", s.lowered, s.loweredOld),
                  pickStr(s.starts[10]) + L" / " + pickStr(s.starts[11]));
        }
        // 4. steady unloaded input: the same start picks as the old rule (cards at or faster than the priors at once;
        //    slower than the priors from the 3rd session on, the sessions before start on the priors)
        {
            Sim fast, slow;
            for (int i = 0; i < 8; ++i) { session(fast, 1.0, {}, 0.85); session(slow, 1.0, {}, 1.25); }
            bool sameFast = true, sameSlow = true;
            for (size_t i = 0; i < fast.starts.size(); ++i) sameFast = sameFast && same(fast.starts[i], fast.startsOld[i]);
            for (size_t i = 3; i < slow.starts.size(); ++i) sameSlow = sameSlow && same(slow.starts[i], slow.startsOld[i]);
            check(sameFast, L"learned: steady unloaded input, card at/above the priors: the same picks as the old rule in every session", pickStr(fast.starts.back()));
            check(sameSlow, L"learned: steady unloaded input, card slower than the priors: the same picks as the old rule from the 4th session on", pickStr(slow.starts.back()));
        }
        // 5. the rules themselves
        {
            learned::Values v; v[L"Gpu"] = L"x"; v[L"Model"] = L"3"; v[L"FgScale3"] = L"2.000";
            learned::Reset(v, L"x");
            check(v.count(L"FgScale3") == 0 && learned::Get(v, L"Model") == learned::kModel, L"learned: model 3 values are dropped (model 4)");
            std::vector<learned::Entry> h;
            learned::Record(h, 1, 1.0); learned::Record(h, 2, 3.0); double x = 0;
            check(learned::Resolve(h, 2, 1.0, x) && x == 1.0, L"learned: a slower session does not raise the saved value");
            learned::Record(h, 3, 3.0); learned::Record(h, 4, 3.0);
            check(learned::Resolve(h, 4, 1.0, x) && x == 3.0, L"learned: three slower sessions in a row do");
            learned::Record(h, 5, 0.9);
            check(learned::Resolve(h, 5, 1.0, x) && x == 0.9, L"learned: a faster session lowers it at once");
            learned::Record(h, 5, 1.1);
            check(h.size() == 3 && learned::Resolve(h, 5, 1.0, x) && x == 1.1, L"learned: periodic saves replace the session's own entry");
            std::vector<learned::Entry> one; learned::Record(one, 10, 2.0);
            check(!learned::Resolve(one, 10, 1.0, x), L"learned: one session slower than the prior is not trusted (the prior stays)");
            std::vector<learned::Entry> st = { { 1, 2.0 }, { 2, 2.0 }, { 3, 2.0 } };
            check(learned::Resolve(st, 20, 1.0, x) && x == 2.0 && !learned::Resolve(st, 23, 1.0, x), L"learned: a value not measured for 20 sessions goes back to the prior");
            check(learned::Encode(learned::Decode(L"7:1.2500;9:0.5000")) == L"7:1.2500;9:0.5000", L"learned: the history text round-trips");
            // the PushFlow speed test: median results, first run trusted, faster replaces, slower only when 3 runs agree
            learned::Values b; double f[3] = { 6, 6, 6 }, n[3] = { 4, 4, 4 }, r[3] = { 1, 1, 1 };
            learned::RecordBench(b, 1, f, n, r);
            Costs c; check(learned::LoadBench(b, 1, c) && c.pfFirst[1] == 6 && c.pfRebuildBenched, L"speed test: the first run is taken (it measures this card)");
            double f2[3] = { 5, 5, 5 }; learned::RecordBench(b, 11, f2, n, r); learned::LoadBench(b, 11, c);
            check(c.pfFirst[1] == 5, L"speed test: a faster rerun replaces it");
            double f3[3] = { 9, 9, 9 }; learned::RecordBench(b, 21, f3, n, r); learned::LoadBench(b, 21, c);
            check(c.pfFirst[1] == 5, L"speed test: one slower rerun does not");
            check(!learned::BenchStale(b, 30) && learned::BenchStale(b, 31), L"speed test: due again 10 sessions after its newest run");
            check(learned::LoadBench(b, 21 + 40, c) && c.pfFirst[1] == 5, L"speed test: its results never expire (40 sessions later still the card's own, only due again)");
        }
        wprintf(L"  learned: %d failed\n", learnFails);
    }
    Display real = QueryDisplay(PlayerWindow());
    wprintf(L"this machine's primary display: %ls %.3f Hz %ux%u HDR %d VRR %d (%ls)\n", real.name.c_str(), real.refreshHz, real.width, real.height, real.hdr, real.vrr, real.vrrHow.c_str());
    return learnFails ? 1 : 0;
}
