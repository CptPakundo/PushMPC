// SPDX-License-Identifier: GPL-3.0-or-later
// pushmpc-capture: PushMPC's capture mode for Linux and macOS, for players PushMPC cannot plug into (Flathub Celluloid,
// Stremio, browsers, QuickTime Player, Infuse, ...). The player's window is captured (cap_source.h), its frames are
// played back on the player's own timeline a fixed delay later (cap_core.h Timeline: the 3:2 judder of a 24 fps video
// on a 60 Hz screen is gone), and every screen refresh shows its own moment: a generated frame between two real ones
// (frame generation, Smoother) on the engine Auto picks, upscaled to the window by NIS / FSR1 on the GPU engine, in a
// window right over the player that lets every click through.
//   Engines (Auto): Linux: nvl (NVIDIA Video Effects: VFG, RTX VSR, the optical-flow tier; behind its own probe and the
//   RTX 30 self-test), else ml (PushFlow / RIFE through pmml, when its runtime is installed), else the portable GPU
//   engine on Vulkan (NVIDIA optical flow as a block-matcher candidate where the device has it), else the CPU engine.
//   macOS: apple (VideoToolbox frame interpolation), else the GPU engine on
//   MoltenVK, else the CPU engine.
// Copy-protected video (a black capture) makes capture mode step aside and say why once. Lip sync: --audio-conf writes
// the measured delay into an mpv-based player's mpv.conf (a marked block, removed again on exit).
// Tests: --no-present --dump out.y4m --frames N (no window: the frames go to the file at --display-hz), --selftest
// (the GPU engine on a synthetic clip, no window, no capture).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include "cap_core.h"
#include "cap_engines.h"
#include "cap_gpu.h"
#include "cap_platform.h"
#include "cap_vk.h"
#include "platform.h"   // ReadSetting, ParseSmoother, the log folder (vsplugin/src)

namespace pmc {

const char* const kMacPermissionMessage =
    "PushMPC capture mode needs your permission to see the player's window. Open System Settings > Privacy & Security > "
    "Screen & System Audio Recording, turn on \"PushMPC Capture\", then start capture mode again.";

namespace {

std::atomic<bool> g_stop{ false };
void OnSignal(int) { g_stop = true; }

FILE* g_log = nullptr;
bool g_verbose = false;
void Log(const char* fmt, ...)
{
    char b[2048];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(b, sizeof(b), fmt, a);
    va_end(a);
    std::fprintf(stderr, "pushmpc-capture: %s\n", b);
    if (g_log) { std::fprintf(g_log, "%.3f %s\n", Now(), b); std::fflush(g_log); }
}

// ------------------------------------------------------------------------------------------------ the y4m dump
class Y4m {
public:
    ~Y4m() { if (m_f) std::fclose(m_f); }
    bool Open(const std::string& path, int w, int h, int bits, double hz)
    {
        m_f = std::fopen(path.c_str(), "wb");
        if (!m_f) return false;
        std::fprintf(m_f, "YUV4MPEG2 W%d H%d F%d:1000 Ip A1:1 %s\n", w, h, int(std::lround(hz * 1000)), bits > 8 ? "C420p10 XYSCSS=420P10" : "C420jpeg");
        m_w = w; m_h = h; m_bits = bits;
        return true;
    }
    void Write(const I420& p)
    {
        if (!m_f || p.w != m_w || p.h != m_h || (p.bits > 8) != (m_bits > 8)) return;
        std::fputs("FRAME\n", m_f);
        std::fwrite(p.y.data(), 1, p.y.size(), m_f);
        std::fwrite(p.u.data(), 1, p.u.size(), m_f);
        std::fwrite(p.v.data(), 1, p.v.size(), m_f);
    }
    bool IsOpen() const { return m_f != nullptr; }
private:
    FILE* m_f = nullptr;
    int m_w = 0, m_h = 0, m_bits = 8;
};

// ------------------------------------------------------------------------------------------------ the engines
// one of: the GPU pipeline (frames stay on the GPU) or a host pipeline (nvl / cpu / ml / apple)
struct Engines {
    std::unique_ptr<GpuPipeline> gpu;
    std::unique_ptr<HostPipeline> host;
    std::string name, desc;
    int w = 0, h = 0, bits = 8;
    std::vector<uint64_t> Frames() const { return gpu ? gpu->Frames() : host ? host->Frames() : std::vector<uint64_t>{}; }
    std::vector<uint64_t> Discs() const { return gpu ? gpu->Discontinuities() : host ? host->Discontinuities() : std::vector<uint64_t>{}; }
    uint64_t Generated() const { return gpu ? gpu->Generated() : host ? host->Generated() : 0; }
    int Steps() const { return host && host->Engine() ? host->Engine()->Steps() : 0; }
};

// bits / pq: the captured pictures (10 + pq: an HDR capture); hdrOut: the overlay is HDR10
bool OpenEngines(Engines& e, const std::string& want, Vk* vk, int w, int h, int bits, bool pq, bool hdrOut, int outW, int outH, const Options& o, double hz,
                 int smoother, bool hostOut, std::string& err)
{
    e = Engines{};
    e.w = w; e.h = h; e.bits = bits;
    std::vector<std::string> order;
    if (want == "auto") {
#ifdef __APPLE__
        order = { "apple", "gpu", "cpu" };
#else
        order = { "nvl", "ml", "gpu", "cpu" };   // NVIDIA Video Effects first where they run (RTX 30 after its self-test)
#endif
    } else {
        order = { want };
        if (want != "cpu") order.push_back(want == "gpu" ? "cpu" : "gpu"), order.push_back("cpu");
    }
    std::string why;
    auto note = [&](const std::string& n, const std::string& w) { why += (why.empty() ? "" : "; ") + n + ": " + w; };
    for (const std::string& n : order) {
        if (n == "nvl" && want == "auto" && !NvlAvailable(nullptr)) continue;
        if (n == "ml" && want == "auto" && !MlInstalled()) continue;
        if (n == "apple" && want == "auto" && !AppleAvailable(nullptr)) continue;
        if (n == "gpu") {
            if (!vk || !vk->dev) { note(n, "no Vulkan device"); continue; }
            auto g = std::make_unique<GpuPipeline>();
            GpuRequest r;
            r.w = w; r.h = h; r.outW = outW; r.outH = outH; r.upscaler = o.upscaler; r.hostOut = hostOut; r.bits = bits; r.pq = pq;
            std::string d, ge;
            if (!g->Open(*vk, r, d, ge)) { note(n, ge); continue; }
            e.gpu = std::move(g); e.name = "gpu"; e.desc = d;
        } else {
            EngineRequest r;
            r.w = w; r.h = h; r.displayHz = hz; r.smoother = smoother; r.displayW = outW; r.displayH = outH;
            r.bits = bits; r.pq = pq; r.hdrOut = hdrOut;
            std::string he;
            auto eng = OpenHostEngine(n, r, he);
            if (!eng) { note(n, he); continue; }
            e.desc = eng->Describe();
            e.host = std::make_unique<HostPipeline>(std::move(eng));
            e.name = n;
        }
        if (n != order.front()) Log("engine %s instead of %s (%s)", n.c_str(), order.front().c_str(), why.c_str());
        return true;
    }
    err = "no engine can run here (" + why + ")";
    return false;
}

// ------------------------------------------------------------------------------------------------ the self-test
// the GPU engine through the capture pipeline on a synthetic moving square (no window, no capture); then the CPU engine
int SelfTest(const Options& o)
{
    const int W = 320, H = 180;
    auto frame = [&](int k, std::vector<uint8_t>& px) {
        px.assign(size_t(W) * H * 4, 40);
        for (int y = 60; y < 120; ++y)
            for (int x = 40 + 8 * k; x < 100 + 8 * k; ++x) { uint8_t* p = &px[(size_t(y) * W + x) * 4]; p[0] = 30; p[1] = 200; p[2] = 230; p[3] = 255; }
    };
    auto centre = [&](const I420& pic) {   // the bright square's horizontal centre (luma above mid grey)
        double sx = 0, n = 0;
        for (int y = 0; y < pic.h; ++y) for (int x = 0; x < pic.w; ++x) if (pic.y[size_t(y) * pic.w + x] > 120) { sx += x; n += 1; }
        return n > 0 ? sx / n : -1.0;
    };
    int fails = 0;
    Vk vk;
    std::string err;
    if (!vk.CreateInstance({}, err) || !vk.CreateDevice(VK_NULL_HANDLE, o.vulkanDevice, true, err)) {
        Log("selftest: no Vulkan device (%s): the GPU engine is skipped", err.c_str());
    } else {
        Log("selftest: Vulkan device %s%s", vk.name.c_str(), vk.cpuDevice ? " (software)" : "");
        GpuPipeline g;
        GpuRequest r; r.w = W; r.h = H; r.upscaler = "off"; r.hostOut = true;
        std::string d;
        if (!g.Open(vk, r, d, err)) { Log("selftest: FAIL GPU engine: %s", err.c_str()); ++fails; }
        else {
            std::vector<uint8_t> px[2];
            for (int k = 0; k < 2 && !fails; ++k) {
                frame(k, px[k]);
                SourceFrame f;
                f.width = W; f.height = H; f.fmt = PixFmt::BGRX; f.ptr = px[k].data(); f.pitch = W * 4;
                if (!g.Add(uint64_t(k), k == 0, f, err)) { Log("selftest: FAIL upload: %s", err.c_str()); ++fails; }
            }
            I420 out;
            Pick p; p.valid = true; p.a = 0; p.b = 1; p.t = 0.5f;
            if (!fails && !g.Show(p, nullptr, &out, err)) { Log("selftest: FAIL generate: %s", err.c_str()); ++fails; }
            if (!fails) {
                const double c = centre(out);   // frame 0: 69.5, frame 1: 77.5; halfway 73.5
                Log("selftest: GPU engine halfway frame: square centre %.2f (want 73.5)", c);
                if (std::fabs(c - 73.5) > 2.0) { Log("selftest: FAIL the generated frame is not halfway"); ++fails; }
            }
            g.Poll();
        }
    }
    EngineRequest er; er.w = W; er.h = H; er.displayHz = 60; er.smoother = 0;
    std::string why;
    if (auto cpu = OpenHostEngine("cpu", er, why)) {
        HostPipeline hp(std::move(cpu));
        for (int k = 0; k < 2; ++k) {
            std::vector<uint8_t> px;
            frame(k, px);
            I420 pic;
            ToI420(px.data(), W * 4, W, H, PixFmt::BGRX, pic, 1);
            hp.Add(uint64_t(k), k == 0, std::move(pic));
        }
        Pick p; p.valid = true; p.a = 0; p.b = 1; p.t = 0.5f;
        const I420* out = hp.Get(p, err);
        const double c = out ? centre(*out) : -1;
        Log("selftest: CPU engine halfway frame: square centre %.2f (want 73.5)", c);
        if (!out || std::fabs(c - 73.5) > 2.0) { Log("selftest: FAIL CPU engine"); ++fails; }
    } else {
        Log("selftest: FAIL CPU engine: %s", why.c_str()); ++fails;
    }
    {   // NVIDIA Video Effects: what the probe finds (no driver: a clean refusal, the next engine runs)
        std::string nv;
        const bool on = NvlAvailable(&nv);
        Log("selftest: nvl: %s (%s)", on ? "available" : "not available", nv.c_str());
    }
    Log("selftest: %s", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}

// ------------------------------------------------------------------------------------------------ the run
int List()
{
    std::vector<WindowInfo> w;
    std::string err;
    bool ok = false;
#if defined(__APPLE__)
    ok = ListMacWindows(w, err);
#elif defined(PMC_HAVE_X11)
    if (std::getenv("DISPLAY")) ok = ListX11Windows(w, err);
    else err = "no X display";
#endif
    if (!ok) {
        std::printf("%s\n", std::getenv("WAYLAND_DISPLAY") ? "On Wayland, capture mode asks for the window in your desktop's own picker (no list needed)." : err.c_str());
        return std::getenv("WAYLAND_DISPLAY") ? 0 : 2;
    }
    for (auto& x : w) std::printf("%-12llu %5dx%-5d %-28s %s\n", static_cast<unsigned long long>(x.id), x.w, x.h, x.owner.c_str(), x.title.c_str());
    return 0;
}

int Run(const Options& o)
{
    if (o.list) return List();
    if (o.selftest) return SelfTest(o);

    const int smootherSetting = pmvs::ParseSmoother(pmvs::ReadSetting("Smoother"), 0);
    int smoother = o.smoother >= 0 ? o.smoother : smootherSetting;

    // ---- the source
    std::unique_ptr<Source> src;
    bool x11src = false;
#if defined(__APPLE__)
    src = MakeSckSource(o);
#else
    const bool wayland = std::getenv("WAYLAND_DISPLAY") && *std::getenv("WAYLAND_DISPLAY");
    const bool xdisp = std::getenv("DISPLAY") && *std::getenv("DISPLAY");
    bool usePortal = o.portal || o.monitor || (wayland && !o.x11 && !o.pick);
#ifndef PMC_HAVE_PORTAL
    usePortal = false;
#endif
#ifdef PMC_HAVE_X11
    if (!usePortal && xdisp) { src = MakeX11Source(o); x11src = true; }
#endif
#ifdef PMC_HAVE_PORTAL
    if (!src && (usePortal || !xdisp)) src = MakePortalSource(o);
#endif
    (void)xdisp;
#endif
    if (!src) { Log("no way to capture a window here (this build has neither X11 nor the screen-capture portal)"); return 2; }
    std::string err;
    (void)x11src;
    if (!src->Start(err)) { Log("%s", err.c_str()); return 3; }
    Log("capturing %s", src->Describe().c_str());

    // ---- the presenter and the Vulkan device
    std::unique_ptr<Presenter> pres;
    if (o.present) {
#if defined(__APPLE__)
        pres = MakeMacPresenter(o, src->Window());
#else
#ifdef PMC_HAVE_X11
        if (x11src) pres = MakeX11Presenter(o, src->Window());
#endif
#ifdef PMC_HAVE_WAYLAND
        if (!pres && wayland) pres = MakeWaylandPresenter(o);
#endif
#ifdef PMC_HAVE_X11
        if (!pres && xdisp) pres = MakeX11Presenter(o, 0);   // the portal on an X11 desktop: full screen
#endif
#endif
        if (!pres) { Log("no way to show the picture here (no X11 or Wayland display)"); return 2; }
        if (!pres->Create(err)) { Log("%s", err.c_str()); return 4; }
    }
    auto vk = std::make_unique<Vk>();
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    bool vkOk = vk->CreateInstance(pres ? pres->InstanceExtensions() : std::vector<const char*>{}, err);
    if (vkOk && pres) { surface = pres->Surface(vk->inst, err); vkOk = surface != VK_NULL_HANDLE; }
    if (vkOk) vkOk = vk->CreateDevice(surface, o.vulkanDevice, o.allowCpuVulkan || (pres == nullptr && o.engine == "gpu"), err);
    if (!vkOk) {
        if (pres) { Log("capture mode needs Vulkan to show its picture: %s", err.c_str()); return 4; }
        Log("no Vulkan device (%s): host engines only", err.c_str());
    } else {
        Log("Vulkan device: %s%s%s", vk->name.c_str(), vk->cpuDevice ? " (software)" : "", vk->dmabuf ? ", dma-buf import" : "");
    }
    double hz = o.displayHz > 0 ? o.displayHz : pres ? pres->RefreshHz() : 0;
    if (hz <= 0) hz = 60;
    if (pres) Log("showing on %s", pres->Describe().c_str());

    std::unique_ptr<Screen> screen;
    if (pres && vkOk) {
        int w = 0, h = 0;
        pres->Size(w, h);
        screen = std::make_unique<Screen>();
        // HDR where the window can show it (macOS EDR displays, Wayland with the compositor's colour management):
        // an HDR10 swapchain, RTX Video HDR on, HDR captures kept HDR; else SDR. X11 stays SDR.
        std::string hdrWhy;
        const bool hdrWanted = pres->HdrCapable(hdrWhy);
        if (hdrWanted) pres->SetHdr(true);
        if (!screen->Init(vk.get(), surface, w, h, hdrWanted, err)) { Log("the screen: %s", err.c_str()); return 4; }
        if (screen->Hdr()) {
            Log("HDR: on (HDR10, BT.2020 PQ; SDR pictures at %d nits)", 203);
            Log("HDR capture: %s", src->SetHdr(true) ? "on (10-bit BT.2020 PQ)" : "off (this source captures SDR)");
        } else {
            if (hdrWanted) pres->SetHdr(false);
            Log("HDR: off, SDR (%s)", hdrWanted ? screen->HdrWhy().c_str() : hdrWhy.c_str());
        }
    }

    Y4m dump;
    Engines eng;
    Timeline tl;
    Protected prot;
    I420 hostPic, outPic;
    uint64_t lastPrint = 0, inFrames = 0, dupFrames = 0, shown = 0, presented = 0, drainCuts = 0;
    // new pictures as they arrive (repeats left out): the source's real cadence, which the timeline's fitted rate does
    // not show (a frame the player dropped is just a gap in the fit). CI's capture runs check it before the motion check.
    uint64_t newPics = 0;
    double firstNew = 0, lastNew = 0;
    uint64_t lastFp = 0;
    bool haveFp = false, hidden = false;
    double start = Now(), lastStats = start, audioAt = 0, reopenAt = 0, nextTick = start;
    int audioMs = -1;
    std::string want = o.engine;
    int rc = 0;
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);
#endif

    auto openFor = [&](int w, int h, int bits, bool pq) -> bool {
        int ow = 0, oh = 0;
        if (screen) { ow = screen->Width(); oh = screen->Height(); }
        if (!OpenEngines(eng, want, vkOk ? vk.get() : nullptr, w, h, bits, pq, screen && screen->Hdr(), ow, oh, o, hz, smoother, !screen, err)) { Log("%s", err.c_str()); return false; }
        if (smoother == 0 && eng.Steps() == 1) smoother = 1;   // upscaling only (nvl without frame generation)
        else if (smoother == 0 && eng.Steps() > 0) smoother = std::clamp(eng.Steps(), 2, 8);   // a k-step engine: its grid
        Log("engine: %s (%s), smoother %s", eng.name.c_str(), eng.desc.c_str(), smoother == 0 ? "Auto" : smoother == 1 ? "off" : ("x" + std::to_string(smoother)).c_str());
        if (!o.dump.empty() && !dump.IsOpen()) {
            const int dw = eng.gpu ? eng.gpu->OutW() : w, dh = eng.gpu ? eng.gpu->OutH() : h;
            if (!dump.Open(o.dump, dw, dh, bits, hz)) Log("cannot write %s", o.dump.c_str());
        }
        tl = Timeline{};
        haveFp = false;
        return true;
    };

    while (!g_stop) {
        // ---- the window
        if (pres) {
            bool resized = false;
            if (!pres->Pump(resized)) { Log("closed"); break; }
            if (resized && screen) {
                int w = 0, h = 0;
                pres->Size(w, h);
                if (!screen->Resize(w, h, err)) { Log("the screen: %s", err.c_str()); rc = 5; break; }
                if (eng.gpu) reopenAt = Now() + 0.5;   // the upscaling target follows, once the size settles
            }
        }
        std::string why;
        if (!src->Alive(why)) { Log("%s", why.c_str()); break; }

        // ---- new frames
        // Only the wait for the very first frame may block (50 ms per call). The wait used to be decided once before this
        // loop, so the whole first drain waited 50 ms per call: a player drawing more often than every 50 ms (24 fps: every
        // 42 ms) always had a new picture by then, the loop never ended, nothing was shown and --frames was never reached
        // (CI linux-x11 hung right after the engine line in every run). Now the wait is decided per call, and taking frames
        // also stops after one refresh: a source that outruns the engine (not measured; llvmpipe could) still gets shown.
        // The X11 source keeps only the newest picture, so a picture left for the next refresh is not a backlog.
        SourceFrame f;
        const double drainUntil = Now() + 1.0 / hz;
        for (;;) {
            if (tl.Valid() && Now() >= drainUntil) { ++drainCuts; break; }
            if (!src->Next(f, tl.Valid() ? 0 : 50)) break;
            const int w = f.width & ~1, h = f.height & ~1;
            const int fbits = IsHdr10(f.fmt) ? 10 : 8;
            const bool fpq = IsHdr10(f.fmt);
            if (w < 16 || h < 16) { f.Release(); continue; }
            if (!eng.gpu && !eng.host) { if (!openFor(w, h, fbits, fpq)) { f.Release(); rc = 6; g_stop = true; break; } }
            else if (w != eng.w || h != eng.h || fbits != eng.bits || (reopenAt > 0 && Now() >= reopenAt)) {
                Log("the picture is now %dx%d", w, h);
                reopenAt = 0;
                eng = Engines{};
                if (!openFor(w, h, fbits, fpq)) { f.Release(); rc = 6; g_stop = true; break; }
            }
            f.width = w; f.height = h;
            ++inFrames;
            if (f.ptr) {
                // a player that draws the same picture again (a paused video, an OSD-free redraw): no new frame
                const uint64_t fp = Fingerprint(f.ptr, f.pitch, w, h);
                if (haveFp && fp == lastFp) { ++dupFrames; f.Release(); continue; }
                lastFp = fp; haveFp = true;
                bool told = false;
                const int pr = prot.Feed(f.ptr, f.pitch, w, h, f.time, told);
                if (pr == 1 && pres) { pres->SetVisible(false); hidden = true; }
                if (pr == -1 && pres) { pres->SetVisible(true); hidden = false; }
                if (told) Log("%s", kProtectedMessage);
            }
            if (newPics++ == 0) firstNew = Now();
            lastNew = Now();
            bool disc = false;
            const uint64_t idx = tl.Add(f.time, disc, Now());   // its time, and when it got here (the delay covers both)
            if (eng.gpu) {
                if (!eng.gpu->Add(idx, disc, f, err)) { Log("GPU engine: %s", err.c_str()); f.Release(); rc = 7; g_stop = true; break; }
            } else {
                if (!f.ptr) { f.Release(); Log("the captured picture is not readable by the CPU"); rc = 7; g_stop = true; break; }
                ToI420(f.ptr, f.pitch, w, h, f.fmt, hostPic);
                f.Release();
                eng.host->Add(idx, disc, std::move(hostPic));
                hostPic = I420{};
            }
        }
        if (eng.gpu) eng.gpu->Poll();
        if (g_stop) break;
        if (!tl.Valid() || (!eng.gpu && !eng.host)) continue;

        // ---- one screen refresh
        if (hidden || (pres && !screen)) { std::this_thread::sleep_for(std::chrono::milliseconds(int(1000 / hz))); continue; }
        if (screen) {
            const int b = screen->Begin(err);
            if (b < 0) { Log("%s", err.c_str()); rc = 5; break; }
            if (b == 1) { int w = 0, h = 0; pres->Size(w, h); if (!screen->Resize(w, h, err)) { Log("the screen: %s", err.c_str()); rc = 5; break; } continue; }
        } else {
            // no window: pace at the display rate ourselves
            nextTick += 1.0 / hz;
            const double sleep = nextTick - Now();
            if (sleep > 0) std::this_thread::sleep_for(std::chrono::microseconds(int64_t(sleep * 1e6)));
            else if (sleep < -0.25) nextTick = Now();
        }
        const double pos = tl.Position(Now());
        const std::vector<uint64_t> frames = eng.Frames();
        if (frames.empty()) continue;
        Pick p = Choose(pos, smoother, frames, eng.Discs());
        if (!p.valid) { p.valid = true; p.a = p.b = pos < double(frames.front()) ? frames.front() : frames.back(); p.t = 0; }
        if (eng.gpu) {
            if (!eng.gpu->Show(p, screen.get(), screen ? nullptr : &outPic, err)) { Log("GPU engine: %s", err.c_str()); rc = 7; break; }
            if (!screen) dump.Write(outPic);
        } else {
            const I420* pic = eng.host->Get(p, err);
            if (!pic) { Log("engine: %s", err.c_str()); rc = 7; break; }
            if (screen && !screen->ShowHost(*pic, err)) { Log("the screen: %s", err.c_str()); rc = 5; break; }
            if (!screen) dump.Write(*pic);
        }
        if (screen) {
            const int r = screen->Present(err);
            if (r < 0) { Log("%s", err.c_str()); rc = 5; break; }
            if (r == 1) { int w = 0, h = 0; pres->Size(w, h); if (!screen->Resize(w, h, err)) { Log("the screen: %s", err.c_str()); rc = 5; break; } }
            ++presented;
        }
        ++shown;

        // ---- lip sync: the picture runs Delay() + about one refresh behind the player's own
        const double now = Now();
        if (!o.audioConf.empty() && now - start > 2.0 && now >= audioAt) {
            const int ms = o.audioDelayMs >= 0 ? o.audioDelayMs : int(std::lround((tl.Delay() + 1.0 / hz) * 1000));
            if (audioMs < 0 || std::abs(ms - audioMs) > 15) {
                std::string w;
                if (SetAudioDelay(o.audioConf, ms, w)) { Log("lip sync: audio-delay +%d ms in %s (the player takes it at its next start)", ms, o.audioConf.c_str()); audioMs = ms; }
                else Log("lip sync: %s", w.c_str());
            }
            audioAt = now + 10;
        }
        if (now - lastStats >= 5.0 && (g_verbose || shown - lastPrint > 0)) {
            const double dt = now - lastStats;
            Log("source %.2f fps (%llu captured, %llu repeats; taking frames ran past a refresh %llu times), shown %.1f fps, %llu generated, delay %.0f ms%s%s",
                tl.Fps(), static_cast<unsigned long long>(inFrames), static_cast<unsigned long long>(dupFrames),
                static_cast<unsigned long long>(drainCuts), double(shown - lastPrint) / dt,
                static_cast<unsigned long long>(eng.Generated()), tl.Delay() * 1000, eng.gpu ? "; " : "", eng.gpu ? eng.gpu->Stats().c_str() : "");
            lastStats = now; lastPrint = shown;
        }
        if ((o.frames > 0 && long(shown) >= o.frames) || (o.seconds > 0 && now - start >= o.seconds)) break;
    }
    if (!o.audioConf.empty() && audioMs >= 0) ClearAudioDelay(o.audioConf);
    Log("done: %llu frames shown (%llu presented), %llu generated; source: %llu new pictures in %.1f s (%.2f a second)",
        static_cast<unsigned long long>(shown), static_cast<unsigned long long>(presented), static_cast<unsigned long long>(eng.Generated()),
        static_cast<unsigned long long>(newPics), lastNew - firstNew, newPics > 1 && lastNew > firstNew ? double(newPics - 1) / (lastNew - firstNew) : 0.0);
    // the order matters: frames lent by the source go back before it closes; the swapchain before its surface; the
    // surface before the instance; the window last
    eng = Engines{};
    screen.reset();
    if (surface && vk->inst) vkDestroySurfaceKHR(vk->inst, surface, nullptr);
    vk.reset();
    src.reset();
    pres.reset();
    return rc;
}

} // namespace
} // namespace pmc

int main(int argc, char** argv)
{
    pmc::Options o;
    std::string why;
    if (!pmc::ParseOptions(argc, argv, o, why)) { std::fprintf(stderr, "pushmpc-capture: %s\n%s", why.c_str(), pmc::kUsage); return 2; }
    if (o.help) { std::fputs(pmc::kUsage, stdout); return 0; }
    pmc::g_verbose = o.verbose;
    if (!o.log.empty()) pmc::g_log = std::fopen(o.log.c_str(), "a");
#if defined(__APPLE__)
    // AppKit wants the main thread: the run goes to a worker (no window: it can stay here)
    if (o.present && !o.list && !o.selftest) return pmc::MacMain([&] { return pmc::Run(o); });
#endif
    return pmc::Run(o);
}
