// pushcore_route_test (Windows): the Kodi add-on's pushcore route (src/engine/pushcore_route.*) on WARP with the stub
// pushcore (generated frame j of k blends the two source frames at j/k) and pm_pushcore.dll next to this program.
// Flat frames (luma 60, then 180) go in as planes (8-bit and 10-bit), the way software-decoded frames do; for x2 and x4
// every generated frame must come back from the ring with luma 60 + 120 j/k, in Kodi's NV12 / P010 layout and in the
// planar layout (Kodi 21), and the real frames must read back unchanged.
//   pushcore_route_test <folder with the stub pushcore.dll>
#include <windows.h>
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
static double ReadLuma(pmk::PushcoreRoute& r, int slot, int bits, bool semi)
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

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: pushcore_route_test <stub core folder>\n"); return 2; }
    SetEnvironmentVariableA("PUSHMPC_CORE_DIR", argv[1]);
    Run(8, 2, true);
    Run(8, 4, false);
    Run(10, 2, true);
    Run(10, 4, false);
    std::printf(fails ? "pushcore route: %d FAILED\n" : "pushcore route: all passed\n", fails);
    return fails ? 1 : 0;
}
