// PushMPC: Intel Video Super Resolution stage and self-test (see PushCoreIntelVsr.h).
#include "PushCoreIntelVsr.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <format>

using Microsoft::WRL::ComPtr;

namespace pushmpc {
namespace {
// synthetic luma with edges, a frequency sweep, fine lines and circles (deterministic)
double truthY(double x, double y, double w, double h)
{
    const double u = x / w, v = y / h;
    double s = 0.45 + 0.25 * std::sin(40.0 * u * u * 3.14159 + 6.0 * v);                      // chirp
    if (std::fmod(std::floor(x / 37.0) + std::floor(y / 29.0), 2.0) == 0) s += 0.18;           // blocks
    const double cx = x - w * 0.7, cy = y - h * 0.35, r = std::sqrt(cx * cx + cy * cy);
    if (std::fmod(r, 24.0) < 3.0) s -= 0.3;                                                     // rings
    if (std::fmod(x + 2 * y, 61.0) < 1.5) s += 0.3;                                             // thin diagonal lines
    if (u > 0.1 && u < 0.4 && v > 0.6 && v < 0.9 && std::fmod(std::floor(x / 6.0) * std::floor(y / 9.0), 3.0) == 0) s = 0.95;   // "text"
    return std::clamp(s, 0.0, 1.0);
}
uint8_t toLimited(double s) { return uint8_t(std::lround(16.0 + 219.0 * s)); }
double psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, uint32_t w, uint32_t h, uint32_t border)
{
    double se = 0; uint64_t n = 0;
    for (uint32_t y = border; y + border < h; ++y) for (uint32_t x = border; x + border < w; ++x) { const double d = double(a[size_t(y) * w + x]) - b[size_t(y) * w + x]; se += d * d; ++n; }
    const double mse = n ? se / double(n) : 0;
    return mse > 0 ? 10.0 * std::log10(255.0 * 255.0 / mse) : 99.0;
}
double sharpness(const std::vector<uint8_t>& a, uint32_t w, uint32_t h)
{
    double g = 0; uint64_t n = 0;
    for (uint32_t y = 1; y + 1 < h; ++y) for (uint32_t x = 1; x + 1 < w; ++x) {
        const double gx = double(a[size_t(y) * w + x + 1]) - a[size_t(y) * w + x - 1], gy = double(a[size_t(y + 1) * w + x]) - a[size_t(y - 1) * w + x];
        g += std::sqrt(gx * gx + gy * gy); ++n;
    }
    return n ? g / double(n) : 0;
}
double stddev(const std::vector<uint8_t>& a)
{
    double m = 0, q = 0;
    for (uint8_t v : a) { m += v; q += double(v) * v; }
    m /= double(a.size()); q /= double(a.size());
    return std::sqrt(std::max(0.0, q - m * m));
}
}  // namespace

IvsrSelfTest intelVsrSelfTest(D3D11Bridge& b, bool requireExtension)
{
    IvsrSelfTest r;
    r.ran = true;
    const uint32_t tw = 1920, th = 1080, sw = tw / 2, sh = th / 2;
    std::vector<uint8_t> truth(size_t(tw) * th);
    for (uint32_t y = 0; y < th; ++y) for (uint32_t x = 0; x < tw; ++x) truth[size_t(y) * tw + x] = toLimited(truthY(x + 0.5, y + 0.5, tw, th));
    std::vector<uint8_t> src(size_t(sw) * sh * 3 / 2, uint8_t(128));
    for (uint32_t y = 0; y < sh; ++y) for (uint32_t x = 0; x < sw; ++x) {
        const auto t = [&](uint32_t xx, uint32_t yy) { return int(truth[size_t(yy) * tw + xx]); };
        src[size_t(y) * sw + x] = uint8_t((t(2 * x, 2 * y) + t(2 * x + 1, 2 * y) + t(2 * x, 2 * y + 1) + t(2 * x + 1, 2 * y + 1) + 2) / 4);
    }
    // the bilinear baseline (centre-aligned, as a video scaler samples)
    std::vector<uint8_t> bil(size_t(tw) * th);
    for (uint32_t y = 0; y < th; ++y) for (uint32_t x = 0; x < tw; ++x) {
        const double fx = (x + 0.5) * 0.5 - 0.5, fy = (y + 0.5) * 0.5 - 0.5;
        const int x0 = std::clamp(int(std::floor(fx)), 0, int(sw) - 1), y0 = std::clamp(int(std::floor(fy)), 0, int(sh) - 1);
        const int x1 = std::min(x0 + 1, int(sw) - 1), y1 = std::min(y0 + 1, int(sh) - 1);
        const double ax = std::clamp(fx - x0, 0.0, 1.0), ay = std::clamp(fy - y0, 0.0, 1.0);
        auto s = [&](int xx, int yy) { return double(src[size_t(yy) * sw + xx]); };
        bil[size_t(y) * tw + x] = uint8_t(std::lround((s(x0, y0) * (1 - ax) + s(x1, y0) * ax) * (1 - ay) + (s(x0, y1) * (1 - ax) + s(x1, y1) * ax) * ay));
    }
    r.psnrBilinear = psnr(bil, truth, tw, th, 4);
    r.sharpBilinear = sharpness(bil, tw, th);

    std::string why;
    D3D11_TEXTURE2D_DESC sd{}; sd.Width = sw; sd.Height = sh; sd.MipLevels = 1; sd.ArraySize = 1; sd.Format = DXGI_FORMAT_NV12; sd.SampleDesc = {1, 0};
    sd.Usage = D3D11_USAGE_DEFAULT; sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{src.data(), sw, 0};
    ComPtr<ID3D11Texture2D> in, out, staging;
    HRESULT hr = b.device()->CreateTexture2D(&sd, &init, &in);
    if (FAILED(hr)) { r.reason = std::format("fail: NV12 source texture 0x{:08X}", unsigned(hr)); return r; }
    D3D11_TEXTURE2D_DESC od = sd; od.Width = tw; od.Height = th; od.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(hr = b.device()->CreateTexture2D(&od, nullptr, &out))) { r.reason = std::format("fail: NV12 output texture 0x{:08X} (no NV12 render target)", unsigned(hr)); return r; }
    D3D11_TEXTURE2D_DESC st = od; st.BindFlags = 0; st.Usage = D3D11_USAGE_STAGING; st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(hr = b.device()->CreateTexture2D(&st, nullptr, &staging))) { r.reason = std::format("fail: staging 0x{:08X}", unsigned(hr)); return r; }
    VideoBlit vb;
    if (!vb.init(b, sw, sh, DXGI_FORMAT_NV12, tw, th, DXGI_FORMAT_NV12, why)) { r.reason = "fail: " + why; return r; }
    vb.setColor(true, true, true, true, true);
    std::string extWhy;
    const HRESULT ext = vb.enableIntelSuperResolution(true, extWhy);
    if (FAILED(ext) && requireExtension) { r.reason = "fail: " + extWhy; return r; }
    if (FAILED(hr = vb.blit(in.Get(), 0, out.Get()))) { r.reason = std::format("fail: VideoProcessorBlt 0x{:08X}", unsigned(hr)); return r; }
    b.context()->CopyResource(staging.Get(), out.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(hr = b.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) { r.reason = std::format("fail: Map 0x{:08X}", unsigned(hr)); return r; }
    std::vector<uint8_t> got(size_t(tw) * th);
    for (uint32_t y = 0; y < th; ++y) memcpy(&got[size_t(y) * tw], static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch, tw);
    b.context()->Unmap(staging.Get(), 0);
    r.psnr = psnr(got, truth, tw, th, 4);
    r.sharp = sharpness(got, tw, th);
    const double sd2 = stddev(got);

    // cost: 1080p -> 4K blits, GPU-bound wall time (event query), 8 runs after one warm-up
    {
        VideoBlit vc; ComPtr<ID3D11Texture2D> in2, out2;
        D3D11_TEXTURE2D_DESC a = sd; a.Width = tw; a.Height = th;
        D3D11_TEXTURE2D_DESC o = od; o.Width = 3840; o.Height = 2160;
        D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
        ComPtr<ID3D11Query> q;
        if (SUCCEEDED(b.device()->CreateTexture2D(&a, nullptr, &in2)) && SUCCEEDED(b.device()->CreateTexture2D(&o, nullptr, &out2)) &&
            vc.init(b, tw, th, DXGI_FORMAT_NV12, 3840, 2160, DXGI_FORMAT_NV12, why) && SUCCEEDED(b.device()->CreateQuery(&qd, &q))) {
            vc.setColor(true, true, true, true, true);
            std::string e2;
            if (SUCCEEDED(ext)) vc.enableIntelSuperResolution(true, e2);
            auto drain = [&] { b.context()->End(q.Get()); b.context()->Flush(); BOOL done = FALSE; const auto t = std::chrono::steady_clock::now();
                               while (b.context()->GetData(q.Get(), &done, sizeof(done), 0) != S_OK && std::chrono::steady_clock::now() - t < std::chrono::seconds(5)) Sleep(0); };
            vc.blit(in2.Get(), 0, out2.Get()); drain();
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < 8; ++i) vc.blit(in2.Get(), 0, out2.Get());
            drain();
            r.ms4k = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 8.0;
        }
    }
    const double floor = std::max(22.0, r.psnrBilinear - 1.0);
    const bool blank = sd2 < 10.0;
    const bool sharpOk = r.sharp >= r.sharpBilinear * 1.0;
    r.pass = SUCCEEDED(ext) && !blank && sharpOk && r.psnr >= floor;
    std::string verdict = r.pass ? "pass" : FAILED(ext) ? "fail: " + extWhy : blank ? "fail: blank output" : !sharpOk ? "fail: softer than bilinear" : "fail: PSNR below the floor";
    r.reason = std::format("{}; psnr={:.2f} (bilinear {:.2f}, floor {:.2f}) sharp={:.2f} (bilinear {:.2f}) stddev={:.1f} ms4k={:.2f}",
                           verdict, r.psnr, r.psnrBilinear, floor, r.sharp, r.sharpBilinear, sd2, r.ms4k);
    return r;
}

double intelVsrCostFromReason(const std::wstring& reason)
{
    const auto p = reason.find(L"ms4k=");
    if (p == std::wstring::npos) return -1;
    const double v = std::wcstod(reason.c_str() + p + 5, nullptr);
    return std::isfinite(v) && v > 0 && v < 1000 ? v : -1;
}

// ------------------------------------------------------------------------------------------------ the stage
bool IntelVsrStage::init(D3D11Bridge& b, uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH, DXGI_FORMAT format, bool bt709, bool limited,
                         uint32_t ring, std::string& why)
{
    reset();
    b_ = &b;
    if (!blit_.init(b, inW, inH, format, outW, outH, format, why)) return false;
    blit_.setColor(true, bt709, limited, true, limited);
    std::string ext;
    srOn_ = SUCCEEDED(blit_.enableIntelSuperResolution(true, ext));
    if (!srOn_) { why = ext; blit_.reset(); return false; }
    ring_.resize(ring);
    for (auto& t : ring_)
        if (!b.createFrom11(outW, outH, format, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, t, why)) { reset(); return false; }
    if (!b.createFence11(fence_, why)) { reset(); return false; }
    return true;
}

void IntelVsrStage::reset()
{
    blit_.reset(); ring_.clear(); fence_ = {}; srOn_ = false; pending_.clear(); freeQ_.clear(); done_.clear();
}

uint64_t IntelVsrStage::run(ID3D11Texture2D* source, uint32_t slot, ID3D11Fence* waitFence, uint64_t waitValue, ID3D11Fence* freeFence,
                            uint64_t freeValue, std::string& why)
{
    if (!ready() || slot >= ring_.size()) { why = "Intel VSR stage not ready"; return 0; }
    auto* c = b_->context();
    b_->wait(waitFence, waitValue);
    b_->wait(freeFence, freeValue);
    // GPU timestamps around the blit
    Q q;
    if (!freeQ_.empty()) { q = freeQ_.back(); freeQ_.pop_back(); }
    else {
        D3D11_QUERY_DESC dj{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, ts{D3D11_QUERY_TIMESTAMP, 0};
        if (FAILED(b_->device()->CreateQuery(&dj, &q.disjoint)) || FAILED(b_->device()->CreateQuery(&ts, &q.t0)) || FAILED(b_->device()->CreateQuery(&ts, &q.t1))) q = {};
    }
    if (q.disjoint) { c->Begin(q.disjoint.Get()); c->End(q.t0.Get()); }
    HRESULT hr = blit_.blit(source, 0, ring_[slot].tex.Get());
    if (FAILED(hr) && srOn_) {
        // the extension failing mid-stream: plain video processor scaling from now on (same sizes, the graph is unchanged)
        std::string e;
        blit_.enableIntelSuperResolution(false, e);
        srOn_ = false;
        why = std::format("Intel VSR blit failed 0x{:08X}: plain video processor scaling from now on", unsigned(hr));
        hr = blit_.blit(source, 0, ring_[slot].tex.Get());
    }
    if (q.disjoint) { c->End(q.t1.Get()); c->End(q.disjoint.Get()); pending_.push_back(q); }
    if (FAILED(hr)) { why = std::format("video processor blit failed 0x{:08X}", unsigned(hr)); return 0; }
    // harvest finished timings (never waits)
    while (!pending_.empty()) {
        auto& p = pending_.front();
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{}; UINT64 a = 0, e = 0;
        if (c->GetData(p.disjoint.Get(), &d, sizeof(d), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) break;
        if (c->GetData(p.t0.Get(), &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || c->GetData(p.t1.Get(), &e, sizeof(e), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) break;
        if (!d.Disjoint && d.Frequency && e >= a) { if (done_.size() < 256) done_.push_back(double(e - a) * 1000.0 / double(d.Frequency)); }
        freeQ_.push_back(p); pending_.pop_front();
    }
    return b_->signal(fence_);
}

std::vector<double> IntelVsrStage::takeTimings() { std::vector<double> r; r.swap(done_); return r; }

}  // namespace pushmpc
