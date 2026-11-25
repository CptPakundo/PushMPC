// SPDX-License-Identifier: GPL-3.0-or-later
// Auto for pmml: measure every candidate (runtime/EP x PushFlow tier x per-t / flow once) on the user's GPU at the
// real size, once per card + driver (cached), and pick the best quality whose cost fits the frame budget.
// The measurement of one candidate runs in its own process (pmml_cli probe): a driver or EP fault ends that process,
// not the player, and every ONNX Runtime flavor gets a process of its own. Same rules as the filter's Auto
// (PushMpcAuto.h) and the vsplugin's (autopick.h): a share of the budget, the largest multiplier that fits, quality first.
#pragma once
#include "pmml/pmml.h"

#include <map>
#include <string>
#include <vector>

namespace pmml {

struct Candidate {
    Backend backend;
    std::string tier = "balanced";   // fast | balanced | quality
    bool flowOnce = false;            // network once per pair, every timestep rebuilt from its grids
    std::string key() const;          // "ort:core:trt-rtx:0|quality|once"
    static bool parse(const std::string& key, Candidate* out);
};

struct ProbeResult {
    Candidate candidate;
    bool ok = false;
    std::string error;
    double initMs = 0, packMs = 0, netMs = 0, synthMs = 0;   // medians over the probe's pairs
    int netW = 0, netH = 0;
    std::string describe;
    // cost of one source frame at multiplier m: per t = pack + (m-1)(net + synth); flow once = pack + net + (m-1) synth
    double costMs(int multiplier) const;
};

// Quality rank of a candidate (higher is better): the tier first (quality > balanced > fast), then per t over flow
// once: on public clips at 4x flow once cost 0.3 to 1.6 dB against per t (one clip +0.1 dB), so it is ranked below
// per t of the same tier and used where it lets a higher multiplier fit.
int qualityRank(const Candidate& c);

struct Choice {
    bool ok = false;
    Candidate candidate;
    int multiplier = 1;
    double costMs = 0, budgetMs = 0;
    std::string why;
};
// The best candidate for `wantMultiplier` (stepping down to 2 when nothing fits), within share x the source frame time.
Choice choose(const std::vector<ProbeResult>& results, double sourceFps, int wantMultiplier, double share = 0.8);

// The candidates worth measuring for a device list (pmml::listDevices of each flavor).
std::vector<Candidate> candidatesFor(const std::vector<DeviceInfo>& devices);

// The cache: one entry per (device name, vendor/device id, driver, size), in <cacheDir>/pmml-autobench.json.
std::string cacheKey(const std::string& device, uint32_t vendorId, uint32_t deviceId, const std::string& driver, int w, int h);
bool cacheLoad(const std::string& file, const std::string& key, std::vector<ProbeResult>* out);
bool cacheStore(const std::string& file, const std::string& key, const std::vector<ProbeResult>& results);

std::string toJson(const ProbeResult& r);
bool fromJson(const std::string& json, ProbeResult* r);

}  // namespace pmml
