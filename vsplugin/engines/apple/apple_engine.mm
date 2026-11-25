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
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>
#include "apple_engine.h"

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
                           "frame-rate conversion %s; super resolution (editing) %s; temporal noise filter %s; macOS 27 size queries %s",
                           c.os.c_str(), c.chip.empty() ? "unknown chip" : c.chip.c_str(), model.c_str(), c.sdk.c_str(), c.frameProcessor ? "present" : "absent",
                           st(ll), st(sr), st(frc), st(fsr), st(tnf), c.sizeQueries ? "present" : "absent");
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
                for (int x = 1; x <= 3; ++x) {
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

struct Engine::Impl {
    Config cfg;
    Fi fi = Fi::None;
    int exponent = 0;          // the low-latency grid (configured)
    int srFactor = 0;
    int outW = 0, outH = 0;
    uint32_t fmt = 0;          // our YUV 4:2:0 format (uploads, real frames, super resolution, outputs)
    uint32_t vtFmt = 0;        // what the interpolator takes: fmt, or RGhA (frame-rate conversion lists only that)
    bool rgb = false;          // vtFmt is RGhA: YUV <-> RGB through VTPixelTransferSession, tagged both ways
    bool fmtLossy = false;
    std::vector<uint32_t> fiFormats, srFormats;
    Colour colour; bool colourSet = false;
    Ladder ladder;
    Stats stats;
    int consecutiveErrors = 0;
    bool lastCut = false;
    std::string chain;

    id fiProc = nil;           // VTFrameProcessor (macOS 15.4+)
    id srProc = nil;
    NSDictionary* fiSrcAttrs = nil; NSDictionary* fiDstAttrs = nil; NSDictionary* srSrcAttrs = nil; NSDictionary* srDstAttrs = nil;
    Pool srcPool, vtSrcPool, fiDstPool, srDstPool;
    VTPixelTransferSessionRef scaler = nullptr, xfer = nullptr;

    // the pair
    Buf prev, cur;             // source size, fmt
    Buf prevVt, curVt;         // the interpolator's copies (the same buffers unless rgb)
    Buf prevOut, curOut;       // output size (super resolution / scale of prev, cur); empty without super resolution
    int64_t prevPts = 0, curPts = 0;
    Thumb prevThumb, curThumb;
    bool havePrev = false, haveCur = false;
    bool freshPair = true;     // the next pair is the first since a reset (FRC: random submission, clears its cache)

    ~Impl()
    {
        if (scaler) { VTPixelTransferSessionInvalidate(scaler); CFRelease(scaler); }
        if (xfer) { VTPixelTransferSessionInvalidate(xfer); CFRelease(xfer); }
        if (@available(macOS 15.4, *)) { if (fiProc) [fiProc endSession]; if (srProc) [srProc endSession]; }
    }
    void Log(const std::function<void(const std::string&)>& log, const std::string& s) const { if (log) log(s); }

    bool MakePools(std::string& err)
    {
        const int w = cfg.width, h = cfg.height;
        NSDictionary* srcBase = fi != Fi::None && !rgb ? fiSrcAttrs : srSrcAttrs;
        if (!srcPool.Make(srcBase, w, h, fmt, err)) return false;
        if (rgb && !vtSrcPool.Make(fiSrcAttrs, w, h, vtFmt, err)) return false;
        if (fi != Fi::None && !fiDstPool.Make(fiDstAttrs, w, h, vtFmt, err)) return false;
        if (srFactor && !srDstPool.Make(srDstAttrs, outW, outH, fmt, err)) return false;
        return true;
    }
    void ResetPair() { prev = cur = prevVt = curVt = prevOut = curOut = Buf(); havePrev = haveCur = false; freshPair = true; }
    // the first frame's range differs from the guess: the other range tag, if every processor lists it
    void FollowRange(bool fullRange, const std::function<void(const std::string&)>& log)
    {
        const uint32_t want = FormatBits(fmt) == 10 ? (fullRange ? kxf20 : kx420) : (fullRange ? k420f : k420v);
        if (want == fmt) return;
        auto listed = [&](const std::vector<uint32_t>& l) { return l.empty() || std::find(l.begin(), l.end(), want) != l.end(); };
        if ((!rgb && fi != Fi::None && !listed(fiFormats)) || (srFactor && !listed(srFormats))) return;   // values pass unchanged either way; only the tag differs
        const uint32_t old = fmt;
        fmt = want;
        if (!rgb) vtFmt = want;
        std::string err;
        if (!MakePools(err)) { fmt = old; if (!rgb) vtFmt = old; MakePools(err); Log(log, "apple: could not switch to " + FourCCText(want) + ": " + err); return; }
        ResetPair();
        Log(log, "apple: the stream is " + std::string(fullRange ? "full" : "limited") + " range: pixel format " + FourCCText(want));
    }
    // a format conversion (YUV <-> RGhA) into a buffer of pool
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
                    const int v8 = p ? 128 : shade + (x * 32) / std::max(1, n);
                    if (l.v.bytes == 1) r[x] = uint8_t(v8); else { const uint16_t u = uint16_t(v8 << 8); std::memcpy(r + 2 * x, &u, 2); }
                }
            }
        }
    }

    // one super-resolution call (no fallback)
    bool RunSr(const Buf& src, const Buf& dst, int64_t pts, NSError* __autoreleasing* e)
    {
#ifdef PMVS_VT_LL
        if (@available(macOS 26.0, *)) {
            VTFrameProcessorFrame* s = [[VTFrameProcessorFrame alloc] initWithBuffer:src.ref presentationTimeStamp:Pts(pts)];
            VTFrameProcessorFrame* d = [[VTFrameProcessorFrame alloc] initWithBuffer:dst.ref presentationTimeStamp:Pts(pts)];
            if (!s || !d) return false;
            VTLowLatencySuperResolutionScalerParameters* p = [[VTLowLatencySuperResolutionScalerParameters alloc] initWithSourceFrame:s destinationFrame:d];
            ++stats.vtCalls;
            if (p && [srProc processWithParameters:p error:e]) return true;
            ++stats.vtErrors; ++stats.srErrors;
        }
#endif
        return false;
    }
    // one interpolation call: the frames at phases (vtFmt buffers from fiDstPool, one per phase)
    bool RunFi(const Buf& p0, const Buf& c0, int64_t pp, int64_t cp, const std::vector<float>& phases, std::vector<Buf>& dst, bool sequential, NSError* __autoreleasing* e)
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
            if (fi == Fi::Frc)
                params = [[VTFrameRateConversionParameters alloc] initWithSourceFrame:ps nextFrame:cs opticalFlow:nil interpolationPhase:ph
                    submissionMode:(sequential ? VTFrameRateConversionParametersSubmissionModeSequential : VTFrameRateConversionParametersSubmissionModeRandom)
                    destinationFrames:frames];
            ++stats.vtCalls;
            if (ps && cs && params && [fiProc processWithParameters:params error:e]) return true;
            ++stats.vtErrors; ++stats.fiErrors;
        }
        dst.clear();
        return false;
    }

    // src (source size, fmt) -> output size: super resolution, or the plain scale once the ladder dropped it
    Buf Upscale(const Buf& src, int64_t pts, std::string& err, const std::function<void(const std::string&)>& log)
    {
        if (!srFactor) return src;
        Buf dst = srDstPool.Get();
        if (!dst) { err = "no super-resolution output buffer"; return Buf(); }
        Tag(dst.ref, colour);
        if (ladder.SrActive()) {
            NSError* e = nil;
            if (RunSr(src, dst, pts, &e)) return dst;
            if (stats.srErrors <= 3 || stats.srErrors % 240 == 0) Log(log, "apple: super resolution failed: " + ErrText(e) + "; scaling this frame instead");
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

    // the k - 1 generated buffers of the pair (output size, fmt), by the plan; false only on a hard error
    bool Interpolate(bool sequential, std::vector<Buf>& out, std::string& err, const std::function<void(const std::string&)>& log)
    {
        const int k = cfg.k;
        PhasePlan plan;
        if (lastCut || fi == Fi::None) plan = MakePlan(k, 0);
        else if (fi == Fi::Frc && ladder.Level() < 2) plan = MakePlan(k, 0, true);
        else plan = MakePlan(k, ladder.Exponent());
        std::vector<Buf> vt;   // source size, fmt, one per plan phase
        if (!plan.phases.empty()) {
            NSError* e = nil;
            std::vector<Buf> raw;
            bool ok = RunFi(prevVt, curVt, prevPts, curPts, plan.phases, raw, sequential, &e);
            std::string cerr;
            if (ok)
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
        out.clear();
        for (int j = 1; j < k; ++j) {
            const int s = plan.slot[size_t(j - 1)];
            if (s == kPrev) out.push_back(srFactor ? prevOut : prev);
            else if (s == kCur) out.push_back(srFactor ? curOut : cur);
            else {
                const int64_t pts = prevPts + int64_t(std::llround(double(curPts - prevPts) * double(j) / k));
                Buf b = Upscale(vt[size_t(s)], pts, err, log);
                if (!b) return false;
                out.push_back(b);
                ++stats.generated;
            }
        }
        return true;
    }

    // the effects really run here (a VM can list them and still fail every call): one call each on synthetic frames
    bool SelfTestSr(std::string& err)
    {
        Buf s = srcPool.Get(), d = srDstPool.Get();
        if (!s || !d) { err = "no buffers"; return false; }
        Tag(s.ref, colour); Tag(d.ref, colour);
        FillGrey(s, 60);
        NSError* e = nil;
        if (!RunSr(s, d, 0, &e)) { err = ErrText(e); return false; }
        return true;
    }
    bool SelfTestFi(std::string& err)
    {
        Buf a = srcPool.Get(), b = srcPool.Get();
        if (!a || !b) { err = "no buffers"; return false; }
        Tag(a.ref, colour); Tag(b.ref, colour);
        FillGrey(a, 60); FillGrey(b, 80);
        Buf av = a, bv = b;
        if (rgb) { av = Convert(a, vtSrcPool, err); bv = Convert(b, vtSrcPool, err); if (!av || !bv) return false; }
        NSError* e = nil;
        std::vector<Buf> out;
        if (!RunFi(av, bv, 0, 417083, { 0.5f }, out, false, &e)) { err = ErrText(e); return false; }
        if (rgb && !Convert(out[0], srcPool, err)) return false;
        return true;
    }
};

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

        // ---- interpolation
        if (d.cfg.k >= 2) {
            const bool wantLL = fiMode != "frc", wantFrc = fiMode != "lowlatency";
#ifdef PMVS_VT_LL
            if (wantLL && caps.lowLatencyFI) {
                if (@available(macOS 26.0, *)) {
                    for (int x = ExponentFor(d.cfg.k); x >= 1 && d.fi == Fi::None; --x) {
                        VTLowLatencyFrameInterpolationConfiguration* fc = [[VTLowLatencyFrameInterpolationConfiguration alloc] initWithFrameWidth:w frameHeight:h numberOfInterpolatedFrames:x];
                        if (!fc) { notes.push_back(Fmt("low-latency interpolation with %d interpolated frames: not offered for %dx%d", x, w, h)); continue; }
                        VTFrameProcessor* p = [[VTFrameProcessor alloc] init];
                        NSError* e = nil;
                        if (![p startSessionWithConfiguration:fc error:&e]) { notes.push_back("low-latency interpolation: startSession failed: " + ErrText(e)); break; }
                        d.fiProc = p; d.fi = Fi::LowLatency; d.exponent = x;
                        d.fiFormats = Formats(fc.frameSupportedPixelFormats);
                        d.fiSrcAttrs = fc.sourcePixelBufferAttributes; d.fiDstAttrs = fc.destinationPixelBufferAttributes;
                    }
                }
            } else if (wantLL) notes.push_back("low-latency interpolation not supported here");
#endif
            if (d.fi == Fi::None && wantFrc && caps.frc) {
                if (@available(macOS 15.4, *)) {
                    VTFrameRateConversionConfiguration* fc = [[VTFrameRateConversionConfiguration alloc] initWithFrameWidth:w frameHeight:h usePrecomputedFlow:NO
                        qualityPrioritization:VTFrameRateConversionConfigurationQualityPrioritizationNormal revision:VTFrameRateConversionConfiguration.defaultRevision];
                    if (!fc) notes.push_back(Fmt("frame-rate conversion: not offered for %dx%d", w, h));
                    else {
                        VTFrameProcessor* p = [[VTFrameProcessor alloc] init];
                        NSError* e = nil;
                        if (![p startSessionWithConfiguration:fc error:&e]) notes.push_back("frame-rate conversion: startSession failed: " + ErrText(e));
                        else {
                            d.fiProc = p; d.fi = Fi::Frc; d.exponent = ExponentFor(d.cfg.k);
                            d.fiFormats = Formats(fc.frameSupportedPixelFormats);
                            d.fiSrcAttrs = fc.sourcePixelBufferAttributes; d.fiDstAttrs = fc.destinationPixelBufferAttributes;
                        }
                    }
                }
            } else if (d.fi == Fi::None && wantFrc) notes.push_back("frame-rate conversion not supported here");
            if (d.fi == Fi::None) {
                err = "no VideoToolbox interpolator for " + std::to_string(w) + "x" + std::to_string(h) + (notes.empty() ? std::string() : ": " + joined()) + " (" + caps.detail + ")";
                return false;
            }
        }

        // ---- super resolution (after interpolation, on every output frame)
        std::vector<float> factors;
        int maxDim2 = 0, maxDim4 = 0;
#ifdef PMVS_VT_LL
        if (caps.lowLatencySR) {
            if (@available(macOS 26.0, *)) {
                for (NSNumber* n in [VTLowLatencySuperResolutionScalerConfiguration supportedScaleFactorsForFrameWidth:w frameHeight:h]) factors.push_back(n.floatValue);
                const CMVideoDimensions mx = VTLowLatencySuperResolutionScalerConfiguration.maximumDimensions;
                if (mx.width > 0 && mx.height > 0) maxDim2 = maxDim4 = std::max(int(mx.width), int(mx.height));
                const long q2 = ClassIntForFloat("VTLowLatencySuperResolutionScalerConfiguration", "maximumDimensionForSpatialScaleFactor:", 2.0f);
                const long q4 = ClassIntForFloat("VTLowLatencySuperResolutionScalerConfiguration", "maximumDimensionForSpatialScaleFactor:", 4.0f);
                if (q2 > 0) maxDim2 = int(q2);
                if (q4 > 0) maxDim4 = int(q4);
            }
        }
#endif
        const SrChoice src = DecideSr(c.sr, w, h, factors, maxDim2, maxDim4);
        notes.push_back(src.why);
#ifdef PMVS_VT_LL
        if (src.factor) {
            if (@available(macOS 26.0, *)) {
                VTLowLatencySuperResolutionScalerConfiguration* sc = [[VTLowLatencySuperResolutionScalerConfiguration alloc] initWithFrameWidth:w frameHeight:h scaleFactor:float(src.factor)];
                VTFrameProcessor* p = sc ? [[VTFrameProcessor alloc] init] : nil;
                NSError* e = nil;
                if (!sc) notes.push_back("super resolution: configuration not offered: off");
                else if (![p startSessionWithConfiguration:sc error:&e]) notes.push_back("super resolution: startSession failed: " + ErrText(e) + ": off");
                else {
                    d.srProc = p; d.srFactor = src.factor;
                    d.srFormats = Formats(sc.frameSupportedPixelFormats);
                    d.srSrcAttrs = sc.sourcePixelBufferAttributes; d.srDstAttrs = sc.destinationPixelBufferAttributes;
                }
            }
        }
#endif
        if (d.fi == Fi::None && !d.srFactor) { err = "nothing for VideoToolbox to do (x1 and no super resolution): " + src.why; return false; }
        auto dropSr = [&](const std::string& why) {
            notes.push_back(why);
            if (@available(macOS 15.4, *)) { if (d.srProc) [d.srProc endSession]; }
            d.srProc = nil; d.srFactor = 0;
        };

        // ---- pixel formats: the interpolator's own 4:2:0 format, else RGhA with our YUV format around it
        const uint32_t yuvPref = c.bits > 8 ? (c.colour.fullRange ? kxf20 : kx420) : (c.colour.fullRange ? k420f : k420v);
        if (d.fi != Fi::None) {
            const uint32_t f = PickFormat(d.fiFormats, c.bits, c.colour.fullRange, &d.fmtLossy);
            if (f) d.fmt = d.vtFmt = f;
            else if (has(d.fiFormats, kRGhA)) { d.rgb = true; d.vtFmt = kRGhA; d.fmt = yuvPref; d.fmtLossy = false; }
            else { err = "the VideoToolbox interpolator lists no format we can feed (" + FormatList(d.fiFormats) + ")"; return false; }
            if (d.srFactor && !has(d.srFormats, d.fmt)) {
                const uint32_t sf = d.rgb ? PickFormat(d.srFormats, c.bits, c.colour.fullRange) : 0;
                if (sf) d.fmt = sf;
                else dropSr("super resolution shares no format with the interpolator (" + FormatList(d.srFormats) + "): off");
            }
        } else {
            d.fmt = d.vtFmt = PickFormat(d.srFormats, c.bits, c.colour.fullRange, &d.fmtLossy);
            if (!d.fmt) { err = "VideoToolbox super resolution lists no 4:2:0 format we can feed (" + FormatList(d.srFormats) + ")"; return false; }
        }
        d.outW = w * std::max(1, d.srFactor); d.outH = h * std::max(1, d.srFactor);
        if (!d.MakePools(err)) return false;

        // ---- the effects must really run (a VM lists them and fails every call)
        std::string terr;
        if (d.srFactor && !d.SelfTestSr(terr)) {
            dropSr("super resolution self-test failed (" + terr + "): off");
            if (d.fi == Fi::None) { err = "VideoToolbox super resolution does not run here: " + terr; return false; }
            d.outW = w; d.outH = h;
            if (!d.MakePools(err)) return false;
        }
        if (d.fi != Fi::None && !d.SelfTestFi(terr)) {
            err = std::string(d.fi == Fi::LowLatency ? "low-latency interpolation" : "frame-rate conversion") + " is listed but does not run here (self-test: " + terr + ")" +
                  (notes.empty() ? std::string() : "; " + joined()) + " (" + caps.detail + ")";
            return false;
        }
        d.stats = Stats();
        d.ladder.Init(c.srcFps, d.srFactor > 0, d.fi == Fi::LowLatency ? d.exponent : d.fi == Fi::Frc ? 2 : 0);

        const PhasePlan plan = d.fi == Fi::LowLatency ? MakePlan(d.cfg.k, d.exponent) : MakePlan(d.cfg.k, 0, true);
        std::string phases;
        for (float t : plan.phases) phases += Fmt("%s%.4g", phases.empty() ? "" : " ", t);
        d.chain = d.fi == Fi::LowLatency ? Fmt("low-latency interpolation x%d (grid 1/%d, phases %s, max phase error %.3f frame)", d.cfg.k, 1 << d.exponent, phases.c_str(), plan.maxError)
                : d.fi == Fi::Frc ? Fmt("frame-rate conversion x%d (phases %s)", d.cfg.k, phases.c_str())
                : std::string("no interpolation");
        if (d.srFactor) d.chain += Fmt(" + low-latency super resolution x%d (%dx%d -> %dx%d)", d.srFactor, w, h, d.outW, d.outH);
        d.chain += Fmt(", pixel format %s%s%s", FourCCText(d.fmt).c_str(), d.rgb ? (" via " + FourCCText(d.vtFmt)).c_str() : "", d.fmtLossy ? " (fewer bits than the source)" : "");
        info = d.chain;
        if (!notes.empty()) info += " [" + joined() + "]";
        return true;
    }
}

void Engine::Forget() { m->ResetPair(); m->lastCut = false; }

bool Engine::Process(const PlanarView& v, const Colour& colour, int64_t pts, bool contiguous, const std::vector<PlanarOut>& gen, const PlanarOut& curOut,
                     bool& havePair, std::string& err)
{
    @autoreleasepool {
        Impl& d = *m;
        havePair = false;
        if (!d.colourSet) { d.colourSet = true; if (colour.fullRange != d.colour.fullRange) { d.colour = colour; d.FollowRange(colour.fullRange, log); } }
        d.colour = colour;
        if (!contiguous) d.ResetPair();
        // shift the pair, upload the new frame
        if (d.haveCur) { d.prev = d.cur; d.prevVt = d.curVt; d.prevOut = d.curOut; d.prevPts = d.curPts; d.prevThumb = d.curThumb; d.havePrev = true; }
        Buf b = d.srcPool.Get();
        if (!b) { err = "no source buffer from the pool"; return false; }
        Tag(b.ref, colour);
        {
            Locked l(b.ref, false);
            if (!l.Ok()) { err = "could not lock a source buffer"; return false; }
            PlanarToBi(v, l.v);
        }
        d.cur = b;
        d.curVt = b;
        if (d.rgb && d.fi != Fi::None && d.cfg.k >= 2) { d.curVt = d.Convert(b, d.vtSrcPool, err); if (!d.curVt) return false; }
        d.curPts = pts;
        d.curThumb = MakeThumb(v);
        d.haveCur = true;
        const double t0 = NowMs();
        if (d.srFactor) { d.curOut = d.Upscale(d.cur, d.curPts, err, log); if (!d.curOut) return false; }
        if (d.havePrev && d.cfg.k >= 2) {
            havePair = true;
            ++d.stats.pairs;
            d.lastCut = IsCut(ScoreCut(d.prevThumb, d.curThumb));
            if (d.lastCut) ++d.stats.cuts;
            std::vector<Buf> out;
            const bool sequential = !d.freshPair;
            d.freshPair = false;
            if (!d.Interpolate(sequential, out, err, log)) return false;
            for (size_t i = 0; i < out.size() && i < gen.size(); ++i) if (!d.Emit(out[i], gen[i], err)) return false;
            const double ms = NowMs() - t0;
            d.stats.pairMs = d.stats.pairMs <= 0 ? ms : d.stats.pairMs * 0.95 + ms * 0.05;
            if (d.ladder.Record(ms) && log)
                log(Fmt("apple: pairs take %.1f ms (budget %.1f ms per source frame): stepping down to %s", d.ladder.MeanMs(), d.ladder.BudgetMs(), d.ladder.Describe().c_str()));
        }
        // the real frame: bit-exact copy without super resolution
        if (d.srFactor) { if (!d.Emit(d.curOut, curOut, err)) return false; }
        else CopyPlanar(v, curOut);
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
std::string ProbeReport() { return "apple probe: " + Probe().detail + "
"; }
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

#endif

} // namespace pmvs::apple
