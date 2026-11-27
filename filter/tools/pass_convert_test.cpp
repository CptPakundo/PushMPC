// pass_convert_test: the pass-through's CPU conversions (filter/src/PushMpcPassConvert.h) on synthetic NV12 / P010
// pictures: YUY2 byte order and chroma rows, RGB32 colours for black / white / grey / the primaries in BT.601 and
// BT.709, P010 at its top byte, row strides wider than the picture. CPU only. Exit code 0 = all passed.
#define NOMINMAX
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>
#include <vector>

#include "PushMpcPassConvert.h"

static int g_fail = 0, g_checks = 0;
static void Check(bool ok, const std::string& what)
{
    ++g_checks; if (!ok) ++g_fail;
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
}

// a w x h NV12 (or P010) picture filled with one Y'CbCr colour; stride in bytes
static std::vector<uint8_t> Picture(int w, int h, size_t stride, int Y, int U, int V, bool p010)
{
    std::vector<uint8_t> b(stride * size_t(h) * 3 / 2, 0xCD);
    auto put = [&](uint8_t* row, int x, int v) { if (p010) { row[2 * x] = 0x40; row[2 * x + 1] = uint8_t(v); } else row[x] = uint8_t(v); };
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) put(&b[stride * y], x, Y);
    for (int y = 0; y < h / 2; ++y) for (int x = 0; x < w; x += 2) { put(&b[stride * (h + y)], x, U); put(&b[stride * (h + y)], x + 1, V); }
    return b;
}

int main()
{
    const int w = 8, h = 4;
    for (bool p010 : { false, true }) {
        const size_t stride = size_t(w) * (p010 ? 2 : 1) + 16;   // wider than the picture
        const char* tag = p010 ? "P010" : "NV12";
        {   // YUY2: Y0 U Y1 V, every row (4:2:0 chroma repeated on both rows)
            auto src = Picture(w, h, stride, 100, 60, 200, p010);
            src[stride * 1 + (p010 ? 2 * 3 + 1 : 3)] = 150;   // one luma sample differs: x 3, row 1
            const size_t ds = w * 2 + 8;
            std::vector<uint8_t> dst(ds * h, 0);
            pm::pass::ToYuy2(src.data(), stride, w, h, p010, dst.data(), ds);
            bool ok = true;
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; x += 2) {
                const uint8_t* d = &dst[ds * y + 2 * x];
                const int y1 = (y == 1 && x + 1 == 3) ? 150 : 100;
                ok = ok && d[0] == 100 && d[1] == 60 && d[2] == y1 && d[3] == 200;
            }
            Check(ok, std::format("{} -> YUY2: Y0 U Y1 V per pair, every row, the stride kept", tag));
        }
        struct Case { const char* name; int Y, U, V; int B, G, R; bool bt709; };
        const Case cases[] = {
            { "black", 16, 128, 128, 0, 0, 0, false }, { "white", 235, 128, 128, 255, 255, 255, false }, { "grey", 126, 128, 128, 128, 128, 128, true },
            { "BT.601 red", 81, 90, 240, 0, 0, 255, false }, { "BT.601 green", 145, 54, 34, 0, 255, 0, false }, { "BT.601 blue", 41, 240, 110, 255, 0, 0, false },
            { "BT.709 red", 63, 102, 240, 0, 0, 255, true }, { "BT.709 green", 173, 42, 26, 0, 255, 0, true }, { "BT.709 blue", 32, 240, 118, 255, 0, 0, true },
        };
        for (const Case& c : cases) {
            auto src = Picture(w, h, stride, c.Y, c.U, c.V, p010);
            const size_t ds = w * 4;
            std::vector<uint8_t> dst(ds * h, 0);
            pm::pass::ToRgb32(src.data(), stride, w, h, p010, c.bt709, dst.data(), ds);
            int worst = 0; bool alpha = true;
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                const uint8_t* d = &dst[ds * y + 4 * x];
                worst = std::max({ worst, std::abs(d[0] - c.B), std::abs(d[1] - c.G), std::abs(d[2] - c.R) });
                alpha = alpha && d[3] == 255;
            }
            Check(worst <= 3 && alpha, std::format("{} -> RGB32 {}: within {} of B {} G {} R {} (3 allowed: 8-bit reference values)", tag, c.name, worst, c.B, c.G, c.R));
        }
    }
    std::printf("RESULT %s: %d of %d checks passed\n", g_fail ? "FAIL" : "PASS", g_checks - g_fail, g_checks);
    return g_fail ? 1 : 0;
}
