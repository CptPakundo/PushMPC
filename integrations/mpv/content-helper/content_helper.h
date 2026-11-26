// PushMPC: the content helper for plain mpv (pushmpc.lua when no PushMPC engine runs). The model-free parts, kept in
// one header so they are unit-tested without a model or a GPU (content_helper_test.cpp):
//   - the picture protocol: pushmpc.lua takes mpv's "screenshot-raw video" (bgr0, rendered by the VO from the decoder's
//     frame, so hardware decoding stays as it is) about once a second and hands over only the source rows the model's
//     224 x 224 centre square samples (two per output row, the centre columns plus one on each side), as a file in the
//     temp folder renamed into place. Picture() turns it into the model's input with ClipInput's geometry (bilinear,
//     the same sample positions) and CLIP's mean / std, RGB, planar NCHW.
//   - the evidence integrator: there are no per-frame cells here, so the cadence cues are not measured; the image model's
//     evidence goes through ContentDetector's own tracker (kImg* constants, one step per kStepSec of stream time, the
//     hand-built probability neutral at 0.5) and the CG sub-flag through its CG tracker (kCg*), with the same hysteresis
//     and the same policy (PolicyFor).
//   - scene cuts between two pictures from their colour histograms (a cut only makes the integrators forget faster).
//   - the status file the lua reads (key=value lines, written atomically).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>
#include "pushmpc/content_detect.h"

namespace pushmpc::content::mpvhelper {

constexpr int kSide = 224;                  // the model's input square
constexpr int kRowsSent = 2 * kSide;        // two source rows per output row (bilinear)
constexpr const char* kMagic = "PMCD1";

// ------------------------------------------------------------------------------------------------ picture protocol
// The file: one text line "PMCD1 <w> <h> <side> <x0> <y0> <cx0> <ncol> <pts> <cut> <seq>\n", then kRowsSent rows of
// ncol * 4 bytes (bgr0, mpv's screenshot-raw default) in output-row order: for output row j the source rows
// SourceRow(j, 0) and SourceRow(j, 1). side = min(w, h), (x0, y0) the centre square's corner, columns cx0 .. cx0+ncol-1
// (x0-1 .. x0+side clamped to the picture). pts: stream seconds. cut: 0 none, 1 a seek (a discontinuity), 2 a new file.
struct Picture {
    int w = 0, h = 0, side = 0, x0 = 0, y0 = 0, cx0 = 0, ncol = 0, cut = 0;
    double pts = 0;
    long long seq = 0;
    std::vector<uint8_t> rows;   // kRowsSent * ncol * 4 bytes
};

// The source row output row j samples (k = 0: the row above the sample position, 1: the row below), clamped to the
// picture as ClipInput clamps. Computed in double on both sides (the lua has only doubles), so both pick the same rows.
inline int SourceRow(int j, int k, int y0, int side, int h)
{
    const double fy = y0 + (j + 0.5) * side / double(kSide) - 0.5;
    return std::clamp(int(std::floor(fy)) + k, 0, h - 1);
}
inline void Columns(int w, int x0, int side, int& cx0, int& ncol)
{
    cx0 = std::max(0, x0 - 1);
    ncol = std::min(w - 1, x0 + side) - cx0 + 1;
}

inline bool ParsePicture(const std::string& bytes, Picture& p, std::string* err)
{
    auto fail = [&](const char* why) { if (err) *err = why; return false; };
    const size_t nl = bytes.find('\n');
    if (nl == std::string::npos || nl > 200) return fail("no header line");
    const std::string head = bytes.substr(0, nl);
    char magic[8] = {};
    int cut = 0; long long seq = 0; double pts = 0;
    if (std::sscanf(head.c_str(), "%7s %d %d %d %d %d %d %d %lf %d %lld", magic, &p.w, &p.h, &p.side, &p.x0, &p.y0, &p.cx0, &p.ncol, &pts, &cut, &seq) != 11)
        return fail("bad header");
    if (std::strcmp(magic, kMagic) != 0) return fail("not a PMCD1 picture");
    if (p.w < 16 || p.h < 16 || p.w > 16384 || p.h > 16384) return fail("picture size out of range");
    if (p.side != std::min(p.w, p.h) || p.x0 != (p.w - p.side) / 2 || p.y0 != (p.h - p.side) / 2) return fail("centre square does not match the size");
    int cx0, ncol; Columns(p.w, p.x0, p.side, cx0, ncol);
    if (p.cx0 != cx0 || p.ncol != ncol) return fail("columns do not match the size");
    if (cut < 0 || cut > 2 || !std::isfinite(pts)) return fail("bad cut flag or pts");
    const size_t need = size_t(kRowsSent) * size_t(p.ncol) * 4;
    if (bytes.size() - nl - 1 != need) return fail("payload size does not match the header");
    p.pts = pts; p.cut = cut; p.seq = seq;
    p.rows.assign(bytes.begin() + nl + 1, bytes.end());
    return true;
}

// The bytes pushmpc.lua writes, from a full bgr0 picture (tests and the offline check use it: the same rows the lua takes)
inline std::string MakePictureBytes(const uint8_t* bgr0, ptrdiff_t stride, int w, int h, double pts, int cut, long long seq)
{
    const int side = std::min(w, h), x0 = (w - side) / 2, y0 = (h - side) / 2;
    int cx0, ncol; Columns(w, x0, side, cx0, ncol);
    char head[200];
    std::snprintf(head, sizeof head, "%s %d %d %d %d %d %d %d %.3f %d %lld\n", kMagic, w, h, side, x0, y0, cx0, ncol, pts, cut, seq);
    std::string s(head);
    for (int j = 0; j < kSide; ++j)
        for (int k = 0; k < 2; ++k) {
            const uint8_t* r = bgr0 + ptrdiff_t(SourceRow(j, k, y0, side, h)) * stride + ptrdiff_t(cx0) * 4;
            s.append(reinterpret_cast<const char*>(r), size_t(ncol) * 4);
        }
    return s;
}

// The 224 x 224 RGB thumbnail (0..255, interleaved) with ClipInput's sample positions: bilinear between the two sent rows
// and two neighbouring columns
inline std::vector<float> Thumbnail(const Picture& p)
{
    std::vector<float> rgb(size_t(kSide) * kSide * 3);
    const size_t rowBytes = size_t(p.ncol) * 4;
    for (int j = 0; j < kSide; ++j) {
        const double fy = p.y0 + (j + 0.5) * p.side / double(kSide) - 0.5;
        const float ay = float(fy - std::floor(fy));
        const uint8_t* r0 = p.rows.data() + size_t(2 * j) * rowBytes;
        const uint8_t* r1 = r0 + rowBytes;
        for (int i = 0; i < kSide; ++i) {
            const double fx = p.x0 + (i + 0.5) * p.side / double(kSide) - 0.5;
            const int ix = int(std::floor(fx));
            const float ax = float(fx - ix);
            const int c0 = std::clamp(std::clamp(ix, 0, p.w - 1) - p.cx0, 0, p.ncol - 1);
            const int c1 = std::clamp(std::clamp(ix + 1, 0, p.w - 1) - p.cx0, 0, p.ncol - 1);
            for (int c = 0; c < 3; ++c) {
                const int b = 2 - c;   // bgr0 -> R, G, B
                const float top = r0[c0 * 4 + b] * (1 - ax) + r0[c1 * 4 + b] * ax;
                const float bot = r1[c0 * 4 + b] * (1 - ax) + r1[c1 * 4 + b] * ax;
                rgb[(size_t(j) * kSide + i) * 3 + c] = top * (1 - ay) + bot * ay;
            }
        }
    }
    return rgb;
}

// The model's input from the thumbnail: CLIP's mean / std (as ClipInput), planar NCHW
inline std::vector<float> ModelInput(const std::vector<float>& rgb)
{
    static const float mean[3] = { 0.48145466f, 0.4578275f, 0.40821073f }, sd[3] = { 0.26862954f, 0.26130258f, 0.27577711f };
    std::vector<float> out(size_t(3) * kSide * kSide);
    for (size_t px = 0; px < size_t(kSide) * kSide; ++px)
        for (int c = 0; c < 3; ++c)
            out[size_t(c) * kSide * kSide + px] = (std::clamp(rgb[px * 3 + c], 0.f, 255.f) / 255.f - mean[c]) / sd[c];
    return out;
}

// ------------------------------------------------------------------------------------------------ scene cuts
// Pictures come about a second apart, so a cut is judged from the colour distribution alone (motion inside one shot
// moves little of it): 4 x 4 x 4 RGB bins, L1 distance (0 = the same, 2 = nothing in common). At kCutL1 half of the
// picture's colour mass has moved. A false cut costs little: the integrators forget faster once.
class CutMeter {
public:
    static constexpr float kCutL1 = 1.0f;
    void Reset() { m_have = false; m_last = 0; }
    bool Push(const std::vector<float>& rgb)
    {
        std::array<float, 64> h{};
        const size_t n = rgb.size() / 3;
        for (size_t i = 0; i < n; ++i) {
            auto bin = [](float v) { return std::clamp(int(v) >> 6, 0, 3); };
            h[size_t(bin(rgb[i * 3]) * 16 + bin(rgb[i * 3 + 1]) * 4 + bin(rgb[i * 3 + 2]))] += 1.f / float(n);
        }
        bool cut = false;
        if (m_have) { float d = 0; for (int k = 0; k < 64; ++k) d += std::fabs(h[k] - m_prev[k]); m_last = d; cut = d >= kCutL1; }
        m_prev = h; m_have = true;
        return cut;
    }
    float last() const { return m_last; }
private:
    std::array<float, 64> m_prev{};
    bool m_have = false;
    float m_last = 0;
};

// ------------------------------------------------------------------------------------------------ evidence integrator
// ContentDetector::Classify with the image model's evidence and the hand-built probability neutral (0.5): per step
// pd = sigmoid(0.15 * logit(0.5) + 0.85 * logit(pImage)), L = decay * L + clip(logit(pd)), drawn above kImgOn, not drawn
// below -kImgOff; the CG integrator once per new keyframe while drawn (reset when not drawn). A keyframe's evidence is
// applied for each kStepSec of stream time since the last step (the detector repeats the newest keyframe's evidence in
// every window while it is at most 3 s old): at most 6 steps, and a jump (seek, a long gap) counts as one step after a cut.
class ImageTracker {
public:
    using D = ContentDetector;
    void Reset() { *this = ImageTracker(); }
    void Cut() { m_cut = true; }
    // the image model's result for the picture at stream time pts (pCg < 0: no CG probability)
    void Evidence(double pDrawn, double pts, double pCg)
    {
        m_img = pDrawn;
        if (pCg >= 0) { m_imgCg = pCg; m_cgNew = true; }
        m_haveCg = m_haveCg || pCg >= 0;
        int n;
        if (m_last < -1e17 || pts < m_last - 1e-6 || pts - m_last > 3.0 + D::kStepSec) {
            if (m_last > -1e17) m_cut = true;
            n = 1; m_last = pts;
        } else {
            n = int(std::floor((pts - m_last) / D::kStepSec + 1e-9));
            m_last += n * D::kStepSec;
        }
        for (int k = 0; k < n; ++k) Step();
    }
    const State& Current() const { return m_s; }
    int steps() const { return m_steps; }
    const DrawnPolicy& Policy() const { return PolicyFor(m_s.cls, m_s.cg); }

private:
    static double Logit(double q) { q = std::clamp(q, 1e-4, 1.0 - 1e-4); return std::log(q / (1.0 - q)); }
    void Step()
    {
        ++m_steps;
        const double pd = 1.0 / (1.0 + std::exp(-(0.15 * Logit(0.5) + 0.85 * Logit(m_img))));
        const bool isCut = m_cut; m_cut = false;
        State& s = m_s;
        ++s.windows;
        m_L = (isCut ? D::kImgCutDecay : D::kImgDecay) * m_L + std::clamp(Logit(pd), -double(D::kClip), double(D::kClip));
        int next = s.cls;
        if (m_L > D::kImgOn) next = kDrawn;
        else if (m_L < -D::kImgOff) next = kOther;
        if (next != s.cls) { if (s.cls != kUnknown) ++s.changes; s.cls = next; }
        if (s.cls != kDrawn) { m_Lcg = 0; s.cg = false; }
        else if (m_cgNew) {
            m_Lcg = (isCut ? D::kCgCutDecay : D::kCgDecay) * m_Lcg + std::clamp(Logit(m_imgCg), -double(D::kClip), double(D::kClip));
            if (m_Lcg > D::kCgOn) s.cg = true; else if (m_Lcg < -D::kCgOff) s.cg = false;
        }
        m_cgNew = false;
        s.drawnShare = float(pd); s.logOdds = float(m_L); s.imageEvidence = true;
        s.cgShare = m_haveCg ? float(m_imgCg) : -1.f; s.cgLogOdds = float(m_Lcg);
        s.prob[kDrawn] = float(pd); s.prob[kOther] = float(1.0 - pd);
        s.confidence = s.cls == kDrawn ? float(pd) : s.cls == kOther ? float(1.0 - pd) : 0.f;
    }
    State m_s;
    double m_L = 0, m_Lcg = 0, m_img = 0.5, m_imgCg = 0.5, m_last = -1e18;
    bool m_cut = false, m_cgNew = false, m_haveCg = false;
    int m_steps = 0;
};

// ------------------------------------------------------------------------------------------------ status
// Key=value lines; pushmpc.lua reads class, cg, state and the policy's animeUpscaler / animeUpscalerDs.
struct StatusInfo {
    std::string state = "loading";   // loading | ready | failed
    std::string model, reason;
    long long seq = -1;               // the last picture the model answered for
    double pImage = -1, pCg = -1, ms = -1, cutL1 = 0;
    long long pictures = 0, dropped = 0;
};
inline const char* ClassKey(int cls) { return cls == kDrawn ? "drawn" : cls == kUnknown ? "measuring" : "not-drawn"; }
inline std::string StatusText(const StatusInfo& i, const ImageTracker& t)
{
    const State& s = t.Current();
    const DrawnPolicy& p = t.Policy();
    std::ostringstream o;
    auto clean = [](std::string v) { for (char& c : v) if (c == '\n' || c == '\r') c = ' '; return v; };
    o.setf(std::ios::fixed); o.precision(4);
    o << "pushmpc-content=1\n" << "state=" << i.state << "\n" << "model=" << clean(i.model) << "\n" << "reason=" << clean(i.reason) << "\n"
      << "seq=" << i.seq << "\n" << "class=" << ClassKey(s.cls) << "\n" << "cg=" << (s.cg ? 1 : 0) << "\n"
      << "pImage=" << i.pImage << "\n" << "pDrawn=" << s.drawnShare << "\n" << "pCg=" << i.pCg << "\n"
      << "logOdds=" << s.logOdds << "\n" << "cgLogOdds=" << s.cgLogOdds << "\n" << "changes=" << s.changes << "\n"
      << "ms=" << i.ms << "\n" << "pictures=" << i.pictures << "\n" << "dropped=" << i.dropped << "\n" << "cutL1=" << i.cutL1 << "\n"
      << "holdRepair=" << p.holdRepair << "\n" << "animeUpscaler=" << p.animeUpscaler << "\n" << "animeUpscalerDs=" << p.animeUpscalerDs << "\n"
      << "signHold=" << p.signHold << "\n" << "lineArt=" << p.lineArt << "\n" << "animeDeband=" << p.animeDeband << "\n";
    return o.str();
}
inline std::map<std::string, std::string> ParseStatus(const std::string& t)
{
    std::map<std::string, std::string> m;
    std::istringstream in(t);
    for (std::string l; std::getline(in, l);) { const size_t e = l.find('='); if (e != std::string::npos) m[l.substr(0, e)] = l.substr(e + 1); }
    return m;
}
// atomic for the reader: a temp file renamed over the old one (fails while the reader has the file open on Windows:
// the caller retries)
inline bool WriteAtomic(const std::filesystem::path& p, const std::string& text)
{
    std::filesystem::path tmp = p; tmp += ".tmp";
    { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); if (!f) return false; f << text; if (!f.flush()) return false; }
    std::error_code ec;
    std::filesystem::rename(tmp, p, ec);
    if (ec) { std::filesystem::remove(tmp, ec); return false; }
    return true;
}
inline bool ReadFile(const std::filesystem::path& p, std::string& out)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream s; s << f.rdbuf(); out = s.str();
    return true;
}

} // namespace pushmpc::content::mpvhelper
