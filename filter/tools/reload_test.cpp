// reload_test: the settings reload of the filter (filter/src/PushMpcReload.h, used by CPushMpcFilter::DoReload) against
// a stub pushcore API whose reload and close can be fast, slow, blocked until released, or hang while the other one
// runs (the F-T1 interleaving: the idle quick graph's close in flight when the reload of the full graph starts).
// CPU only (no device, no pushcore.dll). Threads that stand for Receive and the deliverer's stats take the same core
// lock the reload uses, so a reload that held it would show up as a gap in their progress.
// Exit code 0 = all passed. One line per check and a RESULT line.
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "PushMpcReload.h"

using namespace std::chrono_literals;
namespace rl = pm::reload;

static int g_fail = 0, g_checks = 0;
static void Check(bool ok, const std::string& what)
{
    ++g_checks;
    if (!ok) ++g_fail;
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    std::fflush(stdout);
}
static std::mutex g_logLock;
static void Log(const std::string& s) { std::lock_guard<std::mutex> lk(g_logLock); std::printf("     log: %s\n", s.c_str()); std::fflush(stdout); }

// ------------------------------------------------------------------------------------------------ the stub pushcore
enum class Mode { Fast, Slow, Block, HangIfOther };   // HangIfOther: blocks while the other kind of call runs at its start
struct StubCore {
    int id = 0;
    Mode reload = Mode::Fast, close = Mode::Fast;
    int slowMs = 300;
    std::atomic<bool> closed{ false };
    std::atomic<int> reloads{ 0 };
};
static std::atomic<int> g_inClose{ 0 }, g_inReload{ 0 }, g_overlap{ 0 };
static HANDLE g_release = nullptr;   // manual-reset: releases every blocked call
static StubCore* S(PushCore* c) { return reinterpret_cast<StubCore*>(c); }
static PushCore* P(StubCore& c) { return reinterpret_cast<PushCore*>(&c); }

static void Behave(Mode m, int slowMs, bool otherRuns)
{
    switch (m) {
    case Mode::Fast: Sleep(2); break;
    case Mode::Slow: Sleep(slowMs); break;
    case Mode::Block: WaitForSingleObject(g_release, INFINITE); break;
    case Mode::HangIfOther: if (otherRuns) WaitForSingleObject(g_release, INFINITE); else Sleep(2); break;
    }
}
static int StubReload(PushCore* c)
{
    const bool other = g_inClose.load() > 0;
    if (other) ++g_overlap;
    ++g_inReload;
    S(c)->reloads++;
    Behave(S(c)->reload, S(c)->slowMs, other);
    --g_inReload;
    return PUSHCORE_OK;
}
static void StubClose(PushCore* c)
{
    const bool other = g_inReload.load() > 0;
    if (other) ++g_overlap;
    ++g_inClose;
    Behave(S(c)->close, S(c)->slowMs, other);
    S(c)->closed = true;
    --g_inClose;
}
static int StubOutputInfo(PushCore* c, PushCoreOutputInfo* oi) { *oi = {}; oi->width = 1920; oi->height = 1080; oi->multiplier = 4; (void)c; return PUSHCORE_OK; }
static const char* StubLastError(PushCore*) { return "stub"; }
static PushCoreApi MakeApi()
{
    PushCoreApi a = {};
    a.structSize = sizeof(a);
    a.reload = &StubReload; a.outputInfo = &StubOutputInfo; a.lastError = &StubLastError; a.close = &StubClose;
    return a;
}
static const PushCoreApi g_api = MakeApi();
static void CloseFn(const PushCoreApi* a, PushCore* c) { a->close(c); }
static void Spawn(std::function<void()> f) { std::thread(std::move(f)).detach(); }

// ------------------------------------------------------------------------------------------------ a filter stand-in
// The core slot and its lock, a "Receive" thread (every 2 ms: the core lock, a submit when there is an instance, else a
// source frame shown) and a "deliverer" (every 10 ms: the core lock for stats, as at its 10-s stats line).
struct Filter {
    std::mutex coreLock;
    PushCore* core = nullptr;
    std::atomic<bool> quit{ false }, stopThreads{ false };
    std::atomic<int> sourceFrames{ 0 }, submits{ 0 }, delivered{ 0 };
    std::atomic<double> maxGapMs{ 0 };
    std::vector<std::thread> threads;
    void Start()
    {
        auto loop = [this](int periodMs, bool receive) {
            auto last = rl::Clock::now();
            while (!stopThreads) {
                {
                    std::lock_guard<std::mutex> lk(coreLock);   // blocking, as Receive's submit and the old stats line
                    if (receive) { if (core) ++submits; else ++sourceFrames; } else ++delivered;
                }
                const double gap = rl::MsSince(last);
                last = rl::Clock::now();
                double m = maxGapMs.load();
                while (gap > m && !maxGapMs.compare_exchange_weak(m, gap)) {}
                Sleep(periodMs);
            }
        };
        threads.emplace_back(loop, 2, true);
        threads.emplace_back(loop, 10, false);
    }
    void Stop() { stopThreads = true; for (auto& t : threads) t.join(); threads.clear(); }
    rl::Hooks Hooks()
    {
        rl::Hooks h;
        h.close = &CloseFn; h.log = &Log; h.spawn = &Spawn;
        h.quit = [this] { return quit.load(); };
        h.tick = [] {};
        h.keep = [] { return true; };
        return h;
    }
};

static bool WaitIdle(double ms)
{
    const auto t = rl::Clock::now();
    while (!rl::Busy().Idle()) { if (rl::MsSince(t) > ms) return false; Sleep(5); }
    return true;
}
static void Reset() { ResetEvent(g_release); g_overlap = 0; }

int main()
{
    g_release = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    {   // 1. a fast reload: the same instance back in the slot, quickly, as before
        Reset();
        Filter f; StubCore c; c.id = 1; f.core = P(c);
        const rl::Result r = rl::Run(&g_api, f.coreLock, f.core, nullptr, f.Hooks(), rl::Limits{});
        Check(r.how == rl::Outcome::Done && r.r == PUSHCORE_OK && r.reinstalled && f.core == P(c), "fast reload: done, the instance is back in the slot");
        Check(r.oi.width == 1920 && r.oi.multiplier == 4, "fast reload: the new output info is returned");
        Check(r.totalMs < 500, std::format("fast reload: {:.0f} ms (< 500)", r.totalMs));
        Check(!c.closed && c.reloads == 1 && WaitIdle(1000), "fast reload: nothing closed, nothing left running");
    }
    {   // 2. control: the OLD interleaving (the quick graph's close started on another thread, the reload at once) hangs this stub
        Reset();
        StubCore quick; quick.id = 2; quick.close = Mode::Slow; quick.slowMs = 300;
        StubCore full; full.id = 3; full.reload = Mode::HangIfOther;
        std::thread closer([&] { StubClose(P(quick)); });
        Sleep(20);
        std::atomic<bool> returned{ false };
        std::thread reloader([&] { StubReload(P(full)); returned = true; });
        Sleep(600);
        Check(!returned.load() && g_overlap.load() > 0, "control: close in flight + reload started at once -> the stub reload hangs (the F-T1 interleaving)");
        SetEvent(g_release);
        closer.join(); reloader.join();
    }
    {   // 3. the F-T1 case through the fix: the retired quick graph is closed first, then the reload; never both at once
        Reset();
        Filter f; f.Start();
        StubCore quick; quick.id = 4; quick.close = Mode::HangIfOther;
        quick.slowMs = 300;
        StubCore full; full.id = 5; full.reload = Mode::HangIfOther;
        f.core = P(full);
        rl::Limits lim; lim.timeoutMs = 3000;
        const rl::Result r = rl::Run(&g_api, f.coreLock, f.core, P(quick), f.Hooks(), lim);
        Check(r.how == rl::Outcome::Done && r.reinstalled && f.core == P(full), "retired quick graph + reload: done, the full graph is back");
        Check(g_overlap.load() == 0, "retired quick graph + reload: the close and the reload never overlapped");
        Check(quick.closed.load() && !full.closed.load(), "retired quick graph + reload: the quick graph is closed, the full graph kept");
        f.Stop();
    }
    {   // 4. a close started earlier (CloseAsync at a pause) still runs: the reload waits for it, then runs alone
        Reset();
        Filter f; f.Start();
        StubCore old; old.id = 6; old.close = Mode::Slow; old.slowMs = 400;
        StubCore full; full.id = 7; full.reload = Mode::HangIfOther;
        f.core = P(full);
        rl::CloseDetached(&Spawn, &g_api, P(old), &CloseFn, &Log, "the quick graph (earlier close)");
        Sleep(20);
        const int before = f.sourceFrames.load();
        const rl::Result r = rl::Run(&g_api, f.coreLock, f.core, nullptr, f.Hooks(), rl::Limits{});
        Check(r.how == rl::Outcome::Done && r.reinstalled && g_overlap.load() == 0, std::format("earlier close in flight: the reload waited {:.0f} ms for it, then ran alone", r.waitMs));
        Check(r.waitMs >= 250, "earlier close in flight: the wait covered the close");
        Check(f.sourceFrames.load() - before > 20, std::format("earlier close in flight: source frames shown meanwhile ({})", f.sourceFrames.load() - before));
        f.Stop();
        Check(WaitIdle(2000), "earlier close in flight: nothing left running");
    }
    {   // 5. the reload blocks (pushcore never returns): frames keep flowing, the deliverer never stalls, Stop returns in bound
        Reset();
        Filter f; f.Start();
        StubCore full; full.id = 8; full.reload = Mode::Block; full.close = Mode::Fast;
        f.core = P(full);
        std::thread stopper([&] { Sleep(11000); f.quit = true; });   // past the 10-s stats tick of the old code
        f.maxGapMs = 0;
        const int s0 = f.sourceFrames.load(), d0 = f.delivered.load();
        const auto tq = rl::Clock::now();
        const rl::Result r = rl::Run(&g_api, f.coreLock, f.core, nullptr, f.Hooks(), rl::Limits{});
        const double afterQuit = rl::MsSince(tq) - 11000;
        stopper.join();
        Check(r.how == rl::Outcome::Stopped, "reload blocked: Stop abandons it");
        Check(afterQuit < 1500, std::format("reload blocked: Stop returned {:.0f} ms after it was asked (grace 1000, bound 1500)", afterQuit));
        Check(f.core == nullptr, "reload blocked: the filter no longer holds the instance");
        Check(f.sourceFrames.load() - s0 > 1000 && f.delivered.load() - d0 > 500,
              std::format("reload blocked: source frames {} and deliverer ticks {} kept flowing for 11 s", f.sourceFrames.load() - s0, f.delivered.load() - d0));
        Check(f.maxGapMs.load() < 200, std::format("reload blocked: the longest wait for the core lock was {:.0f} ms (< 200)", f.maxGapMs.load()));
        // the player's close and the next file while the stuck reload still runs
        Check(!rl::Busy().Idle(), "reload blocked: Busy() reports it (CloseCore closes on its own thread instead of waiting)");
        StubCore next; next.id = 9;
        Filter g; g.core = P(next);
        rl::Limits lim; lim.timeoutMs = 2000;
        const auto t2 = rl::Clock::now();
        const rl::Result r2 = rl::Run(&g_api, g.coreLock, g.core, nullptr, g.Hooks(), lim);
        Check(r2.how == rl::Outcome::Done && r2.reinstalled && rl::MsSince(t2) < 2500, std::format("reload blocked: the next instance's reload still returns ({:.0f} ms)", rl::MsSince(t2)));
        f.Stop();
        Check(!full.closed.load(), "reload blocked: the abandoned instance is not closed under the stuck call");
        SetEvent(g_release);
        Check(WaitIdle(3000) && full.closed.load(), "reload blocked: once pushcore returns, the job's thread closes the abandoned instance");
    }
    {   // 6. a slow reload past the timeout: abandoned (source frames stay), closed by its thread when it returns
        Reset();
        Filter f; f.Start();
        StubCore full; full.id = 10; full.reload = Mode::Slow; full.slowMs = 1500;
        f.core = P(full);
        rl::Limits lim; lim.timeoutMs = 500;
        const rl::Result r = rl::Run(&g_api, f.coreLock, f.core, nullptr, f.Hooks(), lim);
        Check(r.how == rl::Outcome::TimedOut && r.totalMs < 800 && f.core == nullptr, std::format("slow reload: abandoned after {:.0f} ms (timeout 500)", r.totalMs));
        f.Stop();
        Check(WaitIdle(3000) && full.closed.load() && full.reloads == 1, "slow reload: closed by its own thread after it returned");
    }
    {   // 7. a close that never returns: the reload is not started beside it; bounded; the instance is not reinstalled
        Reset();
        Filter f; f.Start();
        StubCore stuck; stuck.id = 11; stuck.close = Mode::Block;
        StubCore full; full.id = 12;
        f.core = P(full);
        rl::CloseDetached(&Spawn, &g_api, P(stuck), &CloseFn, &Log, "a stuck close");
        Sleep(20);
        rl::Limits lim; lim.closesWaitMs = 400; lim.timeoutMs = 3000;
        const rl::Result r = rl::Run(&g_api, f.coreLock, f.core, nullptr, f.Hooks(), lim);
        Check(r.how == rl::Outcome::Done && r.closesStuck && !r.reinstalled && full.reloads == 0 && r.totalMs < 1000,
              std::format("stuck close: the reload was not started beside it; reported after {:.0f} ms", r.totalMs));
        Check(f.core == nullptr, "stuck close: the filter falls back (no instance in the slot)");
        f.Stop();
        SetEvent(g_release);
        Check(WaitIdle(3000) && stuck.closed.load() && full.closed.load(), "stuck close: everything closed once pushcore returns");
    }
    {   // 8. Stop during a reload that finishes inside the grace: the instance is kept, as before
        Reset();
        Filter f; f.Start();
        StubCore full; full.id = 13; full.reload = Mode::Slow; full.slowMs = 400;
        f.core = P(full);
        std::thread stopper([&] { Sleep(100); f.quit = true; });
        const rl::Result r = rl::Run(&g_api, f.coreLock, f.core, nullptr, f.Hooks(), rl::Limits{});
        stopper.join();
        Check(r.how == rl::Outcome::Done && r.reinstalled && f.core == P(full), "Stop during a short reload: it finishes in the grace and is kept");
        f.Stop();
        Check(WaitIdle(1000), "Stop during a short reload: nothing left running");
    }

    {   // 9. Stop while the quick graph still closes (before the rebuild started): no rebuild for a stopped player
        Reset();
        Filter f; f.Start();
        StubCore quick; quick.id = 14; quick.close = Mode::Slow; quick.slowMs = 600;
        StubCore full; full.id = 15;
        f.core = P(full);
        std::thread stopper([&] { Sleep(50); f.quit = true; });
        rl::Limits lim; lim.quitGraceMs = 100;
        const rl::Result r = rl::Run(&g_api, f.coreLock, f.core, P(quick), f.Hooks(), lim);
        stopper.join();
        Check(r.how == rl::Outcome::Stopped && r.totalMs < 400, std::format("Stop during the quick graph's close: abandoned after {:.0f} ms", r.totalMs));
        f.Stop();
        Check(WaitIdle(3000) && quick.closed.load() && full.closed.load() && full.reloads == 0, "Stop during the quick graph's close: both closed, no rebuild started");
    }

    std::printf("RESULT %s: %d of %d checks passed\n", g_fail ? "FAIL" : "PASS", g_checks - g_fail, g_checks);
    return g_fail ? 1 : 0;
}
