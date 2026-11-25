// pushmpc.Enhance, backend "apple": the Apple-free halves of the Metal stages, so apple_test checks them on every OS
// and the Metal kernels (apple_metal.mm) are checked against them on macOS (apple_vt_test, apple_tool parity):
//   - ArtCNN (MIT, github.com/Artoriuz/ArtCNN) luma doublers: the .pmnn weights that vsplugin/tools/artcnn_tool.py
//     writes from the pinned upstream ONNX files, and a float reference of the network (3x3 convolutions with zero
//     padding, ReLU, the long skip, depth-to-space x2 in ONNX's DCR order, clip);
//   - 10-bit recovery for an 8-bit-only interpolator ("quantisation-residual transfer", second-opinion-apple.md
//     section 2): VideoToolbox's low-latency interpolator may list only 8-bit formats. The 2 bits it never sees are the
//     residuals RA = A10 - 4 * A8 and RB = B10 - 4 * B8 (A8 = what it was given, rounded). They move with the picture,
//     so the generated frame gets G10 = 4 * G8 + LP((1 - t) * RA(x - t * F) + t * RB(x + (1 - t) * F)), F = the
//     forward optical flow of the pair (zero when there is none), LP a 3x3 [1 2 1] blur. Where the flow is wrong the
//     error stays within the residuals' range (-2..+3 codes of 10-bit), never worse than plain 8-bit by more than half
//     an 8-bit step.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace pmvs::apple {

// ------------------------------------------------------------------------------------------------ ArtCNN weights
struct NnLayer {
    enum : uint32_t { kRelu = 1, kAddSkip = 2, kSaveSkip = 4, kFinal = 8 };
    int cin = 0, cout = 0;
    uint32_t flags = 0;
    float lo = 0, hi = 1;
    std::vector<float> w;   // [cout][cin][3][3]
    std::vector<float> b;   // [cout]
};
struct NnModel {
    std::vector<NnLayer> layers;
    std::string name;
    int MaxChannels() const { int m = 1; for (auto& l : layers) m = std::max({ m, l.cin, l.cout }); return m; }
    size_t Parameters() const { size_t n = 0; for (auto& l : layers) n += l.w.size() + l.b.size(); return n; }
    // checks the shape ArtCNN's C models have (what the Metal kernels and the shader generator support)
    bool Valid(std::string& err) const
    {
        if (layers.size() < 2) { err = "fewer than two layers"; return false; }
        if (layers.front().cin != 1) { err = "the first layer must take one channel (luma)"; return false; }
        if (!(layers.back().flags & NnLayer::kFinal) || layers.back().cout != 4) { err = "the last layer must be the 4-channel depth-to-space layer"; return false; }
        int skips = 0;
        for (size_t i = 0; i < layers.size(); ++i) {
            const NnLayer& l = layers[i];
            if (i && layers[i - 1].cout != l.cin) { err = "layers do not chain"; return false; }
            if (l.w.size() != size_t(l.cout) * size_t(l.cin) * 9 || l.b.size() != size_t(l.cout)) { err = "weight sizes"; return false; }
            if (l.flags & NnLayer::kSaveSkip) ++skips;
            if (l.cout > 64 || l.cin > 64) { err = "more than 64 channels"; return false; }
        }
        if (skips > 1) { err = "more than one skip"; return false; }
        return true;
    }
    bool Parse(const uint8_t* p, size_t n, std::string& err)
    {
        layers.clear();
        size_t o = 0;
        auto u32 = [&](uint32_t& v) { if (o + 4 > n) return false; std::memcpy(&v, p + o, 4); o += 4; return true; };
        auto f32 = [&](float& v) { if (o + 4 > n) return false; std::memcpy(&v, p + o, 4); o += 4; return true; };
        if (n < 12 || std::memcmp(p, "PMNN", 4) != 0) { err = "not a .pmnn file"; return false; }
        o = 4;
        uint32_t ver = 0, count = 0;
        u32(ver); u32(count);
        if (ver != 1 || count == 0 || count > 64) { err = "unsupported .pmnn version or layer count"; return false; }
        for (uint32_t i = 0; i < count; ++i) {
            NnLayer l;
            uint32_t ci = 0, co = 0;
            if (!u32(ci) || !u32(co) || !u32(l.flags) || !f32(l.lo) || !f32(l.hi) || ci == 0 || co == 0 || ci > 64 || co > 64) { err = "truncated layer header"; return false; }
            l.cin = int(ci); l.cout = int(co);
            l.w.resize(size_t(co) * ci * 9); l.b.resize(co);
            if (o + 4 * (l.w.size() + l.b.size()) > n) { err = "truncated weights"; return false; }
            std::memcpy(l.w.data(), p + o, 4 * l.w.size()); o += 4 * l.w.size();
            std::memcpy(l.b.data(), p + o, 4 * l.b.size()); o += 4 * l.b.size();
            layers.push_back(std::move(l));
        }
        return Valid(err);
    }
    bool Load(const std::string& path, std::string& err)
    {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) { err = "cannot open " + path; return false; }
        std::vector<uint8_t> d;
        uint8_t buf[65536];
        size_t r;
        while ((r = std::fread(buf, 1, sizeof(buf), f)) > 0) d.insert(d.end(), buf, buf + r);
        std::fclose(f);
        if (!Parse(d.data(), d.size(), err)) { err = path + ": " + err; return false; }
        const size_t s = path.find_last_of("/\\");
        name = path.substr(s == std::string::npos ? 0 : s + 1);
        if (name.size() > 5 && name.substr(name.size() - 5) == ".pmnn") name.resize(name.size() - 5);
        return true;
    }
};

// float reference: in = w x h luma in [0, 1], out = 2w x 2h. Rows are split over threads.
inline void NnReference(const NnModel& m, const float* in, int w, int h, float* out, int threads = 0)
{
    const int C = m.MaxChannels();
    const size_t px = size_t(w) * size_t(h);
    std::vector<float> a(px * size_t(C)), b(px * size_t(C)), skip;
    // channel-major tensors: t[c * px + y * w + x]
    std::memcpy(a.data(), in, px * sizeof(float));
    if (threads <= 0) threads = int(std::max(1u, std::min(16u, std::thread::hardware_concurrency())));
    for (size_t li = 0; li < m.layers.size(); ++li) {
        const NnLayer& L = m.layers[li];
        auto rows = [&](int y0, int y1) {
            for (int oc = 0; oc < L.cout; ++oc)
                for (int y = y0; y < y1; ++y)
                    for (int x = 0; x < w; ++x) {
                        float s = L.b[size_t(oc)];
                        for (int ic = 0; ic < L.cin; ++ic) {
                            const float* src = a.data() + size_t(ic) * px;
                            const float* k = L.w.data() + (size_t(oc) * size_t(L.cin) + size_t(ic)) * 9;
                            for (int ky = 0; ky < 3; ++ky) {
                                const int yy = y + ky - 1;
                                if (yy < 0 || yy >= h) continue;
                                for (int kx = 0; kx < 3; ++kx) {
                                    const int xx = x + kx - 1;
                                    if (xx < 0 || xx >= w) continue;
                                    s += k[ky * 3 + kx] * src[size_t(yy) * size_t(w) + size_t(xx)];
                                }
                            }
                        }
                        if (L.flags & NnLayer::kAddSkip) s += skip[size_t(oc) * px + size_t(y) * size_t(w) + size_t(x)];
                        if (L.flags & NnLayer::kRelu) s = std::max(s, 0.0f);
                        b[size_t(oc) * px + size_t(y) * size_t(w) + size_t(x)] = s;
                    }
        };
        std::vector<std::thread> ts;
        const int n = std::max(1, std::min(threads, h));
        for (int t = 0; t < n; ++t) ts.emplace_back(rows, h * t / n, h * (t + 1) / n);
        for (auto& t : ts) t.join();
        if (L.flags & NnLayer::kSaveSkip) skip.assign(b.begin(), b.begin() + ptrdiff_t(px) * L.cout);
        if (L.flags & NnLayer::kFinal) {
            // DCR depth-to-space: out(2y + i, 2x + j) = channel i * 2 + j
            const int ow = 2 * w;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    for (int i = 0; i < 2; ++i)
                        for (int j = 0; j < 2; ++j)
                            out[size_t(2 * y + i) * size_t(ow) + size_t(2 * x + j)] = std::clamp(b[size_t(i * 2 + j) * px + size_t(y) * size_t(w) + size_t(x)], L.lo, L.hi);
            return;
        }
        std::swap(a, b);
    }
}

inline double PsnrUnit(const float* a, const float* b, size_t n, int border = 0, int w = 0, int h = 0)
{
    double se = 0; size_t cnt = 0;
    for (size_t i = 0; i < n; ++i) {
        if (border && w) { const int x = int(i % size_t(w)), y = int(i / size_t(w)); if (x < border || y < border || x >= w - border || y >= h - border) continue; }
        const double d = double(a[i]) - double(b[i]);
        se += d * d; ++cnt;
    }
    if (!cnt) return 0;
    se /= double(cnt);
    return se <= 1e-20 ? 200.0 : 10.0 * std::log10(1.0 / se);
}

// ------------------------------------------------------------------------------------------------ 10-bit recovery
// a plane of 10-bit codes or 8-bit codes, rows packed
struct Plane16 { int w = 0, h = 0; std::vector<uint16_t> v; uint16_t At(int x, int y) const { x = std::clamp(x, 0, w - 1); y = std::clamp(y, 0, h - 1); return v[size_t(y) * size_t(w) + size_t(x)]; } };
// what VideoToolbox is given for a 10-bit code (apple_convert.h ToBi: rounded, clamped)
inline int To8(int v10) { return std::min(255, (v10 + 2) >> 2); }
inline int Residual(int v10) { return v10 - 4 * To8(v10); }    // -2..+3
// one generated plane. flowX / flowY: the forward flow (previous -> current) in this plane's pixels, per pixel (empty:
// zero motion); t: the phase. a10, b10: the source planes (10-bit codes); g8: the interpolator's output (8-bit codes).
inline void RecoverTenBitPlane(const Plane16& a10, const Plane16& b10, const Plane16& g8, const std::vector<float>& flowX, const std::vector<float>& flowY,
                               float t, Plane16& out)
{
    const int w = g8.w, h = g8.h;
    out.w = w; out.h = h; out.v.assign(size_t(w) * size_t(h), 0);
    auto res = [](const Plane16& p, float x, float y) {
        // bilinear residual at (x, y)
        const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
        const float fx = x - float(x0), fy = y - float(y0);
        const float r00 = float(Residual(p.At(x0, y0))), r10 = float(Residual(p.At(x0 + 1, y0)));
        const float r01 = float(Residual(p.At(x0, y0 + 1))), r11 = float(Residual(p.At(x0 + 1, y0 + 1)));
        return (r00 * (1 - fx) + r10 * fx) * (1 - fy) + (r01 * (1 - fx) + r11 * fx) * fy;
    };
    static const float kW[3] = { 1.f, 2.f, 1.f };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = size_t(y) * size_t(w) + size_t(x);
            const float fx = flowX.empty() ? 0.f : flowX[i], fy = flowY.empty() ? 0.f : flowY[i];
            float acc = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const float px = float(x + dx), py = float(y + dy);
                    const float r = (1 - t) * res(a10, px - t * fx, py - t * fy) + t * res(b10, px + (1 - t) * fx, py + (1 - t) * fy);
                    acc += kW[dx + 1] * kW[dy + 1] * r;
                }
            const float v = 4.f * float(g8.v[i]) + acc / 16.f;
            out.v[i] = uint16_t(std::clamp(int(std::lround(v)), 0, 1023));
        }
}

} // namespace pmvs::apple
