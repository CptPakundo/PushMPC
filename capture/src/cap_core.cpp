// SPDX-License-Identifier: GPL-3.0-or-later
// PushMPC capture mode: the shared parts (cap_core.h).
#include "cap_core.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

namespace pmc {

double Now()
{
#ifdef __APPLE__
    static const double scale = [] { mach_timebase_info_data_t tb; mach_timebase_info(&tb); return double(tb.numer) / double(tb.denom) * 1e-9; }();
    return double(mach_absolute_time()) * scale;
#else
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

// ------------------------------------------------------------------------------------------------ frames
void I420::Resize(int width, int height)
{
    w = width; h = height;
    y.assign(size_t(w) * size_t(h), 16);
    u.assign(size_t(cw()) * size_t(ch()), 128);
    v.assign(size_t(cw()) * size_t(ch()), 128);
}

namespace {
inline void Rgb(const uint8_t* p, PixFmt f, int& r, int& g, int& b)
{
    if (f == PixFmt::BGRX) { b = p[0]; g = p[1]; r = p[2]; }
    else { r = p[0]; g = p[1]; b = p[2]; }
}
inline uint8_t Clip(int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }
// BT.709 limited range, 16-bit fixed point
constexpr int kYr = 11966, kYg = 40254, kYb = 4064;          // 0.1826 0.6142 0.0620
constexpr int kUr = -6596, kUg = -22189, kUb = 28785;        // -0.1006 -0.3386 0.4392
constexpr int kVr = 28785, kVg = -26145, kVb = -2640;        // 0.4392 -0.3989 -0.0403

void RowsToI420(const uint8_t* px, ptrdiff_t pitch, int w, int h, PixFmt f, I420& o, int cy0, int cy1)
{
    const int cw = o.cw();
    for (int cy = cy0; cy < cy1; ++cy) {
        for (int dy = 0; dy < 2; ++dy) {
            const int y = std::min(h - 1, cy * 2 + dy);
            if (cy * 2 + dy >= h) break;
            const uint8_t* row = px + ptrdiff_t(y) * pitch;
            uint8_t* yo = &o.y[size_t(y) * size_t(w)];
            for (int x = 0; x < w; ++x) {
                int r, g, b;
                Rgb(row + 4 * x, f, r, g, b);
                yo[x] = Clip(16 + ((kYr * r + kYg * g + kYb * b + 32768) >> 16));
            }
        }
        const uint8_t* r0 = px + ptrdiff_t(cy * 2) * pitch;
        const uint8_t* r1 = px + ptrdiff_t(std::min(h - 1, cy * 2 + 1)) * pitch;
        uint8_t* uo = &o.u[size_t(cy) * size_t(cw)];
        uint8_t* vo = &o.v[size_t(cy) * size_t(cw)];
        for (int cx = 0; cx < cw; ++cx) {
            const int x0 = cx * 2, x1 = std::min(w - 1, cx * 2 + 1);
            int sr = 0, sg = 0, sb = 0, r, g, b;
            Rgb(r0 + 4 * x0, f, r, g, b); sr += r; sg += g; sb += b;
            Rgb(r0 + 4 * x1, f, r, g, b); sr += r; sg += g; sb += b;
            Rgb(r1 + 4 * x0, f, r, g, b); sr += r; sg += g; sb += b;
            Rgb(r1 + 4 * x1, f, r, g, b); sr += r; sg += g; sb += b;
            // sums of four: the products are four times too large, so shift by 18 (+ rounding)
            uo[cx] = Clip(128 + ((kUr * sr + kUg * sg + kUb * sb + (1 << 17)) >> 18));
            vo[cx] = Clip(128 + ((kVr * sr + kVg * sg + kVb * sb + (1 << 17)) >> 18));
        }
    }
}
} // namespace

void ToI420(const uint8_t* px, ptrdiff_t pitch, int w, int h, PixFmt f, I420& out, int threads)
{
    if (out.w != w || out.h != h) out.Resize(w, h);
    const int ch = out.ch();
    if (threads <= 0) threads = int(std::min(8u, std::max(1u, std::thread::hardware_concurrency())));
    threads = std::max(1, std::min(threads, ch / 16));
    if (threads <= 1) { RowsToI420(px, pitch, w, h, f, out, 0, ch); return; }
    std::vector<std::thread> t;
    for (int i = 0; i < threads; ++i) {
        const int a = ch * i / threads, b = ch * (i + 1) / threads;
        t.emplace_back([=, &out] { RowsToI420(px, pitch, w, h, f, out, a, b); });
    }
    for (auto& x : t) x.join();
}

void FromI420(const I420& in, uint8_t* px, ptrdiff_t pitch, PixFmt f)
{
    const int cw = in.cw();
    for (int y = 0; y < in.h; ++y) {
        uint8_t* row = px + ptrdiff_t(y) * pitch;
        for (int x = 0; x < in.w; ++x) {
            const double Y = 1.16438 * (in.y[size_t(y) * size_t(in.w) + size_t(x)] - 16);
            const double U = in.u[size_t(y / 2) * size_t(cw) + size_t(x / 2)] - 128.0, V = in.v[size_t(y / 2) * size_t(cw) + size_t(x / 2)] - 128.0;
            const uint8_t r = Clip(int(std::lround(Y + 1.79274 * V))), g = Clip(int(std::lround(Y - 0.21325 * U - 0.53291 * V))), b = Clip(int(std::lround(Y + 2.11240 * U)));
            uint8_t* p = row + 4 * x;
            if (f == PixFmt::BGRX) { p[0] = b; p[1] = g; p[2] = r; } else { p[0] = r; p[1] = g; p[2] = b; }
            p[3] = 255;
        }
    }
}

uint64_t Fingerprint(const uint8_t* px, ptrdiff_t pitch, int w, int h)
{
    uint64_t hsh = 1469598103934665603ull;
    const int gx = std::min(w, 96), gy = std::min(h, 54);
    for (int j = 0; j < gy; ++j) {
        const uint8_t* row = px + ptrdiff_t((int64_t(j) * 2 + 1) * h / (2 * gy)) * pitch;
        for (int i = 0; i < gx; ++i) {
            uint32_t v;
            std::memcpy(&v, row + 4 * ((int64_t(i) * 2 + 1) * w / (2 * gx)), 4);
            hsh = (hsh ^ (v & 0xffffffu)) * 1099511628211ull;
        }
    }
    return hsh;
}

// ------------------------------------------------------------------------------------------------ the timeline
uint64_t Timeline::Add(double t, bool& discontinuity)
{
    discontinuity = false;
    uint64_t idx = 0;
    if (n_ == 0) {
        discontinuity = true;
        pts_.clear();
    } else {
        const double gap = t - lastTime_;
        if (gap > std::max(pauseSeconds, pauseIntervals * d_)) {
            // a pause or a seek: the line starts again at this frame (the interval is kept)
            idx = lastIdx_ + 1;
            pts_.clear();
            discontinuity = true;
        } else {
            const long step = n_ >= 3 ? std::max(1L, long(std::lround(gap / d_))) : 1L;
            idx = lastIdx_ + uint64_t(std::min(step, 8L));
        }
    }
    if (discontinuity) { segStart_ = idx; lastPos_ = -1e18; }
    pts_.emplace_back(double(idx), t);
    while (int(pts_.size()) > window) pts_.pop_front();
    lastIdx_ = idx; lastTime_ = t; ++n_;
    Fit();
    return idx;
}

void Timeline::Fit()
{
    const size_t n = pts_.size();
    if (n >= 3) {
        double mi = 0, mt = 0;
        for (auto& p : pts_) { mi += p.first; mt += p.second; }
        mi /= double(n); mt /= double(n);
        double c = 0, v = 0;
        for (auto& p : pts_) { c += (p.first - mi) * (p.second - mt); v += (p.first - mi) * (p.first - mi); }
        if (v > 0) d_ = std::clamp(c / v, minInterval, maxInterval);
        a_ = mt - d_ * mi;
    } else if (n == 2) {
        const double di = pts_[1].first - pts_[0].first;
        if (di > 0) d_ = std::clamp((pts_[1].second - pts_[0].second) / di, minInterval, maxInterval);
        a_ = pts_[1].second - d_ * pts_[1].first;
    } else if (n == 1) {
        a_ = pts_[0].second - d_ * pts_[0].first;
    }
    late_ = 0;
    for (auto& p : pts_) late_ = std::max(late_, p.second - (a_ + d_ * p.first));
    delay_ = d_ + late_ + margin;
}

double Timeline::Position(double T)
{
    // the fit moves a little with every frame; the shown position follows it through a gentle clock (it runs at the
    // fitted rate and takes a few percent of the error per refresh), so the steps stay even. A jump of more than a
    // frame (a restart, a rate change) is taken at once.
    const double target = (T - delay_ - a_) / d_;
    double p = target;
    if (lastPos_ > -1e17) {
        const double pred = lastPos_ + (T - lastT_) / d_;
        const double e = target - pred;
        p = std::fabs(e) > 1.0 ? target : pred + follow * e;
    }
    if (p < lastPos_) p = lastPos_;
    lastPos_ = p; lastT_ = T;
    return p;
}

// ------------------------------------------------------------------------------------------------ pacing
Pick Choose(double pos, int smoother, const std::vector<uint64_t>& frames, const std::vector<uint64_t>& discontinuities)
{
    Pick k;
    if (frames.empty()) return k;
    k.valid = true;
    size_t ia = 0;
    bool found = false;
    for (size_t i = 0; i < frames.size(); ++i) if (double(frames[i]) <= pos) { ia = i; found = true; }
    if (!found) { k.a = k.b = frames.front(); return k; }   // before the first frame: the first one
    k.a = k.b = frames[ia];
    if (ia + 1 >= frames.size()) return k;                  // the newest frame: held until the next arrives
    const uint64_t b = frames[ia + 1];
    if (std::find(discontinuities.begin(), discontinuities.end(), b) != discontinuities.end()) return k;   // no pair across a restart
    double t = (pos - double(k.a)) / double(b - k.a);
    t = std::clamp(t, 0.0, 0.999);
    if (smoother == 1) t = 0;
    else if (smoother >= 2) t = std::floor(t * smoother) / smoother;
    k.b = b;
    k.t = float(t);
    if (k.t <= 0.f) k.b = k.a;
    return k;
}

// ------------------------------------------------------------------------------------------------ copy protection
const char* const kProtectedMessage =
    "The picture of this video is black for screen capture: it is most likely copy-protected (streaming services and "
    "some apps hide protected video from capture). Capture mode has stepped aside so you still see the player's own "
    "picture. Protected video cannot be made smoother; play your own files or use a player PushMPC plugs into.";

bool Protected::Black(const uint8_t* px, ptrdiff_t pitch, int w, int h)
{
    if (!px || w <= 0 || h <= 0) return false;
    const int gx = std::min(w, 64), gy = std::min(h, 36);
    int black = 0;
    for (int j = 0; j < gy; ++j) {
        const uint8_t* row = px + ptrdiff_t((int64_t(j) * 2 + 1) * h / (2 * gy)) * pitch;
        for (int i = 0; i < gx; ++i) {
            const uint8_t* p = row + 4 * ((int64_t(i) * 2 + 1) * w / (2 * gx));
            if (p[0] <= 1 && p[1] <= 1 && p[2] <= 1) ++black;
        }
    }
    return black * 10 >= gx * gy * 9;
}

int Protected::Feed(const uint8_t* px, ptrdiff_t pitch, int w, int h, double t, bool& told)
{
    told = false;
    if (Black(px, pitch, w, h)) {
        if (since_ < 0) since_ = t;
        const double dt = t - since_;
        if (dt >= tellAfter && !told_) { told_ = true; told = true; }
        if (!hidden_ && dt >= hideAfter) { hidden_ = true; return 1; }
        return 0;
    }
    since_ = -1;
    if (hidden_) { hidden_ = false; return -1; }
    return 0;
}

// ------------------------------------------------------------------------------------------------ lip sync
const char* const kDelayBegin = "# >>> PushMPC capture mode: audio delay while capture mode runs (removed when it stops)";
const char* const kDelayEnd = "# <<< PushMPC capture mode";

std::string ClearAudioDelayText(const std::string& text)
{
    const size_t i = text.find(kDelayBegin);
    if (i == std::string::npos) return text;
    const size_t j = text.find(kDelayEnd, i);
    if (j == std::string::npos) return text;
    size_t end = j + std::strlen(kDelayEnd);
    while (end < text.size() && (text[end] == '\r' || text[end] == '\n')) ++end;
    return text.substr(0, i) + text.substr(end);
}

std::string SetAudioDelayText(const std::string& text, int ms)
{
    std::string t = ClearAudioDelayText(text);
    const std::string nl = t.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    if (!t.empty() && t.back() != '\n') t += nl;
    char v[64];
    std::snprintf(v, sizeof(v), "audio-delay=%+.3f", ms / 1000.0);
    return t + kDelayBegin + nl + v + nl + kDelayEnd + nl;
}

namespace {
bool ReadAll(const std::string& p, std::string& out, bool& exists)
{
    std::ifstream f(p, std::ios::binary);
    exists = bool(f);
    if (!f) { out.clear(); return true; }
    std::stringstream ss; ss << f.rdbuf();
    out = ss.str();
    return true;
}
bool WriteAll(const std::string& p, const std::string& text)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << text;
    return bool(f);
}
} // namespace

bool SetAudioDelay(const std::string& conf, int ms, std::string& why)
{
    std::string text; bool exists = false;
    ReadAll(conf, text, exists);
    if (!WriteAll(conf, SetAudioDelayText(text, ms))) { why = "cannot write " + conf; return false; }
    return true;
}

bool ClearAudioDelay(const std::string& conf)
{
    std::string text; bool exists = false;
    ReadAll(conf, text, exists);
    if (!exists) return true;
    const std::string n = ClearAudioDelayText(text);
    if (n == text) return true;
    if (n.find_first_not_of(" \t\r\n") == std::string::npos) return std::remove(conf.c_str()) == 0;
    return WriteAll(conf, n);
}

// ------------------------------------------------------------------------------------------------ options
const char* const kUsage =
    "pushmpc-capture: smoother (and upscaled) video for players PushMPC cannot plug into, through screen capture.\n"
    "  pushmpc-capture [--player <name> | --window <id> | --pick] [options]\n"
    "Which window:\n"
    "  --player <name>     the window of this player (process / app name, e.g. celluloid, firefox, \"QuickTime Player\")\n"
    "  --window <id>       an X11 window id or a macOS window number (see --list)\n"
    "  --pick              X11: click the player's window\n"
    "  --portal            Linux: pick the window in your desktop's own dialog (the default on Wayland)\n"
    "  --x11               Linux: X11 capture even when a portal is there\n"
    "  --monitor           Linux portal: a whole screen instead of one window\n"
    "  --restore-token <file>   Linux portal: where the remembered choice is kept (default: one per --player)\n"
    "  --list             list the windows capture mode can take, then exit\n"
    "Picture:\n"
    "  --smoother auto|off|2..8   frame generation (Auto: a frame of its own for every screen refresh)\n"
    "  --engine auto|gpu|cpu|ml|apple\n"
    "  --upscaler auto|nis|fsr|off\n"
    "  --fullscreen        show the player's picture on the whole screen (upscaled) instead of over its window\n"
    "  --display-hz <hz>   the screen's refresh rate when it cannot be read\n"
    "  --vulkan-device <name or number>\n"
    "Sound:\n"
    "  --audio-conf <mpv.conf>    keep the player's sound in step (an mpv-based player's config; undone on exit)\n"
    "  --audio-delay-ms <ms>      the delay to write there (default: measured)\n"
    "Tests: --no-present --frames <n> --seconds <s> --dump <out.y4m> --monitor --allow-cpu-vulkan --selftest --log <file> -v\n";

bool ParseOptions(int argc, char** argv, Options& o, std::string& why)
{
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&](std::string& out) -> bool {
            if (i + 1 >= argc) { why = a + " needs a value"; return false; }
            out = argv[++i];
            return true;
        };
        std::string v;
        if (a == "--window") { if (!val(o.window)) return false; }
        else if (a == "--player") { if (!val(o.player)) return false; }
        else if (a == "--pick") o.pick = true;
        else if (a == "--list") o.list = true;
        else if (a == "--portal") o.portal = true;
        else if (a == "--x11") o.x11 = true;
        else if (a == "--monitor") o.monitor = true;
        else if (a == "--engine") { if (!val(o.engine)) return false; }
        else if (a == "--smoother") {
            if (!val(v)) return false;
            std::string s = v;
            for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (s == "auto") o.smoother = 0;
            else if (s == "off" || s == "1" || s == "0") o.smoother = 1;
            else {
                const int k = std::atoi(s[0] == 'x' ? s.c_str() + 1 : s.c_str());
                if (k < 2 || k > 8) { why = "--smoother takes auto, off or 2..8"; return false; }
                o.smoother = k;
            }
        }
        else if (a == "--upscaler") { if (!val(o.upscaler)) return false; }
        else if (a == "--fullscreen") o.fullscreen = true;
        else if (a == "--display-hz") { if (!val(v)) return false; o.displayHz = std::atof(v.c_str()); }
        else if (a == "--no-present") o.present = false;
        else if (a == "--frames") { if (!val(v)) return false; o.frames = std::atol(v.c_str()); }
        else if (a == "--seconds") { if (!val(v)) return false; o.seconds = std::atof(v.c_str()); }
        else if (a == "--dump") { if (!val(o.dump)) return false; }
        else if (a == "--audio-conf") { if (!val(o.audioConf)) return false; }
        else if (a == "--audio-delay-ms") { if (!val(v)) return false; o.audioDelayMs = std::atoi(v.c_str()); }
        else if (a == "--vulkan-device") { if (!val(o.vulkanDevice)) return false; }
        else if (a == "--allow-cpu-vulkan") o.allowCpuVulkan = true;
        else if (a == "--log") { if (!val(o.log)) return false; }
        else if (a == "--restore-token") { if (!val(o.restoreToken)) return false; }
        else if (a == "--selftest") o.selftest = true;
        else if (a == "-v" || a == "--verbose") o.verbose = true;
        else if (a == "-h" || a == "--help") o.help = true;
        else { why = "unknown option " + a; return false; }
    }
    if (o.engine != "auto" && o.engine != "gpu" && o.engine != "cpu" && o.engine != "ml" && o.engine != "apple") { why = "--engine takes auto, gpu, cpu, ml or apple"; return false; }
    return true;
}

} // namespace pmc
