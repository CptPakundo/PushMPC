// nvl_test: the Linux NVIDIA backend's pure parts, on every OS (no GPU, no CUDA, no VapourSynth):
//  - colour: YUV <-> RGB matrices, ranges, bit depths, packing, the chroma siting, the cut guard's luma;
//  - the cut guard (the CPU port of PushMPC's GPU pair test) on synthetic sequences: pans, held frames, hard cuts, a dark
//    cut, letterbox bars;
//  - Auto: the multiplier, VFG quality and VSR level against the Windows rules' known results (RTX 5090: 5x at 240 Hz),
//    the GPU classes, the RTX 30 gate, RTX Video HDR;
//  - the SM89 -> SM86 fatbin retarget on synthetic fatbins (accepted, rewritten, rejected);
//  - SHA-256 (FIPS 180-4 vectors); the settings words.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "nvl_auto.h"
#include "nvl_colour.h"
#include "nvl_cutguard.h"
#include "nvl_fatbin.h"
#include "nvl_settings.h"
#include "nvl_sha256.h"
#include "backend.h"

using namespace pmvs::nvl;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do { if (cond) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s | ", __FILE__, __LINE__, #cond); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static void TestColour()
{
    const Coeffs k709 = MatrixCoeffs(1), k601 = MatrixCoeffs(6), k2020 = MatrixCoeffs(9);
    CHECK(std::fabs(k709.kr - 0.2126f) < 1e-6 && std::fabs(k601.kr - 0.299f) < 1e-6 && std::fabs(k2020.kb - 0.0593f) < 1e-6, "matrix codes");
    InFormat f8; f8.bits = 8; f8.full = false; f8.matrix = 1;
    float Y, Cb, Cr, r, g, b;
    Normalise(f8, 16, 128, 128, Y, Cb, Cr); YuvToRgb(k709, Y, Cb, Cr, r, g, b);
    CHECK(r == 0 && g == 0 && b == 0, "limited black -> 0 (%f %f %f)", r, g, b);
    Normalise(f8, 235, 128, 128, Y, Cb, Cr); YuvToRgb(k709, Y, Cb, Cr, r, g, b);
    CHECK(std::fabs(r - 1) < 1e-5 && std::fabs(g - 1) < 1e-5 && std::fabs(b - 1) < 1e-5, "limited white -> 1");
    InFormat f10 = f8; f10.bits = 10;
    Normalise(f10, 940, 512, 512, Y, Cb, Cr);
    CHECK(std::fabs(Y - 1) < 1e-5 && std::fabs(Cb) < 1e-6, "10-bit limited white");
    InFormat ff = f8; ff.full = true;
    Normalise(ff, 255, 128, 128, Y, Cb, Cr);
    CHECK(std::fabs(Y - 1) < 1e-6 && std::fabs(Cb) < 1e-6, "full range");
    // round trips through every matrix at 8 and 10 bits: RGB -> YUV codes -> RGB within one step
    int worst8 = 0, worst10 = 0;
    for (int m : { 1, 6, 9 })
        for (int rr = 0; rr <= 255; rr += 17)
            for (int gg = 0; gg <= 255; gg += 51)
                for (int bb = 0; bb <= 255; bb += 85) {
                    const Coeffs k = MatrixCoeffs(m);
                    float y, cb, cr; RgbToYuv(k, rr / 255.f, gg / 255.f, bb / 255.f, y, cb, cr);
                    for (int bits : { 8, 10 }) {
                        InFormat f; f.bits = bits; f.matrix = m;
                        float Y2, Cb2, Cr2, r2, g2, b2;
                        Normalise(f, float(CodeY(y, bits)), float(CodeC(cb, bits)), float(CodeC(cr, bits)), Y2, Cb2, Cr2);
                        YuvToRgb(k, Y2, Cb2, Cr2, r2, g2, b2);
                        const uint32_t p = bits > 8 ? PackRgb10a2(r2, g2, b2) : PackRgba8(r2, g2, b2);
                        float r3, g3, b3; Unpack(p, bits > 8, r3, g3, b3);
                        const int s = bits > 8 ? 1023 : 255;
                        const int e = std::max({ std::abs(int(std::lround(r3 * s)) - int(std::lround(rr / 255.f * s))), std::abs(int(std::lround(g3 * s)) - int(std::lround(gg / 255.f * s))),
                                                 std::abs(int(std::lround(b3 * s)) - int(std::lround(bb / 255.f * s))) });
                        (bits > 8 ? worst10 : worst8) = std::max(bits > 8 ? worst10 : worst8, e);
                    }
                }
    CHECK(worst8 <= 2, "8-bit round trip within 2 codes (worst %d)", worst8);
    CHECK(worst10 <= 4, "10-bit round trip within 4 codes (worst %d)", worst10);
    // packing layouts (NvCVImage RGBA U8 chunky: bytes R, G, B, A; RGB10A2: r | g << 10 | b << 20 | a << 30)
    CHECK(PackRgba8(1, 0, 0) == 0xFF0000FFu && PackRgba8(0, 1, 0) == 0xFF00FF00u && PackRgba8(0, 0, 1) == 0xFFFF0000u, "RGBA8 layout");
    CHECK(PackRgb10a2(1, 0, 0) == 0xC00003FFu && PackRgb10a2(0, 0, 1) == (0xC0000000u | (1023u << 20)), "RGB10A2 layout");
    // left-sited 4:2:0: luma (2c, y) sits on chroma column c; luma rows 0/1 straddle chroma row 0
    ChromaTap t = ChromaAt(4, 2, 8, 8);
    CHECK(t.x0 == 2 && t.fx == 0.f && t.y0 == 0 && t.y1 == 1 && std::fabs(t.fy - 0.75f) < 1e-6, "chroma tap (4,2): x0 %d fx %f y0 %d fy %f", t.x0, t.fx, t.y0, t.fy);
    t = ChromaAt(0, 0, 8, 8);
    CHECK(t.x0 == 0 && t.y0 == 0 && t.y1 == 0, "chroma clamped at the edge");
    // a whole pixel through PixelToPacked: a flat red field
    InFormat fr; fr.bits = 8; fr.matrix = 1;
    float y, cb, cr; RgbToYuv(k709, 1, 0, 0, y, cb, cr);
    const uint32_t Yc = CodeY(y, 8), Uc = CodeC(cb, 8), Vc = CodeC(cr, 8);
    auto read = [&](int plane, int, int) -> uint32_t { return plane == 0 ? Yc : plane == 1 ? Uc : Vc; };
    const uint32_t px = PixelToPacked(fr, k709, 3, 3, 4, 4, false, read);
    CHECK((px & 0xFF) >= 253 && ((px >> 8) & 0xFF) <= 2 && ((px >> 16) & 0xFF) <= 2, "red pixel 0x%08X", px);
    CHECK(GuardLuma(16, 8, false) == 0 && GuardLuma(235, 8, false) == 255 && GuardLuma(940, 10, false) == 255 && GuardLuma(128, 8, true) == 128, "guard luma scale");
}

// synthetic luma scenes (0..255) for the cut guard, 960 x 540
struct Scene { int kind; double dx; double gain; int bars; };
static uint32_t LumaAt(const Scene& s, int x, int y)
{
    if (s.bars && (y < s.bars || y >= 540 - s.bars)) return 2;
    const double X = x + s.dx;
    double v;
    if (s.kind == 0) v = 110 + 50 * std::sin(X * 0.031) * std::cos(y * 0.043) + 20 * std::sin((X + y) * 0.11);   // landscape
    else if (s.kind == 1) v = 60 + 150 * ((int(X / 37) + int(y / 29)) & 1) + 10 * std::sin(y * 0.2);                 // checkerboard room
    else if (s.kind == 2) v = 18 + 14 * std::sin(X * 0.05) + 10 * std::cos(y * 0.07);                                   // dark street
    else v = 20 + 22 * ((int(X / 53) + int(y / 41)) & 1) + 4 * std::sin(X * 0.3);                                       // dark interior
    v *= s.gain;
    return uint32_t(std::clamp(v, 0.0, 255.0));
}

static void TestCutGuard()
{
    CutGuard g; g.Configure(960, 540);
    int cuts = 0;
    // 30 frames of a pan (3 px a frame): no cut
    for (int i = 0; i < 30; ++i) { Scene s{ 0, 3.0 * i, 1.0, 0 }; cuts += g.Push([&](int x, int y) { return LumaAt(s, x, y); }, true).cut; }
    CHECK(cuts == 0, "pan: %d cuts", cuts);
    // a held frame (animation on twos): hold, no cut
    { Scene s{ 0, 87.0, 1.0, 0 }; CutDecision d = g.Push([&](int x, int y) { return LumaAt(s, x, y); }, true); CHECK(d.hold && !d.cut, "held pair: hold %d cut %d sad %f", d.hold, d.cut, d.m.sad); }
    // a hard cut to another scene
    { Scene s{ 1, 0, 1.0, 0 }; CutDecision d = g.Push([&](int x, int y) { return LumaAt(s, x, y); }, true);
      CHECK(d.cut, "hard cut: sad %.3f hist %.3f hist16 %.3f mrSad %.3f", d.m.sad, d.m.histogram, d.m.histogram16, d.m.mrSad); }
    // the new scene pans: no cut
    cuts = 0;
    for (int i = 1; i < 10; ++i) { Scene s{ 1, 2.0 * i, 1.0, 0 }; cuts += g.Push([&](int x, int y) { return LumaAt(s, x, y); }, true).cut; }
    CHECK(cuts == 0, "second pan: %d cuts", cuts);
    // a seek is never a cut
    { Scene s{ 0, 500, 1.0, 0 }; CutDecision d = g.Push([&](int x, int y) { return LumaAt(s, x, y); }, false); CHECK(!d.compared && !d.cut, "seek"); }
    // dark scenes: a slow pan, then a cut to another dark scene
    CutGuard dk; dk.Configure(960, 540);
    cuts = 0;
    for (int i = 0; i < 12; ++i) { Scene s{ 2, 1.0 * i, 1.0, 0 }; cuts += dk.Push([&](int x, int y) { return LumaAt(s, x, y); }, true).cut; }
    CHECK(cuts == 0, "dark pan: %d cuts", cuts);
    { Scene s{ 3, 0, 1.0, 0 }; CutDecision d = dk.Push([&](int x, int y) { return LumaAt(s, x, y); }, true);
      CHECK(d.cut, "dark cut: mrSadN %.3f histN16 %.3f mrSad %.3f hist16 %.3f dark %d adaptive %d", d.m.mrSadN, d.m.histogramN16, d.m.mrSad, d.m.histogram16, d.darkRule, d.adaptiveRule); }
    // letterbox bars (12 % top and bottom) are found after the first frames and leave the measures
    CutGuard lb; lb.Configure(960, 540);
    for (int i = 0; i < 50; ++i) { Scene s{ 0, 2.0 * i, 1.0, 66 }; lb.Push([&](int x, int y) { return LumaAt(s, x, y); }, true); }
    CHECK(lb.Area().y0 >= 66 && lb.Area().y1 <= 540 - 66 && lb.Area().x0 == 0 && lb.Area().x1 == 960, "letterbox area %d..%d x %d..%d", lb.Area().y0, lb.Area().y1, lb.Area().x0, lb.Area().x1);
    // measures: identical grids
    std::array<uint8_t, kGridN> a{}; for (int i = 0; i < kGridN; ++i) a[size_t(i)] = uint8_t(i * 7);
    const CutMeasures m = CutGuard::Measure(a, a);
    CHECK(m.sad == 0 && m.histogram == 0 && m.mrSad == 0, "identical frames measure zero");
}

static void TestAuto()
{
    CHECK(ClassOf(12, 0, "NVIDIA GeForce RTX 5090") == GpuClass::Rtx50 && ClassOf(8, 9, "RTX 4070") == GpuClass::Rtx40 && ClassOf(8, 6, "RTX 3080") == GpuClass::Rtx30
          && ClassOf(7, 5, "NVIDIA GeForce RTX 2070") == GpuClass::Rtx20 && ClassOf(7, 5, "NVIDIA GeForce GTX 1660 SUPER") == GpuClass::Gtx16 && ClassOf(9, 0, "H100") == GpuClass::Datacenter, "classes");
    CHECK(VfgNative(8, 9) && VfgNative(12, 0) && VfgNative(9, 0) && !VfgNative(8, 6) && VfgAmpere(8, 6) && !VfgNative(7, 5), "VFG architectures");
    Caps c5090; c5090.gpu = GpuClass::Rtx50; c5090.vsr = c5090.hdr = c5090.vfg = true;
    Costs costs; costs.card = CardFactor("NVIDIA GeForce RTX 5090", GpuClass::Rtx50);
    Settings s; Source src; src.w = 1920; src.h = 1080; src.fps = 24000.0 / 1001;
    Display d; d.refreshHz = 240; d.fullscreen = true;   // mpv full screen (display_fs); VRR is never known here: fixed refresh
    Plan p = Decide(s, src, d, c5090, costs);
    CHECK(p.k == 10 && p.vfgQuality == 1 && p.vsr && p.vsrLevel >= 1 && p.outW == 3840 && p.outH == 2160 && !p.hdr, "5090 1080p24 at 240 Hz: x10 (one new frame per refresh): %s", p.why.c_str());
    {   // the player's share per shown frame: a window's composition (10 x 0.33 ms more at 4K) leaves no 1 ms spare for x10 at a
        // fixed 240 Hz: x5, the even cadence; an HDR source on an SDR display adds mpv's tone map per shown frame
        Display w = d; w.fullscreen = false;
        const Plan pw = Decide(s, src, w, c5090, costs);
        CHECK(pw.k == 5 && pw.why.find("spare") != std::string::npos, "windowed 240 Hz at the budget edge: x5 (even cadence): %s", pw.why.c_str());
        Source hs = src; hs.hdr = true;
        Settings f5 = s; f5.smoother = 5;
        const Plan sdr5 = Decide(f5, src, d, c5090, costs), hdr5 = Decide(f5, hs, d, c5090, costs);
        CHECK(hdr5.predictedMs > sdr5.predictedMs + 5 * 0.38 * 0.99 - 0.01 || hdr5.vsrLevel < sdr5.vsrLevel, "an HDR source on an SDR display: mpv's tone map counts per shown frame: %.2f vs %.2f ms", hdr5.predictedMs, sdr5.predictedMs);
    }
    { Source s15 = src; s15.fps = 15.0; const Plan p15 = Decide(s, s15, d, c5090, costs); CHECK(p15.k == 16, "15 fps at 240 Hz: x16 VFG (timestep mode above x8): %s", p15.why.c_str()); }
    { Settings f16 = s; f16.smoother = 16; const Plan pf = Decide(f16, src, d, c5090, costs); CHECK(pf.k == 16, "Smoother 16 is honoured: %s", pf.why.c_str()); }
    d.refreshHz = 0;
    p = Decide(s, src, d, c5090, costs);
    CHECK(p.k == 2, "unknown display -> x2: %s", p.why.c_str());
    d.refreshHz = 60;
    p = Decide(s, src, d, c5090, costs);
    CHECK(p.k == 2 && p.vfgQuality == 2, "60 Hz: x2 High (cheap enough with 3 ms to spare): %s", p.why.c_str());
    d.refreshHz = 120; d.hdr = true;
    p = Decide(s, src, d, c5090, costs);
    CHECK(p.hdr && p.k == 5, "HDR output: RTX Video HDR on, 120 Hz x5: %s", p.why.c_str());
    src.hdr = true;
    p = Decide(s, src, d, c5090, costs);
    CHECK(!p.hdr, "HDR source: no RTX Video HDR");
    src.hdr = false; d.hdr = false;
    Source uhd = src; uhd.w = 3840; uhd.h = 2160;
    p = Decide(s, uhd, d, c5090, costs);
    CHECK(!p.vsr && p.outW == 3840 && p.k >= 2, "4K source: no VSR: %s", p.why.c_str());
    Caps c3080; c3080.gpu = GpuClass::Rtx30; c3080.vsr = c3080.hdr = true; c3080.vfg = false; c3080.vfgWhy = "self-test not passed";
    Costs c30; c30.card = CardFactor("GeForce RTX 3080", GpuClass::Rtx30);
    p = Decide(s, src, d, c3080, c30);
    CHECK(p.k == 1 && p.interpElsewhere && p.vsr, "RTX 30 before its self-test: upscale only: %s", p.why.c_str());
    c3080.vfg = true;
    p = Decide(s, src, d, c3080, c30);
    CHECK(p.k >= 2, "RTX 30 after its self-test: VFG: %s", p.why.c_str());
    Settings off = s; off.smoother = 1;
    p = Decide(off, src, d, c5090, costs);
    CHECK(p.k == 1 && !p.interpElsewhere && p.vsr, "Smoother off");
    Settings fix = s; fix.smoother = 3; fix.sharper = 2; fix.fgQuality = 0;
    p = Decide(fix, src, d, c5090, costs);
    CHECK(p.k == 3 && p.vsrLevel == 2 && p.vfgQuality == 0 && p.vsrQuality == 2, "fixed choices kept: %s", p.why.c_str());
    Source hb = src; hb.bpp = 0.5;
    p = Decide(s, hb, d, c5090, costs);
    CHECK(p.vsrStyle == 1 && p.vsrQuality >= 16 && p.vsrQuality <= 19, "high bitrate style: %d", p.vsrQuality);
    CHECK(TargetForDisplay(1440) == 1440 && TargetForDisplay(1080) == 1440 && TargetForDisplay(2160) == 2160 && TargetForDisplay(2880) == 2880 && TargetForDisplay(0) == 2160, "display height -> upscale target (Windows rule)");
    CHECK(VsrQualityLevel(2, 1) == 21 && VsrQualityLevel(2, 4) == 23 && VsrQualityLevel(0, 3) == 3, "VSR quality levels");
    CHECK(CadenceEven(240, 120) && !CadenceEven(240, 143.86) && std::fabs(HoldMs(240, false, 143.86) - 8.333) < 0.01, "cadence");
}

// a synthetic fatbin: header + entries (kind, sm, ELF payload with e_flags)
static std::vector<uint8_t> Fatbin(const std::vector<std::pair<int, uint32_t>>& entries, bool badMachine = false)
{
    std::vector<uint8_t> b(16, 0);
    auto put32 = [](std::vector<uint8_t>& v, size_t p, uint32_t x) { std::memcpy(v.data() + p, &x, 4); };
    auto put16 = [](std::vector<uint8_t>& v, size_t p, uint16_t x) { std::memcpy(v.data() + p, &x, 2); };
    for (auto [sm, flags] : entries) {
        const size_t p = b.size();
        b.resize(p + 64 + 128, 0);
        put16(b, p, 2); put16(b, p + 2, 0x101); put32(b, p + 4, 64);
        const uint64_t bytes = 128; std::memcpy(b.data() + p + 8, &bytes, 8);
        put32(b, p + 28, uint32_t(sm));
        const size_t e = p + 64;
        put32(b, e, 0x464c457f); b[e + 4] = 2; b[e + 5] = 1; put16(b, e + 18, badMachine ? 62 : 190); put32(b, e + 48, flags);
    }
    put32(b, 0, kFatbinMagic); put16(b, 4, 1); put16(b, 6, 16);
    const uint64_t payload = b.size() - 16; std::memcpy(b.data() + 8, &payload, 8);
    return b;
}

static void TestFatbin()
{
    auto in = Fatbin({ { 89, 0x00590559 }, { 90, 0x005a055a }, { 120, 0x00780578 } });
    CHECK(FatbinSize(in.data()) == in.size(), "fatbin size from the header");
    std::vector<uint8_t> out; FatbinChanges ch; std::string why;
    CHECK(RetargetVfgFatbin(in.data(), in.size(), out, ch, why) && ch.containers == 1 && ch.entries == 3, "retarget: %s", why.c_str());
    uint32_t sm = 0, flags = 0; std::memcpy(&sm, out.data() + 16 + 28, 4); std::memcpy(&flags, out.data() + 16 + 64 + 48, 4);
    CHECK(sm == 86 && flags == 0x00560556, "SM89 entry rewritten to SM86 (%u, 0x%08X)", sm, flags);
    std::memcpy(&sm, in.data() + 16 + 28, 4);
    CHECK(sm == 89, "the input is never written");
    CHECK(std::memcmp(out.data() + 16 + 192, in.data() + 16 + 192, in.size() - 16 - 192) == 0, "other entries unchanged");
    auto odd = Fatbin({ { 89, 0x00590000 } });
    CHECK(!RetargetVfgFatbin(odd.data(), odd.size(), out, ch, why) && why.find("SM89") != std::string::npos, "unknown SM89 layout rejected");
    auto mach = Fatbin({ { 89, 0x00590559 } }, true);
    CHECK(!RetargetVfgFatbin(mach.data(), mach.size(), out, ch, why), "non-CUDA ELF machine rejected");
    auto trunc = in; trunc.resize(trunc.size() - 10);
    CHECK(!RetargetVfgFatbin(trunc.data(), trunc.size(), out, ch, why), "truncated fatbin rejected");
    auto none = Fatbin({ { 90, 0x005a055a } });
    CHECK(RetargetVfgFatbin(none.data(), none.size(), out, ch, why) && ch.containers == 0, "no SM89 entry: unchanged");
    CHECK(VfgReportedAttribute(6, 76, 0, 0, 0) == 9 && VfgReportedAttribute(6, 75, 0, 0, 0) == 6 && VfgReportedAttribute(6, 76, 1, 0, 0) == 6 && VfgReportedAttribute(9, 76, 0, 0, 0) == 9, "attribute spoof");
}

static void TestSha()
{
    Sha256 a; CHECK(a.HexUpper() == "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855", "empty");
    Sha256 b; b.Update("abc", 3); CHECK(b.HexUpper() == "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD", "abc");
    Sha256 c; const char* m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"; c.Update(m, std::strlen(m));
    CHECK(c.HexUpper() == "248D6A61D20638B8E5C026930C3E6039A33CE45964FF2167F6ECEDD419DB06C1", "448-bit");
    Sha256 d; std::string mil(1000000, 'a'); d.Update(mil.data(), 1000); d.Update(mil.data() + 1000, mil.size() - 1000);
    CHECK(d.HexUpper() == "CDC76E5C9914FB9281A1C7E284D73E67F1809A48A497200E046D39CCC7112CD0", "a million a");
}

static void TestSettings()
{
    auto get = [](const char* n) -> std::string {
        const std::string k = n;
        return k == "Smoother" ? "4" : k == "Sharper" ? "Off" : k == "FgQuality" ? "High" : k == "SdrToHdr" ? "On" : k == "VsrStyle" ? "Streaming" : k == "UpscaleTarget" ? "1440p" : k == "DisplayPeak" ? "800" : "";
    };
    Settings s = LoadSettings(get);
    CHECK(s.enable && s.smoother == 4 && s.sharper == -1 && s.fgQuality == 2 && s.sdrToHdr == 1 && s.vsrStyle == 2 && s.targetH == 1440 && s.hdrPeak == 800, "settings.ini words");
    ApplyArg(s, "smoother", "off"); ApplyArg(s, "sharper", "3"); ApplyArg(s, "fg_quality", "auto");
    CHECK(s.smoother == 1 && s.sharper == 3 && s.fgQuality == -1, "arguments override");
    Settings d = LoadSettings([](const char*) { return std::string(); });
    CHECK(d.smoother == 0 && d.sharper == 0 && d.fgQuality == -1 && d.sdrToHdr == 0 && d.targetH == 2160, "defaults are Auto");
    CHECK(d.targetScreen && !d.hdrPeakSet && !s.targetScreen && s.hdrPeakSet, "Screen target and the peak: the display decides unless set");
}

// the optical-flow tier, the guard's flow, the interpolator choice without VFG, the flow arithmetic
static void TestFlowTier()
{
    Caps c20; c20.gpu = GpuClass::Rtx20; c20.vsr = c20.hdr = true; c20.vfg = false; c20.nvof = true; c20.vfgWhy = "Turing";
    Costs k20; k20.card = CardFactor("GeForce RTX 2070", GpuClass::Rtx20);
    Settings s; Source src; src.w = 1920; src.h = 1080; src.fps = 24000.0 / 1001;
    Display d; d.refreshHz = 144;
    Plan p = Decide(s, src, d, c20, k20);
    CHECK(p.nvof && p.k >= 2 && !p.interpElsewhere && p.guardFlow, "RTX 20: the optical-flow tier generates: %s", p.why.c_str());
    k20.flowMeasured = 40; k20.warpMeasured = 5;   // a slow measurement: the budget limits the multiplier
    Plan slow = Decide(s, src, d, c20, k20);
    CHECK(slow.k < p.k, "measured costs decide (x%d -> x%d): %s", p.k, slow.k, slow.why.c_str());
    Caps none = c20; none.nvof = false;
    p = Decide(s, src, d, none, Costs());
    CHECK(p.k == 1 && p.interpElsewhere && !p.nvof, "no VFG, no NVOFA: interpolation elsewhere");
    Caps c50; c50.gpu = GpuClass::Rtx50; c50.vsr = c50.hdr = c50.vfg = c50.nvof = true;
    Settings of = s; of.fgMethod = 1;
    p = Decide(of, src, d, c50, Costs());
    CHECK(p.nvof && p.k >= 2, "optical flow asked for on an RTX 50: %s", p.why.c_str());
    p = Decide(s, src, d, c50, Costs());
    CHECK(!p.nvof && p.k >= 2 && p.guardFlow, "RTX 50: VFG, with the guard's flow when it fits: %s", p.why.c_str());
    // PickInterp
    pmvs::BackendProbe b; b.nvidiaBuilt = b.pushcoreLoaded = true; b.nvidiaNoFg = true; b.gpuFamily = 4;
    std::string why;
    b.mlBuilt = b.mlAvailable = true; b.mlK = 3; b.nvidiaNvof = true; b.nvofK = 3;
    CHECK(pmvs::PickInterp(b, &why) == "ml", "ml measured as high as the optical flow: ml (%s)", why.c_str());
    b.nvofK = 5;
    CHECK(pmvs::PickInterp(b, &why) == "", "the optical flow reaches more: the nvidia backend generates (%s)", why.c_str());
    b.mlK = -1; b.srcHeight = 2160; b.nvofK = 2;
    CHECK(pmvs::PickInterp(b, &why) == "", "4K, ml not measured: optical flow (%s)", why.c_str());
    b.srcHeight = 1080;
    CHECK(pmvs::PickInterp(b, &why) == "ml", "1080p, ml not measured: ml (%s)", why.c_str());
    b.mlAvailable = false; b.nvidiaNvof = false; b.rifeAvailable = true;
    CHECK(pmvs::PickInterp(b, &why) == "rife", "then rife");
    b.rifeAvailable = false;
    CHECK(pmvs::PickInterp(b, &why) == "portable", "then portable");
    // flow arithmetic: at t = 0 the sample for A is the pixel itself, at t = 1 B's; a consistent pair weighs by time
    float a0x, a0y, a1x, a1y;
    IntermediateFlows(0.f, 8.f, 2.f, -8.f, -2.f, a0x, a0y, a1x, a1y);
    CHECK(a0x == 0 && a0y == 0 && a1x == 8 && a1y == 2, "t = 0: A in place, B one full vector away");
    IntermediateFlows(0.5f, 8.f, 0.f, -8.f, 0.f, a0x, a0y, a1x, a1y);
    CHECK(std::fabs(a0x + 4.f) < 1e-6 && std::fabs(a1x - 4.f) < 1e-6, "t = 0.5 linear motion: half a vector each way (%f, %f)", a0x, a1x);
    CHECK(RoundTripWeight(0.5f, 0.f) == 0.5f && RoundTripWeight(0.5f, 4.f) < 0.1f, "a vector that does not come back loses its weight");
}

int main()
{
    TestColour();
    TestCutGuard();
    TestAuto();
    TestFatbin();
    TestSha();
    TestSettings();
    TestFlowTier();
    std::printf("nvl_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
