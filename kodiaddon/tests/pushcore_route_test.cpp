// pushcore_route_test (Windows): the Kodi add-on's pushcore route (src/engine/pushcore_route.*) on WARP with the stub
// pushcore (generated frame j of k blends the two source frames at j/k) and pm_pushcore.dll next to this program.
// Flat frames (luma 60, then 180) go in as planes (8-bit and 10-bit), the way software-decoded frames do; for x2 and x4
// every generated frame must come back from the ring with luma 60 + 120 j/k, in Kodi's NV12 / P010 layout and in the
// planar layout (Kodi 21), and the real frames must read back unchanged.
// With the bridge's ABI 3 (the stub's PUSHCORE_STUB_SR mode: its "upscaler" is the nearest pixel):
//   - pushcore's upscaler to a fixed size: every picture, real ones too, comes from pushcore at that size; a decoded
//     frame kept at the source size reads back enlarged (it stands in for a missing picture); the anime model is
//     switched mid-stream through the settings update and back, the output size unchanged;
//   - the drawn treatment: held-frame re-timing reported, the pictures collected as they come (CollectInto);
//   - ContentDetect's GPU pass on a texture of the route's device runs (WARP).
//   pushcore_route_test <folder with the stub pushcore.dll>
#include <windows.h>
#include <d3d11.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "pushcore_route.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); fails++; } } while (0)

static const int W = 640, H = 360;

struct Frame {
    std::vector<uint8_t> b;
    pm_yuv v{};
    Frame(int bits, int luma)
    {
        const int bps = bits > 8 ? 2 : 1, cw = W / 2, ch = H / 2;
        b.resize(size_t(W) * H * bps + 2 * size_t(cw) * ch * bps);
        uint8_t* p[3] = { b.data(), b.data() + size_t(W) * H * bps, b.data() + size_t(W) * H * bps + size_t(cw) * ch * bps };
        for (int i = 0; i < 3; ++i) {
            const int n = i ? cw * ch : W * H, val = i ? (bits > 8 ? 512 : 128) : (bits > 8 ? luma * 4 : luma);
            for (int x = 0; x < n; ++x) {
                if (bps == 2) reinterpret_cast<uint16_t*>(p[i])[x] = uint16_t(val);
                else p[i][x] = uint8_t(val);
            }
            v.data[i] = p[i];
            v.stride[i] = (i ? cw : W) * bps;
        }
    }
};

// mean luma (8-bit scale) of a read-back picture
static double ReadLuma(pmk::PushcoreRoute& r, int slot, int bits, bool semi, int W = ::W, int H = ::H)
{
    const int bps = bits > 8 ? 2 : 1, cw = W / 2, ch = H / 2;
    std::vector<uint8_t> y(size_t(W) * H * bps), u(size_t(cw) * ch * bps * 2), v(size_t(cw) * ch * bps);
    uint8_t* const d[3] = { y.data(), u.data(), semi ? nullptr : v.data() };
    const int s[3] = { W * bps, semi ? cw * 2 * bps : cw * bps, cw * bps };
    std::string err;
    if (!r.Read(slot, d, s, semi, err)) { std::printf("read: %s\n", err.c_str()); return -1; }
    double sum = 0;
    for (int i = 0; i < W * H; ++i) {
        if (bps == 1) sum += y[i];
        else {
            const uint16_t w = reinterpret_cast<const uint16_t*>(y.data())[i];
            sum += semi ? (w >> 6) / 4.0 : w / 4.0;   // P010 high bits, planar low bits
        }
    }
    return sum / (W * H);
}

static void Run(int bits, int k, bool semi)
{
    pmk::PushcoreRoute r;
    pmk::PcConfig c;
    c.width = W; c.height = H; c.bits = bits; c.smoother = k; c.displayHz = 240; c.warp = true;
    std::string why;
    if (!r.Open(c, why)) { CHECK(false, "open %d-bit x%d: %s", bits, k, why.c_str()); return; }
    std::printf("open %d-bit x%d: k=%d, %s\n", bits, k, r.K(), r.Describe().c_str());
    CHECK(r.K() == k, "k %d, wanted %d", r.K(), k);
    const int64_t fus = 41708;
    Frame a(bits, 60), b(bits, 180);
    std::string err;
    const int s0 = r.PushPlanes(a.v, 0, false, err);
    CHECK(s0 >= 0, "push 0: %s", err.c_str());
    const int s1 = r.PushPlanes(b.v, fus, true, err);
    CHECK(s1 >= 0, "push 1: %s", err.c_str());
    std::vector<int64_t> want;
    for (int j = 1; j < r.K(); ++j) want.push_back(fus * j / r.K());
    std::vector<int> got;
    r.Collect(want, double(fus), got);
    const double l0 = ReadLuma(r, s0, bits, semi), l1 = ReadLuma(r, s1, bits, semi);
    std::printf("  real frames: luma %.1f, %.1f\n", l0, l1);
    CHECK(std::fabs(l0 - 60) < 1 && std::fabs(l1 - 180) < 1, "real frames read back wrong (%.1f, %.1f)", l0, l1);
    for (size_t j = 0; j < got.size(); ++j) {
        CHECK(got[j] >= 0, "generated frame %zu not delivered", j + 1);
        if (got[j] < 0) continue;
        const double l = ReadLuma(r, got[j], bits, semi), exp = 60 + 120.0 * double(j + 1) / r.K();
        std::printf("  generated %zu/%d: luma %.1f (expected %.1f)\n", j + 1, r.K(), l, exp);
        CHECK(std::fabs(l - exp) < 3, "generated %zu luma %.1f, expected %.1f", j + 1, l, exp);
    }
    // a third frame keeps the pipeline going (the ring wraps; nothing stale)
    const int s2 = r.PushPlanes(a.v, 2 * fus, true, err);
    CHECK(s2 >= 0 && s2 != s0 && s2 != s1, "push 2: slot %d (%s)", s2, err.c_str());
    for (auto& w : want) w += fus;
    r.Collect(want, double(fus), got);
    for (size_t j = 0; j < got.size(); ++j) CHECK(got[j] >= 0, "second pair: generated frame %zu not delivered", j + 1);
    std::printf("  delivered %llu, missing %llu\n", static_cast<unsigned long long>(r.Delivered()), static_cast<unsigned long long>(r.Missed()));
}

// ABI 3: pushcore's upscaler to a fixed size (sr), every picture from pushcore
static void RunSr(int bits, bool semi)
{
    pmk::PushcoreRoute r;
    pmk::PcConfig c;
    c.width = W; c.height = H; c.bits = bits; c.smoother = 2; c.displayHz = 240; c.warp = true;
    c.sr = true; c.outW = 1920; c.outH = 1080;   // the screen: pushcore's 1440p box holds it
    std::string why;
    if (!r.Open(c, why)) { CHECK(false, "open sr %d-bit: %s", bits, why.c_str()); return; }
    std::printf("open sr %d-bit: %dx%d, k=%d, %s\n", bits, r.OutWidth(), r.OutHeight(), r.K(), r.Describe().c_str());
    const int ow = r.OutWidth(), oh = r.OutHeight();
    CHECK(r.Upscales() && r.AllOut() && ow == 2560 && oh == 1440 && r.AnimeModel() == -1, "sr output %dx%d", ow, oh);
    const int64_t fus = 41708;
    Frame a(bits, 60), b(bits, 180);
    std::string err;
    const int s0 = r.PushPlanes(a.v, 0, false, err);
    const int s1 = r.PushPlanes(b.v, fus, true, err);
    CHECK(s0 >= 0 && s1 >= 0, "push: %s", err.c_str());
    std::vector<int64_t> want = { 0, fus / 2, fus };
    std::vector<int> got(3, -1);
    r.CollectInto(want, fus / 4, 250, got);
    const double exp[3] = { 60, 120, 180 };
    for (int j = 0; j < 3; ++j) {
        CHECK(got[j] >= 0, "sr picture %d not delivered", j);
        if (got[j] < 0) continue;
        const double l = ReadLuma(r, got[j], bits, semi, ow, oh);
        std::printf("  picture %d at %lld: luma %.1f (expected %.0f)\n", j, (long long)want[j], l, exp[j]);
        CHECK(std::fabs(l - exp[j]) < 3, "sr picture %d luma %.1f", j, l);
    }
    // the decoded frame kept at the source size reads back at the output size (it stands in for a missing picture)
    const double lf = ReadLuma(r, s1, bits, semi, ow, oh);
    std::printf("  stand-in (decoded frame enlarged): luma %.1f\n", lf);
    CHECK(std::fabs(lf - 180) < 1, "stand-in luma %.1f", lf);
    // drawn animation measured: the anime upscaler at the same size, then Video Super Resolution again
    CHECK(r.Update(1, false, 0, why) && r.AnimeModel() == 0, "update to the anime model: %s", why.c_str());
    CHECK(r.OutWidth() == ow && r.OutHeight() == oh, "the output size stays");
    std::printf("  after the update: %s\n", r.Describe().c_str());
    CHECK(r.Update(3, false, -1, why) && r.AnimeModel() == -1, "update back: %s", why.c_str());
    // still running
    const int s2 = r.PushPlanes(a.v, 2 * fus, false, err);
    const int s3 = r.PushPlanes(b.v, 3 * fus, true, err);
    CHECK(s2 >= 0 && s3 >= 0, "push after update: %s", err.c_str());
    want = { 2 * fus, 2 * fus + fus / 2, 3 * fus };
    got.assign(3, -1);
    r.CollectInto(want, fus / 4, 250, got);
    for (int j = 0; j < 3; ++j) CHECK(got[j] >= 0, "after the update: picture %d not delivered", j);
}

// ABI 3: the drawn treatment (held-frame re-timing; every picture from pushcore, collected as it comes)
static void RunDrawn()
{
    pmk::PushcoreRoute r;
    pmk::PcConfig c;
    c.width = W; c.height = H; c.bits = 8; c.smoother = 2; c.displayHz = 240; c.warp = true; c.drawn = true;
    std::string why;
    if (!r.Open(c, why)) { CHECK(false, "open drawn: %s", why.c_str()); return; }
    std::printf("open drawn: %s; held %d, lag %d\n", r.Describe().c_str(), r.Held() ? 1 : 0, r.Lag());
    CHECK(r.AllOut() && !r.Upscales() && r.OutWidth() == W, "drawn: every picture from pushcore at the source size");
    CHECK(r.Held() == (r.Lag() > 0), "lag follows held frames");
    const int64_t fus = 41708;
    Frame a(8, 60), b(8, 180);
    std::string err;
    r.PushPlanes(a.v, 0, false, err);
    r.PushPlanes(b.v, fus, true, err);
    std::vector<int64_t> want = { 0, fus / 2 };
    std::vector<int> got(2, -1);
    r.CollectInto(want, fus / 4, 0, got);   // only what is finished
    for (int i = 0; i < 20 && (got[0] < 0 || got[1] < 0); ++i) r.CollectInto(want, fus / 4, 20, got);
    CHECK(got[0] >= 0 && got[1] >= 0, "drawn: pictures delivered");
    if (got[1] >= 0) CHECK(std::fabs(ReadLuma(r, got[1], 8, true) - 120) < 3, "drawn: generated picture");
    CHECK(r.Update(1, false, -1, why), "drawn: the measured class goes in: %s", why.c_str());
    std::printf("  drawn measured: %s\n", r.Describe().c_str());
    // ContentDetect's GPU pass on a decoded texture of the route's device
    ID3D11Texture2D* tex = nullptr;
    r.WithDevice([&](void* dev, void*) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_NV12; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        static_cast<ID3D11Device*>(dev)->CreateTexture2D(&d, nullptr, &tex);
    });
    CHECK(tex != nullptr, "a texture on the route's device");
    int measured = 0, cls = -1; bool cg = false;
    for (int i = 0; tex && i < 40; ++i) measured += r.MeasureTexture(tex, 0, i * fus, i == 0, true, cls, cg) ? 1 : 0;
    std::printf("  GPU content pass: %d of 40 frames, class %d\n", measured, cls);
    CHECK(measured == 40 && cls >= 0, "the GPU content pass runs on the route's device");
    if (tex) tex->Release();
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: pushcore_route_test <stub core folder>\n"); return 2; }
    SetEnvironmentVariableA("PUSHMPC_CORE_DIR", argv[1]);
    SetEnvironmentVariableA("PUSHCORE_STUB_SR", "1");   // the stub honours the upscaler (and reports the anime features)
    pmk::PushcoreRoute::SetLog([](const std::string& l) { std::printf("  [route] %s\n", l.c_str()); });
    Run(8, 2, true);
    Run(8, 4, false);
    Run(10, 2, true);
    Run(10, 4, false);
    RunSr(8, true);
    RunSr(10, false);
    RunDrawn();
    std::printf(fails ? "pushcore route: %d FAILED\n" : "pushcore route: all passed\n", fails);
    return fails ? 1 : 0;
}
