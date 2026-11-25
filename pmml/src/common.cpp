// SPDX-License-Identifier: GPL-3.0-or-later
#include "pmml/pmml.h"
#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace pmml {

Colour Colour::limited(int bits, float kr, float kb) {
    Colour c;
    c.kr = kr; c.kb = kb;
    const float mx = float((1u << bits) - 1u), sh = float(1u << (bits - 8));
    c.maxValue = mx;
    c.yOff = 16.f * sh / mx; c.yScale = 219.f * sh / mx;
    c.cOff = 128.f * sh / mx; c.cScale = 224.f * sh / mx;
    return c;
}

Colour Colour::full(int bits, float kr, float kb) {
    Colour c;
    c.kr = kr; c.kb = kb;
    c.maxValue = float((1u << bits) - 1u);
    c.yOff = 0.f; c.yScale = 1.f; c.cOff = 0.5f; c.cScale = 1.f;
    return c;
}

bool Backend::parse(const std::string& text, Backend* out) {
    Backend b;
    std::vector<std::string> parts;
    std::stringstream ss(text);
    for (std::string p; std::getline(ss, p, ':');) parts.push_back(p);
    if (parts.empty()) return false;
    if (parts.size() == 1) {
        const std::string& e = parts[0];
        if (e == "ncnn" || e == "vulkan") { b.runtime = "ncnn"; b.flavor = "ncnn"; b.ep = "vulkan"; }
        else if (e == "dml" || e == "directml") { b.flavor = "directml"; b.ep = "dml"; }
        else if (e == "migraphx") { b.flavor = "migraphx"; b.ep = "migraphx"; }
        else if (e == "cpu" || e == "trt-rtx" || e == "openvino" || e == "qnn" || e == "qnn-gpu" || e == "amdgpu" || e == "coreml") { b.ep = e; }
        else return false;
        *out = b;
        return true;
    }
    if (parts.size() < 3) return false;
    b.runtime = parts[0]; b.flavor = parts[1]; b.ep = parts[2];
    if (parts.size() > 3) b.device = std::atoi(parts[3].c_str());
    if (b.runtime != "ort" && b.runtime != "ncnn") return false;
    *out = b;
    return true;
}

std::string modelRole(const std::string& tier) { return tier == "quality" ? "quality" : "fast"; }

NetworkSize networkSize(int width, int height, const std::string& tier, int motionHeight, int pad) {
    // PushFlow: Fast = RIFE 4.25 lite at up to 960x540 (at most half size), Balanced = 4.25 lite up to 1280x720,
    // Quality = 4.26 up to 1920x1080; a picture is never enlarged. A chosen motion size replaces the tier's cap.
    int cap = tier == "fast" ? 540 : tier == "quality" ? 1080 : 720;
    double half = tier == "fast" ? 0.5 : 1.0;
    if (motionHeight > 0) { cap = motionHeight; half = 1.0; }
    const double scale = std::min({half, double(cap) / double(height), 1.0});
    NetworkSize n;
    n.h = std::max(32, int(std::lround(height * scale / 2.0)) * 2);
    n.w = std::max(32, int(std::lround(width * scale / 2.0)) * 2);
    pad = std::max(1, pad);
    n.padH = (n.h + pad - 1) / pad * pad;
    n.padW = (n.w + pad - 1) / pad * pad;
    return n;
}

std::array<float, 4> gridAffine(int width, int height, const NetworkSize& n) {
    // network-normalised grid (align_corners over the padded size) -> full-size-normalised grid:
    //   xm = (g + 1) / 2 * (p - 1);  xs = (xm + 0.5) * full / n - 0.5;  gf = 2 * xs / (full - 1) - 1
    auto axis = [](double full, double net, double p, float* a, float* b) {
        *a = float((p - 1) / 2 * full / net * 2 / (full - 1));
        *b = float((((p - 1) / 2 + 0.5) * full / net - 0.5) * 2 / (full - 1) - 1);
    };
    std::array<float, 4> r{};
    axis(width, n.w, n.padW, &r[0], &r[1]);
    axis(height, n.h, n.padH, &r[2], &r[3]);
    return r;
}

namespace detail {

std::string readText(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool fileExists(const std::string& path) { std::ifstream f(path, std::ios::binary); return bool(f); }

std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    const char last = a.back();
    return (last == '/' || last == '\\') ? a + b : a + "/" + b;
}

std::map<std::string, std::string> readFacts(const std::string& path) {
    std::map<std::string, std::string> m;
    std::stringstream ss(readText(path));
    for (std::string line; std::getline(ss, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const auto eq = line.find('=');
        if (eq != std::string::npos) m[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return m;
}

// The smallest JSON reader that pmml needs: string and number values by a dotted path, and string arrays.
static size_t skipWs(const std::string& s, size_t i) { while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) ++i; return i; }
static size_t skipValue(const std::string& s, size_t i);
static size_t skipString(const std::string& s, size_t i) { ++i; while (i < s.size() && s[i] != '"') { if (s[i] == '\\') ++i; ++i; } return i + 1; }
static size_t skipValue(const std::string& s, size_t i) {
    i = skipWs(s, i);
    if (i >= s.size()) return i;
    if (s[i] == '"') return skipString(s, i);
    if (s[i] == '{' || s[i] == '[') {
        const char close = s[i] == '{' ? '}' : ']';
        ++i;
        while (true) {
            i = skipWs(s, i);
            if (i >= s.size()) return i;
            if (s[i] == close) return i + 1;
            if (s[i] == ',') { ++i; continue; }
            i = skipValue(s, i);
            i = skipWs(s, i);
            if (i < s.size() && s[i] == ':') i = skipValue(s, i + 1);
        }
    }
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']') ++i;
    return i;
}
static std::string unquote(const std::string& s, size_t i, size_t end) {
    std::string out;
    for (size_t k = i + 1; k + 1 < end; ++k) {
        if (s[k] == '\\' && k + 2 < end) { ++k; out += s[k] == 'n' ? '\n' : s[k]; } else out += s[k];
    }
    return out;
}
// Finds the raw text of the value at `path` (keys separated by '.').
bool jsonRaw(const std::string& s, const std::string& path, std::string* raw) {
    size_t i = skipWs(s, 0);
    std::stringstream ps(path);
    for (std::string key; std::getline(ps, key, '.');) {
        if (i >= s.size() || s[i] != '{') return false;
        ++i;
        bool found = false;
        while (true) {
            i = skipWs(s, i);
            if (i >= s.size() || s[i] == '}') return false;
            if (s[i] == ',') { ++i; continue; }
            const size_t ke = skipString(s, i);
            const std::string k = unquote(s, i, ke);
            i = skipWs(s, ke);
            if (i >= s.size() || s[i] != ':') return false;
            i = skipWs(s, i + 1);
            if (k == key) { found = true; break; }
            i = skipValue(s, i);
        }
        if (!found) return false;
    }
    const size_t e = skipValue(s, i);
    *raw = s.substr(i, e - i);
    return true;
}
std::string jsonString(const std::string& s, const std::string& path, const std::string& def) {
    std::string raw;
    if (!jsonRaw(s, path, &raw) || raw.empty()) return def;
    if (raw[0] == '"') return unquote(raw, 0, raw.size());
    return raw;
}
double jsonNumber(const std::string& s, const std::string& path, double def) {
    std::string raw;
    if (!jsonRaw(s, path, &raw) || raw.empty() || raw[0] == '"' || raw[0] == '{' || raw[0] == '[') return def;
    return std::atof(raw.c_str());
}
std::vector<std::string> jsonStrings(const std::string& s, const std::string& path) {
    std::vector<std::string> out;
    std::string raw;
    if (!jsonRaw(s, path, &raw) || raw.empty() || raw[0] != '[') return out;
    size_t i = 1;
    while (i < raw.size()) {
        i = skipWs(raw, i);
        if (i >= raw.size() || raw[i] == ']') break;
        if (raw[i] == ',') { ++i; continue; }
        const size_t e = skipValue(raw, i);
        out.push_back(raw[i] == '"' ? unquote(raw, i, e) : raw.substr(i, e - i));
        i = e;
    }
    return out;
}
std::vector<std::string> jsonKeys(const std::string& s, const std::string& path) {
    std::vector<std::string> out;
    std::string raw = s;
    if (!path.empty() && !jsonRaw(s, path, &raw)) return out;
    size_t i = skipWs(raw, 0);
    if (i >= raw.size() || raw[i] != '{') return out;
    ++i;
    while (true) {
        i = skipWs(raw, i);
        if (i >= raw.size() || raw[i] == '}') break;
        if (raw[i] == ',') { ++i; continue; }
        const size_t ke = skipString(raw, i);
        out.push_back(unquote(raw, i, ke));
        i = skipWs(raw, ke);
        i = skipValue(raw, i + 1);
    }
    return out;
}

std::string jsonEscape(const std::string& v) {
    std::string o;
    for (char c : v) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
        else o += c;
    }
    return o;
}

}  // namespace detail
}  // namespace pmml
