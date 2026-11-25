// SPDX-License-Identifier: GPL-3.0-or-later
// pmml_cli: pmml's command line.
//   list   [--runtime DIR] [--flavor F] [--json]                          devices of one ONNX Runtime flavor (+ ncnn)
//   run    --clip in.y4m [--frames N] [--mult 2|4] [--flow-once] ...     held-out frames of a clip: PSNR-Y per t /
//          flow once against the truth, blend and repeat; --dump DIR writes the output planes, --compare DIR checks
//          them against reference.py's dump (min PSNR per plane, --min-db)
//   probe  --size WxH [--pairs N] [--flow-once] ...                     one candidate on synthetic frames: medians (JSON)
//   auto   --size WxH --fps F --mult M [--cache DIR]                     every flavor x EP x tier in its own process,
//          the choice, the cache
// Common: --runtime DIR --models DIR --graphs DIR --backend ort:core:cpu:0 (or cpu, dml, trt-rtx, openvino, qnn,
//         qnn-gpu, coreml, migraphx, ncnn) --tier fast|balanced|quality --motion H --fp16 --host-intermediates
#include "pmml/autobench.h"
#include "pmml/pmml.h"
#include "y4m.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define popen _popen
#define pclose _pclose
#endif
#include <sys/stat.h>
#if !defined(_WIN32)
#include <dirent.h>
#endif

namespace {

struct Args {
    std::map<std::string, std::string> kv;
    std::vector<std::string> flags;
    bool has(const std::string& k) const { return kv.count(k) || std::find(flags.begin(), flags.end(), k) != flags.end(); }
    std::string get(const std::string& k, const std::string& d = {}) const { auto it = kv.find(k); return it == kv.end() ? d : it->second; }
};

double psnr(const uint16_t* a, const uint16_t* b, size_t n, double peak) {
    double se = 0;
    for (size_t i = 0; i < n; ++i) { const double d = double(a[i]) - double(b[i]); se += d * d; }
    const double mse = se / double(n);
    return mse < 1e-10 ? 99.0 : 10.0 * std::log10(peak * peak / mse);
}

std::string exePath(const char* argv0) {
#ifdef _WIN32
    char buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameA(nullptr, buf, sizeof buf);
    return n ? std::string(buf, n) : argv0;
#else
    return argv0;
#endif
}

std::vector<std::string> flavors(const std::string& runtimeDir) {
    std::vector<std::string> v;
    for (const char* f : {"core", "directml", "migraphx"}) {
        struct stat st {};
        if (stat((runtimeDir + "/ort/" + f).c_str(), &st) == 0) v.push_back(f);
    }
    return v;
}

pmml::Options options(const Args& a, std::string* err) {
    pmml::Options o;
    o.runtimeDir = a.get("--runtime", "runtime");
    o.modelsDir = a.get("--models", o.runtimeDir + "/models");
    o.graphsDir = a.get("--graphs", o.runtimeDir + "/graphs");
    o.cacheDir = a.get("--cache", "");
    o.ncnnModelsDir = a.get("--ncnn-models", "");
    if (!pmml::Backend::parse(a.get("--backend", "cpu"), &o.backend)) *err = "bad --backend " + a.get("--backend");
    o.tier = a.get("--tier", "balanced");
    o.motionHeight = std::atoi(a.get("--motion", "0").c_str());
    o.fp16 = a.has("--fp16");
    o.hostIntermediates = a.has("--host-intermediates");
    o.syncTiming = a.has("--sync-timing");
    return o;
}

// a moving textured pattern (for probes: content does not change the cost)
void synthetic(int w, int h, int shift, std::vector<uint16_t>* y, std::vector<uint16_t>* u, std::vector<uint16_t>* v) {
    y->resize(size_t(w) * h); u->assign(size_t(w / 2) * (h / 2), 128); v->assign(size_t(w / 2) * (h / 2), 128);
    for (int r = 0; r < h; ++r)
        for (int c = 0; c < w; ++c) {
            const int x = c - shift;
            (*y)[size_t(r) * w + c] = uint16_t(40 + ((x * 7 + r * 3) & 127) + (((x >> 4) + (r >> 4)) & 1) * 60);
        }
}

int cmdList(const Args& a) {
    std::string log;
    const std::string rt = a.get("--runtime", "runtime");
    if (a.has("--flavor")) {
#ifdef _WIN32
        _putenv_s("PMML_FLAVOR", a.get("--flavor").c_str());
#else
        setenv("PMML_FLAVOR", a.get("--flavor").c_str(), 1);
#endif
    }
    auto devs = pmml::listDevices(rt, &log);
    std::fputs(log.c_str(), stderr);
    for (const auto& d : devs) {
        if (a.has("--json"))
            std::printf("PMML_DEVICE {\"backend\": \"%s\", \"name\": \"%s\", \"type\": \"%s\", \"vendorId\": %u, \"deviceId\": %u, \"driver\": \"%s\", \"note\": \"%s\"}\n",
                        d.backend.key().c_str(), d.name.c_str(), d.type.c_str(), d.vendorId, d.deviceId, d.driver.c_str(), d.note.c_str());
        else
            std::printf("%-28s %-4s %-40s ven %04x dev %04x driver %s %s\n", d.backend.key().c_str(), d.type.c_str(), d.name.c_str(), d.vendorId, d.deviceId, d.driver.c_str(), d.note.c_str());
    }
    return devs.empty() ? 1 : 0;
}

int cmdRun(const Args& a) {
    std::string err;
    pmml::Options o = options(a, &err);
    if (!err.empty()) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
    Y4m clip;
    if (!clip.read(a.get("--clip"), std::atoi(a.get("--frames", "9").c_str()), &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
    o.width = clip.w; o.height = clip.h;
    o.colour = pmml::Colour::limited(clip.bits);
    auto ip = pmml::Interpolator::create(o, &err);
    if (!ip) { std::fprintf(stderr, "create: %s\n", err.c_str()); return 3; }
    std::printf("%s\n", ip->describe().c_str());
    const int mult = std::atoi(a.get("--mult", "4").c_str());
    const bool once = a.has("--flow-once");
    const double peak = double((1 << clip.bits) - 1);
    const std::string dump = a.get("--dump"), compare = a.get("--compare");
    const double minDb = std::atof(a.get("--min-db", "40").c_str());
    std::vector<double> got, blend, repeat, cmp;
    std::vector<uint16_t> oy(size_t(clip.w) * clip.h), ou(oy.size() / 4), ov(oy.size() / 4);
    double ms = 0;
    int outputs = 0;
    for (size_t k = 0; k + mult < clip.frames.size(); k += size_t(mult)) {
        const auto& f0 = clip.frames[k];
        const auto& f1 = clip.frames[k + size_t(mult)];
        pmml::Frame A, B;
        A.plane[0] = f0.y.data(); A.plane[1] = f0.u.data(); A.plane[2] = f0.v.data();
        B.plane[0] = f1.y.data(); B.plane[1] = f1.u.data(); B.plane[2] = f1.v.data();
        A.stride[0] = B.stride[0] = clip.w; A.stride[1] = A.stride[2] = B.stride[1] = B.stride[2] = clip.w / 2;
        if (!ip->setPair(A, B, once ? mult - 1 : 1, &err)) { std::fprintf(stderr, "setPair: %s\n", err.c_str()); return 4; }
        for (int j = 1; j < mult; ++j) {
            const float t = float(j) / float(mult);
            pmml::OutFrame O;
            O.plane[0] = oy.data(); O.plane[1] = ou.data(); O.plane[2] = ov.data();
            O.stride[0] = clip.w; O.stride[1] = O.stride[2] = clip.w / 2;
            const auto s0 = ip->stats();
            if (!ip->render(t, O, &err)) { std::fprintf(stderr, "render: %s\n", err.c_str()); return 5; }
            const auto s1 = ip->stats();
            ms += s1.synthMs + (s1.networkRuns != s0.networkRuns ? s1.netMs : 0);
            ++outputs;
            const auto& truth = clip.frames[k + size_t(j)].y;
            got.push_back(psnr(oy.data(), truth.data(), oy.size(), peak));
            std::vector<uint16_t> bl(oy.size());
            for (size_t i = 0; i < bl.size(); ++i) bl[i] = uint16_t(std::lround(f0.y[i] * (1 - t) + f1.y[i] * t));
            blend.push_back(psnr(bl.data(), truth.data(), bl.size(), peak));
            repeat.push_back(psnr((t < 0.5f ? f0.y : f1.y).data(), truth.data(), oy.size(), peak));
            char name[128];
            const char* kind = once && mult > 2 ? "once" : "per_t";
            for (int p = 0; p < 3; ++p) {
                std::snprintf(name, sizeof name, "/f%03d_t%d_%s_%c.u16", int(k), j, kind, "yuv"[p]);
                const std::vector<uint16_t>& plane = p == 0 ? oy : p == 1 ? ou : ov;
                if (!dump.empty()) { FILE* f = std::fopen((dump + name).c_str(), "wb"); if (f) { std::fwrite(plane.data(), 2, plane.size(), f); std::fclose(f); } }
                if (!compare.empty()) {
                    FILE* f = std::fopen((compare + name).c_str(), "rb");
                    if (!f) { std::fprintf(stderr, "no reference %s\n", (compare + name).c_str()); return 6; }
                    std::vector<uint16_t> ref(plane.size());
                    const size_t n = std::fread(ref.data(), 2, ref.size(), f);
                    std::fclose(f);
                    if (n != ref.size()) { std::fprintf(stderr, "short reference %s\n", name); return 6; }
                    cmp.push_back(psnr(plane.data(), ref.data(), ref.size(), peak));
                }
            }
        }
    }
    auto mean = [](const std::vector<double>& v) { double s = 0; for (double x : v) s += x; return v.empty() ? 0 : s / double(v.size()); };
    const auto st = ip->stats();
    std::printf("PMML_RUN {\"backend\": \"%s\", \"tier\": \"%s\", \"mult\": %d, \"flowOnce\": %d, \"psnr_y\": %.2f, \"blend_y\": %.2f, \"repeat_y\": %.2f, "
                "\"vs_reference_min\": %.2f, \"outputs\": %d, \"ms_per_output\": %.2f, \"pack_ms\": %.2f, \"net_ms\": %.2f, \"synth_ms\": %.2f, \"network\": \"%dx%d\"}\n",
                o.backend.key().c_str(), o.tier.c_str(), mult, once ? 1 : 0, mean(got), mean(blend), mean(repeat),
                cmp.empty() ? 0.0 : *std::min_element(cmp.begin(), cmp.end()), outputs, outputs ? ms / outputs : 0.0, st.packMs, st.netMs, st.synthMs, st.netW, st.netH);
    if (!cmp.empty() && *std::min_element(cmp.begin(), cmp.end()) < minDb) { std::fprintf(stderr, "FAIL: below %.1f dB against the reference\n", minDb); return 7; }
    if (mean(got) <= mean(repeat)) { std::fprintf(stderr, "FAIL: not better than repeating the nearer frame\n"); return 8; }   // a blend can win on slow, finely textured content
    return 0;
}

double median(std::vector<double> v) { if (v.empty()) return 0; std::sort(v.begin(), v.end()); return v[v.size() / 2]; }

int cmdProbe(const Args& a) {
    std::string err;
    pmml::Options o = options(a, &err);
    pmml::ProbeResult r;
    r.candidate.backend = o.backend;
    r.candidate.tier = o.tier;
    r.candidate.flowOnce = a.has("--flow-once");
    const std::string size = a.get("--size", "1920x1080");
    o.width = std::atoi(size.c_str());
    o.height = std::atoi(size.substr(size.find('x') + 1).c_str());
    const int pairs = std::max(3, std::atoi(a.get("--pairs", "8").c_str()));
    o.syncTiming = true;
    if (err.empty()) {
        auto t0 = std::chrono::steady_clock::now();
        auto ip = pmml::Interpolator::create(o, &err);
        r.initMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ip) {
            r.describe = ip->describe();
            std::vector<uint16_t> y0, u0, v0, y1, u1, v1, oy(size_t(o.width) * o.height), ou(oy.size() / 4), ov(oy.size() / 4);
            synthetic(o.width, o.height, 0, &y0, &u0, &v0);
            synthetic(o.width, o.height, 24, &y1, &u1, &v1);
            pmml::Frame A, B;
            A.plane[0] = y0.data(); A.plane[1] = u0.data(); A.plane[2] = v0.data();
            B.plane[0] = y1.data(); B.plane[1] = u1.data(); B.plane[2] = v1.data();
            A.stride[0] = B.stride[0] = o.width; A.stride[1] = A.stride[2] = B.stride[1] = B.stride[2] = o.width / 2;
            pmml::OutFrame O;
            O.plane[0] = oy.data(); O.plane[1] = ou.data(); O.plane[2] = ov.data();
            O.stride[0] = o.width; O.stride[1] = O.stride[2] = o.width / 2;
            std::vector<double> pack, net, syn;
            bool ok = true;
            for (int i = 0; i < pairs + 2 && ok; ++i) {   // two warm-up pairs (EP planning, JIT, clocks)
                ok = ip->setPair(i & 1 ? B : A, i & 1 ? A : B, r.candidate.flowOnce ? 3 : 1, &err);
                for (int j = 0; ok && j < (r.candidate.flowOnce ? 3 : 1); ++j) ok = ip->render(0.25f * float(j + 1), O, &err);
                if (ok && i >= 2) { const auto s = ip->stats(); pack.push_back(s.packMs); net.push_back(s.netMs); syn.push_back(s.synthMs); }
            }
            const auto s = ip->stats();
            r.ok = ok;
            r.packMs = median(pack); r.netMs = median(net); r.synthMs = median(syn);
            r.netW = s.netW; r.netH = s.netH;
        }
    }
    r.error = err;
    std::printf("PMML_PROBE %s\n", pmml::toJson(r).c_str());
    return r.ok ? 0 : 1;
}

// raw: pmml::RawSession (pushcore's hook) on a synthetic packed tensor, against the same model on the CPU EP of the
// same ONNX Runtime flavor; prints PSNR per output (0..1 picture, grids, mask)
int cmdRaw(const Args& a) {
    std::string err;
    pmml::Options o = options(a, &err);
    const std::string model = a.get("--model");
    const std::string size = a.get("--size", "640x384");
    const int w = std::atoi(size.c_str()), h = std::atoi(size.substr(size.find('x') + 1).c_str());
    const std::vector<std::pair<std::string, int64_t>> dims = {{"batch", 1}, {"height", h}, {"width", w}};
    std::vector<float> in(size_t(7) * w * h);
    for (int c = 0; c < 6; ++c)
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const int sx = x - (c >= 3 ? 12 : 0);
                in[(size_t(c) * h + y) * w + x] = 0.3f + 0.2f * std::sin(sx / 7.0f) * std::cos(y / 9.0f) + ((sx > 100 && sx < 160 && y > 80 && y < 140) ? 0.4f : 0.f);
            }
    std::fill(in.begin() + size_t(6) * w * h, in.end(), 0.5f);
    const size_t plane = size_t(w) * h;
    const size_t counts[4] = {plane * 3, plane * 2, plane * 2, plane};
    auto runOne = [&](const pmml::Backend& b, std::vector<std::vector<float>>& outs) -> bool {
        auto s = pmml::RawSession::create(o.runtimeDir, b, model, dims, o.cacheDir, &err);
        if (!s) return false;
        std::printf("%s: %s, %zu outputs\n", b.key().c_str(), s->describe().c_str(), s->outputCount());
        outs.assign(4, {});
        std::vector<float*> ptr; std::vector<size_t> bytes;
        for (int i = 0; i < 4; ++i) { outs[size_t(i)].resize(counts[i]); ptr.push_back(outs[size_t(i)].data()); bytes.push_back(counts[i] * 4); }
        return s->run(in.data(), {1, 7, h, w}, ptr, bytes, &err);
    };
    std::vector<std::vector<float>> got, ref;
    if (!runOne(o.backend, got)) { std::fprintf(stderr, "raw: %s\n", err.c_str()); return 3; }
    pmml::Backend cpu = o.backend; cpu.ep = "cpu"; cpu.device = 0;
    if (!runOne(cpu, ref)) { std::fprintf(stderr, "raw (cpu): %s\n", err.c_str()); return 3; }
    double worst = 99;
    for (int i = 0; i < 4; ++i) {
        double se = 0;
        for (size_t k = 0; k < counts[i]; ++k) { const double d = got[size_t(i)][k] - ref[size_t(i)][k]; se += d * d; }
        const double mse = se / double(counts[i]);
        const double db = mse < 1e-14 ? 99.0 : 10 * std::log10((i == 1 || i == 2 ? 4.0 : 1.0) / mse);
        worst = std::min(worst, db);
        std::printf("PMML_RAW output %d: %.2f dB against the CPU EP\n", i, db);
    }
    const double minDb = std::atof(a.get("--min-db", "40").c_str());
    return worst >= minDb ? 0 : 7;
}

std::vector<std::string> runLines(const std::string& cmd, const char* prefix) {
    std::vector<std::string> out;
#ifdef _WIN32
    FILE* p = popen(("\"" + cmd + "\"").c_str(), "r");   // cmd.exe /c strips the outer quotes
#else
    FILE* p = popen(cmd.c_str(), "r");
#endif
    if (!p) return out;
    char buf[8192];
    while (std::fgets(buf, sizeof buf, p)) {
        std::string l(buf);
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        if (l.rfind(prefix, 0) == 0) out.push_back(l.substr(std::strlen(prefix)));
    }
    pclose(p);
    return out;
}

std::string q(const std::string& s) { return "\"" + s + "\""; }

int cmdAuto(const Args& a, const char* argv0) {
    const std::string self = exePath(argv0);
    const std::string rt = a.get("--runtime", "runtime");
    const std::string size = a.get("--size", "1920x1080");
    const double fps = std::atof(a.get("--fps", "23.976").c_str());
    const int mult = std::atoi(a.get("--mult", "5").c_str());
    const std::string cacheDir = a.get("--cache", rt);
    const std::string common = " --runtime " + q(rt) + " --models " + q(a.get("--models", rt + "/models")) + " --graphs " + q(a.get("--graphs", rt + "/graphs")) +
                               (a.has("--ncnn-models") ? " --ncnn-models " + q(a.get("--ncnn-models")) : "") +
                               (cacheDir.empty() ? "" : " --cache " + q(cacheDir));
    std::vector<pmml::ProbeResult> results;
    std::string cacheKeyText;
    const std::string cacheFile = cacheDir + "/pmml-autobench.json";
    for (const auto& fl : flavors(rt)) {
        auto devLines = runLines(q(self) + " list --json --flavor " + fl + " --runtime " + q(rt) + " 2>&1", "PMML_DEVICE ");
        std::vector<pmml::DeviceInfo> devs;
        for (const auto& l : devLines) {
            pmml::DeviceInfo d;
            std::string key;
            const auto b = l.find("\"backend\": \"") + 12;
            key = l.substr(b, l.find('"', b) - b);
            if (!pmml::Backend::parse(key, &d.backend)) continue;
            const auto t = l.find("\"type\": \"") + 9;
            d.type = l.substr(t, l.find('"', t) - t);
            const auto n = l.find("\"name\": \"") + 9;
            d.name = l.substr(n, l.find('"', n) - n);
            const auto dr = l.find("\"driver\": \"") + 11;
            d.driver = l.substr(dr, l.find('"', dr) - dr);
            if (d.type != "CPU" && cacheKeyText.empty()) cacheKeyText = pmml::cacheKey(d.name, 0, 0, d.driver, std::atoi(size.c_str()), std::atoi(size.substr(size.find('x') + 1).c_str()));
            devs.push_back(d);
            std::printf("device  %s  %s  %s %s\n", key.c_str(), d.type.c_str(), d.name.c_str(), d.driver.c_str());
        }
        if (!a.has("--refresh") && !cacheKeyText.empty() && results.empty()) {
            std::vector<pmml::ProbeResult> cached;
            if (pmml::cacheLoad(cacheFile, cacheKeyText, &cached)) { std::printf("cache   %s (%zu results)\n", cacheKeyText.c_str(), cached.size()); results = cached; break; }
        }
        for (const auto& c : pmml::candidatesFor(devs)) {
            const std::string cmd = q(self) + " probe --backend " + c.backend.key() + " --tier " + c.tier + (c.flowOnce ? " --flow-once" : "") + " --size " + size +
                                    " --pairs " + a.get("--pairs", "6") + common + " 2>&1";
            auto lines = runLines(cmd, "PMML_PROBE ");
            pmml::ProbeResult r;
            r.candidate = c;
            if (lines.empty() || !pmml::fromJson(lines.back(), &r)) { r.ok = false; r.error = "the probe process ended without a result (driver or EP fault)"; }
            std::printf("probe   %-40s %s pack %.2f net %.2f synth %.2f ms  cost %dx %.2f ms %s\n", c.key().c_str(), r.ok ? "ok  " : "FAIL", r.packMs, r.netMs, r.synthMs,
                        mult, r.costMs(mult), r.ok ? "" : r.error.c_str());
            std::fflush(stdout);
            results.push_back(r);
        }
    }
    if (!cacheKeyText.empty() && !results.empty()) pmml::cacheStore(cacheFile, cacheKeyText, results);
    const auto choice = pmml::choose(results, fps, mult);
    std::printf("PMML_CHOICE {\"ok\": %d, \"candidate\": \"%s\", \"multiplier\": %d, \"costMs\": %.2f, \"budgetMs\": %.2f, \"why\": \"%s\"}\n", choice.ok ? 1 : 0,
                choice.candidate.key().c_str(), choice.multiplier, choice.costMs, choice.budgetMs, choice.why.c_str());
    return choice.ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "pmml_cli list|run|probe|auto [options] (pmml %s)\n", pmml::kVersion); return 2; }
    Args a;
    for (int i = 2; i < argc; ++i) {
        std::string k = argv[i];
        if (k.rfind("--", 0) != 0) continue;
        if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) a.kv[k] = argv[++i];
        else a.flags.push_back(k);
    }
    const std::string cmd = argv[1];
    if (cmd == "list") return cmdList(a);
    if (cmd == "run") return cmdRun(a);
    if (cmd == "probe") return cmdProbe(a);
    if (cmd == "raw") return cmdRaw(a);
    if (cmd == "auto") return cmdAuto(a, argv[0]);
    std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
    return 2;
}
