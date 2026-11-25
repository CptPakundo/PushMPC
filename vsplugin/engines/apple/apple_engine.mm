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
            const int frc = ClassBool("VTFrameRateConversionConfiguration", "isSupported");
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
    uint32_t fmt = 0;
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
    Pool srcPool, fiDstPool, srDstPool;
    VTPixelTransferSessionRef scaler = nullptr;

    // the pair
    Buf prev, cur;             // source size
    Buf prevOut, curOut;       // output size (super resolution / scale of prev, cur); empty without super resolution
    int64_t prevPts = 0, curPts = 0;
    Thumb prevThumb, curThumb;
    bool havePrev = false, haveCur = false;
    bool freshPair = true;     // the next pair is the first since a reset (FRC: random submission, clears its cache)

    ~Impl()
    {
        if (scaler) { VTPixelTransferSessionInvalidate(scaler); CFRelease(scaler); }
        if (@available(macOS 15.4, *)) { if (fiProc) [fiProc endSession]; if (srProc) [srProc endSession]; }
    }
    void Log(const std::function<void(const std::string&)>& log, const std::string& s) const { if (log) log(s); }

    bool MakePools(std::string& err)
    {
        const int w = cfg.width, h = cfg.height;
        NSDictionary* srcBase = fi != Fi::None ? fiSrcAttrs : srSrcAttrs;
        if (!srcPool.Make(srcBase, w, h, fmt, err)) return false;
        if (fi != Fi::None && !fiDstPool.Make(fiDstAttrs, w, h, fmt, err)) return false;
        if (srFactor && !srDstPool.Make(srDstAttrs, outW, outH, fmt, err)) return false;
        return true;
    }
    // the first frame's range differs from the guess: the other range tag, if every processor lists it
    void FollowRange(bool fullRange, const std::function<void(const std::string&)>& log)
    {
        const uint32_t want = FormatBits(fmt) == 10 ? (fullRange ? kxf20 : kx420) : (fullRange ? k420f : k420v);
        if (want == fmt) return;
        auto listed = [&](const std::vector<uint32_t>& l) { return l.empty() || std::find(l.begin(), l.end(), want) != l.end(); };
        if (!listed(fiFormats) || !listed(srFormats)) return;   // the values pass unchanged either way; only the tag differs
        const uint32_t old = fmt;
        fmt = want;
        std::string err;
        if (!MakePools(err)) { fmt = old; MakePools(err); Log(log, "apple: could not switch to " + FourCCText(want) + ": " + err); return; }
        prev = Buf(); cur = Buf(); prevOut = Buf(); curOut = Buf(); havePrev = haveCur = false;
        Log(log, "apple: the stream is " + std::string(fullRange ? "full" : "limited") + " range: pixel format " + FourCCText(want));
    }

    // src (source size) -> output size: super resolution, or the plain scale once the ladder dropped it
    Buf Upscale(const Buf& src, int64_t pts, std::string& err, const std::function<void(const std::string&)>& log)
    {
        if (!srFactor) return src;
        Buf dst = srDstPool.Get();
        if (!dst) { err = "no super-resolution output buffer"; return Buf(); }
        Tag(dst.ref, colour);
        if (ladder.SrActive()) {
#ifdef PMVS_VT_LL
            if (@available(macOS 26.0, *)) {
                VTFrameProcessorFrame* s = [[VTFrameProcessorFrame alloc] initWithBuffer:src.ref presentationTimeStamp:Pts(pts)];
                VTFrameProcessorFrame* d = [[VTFrameProcessorFrame alloc] initWithBuffer:dst.ref presentationTimeStamp:Pts(pts)];
                VTLowLatencySuperResolutionScalerParameters* p = [[VTLowLatencySuperResolutionScalerParameters alloc] initWithSourceFrame:s destinationFrame:d];
                NSError* e = nil;
                ++stats.vtCalls;
                if (s && d && p && [srProc processWithParameters:p error:&e]) return dst;
                ++stats.vtErrors;
                Log(log, "apple: super resolution failed: " + ErrText(e) + "; scaling this frame instead");
            }
#endif
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

    // the k - 1 generated buffers of the pair (output size), by the plan; false only on a hard error
    bool Interpolate(bool contiguous, std::vector<Buf>& out, std::string& err, const std::function<void(const std::string&)>& log)
    {
        const int k = cfg.k;
        PhasePlan plan;
        if (lastCut || fi == Fi::None) plan = MakePlan(k, 0);
        else if (fi == Fi::Frc && ladder.Level() < 2) plan = MakePlan(k, 0, true);
        else plan = MakePlan(k, ladder.Exponent());
        std::vector<Buf> vt;   // source size, one per plan phase
        if (!plan.phases.empty()) {
            NSMutableArray<NSNumber*>* phases = [NSMutableArray array];
            NSMutableArray* dsts = [NSMutableArray array];
            bool ok = true;
            for (float t : plan.phases) {
                Buf d = fiDstPool.Get();
                if (!d) { ok = false; break; }
                Tag(d.ref, colour);
                const int64_t pts = prevPts + int64_t(std::llround(double(curPts - prevPts) * double(t)));
                VTFrameProcessorFrame* f = nil;
                if (@available(macOS 15.4, *)) f = [[VTFrameProcessorFrame alloc] initWithBuffer:d.ref presentationTimeStamp:Pts(pts)];
                if (!f) { ok = false; break; }
                [phases addObject:@(t)];
                [dsts addObject:f];
                vt.push_back(d);
            }
            NSError* e = nil;
            bool done = false;
            if (ok) {
                ++stats.vtCalls;
                if (@available(macOS 15.4, *)) {
                    VTFrameProcessorFrame* ps = [[VTFrameProcessorFrame alloc] initWithBuffer:prev.ref presentationTimeStamp:Pts(prevPts)];
                    VTFrameProcessorFrame* cs = [[VTFrameProcessorFrame alloc] initWithBuffer:cur.ref presentationTimeStamp:Pts(curPts)];
                    id<VTFrameProcessorParameters> params = nil;
#ifdef PMVS_VT_LL
                    if (fi == Fi::LowLatency) {
                        if (@available(macOS 26.0, *))
                            params = [[VTLowLatencyFrameInterpolationParameters alloc] initWithSourceFrame:cs previousFrame:ps interpolationPhase:phases destinationFrames:dsts];
                    }
#endif
#ifdef PMVS_VT_FRC
                    if (fi == Fi::Frc)
                        params = [[VTFrameRateConversionParameters alloc] initWithSourceFrame:ps nextFrame:cs opticalFlow:nil interpolationPhase:phases
                            submissionMode:(contiguous ? VTFrameRateConversionParametersSubmissionModeSequential : VTFrameRateConversionParametersSubmissionModeRandom)
                            destinationFrames:dsts];
#endif
                    if (ps && cs && params) done = [fiProc processWithParameters:params error:&e];
                }
            }
            if (!done) {
                ++stats.vtErrors;
                if (++consecutiveErrors <= 3 || consecutiveErrors % 240 == 0)
                    Log(log, Fmt("apple: interpolation failed (%d in a row): %s; this pair repeats the nearer real frame", consecutiveErrors, ErrText(e).c_str()));
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
            } else if (wantLL) notes.push_back(caps.lowLatencyFI ? "" : "low-latency interpolation not supported here");
#endif
#ifdef PMVS_VT_FRC
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
#endif
            if (d.fi == Fi::None) {
                std::string n;
                for (auto& s : notes) if (!s.empty()) n += (n.empty() ? "" : "; ") + s;
                err = "no VideoToolbox interpolator for " + std::to_string(w) + "x" + std::to_string(h) + (n.empty() ? std::string() : ": " + n) + " (" + caps.detail + ")";
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

        // ---- one pixel format for the whole chain
        std::vector<uint32_t> common;
        if (d.fi != Fi::None && d.srFactor) { for (uint32_t f : d.fiFormats) if (std::find(d.srFormats.begin(), d.srFormats.end(), f) != d.srFormats.end()) common.push_back(f); }
        else common = d.fi != Fi::None ? d.fiFormats : d.srFormats;
        d.fmt = PickFormat(common, c.bits, c.colour.fullRange, &d.fmtLossy);
        if (!d.fmt && d.srFactor && d.fi != Fi::None) {
            notes.push_back("super resolution shares no 4:2:0 format with the interpolator (" + FormatList(d.srFormats) + "): off");
            if (@available(macOS 15.4, *)) { [d.srProc endSession]; d.srProc = nil; }
            d.srFactor = 0;
            d.fmt = PickFormat(d.fiFormats, c.bits, c.colour.fullRange, &d.fmtLossy);
        }
        if (!d.fmt) { err = "VideoToolbox lists no 4:2:0 bi-planar format we can feed (" + FormatList(d.fi != Fi::None ? d.fiFormats : d.srFormats) + ")"; return false; }
        d.outW = w * std::max(1, d.srFactor); d.outH = h * std::max(1, d.srFactor);
        if (!d.MakePools(err)) return false;
        d.ladder.Init(c.srcFps, d.srFactor > 0, d.fi == Fi::LowLatency ? d.exponent : d.fi == Fi::Frc ? 2 : 0);

        const PhasePlan plan = d.fi == Fi::LowLatency ? MakePlan(d.cfg.k, d.exponent) : MakePlan(d.cfg.k, 0, true);
        std::string phases;
        for (float t : plan.phases) phases += Fmt("%s%.4g", phases.empty() ? "" : " ", t);
        d.chain = d.fi == Fi::LowLatency ? Fmt("low-latency interpolation x%d (grid 1/%d, phases %s, max phase error %.3f frame)", d.cfg.k, 1 << d.exponent, phases.c_str(), plan.maxError)
                : d.fi == Fi::Frc ? Fmt("frame-rate conversion x%d (phases %s)", d.cfg.k, phases.c_str())
                : std::string("no interpolation");
        if (d.srFactor) d.chain += Fmt(" + low-latency super resolution x%d (%dx%d -> %dx%d)", d.srFactor, w, h, d.outW, d.outH);
        d.chain += Fmt(", pixel format %s%s", FourCCText(d.fmt).c_str(), d.fmtLossy ? " (fewer bits than the source)" : "");
        info = d.chain;
        std::string n;
        for (auto& s : notes) if (!s.empty()) n += (n.empty() ? "" : "; ") + s;
        if (!n.empty()) info += " [" + n + "]";
        return true;
    }
}

void Engine::Forget()
{
    m->prev = Buf(); m->cur = Buf(); m->prevOut = Buf(); m->curOut = Buf();
    m->havePrev = m->haveCur = false; m->lastCut = false; m->freshPair = true;
}

bool Engine::Process(const PlanarView& v, const Colour& colour, int64_t pts, bool contiguous, const std::vector<PlanarOut>& gen, const PlanarOut& curOut,
                     bool& havePair, std::string& err)
{
    @autoreleasepool {
        Impl& d = *m;
        havePair = false;
        if (!d.colourSet) { d.colourSet = true; if (colour.fullRange != d.colour.fullRange) { d.colour = colour; d.FollowRange(colour.fullRange, log); } }
        d.colour = colour;
        if (!contiguous) { d.prev = Buf(); d.prevOut = Buf(); d.havePrev = d.haveCur = false; d.freshPair = true; }
        // shift the pair, upload the new frame
        if (d.haveCur) { d.prev = d.cur; d.prevOut = d.curOut; d.prevPts = d.curPts; d.prevThumb = d.curThumb; d.havePrev = true; }
        Buf b = d.srcPool.Get();
        if (!b) { err = "no source buffer from the pool"; return false; }
        Tag(b.ref, colour);
        {
            Locked l(b.ref, false);
            if (!l.Ok()) { err = "could not lock a source buffer"; return false; }
            PlanarToBi(v, l.v);
        }
        d.cur = b;
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
