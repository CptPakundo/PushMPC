// SPDX-License-Identifier: GPL-3.0-or-later
#include "pmml/autobench.h"
#include "internal.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

namespace pmml {

using namespace detail;

std::string Candidate::key() const { return backend.key() + "|" + tier + "|" + (flowOnce ? "once" : "pert"); }

bool Candidate::parse(const std::string& key, Candidate* out) {
    const auto a = key.find('|'), b = key.rfind('|');
    if (a == std::string::npos || b == a) return false;
    Candidate c;
    if (!Backend::parse(key.substr(0, a), &c.backend)) return false;
    c.tier = key.substr(a + 1, b - a - 1);
    c.flowOnce = key.substr(b + 1) == "once";
    *out = c;
    return true;
}

double ProbeResult::costMs(int m) const {
    const int extra = std::max(0, m - 1);
    return candidate.flowOnce ? packMs + (extra > 0 ? netMs : 0) + extra * synthMs : packMs + extra * (netMs + synthMs);
}

int qualityRank(const Candidate& c) {
    const int tier = c.tier == "quality" ? 3 : c.tier == "balanced" ? 2 : 1;
    // O4 (qnn-split, the int8 network): just below the same tier and mode in floating point, so it is chosen only where
    // the float network does not fit (it is faster), never over it; its accuracy gate is in findModel
    return tier * 4 + (c.flowOnce ? 0 : 2) + (c.backend.ep == "qnn-split" ? 0 : 1);
}

Choice choose(const std::vector<ProbeResult>& results, double sourceFps, int wantMultiplier, double share) {
    Choice best;
    best.budgetMs = sourceFps > 0 ? share * 1000.0 / sourceFps : 0;
    for (int m = std::max(2, wantMultiplier); m >= 2; --m) {
        const ProbeResult* pick = nullptr;
        for (const auto& r : results) {
            if (!r.ok || (r.candidate.flowOnce && m <= 2)) continue;   // flow once only helps with 2+ timesteps per pair
            if (r.costMs(m) > best.budgetMs) continue;
            if (!pick || qualityRank(r.candidate) > qualityRank(pick->candidate) ||
                (qualityRank(r.candidate) == qualityRank(pick->candidate) && r.costMs(m) < pick->costMs(m))) pick = &r;
        }
        if (pick) {
            best.ok = true;
            best.candidate = pick->candidate;
            best.multiplier = m;
            best.costMs = pick->costMs(m);
            std::ostringstream ss;
            ss << pick->candidate.key() << " at " << m << "x: " << best.costMs << " ms of " << best.budgetMs << " ms per source frame";
            if (m < wantMultiplier) ss << " (" << wantMultiplier << "x does not fit)";
            best.why = ss.str();
            return best;
        }
    }
    best.why = "no candidate fits even 2x within " + std::to_string(best.budgetMs) + " ms";
    return best;
}

std::vector<Candidate> candidatesFor(const std::vector<DeviceInfo>& devices) {
    std::vector<Candidate> v;
    for (const auto& d : devices) {
        // the CPU only as a last resort, and only the Fast tier
        const bool cpu = d.backend.ep == "cpu" || d.type == "CPU";
        for (const char* tier : {"quality", "balanced", "fast"}) {
            if (cpu && std::string(tier) != "fast") continue;
            for (bool once : {false, true}) {
                Candidate c;
                c.backend = d.backend;
                c.tier = tier;
                c.flowOnce = once;
                v.push_back(c);
            }
        }
    }
    // O4 on Snapdragon: the int8 network on the NPU with the warp on the Adreno GPU, when the QNN EP offers both devices
    const auto has = [&](const char* ep) { return std::any_of(devices.begin(), devices.end(), [&](const DeviceInfo& d) { return d.backend.ep == ep; }); };
    if (has("qnn") && has("qnn-gpu")) {
        for (const char* tier : {"quality", "balanced", "fast"})
            for (bool once : {false, true}) {
                Candidate c;
                c.backend.ep = "qnn-split";
                c.tier = tier;
                c.flowOnce = once;
                v.push_back(c);
            }
    }
    return v;
}

static std::string sanitize(std::string s) {
    for (char& c : s) if (c == '.' || c == '"' || c == '\\') c = '_';
    return s;
}

std::string cacheKey(const std::string& device, uint32_t vendorId, uint32_t deviceId, const std::string& driver, int w, int h) {
    char ids[64];
    std::snprintf(ids, sizeof ids, "%04x-%04x", vendorId, deviceId);
    return sanitize(device + " " + ids + " drv " + driver + " " + std::to_string(w) + "x" + std::to_string(h));
}

std::string toJson(const ProbeResult& r) {
    std::ostringstream ss;
    ss << "{\"candidate\": \"" << jsonEscape(r.candidate.key()) << "\", \"ok\": " << (r.ok ? 1 : 0) << ", \"error\": \"" << jsonEscape(r.error)
       << "\", \"initMs\": " << r.initMs << ", \"packMs\": " << r.packMs << ", \"netMs\": " << r.netMs << ", \"synthMs\": " << r.synthMs
       << ", \"netW\": " << r.netW << ", \"netH\": " << r.netH << ", \"describe\": \"" << jsonEscape(r.describe) << "\"}";
    return ss.str();
}

bool fromJson(const std::string& j, ProbeResult* r) {
    ProbeResult p;
    if (!Candidate::parse(jsonString(j, "candidate"), &p.candidate)) return false;
    p.ok = jsonNumber(j, "ok") != 0;
    p.error = jsonString(j, "error");
    p.initMs = jsonNumber(j, "initMs"); p.packMs = jsonNumber(j, "packMs"); p.netMs = jsonNumber(j, "netMs"); p.synthMs = jsonNumber(j, "synthMs");
    p.netW = int(jsonNumber(j, "netW")); p.netH = int(jsonNumber(j, "netH"));
    p.describe = jsonString(j, "describe");
    *r = p;
    return true;
}

bool cacheLoad(const std::string& file, const std::string& key, std::vector<ProbeResult>* out) {
    const std::string all = readText(file);
    if (all.empty()) return false;
    std::string entry;
    if (!jsonRaw(all, "entries." + key, &entry)) return false;
    std::string results;
    if (!jsonRaw(entry, "results", &results)) return false;
    out->clear();
    for (const auto& k : jsonKeys(results, "")) {
        std::string raw;
        if (!jsonRaw(results, k, &raw)) continue;
        ProbeResult r;
        if (fromJson(raw, &r)) out->push_back(r);
    }
    return !out->empty();
}

bool cacheStore(const std::string& file, const std::string& key, const std::vector<ProbeResult>& results) {
    // keep the other entries (other cards, drivers, sizes)
    const std::string all = readText(file);
    std::ostringstream ss;
    ss << "{\n  \"schema\": 1,\n  \"entries\": {\n";
    bool first = true;
    for (const auto& k : jsonKeys(all, "entries")) {
        if (k == key) continue;
        std::string raw;
        if (!jsonRaw(all, "entries." + k, &raw)) continue;
        ss << (first ? "" : ",\n") << "    \"" << k << "\": " << raw;
        first = false;
    }
    ss << (first ? "" : ",\n") << "    \"" << key << "\": {\"time\": " << static_cast<long long>(std::time(nullptr)) << ", \"pmml\": \"" << kVersion << "\", \"results\": {\n";
    for (size_t i = 0; i < results.size(); ++i)
        ss << "      \"" << sanitize(results[i].candidate.key()) << "\": " << toJson(results[i]) << (i + 1 < results.size() ? ",\n" : "\n");
    ss << "    }}\n  }\n}\n";
    const std::string tmp = file + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << ss.str();
    }
    std::remove(file.c_str());
    return std::rename(tmp.c_str(), file.c_str()) == 0;
}

}  // namespace pmml
