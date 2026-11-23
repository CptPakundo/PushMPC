// Headless check of the Auto rules (no GPU): prints the decision for a few sources, displays and GPU families.
#include "../src/PushMpcAuto.h"
#include "../src/PushMpcExtras.h"
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
    Display real = QueryDisplay(PlayerWindow());
    wprintf(L"this machine's primary display: %ls %.3f Hz %ux%u HDR %d VRR %d (%ls)\n", real.name.c_str(), real.refreshHz, real.width, real.height, real.hdr, real.vrr, real.vrrHow.c_str());
    return 0;
}
