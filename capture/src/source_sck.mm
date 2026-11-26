// SPDX-License-Identifier: GPL-3.0-or-later
// PushMPC capture mode, macOS source: ScreenCaptureKit captures one window on its own (a desktop-independent window
// filter: what is above it, our overlay included, is not in the picture), in BGRA sRGB at the window's pixel size,
// without the cursor. Only complete frames count; the content rectangle crops the picture; the frame's display time
// (mach_absolute_time, the clock Now() reads) is its time. The newest sample is held until the engine has read it.
// macOS asks for the Screen Recording permission the first time: until it is given, capture mode says so in plain words
// and stops. --list needs no permission (CGWindowList).
#import <AppKit/AppKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#include <CoreGraphics/CoreGraphics.h>
#include <mach/mach_time.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include "cap_platform.h"

namespace pmc { class SckSource; }

@interface PmcSckOutput : NSObject <SCStreamOutput, SCStreamDelegate>
@property(nonatomic, assign) pmc::SckSource* owner;
@end

namespace pmc {

namespace {
double MachSeconds(uint64_t t)
{
    static const double scale = [] { mach_timebase_info_data_t tb; mach_timebase_info(&tb); return double(tb.numer) / double(tb.denom) * 1e-9; }();
    return double(t) * scale;
}
std::string Utf8(NSString* s) { return s ? std::string(s.UTF8String ? s.UTF8String : "") : std::string(); }
std::string Lower(std::string s) { for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }

// the on-screen windows (front to back), layer 0, from CGWindowList (no permission needed for ids, owners, bounds)
std::vector<WindowInfo> OnScreen()
{
    std::vector<WindowInfo> out;
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
    if (!list) return out;
    for (NSDictionary* w in (__bridge NSArray*)list) {
        if ([w[(id)kCGWindowLayer] intValue] != 0) continue;
        WindowInfo wi;
        wi.id = [w[(id)kCGWindowNumber] unsignedLongLongValue];
        wi.owner = Utf8(w[(id)kCGWindowOwnerName]);
        wi.title = Utf8(w[(id)kCGWindowName]);
        CGRect r = CGRectZero;
        if (w[(id)kCGWindowBounds]) CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)w[(id)kCGWindowBounds], &r);
        wi.x = int(r.origin.x); wi.y = int(r.origin.y); wi.w = int(r.size.width); wi.h = int(r.size.height);
        out.push_back(wi);
    }
    CFRelease(list);
    return out;
}
} // namespace

class SckSource final : public Source {
public:
    explicit SckSource(const Options& o) : m_o(o) {}
    ~SckSource() override;
    bool Start(std::string& err) override;
    bool Next(SourceFrame& f, int timeoutMs) override;
    bool Alive(std::string& why) override;
    std::string Describe() const override;
    std::string PlayerName() const override { return m_owner; }
    uint64_t Window() const override { return m_id; }
    bool SetHdr(bool on) override;

    void Deliver(CMSampleBufferRef sb, CGRect crop, double t)
    {
        CFRetain(sb);
        std::lock_guard<std::mutex> lk(m_mx);
        if (m_ready) CFRelease(m_ready);
        m_ready = sb; m_crop = crop; m_t = t;
        m_cv.notify_one();
    }
    void Stopped(const std::string& why)
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_dead = true; m_why = why.empty() ? "the capture stopped" : "the capture stopped (" + why + ")";
        m_cv.notify_all();
    }

private:
    bool Configure(int w, int h, bool update, std::string& err);
    Options m_o;
    uint64_t m_id = 0;
    std::string m_owner, m_title;
    SCStream* m_stream = nil;
    SCContentFilter* m_filter = nil;
    PmcSckOutput* m_out = nil;
    dispatch_queue_t m_queue = nil;
    int m_w = 0, m_h = 0;           // the window in points
    bool m_hdr = false;             // HDR capture: 10-bit BT.2020 PQ ('l10r')
    double m_scale = 2, m_lastCheck = 0;
    std::mutex m_mx;
    std::condition_variable m_cv;
    CMSampleBufferRef m_ready = nullptr;
    CGRect m_crop = CGRectZero;
    double m_t = 0;
    bool m_dead = false;
    std::string m_why;
};

SckSource::~SckSource()
{
    if (m_stream) {
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [m_stream stopCaptureWithCompletionHandler:^(NSError*) { dispatch_semaphore_signal(done); }];
        dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC));
    }
    std::lock_guard<std::mutex> lk(m_mx);
    if (m_ready) CFRelease(m_ready);
    m_ready = nullptr;
}

bool SckSource::Configure(int w, int h, bool update, std::string& err)
{
    SCStreamConfiguration* c = [[SCStreamConfiguration alloc] init];
    c.width = size_t(std::max(2, int(w * m_scale)));
    c.height = size_t(std::max(2, int(h * m_scale)));
    c.pixelFormat = kCVPixelFormatType_32BGRA;
    c.colorSpaceName = kCGColorSpaceSRGB;
    if (m_hdr) {
        if (@available(macOS 15.0, *)) {
            // the window as the display shows it, HDR kept: 10-bit R'G'B' in BT.2020 PQ (ITU-R BT.2100)
            c.captureDynamicRange = SCCaptureDynamicRangeHDRLocalDisplay;
            c.pixelFormat = kCVPixelFormatType_ARGB2101010LEPacked;
            c.colorSpaceName = kCGColorSpaceITUR_2100_PQ;
        }
    }
    c.showsCursor = NO;
    c.minimumFrameInterval = CMTimeMake(1, 240);   // every frame the player shows (up to 240 a second)
    c.queueDepth = 6;
    if (@available(macOS 14.0, *)) c.ignoreShadowsSingleWindow = YES;
    if (!update) {
        m_stream = [[SCStream alloc] initWithFilter:m_filter configuration:c delegate:m_out];
        NSError* e = nil;
        if (![m_stream addStreamOutput:m_out type:SCStreamOutputTypeScreen sampleHandlerQueue:m_queue error:&e]) { err = "ScreenCaptureKit: " + Utf8(e.localizedDescription); return false; }
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        __block NSError* startErr = nil;
        [m_stream startCaptureWithCompletionHandler:^(NSError* se) { startErr = se; dispatch_semaphore_signal(done); }];
        if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) != 0) { err = "ScreenCaptureKit did not start the capture"; return false; }
        if (startErr) { err = "ScreenCaptureKit: " + Utf8(startErr.localizedDescription); return false; }
    } else {
        [m_stream updateConfiguration:c completionHandler:^(NSError*) {}];
    }
    m_w = w; m_h = h;
    return true;
}

bool SckSource::Start(std::string& err)
{
    if (!CGPreflightScreenCaptureAccess()) {
        CGRequestScreenCaptureAccess();   // macOS shows its dialog (once) and lists the app in its settings
        err = kMacPermissionMessage;
        return false;
    }
    const std::vector<WindowInfo> wins = OnScreen();
    if (!m_o.window.empty()) {
        m_id = std::strtoull(m_o.window.c_str(), nullptr, 0);
    } else if (!m_o.player.empty()) {
        const std::string want = Lower(m_o.player);
        long best = -1;
        for (auto& w : wins) {
            if (Lower(w.owner).find(want) == std::string::npos) continue;
            const long area = long(w.w) * long(w.h);
            if (area > best) { best = area; m_id = w.id; }
        }
        if (!m_id) { err = "no window of \"" + m_o.player + "\" on screen (start the player first; --list shows the windows)"; return false; }
    } else {
        err = "say which window: --player <app name> or --window <number> (see --list)";
        return false;
    }
    __block SCShareableContent* content = nil;
    __block NSError* ce = nil;
    dispatch_semaphore_t got = dispatch_semaphore_create(0);
    [SCShareableContent getShareableContentExcludingDesktopWindows:YES onScreenWindowsOnly:NO completionHandler:^(SCShareableContent* c, NSError* e) {
        content = c; ce = e; dispatch_semaphore_signal(got);
    }];
    if (dispatch_semaphore_wait(got, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) != 0 || !content) {
        err = ce ? "ScreenCaptureKit: " + Utf8(ce.localizedDescription) : std::string("ScreenCaptureKit did not list the windows");
        return false;
    }
    SCWindow* win = nil;
    for (SCWindow* w in content.windows) if (w.windowID == CGWindowID(m_id)) { win = w; break; }
    if (!win) { err = "window " + std::to_string(m_id) + " cannot be captured (closed, or not a normal window)"; return false; }
    m_owner = Utf8(win.owningApplication.applicationName);
    m_title = Utf8(win.title);
    m_filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:win];
    m_scale = 2;
    if (@available(macOS 14.0, *)) m_scale = m_filter.pointPixelScale;
    else {
        for (NSScreen* s in NSScreen.screens) if (NSIntersectsRect(s.frame, NSMakeRect(win.frame.origin.x, win.frame.origin.y, win.frame.size.width, win.frame.size.height))) { m_scale = s.backingScaleFactor; break; }
    }
    m_out = [[PmcSckOutput alloc] init];
    m_out.owner = this;
    m_queue = dispatch_queue_create("pushmpc.capture.sck", DISPATCH_QUEUE_SERIAL);
    return Configure(int(win.frame.size.width), int(win.frame.size.height), false, err);
}

bool SckSource::SetHdr(bool on)
{
    if (@available(macOS 15.0, *)) {
        if (on == m_hdr) return true;
        m_hdr = on;
        std::string e;
        return Configure(m_w, m_h, true, e);
    }
    return false;   // HDR capture needs macOS 15
}

bool SckSource::Next(SourceFrame& f, int timeoutMs)
{
    std::unique_lock<std::mutex> lk(m_mx);
    if (!m_ready) m_cv.wait_for(lk, std::chrono::milliseconds(std::max(0, timeoutMs)), [&] { return m_ready != nullptr || m_dead; });
    if (!m_ready) return false;
    CMSampleBufferRef sb = m_ready;
    m_ready = nullptr;
    const CGRect crop = m_crop;
    const double t = m_t;
    lk.unlock();
    CVImageBufferRef pb = CMSampleBufferGetImageBuffer(sb);
    if (!pb || CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) { CFRelease(sb); return false; }
    const int W = int(CVPixelBufferGetWidth(pb)), H = int(CVPixelBufferGetHeight(pb));
    const size_t bpr = CVPixelBufferGetBytesPerRow(pb);
    const uint8_t* base = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(pb));
    int x = 0, y = 0, w = W, h = H;
    if (crop.size.width > 0 && crop.size.height > 0) {
        x = std::clamp(int(crop.origin.x), 0, W - 1); y = std::clamp(int(crop.origin.y), 0, H - 1);
        w = std::min(int(crop.size.width + 0.5), W - x); h = std::min(int(crop.size.height + 0.5), H - y);
    }
    f = SourceFrame{};
    f.width = w; f.height = h;
    f.fmt = CVPixelBufferGetPixelFormatType(pb) == kCVPixelFormatType_ARGB2101010LEPacked ? PixFmt::RGB10 : PixFmt::BGRX;
    f.ptr = base + size_t(y) * bpr + size_t(x) * 4;
    f.pitch = ptrdiff_t(bpr);
    f.time = t;
    f.release = [pb, sb] { CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly); CFRelease(sb); };
    return w > 0 && h > 0;
}

bool SckSource::Alive(std::string& why)
{
    {
        std::lock_guard<std::mutex> lk(m_mx);
        if (m_dead) { why = m_why; return false; }
    }
    const double now = Now();
    if (now - m_lastCheck < 1.0) return true;
    m_lastCheck = now;
    // the window's size, once a second: the capture follows a resize
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow, CGWindowID(m_id));
    bool found = false;
    if (list) {
        for (NSDictionary* w in (__bridge NSArray*)list) {
            CGRect r = CGRectZero;
            if (w[(id)kCGWindowBounds]) CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)w[(id)kCGWindowBounds], &r);
            found = true;
            const int nw = int(r.size.width), nh = int(r.size.height);
            if (nw > 0 && nh > 0 && (nw != m_w || nh != m_h)) { std::string e; Configure(nw, nh, true, e); }
        }
        CFRelease(list);
    }
    if (!found) { why = "the player's window closed"; return false; }
    return true;
}

std::string SckSource::Describe() const
{
    char b[300];
    std::snprintf(b, sizeof(b), "window %llu of %s (%dx%d points, scale %.1f), ScreenCaptureKit%s", static_cast<unsigned long long>(m_id), m_owner.c_str(), m_w, m_h, m_scale, m_hdr ? ", HDR (10-bit BT.2020 PQ)" : "");
    return b;
}

std::unique_ptr<Source> MakeSckSource(const Options& o) { return std::make_unique<SckSource>(o); }

bool ListMacWindows(std::vector<WindowInfo>& out, std::string& err)
{
    out = OnScreen();
    if (out.empty()) err = "no windows on screen";
    return true;
}

} // namespace pmc

@implementation PmcSckOutput
- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sb ofType:(SCStreamOutputType)type
{
    if (type != SCStreamOutputTypeScreen || !self.owner || !CMSampleBufferIsValid(sb)) return;
    CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sb, false);
    if (!att || CFArrayGetCount(att) < 1) return;
    NSDictionary* info = (__bridge NSDictionary*)CFArrayGetValueAtIndex(att, 0);
    NSNumber* status = info[SCStreamFrameInfoStatus];
    if (!status || status.integerValue != SCFrameStatusComplete) return;   // idle / blank / suspended: no new picture
    CGRect content = CGRectZero;
    if (info[SCStreamFrameInfoContentRect]) CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)info[SCStreamFrameInfoContentRect], &content);
    const double scale = info[SCStreamFrameInfoScaleFactor] ? [info[SCStreamFrameInfoScaleFactor] doubleValue] : 1.0;
    // the content rectangle is in points: the crop in the buffer's pixels
    const CGRect crop = CGRectMake(content.origin.x * scale, content.origin.y * scale, content.size.width * scale, content.size.height * scale);
    NSNumber* dt = info[SCStreamFrameInfoDisplayTime];
    const double t = dt ? pmc::MachSeconds(dt.unsignedLongLongValue) : pmc::Now();
    self.owner->Deliver(sb, crop, t);
}
- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error
{
    if (self.owner) self.owner->Stopped(error ? pmc::Utf8(error.localizedDescription) : std::string());
}
@end
