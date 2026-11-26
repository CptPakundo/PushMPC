// SPDX-License-Identifier: GPL-3.0-or-later
// PushMPC capture mode, X11 source: the player's window is redirected off screen (XComposite, automatic: the X server
// still draws it, and capture gets its own pixmap even under our overlay), XDamage says when the player drew, and the
// picture is read from that pixmap into one of three shared-memory images (MIT-SHM; plain XGetImage on a remote
// display). An image is reused only after the engine has read it (SourceFrame::release).
// Which window: --window <id>, --player <name> (WM_CLASS, or the process name of _NET_WM_PID: /proc/<pid>/comm and
// the executable), --pick (click it). --list prints what it can take.
#include "cap_platform.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <poll.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>
// X11 last: its macros (None, Bool, Status, Success) must not reach the headers above
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>

namespace pmc {
int X11ErrorHandler(Display* d, XErrorEvent* e);
namespace {

std::atomic<int> g_xerr{ 0 };
// errors of the requests between Begin and End (the window may vanish at any time: no exit)
struct XTrap {
    Display* d;
    explicit XTrap(Display* dpy) : d(dpy) { XSync(d, False); g_xerr = 0; }
    int End() { XSync(d, False); return g_xerr.exchange(0); }
};

std::string Lower(std::string s) { for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }

std::vector<unsigned long> PropLongs(Display* d, ::Window w, const char* name, Atom type)
{
    std::vector<unsigned long> out;
    Atom prop = XInternAtom(d, name, True);
    if (prop == 0) return out;
    Atom at = 0; int fmt = 0; unsigned long n = 0, after = 0; unsigned char* data = nullptr;
    XTrap t(d);
    const int r = XGetWindowProperty(d, w, prop, 0, 65536, False, type, &at, &fmt, &n, &after, &data);
    t.End();
    if (r == 0 && data && fmt == 32 && at == type) {
        const unsigned long* v = reinterpret_cast<const unsigned long*>(data);   // format 32 = long in Xlib
        out.assign(v, v + n);
    }
    if (data) XFree(data);
    return out;
}

std::string PropString(Display* d, ::Window w, const char* name, const char* typeName)
{
    Atom prop = XInternAtom(d, name, True), type = XInternAtom(d, typeName, True);
    if (prop == 0 || type == 0) return {};
    Atom at = 0; int fmt = 0; unsigned long n = 0, after = 0; unsigned char* data = nullptr;
    XTrap t(d);
    const int r = XGetWindowProperty(d, w, prop, 0, 4096, False, type, &at, &fmt, &n, &after, &data);
    t.End();
    std::string s;
    if (r == 0 && data && fmt == 8) s.assign(reinterpret_cast<const char*>(data), n);
    if (data) XFree(data);
    return s;
}

std::string Title(Display* d, ::Window w)
{
    std::string t = PropString(d, w, "_NET_WM_NAME", "UTF8_STRING");
    if (!t.empty()) return t;
    char* name = nullptr;
    XTrap tr(d);
    if (XFetchName(d, w, &name) && name) { t = name; XFree(name); }
    tr.End();
    return t;
}

std::string ClassOf(Display* d, ::Window w)
{
    XClassHint h{};
    std::string s;
    XTrap t(d);
    if (XGetClassHint(d, w, &h)) {
        if (h.res_name) s = h.res_name;
        if (h.res_class) s += std::string(s.empty() ? "" : "/") + h.res_class;
        if (h.res_name) XFree(h.res_name);
        if (h.res_class) XFree(h.res_class);
    }
    t.End();
    return s;
}

std::string ReadSmall(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    std::string s;
    std::getline(f, s, '\0');
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// the process behind the window: comm, the executable's and argv[0]'s base names
std::string ProcessOf(Display* d, ::Window w)
{
    const auto pid = PropLongs(d, w, "_NET_WM_PID", XA_CARDINAL);
    if (pid.empty() || pid[0] == 0) return {};
    const std::string base = "/proc/" + std::to_string(pid[0]);
    std::string out = ReadSmall(base + "/comm");
    char exe[4096] = {};
    const ssize_t n = readlink((base + "/exe").c_str(), exe, sizeof(exe) - 1);
    if (n > 0) { std::string e(exe, size_t(n)); out += " " + e.substr(e.rfind('/') + 1); }
    std::string a0 = ReadSmall(base + "/cmdline");
    if (!a0.empty()) out += " " + a0.substr(a0.rfind('/') + 1);
    return out;
}

bool HasWmState(Display* d, ::Window w)
{
    Atom s = XInternAtom(d, "WM_STATE", True);
    if (s == 0) return false;
    Atom at = 0; int fmt = 0; unsigned long n = 0, after = 0; unsigned char* data = nullptr;
    XTrap t(d);
    const int r = XGetWindowProperty(d, w, s, 0, 0, False, AnyPropertyType, &at, &fmt, &n, &after, &data);
    t.End();
    if (data) XFree(data);
    return r == 0 && at != 0;
}

// the client window below a frame (what xprop's / XmuClientWindow's search does): breadth first, WM_STATE marks it
::Window ClientBelow(Display* d, ::Window w, int depth = 0)
{
    if (HasWmState(d, w)) return w;
    if (depth > 4) return 0;
    ::Window root = 0, parent = 0, *kids = nullptr; unsigned n = 0;
    XTrap t(d);
    const int ok = XQueryTree(d, w, &root, &parent, &kids, &n);
    t.End();
    if (!ok) return 0;
    ::Window found = 0;
    for (unsigned i = 0; i < n && !found; ++i) if (HasWmState(d, kids[i])) found = kids[i];
    for (unsigned i = 0; i < n && !found; ++i) found = ClientBelow(d, kids[i], depth + 1);
    if (kids) XFree(kids);
    return found;
}

// the top-level windows: _NET_CLIENT_LIST (a window manager), else the mapped children of the root (no window manager:
// Xvfb in CI) and the clients below them
std::vector<::Window> Clients(Display* d)
{
    std::vector<::Window> out;
    const ::Window root = DefaultRootWindow(d);
    for (unsigned long w : PropLongs(d, root, "_NET_CLIENT_LIST", XA_WINDOW)) out.push_back(::Window(w));
    if (!out.empty()) return out;
    ::Window r = 0, p = 0, *kids = nullptr; unsigned n = 0;
    if (!XQueryTree(d, root, &r, &p, &kids, &n)) return out;
    for (unsigned i = 0; i < n; ++i) {
        XWindowAttributes a{};
        XTrap t(d);
        const int ok = XGetWindowAttributes(d, kids[i], &a);
        t.End();
        if (!ok || a.map_state != IsViewable || a.c_class != InputOutput || a.override_redirect) continue;
        ::Window c = ClientBelow(d, kids[i]);
        if (!c && !ClassOf(d, kids[i]).empty()) c = kids[i];
        if (c) out.push_back(c);
    }
    if (kids) XFree(kids);
    return out;
}

bool Geometry(Display* d, ::Window w, WindowInfo& wi, bool* viewable = nullptr)
{
    XWindowAttributes a{};
    XTrap t(d);
    int ok = XGetWindowAttributes(d, w, &a);
    int x = 0, y = 0; ::Window child = 0;
    if (ok) ok = XTranslateCoordinates(d, w, DefaultRootWindow(d), 0, 0, &x, &y, &child);
    if (t.End() || !ok) return false;
    wi.x = x; wi.y = y; wi.w = a.width; wi.h = a.height;
    if (viewable) *viewable = a.map_state == IsViewable;
    return true;
}

::Window Pick(Display* d, std::string& err)
{
    const ::Window root = DefaultRootWindow(d);
    Cursor c = XCreateFontCursor(d, XC_crosshair);
    if (XGrabPointer(d, root, False, ButtonPressMask | ButtonReleaseMask, GrabModeSync, GrabModeAsync, root, c, CurrentTime) != GrabSuccess) {
        XFreeCursor(d, c);
        err = "could not take the mouse to pick a window (another program holds it)";
        return 0;
    }
    std::fprintf(stderr, "pushmpc-capture: click the player's window\n");
    ::Window picked = 0;
    for (;;) {
        XEvent ev;
        XAllowEvents(d, SyncPointer, CurrentTime);
        XWindowEvent(d, root, ButtonPressMask, &ev);
        if (ev.type == ButtonPress) { picked = ev.xbutton.subwindow ? ev.xbutton.subwindow : ev.xbutton.window; break; }
    }
    XUngrabPointer(d, CurrentTime);
    XFreeCursor(d, c);
    XSync(d, False);
    if (!picked || picked == root) { err = "no window was picked"; return 0; }
    const ::Window client = ClientBelow(d, picked);
    return client ? client : picked;
}

// the player by name: WM_CLASS or its process; the largest visible match
::Window FindPlayer(Display* d, const std::string& name)
{
    const std::string want = Lower(name);
    ::Window best = 0; long bestArea = -1;
    for (::Window w : Clients(d)) {
        const std::string hay = Lower(ClassOf(d, w) + " " + ProcessOf(d, w));
        if (hay.find(want) == std::string::npos) continue;
        WindowInfo wi; bool viewable = false;
        if (!Geometry(d, w, wi, &viewable) || !viewable) continue;
        const long area = long(wi.w) * long(wi.h);
        if (area > bestArea) { bestArea = area; best = w; }
    }
    return best;
}

class X11Source final : public Source {
public:
    explicit X11Source(const Options& o) : m_o(o) {}
    ~X11Source() override;
    bool Start(std::string& err) override;
    bool Next(SourceFrame& f, int timeoutMs) override;
    bool Alive(std::string& why) override { why = m_why; return !m_dead; }
    std::string Describe() const override;
    std::string PlayerName() const override { return m_player; }
    uint64_t Window() const override { return uint64_t(m_win); }

private:
    struct Img { XImage* img = nullptr; XShmSegmentInfo shm{}; std::atomic<bool> busy{ false }; };
    bool Setup(std::string& err);
    void DropImages();
    void Events();
    bool Grab(Img& im);

    Options m_o;
    Display* m_d = nullptr;
    ::Window m_win = 0;
    Pixmap m_pix = 0;
    Damage m_damage = 0;
    int m_dmgEvent = 0, m_dmgError = 0;
    bool m_useShm = true, m_dirty = true, m_dead = false, m_reconf = false, m_mapped = true;
    Img m_img[3];
    int m_w = 0, m_h = 0, m_depth = 24;
    Visual* m_vis = nullptr;
    PixFmt m_fmt = PixFmt::BGRX;
    std::string m_why, m_player, m_class;
};

X11Source::~X11Source()
{
    if (!m_d) return;
    XTrap t(m_d);
    DropImages();
    if (m_damage) XDamageDestroy(m_d, m_damage);
    if (m_pix) XFreePixmap(m_d, m_pix);
    if (m_win) XCompositeUnredirectWindow(m_d, m_win, CompositeRedirectAutomatic);
    t.End();
    XCloseDisplay(m_d);
}

bool X11Source::Start(std::string& err)
{
    XInitThreads();
    m_d = XOpenDisplay(nullptr);
    if (!m_d) { err = "cannot open the X display (DISPLAY is not set or not reachable)"; return false; }
    XSetErrorHandler(X11ErrorHandler);
    int ev = 0, er = 0, major = 0, minor = 2;
    if (!XCompositeQueryExtension(m_d, &ev, &er) || !XCompositeQueryVersion(m_d, &major, &minor) || (major == 0 && minor < 2)) {
        err = "the X server has no Composite extension 0.2 (window capture needs it)"; return false;
    }
    if (!XDamageQueryExtension(m_d, &m_dmgEvent, &m_dmgError)) { err = "the X server has no Damage extension"; return false; }
    m_useShm = XShmQueryExtension(m_d) && std::getenv("PUSHMPC_CAPTURE_NO_SHM") == nullptr;
    if (!m_o.window.empty()) {
        m_win = ::Window(std::strtoul(m_o.window.c_str(), nullptr, 0));
        if (!m_win) { err = "--window takes an X11 window id (see --list)"; return false; }
        if (const ::Window c = ClientBelow(m_d, m_win)) m_win = c;
    } else if (m_o.pick) {
        m_win = Pick(m_d, err);
        if (!m_win) return false;
    } else if (!m_o.player.empty()) {
        m_win = FindPlayer(m_d, m_o.player);
        if (!m_win) { err = "no visible window of \"" + m_o.player + "\" (start the player first; --list shows the windows)"; return false; }
    } else {
        err = "say which window: --player <name>, --window <id> or --pick";
        return false;
    }
    m_class = ClassOf(m_d, m_win);
    m_player = m_class.substr(0, m_class.find('/'));
    XTrap t(m_d);
    XSelectInput(m_d, m_win, StructureNotifyMask);
    XCompositeRedirectWindow(m_d, m_win, CompositeRedirectAutomatic);
    m_damage = XDamageCreate(m_d, m_win, XDamageReportNonEmpty);
    if (t.End()) { err = "the window cannot be captured (it closed, or the X server refused)"; return false; }
    return Setup(err);
}

void X11Source::DropImages()
{
    for (auto& im : m_img) {
        if (!im.img) continue;
        if (m_useShm) {
            XShmDetach(m_d, &im.shm);
            XDestroyImage(im.img);   // an XShm image's destroy frees the structure, not the segment
            shmdt(im.shm.shmaddr);
        } else {
            XDestroyImage(im.img);
        }
        im.img = nullptr;
        im.shm = {};
    }
}

bool X11Source::Setup(std::string& err)
{
    DropImages();
    if (m_pix) { XTrap t(m_d); XFreePixmap(m_d, m_pix); t.End(); m_pix = 0; }
    XWindowAttributes a{};
    XTrap t(m_d);
    const int ok = XGetWindowAttributes(m_d, m_win, &a);
    if (t.End() || !ok) { m_dead = true; m_why = "the player's window closed"; err = m_why; return false; }
    m_w = a.width; m_h = a.height; m_vis = a.visual; m_depth = a.depth;
    m_mapped = a.map_state == IsViewable;
    if (m_depth != 24 && m_depth != 32) { err = "the window has " + std::to_string(m_depth) + "-bit colour (capture takes 24 / 32 bit)"; return false; }
    m_fmt = m_vis && m_vis->red_mask == 0xffu ? PixFmt::RGBX : PixFmt::BGRX;
    if (!m_mapped) return true;   // named once it is shown again (MapNotify)
    XTrap tp(m_d);
    m_pix = XCompositeNameWindowPixmap(m_d, m_win);
    if (tp.End()) { m_pix = 0; return true; }   // not viewable yet: MapNotify / ConfigureNotify tries again
    if (m_useShm) {
        for (auto& im : m_img) {
            im.img = XShmCreateImage(m_d, m_vis, unsigned(m_depth), ZPixmap, nullptr, &im.shm, unsigned(m_w), unsigned(m_h));
            if (!im.img || im.img->bits_per_pixel != 32) { if (im.img) XDestroyImage(im.img); im.img = nullptr; m_useShm = false; break; }
            im.shm.shmid = shmget(IPC_PRIVATE, size_t(im.img->bytes_per_line) * size_t(m_h), IPC_CREAT | 0600);
            if (im.shm.shmid < 0) { XDestroyImage(im.img); im.img = nullptr; m_useShm = false; break; }
            void* addr = shmat(im.shm.shmid, nullptr, 0);
            if (addr == reinterpret_cast<void*>(-1)) { shmctl(im.shm.shmid, IPC_RMID, nullptr); XDestroyImage(im.img); im.img = nullptr; im.shm = {}; m_useShm = false; break; }
            im.shm.shmaddr = im.img->data = static_cast<char*>(addr);
            im.shm.readOnly = False;
            XTrap ts(m_d);
            const int att = XShmAttach(m_d, &im.shm);
            const int e = ts.End();
            shmctl(im.shm.shmid, IPC_RMID, nullptr);   // freed once both sides detach
            if (!att || e) { shmdt(im.shm.shmaddr); XDestroyImage(im.img); im.img = nullptr; im.shm = {}; m_useShm = false; break; }
        }
        if (!m_useShm) { DropImages(); std::fprintf(stderr, "pushmpc-capture: shared memory with the X server failed: copying through the socket\n"); }
    }
    m_dirty = true;
    return true;
}

void X11Source::Events()
{
    while (XPending(m_d)) {
        XEvent ev;
        XNextEvent(m_d, &ev);
        if (ev.type == m_dmgEvent + XDamageNotify) { m_dirty = true; continue; }
        switch (ev.type) {
        case ConfigureNotify:
            if (ev.xconfigure.window == m_win && (ev.xconfigure.width != m_w || ev.xconfigure.height != m_h)) m_reconf = true;
            break;
        case MapNotify:
            if (ev.xmap.window == m_win) { m_mapped = true; m_reconf = true; }
            break;
        case UnmapNotify:
            if (ev.xunmap.window == m_win) m_mapped = false;
            break;
        case DestroyNotify:
            if (ev.xdestroywindow.window == m_win) { m_dead = true; m_why = "the player's window closed"; }
            break;
        default: break;
        }
    }
}

bool X11Source::Grab(Img& im)
{
    XTrap t(m_d);
    bool ok = false;
    if (m_useShm) {
        ok = XShmGetImage(m_d, m_pix, im.img, 0, 0, AllPlanes) != 0;
    } else {
        if (im.img) { XDestroyImage(im.img); im.img = nullptr; }
        im.img = XGetImage(m_d, m_pix, 0, 0, unsigned(m_w), unsigned(m_h), AllPlanes, ZPixmap);
        ok = im.img && im.img->bits_per_pixel == 32;
    }
    if (t.End()) { ok = false; m_reconf = true; }   // the pixmap went stale (a resize in between): name it again
    return ok;
}

bool X11Source::Next(SourceFrame& f, int timeoutMs)
{
    const double end = Now() + std::max(0, timeoutMs) * 1e-3;
    for (;;) {
        Events();
        if (m_dead) return false;
        if (m_reconf) {
            bool idle = true;
            for (auto& im : m_img) if (im.busy) idle = false;
            if (idle) {   // only when no image is lent out
                std::string e;
                m_reconf = false;
                if (!Setup(e)) return false;
            }
        }
        if (m_dirty && m_pix && m_mapped && !m_reconf) {
            Img* free = nullptr;
            for (auto& im : m_img) if (!im.busy) { free = &im; break; }
            if (free) {
                XDamageSubtract(m_d, m_damage, 0, 0);
                m_dirty = false;
                const double t = Now();
                if (Grab(*free)) {
                    free->busy = true;
                    f = SourceFrame{};
                    f.width = m_w; f.height = m_h; f.fmt = m_fmt;
                    f.ptr = reinterpret_cast<const uint8_t*>(free->img->data);
                    f.pitch = free->img->bytes_per_line;
                    f.time = t;
                    Img* im = free;
                    f.release = [im] { im->busy = false; };
                    return true;
                }
            }
        }
        const double left = end - Now();
        if (left <= 0) return false;
        XFlush(m_d);
        pollfd p{ ConnectionNumber(m_d), POLLIN, 0 };
        // with every image lent out, look again soon (the engine gives one back)
        const int ms = (m_dirty && m_pix) ? std::min(2, int(left * 1000) + 1) : int(left * 1000) + 1;
        poll(&p, 1, ms);
    }
}

std::string X11Source::Describe() const
{
    char b[256];
    std::snprintf(b, sizeof(b), "X11 window 0x%lx (%s), %dx%d, XComposite + XDamage, %s", static_cast<unsigned long>(m_win), m_class.c_str(), m_w, m_h,
                  m_useShm ? "shared memory" : "copied through the X socket");
    return b;
}

} // namespace

// one handler for the whole process (Xlib's is global; the presenter installs the same): it records, never exits
int X11ErrorHandler(Display*, XErrorEvent* e) { g_xerr = e->error_code ? e->error_code : 1; return 0; }

std::unique_ptr<Source> MakeX11Source(const Options& o) { return std::make_unique<X11Source>(o); }

bool ListX11Windows(std::vector<WindowInfo>& out, std::string& err)
{
    Display* d = XOpenDisplay(nullptr);
    if (!d) { err = "cannot open the X display"; return false; }
    XSetErrorHandler(X11ErrorHandler);
    for (::Window w : Clients(d)) {
        WindowInfo wi; bool viewable = false;
        if (!Geometry(d, w, wi, &viewable) || !viewable) continue;
        wi.id = uint64_t(w);
        wi.title = Title(d, w);
        std::string proc = ProcessOf(d, w);
        wi.owner = ClassOf(d, w) + (proc.empty() ? "" : " [" + proc.substr(0, proc.find(' ')) + "]");
        out.push_back(wi);
    }
    XCloseDisplay(d);
    return true;
}

} // namespace pmc
