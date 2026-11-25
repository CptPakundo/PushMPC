// pushmpc.Enhance, backend "apple": the VideoToolbox implementation (Objective-C++, ARC). See apple_engine.h.
// Built with any macOS SDK: what the SDK lacks is compiled out (macOS 15.4 SDK: frame-rate conversion; macOS 26 SDK:
// the low-latency interpolator and super resolution), what the running macOS lacks is skipped at run time
// (@available, isSupported, a nil configuration). The macOS 27 size queries are looked up by selector.
#import <Foundation/Foundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>
#if __has_include(<VideoToolbox/VTFrameProcessor.h>)
#import <VideoToolbox/VTFrameProcessor.h>
#define PMVS_VT_FP 1
#endif
#include <objc/message.h>
#include <objc/runtime.h>
#include <sys/sysctl.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>
#include "apple_engine.h"
#include "apple_metal.h"

#if defined(PMVS_VT_FP) && defined(__MAC_15_4)
#define PMVS_VT_FRC 1
#endif
#if defined(PMVS_VT_FP) && defined(__MAC_26_0)
#define PMVS_VT_LL 1
#endif

namespace pmvs::apple {

#ifdef PMVS_VT_FRC

namespace {

std::string Fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string Fmt(const char* f, ...)
{
    char b[1024];
    va_list ap; va_start(ap, f); vsnprintf(b, sizeof(b), f, ap); va_end(ap);
    return b;
}
std::string Str(NSString* s) { return s ? std::string([s UTF8String]) : std::string(); }
std::string ErrText(NSError* e) { return e ? Fmt("%s (%s %ld)", Str(e.localizedDescription).c_str(), Str(e.domain).c_str(), long(e.code)) : std::string("no error object"); }
double NowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// a class-level BOOL property, if the class and the selector exist (no compile-time dependency)
int ClassBool(const char* cls, const char* sel)
{
    Class c = NSClassFromString([NSString stringWithUTF8String:cls]);
    if (!c) return -1;
    SEL s = sel_registerName(sel);
    if (![c respondsToSelector:s]) return -2;
    return ((BOOL(*)(id, SEL))objc_msgSend)(c, s) ? 1 : 0;
}
long ClassIntForInt(const char* cls, const char* sel, long arg)
{
    Class c = NSClassFromString([NSString stringWithUTF8String:cls]);
    SEL s = sel_registerName(sel);
    if (!c || ![c respondsToSelector:s]) return -1;
    return long(((NSInteger(*)(id, SEL, NSInteger))objc_msgSend)(c, s, NSInteger(arg)));
}
long ClassIntForFloat(const char* cls, const char* sel, float arg)
{
    Class c = NSClassFromString([NSString stringWithUTF8String:cls]);
    SEL s = sel_registerName(sel);
    if (!c || ![c respondsToSelector:s]) return -1;
    return long(((NSInteger(*)(id, SEL, float))objc_msgSend)(c, s, arg));
}
std::string Sysctl(const char* name)
{
    char b[256] = {}; size_t n = sizeof(b) - 1;
    return sysctlbyname(name, b, &n, nullptr, 0) == 0 ? std::string(b) : std::string();
}
std::vector<uint32_t> Formats(NSArray<NSNumber*>* a)
{
    std::vector<uint32_t> v;
    for (NSNumber* n in a) v.push_back(uint32_t(n.unsignedIntValue));
    return v;
}
std::string FormatList(const std::vector<uint32_t>& v)
{
    std::string s;
    for (uint32_t f : v) s += (s.empty() ? "" : ",") + FourCCText(f);
    return s.empty() ? "(none)" : s;
}

// VapourSynth colour codes -> CoreVideo attachments
CFStringRef MatrixKey(int m)
{
    switch (m) {
    case 5: case 6: return kCVImageBufferYCbCrMatrix_ITU_R_601_4;
    case 7: return kCVImageBufferYCbCrMatrix_SMPTE_240M_1995;
    case 9: case 10: return kCVImageBufferYCbCrMatrix_ITU_R_2020;
    default: return kCVImageBufferYCbCrMatrix_ITU_R_709_2;
    }
}
CFStringRef TransferKey(int t)
{
    switch (t) {
    case 16: return kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ;
    case 18: return kCVImageBufferTransferFunction_ITU_R_2100_HLG;
    case 7: return kCVImageBufferTransferFunction_SMPTE_240M_1995;
    case 13: return kCVImageBufferTransferFunction_sRGB;
    case 8: return kCVImageBufferTransferFunction_Linear;
    default: return kCVImageBufferTransferFunction_ITU_R_709_2;
    }
}
CFStringRef PrimariesKey(int p)
{
    switch (p) {
    case 5: return kCVImageBufferColorPrimaries_EBU_3213;
    case 6: case 7: return kCVImageBufferColorPrimaries_SMPTE_C;
    case 9: return kCVImageBufferColorPrimaries_ITU_R_2020;
    case 11: return kCVImageBufferColorPrimaries_DCI_P3;
    case 12: return kCVImageBufferColorPrimaries_P3_D65;
    default: return kCVImageBufferColorPrimaries_ITU_R_709_2;
    }
}
void Tag(CVPixelBufferRef b, const Colour& c)
{
    CVBufferSetAttachment(b, kCVImageBufferYCbCrMatrixKey, MatrixKey(c.matrix), kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(b, kCVImageBufferTransferFunctionKey, TransferKey(c.transfer), kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(b, kCVImageBufferColorPrimariesKey, PrimariesKey(c.primaries), kCVAttachmentMode_ShouldPropagate);
}

// a CVPixelBuffer we own (CF retain / release)
struct Buf {
    CVPixelBufferRef ref = nullptr;
    Buf() = default;
    explicit Buf(CVPixelBufferRef r) : ref(r) {}
    Buf(const Buf& o) : ref(o.ref) { if (ref) CVPixelBufferRetain(ref); }
    Buf& operator=(const Buf& o) { if (this != &o) { if (o.ref) CVPixelBufferRetain(o.ref); if (ref) CVPixelBufferRelease(ref); ref = o.ref; } return *this; }
    ~Buf() { if (ref) CVPixelBufferRelease(ref); }
    explicit operator bool() const { return ref != nullptr; }
};

struct Pool {
    CVPixelBufferPoolRef pool = nullptr;
    int w = 0, h = 0; uint32_t fmt = 0;
    ~Pool() { Reset(); }
    void Reset() { if (pool) CVPixelBufferPoolRelease(pool); pool = nullptr; }
    // base: the processor's attribute dictionary (its requirements), then our format, size, IOSurface backing
    bool Make(NSDictionary* base, int width, int height, uint32_t format, std::string& err)
    {
        Reset();
        NSMutableDictionary* a = base ? [base mutableCopy] : [NSMutableDictionary dictionary];
        a[(__bridge NSString*)kCVPixelBufferPixelFormatTypeKey] = @(format);
        a[(__bridge NSString*)kCVPixelBufferWidthKey] = @(width);
        a[(__bridge NSString*)kCVPixelBufferHeightKey] = @(height);
        a[(__bridge NSString*)kCVPixelBufferIOSurfacePropertiesKey] = @{};
        a[(__bridge NSString*)kCVPixelBufferMetalCompatibilityKey] = @YES;
        const CVReturn r = CVPixelBufferPoolCreate(kCFAllocatorDefault, nullptr, (__bridge CFDictionaryRef)a, &pool);
        if (r != kCVReturnSuccess || !pool) { err = Fmt("CVPixelBufferPoolCreate %dx%d %s failed (%d)", width, height, FourCCText(format).c_str(), int(r)); return false; }
        w = width; h = height; fmt = format;
        return true;
    }
    Buf Get()
    {
        CVPixelBufferRef b = nullptr;
        if (!pool || CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &b) != kCVReturnSuccess) return Buf();
        return Buf(b);
    }
};

// lock a bi-planar buffer and describe it
struct Locked {
    CVPixelBufferRef b; CVOptionFlags flags; BiView v;
    Locked(CVPixelBufferRef buf, bool readOnly) : b(buf), flags(readOnly ? kCVPixelBufferLock_ReadOnly : 0)
    {
        CVPixelBufferLockBaseAddress(b, flags);
        const uint32_t f = CVPixelBufferGetPixelFormatType(b);
        v.bytes = FormatBytes(f);
        v.width = int(CVPixelBufferGetWidth(b)); v.height = int(CVPixelBufferGetHeight(b));
        for (int p = 0; p < 2; ++p) { v.data[p] = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(b, size_t(p))); v.stride[p] = ptrdiff_t(CVPixelBufferGetBytesPerRowOfPlane(b, size_t(p))); }
    }
    ~Locked() { CVPixelBufferUnlockBaseAddress(b, flags); }
    bool Ok() const { return v.data[0] && v.data[1]; }
};

CMTime Pts(int64_t pts100ns) { return CMTimeMake(pts100ns, 10000000); }
const char* ModelText(int s) { return s == 2 ? "ready" : s == 1 ? "downloading" : s == 0 ? "download required" : "n/a"; }

} // namespace

// ------------------------------------------------------------------------------------------------ probe
const Caps& Probe()
{
    static Caps caps;
    static std::once_flag once;
    std::call_once(once, [] {
        @autoreleasepool {
            Caps& c = caps;
            c.built = true;
            NSOperatingSystemVersion v = NSProcessInfo.processInfo.operatingSystemVersion;
            c.os = Fmt("macOS %ld.%ld.%ld", long(v.majorVersion), long(v.minorVersion), long(v.patchVersion));
            c.chip = Sysctl("machdep.cpu.brand_string");
            const std::string model = Sysctl("hw.model");
            c.sdk = Fmt("SDK %d", int(__MAC_OS_X_VERSION_MAX_ALLOWED));
#ifndef PMVS_VT_FRC
            c.sdk += " (no VTFrameProcessor headers: frame processing compiled out)";
#elif !defined(PMVS_VT_LL)
            c.sdk += " (no macOS 26 headers: low-latency interpolation and super resolution compiled out)";
#endif
            c.frameProcessor = NSClassFromString(@"VTFrameProcessor") != nil;
            const int ll = ClassBool("VTLowLatencyFrameInterpolationConfiguration", "isSupported");
            const int sr = ClassBool("VTLowLatencySuperResolutionScalerConfiguration", "isSupported");
            int frc = ClassBool("VTFrameRateConversionConfiguration", "isSupported");
            if (frc == -2) frc = ClassBool("VTFrameRateConversionConfiguration", "processorSupported");   // the macOS 15.x name
            const int fsr = ClassBool("VTSuperResolutionScalerConfiguration", "isSupported");
            const int tnf = ClassBool("VTTemporalNoiseFilterConfiguration", "isSupported");
            int of = ClassBool("VTOpticalFlowConfiguration", "isSupported");
            if (of == -2) of = ClassBool("VTOpticalFlowConfiguration", "processorSupported");
            c.opticalFlow = of == 1;
            c.commandBuffer = [NSClassFromString(@"VTFrameProcessor") instancesRespondToSelector:sel_registerName("processWithCommandBuffer:parameters:")];
            std::string merr;
            if (Metal* mt = Metal::Shared(merr)) c.metal = mt->DeviceName();
#ifdef PMVS_VT_LL
            c.lowLatencyFI = ll == 1; c.lowLatencySR = sr == 1;
#endif
#ifdef PMVS_VT_FRC
            c.frc = frc == 1;
#endif
            c.superResolution = fsr == 1;
            Class llc = NSClassFromString(@"VTLowLatencyFrameInterpolationConfiguration");
            c.sizeQueries = llc && [llc respondsToSelector:sel_registerName("maximumDimensionForSpatialScaleFactor:")];
            auto st = [](int x) { return x == 1 ? "supported" : x == 0 ? "not supported" : x == -1 ? "class absent" : "no isSupported"; };
            c.detail = Fmt("%s, %s (%s), %s; VTFrameProcessor %s; low-latency interpolation %s; low-latency super resolution %s; "
                           "frame-rate conversion %s; super resolution (editing) %s (model %s); optical flow %s; temporal noise filter %s; macOS 27 size queries %s; "
                           "Metal command-buffer processing %s; Metal device %s",
                           c.os.c_str(), c.chip.empty() ? "unknown chip" : c.chip.c_str(), model.c_str(), c.sdk.c_str(), c.frameProcessor ? "present" : "absent",
                           st(ll), st(sr), st(frc), st(fsr), ModelText(SrModelStatus()), st(of), st(tnf), c.sizeQueries ? "present" : "absent",
                           c.commandBuffer ? "present" : "absent", c.metal.empty() ? "none" : c.metal.c_str());
        }
    });
    return caps;
}

std::string ProbeReport()
{
    const Caps& c = Probe();
    std::string r = "apple probe: " + c.detail + "\n";
    @autoreleasepool {
        const int sizes[][2] = { { 1280, 720 }, { 1920, 1080 }, { 3840, 2160 } };
#ifdef PMVS_VT_LL
        if (@available(macOS 26.0, *)) {
            for (auto& s : sizes) {
                for (int x = 1; x <= 4; ++x) {
                    VTLowLatencyFrameInterpolationConfiguration* fi = [[VTLowLatencyFrameInterpolationConfiguration alloc] initWithFrameWidth:s[0] frameHeight:s[1] numberOfInterpolatedFrames:x];
                    r += Fmt("  low-latency interpolation %dx%d, %d interpolated: %s\n", s[0], s[1], x, fi ? ("formats " + FormatList(Formats(fi.frameSupportedPixelFormats))).c_str() : "nil (not offered)");
                }
                VTLowLatencyFrameInterpolationConfiguration* fs = [[VTLowLatencyFrameInterpolationConfiguration alloc] initWithFrameWidth:s[0] frameHeight:s[1] spatialScaleFactor:2];
                r += Fmt("  low-latency interpolation + 2x scale %dx%d: %s\n", s[0], s[1], fs ? "offered" : "nil");
                NSArray<NSNumber*>* f = [VTLowLatencySuperResolutionScalerConfiguration supportedScaleFactorsForFrameWidth:s[0] frameHeight:s[1]];
                std::string fl;
                for (NSNumber* n in f) fl += Fmt("%s%g", fl.empty() ? "" : ",", n.floatValue);
                r += Fmt("  low-latency super resolution %dx%d: factors [%s]\n", s[0], s[1], fl.c_str());
            }
            const CMVideoDimensions mx = VTLowLatencySuperResolutionScalerConfiguration.maximumDimensions;
            const CMVideoDimensions mn = VTLowLatencySuperResolutionScalerConfiguration.minimumDimensions;
            r += Fmt("  low-latency super resolution: minimum %dx%d, maximum %dx%d\n", int(mn.width), int(mn.height), int(mx.width), int(mx.height));
        }
#endif
        // isSupported, and the size queries per spatial scale factor
        r += Fmt("  low-latency interpolation isSupported=%d, super resolution isSupported=%d; size queries (maximumDimension / "
                 "maximumPixelCount, macOS 27 API): %s\n", int(c.lowLatencyFI), int(c.lowLatencySR), c.sizeQueries ? "present" : ("absent on " + c.os).c_str());
        if (c.sizeQueries)
            for (int f : { 1, 2 })
                r += Fmt("  macOS 27: low-latency interpolation, scale %d: max dimension %ld, max pixels %ld; super resolution x%d: max dimension %ld\n", f,
                         ClassIntForInt("VTLowLatencyFrameInterpolationConfiguration", "maximumDimensionForSpatialScaleFactor:", f),
                         ClassIntForInt("VTLowLatencyFrameInterpolationConfiguration", "maximumPixelCountForSpatialScaleFactor:", f), 2 * f,
                         ClassIntForFloat("VTLowLatencySuperResolutionScalerConfiguration", "maximumDimensionForSpatialScaleFactor:", float(2 * f)));
#ifdef PMVS_VT_FRC
        if (@available(macOS 15.4, *)) {
            for (auto& s : sizes) {
                VTFrameRateConversionConfiguration* frc = [[VTFrameRateConversionConfiguration alloc] initWithFrameWidth:s[0] frameHeight:s[1] usePrecomputedFlow:NO
                    qualityPrioritization:VTFrameRateConversionConfigurationQualityPrioritizationNormal revision:VTFrameRateConversionConfiguration.defaultRevision];
                r += Fmt("  frame-rate conversion %dx%d: %s\n", s[0], s[1], frc ? ("formats " + FormatList(Formats(frc.frameSupportedPixelFormats))).c_str() : "nil (not offered)");
            }
        }
#endif
    }
    return r;
}

// ------------------------------------------------------------------------------------------------ engine
enum class Fi { None, LowLatency, Frc };

namespace {
// what Interpolate hands to the output stage: per output slot, the source-size buffer and where it comes from
struct Slot { int kind = 0; int gen = -1; float phase = 0; };   // kind: 0 previous real, 1 current real, 2 generated (gen index)

// VTFrameProcessor processWithCommandBuffer:parameters: (looked up by selector: older SDKs lack the declaration)
bool HasCommandBufferProcess(id proc) { return proc && [proc respondsToSelector:sel_registerName("processWithCommandBuffer:parameters:")]; }
void ProcessInCommandBuffer(id proc, void* cmd, id params)
{
    ((void(*)(id, SEL, id, id))objc_msgSend)(proc, sel_registerName("processWithCommandBuffer:parameters:"), (__bridge id)cmd, params);
}
// 16-bit (10 significant bits) -> 8-bit bi-planar with the same rounding as apple_convert.h ToBi (the recovery's
// residuals assume it)
void Bi16To8(const BiView& in, const BiView& out)
{
    for (int p = 0; p < 2; ++p) {
        const int rows = p ? ChromaH(in.height) : in.height, n = p ? 2 * ChromaW(in.width) : in.width;
        for (int y = 0; y < rows; ++y) {
            const uint8_t* s = in.data[p] + y * in.stride[p];
            uint8_t* d = out.data[p] + y * out.stride[p];
            for (int x = 0; x < n; ++x) d[x] = uint8_t(To8(int(detail::Rd16(s, x) >> 6)));
        }
    }
}
} // namespace

struct Engine::Impl {
    Config cfg;
    Fi fi = Fi::None;
    int exponent = 0;          // the low-latency grid (configured)
    SrKind srKind = SrKind::None;
    int srFactor = 0;
    int outW = 0, outH = 0;
    uint32_t fmt = 0;          // the interpolator's YUV 4:2:0 format (uploads)
    uint32_t ofmt = 0;         // the output stage's format: fmt, or 'x420' / 'xf20' with the 10-bit recovery
    uint32_t vtFmt = 0;        // what the interpolator takes: fmt, or RGhA (frame-rate conversion lists only that)
    bool rgb = false;          // vtFmt is RGhA: YUV <-> RGB through VTPixelTransferSession, tagged both ways
    bool fmtLossy = false;
    bool recover = false;      // 10-bit recovery on
    bool frcFlow = false;      // frame-rate conversion configured with precomputed flow
    bool editFlow = false;     // editing super resolution configured with precomputed flow
    std::vector<uint32_t> fiFormats, srFormats;
    Colour colour; bool colourSet = false;
    Ladder ladder;
    Stats stats;
    int consecutiveErrors = 0;
    bool lastCut = false;
    std::string chain;

    id fiProc = nil;           // VTFrameProcessor (macOS 15.4+)
    id srProc = nil;           // low-latency or editing super resolution
    id flowProc = nil;         // VTOpticalFlow
    Metal* metal = nullptr;
    std::shared_ptr<Metal::Net> net;
    NSDictionary* fiSrcAttrs = nil; NSDictionary* fiDstAttrs = nil; NSDictionary* srSrcAttrs = nil; NSDictionary* srDstAttrs = nil;
    NSDictionary* flowSrcAttrs = nil; NSDictionary* flowDstAttrs = nil;
    std::vector<uint32_t> flowFormats;
    Pool srcPool, src16Pool, vtSrcPool, fiDstPool, recPool, srDstPool, flowPool;
    VTPixelTransferSessionRef scaler = nullptr, xfer = nullptr;

    // the pair
    Buf prev, cur;             // source size, fmt
    Buf prev16, cur16;         // source size, ofmt (recovery only)
    Buf prevVt, curVt;         // the interpolator's copies (the same buffers unless rgb)
    Buf prevOut, curOut;       // output size (super resolution / scale of prev, cur); empty without super resolution
    Buf flowF, flowB;          // the pair's optical flow (prev -> cur, cur -> prev)
    bool flowValid = false;
    int64_t prevPts = 0, curPts = 0;
    Thumb prevThumb, curThumb;
    bool havePrev = false, haveCur = false;
    bool freshPair = true;     // the next pair is the first since a reset (FRC: random submission, clears its cache)
    // editing super resolution's temporal state: the last frame it upscaled (source size, output) in this stream
    Buf srLastSrc, srLastOut;
    float srLastPhase = 0;
    bool srHaveLast = false;

    ~Impl()
    {
        if (scaler) { VTPixelTransferSessionInvalidate(scaler); CFRelease(scaler); }
        if (xfer) { VTPixelTransferSessionInvalidate(xfer); CFRelease(xfer); }
        if (@available(macOS 15.4, *)) { if (fiProc) [fiProc endSession]; if (srProc) [srProc endSession]; if (flowProc) [flowProc endSession]; }
    }
    void Log(const std::function<void(const std::string&)>& log, const std::string& s) const { if (log) log(s); }
    Buf Src() const { return recover ? cur16 : cur; }       // the current real frame as the output stage sees it
    Buf PrevSrc() const { return recover ? prev16 : prev; }

    bool MakePools(std::string& err)
    {
        const int w = cfg.width, h = cfg.height;
        NSDictionary* srcBase = fi != Fi::None && !rgb ? fiSrcAttrs : srSrcAttrs;
        if (!srcPool.Make(srcBase, w, h, fmt, err)) return false;
        if (recover && !src16Pool.Make(srSrcAttrs, w, h, ofmt, err)) return false;
        if (recover && !recPool.Make(nil, w, h, ofmt, err)) return false;
        if (rgb && !vtSrcPool.Make(fiSrcAttrs, w, h, vtFmt, err)) return false;
        if (fi != Fi::None && !fiDstPool.Make(fiDstAttrs, w, h, vtFmt, err)) return false;
        if (srFactor && !srDstPool.Make(srDstAttrs, outW, outH, ofmt, err)) return false;
        return true;
    }
    void ResetPair() { prev = cur = prev16 = cur16 = prevVt = curVt = prevOut = curOut = flowF = flowB = srLastSrc = srLastOut = Buf(); flowValid = false; srHaveLast = false; havePrev = haveCur = false; freshPair = true; }
    // the first frame's range differs from the guess: the other range tag, if every processor lists it
    void FollowRange(bool fullRange, const std::function<void(const std::string&)>& log)
    {
        auto other = [&](uint32_t f) { return FormatBits(f) == 10 ? (fullRange ? kxf20 : kx420) : (fullRange ? k420f : k420v); };
        const uint32_t want = other(fmt), wantO = other(ofmt);
        if (want == fmt && wantO == ofmt) return;
        auto listed = [&](const std::vector<uint32_t>& l, uint32_t f) { return l.empty() || std::find(l.begin(), l.end(), f) != l.end(); };
        if ((!rgb && fi != Fi::None && !listed(fiFormats, want)) || (srFactor && srKind != SrKind::ArtCnn32 && srKind != SrKind::ArtCnn16 && !listed(srFormats, wantO))) return;
        const uint32_t old = fmt, oldO = ofmt;
        fmt = want; ofmt = wantO;
        if (!rgb) vtFmt = want;
        std::string err;
        if (!MakePools(err)) { fmt = old; ofmt = oldO; if (!rgb) vtFmt = old; MakePools(err); Log(log, "apple: could not switch to " + FourCCText(want) + ": " + err); return; }
        ResetPair();
        Log(log, "apple: the stream is " + std::string(fullRange ? "full" : "limited") + " range: pixel format " + FourCCText(want));
    }
    // a format conversion (YUV <-> RGhA, or a size change) into a buffer of pool
    Buf Convert(const Buf& src, Pool& pool, std::string& err)
    {
        Buf dst = pool.Get();
        if (!dst) { err = "no conversion buffer"; return Buf(); }
        Tag(dst.ref, colour);
        if (!xfer && VTPixelTransferSessionCreate(kCFAllocatorDefault, &xfer) != noErr) { err = "VTPixelTransferSessionCreate failed"; return Buf(); }
        const OSStatus st = VTPixelTransferSessionTransferImage(xfer, src.ref, dst.ref);
        if (st != noErr) { err = Fmt("pixel transfer %s -> %s failed (%d)", FourCCText(CVPixelBufferGetPixelFormatType(src.ref)).c_str(), FourCCText(pool.fmt).c_str(), int(st)); return Buf(); }
        return dst;
    }
    void FillGrey(const Buf& b, int shade)
    {
        Locked l(b.ref, false);
        if (!l.Ok()) return;
        for (int p = 0; p < 2; ++p) {
            const int rows = p ? ChromaH(l.v.height) : l.v.height, n = (p ? 2 * ChromaW(l.v.width) : l.v.width);
            for (int y = 0; y < rows; ++y) {
                uint8_t* r = l.v.data[p] + y * l.v.stride[p];
                for (int x = 0; x < n; ++x) {
                    const int v8 = p ? 128 : shade + (x * 32) / std::max(1, n) + (y * 16) / std::max(1, rows);
                    if (l.v.bytes == 1) r[x] = uint8_t(v8); else { const uint16_t u = uint16_t(v8 << 8); std::memcpy(r + 2 * x, &u, 2); }
                }
            }
        }
    }

    // ---- optical flow (once per pair; shared by frame-rate conversion, editing super resolution and the recovery)
    bool NeedFlow() const { return flowProc && ((fi == Fi::Frc && frcFlow) || (srKind == SrKind::Editing && editFlow && ladder.SrActive()) || recover); }
    bool RunFlow(const Buf& a, const Buf& b, bool sequential, Buf& f, Buf& bk, NSError* __autoreleasing* e)
    {
        f = flowPool.Get(); bk = flowPool.Get();
        if (!f || !bk) return false;
        if (@available(macOS 15.4, *)) {
            VTFrameProcessorFrame* fa = [[VTFrameProcessorFrame alloc] initWithBuffer:a.ref presentationTimeStamp:Pts(0)];
            VTFrameProcessorFrame* fb = [[VTFrameProcessorFrame alloc] initWithBuffer:b.ref presentationTimeStamp:Pts(1)];
            VTFrameProcessorOpticalFlow* of = [[VTFrameProcessorOpticalFlow alloc] initWithForwardFlow:f.ref backwardFlow:bk.ref];
            if (!fa || !fb || !of) return false;
            VTOpticalFlowParameters* p = [[VTOpticalFlowParameters alloc] initWithSourceFrame:fa nextFrame:fb
                submissionMode:(sequential ? VTOpticalFlowParametersSubmissionModeSequential : VTOpticalFlowParametersSubmissionModeRandom) destinationOpticalFlow:of];
            ++stats.flowCalls;
            if (p && [flowProc processWithParameters:p error:e]) return true;
            ++stats.flowErrors;
        }
        return false;
    }
    // the flow object for a phase step (scale 1: the pair's own flow)
    id FlowFor(float scale, std::string& err)
    {
        if (@available(macOS 15.4, *)) {
            if (!flowValid) return nil;
            if (std::fabs(scale - 1.0f) < 1e-4f) return [[VTFrameProcessorOpticalFlow alloc] initWithForwardFlow:flowF.ref backwardFlow:flowB.ref];
            if (!metal) return nil;
            Buf f = flowPool.Get(), b = flowPool.Get();
            if (!f || !b) { err = "no flow buffer"; return nil; }
            void* cmd = metal->NewCommandBuffer();
            bool ok = metal->EncodeFlowScale(cmd, flowF.ref, f.ref, scale, err) && metal->EncodeFlowScale(cmd, flowB.ref, b.ref, scale, err);
            ok = metal->Commit(cmd, true, err) && ok;
            if (!ok) return nil;
            return [[VTFrameProcessorOpticalFlow alloc] initWithForwardFlow:f.ref backwardFlow:b.ref];
        }
        return nil;
    }

    // ---- super resolution, one frame (no fallback). phase: the frame's position in the pair (1 = the real frame);
    // random: no temporal history (a cut, a seek, the first frame)
    bool RunSr(const Buf& src, const Buf& dst, int64_t pts, float phase, bool random, NSError* __autoreleasing* e, std::string& err)
    {
        ++stats.vtCalls;
        if (srKind == SrKind::ArtCnn32 || srKind == SrKind::ArtCnn16) {
            --stats.vtCalls;
            if (!metal || !net) { err = "ArtCNN is not loaded"; return false; }
            void* cmd = metal->NewCommandBuffer();
            const bool enc = metal->EncodeArtCnn(*net, cmd, src.ref, dst.ref, err);
            const bool ok = metal->Commit(cmd, true, err) && enc;
            if (!ok) { ++stats.srErrors; }
            return ok;
        }
#ifdef PMVS_VT_LL
        if (@available(macOS 26.0, *)) {
            VTFrameProcessorFrame* s = [[VTFrameProcessorFrame alloc] initWithBuffer:src.ref presentationTimeStamp:Pts(pts)];
            VTFrameProcessorFrame* d = [[VTFrameProcessorFrame alloc] initWithBuffer:dst.ref presentationTimeStamp:Pts(pts)];
            if (!s || !d) return false;
            id<VTFrameProcessorParameters> p = nil;
            if (srKind == SrKind::LowLatency) p = [[VTLowLatencySuperResolutionScalerParameters alloc] initWithSourceFrame:s destinationFrame:d];
            else if (srKind == SrKind::Editing) {
                VTFrameProcessorFrame *ps = nil, *po = nil;
                VTFrameProcessorOpticalFlow* of = nil;
                const bool useLast = !random && srHaveLast;
                if (useLast) {
                    ps = [[VTFrameProcessorFrame alloc] initWithBuffer:srLastSrc.ref presentationTimeStamp:Pts(pts - 1)];
                    po = [[VTFrameProcessorFrame alloc] initWithBuffer:srLastOut.ref presentationTimeStamp:Pts(pts - 1)];
                    if (editFlow) {
                        of = FlowFor(phase - srLastPhase, err);
                        if (!of) { ps = po = nil; }          // no flow for this step: start a new sequence
                    }
                }
                p = [[VTSuperResolutionScalerParameters alloc] initWithSourceFrame:s previousFrame:ps previousOutputFrame:po opticalFlow:of
                    submissionMode:(ps ? VTSuperResolutionScalerParametersSubmissionModeSequential : VTSuperResolutionScalerParametersSubmissionModeRandom) destinationFrame:d];
            }
            if (p && [srProc processWithParameters:p error:e]) {
                if (srKind == SrKind::Editing) { srLastSrc = src; srLastOut = dst; srLastPhase = phase >= 1.0f ? 0.0f : phase; srHaveLast = true; }
                return true;
            }
            ++stats.vtErrors; ++stats.srErrors;
            if (srKind == SrKind::Editing) srHaveLast = false;
        }
#endif
        (void)phase; (void)random; (void)pts;
        return false;
    }

    // one interpolation call: the frames at phases (vtFmt buffers from fiDstPool, one per phase). With the 10-bit
    // recovery the recovered frames (ofmt, recPool) come back in rec, encoded in the same Metal command buffer when
    // VideoToolbox offers processWithCommandBuffer.
    bool RunFi(const Buf& p0, const Buf& c0, const Buf& p16, const Buf& c16, int64_t pp, int64_t cp, const std::vector<float>& phases, std::vector<Buf>& dst,
               std::vector<Buf>* rec, bool sequential, NSError* __autoreleasing* e, std::string& err)
    {
        dst.clear();
        if (@available(macOS 15.4, *)) {
            NSMutableArray<NSNumber*>* ph = [NSMutableArray array];
            NSMutableArray* frames = [NSMutableArray array];
            for (float t : phases) {
                Buf d = fiDstPool.Get();
                if (!d) return false;
                Tag(d.ref, colour);
                VTFrameProcessorFrame* f = [[VTFrameProcessorFrame alloc] initWithBuffer:d.ref presentationTimeStamp:Pts(pp + int64_t(std::llround(double(cp - pp) * double(t))))];
                if (!f) return false;
                [ph addObject:@(t)];
                [frames addObject:f];
                dst.push_back(d);
            }
            VTFrameProcessorFrame* ps = [[VTFrameProcessorFrame alloc] initWithBuffer:p0.ref presentationTimeStamp:Pts(pp)];
            VTFrameProcessorFrame* cs = [[VTFrameProcessorFrame alloc] initWithBuffer:c0.ref presentationTimeStamp:Pts(cp)];
            id<VTFrameProcessorParameters> params = nil;
#ifdef PMVS_VT_LL
            if (fi == Fi::LowLatency) {
                if (@available(macOS 26.0, *))
                    params = [[VTLowLatencyFrameInterpolationParameters alloc] initWithSourceFrame:cs previousFrame:ps interpolationPhase:ph destinationFrames:frames];
            }
#endif
            if (fi == Fi::Frc) {
                VTFrameProcessorOpticalFlow* of = nil;
                if (frcFlow) { std::string ferr; of = FlowFor(1.0f, ferr); if (!of) { err = "no optical flow for frame-rate conversion"; return false; } }
                params = [[VTFrameRateConversionParameters alloc] initWithSourceFrame:ps nextFrame:cs opticalFlow:of interpolationPhase:ph
                    submissionMode:(sequential ? VTFrameRateConversionParametersSubmissionModeSequential : VTFrameRateConversionParametersSubmissionModeRandom)
                    destinationFrames:frames];
            }
            ++stats.vtCalls;
            if (!ps || !cs || !params) { ++stats.vtErrors; ++stats.fiErrors; dst.clear(); return false; }
            const bool wantRec = rec && recover && metal;
            if (wantRec && HasCommandBufferProcess(fiProc)) {
                // VideoToolbox and the recovery in one command buffer
                void* cmd = metal->NewCommandBuffer();
                ProcessInCommandBuffer(fiProc, cmd, params);
                rec->clear();
                bool ok = true;
                for (size_t i = 0; i < phases.size() && ok; ++i) {
                    Buf r = recPool.Get();
                    if (!r) { ok = false; err = "no recovery buffer"; break; }
                    Tag(r.ref, colour);
                    ok = metal->EncodeRecover(cmd, p16.ref, c16.ref, dst[i].ref, flowValid ? flowF.ref : nullptr, phases[i], r.ref, err);
                    rec->push_back(r);
                }
                ok = metal->Commit(cmd, true, err) && ok;
                if (ok) { stats.recovered += phases.size(); return true; }
                ++stats.vtErrors; ++stats.fiErrors; dst.clear(); rec->clear();
                return false;
            }
            if (![fiProc processWithParameters:params error:e]) { ++stats.vtErrors; ++stats.fiErrors; dst.clear(); return false; }
            if (wantRec) {
                void* cmd = metal->NewCommandBuffer();
                rec->clear();
                bool ok = true;
                for (size_t i = 0; i < phases.size() && ok; ++i) {
                    Buf r = recPool.Get();
                    if (!r) { ok = false; err = "no recovery buffer"; break; }
                    Tag(r.ref, colour);
                    ok = metal->EncodeRecover(cmd, p16.ref, c16.ref, dst[i].ref, flowValid ? flowF.ref : nullptr, phases[i], r.ref, err);
                    rec->push_back(r);
                }
                ok = metal->Commit(cmd, true, err) && ok;
                if (!ok) { rec->clear(); return false; }
                stats.recovered += phases.size();
            }
            return true;
        }
        dst.clear();
        return false;
    }

    // src (source size, ofmt) -> output size: super resolution, or the plain scale once the ladder dropped it
    Buf Upscale(const Buf& src, int64_t pts, float phase, bool random, std::string& err, const std::function<void(const std::string&)>& log)
    {
        if (!srFactor) return src;
        Buf dst = srDstPool.Get();
        if (!dst) { err = "no super-resolution output buffer"; return Buf(); }
        Tag(dst.ref, colour);
        if (ladder.SrActive()) {
            NSError* e = nil;
            std::string serr;
            if (RunSr(src, dst, pts, phase, random, &e, serr)) return dst;
            if (stats.srErrors <= 3 || stats.srErrors % 240 == 0) Log(log, "apple: super resolution failed: " + (serr.empty() ? ErrText(e) : serr) + "; scaling this frame instead");
        }
        if (!scaler) {
            if (VTPixelTransferSessionCreate(kCFAllocatorDefault, &scaler) != noErr) { err = "VTPixelTransferSessionCreate failed"; return Buf(); }
            VTSessionSetProperty(scaler, kVTPixelTransferPropertyKey_ScalingMode, kVTScalingMode_Normal);
        }
        const OSStatus st = VTPixelTransferSessionTransferImage(scaler, src.ref, dst.ref);
        if (st != noErr) { err = Fmt("VTPixelTransferSessionTransferImage failed (%d)", int(st)); return Buf(); }
        return dst;
    }

    bool Emit(const Buf& b, const PlanarOut& o, std::string& err)
    {
        Locked l(b.ref, true);
        if (!l.Ok()) { err = "could not lock an output buffer"; return false; }
        BiToPlanar(l.v, o, cfg.bits);
        return true;
    }

    // the pair: k - 1 output buffers (output size, ofmt) by the plan, then the real frame's output (curOut); false
    // only on a hard error. Super resolution runs in presentation order (generated frames, then the real one).
    bool Interpolate(bool sequential, std::vector<Buf>& out, std::string& err, const std::function<void(const std::string&)>& log)
    {
        const int k = cfg.k;
        PhasePlan plan;
        if (lastCut || fi == Fi::None) plan = MakePlan(k, 0);
        else if (fi == Fi::Frc && ladder.Level() < 2) plan = MakePlan(k, 0, true);
        else plan = MakePlan(k, ladder.Exponent());
        flowValid = false;
        if (!lastCut && NeedFlow() && (!plan.phases.empty() || srKind == SrKind::Editing)) {
            NSError* e = nil;
            flowValid = RunFlow(prev, cur, sequential, flowF, flowB, &e);
            if (!flowValid && (stats.flowErrors <= 3 || stats.flowErrors % 240 == 0)) Log(log, "apple: optical flow failed: " + ErrText(e));
        }
        std::vector<Buf> vt;   // source size, ofmt, one per plan phase
        if (!plan.phases.empty()) {
            NSError* e = nil;
            std::vector<Buf> raw, rec;
            std::string cerr;
            bool ok = (fi != Fi::Frc || !frcFlow || flowValid) &&
                      RunFi(prevVt, curVt, prev16, cur16, prevPts, curPts, plan.phases, raw, recover ? &rec : nullptr, sequential, &e, cerr);
            if (ok && recover) vt = rec;
            else if (ok)
                for (const Buf& r : raw) {
                    Buf y = rgb ? Convert(r, srcPool, cerr) : r;
                    if (!y) { ok = false; break; }
                    vt.push_back(y);
                }
            if (!ok) {
                if (++consecutiveErrors <= 3 || consecutiveErrors % 240 == 0)
                    Log(log, Fmt("apple: interpolation failed (%d in a row): %s; this pair repeats the nearer real frame", consecutiveErrors, cerr.empty() ? ErrText(e).c_str() : cerr.c_str()));
                plan = MakePlan(k, 0);
                vt.clear();
            } else consecutiveErrors = 0;
        }
        // super resolution in presentation order: each generated phase once, then the real frame
        std::vector<Buf> up(vt.size());
        if (srFactor) {
            std::vector<size_t> order(vt.size());
            for (size_t i = 0; i < order.size(); ++i) order[i] = i;
            std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return plan.phases[a] < plan.phases[b]; });
            for (size_t i : order) {
                const float t = plan.phases[i];
                const int64_t pts = prevPts + int64_t(std::llround(double(curPts - prevPts) * double(t)));
                up[i] = Upscale(vt[i], pts, t, lastCut, err, log);
                if (!up[i]) return false;
            }
            curOut = Upscale(Src(), curPts, 1.0f, lastCut, err, log);
            if (!curOut) return false;
        }
        out.clear();
        for (int j = 1; j < k; ++j) {
            const int s = plan.slot[size_t(j - 1)];
            if (s == kPrev) out.push_back(srFactor ? prevOut : PrevSrc());
            else if (s == kCur) out.push_back(srFactor ? curOut : Src());
            else { out.push_back(srFactor ? up[size_t(s)] : vt[size_t(s)]); ++stats.generated; }
        }
        return true;
    }

    // ---- self-tests and benchmarks on synthetic frames (a VM lists effects and fails every call)
    static double Median(std::vector<double> v) { std::sort(v.begin(), v.end()); return v.empty() ? 0 : v[v.size() / 2]; }
    bool SelfTestSr(std::string& err, double* ms = nullptr)
    {
        Buf s = recover ? src16Pool.Get() : srcPool.Get(), d = srDstPool.Get();
        if (!s || !d) { err = "no buffers"; return false; }
        Tag(s.ref, colour); Tag(d.ref, colour);
        FillGrey(s, 60);
        std::vector<double> t;
        for (int i = 0; i < (ms ? 6 : 1); ++i) {
            NSError* e = nil;
            const double t0 = NowMs();
            if (!RunSr(s, d, int64_t(i) * 417083, 1.0f, i == 0, &e, err)) { if (err.empty()) err = ErrText(e); return false; }
            if (i) t.push_back(NowMs() - t0);
        }
        srHaveLast = false; srLastSrc = srLastOut = Buf();
        if (ms) *ms = Median(t);
        return true;
    }
    bool SelfTestFlow(std::string& err, double* ms)
    {
        Buf a = srcPool.Get(), b = srcPool.Get();
        if (!a || !b) { err = "no buffers"; return false; }
        Tag(a.ref, colour); Tag(b.ref, colour);
        FillGrey(a, 60); FillGrey(b, 70);
        std::vector<double> t;
        for (int i = 0; i < 4; ++i) {
            NSError* e = nil;
            const double t0 = NowMs();
            if (!RunFlow(a, b, i > 0, flowF, flowB, &e)) { err = ErrText(e); return false; }
            if (i) t.push_back(NowMs() - t0);
        }
        flowValid = false;
        if (ms) *ms = Median(t);
        return true;
    }
    bool SelfTestFi(std::string& err, double* ms = nullptr)
    {
        Buf a = srcPool.Get(), b = srcPool.Get();
        if (!a || !b) { err = "no buffers"; return false; }
        Tag(a.ref, colour); Tag(b.ref, colour);
        FillGrey(a, 60); FillGrey(b, 80);
        Buf a16, b16;
        if (recover) { a16 = src16Pool.Get(); b16 = src16Pool.Get(); if (!a16 || !b16) { err = "no buffers"; return false; } FillGrey(a16, 60); FillGrey(b16, 80); }
        Buf av = a, bv = b;
        if (rgb) { av = Convert(a, vtSrcPool, err); bv = Convert(b, vtSrcPool, err); if (!av || !bv) return false; }
        if (fi == Fi::Frc && frcFlow) { NSError* fe = nil; flowValid = RunFlow(a, b, false, flowF, flowB, &fe); if (!flowValid) { err = "optical flow: " + ErrText(fe); return false; } }
        // the phases this configuration will really ask for (an x = 3 grid is an unlock: it must run, not only be listed)
        const PhasePlan plan = fi == Fi::LowLatency ? MakePlan(cfg.k, exponent) : MakePlan(cfg.k, 0, true);
        const std::vector<float> phases = plan.phases.empty() ? std::vector<float>{ 0.5f } : plan.phases;
        std::vector<double> t;
        for (int i = 0; i < (ms ? 4 : 1); ++i) {
            NSError* e = nil;
            std::vector<Buf> out, rec;
            const double t0 = NowMs();
            if (!RunFi(av, bv, a16, b16, 0, 417083, phases, out, recover ? &rec : nullptr, i > 0, &e, err)) { if (err.empty()) err = ErrText(e); flowValid = false; return false; }
            if (i) t.push_back(NowMs() - t0);
            if (i == 0 && rgb && !Convert(out[0], srcPool, err)) { flowValid = false; return false; }
        }
        flowValid = false;
        if (ms) *ms = Median(t);
        return true;
    }
};

// ------------------------------------------------------------------------------------------------ model download
int SrModelStatus(int width, int height, float* percent)
{
#ifdef PMVS_VT_LL
    if (@available(macOS 26.0, *)) {
        @autoreleasepool {
            if (!VTSuperResolutionScalerConfiguration.isSupported) return -1;
            VTSuperResolutionScalerConfiguration* c = [[VTSuperResolutionScalerConfiguration alloc] initWithFrameWidth:width frameHeight:height scaleFactor:2
                inputType:VTSuperResolutionScalerConfigurationInputTypeVideo usePrecomputedFlow:NO
                qualityPrioritization:VTSuperResolutionScalerConfigurationQualityPrioritizationNormal revision:VTSuperResolutionScalerConfiguration.defaultRevision];
            if (!c) return -1;
            if (percent) *percent = c.configurationModelPercentageAvailable;
            return int(c.configurationModelStatus);
        }
    }
#endif
    (void)width; (void)height; (void)percent;
    return -1;
}

bool SrModelDownload(double waitSeconds, const std::function<void(const std::string&)>& log, std::string& err)
{
#ifdef PMVS_VT_LL
    if (@available(macOS 26.0, *)) {
        @autoreleasepool {
            if (!VTSuperResolutionScalerConfiguration.isSupported) { err = "VideoToolbox super resolution is not supported on this Mac"; return false; }
            VTSuperResolutionScalerConfiguration* c = [[VTSuperResolutionScalerConfiguration alloc] initWithFrameWidth:1920 frameHeight:1080 scaleFactor:2
                inputType:VTSuperResolutionScalerConfigurationInputTypeVideo usePrecomputedFlow:NO
                qualityPrioritization:VTSuperResolutionScalerConfigurationQualityPrioritizationNormal revision:VTSuperResolutionScalerConfiguration.defaultRevision];
            if (!c) { err = "no configuration for 1920x1080"; return false; }
            if (c.configurationModelStatus == VTSuperResolutionScalerConfigurationModelStatusReady) { if (log) log("the super-resolution model is already downloaded"); return true; }
            static std::atomic<bool> started{ false };
            static std::atomic<int> result{ 0 };   // 0 running, 1 ok, 2 failed
            static std::string lastErr;
            static std::mutex errLock;
            if (!started.exchange(true)) {
                if (log) log("downloading the super-resolution model (Apple's one-time download)");
                VTSuperResolutionScalerConfiguration* keep = c;
                [c downloadConfigurationModelWithCompletionHandler:^(NSError* e) {
                    (void)keep;
                    if (e) { std::lock_guard<std::mutex> lk(errLock); lastErr = ErrText(e); result = 2; started = false; }
                    else result = 1;
                }];
            }
            if (waitSeconds <= 0) return true;
            const double t0 = NowMs();
            int lastPct = -1;
            while (result == 0 && NowMs() - t0 < waitSeconds * 1000.0) {
                const int pct = int(c.configurationModelPercentageAvailable * 100.0f);
                if (pct != lastPct && log) { log(Fmt("model download: %d %%", pct)); lastPct = pct; }
                [NSThread sleepForTimeInterval:0.5];
            }
            if (result == 1 || c.configurationModelStatus == VTSuperResolutionScalerConfigurationModelStatusReady) { if (log) log("the super-resolution model is ready"); return true; }
            if (result == 2) { std::lock_guard<std::mutex> lk(errLock); err = "download failed: " + lastErr; return false; }
            err = "the download is still running";
            return false;
        }
    }
#endif
    (void)waitSeconds; (void)log;
    err = "VideoToolbox super resolution needs macOS 26 and a build with the macOS 26 SDK";
    return false;
}

Engine::Engine() : m(std::make_unique<Impl>()) {}
Engine::~Engine() { @autoreleasepool { m.reset(); } }
int Engine::OutWidth() const { return m->outW; }
int Engine::OutHeight() const { return m->outH; }
int Engine::K() const { return m->cfg.k; }
bool Engine::LastPairWasCut() const { return m->lastCut; }
const Stats& Engine::Stat() const { m->stats.ladder = m->ladder.Level(); return m->stats; }
std::string Engine::Describe() const { return m->chain + "; ladder: " + m->ladder.Describe(); }

bool Engine::Configure(const Config& c, std::string& err, std::string& info)
{
    @autoreleasepool {
        Impl& d = *m;
        d.cfg = c;
        d.cfg.k = std::clamp(c.k, 1, 8);
        d.colour = c.colour;
        const Caps& caps = Probe();
        const int w = c.width, h = c.height;
        if (w < 2 || h < 2 || (w & 1) || (h & 1)) { err = Fmt("%dx%d: VideoToolbox needs an even size", w, h); return false; }
        if (@available(macOS 15.4, *)) {} else { err = caps.os + ": VideoToolbox frame processing needs macOS 15.4 or newer"; return false; }
        std::string fiMode = c.fi;
        for (auto& ch : fiMode) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        std::vector<std::string> notes;
        auto joined = [&] { std::string n; for (auto& s : notes) if (!s.empty()) n += (n.empty() ? "" : "; ") + s; return n; };
        auto has = [](const std::vector<uint32_t>& l, uint32_t f) { return std::find(l.begin(), l.end(), f) != l.end(); };
        std::string merr;
        d.metal = Metal::Shared(merr);
        if (!d.metal) notes.push_back("Metal: " + merr);

        // ---- optical flow (VTOpticalFlow): set up once, used per pair only by the stages that take it
        const uint32_t yuvPref = c.bits > 8 ? (c.colour.fullRange ? kxf20 : kx420) : (c.colour.fullRange ? k420f : k420v);
        if (caps.opticalFlow && (d.cfg.k >= 2 || c.sr != "off")) {
            if (@available(macOS 15.4, *)) {
                VTOpticalFlowConfiguration* oc = [[VTOpticalFlowConfiguration alloc] initWithFrameWidth:w frameHeight:h
                    qualityPrioritization:VTOpticalFlowConfigurationQualityPrioritizationNormal revision:VTOpticalFlowConfiguration.defaultRevision];
                VTFrameProcessor* p = oc ? [[VTFrameProcessor alloc] init] : nil;
                NSError* e = nil;
                if (oc && [p startSessionWithConfiguration:oc error:&e]) {
                    d.flowProc = p;
                    d.flowFormats = Formats(oc.frameSupportedPixelFormats);
                    d.flowSrcAttrs = oc.sourcePixelBufferAttributes; d.flowDstAttrs = oc.destinationPixelBufferAttributes;
                    id pf = d.flowDstAttrs[(__bridge NSString*)kCVPixelBufferPixelFormatTypeKey];
                    uint32_t ff = kCVPixelFormatType_TwoComponent16Half;
                    if ([pf isKindOfClass:[NSNumber class]]) ff = [pf unsignedIntValue];
                    else if ([pf isKindOfClass:[NSArray class]] && [pf count]) ff = [[pf firstObject] unsignedIntValue];
                    std::string perr;
                    if (!d.flowPool.Make(d.flowDstAttrs, w, h, ff, perr)) { notes.push_back("optical flow: " + perr); [p endSession]; d.flowProc = nil; }
                } else notes.push_back("optical flow: " + (oc ? "startSession failed: " + ErrText(e) : std::string("not offered for this size")));
            }
        }

        // ---- interpolation. The low-latency interpolator offers 2^x - 1 phases (x = 1: 0.5; x = 2: quarters), so
        // x2 and x4 are native. Other multipliers use frame-rate conversion (any phase) when it runs here. x8 first
        // tries the low-latency x = 3 grid (eighths: macOS 26 accepts it, beyond Apple's quarter-phase example) and
        // keeps it only if it really runs. The last resort is the low-latency eighth or quarter grid with the phases
        // rounded to it. Every candidate must pass a self-test on synthetic frames with the phases it will use.
        auto pickFiFormat = [&]() -> bool {
            const uint32_t f = PickFormat(d.fiFormats, c.bits, c.colour.fullRange, &d.fmtLossy);
            d.rgb = false;
            if (f) { d.fmt = d.vtFmt = f; return true; }
            if (has(d.fiFormats, kRGhA)) { d.rgb = true; d.vtFmt = kRGhA; d.fmt = yuvPref; d.fmtLossy = false; return true; }
            return false;
        };
        auto endFi = [&] { if (@available(macOS 15.4, *)) { if (d.fiProc) [d.fiProc endSession]; } d.fiProc = nil; d.fi = Fi::None; d.recover = false; };
        bool flowTested = false, flowOk = false;
        // one candidate: its configuration and session, formats, pools, self-test (and its benchmark)
        auto tryFi = [&](Fi kind, int x, bool withFlow) -> bool {
            const std::string name = kind == Fi::LowLatency ? Fmt("low-latency interpolation (grid 1/%d)", 1 << x) : std::string(withFlow ? "frame-rate conversion (shared optical flow)" : "frame-rate conversion");
            id cfgObj = nil;
            if (kind == Fi::LowLatency) {
                if (!caps.lowLatencyFI) { notes.push_back("low-latency interpolation not supported here"); return false; }
#ifdef PMVS_VT_LL
                if (@available(macOS 26.0, *)) {
                    VTLowLatencyFrameInterpolationConfiguration* fc = [[VTLowLatencyFrameInterpolationConfiguration alloc] initWithFrameWidth:w frameHeight:h numberOfInterpolatedFrames:x];
                    if (fc) { d.fiFormats = Formats(fc.frameSupportedPixelFormats); d.fiSrcAttrs = fc.sourcePixelBufferAttributes; d.fiDstAttrs = fc.destinationPixelBufferAttributes; }
                    cfgObj = fc;
                }
#endif
            } else {
                if (!caps.frc) { notes.push_back("frame-rate conversion not supported here"); return false; }
                if (@available(macOS 15.4, *)) {
                    VTFrameRateConversionConfiguration* fc = [[VTFrameRateConversionConfiguration alloc] initWithFrameWidth:w frameHeight:h usePrecomputedFlow:withFlow
                        qualityPrioritization:VTFrameRateConversionConfigurationQualityPrioritizationNormal revision:VTFrameRateConversionConfiguration.defaultRevision];
                    if (fc) { d.fiFormats = Formats(fc.frameSupportedPixelFormats); d.fiSrcAttrs = fc.sourcePixelBufferAttributes; d.fiDstAttrs = fc.destinationPixelBufferAttributes; }
                    cfgObj = fc;
                }
            }
            if (!cfgObj) { notes.push_back(name + Fmt(": not offered for %dx%d", w, h)); return false; }
            NSError* e = nil;
            VTFrameProcessor* p = nil;
            if (@available(macOS 15.4, *)) p = [[VTFrameProcessor alloc] init];
            if (!p || ![p startSessionWithConfiguration:cfgObj error:&e]) { notes.push_back(name + ": startSession failed: " + ErrText(e)); return false; }
            d.fiProc = p; d.fi = kind; d.exponent = kind == Fi::LowLatency ? x : ExponentFor(d.cfg.k);
            d.frcFlow = kind == Fi::Frc && withFlow;
            std::string perr;
            if (!pickFiFormat()) { notes.push_back(name + " lists no format we can feed (" + FormatList(d.fiFormats) + ")"); endFi(); return false; }
            d.srFactor = 0; d.srKind = SrKind::None; d.outW = w; d.outH = h;
            // 10-bit recovery: an 8-bit-only low-latency interpolator on a deeper source
            std::string tb = c.tenBit; for (auto& ch : tb) ch = char(std::tolower(static_cast<unsigned char>(ch)));
            d.recover = kind == Fi::LowLatency && c.bits > 8 && d.fmtLossy && !d.rgb && d.metal && tb != "off";
            d.ofmt = d.recover ? (c.colour.fullRange ? kxf20 : kx420) : d.fmt;
            if (d.recover && !d.flowProc) notes.push_back("10-bit recovery without optical flow (none here): co-located residuals");
            if (!d.MakePools(perr)) { notes.push_back(name + ": " + perr); endFi(); return false; }
            if (d.flowProc && !flowTested && (d.frcFlow || d.recover)) {
                flowTested = true;
                if (!has(d.flowFormats, d.fmt) && !d.flowFormats.empty()) { notes.push_back("optical flow does not list " + FourCCText(d.fmt)); flowOk = false; }
                else { std::string ferr; flowOk = d.SelfTestFlow(ferr, &d.stats.benchFlowMs); if (!flowOk) notes.push_back("optical flow is listed but does not run here (" + ferr + ")"); }
                if (!flowOk) { if (@available(macOS 15.4, *)) [d.flowProc endSession]; d.flowProc = nil; }
            }
            if (d.frcFlow && !d.flowProc) { notes.push_back(name + ": no working optical flow"); endFi(); return false; }
            double ms = 0;
            if (!d.SelfTestFi(perr, &ms)) {
                notes.push_back(name + (d.recover ? " with 10-bit recovery" : "") + " is listed but does not run here (self-test: " + perr + ")");
                if (d.recover) {   // once more without the recovery: plain 8-bit generated frames beat none
                    d.recover = false; d.ofmt = d.fmt; perr.clear();
                    if (d.MakePools(perr) && d.SelfTestFi(perr, &ms)) { notes.push_back("10-bit recovery off (its self-test failed)"); d.stats.benchFiMs = ms; return true; }
                }
                endFi(); return false;
            }
            d.stats.benchFiMs = ms;
            return true;
        };
        if (d.cfg.k >= 2) {
            const int k = d.cfg.k;
            std::vector<std::pair<Fi, int>> order;
            if (fiMode == "frc") order = { { Fi::Frc, 0 } };
            else if (fiMode == "lowlatency") { for (int x = std::min(ExponentFor(k), 3); x >= 1; --x) order.push_back({ Fi::LowLatency, x }); }
            else if (k == 2 || k == 4) order = { { Fi::LowLatency, k == 2 ? 1 : 2 }, { Fi::Frc, 0 } };
            else if (k == 8) order = { { Fi::LowLatency, 3 }, { Fi::Frc, 0 }, { Fi::LowLatency, 2 } };
            else order = { { Fi::Frc, 0 }, { Fi::LowLatency, 3 }, { Fi::LowLatency, 2 } };
            for (auto& o : order) {
                if (o.first == Fi::Frc && d.flowProc && tryFi(Fi::Frc, 0, true)) break;   // shared flow first, then FRC's own
                if (tryFi(o.first, o.second, false)) break;
            }
            if (d.fi == Fi::None) {
                err = "no VideoToolbox interpolator runs for " + std::to_string(w) + "x" + std::to_string(h) + " x" + std::to_string(k) + (notes.empty() ? std::string() : ": " + joined()) + " (" + caps.detail + ")";
                return false;
            }
        }
        if (d.fi == Fi::None) { d.fmt = d.vtFmt = d.ofmt = yuvPref; d.recover = false; }

        // ---- super resolution (after interpolation, on every output frame): the candidates in Auto's order, each
        // with its self-test and real-time benchmark
        SrInputs si;
        si.mode = c.sr; si.w = w; si.h = h; si.anime = c.anime;
        int maxDim2 = 0, maxDim4 = 0;
#ifdef PMVS_VT_LL
        if (@available(macOS 26.0, *)) {
            if (caps.lowLatencySR) {
                for (NSNumber* n in [VTLowLatencySuperResolutionScalerConfiguration supportedScaleFactorsForFrameWidth:w frameHeight:h]) si.llFactors.push_back(n.floatValue);
                const CMVideoDimensions mx = VTLowLatencySuperResolutionScalerConfiguration.maximumDimensions;
                if (mx.width > 0 && mx.height > 0) maxDim2 = maxDim4 = std::max(int(mx.width), int(mx.height));
                const long q2 = ClassIntForFloat("VTLowLatencySuperResolutionScalerConfiguration", "maximumDimensionForSpatialScaleFactor:", 2.0f);
                const long q4 = ClassIntForFloat("VTLowLatencySuperResolutionScalerConfiguration", "maximumDimensionForSpatialScaleFactor:", 4.0f);
                if (q2 > 0) maxDim2 = int(q2);
                if (q4 > 0) maxDim4 = int(q4);
                si.llMaxDim2 = maxDim2; si.llMaxDim4 = maxDim4;
            }
            if (caps.superResolution) {
                si.editing = true;
                for (NSNumber* n in VTSuperResolutionScalerConfiguration.supportedScaleFactors) si.editingFactors.push_back(n.floatValue);
                si.editingModel = SrModelStatus(std::min(w, 1920), std::min(h, 1080));
                if (si.editingModel < 0) si.editing = false;
            }
        }
#endif
        std::shared_ptr<Metal::Net> net32, net16;
        if (d.metal) {
            for (const char* nm : { "ArtCNN_C4F32", "ArtCNN_C4F16" }) {
                const std::string path = FindModel(nm);
                if (path.empty()) continue;
                NnModel model; std::string lerr;
                std::shared_ptr<Metal::Net> n;
                if (model.Load(path, lerr)) n = d.metal->LoadNet(model, lerr);
                if (!n) { notes.push_back(std::string(nm) + ": " + lerr); continue; }
                (std::string(nm) == "ArtCNN_C4F32" ? net32 : net16) = n;
            }
            si.artcnn32 = net32 != nullptr; si.artcnn16 = net16 != nullptr;
            if (!net32 && !net16) notes.push_back("ArtCNN weights not found (PUSHMPC_MODELS, <plugin>/pushmpc-models, ~/.config/pushmpc/models)");
        }
        const SrOrderResult so = DecideSrOrder(si);
        notes.push_back(so.why);
        if (so.wantModelDownload) {
            std::string md = c.modelDownload; for (auto& ch : md) ch = char(std::tolower(static_cast<unsigned char>(ch)));
            if (md != "never") { std::string derr; if (!SrModelDownload(0, nullptr, derr)) notes.push_back("model download: " + derr); else notes.push_back("the editing-grade model download started (Apple's one-time download; AppleSrModelDownload=never turns this off)"); }
        }
        auto endSr = [&] { if (@available(macOS 15.4, *)) { if (d.srProc) [d.srProc endSession]; } d.srProc = nil; d.srFactor = 0; d.srKind = SrKind::None; d.net.reset(); d.editFlow = false; d.outW = w; d.outH = h; };
        for (const SrCandidate& cand : so.order) {
            const std::string name = Fmt("%s x%d", SrKindName(cand.kind), cand.factor);
            d.srKind = cand.kind; d.srFactor = cand.factor;
            d.outW = w * cand.factor; d.outH = h * cand.factor;
            d.srFormats.clear(); d.srSrcAttrs = d.srDstAttrs = nil;
            bool set = false;
            if (cand.kind == SrKind::ArtCnn32 || cand.kind == SrKind::ArtCnn16) {
                d.net = cand.kind == SrKind::ArtCnn32 ? net32 : net16;
                set = d.net != nullptr;
            }
#ifdef PMVS_VT_LL
            else if (@available(macOS 26.0, *)) {
                id cfgObj = nil;
                if (cand.kind == SrKind::LowLatency) {
                    VTLowLatencySuperResolutionScalerConfiguration* sc = [[VTLowLatencySuperResolutionScalerConfiguration alloc] initWithFrameWidth:w frameHeight:h scaleFactor:float(cand.factor)];
                    if (sc) { d.srFormats = Formats(sc.frameSupportedPixelFormats); d.srSrcAttrs = sc.sourcePixelBufferAttributes; d.srDstAttrs = sc.destinationPixelBufferAttributes; }
                    cfgObj = sc;
                } else {
                    // shared flow when there is a flow session and Metal to scale it per phase step
                    d.editFlow = d.flowProc && d.metal && (d.cfg.k < 2 || has(d.flowFormats, d.ofmt) || d.flowFormats.empty());
                    VTSuperResolutionScalerConfiguration* sc = [[VTSuperResolutionScalerConfiguration alloc] initWithFrameWidth:w frameHeight:h scaleFactor:cand.factor
                        inputType:VTSuperResolutionScalerConfigurationInputTypeVideo usePrecomputedFlow:d.editFlow
                        qualityPrioritization:VTSuperResolutionScalerConfigurationQualityPrioritizationNormal revision:VTSuperResolutionScalerConfiguration.defaultRevision];
                    if (sc) { d.srFormats = Formats(sc.frameSupportedPixelFormats); d.srSrcAttrs = sc.sourcePixelBufferAttributes; d.srDstAttrs = sc.destinationPixelBufferAttributes; }
                    cfgObj = sc;
                }
                NSError* e = nil;
                VTFrameProcessor* p = cfgObj ? [[VTFrameProcessor alloc] init] : nil;
                if (!cfgObj) notes.push_back(name + ": configuration not offered");
                else if (![p startSessionWithConfiguration:cfgObj error:&e]) notes.push_back(name + ": startSession failed: " + ErrText(e));
                else { d.srProc = p; set = true; }
            }
#endif
            if (!set) { endSr(); continue; }
            // formats: super resolution takes the output stage's format (ArtCNN takes any of ours)
            const bool art = cand.kind == SrKind::ArtCnn32 || cand.kind == SrKind::ArtCnn16;
            if (!art) {
                if (d.fi != Fi::None && !has(d.srFormats, d.ofmt)) {
                    const uint32_t sf = d.rgb ? PickFormat(d.srFormats, c.bits, c.colour.fullRange) : 0;
                    if (sf) d.fmt = d.ofmt = sf;
                    else { notes.push_back(name + " shares no format with the interpolator (" + FormatList(d.srFormats) + ")"); endSr(); continue; }
                } else if (d.fi == Fi::None) {
                    const uint32_t sf = PickFormat(d.srFormats, c.bits, c.colour.fullRange, &d.fmtLossy);
                    if (!sf) { notes.push_back(name + " lists no 4:2:0 format we can feed (" + FormatList(d.srFormats) + ")"); endSr(); continue; }
                    d.fmt = d.vtFmt = d.ofmt = sf;
                }
            }
            std::string perr;
            if (!d.MakePools(perr)) { notes.push_back(name + ": " + perr); endSr(); continue; }
            double ms = 0, cost = 0;
            if (!d.SelfTestSr(perr, &ms)) { notes.push_back(name + " does not run here (self-test: " + perr + ")"); endSr(); continue; }
            const double flowMs = (d.editFlow || d.recover || d.frcFlow) ? d.stats.benchFlowMs : 0.0;
            if (!FitsRealTime(ms, d.stats.benchFiMs, flowMs, std::max(1, d.cfg.k), c.srcFps, &cost)) {
                notes.push_back(Fmt("%s is too slow here (%.1f ms a frame, %.1f ms per source frame with interpolation; budget %.1f ms)", name.c_str(), ms, cost, 1000.0 / (c.srcFps > 0 ? c.srcFps : 23.976) * 0.85));
                endSr(); continue;
            }
            d.stats.benchSrMs = ms;
            notes.push_back(Fmt("%s: %.1f ms a frame", name.c_str(), ms));
            break;
        }
        if (!d.srFactor) { d.outW = w; d.outH = h; if (d.fi == Fi::None) { err = "nothing for VideoToolbox to do (x1 and no super resolution): " + joined(); return false; } }
        if (!d.MakePools(err)) return false;
        // the flow session stays only when a stage uses it
        if (d.flowProc && !(d.frcFlow || d.recover || (d.srKind == SrKind::Editing && d.editFlow))) { if (@available(macOS 15.4, *)) [d.flowProc endSession]; d.flowProc = nil; }

        const Stats bench = d.stats;
        d.stats = Stats();
        d.stats.benchFiMs = bench.benchFiMs; d.stats.benchSrMs = bench.benchSrMs; d.stats.benchFlowMs = bench.benchFlowMs;
        d.ladder.Init(c.srcFps, d.srFactor > 0, d.fi == Fi::LowLatency ? d.exponent : d.fi == Fi::Frc ? 2 : 0);

        const PhasePlan plan = d.fi == Fi::LowLatency ? MakePlan(d.cfg.k, d.exponent) : MakePlan(d.cfg.k, 0, true);
        std::string phases;
        for (float t : plan.phases) phases += Fmt("%s%.4g", phases.empty() ? "" : " ", t);
        d.chain = d.fi == Fi::LowLatency ? Fmt("low-latency interpolation x%d (grid 1/%d, phases %s, max phase error %.3f frame)", d.cfg.k, 1 << d.exponent, phases.c_str(), plan.maxError)
                : d.fi == Fi::Frc ? Fmt("frame-rate conversion x%d (phases %s%s)", d.cfg.k, phases.c_str(), d.frcFlow ? ", shared optical flow" : "")
                : std::string("no interpolation");
        if (d.recover) d.chain += " + 10-bit recovery on Metal";
        if (d.srFactor) d.chain += Fmt(" + %s x%d (%dx%d -> %dx%d%s)", SrKindName(d.srKind), d.srFactor, w, h, d.outW, d.outH, d.editFlow ? ", shared optical flow" : "");
        d.chain += Fmt(", pixel format %s%s%s", FourCCText(d.fmt).c_str(), d.rgb ? (" via " + FourCCText(d.vtFmt)).c_str() : "", d.fmtLossy && !d.recover ? " (fewer bits than the source)" : "");
        if (d.ofmt != d.fmt) d.chain += ", output " + FourCCText(d.ofmt);
        info = d.chain;
        if (!notes.empty()) info += " [" + joined() + "]";
        return true;
    }
}

void Engine::Forget() { m->ResetPair(); m->lastCut = false; }

namespace {
// the shared middle of Process / ProcessBuffer: the new frame is uploaded (d.cur, d.cur16, d.curVt); runs the pair
bool Step(Engine::Impl& d, int64_t pts, const Thumb& thumb, std::vector<Buf>& out, bool& havePair, std::string& err, const std::function<void(const std::string&)>& log)
{
    if (d.rgb && d.fi != Fi::None && d.cfg.k >= 2) { d.curVt = d.Convert(d.cur, d.vtSrcPool, err); if (!d.curVt) return false; }
    d.curPts = pts;
    d.curThumb = thumb;
    d.haveCur = true;
    const double t0 = NowMs();
    if (d.havePrev && d.cfg.k >= 2) {
        havePair = true;
        ++d.stats.pairs;
        d.lastCut = IsCut(ScoreCut(d.prevThumb, d.curThumb));
        if (d.lastCut) ++d.stats.cuts;
        const bool sequential = !d.freshPair;
        d.freshPair = false;
        if (!d.Interpolate(sequential, out, err, log)) return false;
        const double ms = NowMs() - t0;
        d.stats.pairMs = d.stats.pairMs <= 0 ? ms : d.stats.pairMs * 0.95 + ms * 0.05;
        if (d.ladder.Record(ms) && log)
            log(Fmt("apple: pairs take %.1f ms (budget %.1f ms per source frame): stepping down to %s", d.ladder.MeanMs(), d.ladder.BudgetMs(), d.ladder.Describe().c_str()));
    } else if (d.srFactor) {
        d.curOut = d.Upscale(d.Src(), d.curPts, 1.0f, true, err, log);
        if (!d.curOut) return false;
    }
    return true;
}
void Shift(Engine::Impl& d)
{
    if (d.haveCur) { d.prev = d.cur; d.prev16 = d.cur16; d.prevVt = d.curVt; d.prevOut = d.curOut; d.prevPts = d.curPts; d.prevThumb = d.curThumb; d.havePrev = true; }
    d.curOut = Buf();
}
} // namespace

bool Engine::Process(const PlanarView& v, const Colour& colour, int64_t pts, bool contiguous, const std::vector<PlanarOut>& gen, const PlanarOut& curOut,
                     bool& havePair, std::string& err)
{
    @autoreleasepool {
        Impl& d = *m;
        havePair = false;
        if (!d.colourSet) { d.colourSet = true; if (colour.fullRange != d.colour.fullRange) { d.colour = colour; d.FollowRange(colour.fullRange, log); } }
        d.colour = colour;
        if (!contiguous) d.ResetPair();
        Shift(d);
        Buf b = d.srcPool.Get();
        if (!b) { err = "no source buffer from the pool"; return false; }
        Tag(b.ref, colour);
        {
            Locked l(b.ref, false);
            if (!l.Ok()) { err = "could not lock a source buffer"; return false; }
            PlanarToBi(v, l.v);
        }
        d.cur = b; d.curVt = b;
        if (d.recover) {
            Buf b16 = d.src16Pool.Get();
            if (!b16) { err = "no 16-bit source buffer"; return false; }
            Tag(b16.ref, colour);
            Locked l(b16.ref, false);
            if (!l.Ok()) { err = "could not lock a source buffer"; return false; }
            PlanarToBi(v, l.v);
            d.cur16 = b16;
        }
        std::vector<Buf> out;
        if (!Step(d, pts, MakeThumb(v), out, havePair, err, log)) return false;
        for (size_t i = 0; i < out.size() && i < gen.size(); ++i) if (!d.Emit(out[i], gen[i], err)) return false;
        // the real frame: bit-exact copy without super resolution
        if (d.srFactor) { if (!d.Emit(d.curOut, curOut, err)) return false; }
        else CopyPlanar(v, curOut);
        return true;
    }
}

bool Engine::ProcessBuffer(void* pixelBuffer, const Colour& colour, int64_t pts, bool contiguous, std::vector<void*>& gen, void*& curRet, bool& havePair, std::string& err)
{
    @autoreleasepool {
        Impl& d = *m;
        havePair = false;
        gen.clear(); curRet = nullptr;
        CVPixelBufferRef in = static_cast<CVPixelBufferRef>(pixelBuffer);
        if (!in) { err = "no input buffer"; return false; }
        if (int(CVPixelBufferGetWidth(in)) != d.cfg.width || int(CVPixelBufferGetHeight(in)) != d.cfg.height) { err = "the input size changed"; return false; }
        if (!d.colourSet) { d.colourSet = true; if (colour.fullRange != d.colour.fullRange) { d.colour = colour; d.FollowRange(colour.fullRange, log); } }
        d.colour = colour;
        if (!contiguous) d.ResetPair();
        Shift(d);
        const uint32_t inFmt = CVPixelBufferGetPixelFormatType(in);
        const bool ioSurface = CVPixelBufferGetIOSurface(in) != nullptr;
        Buf inBuf(CVPixelBufferRetain(in));
        // the interpolator's copy: the input itself when the format matches (zero copy)
        if (inFmt == d.fmt && ioSurface) d.cur = inBuf;
        else if (d.recover && FormatBits(inFmt) == 10 && FormatBits(d.fmt) == 8) {
            Buf b = d.srcPool.Get();
            if (!b) { err = "no source buffer"; return false; }
            Locked li(in, true), lo(b.ref, false);
            if (!li.Ok() || !lo.Ok()) { err = "could not lock a buffer"; return false; }
            Bi16To8(li.v, lo.v);
            d.cur = b;
        } else { d.cur = d.Convert(inBuf, d.srcPool, err); if (!d.cur) return false; }
        Tag(d.cur.ref, colour);
        d.curVt = d.cur;
        if (d.recover) {
            if (inFmt == d.ofmt && ioSurface) d.cur16 = inBuf;
            else { d.cur16 = d.Convert(inBuf, d.src16Pool, err); if (!d.cur16) return false; }
        }
        Thumb th;
        {
            Locked l(d.cur.ref, true);
            if (!l.Ok()) { err = "could not lock a buffer"; return false; }
            // the thumbnail from the Y plane (8-bit, or the high byte of 16-bit words)
            PlanarView pv; pv.width = l.v.width; pv.height = l.v.height; pv.bits = l.v.bytes == 2 ? 16 : 8;
            pv.data[0] = l.v.data[0]; pv.stride[0] = l.v.stride[0];
            th = MakeThumb(pv);
        }
        std::vector<Buf> out;
        if (!Step(d, pts, th, out, havePair, err, log)) return false;
        for (const Buf& b : out) gen.push_back(CVPixelBufferRetain(b.ref));
        const Buf& c = d.srFactor ? d.curOut : (d.recover ? d.cur16 : inBuf);
        curRet = CVPixelBufferRetain(c.ref);
        return true;
    }
}

#else // no VTFrameProcessor in this SDK: the backend reports itself unavailable, the plugin uses the portable one

const Caps& Probe()
{
    static Caps caps = [] {
        Caps c;
        c.sdk = "SDK " + std::to_string(int(__MAC_OS_X_VERSION_MAX_ALLOWED));
        c.detail = c.sdk + ": built without VTFrameProcessor (needs the macOS 15.4 SDK or newer): the apple backend is not available";
        return c;
    }();
    return caps;
}
std::string ProbeReport() { return "apple probe: " + Probe().detail + std::string(1, '\n'); }
struct Engine::Impl { Stats stats; };
Engine::Engine() : m(std::make_unique<Impl>()) {}
Engine::~Engine() = default;
bool Engine::Configure(const Config&, std::string& err, std::string&) { err = Probe().detail; return false; }
int Engine::OutWidth() const { return 0; }
int Engine::OutHeight() const { return 0; }
int Engine::K() const { return 1; }
bool Engine::Process(const PlanarView&, const Colour&, int64_t, bool, const std::vector<PlanarOut>&, const PlanarOut&, bool& havePair, std::string& err) { havePair = false; err = "unavailable"; return false; }
void Engine::Forget() {}
bool Engine::LastPairWasCut() const { return false; }
const Stats& Engine::Stat() const { return m->stats; }
std::string Engine::Describe() const { return "unavailable"; }
bool Engine::ProcessBuffer(void*, const Colour&, int64_t, bool, std::vector<void*>& gen, void*& cur, bool& havePair, std::string& err) { gen.clear(); cur = nullptr; havePair = false; err = "unavailable"; return false; }
int SrModelStatus(int, int, float*) { return -1; }
bool SrModelDownload(double, const std::function<void(const std::string&)>&, std::string& err) { err = Probe().detail; return false; }

#endif

} // namespace pmvs::apple
