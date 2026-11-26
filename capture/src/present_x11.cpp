// SPDX-License-Identifier: GPL-3.0-or-later
// PushMPC capture mode, X11 presenter: an override-redirect window (the window manager leaves it alone) exactly over the
// player's window, with an empty input shape so every click and scroll goes to the player below. It follows the player
// as it moves and resizes, steps aside while another window covers the player or the player is minimised, and raises
// itself again every half second (a click on the player raises the player). The screen's refresh rate comes from
// RandR (the monitor under the player). Vulkan draws into it through VK_KHR_xlib_surface.
#include "cap_platform.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
// X11 and the Xlib WSI header last: Xlib's macros must not reach the headers above
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/shape.h>
#include <vulkan/vulkan_xlib.h>   // volk.h has no platform macros: the Xlib WSI types come from here

namespace pmc {
// source_x11.cpp: one handler for the process (Xlib's is global); it records the error and never exits
int X11ErrorHandler(Display* d, XErrorEvent* e);
namespace {

struct Rect { int x = 0, y = 0, w = 0, h = 0; bool operator!=(const Rect& o) const { return x != o.x || y != o.y || w != o.w || h != o.h; } };
bool Overlaps(const Rect& a, const Rect& b) { return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h; }

class X11Presenter final : public Presenter {
public:
    X11Presenter(const Options& o, uint64_t follow) : m_o(o), m_follow(::Window(follow)) {}
    ~X11Presenter() override
    {
        if (!m_d) return;
        if (m_win) XDestroyWindow(m_d, m_win);
        XCloseDisplay(m_d);
    }
    std::vector<const char*> InstanceExtensions() const override { return { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_XLIB_SURFACE_EXTENSION_NAME }; }
    bool Create(std::string& err) override;
    VkSurfaceKHR Surface(VkInstance instance, std::string& err) override;
    void Size(int& w, int& h) const override { w = m_rect.w; h = m_rect.h; }
    bool Pump(bool& resized) override;
    void SetVisible(bool on) override { m_wanted = on; Apply(); }
    double RefreshHz() const override { return m_hz; }
    std::string Describe() const override;

private:
    bool Target(Rect& r, bool& viewable);
    bool Covered(const Rect& r);
    void Apply();
    double Refresh(const Rect& r);
    Rect Monitor(const Rect& r);

    Options m_o;
    ::Window m_follow = 0;
    Display* m_d = nullptr;
    ::Window m_win = 0, m_root = 0;
    Rect m_rect;
    double m_hz = 0, m_lastCheck = 0, m_lastRaise = 0;
    bool m_wanted = true, m_shown = false, m_targetOk = true, m_covered = false;
};

bool X11Presenter::Target(Rect& r, bool& viewable)
{
    XWindowAttributes a{};
    XSync(m_d, False);
    if (!XGetWindowAttributes(m_d, m_follow, &a)) return false;
    int x = 0, y = 0; ::Window child = 0;
    if (!XTranslateCoordinates(m_d, m_follow, m_root, 0, 0, &x, &y, &child)) return false;
    r = { x, y, a.width, a.height };
    viewable = a.map_state == IsViewable;
    return true;
}

// the monitor (RandR 1.5 monitors, else the whole screen) under the centre of r
Rect X11Presenter::Monitor(const Rect& r)
{
    Rect s{ 0, 0, DisplayWidth(m_d, DefaultScreen(m_d)), DisplayHeight(m_d, DefaultScreen(m_d)) };
    int n = 0;
    XRRMonitorInfo* m = XRRGetMonitors(m_d, m_root, True, &n);
    if (!m) return s;
    const int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    for (int i = 0; i < n; ++i)
        if (cx >= m[i].x && cy >= m[i].y && cx < m[i].x + m[i].width && cy < m[i].y + m[i].height) { s = { m[i].x, m[i].y, m[i].width, m[i].height }; break; }
    XRRFreeMonitors(m);
    return s;
}

// the refresh rate of the CRTC under the centre of r (dot clock / total pixels of its mode)
double X11Presenter::Refresh(const Rect& r)
{
    XRRScreenResources* res = XRRGetScreenResourcesCurrent(m_d, m_root);
    if (!res) return 0;
    double hz = 0;
    const int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    for (int i = 0; i < res->ncrtc && hz == 0; ++i) {
        XRRCrtcInfo* c = XRRGetCrtcInfo(m_d, res, res->crtcs[i]);
        if (!c) continue;
        if (c->mode && cx >= c->x && cy >= c->y && cx < c->x + int(c->width) && cy < c->y + int(c->height)) {
            for (int k = 0; k < res->nmode; ++k) {
                const XRRModeInfo& mi = res->modes[k];
                if (mi.id != c->mode || !mi.hTotal || !mi.vTotal) continue;
                double v = double(mi.dotClock) / (double(mi.hTotal) * double(mi.vTotal));
                if (mi.modeFlags & RR_DoubleScan) v /= 2;
                if (mi.modeFlags & RR_Interlace) v *= 2;
                hz = v;
            }
        }
        XRRFreeCrtcInfo(c);
    }
    XRRFreeScreenResources(res);
    return hz;
}

// another managed window above the player and over it (_NET_CLIENT_LIST_STACKING: bottom to top)
bool X11Presenter::Covered(const Rect& r)
{
    Atom prop = XInternAtom(m_d, "_NET_CLIENT_LIST_STACKING", True);
    if (!prop || !m_follow) return false;
    Atom at = 0; int fmt = 0; unsigned long n = 0, after = 0; unsigned char* data = nullptr;
    if (XGetWindowProperty(m_d, m_root, prop, 0, 65536, False, XA_WINDOW, &at, &fmt, &n, &after, &data) != 0 || !data) return false;
    const unsigned long* w = reinterpret_cast<const unsigned long*>(data);
    long self = -1;
    for (unsigned long i = 0; i < n; ++i) if (::Window(w[i]) == m_follow) self = long(i);
    bool covered = false;
    for (unsigned long i = self < 0 ? n : static_cast<unsigned long>(self + 1); i < n && !covered; ++i) {
        XWindowAttributes a{};
        if (!XGetWindowAttributes(m_d, ::Window(w[i]), &a) || a.map_state != IsViewable) continue;
        int x = 0, y = 0; ::Window child = 0;
        XTranslateCoordinates(m_d, ::Window(w[i]), m_root, 0, 0, &x, &y, &child);
        // a window that covers only a sliver (a panel, a tooltip-sized popup) does not count
        const Rect o{ x, y, a.width, a.height };
        if (Overlaps(o, r) && long(a.width) * long(a.height) > long(r.w) * long(r.h) / 50) covered = true;
    }
    XFree(data);
    return covered;
}

void X11Presenter::Apply()
{
    const bool show = m_wanted && m_targetOk && !m_covered;
    if (show == m_shown) return;
    if (show) XMapRaised(m_d, m_win); else XUnmapWindow(m_d, m_win);
    XFlush(m_d);
    m_shown = show;
}

bool X11Presenter::Create(std::string& err)
{
    XInitThreads();
    m_d = XOpenDisplay(nullptr);
    if (!m_d) { err = "cannot open the X display"; return false; }
    XSetErrorHandler(X11ErrorHandler);
    m_root = DefaultRootWindow(m_d);
    int sev = 0, ser = 0;
    if (!XShapeQueryExtension(m_d, &sev, &ser)) { err = "the X server has no Shape extension (clicks could not go through to the player)"; return false; }
    Rect t{ 0, 0, DisplayWidth(m_d, DefaultScreen(m_d)), DisplayHeight(m_d, DefaultScreen(m_d)) };
    bool viewable = true;
    if (m_follow && !Target(t, viewable)) { err = "the player's window is gone"; return false; }
    m_rect = m_o.fullscreen || !m_follow ? Monitor(t) : t;
    if (m_rect.w <= 0 || m_rect.h <= 0) m_rect = { 0, 0, 640, 360 };
    m_hz = m_o.displayHz > 0 ? m_o.displayHz : Refresh(m_rect);
    XSetWindowAttributes sa{};
    sa.override_redirect = True;
    sa.background_pixel = BlackPixel(m_d, DefaultScreen(m_d));
    sa.border_pixel = 0;
    sa.event_mask = StructureNotifyMask | ExposureMask;
    m_win = XCreateWindow(m_d, m_root, m_rect.x, m_rect.y, unsigned(m_rect.w), unsigned(m_rect.h), 0, CopyFromParent, InputOutput, CopyFromParent,
                          CWOverrideRedirect | CWBackPixel | CWBorderPixel | CWEventMask, &sa);
    if (!m_win) { err = "XCreateWindow failed"; return false; }
    XStoreName(m_d, m_win, "PushMPC capture");
    XClassHint ch{};
    char resName[] = "pushmpc-capture", resClass[] = "PushMPC";
    ch.res_name = resName; ch.res_class = resClass;
    XSetClassHint(m_d, m_win, &ch);
    // no input at all: an empty input shape (clicks, scrolls and drags reach the player)
    XShapeCombineRectangles(m_d, m_win, ShapeInput, 0, 0, nullptr, 0, ShapeSet, Unsorted);
    if (m_follow) XSelectInput(m_d, m_follow, StructureNotifyMask);
    m_targetOk = viewable;
    Apply();
    XSync(m_d, False);
    return true;
}

VkSurfaceKHR X11Presenter::Surface(VkInstance instance, std::string& err)
{
    auto create = reinterpret_cast<PFN_vkCreateXlibSurfaceKHR>(vkGetInstanceProcAddr(instance, "vkCreateXlibSurfaceKHR"));
    if (!create) { err = "the Vulkan driver has no Xlib surface"; return VK_NULL_HANDLE; }
    VkXlibSurfaceCreateInfoKHR ci{ VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR };
    ci.dpy = m_d; ci.window = m_win;
    VkSurfaceKHR s = VK_NULL_HANDLE;
    const VkResult r = create(instance, &ci, nullptr, &s);
    if (r != VK_SUCCESS) { err = "vkCreateXlibSurfaceKHR failed (" + std::to_string(int(r)) + ")"; return VK_NULL_HANDLE; }
    return s;
}

bool X11Presenter::Pump(bool& resized)
{
    resized = false;
    while (XPending(m_d)) { XEvent ev; XNextEvent(m_d, &ev); }   // the player's ConfigureNotify only wakes us; the check below reads it
    const double now = Now();
    if (now - m_lastCheck < 0.05) return true;
    m_lastCheck = now;
    if (m_follow) {
        Rect t; bool viewable = false;
        if (!Target(t, viewable)) { m_targetOk = false; Apply(); return true; }   // gone: the source says so and ends the run
        m_targetOk = viewable;
        const Rect want = m_o.fullscreen ? Monitor(t) : t;
        if (viewable && want != m_rect && want.w > 0 && want.h > 0) {
            const bool sized = want.w != m_rect.w || want.h != m_rect.h;
            XMoveResizeWindow(m_d, m_win, want.x, want.y, unsigned(want.w), unsigned(want.h));
            m_rect = want;
            if (sized) resized = true;
            if (m_o.displayHz <= 0) m_hz = Refresh(m_rect);
        }
        if (now - m_lastRaise >= 0.5) {
            m_lastRaise = now;
            m_covered = !m_o.fullscreen && Covered(t);
            if (m_shown) XRaiseWindow(m_d, m_win);
        }
    }
    Apply();
    XFlush(m_d);
    return true;
}

std::string X11Presenter::Describe() const
{
    char b[200];
    std::snprintf(b, sizeof(b), "X11 overlay %dx%d at %d,%d%s, %.3f Hz", m_rect.w, m_rect.h, m_rect.x, m_rect.y, m_o.fullscreen ? " (full screen)" : " over the player", m_hz);
    return b;
}

} // namespace

std::unique_ptr<Presenter> MakeX11Presenter(const Options& o, uint64_t followWindow) { return std::make_unique<X11Presenter>(o, followWindow); }

} // namespace pmc
