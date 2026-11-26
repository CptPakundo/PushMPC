// Headless check of the Auto rules (no GPU): prints the decision for a few sources, displays and GPU families.
#include "../src/PushMpcAuto.h"
#include "../src/PushMpcExtras.h"
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
    Display d4kVrr = d4k; d4kVrr.vrr = true;
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
        wprintf(L"  user's profile 1/1, clip A: windowed %dx %.1f fps (VRR %d), fullscreen %dx %.1f fps (VRR %d)  [expected 5x / 8x]\n",
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
            Settings fm; fm.fgMethod = FgMethod::Nvof;
            check(CoreFgFor(FgMethod::Nvof) == 5u, L"FgMethod::Nvof maps to PUSHCORE_FG_NVOF (5)");
            (void)fm;
        }
        {   // Subtitle Hold: Auto = 2 (gated) when the core gates it and the unlock is not off; Off / On stay manual
            Settings a; Settings off; off.subtitleHold = kOff; Settings on; on.subtitleHold = 1;
            check(SubtitleHoldValue(a, true, true) == 2 && SubtitleHoldValue(a, false, true) == 0 && SubtitleHoldValue(a, true, false) == 0
                  && SubtitleHoldValue(off, true, true) == 0 && SubtitleHoldValue(on, false, false) == 1, L"Subtitle Hold: Auto gated (2) only with the core's gate and the unlock on; Off/On manual");
        }
        wprintf(L"  vendor paths: %d failed\n", fails);
    }
    Display real = QueryDisplay(PlayerWindow());
    wprintf(L"this machine's primary display: %ls %.3f Hz %ux%u HDR %d VRR %d (%ls)\n", real.name.c_str(), real.refreshHz, real.width, real.height, real.hdr, real.vrr, real.vrrHow.c_str());
    return 0;
}
