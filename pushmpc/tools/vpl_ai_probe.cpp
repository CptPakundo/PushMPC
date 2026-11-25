// vpl_ai_probe: Intel VPL AI frame interpolation (VplFi) and AI super resolution (VplSr) probe.
//
// What it does
//  - lists the DXGI adapters and every VPL implementation the dispatcher finds (MFXEnumImplementations:
//    hardware/software, acceleration mode, API version, vendor, device, LUID-matched adapter name, the VPP
//    filter FourCCs it advertises, 'VAFI' = AI frame interpolation, 'VASR' = AI super resolution);
//  - per implementation: MFXVideoVPP_Query with mfxExtVPPAIFrameInterpolation / mfxExtVPPAISuperResolution
//    (video memory = the runtime's own D3D11 NV12 surfaces, then system memory), at 1080p and 4K;
//  - where Query succeeds it runs them:
//      FI 2x, held-out test: input = the even frames of a 60 fps clip (H.264 QP 6, 30 fps), each generated
//      frame is compared with the dropped real odd frame (PSNR-Y, SSIM-Y on the luma plane, limited range NV12);
//      SR 2x: input = the real frame downscaled 2x (area), output compared with the real frame;
//    ms per call (RunFrameVPPAsync + sync, surface upload/readback excluded);
//  - baselines that need no VPL: 50/50 blend and repeat for FI, bilinear and Lanczos for SR, plus a codec
//    calibration (decoded input vs truth);
//  - writes <out>\vpl_ai_probe.json and <out>\summary.txt.
// Exit code: 0 = the probe ran (supported or not), 2 = setup error (no ffmpeg, bad arguments), 3 = watchdog timeout
// (a VPL call hung; the JSON holds the stage).
//
// usage: vpl_ai_probe [--out DIR] [--ffmpeg EXE] [--sizes 1080,2160] [--seconds 3] [--clip60 FILE]
//                     [--fi 0|1] [--sr 0|1] [--sr4k 0|1] [--modes default,speed,quality] [--kfirst 4] [--timeout S]
//        vpl_ai_probe --score-harness DIR --truth TRUTH60 --size WxH [--kfirst 4] [--klast N] [--ffmpeg EXE] [--json FILE]
//          scores pushcore_gpu_harness PNGs (frameKKKKK-0-generated.png = truth 2K-1, frameKKKKK-1-real.png = truth 2K)
//          with exactly the same metric, so PushFlow / VFG numbers compare with the VPL ones.
// The synthetic clip (default): ffmpeg lavfi testsrc2 at 1.5x the size, a slow perspective zoom, a small rotation and a
// sinusoidal pan (up to ~25 px between input frames at 1080p), cropped to the size; deterministic.
// Built without the VPL dispatcher (no HAVE_VPL) it still runs the baselines and reports "unsupported".
#define NOMINMAX
#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#ifdef HAVE_VPL
#include <vpl/mfx.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------- tiny JSON
struct J {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false; double n = 0; std::string s;
    std::vector<J> a; std::vector<std::pair<std::string, J>> o;
    J() = default;
    J(bool v) : t(Bool), b(v) {}
    J(int v) : t(Num), n(v) {}
    J(unsigned v) : t(Num), n(v) {}
    J(int64_t v) : t(Num), n(double(v)) {}
    J(uint64_t v) : t(Num), n(double(v)) {}
    J(double v) : t(Num), n(v) {}
    J(const char* v) : t(Str), s(v) {}
    J(std::string v) : t(Str), s(std::move(v)) {}
    static J arr() { J j; j.t = Arr; return j; }
    static J obj() { J j; j.t = Obj; return j; }
    J& operator[](const std::string& k) {
        if (t != Obj) { t = Obj; o.clear(); }
        for (auto& p : o) if (p.first == k) return p.second;
        o.emplace_back(k, J()); return o.back().second;
    }
    void push(J v) { if (t != Arr) { t = Arr; a.clear(); } a.push_back(std::move(v)); }
    static void esc(std::ostream& os, const std::string& x) {
        os << '"';
        for (unsigned char c : x) {
            if (c == '"' || c == '\\') os << '\\' << c;
            else if (c < 0x20) os << std::format("\\u{:04x}", unsigned(c));
            else os << c;
        }
        os << '"';
    }
    void dump(std::ostream& os, int ind = 0) const {
        const std::string pad(size_t(ind + 2), ' '), pad0(size_t(ind), ' ');
        switch (t) {
        case Null: os << "null"; break;
        case Bool: os << (b ? "true" : "false"); break;
        case Num: if (!std::isfinite(n)) os << "null"; else if (n == std::floor(n) && std::fabs(n) < 1e15) os << int64_t(n); else os << std::format("{:.4f}", n); break;
        case Str: esc(os, s); break;
        case Arr:
            if (a.empty()) { os << "[]"; break; }
            os << "[\n";
            for (size_t i = 0; i < a.size(); ++i) { os << pad; a[i].dump(os, ind + 2); os << (i + 1 < a.size() ? ",\n" : "\n"); }
            os << pad0 << "]"; break;
        case Obj:
            if (o.empty()) { os << "{}"; break; }
            os << "{\n";
            for (size_t i = 0; i < o.size(); ++i) { os << pad; esc(os, o[i].first); os << ": "; o[i].second.dump(os, ind + 2); os << (i + 1 < o.size() ? ",\n" : "\n"); }
            os << pad0 << "}"; break;
        }
    }
};

// ---------------------------------------------------------------- globals: report, watchdog
std::mutex g_mx;
J g_report = J::obj();
fs::path g_outDir;
std::string g_stage = "start";
std::atomic<int64_t> g_deadlineMs{0};
std::vector<std::string> g_summary;

void flushReport() {
    std::lock_guard<std::mutex> l(g_mx);
    std::ofstream f(g_outDir / "vpl_ai_probe.json", std::ios::binary);
    g_report.dump(f); f << "\n";
}
void stage(const std::string& s) {
    { std::lock_guard<std::mutex> l(g_mx); g_stage = s; }
    if (s.find(" frame ") == std::string::npos) std::cout << "[stage] " << s << std::endl;   // per-frame stages only feed the watchdog
}
void say(const std::string& s) { std::cout << s << std::endl; std::lock_guard<std::mutex> l(g_mx); g_summary.push_back(s); }
int64_t nowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count(); }

void startWatchdog(int seconds) {
    g_deadlineMs = nowMs() + int64_t(seconds) * 1000;
    std::thread([] {
        for (;;) {
            Sleep(500);
            if (nowMs() > g_deadlineMs.load()) {
                std::string st;
                { std::lock_guard<std::mutex> l(g_mx); st = g_stage; g_report["status"] = "timeout"; g_report["timeoutStage"] = st; }
                flushReport();
                std::cout << "TIMEOUT in stage '" << st << "' (a VPL call did not return; see the JSON)" << std::endl;
                std::fflush(stdout);
                TerminateProcess(GetCurrentProcess(), 3);
            }
        }
    }).detach();
}

std::string utf8(const std::wstring& s) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string r(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n, nullptr, nullptr);
    return r;
}
std::wstring wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring r(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n);
    return r;
}

// ---------------------------------------------------------------- child processes (ffmpeg)
std::string g_ffmpeg = "ffmpeg";
std::string q(const fs::path& p) { return "\"" + utf8(p.wstring()) + "\""; }

int runWait(const std::string& cmd, std::string* errOut = nullptr) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    CreatePipe(&rd, &wr, &sa, 0);
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE); si.hStdOutput = wr; si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::wstring c = wide(cmd);
    if (!CreateProcessW(nullptr, c.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr); if (errOut) *errOut = "CreateProcess failed " + std::to_string(GetLastError()); return -1;
    }
    CloseHandle(wr);
    std::string out; char buf[4096]; DWORD got = 0;
    while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got) out.append(buf, got);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    if (errOut) *errOut = out;
    return int(code);
}

// Reads raw NV12 frames from an ffmpeg child's stdout.
struct FrameReader {
    HANDLE rd = nullptr; PROCESS_INFORMATION pi{}; int w = 0, h = 0; int64_t index = -1; bool ok = false;
    std::vector<uint8_t> frame;   // NV12, w*h*3/2
    FrameReader(const std::string& args, int w_, int h_) : w(w_), h(h_) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE wr = nullptr;
        CreatePipe(&rd, &wr, &sa, 1 << 20);
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE); si.hStdOutput = wr; si.hStdError = nul;
        std::wstring c = wide("\"" + g_ffmpeg + "\" -v error -nostdin " + args + " -f rawvideo -pix_fmt nv12 -");
        ok = CreateProcessW(nullptr, c.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != FALSE;
        CloseHandle(wr); CloseHandle(nul);
        frame.resize(size_t(w) * h * 3 / 2);
    }
    ~FrameReader() {
        if (rd) CloseHandle(rd);
        if (pi.hProcess) { if (WaitForSingleObject(pi.hProcess, 2000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
    }
    bool next() {
        if (!ok) return false;
        size_t got = 0;
        while (got < frame.size()) {
            DWORD n = 0;
            if (!ReadFile(rd, frame.data() + got, DWORD(std::min<size_t>(frame.size() - got, 1 << 24)), &n, nullptr) || n == 0) { ok = false; return false; }
            got += n;
        }
        ++index; return true;
    }
    bool seek(int64_t i) { while (index < i) if (!next()) return false; return index == i; }
    const uint8_t* y() const { return frame.data(); }
};

// ---------------------------------------------------------------- metrics (luma plane)
double psnrY(const uint8_t* a, const uint8_t* b, int w, int h) {
    uint64_t se = 0;
    for (size_t i = 0, n = size_t(w) * h; i < n; ++i) { const int d = int(a[i]) - int(b[i]); se += uint64_t(d * d); }
    if (se == 0) return 100.0;
    const double mse = double(se) / (double(w) * h);
    return 10.0 * std::log10(255.0 * 255.0 / mse);
}
// SSIM over 8x8 windows at a 4-pixel stride (the x264/FFmpeg 'ssim' window layout), unweighted.
double ssimY(const uint8_t* a, const uint8_t* b, int w, int h) {
    const double c1 = (0.01 * 255) * (0.01 * 255), c2 = (0.03 * 255) * (0.03 * 255);
    double acc = 0; uint64_t cnt = 0;
    for (int y = 0; y + 8 <= h; y += 4)
        for (int x = 0; x + 8 <= w; x += 4) {
            uint32_t sa = 0, sb = 0; uint64_t saa = 0, sbb = 0, sab = 0;
            for (int j = 0; j < 8; ++j) {
                const uint8_t* pa = a + size_t(y + j) * w + x; const uint8_t* pb = b + size_t(y + j) * w + x;
                for (int i = 0; i < 8; ++i) { const uint32_t va = pa[i], vb = pb[i]; sa += va; sb += vb; saa += va * va; sbb += vb * vb; sab += va * vb; }
            }
            const double ma = sa / 64.0, mb = sb / 64.0;
            const double va = saa / 64.0 - ma * ma, vb = sbb / 64.0 - mb * mb, cov = sab / 64.0 - ma * mb;
            acc += ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2));
            ++cnt;
        }
    return cnt ? acc / double(cnt) : 0;
}

struct Stat {
    std::vector<double> v;
    void add(double x) { v.push_back(x); }
    double mean() const { double s = 0; for (double x : v) s += x; return v.empty() ? NAN : s / double(v.size()); }
    double pct(double p) const {
        if (v.empty()) return NAN;
        auto s = v; std::sort(s.begin(), s.end());
        return s[std::min(s.size() - 1, size_t(std::max(0.0, std::ceil(p * double(s.size())) - 1)))];
    }
    double minv() const { return v.empty() ? NAN : *std::min_element(v.begin(), v.end()); }
};
J scoreJson(const Stat& p, const Stat& s) {
    J j = J::obj();
    j["n"] = uint64_t(p.v.size());
    j["psnrY"] = p.mean(); j["psnrYp10"] = p.pct(0.10); j["psnrYmin"] = p.minv();
    j["ssimY"] = s.mean();
    return j;
}
J msJson(const Stat& t) {
    J j = J::obj(); j["n"] = uint64_t(t.v.size()); j["median"] = t.pct(0.5); j["p95"] = t.pct(0.95); j["mean"] = t.mean(); return j;
}
std::string fmt2(double x) { return std::isfinite(x) ? std::format("{:.2f}", x) : "n/a"; }

// ---------------------------------------------------------------- test material
struct Clip { int w = 0, h = 0, inputFrames = 0; fs::path truth60, input30; std::string label; };

bool probeSize(const fs::path& f, int& w, int& h, int& frames) {
    std::string out;
    // size from the stream line, frame count from a stream copy's final stats
    runWait("\"" + g_ffmpeg + "\" -hide_banner -nostdin -i " + q(f) + " -map 0:v:0 -c copy -f null -", &out);
    const auto p = out.find("Video:");
    if (p == std::string::npos) return false;
    for (size_t i = p; i + 3 < out.size() && i < p + 400; ++i)
        if (isdigit(uint8_t(out[i])) && out.find('x', i) != std::string::npos) {
            int a = 0, b = 0;
            if (sscanf_s(out.c_str() + i, "%dx%d", &a, &b) == 2 && a >= 64 && b >= 64 && a < 20000 && b < 20000) { w = a; h = b; break; }
        }
    const auto fpos = out.rfind("frame=");
    frames = fpos != std::string::npos ? atoi(out.c_str() + fpos + 6) : 0;
    return w > 0 && h > 0;
}

std::string synthFilter(int w, int h, double seconds) {
    // background: one Mandelbrot frame (fine multi-scale texture) on a 1.5x canvas, held; perspective: zoom by moving the
    // source corners inward (period 4 s, up to 12 %); rotate: +-0.05 rad (period 5 s); crop: pan of +-0.24 W (period 4 s)
    // and +-H/6 (period 3.3 s). Foreground: a W/4 testsrc2 window (its own animation and text) moving on its own path, so
    // there is occlusion and disocclusion. Every 60 fps frame is rendered, so the dropped odd frames are real truth.
    const std::string z = "0.06*(1-cos(2*PI*in/240))";
    const int frames = int(std::lround(seconds * 60));
    std::string f = std::format("mandelbrot=s={}x{}:start_scale=0.12:maxiter=1500:rate=60:outer=normalized_iteration_count,trim=end_frame=1,"
                                "loop=loop=-1:size=1,setpts=N/(60*TB),format=rgb24,", w * 3 / 2, h * 3 / 2);
    f += std::format("perspective=x0='W*{0}':y0='H*{0}':x1='W-W*{0}':y1='H*{0}':x2='W*{0}':y2='H-H*{0}':x3='W-W*{0}':y3='H-H*{0}':interpolation=cubic:eval=frame,", z);
    f += "rotate=a='0.05*sin(2*PI*t/5)':c=black,";
    f += std::format("crop=w={0}:h={1}:x='(iw-ow)/2+{2}*sin(2*PI*t/4)':y='(ih-oh)/2+{3}*sin(2*PI*t/3.3)'[bg];", w, h, int(w * 0.24), h / 6);
    f += std::format("testsrc2=s={}x{}:r=60,trim=end_frame={},format=rgb24[fg];", w / 4, h / 4, frames);
    f += std::format("[bg][fg]overlay=x='{}+{}*sin(2*PI*t/2.5)':y='{}+{}*cos(2*PI*t/3)':shortest=1,", (w - w / 4) / 2, int(w * 0.31), (h - h / 4) / 2, int(h * 0.28));
    f += "scale=out_color_matrix=bt709:out_range=tv,format=yuv420p";
    return f;
}

bool makeClip(Clip& c, const fs::path& dir, int h, double seconds, const fs::path& clip60, J& info) {
    fs::create_directories(dir);
    c.truth60 = dir / "truth60.mkv"; c.input30 = dir / "input30.mp4";
    std::string log;
    if (clip60.empty()) {
        c.h = h; c.w = (h * 16 / 9 + 15) / 16 * 16; if (h == 1080) c.w = 1920; if (h == 2160) c.w = 3840;
        c.label = std::format("synthetic {}p", h);
        if (!fs::exists(c.truth60)) {
            stage("generate " + c.label);
            const std::string cmd = "\"" + g_ffmpeg + "\" -v error -nostdin -y -f lavfi -i \"" + synthFilter(c.w, c.h, seconds) +
                                    "\" -c:v ffv1 -level 3 -color_primaries bt709 -color_trc bt709 -colorspace bt709 -color_range tv " + q(c.truth60);
            if (runWait(cmd, &log) != 0) { info["error"] = "ffmpeg synthetic generation failed: " + log.substr(0, 600); return false; }
        }
        info["generator"] = synthFilter(c.w, c.h, seconds);
    } else {
        int fr = 0;
        c.truth60 = clip60;
        if (!probeSize(clip60, c.w, c.h, fr)) { info["error"] = "cannot read --clip60"; return false; }
        c.label = std::format("clip {}x{}", c.w, c.h);
    }
    if (!fs::exists(c.input30)) {
        stage("encode input30 " + c.label);
        const std::string cmd = "\"" + g_ffmpeg + "\" -v error -nostdin -y -i " + q(c.truth60) +
                                " -vf \"select='not(mod(n\\,2))',setpts=N/(30*TB)\" -r 30 -c:v libx264 -preset medium -qp 6 -pix_fmt yuv420p "
                                "-color_primaries bt709 -color_trc bt709 -colorspace bt709 -color_range tv " + q(c.input30);
        if (runWait(cmd, &log) != 0) { info["error"] = "ffmpeg input30 encode failed: " + log.substr(0, 600); return false; }
    }
    // count decoded input frames
    FrameReader r("-i " + q(c.input30), c.w, c.h);
    while (r.next()) ++c.inputFrames;
    info["label"] = c.label; info["width"] = c.w; info["height"] = c.h; info["inputFrames30"] = c.inputFrames;
    info["truth60"] = c.truth60.filename().string(); info["input30"] = "x264 QP 6, even frames of truth60";
    return c.inputFrames >= 8;
}

// FI baselines: blend 50/50 and repeat of the decoded input vs truth 2k-1; calibration: decoded input k vs truth 2k.
J fiBaselines(const Clip& c, int kFirst) {
    stage("FI baselines " + c.label);
    FrameReader in("-i " + q(c.input30), c.w, c.h), tr("-i " + q(c.truth60), c.w, c.h);
    std::vector<uint8_t> prev, blend(size_t(c.w) * c.h);
    Stat bp, bs, rp, rs, cp, cs;
    for (int k = 0; in.next(); ++k) {
        if (k > 0 && k >= kFirst) {
            if (!tr.seek(2 * k - 1)) break;
            for (size_t i = 0; i < blend.size(); ++i) blend[i] = uint8_t((prev[i] + in.y()[i] + 1) >> 1);
            bp.add(psnrY(blend.data(), tr.y(), c.w, c.h)); bs.add(ssimY(blend.data(), tr.y(), c.w, c.h));
            rp.add(psnrY(prev.data(), tr.y(), c.w, c.h)); rs.add(ssimY(prev.data(), tr.y(), c.w, c.h));
            if (!tr.seek(2 * k)) break;
            cp.add(psnrY(in.y(), tr.y(), c.w, c.h)); cs.add(ssimY(in.y(), tr.y(), c.w, c.h));
        }
        prev.assign(in.y(), in.y() + size_t(c.w) * c.h);
    }
    J j = J::obj();
    j["blend"] = scoreJson(bp, bs); j["repeat"] = scoreJson(rp, rs); j["calibrationInputVsTruth"] = scoreJson(cp, cs);
    say(std::format("  FI baselines {}: blend PSNR-Y {} dB SSIM {:.4f}, repeat {} dB, codec calibration {} dB", c.label, fmt2(bp.mean()), bs.mean(), fmt2(rp.mean()), fmt2(cp.mean())));
    return j;
}

// SR material: HR = the first N truth frames; LR = area 2x downscale. Baselines: bilinear and Lanczos back up.
const int kSrFrames = 30;
std::string srLrArgs(const Clip& c) { return std::format("-i {} -frames:v {} -vf scale={}:{}:flags=area", q(c.truth60), kSrFrames, c.w / 2, c.h / 2); }
J srBaselines(const Clip& c) {
    stage("SR baselines " + c.label);
    J j = J::obj();
    for (const char* f : {"bilinear", "lanczos", "bicubic"}) {
        FrameReader hr(std::format("-i {} -frames:v {}", q(c.truth60), kSrFrames), c.w, c.h);
        FrameReader up(std::format("-i {} -frames:v {} -vf scale={}:{}:flags=area,scale={}:{}:flags={}", q(c.truth60), kSrFrames, c.w / 2, c.h / 2, c.w, c.h, f), c.w, c.h);
        Stat p, s;
        while (hr.next() && up.next()) { p.add(psnrY(up.y(), hr.y(), c.w, c.h)); s.add(ssimY(up.y(), hr.y(), c.w, c.h)); }
        j[f] = scoreJson(p, s);
        say(std::format("  SR baseline {} -> {}: {} PSNR-Y {} dB SSIM {:.4f}", std::format("{}x{}", c.w / 2, c.h / 2), c.label, f, fmt2(p.mean()), s.mean()));
    }
    return j;
}

// ---------------------------------------------------------------- DXGI
struct Adapter { std::wstring name; uint32_t vendor = 0, device = 0; LUID luid{}; std::string driver; uint64_t vram = 0; };
std::vector<Adapter> listAdapters() {
    std::vector<Adapter> r;
    ComPtr<IDXGIFactory1> f;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return r;
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d);
        Adapter x; x.name = d.Description; x.vendor = d.VendorId; x.device = d.DeviceId; x.luid = d.AdapterLuid; x.vram = d.DedicatedVideoMemory;
        LARGE_INTEGER umd{};
        if (SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
            x.driver = std::format("{}.{}.{}.{}", HIWORD(umd.HighPart), LOWORD(umd.HighPart), HIWORD(umd.LowPart), LOWORD(umd.LowPart));
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) x.name += L" (software)";
        r.push_back(x);
    }
    return r;
}

// ---------------------------------------------------------------- VPL
#ifdef HAVE_VPL
std::string fourcc(mfxU32 f) { std::string s; for (int i = 0; i < 4; ++i) { const char c = char((f >> (8 * i)) & 0xFF); s += (c >= 32 && c < 127) ? c : '?'; } return s; }
std::string stsName(mfxStatus s) {
    switch (s) {
    case MFX_ERR_NONE: return "MFX_ERR_NONE"; case MFX_ERR_UNSUPPORTED: return "MFX_ERR_UNSUPPORTED";
    case MFX_ERR_INVALID_VIDEO_PARAM: return "MFX_ERR_INVALID_VIDEO_PARAM"; case MFX_ERR_INCOMPATIBLE_VIDEO_PARAM: return "MFX_ERR_INCOMPATIBLE_VIDEO_PARAM";
    case MFX_ERR_NOT_FOUND: return "MFX_ERR_NOT_FOUND"; case MFX_ERR_MORE_DATA: return "MFX_ERR_MORE_DATA"; case MFX_ERR_MORE_SURFACE: return "MFX_ERR_MORE_SURFACE";
    case MFX_ERR_DEVICE_FAILED: return "MFX_ERR_DEVICE_FAILED"; case MFX_ERR_DEVICE_LOST: return "MFX_ERR_DEVICE_LOST"; case MFX_ERR_MEMORY_ALLOC: return "MFX_ERR_MEMORY_ALLOC";
    case MFX_ERR_NOT_INITIALIZED: return "MFX_ERR_NOT_INITIALIZED"; case MFX_ERR_INVALID_HANDLE: return "MFX_ERR_INVALID_HANDLE"; case MFX_ERR_NULL_PTR: return "MFX_ERR_NULL_PTR";
    case MFX_ERR_ABORTED: return "MFX_ERR_ABORTED"; case MFX_ERR_GPU_HANG: return "MFX_ERR_GPU_HANG"; case MFX_ERR_UNDEFINED_BEHAVIOR: return "MFX_ERR_UNDEFINED_BEHAVIOR";
    case MFX_WRN_INCOMPATIBLE_VIDEO_PARAM: return "MFX_WRN_INCOMPATIBLE_VIDEO_PARAM"; case MFX_WRN_PARTIAL_ACCELERATION: return "MFX_WRN_PARTIAL_ACCELERATION";
    case MFX_WRN_FILTER_SKIPPED: return "MFX_WRN_FILTER_SKIPPED"; case MFX_WRN_DEVICE_BUSY: return "MFX_WRN_DEVICE_BUSY"; case MFX_WRN_IN_EXECUTION: return "MFX_WRN_IN_EXECUTION";
    case MFX_WRN_VALUE_NOT_CHANGED: return "MFX_WRN_VALUE_NOT_CHANGED"; case MFX_WRN_OUT_OF_RANGE: return "MFX_WRN_OUT_OF_RANGE";
    default: return std::format("mfxStatus {}", int(s));
    }
}
mfxU16 align16(int v) { return mfxU16((v + 15) & ~15); }
void fillInfo(mfxFrameInfo& fi, int w, int h, int fps) {
    fi = {};
    fi.FourCC = MFX_FOURCC_NV12; fi.ChromaFormat = MFX_CHROMAFORMAT_YUV420; fi.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    fi.Width = align16(w); fi.Height = align16(h); fi.CropW = mfxU16(w); fi.CropH = mfxU16(h);
    fi.FrameRateExtN = mfxU32(fps); fi.FrameRateExtD = 1; fi.BitDepthLuma = 8; fi.BitDepthChroma = 8;
}

enum class Feat { FI, SR };
struct VppCfg {
    Feat feat = Feat::FI; bool video = true; int mode = 1; int w = 0, h = 0; bool withFrc = false;
    mfxExtVPPAIFrameInterpolation fi{}; mfxExtVPPAISuperResolution sr{}; mfxExtVPPFrameRateConversion frc{};
    mfxExtBuffer* ext[2]{}; mfxVideoParam par{};
    void build() {
        par = {};
        par.IOPattern = video ? mfxU16(MFX_IOPATTERN_IN_VIDEO_MEMORY | MFX_IOPATTERN_OUT_VIDEO_MEMORY) : mfxU16(MFX_IOPATTERN_IN_SYSTEM_MEMORY | MFX_IOPATTERN_OUT_SYSTEM_MEMORY);
        par.AsyncDepth = 1;
        int n = 0;
        if (feat == Feat::FI) {
            fillInfo(par.vpp.In, w, h, 30); fillInfo(par.vpp.Out, w, h, 60);
            fi = {}; fi.Header.BufferId = MFX_EXTBUFF_VPP_AI_FRAME_INTERPOLATION; fi.Header.BufferSz = sizeof(fi);
            fi.FIMode = mfxAIFrameInterpolationMode(mode); fi.EnableScd = 1;
            ext[n++] = &fi.Header;
            if (withFrc) { frc = {}; frc.Header.BufferId = MFX_EXTBUFF_VPP_FRAME_RATE_CONVERSION; frc.Header.BufferSz = sizeof(frc); frc.Algorithm = MFX_FRCALGM_AI_FRAME_INTERPOLATION; ext[n++] = &frc.Header; }
        } else {
            fillInfo(par.vpp.In, w / 2, h / 2, 30); fillInfo(par.vpp.Out, w, h, 30);
            sr = {}; sr.Header.BufferId = MFX_EXTBUFF_VPP_AI_SUPER_RESOLUTION; sr.Header.BufferSz = sizeof(sr);
            sr.SRMode = mfxAISuperResolutionMode(mode); sr.SRAlgorithm = MFX_AI_SUPER_RESOLUTION_ALGORITHM_DEFAULT;
            ext[n++] = &sr.Header;
        }
        par.ExtParam = ext; par.NumExtParam = mfxU16(n);
    }
};
const char* fiModeName(int m) { return m == 1 ? "default" : m == 2 ? "best_speed" : m == 3 ? "best_quality" : "?"; }

mfxStatus queryCfg(mfxSession s, VppCfg& c) {
    c.build();
    VppCfg out = c; out.build();   // own copies of the ext buffers for the output side
    return MFXVideoVPP_Query(s, &c.par, &out.par);
}

uint32_t pitchOf(const mfxFrameSurface1* s) { return (uint32_t(s->Data.PitchHigh) << 16) | uint32_t(s->Data.PitchLow); }
bool upload(mfxFrameSurface1* s, const uint8_t* nv12, int w, int h, std::string& err) {
    mfxStatus st = s->FrameInterface->Map(s, MFX_MAP_WRITE);
    if (st != MFX_ERR_NONE) { err = "Map(WRITE) " + stsName(st); return false; }
    const uint32_t p = pitchOf(s);
    for (int y = 0; y < h; ++y) memcpy(s->Data.Y + size_t(y) * p, nv12 + size_t(y) * w, size_t(w));
    for (int y = 0; y < h / 2; ++y) memcpy(s->Data.UV + size_t(y) * p, nv12 + size_t(w) * h + size_t(y) * w, size_t(w));
    s->FrameInterface->Unmap(s);
    return true;
}
bool readY(mfxFrameSurface1* s, std::vector<uint8_t>& y, int w, int h, std::string& err) {
    mfxStatus st = s->FrameInterface->Map(s, MFX_MAP_READ);
    if (st != MFX_ERR_NONE) { err = "Map(READ) " + stsName(st); return false; }
    const uint32_t p = pitchOf(s);
    y.resize(size_t(w) * h);
    for (int r = 0; r < h; ++r) memcpy(y.data() + size_t(r) * w, s->Data.Y + size_t(r) * p, size_t(w));
    s->FrameInterface->Unmap(s);
    return true;
}
// RunFrameVPPAsync with device-busy retries, then sync. Returns the run status; *ms = call + sync wall time.
mfxStatus runSync(mfxSession ses, mfxFrameSurface1* in, mfxFrameSurface1* out, double* ms, std::string& err) {
    const auto t0 = Clock::now();
    mfxSyncPoint sp = nullptr; mfxStatus st;
    for (int i = 0;; ++i) {
        st = MFXVideoVPP_RunFrameVPPAsync(ses, in, out, nullptr, &sp);
        if (st != MFX_WRN_DEVICE_BUSY || i > 2000) break;
        Sleep(1);
    }
    if ((st == MFX_ERR_NONE || st == MFX_ERR_MORE_SURFACE || st > 0) && sp) {
        mfxStatus ss;
        do { ss = MFXVideoCORE_SyncOperation(ses, sp, 1000); } while (ss == MFX_WRN_IN_EXECUTION);
        if (ss < MFX_ERR_NONE) { err = "SyncOperation " + stsName(ss); *ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); return ss; }
    }
    *ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    return st;
}

// FI 2x held-out run. Output index o of the 60 fps stream = truth frame o (even o: real k=o/2, odd: generated).
J runFi(mfxSession ses, VppCfg& c, const Clip& clip, int kFirst) {
    J j = J::obj();
    c.build();
    mfxStatus st = MFXVideoVPP_Init(ses, &c.par);
    j["init"] = stsName(st);
    if (st < MFX_ERR_NONE) return j;
    FrameReader in("-i " + q(clip.input30), clip.w, clip.h), tr("-i " + q(clip.truth60), clip.w, clip.h);
    Stat gp, gs, rp, rs, tGen, tReal, tPair; std::string err; std::vector<uint8_t> y;
    int64_t o = 0; int calls = 0; std::map<std::string, int> statuses;
    for (int k = 0; in.next() && err.empty(); ++k) {
        stage(std::format("FI run {} {} {} frame {}", clip.label, c.video ? "video" : "system", fiModeName(c.mode), k));
        mfxFrameSurface1* s = nullptr;
        if ((st = MFXMemory_GetSurfaceForVPP(ses, &s)) != MFX_ERR_NONE) { err = "GetSurfaceForVPP " + stsName(st); break; }
        if (!upload(s, in.frame.data(), clip.w, clip.h, err)) { s->FrameInterface->Release(s); break; }
        s->Data.FrameOrder = mfxU32(k); s->Data.TimeStamp = mfxU64(k) * 3000;   // 90 kHz at 30 fps
        double pair = 0;
        for (int guard = 0; guard < 8; ++guard) {
            mfxFrameSurface1* out = nullptr;
            if ((st = MFXMemory_GetSurfaceForVPPOut(ses, &out)) != MFX_ERR_NONE) { err = "GetSurfaceForVPPOut " + stsName(st); break; }
            double ms = 0;
            st = runSync(ses, s, out, &ms, err); ++calls; statuses[stsName(st)]++;
            if (st == MFX_ERR_NONE || st == MFX_ERR_MORE_SURFACE || st > 0) {
                pair += ms;
                const bool gen = (o & 1) != 0;
                (gen ? tGen : tReal).add(ms);
                const int64_t srcK = (o + 1) / 2;
                if (srcK >= kFirst && tr.seek(o) && readY(out, y, clip.w, clip.h, err)) {
                    const double p = psnrY(y.data(), tr.y(), clip.w, clip.h), ss = ssimY(y.data(), tr.y(), clip.w, clip.h);
                    if (gen) { gp.add(p); gs.add(ss); } else { rp.add(p); rs.add(ss); }
                }
                ++o;
                out->FrameInterface->Release(out);
                if (st == MFX_ERR_MORE_SURFACE) continue;
                break;
            }
            out->FrameInterface->Release(out);
            if (st == MFX_ERR_MORE_DATA) break;
            if (err.empty()) err = "RunFrameVPPAsync " + stsName(st);
            break;
        }
        if (k >= kFirst) tPair.add(pair);
        s->FrameInterface->Release(s);
    }
    MFXVideoVPP_Close(ses);
    j["outputs"] = int64_t(o); j["calls"] = calls;
    J sj = J::obj(); for (auto& [k, v] : statuses) sj[k] = v; j["runStatuses"] = sj;
    if (!err.empty()) j["error"] = err;
    j["generated"] = scoreJson(gp, gs); j["passthroughCalibration"] = scoreJson(rp, rs);
    j["msGeneratedCall"] = msJson(tGen); j["msRealCall"] = msJson(tReal); j["msPerSourceFrame"] = msJson(tPair);
    return j;
}

J runSr(mfxSession ses, VppCfg& c, const Clip& clip) {
    J j = J::obj();
    c.build();
    mfxStatus st = MFXVideoVPP_Init(ses, &c.par);
    j["init"] = stsName(st);
    if (st < MFX_ERR_NONE) return j;
    FrameReader lr(srLrArgs(clip), clip.w / 2, clip.h / 2), hr(std::format("-i {} -frames:v {}", q(clip.truth60), kSrFrames), clip.w, clip.h);
    Stat p, s, t; std::string err; std::vector<uint8_t> y;
    for (int k = 0; lr.next() && hr.next() && err.empty(); ++k) {
        stage(std::format("SR run {} {} frame {}", clip.label, c.video ? "video" : "system", k));
        mfxFrameSurface1 *in = nullptr, *out = nullptr;
        if ((st = MFXMemory_GetSurfaceForVPP(ses, &in)) != MFX_ERR_NONE) { err = "GetSurfaceForVPP " + stsName(st); break; }
        if (!upload(in, lr.frame.data(), clip.w / 2, clip.h / 2, err)) { in->FrameInterface->Release(in); break; }
        in->Data.FrameOrder = mfxU32(k);
        if ((st = MFXMemory_GetSurfaceForVPPOut(ses, &out)) != MFX_ERR_NONE) { err = "GetSurfaceForVPPOut " + stsName(st); in->FrameInterface->Release(in); break; }
        double ms = 0;
        st = runSync(ses, in, out, &ms, err);
        if (st == MFX_ERR_NONE || st > 0) {
            if (k >= 2) t.add(ms);
            if (readY(out, y, clip.w, clip.h, err)) { p.add(psnrY(y.data(), hr.y(), clip.w, clip.h)); s.add(ssimY(y.data(), hr.y(), clip.w, clip.h)); }
        } else if (err.empty()) err = "RunFrameVPPAsync " + stsName(st);
        out->FrameInterface->Release(out); in->FrameInterface->Release(in);
    }
    MFXVideoVPP_Close(ses);
    if (!err.empty()) j["error"] = err;
    j["score"] = scoreJson(p, s); j["ms"] = msJson(t);
    return j;
}
#endif

// ---------------------------------------------------------------- score-harness mode
int scoreHarness(const fs::path& dir, const fs::path& truth, int w, int h, int kFirst, int kLast, const fs::path& jsonOut) {
    auto pngY = [&](const fs::path& f, std::vector<uint8_t>& y) -> bool {
        FrameReader r("-i " + q(f) + " -vf scale=out_color_matrix=bt709:out_range=tv", w, h);
        if (!r.next()) return false;
        y.assign(r.y(), r.y() + size_t(w) * h); return true;
    };
    FrameReader tr("-i " + q(truth), w, h);
    Stat gp, gs, rp, rs; std::vector<uint8_t> y; int missing = 0;
    for (int k = kFirst; k <= kLast; ++k) {
        const fs::path g = dir / std::format("frame{:05}-0-generated.png", k), r = dir / std::format("frame{:05}-1-real.png", k);
        if (fs::exists(g) && tr.seek(2 * k - 1) && pngY(g, y)) { gp.add(psnrY(y.data(), tr.y(), w, h)); gs.add(ssimY(y.data(), tr.y(), w, h)); } else ++missing;
        if (fs::exists(r) && tr.seek(2 * k) && pngY(r, y)) { rp.add(psnrY(y.data(), tr.y(), w, h)); rs.add(ssimY(y.data(), tr.y(), w, h)); }
    }
    J j = J::obj();
    j["dir"] = dir.filename().string(); j["kFirst"] = kFirst; j["kLast"] = kLast; j["missingGenerated"] = missing;
    j["generated"] = scoreJson(gp, gs); j["realCalibration"] = scoreJson(rp, rs);
    std::ostringstream os; j.dump(os);
    std::cout << os.str() << std::endl;
    if (!jsonOut.empty()) { std::ofstream f(jsonOut, std::ios::binary); f << os.str() << "\n"; }
    return gp.v.empty() ? 1 : 0;
}

// ---------------------------------------------------------------- main
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::map<std::string, std::string> a;
    for (int i = 1; i + 1 < argc; i += 2) a[utf8(argv[i])] = utf8(argv[i + 1]);
    if (argc % 2 == 0) { std::cout << "arguments come in pairs (--key value)\n"; return 2; }
    auto arg = [&](const char* k, const std::string& d) { auto it = a.find(k); return it == a.end() ? d : it->second; };
    g_ffmpeg = arg("--ffmpeg", "ffmpeg");
    const int kFirst = std::stoi(arg("--kfirst", "4"));

    if (a.count("--score-harness")) {
        int w = 0, h = 0;
        if (sscanf_s(arg("--size", "").c_str(), "%dx%d", &w, &h) != 2) { std::cout << "--size WxH required\n"; return 2; }
        return scoreHarness(fs::path(wide(a["--score-harness"])), fs::path(wide(arg("--truth", ""))), w, h, kFirst, std::stoi(arg("--klast", "89")),
                            fs::path(wide(arg("--json", ""))));
    }

    g_outDir = fs::path(wide(arg("--out", "vpl_probe_out")));
    fs::create_directories(g_outDir);
    const double seconds = std::stod(arg("--seconds", "3"));
    const bool doFi = arg("--fi", "1") != "0", doSr = arg("--sr", "1") != "0", sr4k = arg("--sr4k", "1") != "0";
    std::vector<int> sizes;
    { std::stringstream ss(arg("--sizes", "1080,2160")); std::string t; while (std::getline(ss, t, ',')) if (!t.empty()) sizes.push_back(std::stoi(t)); }
    std::vector<int> modes;
    { std::stringstream ss(arg("--modes", "default,speed,quality")); std::string t; while (std::getline(ss, t, ',')) modes.push_back(t == "speed" ? 2 : t == "quality" ? 3 : 1); }
    const fs::path clip60 = fs::path(wide(arg("--clip60", "")));
    startWatchdog(std::stoi(arg("--timeout", "1800")));

    g_report["tool"] = "vpl_ai_probe";
    g_report["status"] = "running";
#ifdef HAVE_VPL
    g_report["builtWithVpl"] = true;
    g_report["headerApiVersion"] = std::format("{}.{}", MFX_VERSION_MAJOR, MFX_VERSION_MINOR);
#else
    g_report["builtWithVpl"] = false;
#endif
    {
        std::string v;
        if (runWait("\"" + g_ffmpeg + "\" -hide_banner -version", &v) != 0) {
            std::cout << "ffmpeg not found (" << g_ffmpeg << "); pass --ffmpeg EXE\n";
            g_report["status"] = "setup error: ffmpeg not found"; flushReport(); return 2;
        }
        g_report["ffmpeg"] = v.substr(0, v.find('\n'));
    }

    // adapters
    const auto adapters = listAdapters();
    bool intelGpu = false;
    {
        J aj = J::arr();
        for (auto& x : adapters) {
            J o = J::obj(); o["name"] = utf8(x.name); o["vendorId"] = std::format("0x{:04X}", x.vendor); o["deviceId"] = std::format("0x{:04X}", x.device);
            o["driver"] = x.driver; o["vramMiB"] = uint64_t(x.vram >> 20); aj.push(o);
            if (x.vendor == 0x8086) intelGpu = true;
            say(std::format("adapter: {} vendor 0x{:04X} device 0x{:04X} driver {}", utf8(x.name), x.vendor, x.device, x.driver));
        }
        g_report["adapters"] = aj; g_report["intelAdapterPresent"] = intelGpu;
    }

    // test material + baselines (no VPL needed)
    std::vector<Clip> clips;
    {
        J mj = J::arr();
        for (int h : (clip60.empty() ? sizes : std::vector<int>{0})) {
            Clip c; J info = J::obj();
            if (!makeClip(c, g_outDir / (clip60.empty() ? std::format("synthetic{}", h) : std::string("clip")), h, seconds, clip60, info)) {
                say("material error: " + (info["error"].t == J::Str ? info["error"].s : std::string("?")));
                mj.push(info); continue;
            }
            if (doFi) info["fiBaselines"] = fiBaselines(c, kFirst);
            if (doSr) info["srBaselines"] = srBaselines(c);
            mj.push(info); clips.push_back(c);
            { std::lock_guard<std::mutex> l(g_mx); g_report["material"] = mj; }
            flushReport();
        }
        g_report["material"] = mj; g_report["kFirst"] = kFirst;
    }

    std::string verdictFi = "unsupported: ", verdictSr = "unsupported: ";
#ifndef HAVE_VPL
    verdictFi += "this build has no VPL dispatcher"; verdictSr = verdictFi;
    g_report["implementations"] = J::arr();
#else
    stage("MFXLoad");
    mfxLoader loader = MFXLoad();
    J impls = J::arr();
    int hwIntel = 0;
    std::vector<std::pair<mfxU32, std::string>> candidates;   // impl index, label
    if (!loader) {
        verdictFi += "MFXLoad failed"; verdictSr = verdictFi;
    } else {
        for (mfxU32 i = 0; i < 64; ++i) {
            stage(std::format("MFXEnumImplementations {}", i));
            mfxImplDescription* d = nullptr;
            const mfxStatus st = MFXEnumImplementations(loader, i, MFX_IMPLCAPS_IMPLDESCSTRUCTURE, reinterpret_cast<mfxHDL*>(&d));
            if (st == MFX_ERR_NOT_FOUND) break;
            if (st != MFX_ERR_NONE || !d) { J e = J::obj(); e["index"] = i; e["error"] = stsName(st); impls.push(e); break; }
            J o = J::obj();
            o["index"] = i;
            o["impl"] = d->Impl == MFX_IMPL_TYPE_HARDWARE ? "hardware" : d->Impl == MFX_IMPL_TYPE_SOFTWARE ? "software" : std::format("{}", int(d->Impl));
            o["accelerationMode"] = d->AccelerationMode == MFX_ACCEL_MODE_VIA_D3D11 ? "D3D11" : d->AccelerationMode == MFX_ACCEL_MODE_VIA_D3D9 ? "D3D9" : std::format("0x{:X}", unsigned(d->AccelerationMode));
            o["apiVersion"] = std::format("{}.{}", d->ApiVersion.Major, d->ApiVersion.Minor);
            o["implName"] = std::string(d->ImplName); o["license"] = std::string(d->License); o["keywords"] = std::string(d->Keywords);
            o["vendorId"] = std::format("0x{:04X}", d->VendorID); o["vendorImplId"] = std::format("0x{:X}", d->VendorImplID);
            o["deviceId"] = std::string(d->Dev.DeviceID);
            o["mediaAdapterType"] = d->Dev.MediaAdapterType == MFX_MEDIA_INTEGRATED ? "integrated" : d->Dev.MediaAdapterType == MFX_MEDIA_DISCRETE ? "discrete" : "unknown";
            bool vafi = false, vasr = false;
            J fl = J::arr();
            for (mfxU16 f = 0; f < d->VPP.NumFilters; ++f) {
                const mfxU32 cc = d->VPP.Filters[f].FilterFourCC;
                fl.push(fourcc(cc));
                if (cc == MFX_EXTBUFF_VPP_AI_FRAME_INTERPOLATION) vafi = true;
                if (cc == MFX_EXTBUFF_VPP_AI_SUPER_RESOLUTION) vasr = true;
            }
            o["vppFilters"] = fl; o["advertisesVAFI"] = vafi; o["advertisesVASR"] = vasr;
            const bool hw = d->Impl == MFX_IMPL_TYPE_HARDWARE;
            const std::string label = std::format("#{} {} {} API {}.{} {}", i, hw ? "HW" : "SW", d->ImplName, d->ApiVersion.Major, d->ApiVersion.Minor, d->Dev.DeviceID);
            const bool apiOk = d->ApiVersion.Major > 2 || (d->ApiVersion.Major == 2 && d->ApiVersion.Minor >= 12);
            o["apiAtLeast2_12"] = apiOk;
            if (hw && d->VendorID == 0x8086) ++hwIntel;
            MFXDispReleaseImplDescription(loader, d);
            mfxHDL path = nullptr;
            if (MFXEnumImplementations(loader, i, MFX_IMPLCAPS_IMPLPATH, &path) == MFX_ERR_NONE && path) {
                o["path"] = fs::path(reinterpret_cast<const char*>(path)).filename().string();
                MFXDispReleaseImplDescription(loader, path);
            }
            mfxExtendedDeviceId* ext = nullptr;
            if (MFXEnumImplementations(loader, i, MFX_IMPLCAPS_DEVICE_ID_EXTENDED, reinterpret_cast<mfxHDL*>(&ext)) == MFX_ERR_NONE && ext) {
                if (ext->LUIDValid)
                    for (auto& x : adapters) {
                        LUID l{}; memcpy(&l, ext->DeviceLUID, sizeof(l));
                        if (l.LowPart == x.luid.LowPart && l.HighPart == x.luid.HighPart) { o["adapter"] = utf8(x.name); o["adapterDriver"] = x.driver; }
                    }
                MFXDispReleaseImplDescription(loader, ext);
            }
            say("VPL implementation " + label + (vafi ? " [VAFI]" : "") + (vasr ? " [VASR]" : ""));
            impls.push(o);
            candidates.emplace_back(i, label);
        }
        if (candidates.empty()) {
            verdictFi += intelGpu ? "the VPL dispatcher found no implementation although an Intel adapter is present (driver too old or VPL runtime missing)"
                                  : "no Intel GPU and no VPL implementation on this system";
            verdictSr = verdictFi;
        }
    }
    g_report["implementations"] = impls; g_report["intelHardwareImplementations"] = hwIntel;
    flushReport();

    J tests = J::arr();
    bool fiPass = false, srPass = false; std::string fiWhy, srWhy;
    for (auto& [idx, label] : candidates) {
        J tj = J::obj(); tj["implementation"] = label;
        stage("MFXCreateSession " + label);
        mfxSession ses = nullptr;
        mfxStatus st = MFXCreateSession(loader, idx, &ses);
        tj["createSession"] = stsName(st);
        if (st != MFX_ERR_NONE) { tests.push(tj); fiWhy = srWhy = label + ": MFXCreateSession " + stsName(st); continue; }
        mfxVersion ver{}; MFXQueryVersion(ses, &ver); tj["sessionApiVersion"] = std::format("{}.{}", ver.Major, ver.Minor);
        J per = J::arr();
        for (auto& clip : clips) {
            J cj = J::obj(); cj["clip"] = clip.label;
            if (doFi) {
                // every combination is queried and recorded; the run uses the first that passes in this order:
                // video memory before system memory, the AI FI buffer alone before AI FI + FRC(AI), the default mode first
                J qj = J::obj(); VppCfg best; bool any = false;
                for (bool video : {true, false})
                    for (bool frc : {false, true})
                        for (int m : {1, 2, 3}) {
                            VppCfg c; c.feat = Feat::FI; c.video = video; c.mode = m; c.w = clip.w; c.h = clip.h; c.withFrc = frc;
                            stage(std::format("FI query {} {} {}{}", clip.label, video ? "video" : "system", fiModeName(m), frc ? " +FRC" : ""));
                            const mfxStatus qs = queryCfg(ses, c);
                            qj[std::format("{}_{}{}", video ? "video" : "system", fiModeName(m), frc ? "_frc" : "")] = stsName(qs);
                            if (qs >= MFX_ERR_NONE && !any) { best = c; best.mode = 1; any = true; }
                        }
                cj["fiQuery"] = qj;
                if (!any) fiWhy = label + ": MFXVideoVPP_Query rejected AI frame interpolation for " + clip.label;
                else {
                    J runs = J::obj();
                    for (int m : modes) {
                        VppCfg c = best; c.mode = m;
                        if (queryCfg(ses, c) < MFX_ERR_NONE) { runs[fiModeName(m)] = "query failed"; continue; }
                        J r = runFi(ses, c, clip, kFirst);
                        r["memory"] = c.video ? "video (D3D11)" : "system"; r["withFrc"] = c.withFrc;
                        say(std::format("  FI {} {} {}: PSNR-Y {} dB SSIM {}, ms/generated {} (p95 {}), ms/source frame {}, passthrough {} dB{}",
                                        label, clip.label, fiModeName(m), fmt2(r["generated"]["psnrY"].n), fmt2(r["generated"]["ssimY"].n),
                                        fmt2(r["msGeneratedCall"]["median"].n), fmt2(r["msGeneratedCall"]["p95"].n), fmt2(r["msPerSourceFrame"]["median"].n),
                                        fmt2(r["passthroughCalibration"]["psnrY"].n), r["error"].t == J::Str ? " ERROR " + r["error"].s : std::string()));
                        if (r["generated"]["n"].n > 0) fiPass = true;
                        runs[fiModeName(m)] = r;
                        { std::lock_guard<std::mutex> l(g_mx); g_report["partial"] = runs; }
                        flushReport();
                    }
                    cj["fiRuns"] = runs;
                }
            }
            if (doSr && (sr4k || clip.h < 2000)) {
                J qj = J::obj(); VppCfg best; bool any = false;
                for (bool video : {true, false}) {
                    VppCfg c; c.feat = Feat::SR; c.video = video; c.mode = 1; c.w = clip.w; c.h = clip.h;
                    stage(std::format("SR query {} {}", clip.label, video ? "video" : "system"));
                    const mfxStatus qs = queryCfg(ses, c);
                    qj[video ? "video_default" : "system_default"] = stsName(qs);
                    if (qs >= MFX_ERR_NONE && !any) { best = c; any = true; }
                }
                cj["srQuery"] = qj;
                if (!any) srWhy = label + ": MFXVideoVPP_Query rejected AI super resolution for " + clip.label;
                else {
                    J r = runSr(ses, best, clip);
                    r["memory"] = best.video ? "video (D3D11)" : "system";
                    say(std::format("  SR {} {}: PSNR-Y {} dB SSIM {}, ms {} (p95 {}){}", label, clip.label, fmt2(r["score"]["psnrY"].n), fmt2(r["score"]["ssimY"].n),
                                    fmt2(r["ms"]["median"].n), fmt2(r["ms"]["p95"].n), r["error"].t == J::Str ? " ERROR " + r["error"].s : std::string()));
                    if (r["score"]["n"].n > 0) srPass = true;
                    cj["srRun"] = r;
                }
            }
            per.push(cj);
            flushReport();
        }
        tj["clips"] = per;
        MFXClose(ses);
        tests.push(tj);
        { std::lock_guard<std::mutex> l(g_mx); g_report["tests"] = tests; }
        flushReport();
    }
    g_report["tests"] = tests;
    if (loader) MFXUnload(loader);
    if (!candidates.empty()) {
        verdictFi = fiPass ? "supported" : "unsupported: " + (fiWhy.empty() ? std::string("no run produced frames") : fiWhy);
        verdictSr = srPass ? "supported" : "unsupported: " + (srWhy.empty() ? std::string("no run produced frames") : srWhy);
    }
#endif
    if (!doFi) verdictFi = "skipped";
    if (!doSr) verdictSr = "skipped";
    g_report["VplFi"] = verdictFi; g_report["VplSr"] = verdictSr;
    g_report["status"] = "done";
    say("VplFi: " + verdictFi);
    say("VplSr: " + verdictSr);
    {
        std::lock_guard<std::mutex> l(g_mx);
        std::ofstream f(g_outDir / "summary.txt", std::ios::binary);
        for (auto& s : g_summary) f << s << "\n";
    }
    flushReport();
    g_deadlineMs = INT64_MAX;
    return 0;
}
