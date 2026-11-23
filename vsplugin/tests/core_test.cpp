// The plugin's D3D11 host (corehost.cpp) and pipeline against the stub pushcore, on WARP (no GPU, no VapourSynth):
// NV12/P010 upload, submit2 with streamNow = INT64_MIN, fence wait, 4:4:4 readback and the 4:2:0 compute shader,
// pts -> output index, the warm-core reuse (flush / reload / reopen) and the missing-generated fallback.
//   core_test.exe <folder with the stub pushcore.dll>
#include <cstdio>
#include <memory>
#include "corehost.h"
#include "vslog.h"

using namespace pmvs;

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

struct Img { uint32_t w, h; int bytes; std::vector<uint8_t> p[3]; int64_t pts = 0; bool gen = false; };

// a source frame: flat luma (16 + 4 s, in 8-bit terms) and neutral chroma
struct Src {
    uint32_t w, h; int bits; std::vector<uint8_t> y, u, v;
    void Fill(int64_t s)
    {
        const int bpp = bits > 8 ? 2 : 1;
        y.assign(size_t(w) * h * bpp, 0); u.assign(size_t(w / 2) * (h / 2) * bpp, 0); v = u;
        const uint32_t luma = uint32_t(16 + 4 * (s % 50)), chroma = 128;
        auto put = [&](std::vector<uint8_t>& p, uint32_t val8) {
            if (bpp == 1) std::fill(p.begin(), p.end(), uint8_t(val8));
            else { auto* q = reinterpret_cast<uint16_t*>(p.data()); for (size_t i = 0; i < p.size() / 2; ++i) q[i] = uint16_t(val8 << (bits - 8)); }
        };
        put(y, luma); put(u, chroma); put(v, chroma);
    }
};

static int Run(CoreHost& h, OutFmt fmt, uint32_t w, uint32_t hh, int bits, int transfer, uint32_t displayHdr, int k, int frames, Reuse expect, int skipExpect = -1)
{
    std::string err;
    PushCoreStreamInfo si = {}; si.structSize = sizeof(si); si.width = w; si.height = hh; si.format = bits > 8 ? PUSHCORE_INPUT_P010 : PUSHCORE_INPUT_NV12;
    si.fpsNum = 24000; si.fpsDen = 1001; si.range = 1; si.matrix = transfer == 16 ? 9 : 1; si.primaries = transfer == 16 ? 9 : 1; si.transfer = transfer; si.displayHdr = displayHdr;
    PushCoreSettings ps; memset(&ps, 0, sizeof(ps)); ps.structSize = sizeof(ps); ps.multiplier = uint32_t(k);
    CoreKey key; key.width = w; key.height = hh; key.format = si.format; key.fpsNum = si.fpsNum; key.fpsDen = si.fpsDen; key.range = si.range; key.matrix = si.matrix;
    key.primaries = si.primaries; key.transfer = si.transfer; key.displayHdr = displayHdr; key.adapterLuid = h.DeviceLuid();
    key.settings.assign(reinterpret_cast<uint8_t*>(&ps), reinterpret_cast<uint8_t*>(&ps) + sizeof(ps));
    Reuse how; double ms;
    const uint64_t token = h.Acquire(key, si, ps, how, ms, err);
    CHECK(token != 0);
    if (!token) { std::printf("  acquire: %s\n", err.c_str()); return 0; }
    CHECK(how == expect);
    if (how != expect) std::printf("  reuse %s, expected %s\n", ReuseName(how), ReuseName(expect));
    CHECK(h.OutInfo().multiplier == uint32_t(k));
    CHECK(h.ConfigureEgress(fmt, transfer == 16, err));

    Pipeline<void*> pipe; pipe.Init(k, frames, SrcDur100ns(24000, 1001)); pipe.Reset();
    Src src{ w, hh, bits };
    h.source = [&](int64_t s, PlaneView& v) {
        src.Fill(s);
        const int bpp = bits > 8 ? 2 : 1;
        v.data[0] = src.y.data(); v.data[1] = src.u.data(); v.data[2] = src.v.data();
        v.stride[0] = ptrdiff_t(w) * bpp; v.stride[1] = v.stride[2] = ptrdiff_t(w / 2) * bpp; v.width = w; v.height = hh; v.bits = bits;
        return true;
    };
    const bool yuv = OutIs420(fmt), deep = fmt == OutFmt::Rgb30 || fmt == OutFmt::Yuv420P10;
    h.alloc = [&](uint32_t ow, uint32_t oh, OutImage& img) {
        auto* m = new Img{ ow, oh, deep ? 2 : 1 };
        for (int i = 0; i < 3; ++i) {
            const uint32_t pw = yuv && i ? ow / 2 : ow, ph = yuv && i ? oh / 2 : oh;
            m->p[i].assign(size_t(pw) * ph * m->bytes, 0xCD);
            img.data[i] = m->p[i].data(); img.stride[i] = ptrdiff_t(pw) * m->bytes;
        }
        img.handle = m; return true;
    };
    int live = 0;
    h.release = [&](void* f) { delete static_cast<Img*>(f); --live; };
    h.tag = [&](int64_t pts, bool gen, void* f) { auto* m = static_cast<Img*>(f); m->pts = pts; m->gen = gen; ++live; };

    int ok = 0, monotone = 0, realExact = 0, reals = 0, repeats = 0;
    double last = -1;
    for (int64_t n = 0; n < int64_t(frames) * k; ++n) {
        int64_t from, to; pipe.Requests(n, from, to);
        auto r = pipe.Produce(n, from, h, 5000);
        if (!r.ok) { std::printf("  frame %lld: %s\n", (long long)n, r.err.c_str()); break; }
        ++ok;
        if (r.repeated) { ++repeats; continue; }
        const Img* m = static_cast<const Img*>(r.frame);
        // the first sample of plane 0 (R for RGB, Y for YUV), in 8-bit terms
        const double v = m->bytes == 2 ? *reinterpret_cast<const uint16_t*>(m->p[0].data()) / 4.0 : m->p[0][0];
        const int64_t s = n / k;
        if (n % k == 0) {
            ++reals;
            // RGB: (16 + 4 s - 16) * 255 / 219; Y'CbCr after the 4:2:0 shader: 16 + 4 s again
            const double expectV = yuv ? 16 + 4 * (s % 50) : (4.0 * (s % 50)) * 255.0 / 219.0;
            if (std::fabs(v - expectV) <= 1.5) ++realExact; else if (reals < 4) std::printf("  real %lld: %.1f, expected %.1f\n", (long long)n, v, expectV);
        }
        if (s % 50 != 49 && v >= last - 0.01) ++monotone;
        last = v;
        if (s % 50 == 49) last = -1;
    }
    CHECK(ok == frames * k);
    CHECK(realExact == reals);
    CHECK(skipExpect < 0 ? repeats == k - 1 : repeats == skipExpect);
    CHECK(pipe.Count().offGrid == 0);
    std::printf("  %s %ux%u %d-bit transfer %d k %d: %d frames, %d real exact of %d, %d repeated, %llu generated, monotone %d, %s\n", OutFmtName(fmt), w, hh, bits, transfer, k, ok,
        realExact, reals, repeats, (unsigned long long)pipe.Count().generated, monotone, ReuseName(how));
    pipe.ClearWith([&](void* f) { delete static_cast<Img*>(f); --live; });
    CHECK(live == 0);
    h.Release(token);
    CHECK(!h.Owns(token));
    return ok;
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: core_test <stub pushcore folder>\n"); return 2; }
    CoreHost& h = CoreHost::Get();
    std::string err;
    HostPaths p; p.coreDir = Widen(argv[1]); p.runtimeDir = p.coreDir;
    CHECK(h.Load(p, err));
    if (!h.Loaded()) { std::printf("load: %s\n", err.c_str()); return 1; }
    CHECK(h.EnsureDevice(0, AdapterPick::Warp, err));
    PushCoreCaps caps; CHECK(h.Caps(caps) && caps.family == PUSHCORE_FAMILY_RTX50);
    auto opens = reinterpret_cast<uint32_t (*)(void)>(GetProcAddress(GetModuleHandleW(L"pushcore.dll"), "pushcore_stub_opens"));
    CHECK(opens != nullptr);

    // SDR 1080p-class (small), x4, 4:4:4: cold open
    Run(h, OutFmt::Rgb24, 320, 180, 8, 1, 0, 4, 30, Reuse::Open);
    // the same stream again (mpv's reload after a seek): flush, no open
    Run(h, OutFmt::Rgb24, 320, 180, 8, 1, 0, 4, 30, Reuse::Flush);
    CHECK(opens && opens() == 1);
    // other settings, same stream: setSettings + reload
    Run(h, OutFmt::Rgb24, 320, 180, 8, 1, 0, 2, 20, Reuse::Reload);
    CHECK(opens && opens() == 1);
    // 4:2:0 on the GPU (the compute shader), 8-bit
    Run(h, OutFmt::Yuv420P8, 320, 180, 8, 1, 0, 2, 20, Reuse::Flush);
    // HDR: P010 PQ source on an HDR display -> R10G10B10A2 -> RGB30, then 4:2:0 10-bit: another stream, reopen
    Run(h, OutFmt::Rgb30, 256, 144, 10, 16, 1, 3, 20, Reuse::Reopen);
    Run(h, OutFmt::Yuv420P10, 256, 144, 10, 16, 1, 3, 20, Reuse::Flush);
    CHECK(opens && opens() == 2);
    // skipped generated frames (every 4th pair): repeats of the nearest real frame
    SetEnvironmentVariableW(L"PUSHCORE_STUB_SKIP_EVERY", L"4");
    Run(h, OutFmt::Rgb24, 160, 90, 8, 1, 0, 3, 21, Reuse::Reopen, 5 * 2 + 2);
    SetEnvironmentVariableW(L"PUSHCORE_STUB_SKIP_EVERY", nullptr);
    std::printf("core_test: %d passed, %d failed (log %s)\n", g_pass, g_fail, Narrow(LogPath()).c_str());
    return g_fail ? 1 : 0;
}
