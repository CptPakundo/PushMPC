// PushMpcLearned.h: what Auto keeps between sessions of what this GPU can do (HKCU\Software\PushMPC\Learned, model 4),
// built so that load from other programs on the GPU cannot lower later picks for good.
//
// Principle: foreign load can only make a card look SLOWER, never faster. So
//   - within a session the costs follow the measurements as before (LearnCosts: the running graph's p95, smoothed;
//     the step-downs, the delivery check and the admission react to "how it does now");
//   - between sessions each value keeps a short history, one entry per session that measured it (the session's last
//     value). The saved value is the LOWEST of the last kKeepSessions entries: a faster session lowers it at once, a
//     slower one raises it only once kKeepSessions sessions in a row measured it slower (the low one drops out);
//   - a value slower than its prior that fewer than kKeepSessions sessions measured is not trusted: the prior stays
//     (with Decide's own family factors) until enough sessions agree;
//   - an entry older than kStaleSessions sessions expires: a value Auto stopped running goes back to its prior, so
//     the candidate is tried again (the per-file step-downs and the 5-min failed-upgrade ban bound the churn);
//   - the PushFlow speed test keeps the same history per tier value (its median, not its p95), the first run trusted
//     (it measures this card): a faster rerun replaces a slower result, a slower one only when kKeepSessions runs agree;
//     it runs again once its newest result is kBenchStaleSessions sessions old.
// The plain values (FgScale3, NrMs, PfFirst1, ...) are the saved values the readers use (the filter and
// vsplugin/src/autopick.h); "<name>.h" holds the history ("session:value;..."); "Session" counts the sessions.
// Model 4: values learned under the old rules (model 3: every value followed the last measurement) are dropped.
// Pure functions over a name -> text map, so filter/tools/auto_test.cpp runs the same rules without a registry or GPU.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <format>
#include <map>
#include <string>
#include <vector>

#include "PushMpcAuto.h"

namespace pm::learned {

inline constexpr const wchar_t* kModel = L"4";
inline constexpr int kKeepSessions = 3;        // a slower value is saved only when this many sessions in a row agree
inline constexpr uint32_t kStaleSessions = 20; // a value not measured for this many sessions goes back to its prior
inline constexpr uint32_t kBenchStaleSessions = 10;   // the PushFlow speed test runs again after this many sessions

using Values = std::map<std::wstring, std::wstring>;   // registry value name -> REG_SZ text

struct Entry { uint32_t session = 0; double v = 0; };
inline std::vector<Entry> Decode(const std::wstring& s)
{
    std::vector<Entry> e;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(L';', i); if (j == std::wstring::npos) j = s.size();
        const std::wstring part = s.substr(i, j - i);
        const size_t c = part.find(L':');
        if (c != std::wstring::npos) {
            const double v = _wtof(part.c_str() + c + 1);
            const unsigned long sid = wcstoul(part.substr(0, c).c_str(), nullptr, 10);
            if (std::isfinite(v) && v > 0) e.push_back({ uint32_t(sid), v });
        }
        i = j + 1;
    }
    return e;
}
inline std::wstring Encode(const std::vector<Entry>& e)
{
    std::wstring s;
    for (const auto& x : e) s += std::format(L"{}{}:{:.4f}", s.empty() ? L"" : L";", x.session, x.v);
    return s;
}
inline bool Fresh(const Entry& e, uint32_t session) { return e.session + kStaleSessions > session; }   // newer (another process) too
// this session's value into the history: replaces this session's entry (periodic saves), else appended; stale entries
// and all but the newest kKeepSessions go
// (age false: the speed test's results never expire, only the newest kKeepSessions runs count; staleness only makes it due)
inline void Record(std::vector<Entry>& h, uint32_t session, double v, bool age = true)
{
    if (age) h.erase(std::remove_if(h.begin(), h.end(), [&](const Entry& e) { return !Fresh(e, session); }), h.end());
    auto it = std::find_if(h.begin(), h.end(), [&](const Entry& e) { return e.session == session; });
    if (it != h.end()) it->v = v; else h.push_back({ session, v });
    std::sort(h.begin(), h.end(), [](const Entry& a, const Entry& b) { return a.session < b.session; });
    while (h.size() > size_t(kKeepSessions)) h.erase(h.begin());
}
// the saved value: the lowest fresh entry of the last kKeepSessions; untrusted (false) when none is fresh, or when it is
// slower than the prior (neutral) and fewer than kKeepSessions sessions measured it (trustFirst: the speed test)
inline bool Resolve(const std::vector<Entry>& h, uint32_t session, double neutral, double& out, bool trustFirst = false, bool age = true)
{
    double m = 0; int n = 0;
    for (const auto& e : h) if (!age || Fresh(e, session)) { m = n ? std::min(m, e.v) : e.v; ++n; }
    if (!n) return false;
    if (!trustFirst && neutral > 0 && m > neutral && n < kKeepSessions) return false;
    out = m;
    return true;
}

// the learned values: name, the Costs member, its "learned" flag (nullptr: animeSrLearned, unknown = -1), the prior it
// is judged against, and the range LoadLearned accepted before
struct Slot { std::wstring name; double* v; bool* flag; double neutral; double lo, hi; };
inline std::vector<Slot> Slots(Costs& c)
{
    const Costs d{};
    std::vector<Slot> s;
    for (int i = 0; i < kFgKeys; ++i) s.push_back({ std::format(L"FgScale{}", i), &c.fgScale[i], &c.fgLearned[i], 1.0, 0.05, 20 });
    s.push_back({ L"VsrScale", &c.vsrScale, &c.learnedVsr, 1.0, 0.1, 10 });
    s.push_back({ L"IvsrScale", &c.ivsrScale, &c.learnedIvsr, 1.0, 0.1, 10 });
    s.push_back({ L"XessScale", &c.xessScale, &c.learnedXess, 1.0, 0.1, 10 });
    s.push_back({ L"NrMs", &c.nrMs, &c.learnedNr, d.nrMs, 0.05, 200 });
    s.push_back({ L"Hdr4kMs", &c.hdr4k, &c.learnedHdr, d.hdr4k, 0.05, 30 });
    s.push_back({ L"Deband1080Ms", &c.deband1080, &c.learnedDeband, d.deband1080, 0.001, 20 });
    s.push_back({ L"Base4kMs", &c.base4k, &c.learnedBase, d.base4k, 0.05, 20 });
    for (int i = 0; i < kAnimeSrModels; ++i)   // the prior per input Mpx on this card (AnimeSrCostMs without a learned value)
        s.push_back({ std::wstring(L"AnimeSr-") + AnimeSrModel(i).id, &c.animeSrLearned[i], nullptr, AnimeSrModel(i).msPerMpx * c.card, 0.01, 500 });
    return s;
}
inline std::wstring Get(const Values& v, const std::wstring& n) { auto it = v.find(n); return it == v.end() ? std::wstring() : it->second; }

// A new session: the counter goes up (the session number for this filter instance).
inline uint32_t BeginSession(Values& v)
{
    const uint32_t s = uint32_t(wcstoul(Get(v, L"Session").c_str(), nullptr, 10)) + 1;
    v[L"Session"] = std::to_wstring(s);
    return s;
}
// Values written under another model or for another GPU are dropped (the speed test included).
inline void Reset(Values& v, const std::wstring& gpu)
{
    if (Get(v, L"Gpu") == gpu && Get(v, L"Model") == kModel) return;
    const std::wstring sessions = Get(v, L"Session");
    v.clear();
    v[L"Gpu"] = gpu; v[L"Model"] = kModel;
    if (!sessions.empty()) v[L"Session"] = sessions;   // the count goes on (staleness stays meaningful)
}

// PushFlow's speed test (absolute ms at 4K per tier): history per value, the first run trusted
inline bool LoadBench(const Values& v, uint32_t session, Costs& c)
{
    bool bench = true, rebuild = true;
    for (int t = 0; t < 3; ++t) {
        double x = 0;
        if (Resolve(Decode(Get(v, std::format(L"PfFirst{}.h", t))), session, 0, x, true, false)) c.pfFirst[t] = x; else bench = false;
        if (Resolve(Decode(Get(v, std::format(L"PfNext{}.h", t))), session, 0, x, true, false)) c.pfNext[t] = x; else bench = false;
        if (Resolve(Decode(Get(v, std::format(L"PfRebuild{}.h", t))), session, 0, x, true, false)) c.pfRebuild[t] = x; else rebuild = false;
    }
    c.pfBenched = bench;
    c.pfRebuildBenched = bench && rebuild;
    return bench;
}
// newest speed test result older than kBenchStaleSessions (or none): run it again when PushFlow is picked
inline bool BenchStale(const Values& v, uint32_t session)
{
    uint32_t newest = 0; bool any = false;
    for (const auto& e : Decode(Get(v, L"PfFirst1.h"))) { newest = std::max(newest, e.session); any = true; }
    return !any || newest + kBenchStaleSessions <= session;
}
// one speed test run (medians; < 0 = not measured): into the histories, the plain values rewritten
inline void RecordBench(Values& v, uint32_t session, const double first[3], const double next[3], const double rebuild[3])
{
    for (int t = 0; t < 3; ++t) {
        const struct { const wchar_t* n; double x; } items[] = { { L"PfFirst", first[t] }, { L"PfNext", next[t] }, { L"PfRebuild", rebuild[t] } };
        for (const auto& it : items) {
            const std::wstring name = std::format(L"{}{}", it.n, t);
            auto h = Decode(Get(v, name + L".h"));
            if (it.x > 0) Record(h, session, it.x, false);
            v[name + L".h"] = Encode(h);
            double r = 0;
            if (Resolve(h, session, 0, r, true, false)) v[name] = std::format(L"{:.3f}", r); else v.erase(name);
        }
    }
    v[L"PfBenchVersion"] = L"2";
}

// The saved values into Costs at the start of a session (after the card factor is known: the anime priors use it).
// Returns the number of trusted values.
inline int LoadCosts(const Values& v, uint32_t session, Costs& c)
{
    int n = 0;
    for (auto& s : Slots(c)) {
        double x = 0;
        if (!Resolve(Decode(Get(v, s.name + L".h")), session, s.neutral, x) || x < s.lo || x > s.hi) continue;
        *s.v = x;
        if (s.flag) *s.flag = true;
        ++n;
    }
    return n;
}
// This session's measured values (name -> the session's latest value) into the histories; every plain value rewritten
// from its history (a stale or untrusted one removed, so the readers use the prior).
inline void SaveSession(Values& v, uint32_t session, const std::map<std::wstring, double>& measured, const Costs& c)
{
    Costs tmp = c;
    for (auto& s : Slots(tmp)) {
        auto h = Decode(Get(v, s.name + L".h"));
        if (auto it = measured.find(s.name); it != measured.end() && it->second >= s.lo && it->second <= s.hi) Record(h, session, it->second);
        else h.erase(std::remove_if(h.begin(), h.end(), [&](const Entry& e) { return !Fresh(e, session); }), h.end());
        if (h.empty()) v.erase(s.name + L".h"); else v[s.name + L".h"] = Encode(h);
        double x = 0;
        if (Resolve(h, session, s.neutral, x) && x >= s.lo && x <= s.hi) v[s.name] = std::format(L"{:.4f}", x); else v.erase(s.name);
    }
}

}  // namespace pm::learned
