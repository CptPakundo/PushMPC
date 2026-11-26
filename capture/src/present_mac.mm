// SPDX-License-Identifier: GPL-3.0-or-later
// PushMPC capture mode, macOS presenter: a borderless window just above the player's (status-window level, on every
// Space, also next to a full-screen player), that ignores the mouse (clicks reach the player) and is left out of every
// screen capture (sharingType none). It follows the player's window (CGWindowList, top-left screen coordinates turned
// into AppKit's bottom-left ones), steps aside while another window covers the player, and draws through MoltenVK
// (VK_EXT_metal_surface on its CAMetalLayer). AppKit runs on the main thread (MacMain); the render loop on a worker
// thread calls into it with dispatch to the main queue.
#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <QuartzCore/CAMetalLayer.h>
#include <CoreGraphics/CoreGraphics.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <unistd.h>
#include <vector>
#include "cap_platform.h"
#include <vulkan/vulkan_metal.h>

namespace pmc {
namespace {

// the player's bounds (top-left global coordinates) and whether another normal window lies over it
bool TargetRect(uint64_t id, CGRect& r, bool& covered)
{
    covered = false;
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
    if (!list) return false;
    bool found = false;
    const pid_t self = getpid();
    // front to back: every window before the player that overlaps it (and is not ours or a menu-bar item) covers it
    std::vector<CGRect> above;
    for (NSDictionary* w in (__bridge NSArray*)list) {
        CGRect b = CGRectZero;
        if (w[(id)kCGWindowBounds]) CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)w[(id)kCGWindowBounds], &b);
        if ([w[(id)kCGWindowNumber] unsignedLongLongValue] == id) { r = b; found = true; break; }
        if ([w[(id)kCGWindowLayer] intValue] != 0 || [w[(id)kCGWindowOwnerPID] intValue] == self) continue;
        if (w[(id)kCGWindowAlpha] && [w[(id)kCGWindowAlpha] doubleValue] <= 0.01) continue;   // invisible helper windows
        above.push_back(b);
    }
    CFRelease(list);
    if (!found) return false;
    for (const CGRect& a : above) {
        const CGRect i = CGRectIntersection(a, r);
        if (!CGRectIsNull(i) && i.size.width * i.size.height > r.size.width * r.size.height / 50) { covered = true; break; }
    }
    return true;
}

NSRect ToCocoa(CGRect r)
{
    const CGFloat H = NSScreen.screens.count ? NSScreen.screens[0].frame.size.height : 0;   // the primary screen's height
    return NSMakeRect(r.origin.x, H - (r.origin.y + r.size.height), r.size.width, r.size.height);
}

class MacPresenter final : public Presenter {
public:
    MacPresenter(const Options& o, uint64_t follow) : m_o(o), m_follow(follow) {}
    ~MacPresenter() override
    {
        NSWindow* w = m_win;
        m_win = nil;
        if (w) dispatch_sync(dispatch_get_main_queue(), ^{ [w orderOut:nil]; [w close]; });
    }
    std::vector<const char*> InstanceExtensions() const override { return { VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_METAL_SURFACE_EXTENSION_NAME }; }
    bool Create(std::string& err) override;
    VkSurfaceKHR Surface(VkInstance instance, std::string& err) override;
    void Size(int& w, int& h) const override { w = m_pw; h = m_ph; }
    bool Pump(bool& resized) override;
    void SetVisible(bool on) override { m_wanted = on; Apply(); }
    double RefreshHz() const override { return m_hz; }
    std::string Describe() const override;
    bool HdrCapable(std::string& why) const override;
    void SetHdr(bool on) override;

private:
    void Place(NSRect r);   // main thread
    void Apply();

    Options m_o;
    uint64_t m_follow = 0;
    NSWindow* m_win = nil;
    CAMetalLayer* m_layer = nil;
    NSRect m_rect = NSZeroRect;
    int m_pw = 0, m_ph = 0;
    double m_hz = 0, m_lastCheck = 0;
    bool m_wanted = true, m_shown = false, m_targetOk = true, m_covered = false;
};

void MacPresenter::Place(NSRect r)
{
    NSScreen* scr = NSScreen.mainScreen;
    for (NSScreen* s in NSScreen.screens) if (NSPointInRect(NSMakePoint(NSMidX(r), NSMidY(r)), s.frame)) { scr = s; break; }
    if (m_o.fullscreen) r = scr.frame;
    const CGFloat scale = scr.backingScaleFactor;
    [m_win setFrame:r display:NO];
    m_layer.contentsScale = scale;
    m_layer.drawableSize = CGSizeMake(r.size.width * scale, r.size.height * scale);
    m_rect = r;
    m_pw = int(r.size.width * scale); m_ph = int(r.size.height * scale);
    if (m_o.displayHz <= 0) {
        if (@available(macOS 12.0, *)) m_hz = double(scr.maximumFramesPerSecond);
    } else {
        m_hz = m_o.displayHz;
    }
}

bool MacPresenter::Create(std::string& err)
{
    CGRect target = CGRectZero;
    bool covered = false;
    if (m_follow && !TargetRect(m_follow, target, covered)) { err = "the player's window is not on screen"; return false; }
    __block bool ok = true;
    dispatch_sync(dispatch_get_main_queue(), ^{
        NSRect r = m_follow ? ToCocoa(target) : NSScreen.mainScreen.frame;
        m_win = [[NSWindow alloc] initWithContentRect:r styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
        if (!m_win) { ok = false; return; }
        m_win.releasedWhenClosed = NO;
        m_win.level = NSStatusWindowLevel;
        m_win.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorFullScreenAuxiliary |
                                   NSWindowCollectionBehaviorStationary | NSWindowCollectionBehaviorIgnoresCycle;
        m_win.ignoresMouseEvents = YES;
        m_win.opaque = YES;
        m_win.hasShadow = NO;
        m_win.backgroundColor = NSColor.blackColor;
        m_win.sharingType = NSWindowSharingNone;   // never in a screen capture (our own included)
        m_win.title = @"PushMPC capture";
        NSView* v = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, r.size.width, r.size.height)];
        m_layer = [CAMetalLayer layer];
        m_layer.opaque = YES;
        v.layer = m_layer;
        v.wantsLayer = YES;
        m_win.contentView = v;
        Place(r);
    });
    if (!ok) { err = "could not open the capture window"; return false; }
    m_covered = covered;
    Apply();
    return true;
}

VkSurfaceKHR MacPresenter::Surface(VkInstance instance, std::string& err)
{
    auto create = reinterpret_cast<PFN_vkCreateMetalSurfaceEXT>(vkGetInstanceProcAddr(instance, "vkCreateMetalSurfaceEXT"));
    if (!create) { err = "MoltenVK has no Metal surface"; return VK_NULL_HANDLE; }
    VkMetalSurfaceCreateInfoEXT ci{ VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT };
    ci.pLayer = m_layer;
    VkSurfaceKHR s = VK_NULL_HANDLE;
    const VkResult r = create(instance, &ci, nullptr, &s);
    if (r != VK_SUCCESS) { err = "vkCreateMetalSurfaceEXT failed (" + std::to_string(int(r)) + ")"; return VK_NULL_HANDLE; }
    return s;
}

void MacPresenter::Apply()
{
    const bool show = m_wanted && m_targetOk && !m_covered;
    if (show == m_shown) return;
    m_shown = show;
    NSWindow* w = m_win;
    dispatch_async(dispatch_get_main_queue(), ^{ if (show) [w orderFrontRegardless]; else [w orderOut:nil]; });
}

bool MacPresenter::Pump(bool& resized)
{
    resized = false;
    const double now = Now();
    if (now - m_lastCheck < 0.1 || !m_follow) return true;
    m_lastCheck = now;
    CGRect t = CGRectZero;
    bool covered = false;
    m_targetOk = TargetRect(m_follow, t, covered);
    m_covered = !m_o.fullscreen && covered;
    if (m_targetOk && !m_o.fullscreen) {
        const int pw = m_pw, ph = m_ph;
        // AppKit (NSScreen, the window) only on the main thread
        dispatch_sync(dispatch_get_main_queue(), ^{
            const NSRect want = ToCocoa(t);
            if (!NSEqualRects(want, m_rect)) Place(want);
        });
        resized = pw != m_pw || ph != m_ph;
    }
    Apply();
    return true;
}

// an EDR display (extended dynamic range above SDR white: XDR, HDR monitors with HDR on)
bool MacPresenter::HdrCapable(std::string& why) const
{
    if (std::getenv("PUSHMPC_CAPTURE_SDR")) { why = "PUSHMPC_CAPTURE_SDR is set"; return false; }
    __block double edr = 1.0;
    NSWindow* w = m_win;
    dispatch_sync(dispatch_get_main_queue(), ^{
        NSScreen* s = w.screen ? w.screen : NSScreen.mainScreen;
        if (@available(macOS 10.15, *)) edr = s.maximumPotentialExtendedDynamicRangeColorComponentValue;
    });
    if (edr <= 1.0) { why = "this display shows no extended dynamic range (SDR)"; return false; }
    return true;
}

void MacPresenter::SetHdr(bool on)
{
    CAMetalLayer* l = m_layer;
    dispatch_sync(dispatch_get_main_queue(), ^{
        l.wantsExtendedDynamicRangeContent = on;   // MoltenVK sets the PQ colour space from the swapchain
    });
}

std::string MacPresenter::Describe() const
{
    char b[200];
    std::snprintf(b, sizeof(b), "a window over the player, %dx%d pixels%s, %.0f Hz", m_pw, m_ph, m_o.fullscreen ? " (full screen)" : "", m_hz);
    return b;
}

} // namespace

std::unique_ptr<Presenter> MakeMacPresenter(const Options& o, uint64_t followWindow) { return std::make_unique<MacPresenter>(o, followWindow); }

int MacMain(const std::function<int()>& loop)
{
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];   // no Dock icon, no menu bar of its own
        std::atomic<int> rc{ 0 };
        std::thread worker([&] {
            @autoreleasepool { rc = loop(); }
            dispatch_async(dispatch_get_main_queue(), ^{
                [NSApp stop:nil];
                // stop takes effect after the next event: post one
                NSEvent* e = [NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0 timestamp:0
                                            windowNumber:0 context:nil subtype:0 data1:0 data2:0];
                [NSApp postEvent:e atStart:YES];
            });
        });
        [NSApp run];
        worker.join();
        return rc;
    }
}

} // namespace pmc
