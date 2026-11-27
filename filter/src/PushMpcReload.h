// PushMpcReload.h: the settings reload (pushcore_reload, a graph rebuild) on its own thread, never under the filter's
// core lock, never at the same time as another pushcore close, and never waited for without a bound.
//
// Why (F-T1, 2026-11-27): DoReload ran pushcore_reload on the copier thread while holding the core lock, and the
// idle quick graph (fast start) was closed on another thread in the same millisecond (DrainCore -> CloseRetired). In
// 4 of 5 Auto rebuilds neither the close nor the reload ever returned; the deliverer then blocked on the core lock at
// its 10-s stats line (frozen picture) and Stop blocked joining the copier (MPC-HC's watchdog killed the process).
// The one rebuild that completed had the close finish first, then the reload. So here:
//   1. the idle quick graph (and any pushcore close still running on its own thread) is closed BEFORE the reload,
//      one after the other on the reload's thread, as in that working run (and as in 1.0.0, where no second graph was
//      alive at a reload);
//   2. the instance is taken out of the filter's slot under the core lock, then the lock is released: every other
//      thread (Receive, the deliverer's stats, EndFlush, Stop) only ever waits for short pushcore calls, and the
//      source frames are shown meanwhile;
//   3. the caller waits with a bound: on Stop it gives the reload a short grace and then abandons it; after the
//      timeout it abandons it too. An abandoned reload's thread closes the instance itself when (if) pushcore returns,
//      and touches nothing of the filter (it holds only the job and the API table).
// Fast reloads behave as before: the same instance goes back into the slot with its new output info.
// Header-only so filter/tools/reload_test.cpp drives this same code against a stub pushcore API (CPU only).
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "pushmpc/pushcore_abi.h"

namespace pm::reload {

using Clock = std::chrono::steady_clock;
inline double MsSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

// Process-wide count of pushcore work running on threads of its own (closes, reloads). A reload waits until no close
// runs; a close on the player's thread is made synchronously only when nothing runs (else pushcore's graph lock could
// be held by a stuck instance and the player would wait for it).
class Background {
public:
    void BeginClose() { std::lock_guard<std::mutex> lk(m_); ++closes_; }
    void EndClose() { { std::lock_guard<std::mutex> lk(m_); --closes_; } cv_.notify_all(); }
    void BeginReload() { std::lock_guard<std::mutex> lk(m_); ++reloads_; }
    void EndReload() { { std::lock_guard<std::mutex> lk(m_); --reloads_; } cv_.notify_all(); }
    int Closes() { std::lock_guard<std::mutex> lk(m_); return closes_; }
    int Reloads() { std::lock_guard<std::mutex> lk(m_); return reloads_; }
    bool Idle() { std::lock_guard<std::mutex> lk(m_); return closes_ == 0 && reloads_ == 0; }
    // true when no close runs (any more) within ms
    bool WaitNoClose(double ms)
    {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, std::chrono::duration<double, std::milli>(ms), [&] { return closes_ == 0; });
    }
private:
    std::mutex m_;
    std::condition_variable cv_;
    int closes_ = 0, reloads_ = 0;
};
inline Background& Busy() { static Background b; return b; }

using CloseFn = void (*)(const PushCoreApi*, PushCore*);              // exception-safe pushcore close, no filter state
using LogFn = void (*)(const std::string&);
using SpawnFn = std::function<void(std::function<void()>)>;            // runs a function on a detached thread

// A pushcore close on a thread of its own, counted in Busy() (the filter's CloseAsync goes through this)
inline void CloseDetached(const SpawnFn& spawn, const PushCoreApi* api, PushCore* core, CloseFn close, LogFn log, const char* what, unsigned delayMs = 0)
{
    if (!api || !core) return;
    Busy().BeginClose();
    spawn([api, core, close, log, what, delayMs] {
        if (delayMs) std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        const auto t = Clock::now();
        close(api, core);
        if (log) log(std::format("{} closed in {:.0f} ms (on its own thread)", what, MsSince(t)));
        Busy().EndClose();
    });
}

struct Job {
    std::mutex lock;
    std::condition_variable cv;
    bool done = false, abandoned = false;
    int result = PUSHCORE_ERROR;
    bool closesStuck = false;        // earlier closes did not finish in time: the reload was not started
    PushCoreOutputInfo oi = {};
    std::string err;
    double retiredMs = 0, waitMs = 0, reloadMs = 0;
};

// Starts the reload of `core` on its own thread: first `retired` is closed (if any), then it waits up to closesWaitMs
// for every other pushcore close to finish, then pushcore_reload + pushcore_outputInfo. The caller no longer touches
// `core` until the job is done (Wait), and never after it abandoned the job.
inline std::shared_ptr<Job> Start(const PushCoreApi* api, PushCore* core, PushCore* retired, CloseFn close, LogFn log, const SpawnFn& spawn, double closesWaitMs)
{
    auto job = std::make_shared<Job>();
    Busy().BeginReload();
    if (retired) Busy().BeginClose();
    spawn([job, api, core, retired, close, log, closesWaitMs] {
        if (retired) {
            const auto t = Clock::now();
            close(api, retired);
            job->retiredMs = MsSince(t);
            if (log) log(std::format("quick start: the idle quick graph closed in {:.0f} ms, before the reload (on the reload's thread)", job->retiredMs));
            Busy().EndClose();
        }
        int r = PUSHCORE_ERROR;
        std::string err;
        PushCoreOutputInfo oi = {};
        const auto tw = Clock::now();
        const bool closesDone = Busy().WaitNoClose(closesWaitMs);
        const double waitMs = MsSince(tw);
        double reloadMs = 0;
        if (!closesDone) {
            err = std::format("an earlier pushcore close still runs after {:.0f} ms: the reload was not started", waitMs);
        } else {
            const auto tr = Clock::now();
            r = api->reload(core);
            if (r == PUSHCORE_OK) r = api->outputInfo(core, &oi);
            else { const char* e = api->lastError ? api->lastError(core) : nullptr; err = e ? e : ""; }
            reloadMs = MsSince(tr);
        }
        bool abandoned = false;
        {
            std::lock_guard<std::mutex> lk(job->lock);
            job->result = r; job->err = err; job->oi = oi; job->closesStuck = !closesDone;
            job->waitMs = waitMs; job->reloadMs = reloadMs;
            job->done = true;
            abandoned = job->abandoned;
        }
        job->cv.notify_all();
        if (abandoned) {
            // the filter gave up on this reload (stop, timeout): the instance is this thread's to close
            const auto t = Clock::now();
            Busy().BeginClose();
            close(api, core);
            Busy().EndClose();
            if (log) log(std::format("settings reload: the abandoned reload returned {} after {:.0f} ms; its instance closed in {:.0f} ms (on its own thread)", r, reloadMs, MsSince(t)));
        }
        Busy().EndReload();
    });
    return job;
}

enum class Outcome { Done, Stopped, TimedOut };

// Waits for the job, calling tick() every few ms (flush acknowledgements). quit() true: a grace of quitGraceMs, then
// the job is abandoned (Stopped). timeoutMs over: abandoned (TimedOut). Done: the caller owns the instance again.
inline Outcome Wait(Job& job, const std::function<bool()>& quit, const std::function<void()>& tick, double timeoutMs, double quitGraceMs)
{
    const auto t0 = Clock::now();
    Clock::time_point quitAt{};
    bool quitting = false;
    std::unique_lock<std::mutex> lk(job.lock);
    for (;;) {
        if (job.done) return Outcome::Done;
        job.cv.wait_for(lk, std::chrono::milliseconds(5), [&] { return job.done; });
        if (job.done) return Outcome::Done;
        lk.unlock();
        if (tick) tick();
        const bool q = quit && quit();
        lk.lock();
        if (job.done) return Outcome::Done;
        if (q && !quitting) { quitting = true; quitAt = Clock::now(); }
        if (quitting && MsSince(quitAt) >= quitGraceMs) { job.abandoned = true; return Outcome::Stopped; }
        if (MsSince(t0) >= timeoutMs) { job.abandoned = true; return Outcome::TimedOut; }
    }
}

struct Limits {
    double timeoutMs = 20000;      // the whole reload (closes first, then the rebuild); the source frames show meanwhile
    double quitGraceMs = 1000;     // Stop: how long a nearly finished reload may still take
    double closesWaitMs = 15000;   // how long the reload waits for earlier closes before it gives up
};
struct Hooks {
    CloseFn close = nullptr;
    LogFn log = nullptr;
    SpawnFn spawn;
    std::function<bool()> quit;            // Stop / quit asked
    std::function<void()> tick;            // while waiting (the copier acknowledges flushes here)
    std::function<bool()> keep;            // under the core lock: may the reloaded instance go back (not faulted/stopped)
};
struct Result {
    Outcome how = Outcome::Done;
    bool hadCore = false;
    int r = PUSHCORE_ERROR;
    bool closesStuck = false;
    bool reinstalled = false;              // the instance is back in the slot (r OK or DEVICE_LOST)
    PushCoreOutputInfo oi = {};
    std::string err;
    double retiredMs = 0, waitMs = 0, reloadMs = 0, totalMs = 0;
};

// The whole reload, as the filter's copier runs it: takes the instance out of `slot` (under coreLock), runs the job
// with the lock released, and puts the instance back (under coreLock) when it reloaded or reported a lost device.
// A failed reload's instance is closed on its own thread; a stopped / timed-out one is closed by the job's thread.
// `retired` (already taken out of the filter by the caller) is closed first, on the job's thread.
inline Result Run(const PushCoreApi* api, std::mutex& coreLock, PushCore*& slot, PushCore* retired, const Hooks& h, const Limits& lim)
{
    Result res;
    const auto t0 = Clock::now();
    PushCore* core = nullptr;
    { std::lock_guard<std::mutex> lk(coreLock); core = slot; slot = nullptr; }
    res.hadCore = core != nullptr;
    if (!core) {
        if (retired) CloseDetached(h.spawn, api, retired, h.close, h.log, "the quick graph");
        res.err = "no pushcore instance";
        res.totalMs = MsSince(t0);
        return res;
    }
    auto job = Start(api, core, retired, h.close, h.log, h.spawn, lim.closesWaitMs);
    res.how = Wait(*job, h.quit, h.tick, lim.timeoutMs, lim.quitGraceMs);
    res.totalMs = MsSince(t0);
    if (res.how != Outcome::Done) return res;   // the job's thread closes the instance
    {
        std::lock_guard<std::mutex> jl(job->lock);
        res.r = job->result; res.oi = job->oi; res.err = job->err; res.closesStuck = job->closesStuck;
        res.retiredMs = job->retiredMs; res.waitMs = job->waitMs; res.reloadMs = job->reloadMs;
    }
    if (!res.closesStuck && (res.r == PUSHCORE_OK || res.r == PUSHCORE_DEVICE_LOST)) {
        std::lock_guard<std::mutex> lk(coreLock);
        if (!slot && (!h.keep || h.keep())) { slot = core; res.reinstalled = true; }
    }
    if (!res.reinstalled) CloseDetached(h.spawn, api, core, h.close, h.log, "pushcore (after the settings reload)");
    return res;
}

}  // namespace pm::reload
