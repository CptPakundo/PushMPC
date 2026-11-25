// pushmpc-apple: the apple backend from the command line (macOS).
//   pushmpc-apple probe                       the VideoToolbox / Metal report
//   pushmpc-apple model-status                the editing-grade super-resolution model: ready | downloading | download required | n/a
//   pushmpc-apple model-download [seconds]    starts Apple's one-time model download and waits (default 600 s) with progress;
//                                             pushmpc-setup runs it with the user watching, as Apple asks
//   pushmpc-apple parity <models dir>         Metal against the float references: ArtCNN (PSNR >= 40 dB, 8 px border
//                                             excluded) and the 10-bit recovery kernel (max 1 code off)
//   pushmpc-apple bench [w h k fps]           Configure's choices and per-call timings, then 120 frames through the engine
//                                             (default 1920 1080 2 23.976); real numbers only on bare metal
// Exit code: 0 ok, 1 a check failed, 2 not available here.
#import <Foundation/Foundation.h>
#import <CoreVideo/CoreVideo.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include "apple_engine.h"
#include "apple_metal.h"
#include "apple_nn.h"

using namespace pmvs::apple;

static double NowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

static CVPixelBufferRef MakeBuf(int w, int h, OSType f)
{
    NSDictionary* a = @{ (__bridge NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{}, (__bridge NSString*)kCVPixelBufferMetalCompatibilityKey: @YES };
    CVPixelBufferRef b = nullptr;
    CVPixelBufferCreate(kCFAllocatorDefault, size_t(w), size_t(h), f, (__bridge CFDictionaryRef)a, &b);
    return b;
}

static int Parity(const std::string& dir)
{
    std::string err;
    Metal* mt = Metal::Shared(err);
    if (!mt) { std::printf("no Metal: %s\n", err.c_str()); return 2; }
    std::printf("Metal device: %s\n", mt->DeviceName().c_str());
    int fail = 0;
    // ArtCNN: a textured synthetic luma picture (gradients, edges, noise), Metal against the float reference
    const int w = 160, h = 96;
    std::vector<float> in(size_t(w * h));
    std::mt19937 rng(7);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        float v = 0.2f + 0.5f * float(x) / w + ((x / 12 + y / 12) % 2 ? 0.15f : 0.f) + 0.03f * float(rng() % 100) / 100.f;
        in[size_t(y * w + x)] = std::min(1.f, v);
    }
    for (const char* nm : { "ArtCNN_C4F16", "ArtCNN_C4F32" }) {
        NnModel m;
        if (!m.Load(dir + "/" + nm + ".pmnn", err)) { std::printf("%s: %s\n", nm, err.c_str()); ++fail; continue; }
        auto net = mt->LoadNet(m, err);
        if (!net) { std::printf("%s: %s\n", nm, err.c_str()); ++fail; continue; }
        std::vector<float> ref(size_t(4 * w * h)), gpu(ref.size());
        NnReference(m, in.data(), w, h, ref.data());
        const double t0 = NowMs();
        if (!mt->RunArtCnnFloat(*net, in.data(), w, h, gpu.data(), err)) { std::printf("%s: Metal: %s\n", nm, err.c_str()); ++fail; continue; }
        const double ms = NowMs() - t0;
        const double ps = PsnrUnit(ref.data(), gpu.data(), ref.size(), 8, 2 * w, 2 * h);
        std::printf("%s: %zu parameters, Metal vs float reference PSNR %.2f dB (%.1f ms incl. setup)\n", nm, m.Parameters(), ps, ms);
        if (ps < 40.0) ++fail;
    }
    // 10-bit recovery: a gradient pan; Metal kernel against RecoverTenBitPlane (luma), zero flow and a constant flow
    {
        const int rw = 128, rh = 32;
        CVPixelBufferRef a = MakeBuf(rw, rh, 'x420'), b = MakeBuf(rw, rh, 'x420'), g = MakeBuf(rw, rh, '420v'), o = MakeBuf(rw, rh, 'x420'), fl = MakeBuf(rw, rh, kCVPixelFormatType_TwoComponent16Half);
        if (!a || !b || !g || !o || !fl) { std::printf("recovery: buffers\n"); return 1; }
        Plane16 A, B, G; A.w = B.w = G.w = rw; A.h = B.h = G.h = rh;
        A.v.resize(size_t(rw * rh)); B.v.resize(A.v.size()); G.v.resize(A.v.size());
        auto fill16 = [&](CVPixelBufferRef pb, Plane16& p, double shift) {
            CVPixelBufferLockBaseAddress(pb, 0);
            for (int pl = 0; pl < 2; ++pl) {
                uint8_t* base = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pb, size_t(pl)));
                const size_t st = CVPixelBufferGetBytesPerRowOfPlane(pb, size_t(pl));
                const int ph = int(CVPixelBufferGetHeightOfPlane(pb, size_t(pl))), pw = int(CVPixelBufferGetWidthOfPlane(pb, size_t(pl))) * (pl ? 2 : 1);
                for (int y = 0; y < ph; ++y) for (int x = 0; x < pw; ++x) {
                    const int code = pl ? 512 : int(std::lround(300.0 + 60.0 * std::clamp(double(x) - shift, 0.0, double(rw - 1)) / double(rw - 1)));
                    if (!pl) p.v[size_t(y * rw + x)] = uint16_t(code);
                    const uint16_t u = uint16_t(code << 6); std::memcpy(base + y * st + 2 * x, &u, 2);
                }
            }
            CVPixelBufferUnlockBaseAddress(pb, 0);
        };
        fill16(a, A, 0); fill16(b, B, 3.0);
        CVPixelBufferLockBaseAddress(g, 0);
        for (int pl = 0; pl < 2; ++pl) {
            uint8_t* base = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(g, size_t(pl)));
            const size_t st = CVPixelBufferGetBytesPerRowOfPlane(g, size_t(pl));
            const int ph = int(CVPixelBufferGetHeightOfPlane(g, size_t(pl))), pw = int(CVPixelBufferGetWidthOfPlane(g, size_t(pl))) * (pl ? 2 : 1);
            for (int y = 0; y < ph; ++y) for (int x = 0; x < pw; ++x) {
                const int c8 = pl ? 128 : To8(int(std::lround(300.0 + 60.0 * std::clamp(double(x) - 1.5, 0.0, double(rw - 1)) / double(rw - 1))));
                if (!pl) G.v[size_t(y * rw + x)] = uint16_t(c8);
                base[y * st + x] = uint8_t(c8);
            }
        }
        CVPixelBufferUnlockBaseAddress(g, 0);
        CVPixelBufferLockBaseAddress(fl, 0);
        {
            uint8_t* base = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(fl));
            const size_t st = CVPixelBufferGetBytesPerRow(fl);
            const __fp16 fx = 3.0, fy = 0.0;
            for (int y = 0; y < rh; ++y) for (int x = 0; x < rw; ++x) { std::memcpy(base + y * st + 4 * x, &fx, 2); std::memcpy(base + y * st + 4 * x + 2, &fy, 2); }
        }
        CVPixelBufferUnlockBaseAddress(fl, 0);
        for (int useFlow = 0; useFlow < 2; ++useFlow) {
            void* cmd = mt->NewCommandBuffer();
            bool ok = mt->EncodeRecover(cmd, a, b, g, useFlow ? fl : nullptr, 0.5f, o, err);
            ok = mt->Commit(cmd, true, err) && ok;
            if (!ok) { std::printf("recovery: Metal: %s\n", err.c_str()); ++fail; continue; }
            std::vector<float> fxv(useFlow ? size_t(rw * rh) : 0, 3.0f), fyv(useFlow ? size_t(rw * rh) : 0, 0.0f);
            Plane16 R;
            RecoverTenBitPlane(A, B, G, fxv, fyv, 0.5f, R);
            CVPixelBufferLockBaseAddress(o, kCVPixelBufferLock_ReadOnly);
            const uint8_t* base = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(o, 0));
            const size_t st = CVPixelBufferGetBytesPerRowOfPlane(o, 0);
            int maxd = 0;
            for (int y = 0; y < rh; ++y) for (int x = 0; x < rw; ++x) { uint16_t u; std::memcpy(&u, base + y * st + 2 * x, 2); maxd = std::max(maxd, std::abs(int(u >> 6) - int(R.v[size_t(y * rw + x)]))); }
            CVPixelBufferUnlockBaseAddress(o, kCVPixelBufferLock_ReadOnly);
            std::printf("10-bit recovery (%s flow): Metal vs reference, max difference %d codes\n", useFlow ? "constant" : "no", maxd);
            if (maxd > 1) ++fail;
        }
        for (CVPixelBufferRef p : { a, b, g, o, fl }) CVPixelBufferRelease(p);
    }
    std::printf("parity: %s\n", fail ? "FAILED" : "ok");
    return fail ? 1 : 0;
}

static int Bench(int w, int h, int k, double fps)
{
    Engine e;
    e.log = [](const std::string& m) { std::printf("  log: %s\n", m.c_str()); };
    Config c; c.width = w; c.height = h; c.bits = 8; c.k = k; c.srcFps = fps; c.sr = std::getenv("PUSHMPC_BENCH_SR") ? std::getenv("PUSHMPC_BENCH_SR") : "auto";
    std::string err, info;
    if (!e.Configure(c, err, info)) { std::printf("not available: %s\n", err.c_str()); return 2; }
    const Stats& b = e.Stat();
    std::printf("chain: %s\nbench: interpolation %.2f ms, super resolution %.2f ms a frame, optical flow %.2f ms\n", info.c_str(), b.benchFiMs, b.benchSrMs, b.benchFlowMs);
    const int ow = e.OutWidth(), oh = e.OutHeight();
    std::vector<uint8_t> src[3], outs[8][3];
    PlanarView v; v.width = w; v.height = h; v.bits = 8;
    for (int p = 0; p < 3; ++p) { const int pw = p ? (w + 1) / 2 : w, ph = p ? (h + 1) / 2 : h; src[p].assign(size_t(pw * ph), 128); v.data[p] = src[p].data(); v.stride[p] = pw; }
    std::vector<PlanarOut> gen(size_t(std::max(0, k - 1)));
    PlanarOut cur;
    for (int j = 0; j < k; ++j)
        for (int p = 0; p < 3; ++p) {
            const int pw = p ? (ow + 1) / 2 : ow, ph = p ? (oh + 1) / 2 : oh;
            outs[j][p].assign(size_t(pw * ph), 0);
            PlanarOut& o = j < k - 1 ? gen[size_t(j)] : cur;
            o.data[p] = outs[j][p].data(); o.stride[p] = pw;
        }
    const int frames = 120;
    double total = 0;
    for (int f = 0; f < frames; ++f) {
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) src[0][size_t(y * w + x)] = uint8_t(((x + 6 * f) / 16 + y / 16) % 2 ? 200 : 40);
        bool hp = false;
        const double t0 = NowMs();
        if (!e.Process(v, Colour(), int64_t(f) * int64_t(1e7 / fps), true, gen, cur, hp, err)) { std::printf("frame %d: %s\n", f, err.c_str()); return 1; }
        total += NowMs() - t0;
    }
    const Stats& s = e.Stat();
    std::printf("bench %dx%d x%d at %.3f fps: %.2f ms per source frame (budget %.2f ms), %llu VideoToolbox calls, %llu failed, ladder %d; %s\n", w, h, k, fps, total / frames,
                1000.0 / fps * 0.85, (unsigned long long)s.vtCalls, (unsigned long long)s.vtErrors, s.ladder, e.Describe().c_str());
    std::printf("BENCH_RESULT w=%d h=%d k=%d ms=%.3f ladder=%d\n", w, h, k, total / frames, s.ladder);
    return 0;
}

int main(int argc, char** argv)
{
    @autoreleasepool {
        const std::string cmd = argc > 1 ? argv[1] : "probe";
        auto say = [](const std::string& m) { std::printf("%s\n", m.c_str()); std::fflush(stdout); };
        if (cmd == "probe") { std::printf("%s", ProbeReport().c_str()); return 0; }
        if (cmd == "model-status") {
            float pct = 0;
            const int s = SrModelStatus(1920, 1080, &pct);
            std::printf("%s (%.0f %%)\n", s == 2 ? "ready" : s == 1 ? "downloading" : s == 0 ? "download required" : "n/a", pct * 100.0);
            return s < 0 ? 2 : 0;
        }
        if (cmd == "model-download") {
            std::string err;
            if (SrModelStatus() < 0) { say("VideoToolbox super resolution is not available on this Mac"); return 2; }
            if (!SrModelDownload(argc > 2 ? std::atof(argv[2]) : 600.0, say, err)) { say("model download: " + err); return 1; }
            return 0;
        }
        if (cmd == "parity") return Parity(argc > 2 ? argv[2] : ".");
        if (cmd == "bench") return Bench(argc > 2 ? std::atoi(argv[2]) : 1920, argc > 3 ? std::atoi(argv[3]) : 1080, argc > 4 ? std::atoi(argv[4]) : 2, argc > 5 ? std::atof(argv[5]) : 23.976);
        std::fprintf(stderr, "usage: pushmpc-apple probe | model-status | model-download [seconds] | parity <models dir> | bench [w h k fps]\n");
        return 2;
    }
}
