// Whether variable refresh is running, for Auto's cadence rule on the routes outside the Windows filter (PushMpcAuto.h
// keeps NVIDIA's / AMD's own queries for Windows). Linux / BSD: the kernel's DRM state, read through libdrm loaded at run
// time (no build or link dependency): the CRTC property "VRR_ENABLED" is what the compositor (Wayland, GBM such as
// Kodi on LibreELEC) or the X driver (amdgpu, modesetting) has switched on for an output. It is display-wide, like
// NVIDIA's "VRR running" flag, and it says what runs NOW (a compositor that enables VRR only for full screen sets it once
// the player is full screen). Not covered: drivers that do not expose the property (NVIDIA's proprietary X11 driver),
// X11 RandR has no standard VRR output property. Everything else (Windows, macOS, no DRM access): unknown.
//   LinuxVrrActive(): 1 = VRR is on for an active output, 0 = the outputs have the property and none has it on,
//   -1 = unknown (Auto then assumes a fixed refresh, as before).
// VrrActive(fullscreen) adds the other sources (fullscreen: 1 / 0 / -1 unknown; VRR on a fullscreen-only scope engages
// for a full-screen player only):
//   - NVIDIA's proprietary X11 driver: NV-CONTROL through libXNVCtrl.so.0 (run-time load): NV_CTRL_DISPLAY_VRR_ENABLED (431,
//     any display target) and NV_CTRL_VRR_ALLOWED (408, the X screen), plus full screen (G-SYNC needs flipping);
//   - Windows (MSVC builds): the DirectShow filter's own NVAPI / AMD ADL probe (PushMpcAuto.h ProbeVrr: the driver
//     profile's G-SYNC scope, the monitor's capability, AMD FreeSync switched on) on the primary display (these routes do
//     not know the player's window); the MinGW-built VLC plugin keeps -1;
//   - macOS: NSScreen.mainScreen minimumRefreshInterval < maximumRefreshInterval (macOS 12+: ProMotion or external
//     Adaptive-Sync), through the Objective-C runtime (dlsym, no link dependency).
// Not covered (-1, fixed refresh assumed): X11 RandR has no standard "VRR active" property (DRM covers amdgpu /
// modesetting); Intel's IGCL has no confirmed "VRR active" query.
#pragma once
#include <cstdint>
#if defined(__linux__) || defined(__FreeBSD__)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <dlfcn.h>
#elif defined(_WIN32) && defined(_MSC_VER)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../../filter/src/PushMpcAuto.h"   // ProbeVrr (NVAPI / ADL, header-only, run-time loads)
#endif

namespace pmvs {

#if defined(__linux__) || defined(__FreeBSD__)
namespace vrrdetail {
// the leading members of libdrm's xf86drmMode.h structures (stable ABI since libdrm 2.4)
struct Res { int count_fbs; uint32_t* fbs; int count_crtcs; uint32_t* crtcs; int count_connectors; uint32_t* connectors; int count_encoders; uint32_t* encoders;
             uint32_t min_width, max_width, min_height, max_height; };
struct ObjProps { uint32_t count_props; uint32_t* props; uint64_t* prop_values; };
struct PropHead { uint32_t prop_id; uint32_t flags; char name[32]; };
struct CrtcHead { uint32_t crtc_id, buffer_id, x, y, width, height; int mode_valid; };
constexpr uint32_t kObjectCrtc = 0xccccccccu;   // DRM_MODE_OBJECT_CRTC
}
inline int LinuxVrrActive()
{
    using namespace vrrdetail;
    void* lib = dlopen("libdrm.so.2", RTLD_LAZY | RTLD_LOCAL);
    if (!lib) return -1;
    auto getRes = reinterpret_cast<Res* (*)(int)>(dlsym(lib, "drmModeGetResources"));
    auto freeRes = reinterpret_cast<void (*)(Res*)>(dlsym(lib, "drmModeFreeResources"));
    auto getCrtc = reinterpret_cast<CrtcHead* (*)(int, uint32_t)>(dlsym(lib, "drmModeGetCrtc"));
    auto freeCrtc = reinterpret_cast<void (*)(CrtcHead*)>(dlsym(lib, "drmModeFreeCrtc"));
    auto getProps = reinterpret_cast<ObjProps* (*)(int, uint32_t, uint32_t)>(dlsym(lib, "drmModeObjectGetProperties"));
    auto freeProps = reinterpret_cast<void (*)(ObjProps*)>(dlsym(lib, "drmModeFreeObjectProperties"));
    auto getProp = reinterpret_cast<PropHead* (*)(int, uint32_t)>(dlsym(lib, "drmModeGetProperty"));
    auto freeProp = reinterpret_cast<void (*)(PropHead*)>(dlsym(lib, "drmModeFreeProperty"));
    int result = -1;
    if (getRes && freeRes && getCrtc && freeCrtc && getProps && freeProps && getProp && freeProp) {
        for (int card = 0; card < 16 && result != 1; ++card) {
            char path[32];
            std::snprintf(path, sizeof(path), "/dev/dri/card%d", card);
            const int fd = open(path, O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            if (Res* res = getRes(fd)) {
                for (int i = 0; i < res->count_crtcs && result != 1; ++i) {
                    CrtcHead* crtc = getCrtc(fd, res->crtcs[i]);
                    const bool active = crtc && crtc->mode_valid;
                    if (crtc) freeCrtc(crtc);
                    if (!active) continue;
                    if (ObjProps* p = getProps(fd, res->crtcs[i], kObjectCrtc)) {
                        for (uint32_t j = 0; j < p->count_props; ++j) {
                            PropHead* h = getProp(fd, p->props[j]);
                            if (!h) continue;
                            if (std::strncmp(h->name, "VRR_ENABLED", sizeof(h->name)) == 0) result = p->prop_values[j] ? 1 : (result < 0 ? 0 : result);
                            freeProp(h);
                        }
                        freeProps(p);
                    }
                }
                freeRes(res);
            }
            close(fd);
        }
    }
    dlclose(lib);
    return result;
}
// NVIDIA's proprietary X11 driver (NV-CONTROL; libXNVCtrl's API as in nvidia-settings' NVCtrlLib.h)
inline int NvCtrlVrrActive(int fullscreen)
{
    if (!std::getenv("DISPLAY")) return -1;
    void* x11 = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
    void* nvc = x11 ? dlopen("libXNVCtrl.so.0", RTLD_LAZY | RTLD_LOCAL) : nullptr;
    int result = -1;
    if (x11 && nvc) {
        using Dpy = void;
        auto openD = reinterpret_cast<Dpy* (*)(const char*)>(dlsym(x11, "XOpenDisplay"));
        auto closeD = reinterpret_cast<int (*)(Dpy*)>(dlsym(x11, "XCloseDisplay"));
        auto defScreen = reinterpret_cast<int (*)(Dpy*)>(dlsym(x11, "XDefaultScreen"));
        auto queryExt = reinterpret_cast<int (*)(Dpy*, int*, int*)>(dlsym(nvc, "XNVCTRLQueryExtension"));
        auto queryAttr = reinterpret_cast<int (*)(Dpy*, int, unsigned, unsigned, int*)>(dlsym(nvc, "XNVCTRLQueryAttribute"));
        auto queryTarget = reinterpret_cast<int (*)(Dpy*, int, int, unsigned, unsigned, int*)>(dlsym(nvc, "XNVCTRLQueryTargetAttribute"));
        Dpy* d = openD && closeD ? openD(nullptr) : nullptr;
        int ev = 0, er = 0;
        if (d && defScreen && queryExt && queryAttr && queryTarget && queryExt(d, &ev, &er)) {
            constexpr unsigned kVrrAllowed = 408, kDisplayVrrEnabled = 431;
            constexpr int kTargetDisplay = 8;   // NV_CTRL_TARGET_TYPE_DISPLAY
            int allowed = 0, any = -1;
            const bool haveAllowed = queryAttr(d, defScreen(d), 0, kVrrAllowed, &allowed) != 0;
            for (int id = 0; id < 32 && any != 1; ++id) {
                int v = 0;
                if (queryTarget(d, kTargetDisplay, id, 0, kDisplayVrrEnabled, &v)) any = v ? 1 : (any < 0 ? 0 : any);
            }
            if (any >= 0) result = any == 1 && (!haveAllowed || allowed) && fullscreen == 1 ? 1 : 0;
        }
        if (d) closeD(d);
    }
    if (nvc) dlclose(nvc);
    if (x11) dlclose(x11);
    return result;
}
inline int VrrActive(int fullscreen)
{
    const int drm = LinuxVrrActive(), nv = NvCtrlVrrActive(fullscreen);
    return drm == 1 || nv == 1 ? 1 : drm == 0 || nv == 0 ? 0 : -1;
}
#else
inline int LinuxVrrActive() { return -1; }
#if defined(__APPLE__)
// NSScreen.mainScreen's minimum / maximum refresh interval through the Objective-C runtime (AppKit is loaded in the players)
inline int VrrActive(int)
{
    using Id = void*;
    using Sel = void*;
    auto getClass = reinterpret_cast<Id (*)(const char*)>(dlsym(RTLD_DEFAULT, "objc_getClass"));
    auto selName = reinterpret_cast<Sel (*)(const char*)>(dlsym(RTLD_DEFAULT, "sel_registerName"));
    void* send = dlsym(RTLD_DEFAULT, "objc_msgSend");
    if (!getClass || !selName || !send) return -1;
    const Id cls = getClass("NSScreen");
    if (!cls) return -1;
    const Id screen = reinterpret_cast<Id (*)(Id, Sel)>(send)(cls, selName("mainScreen"));
    const Sel minSel = selName("minimumRefreshInterval"), maxSel = selName("maximumRefreshInterval"), resp = selName("respondsToSelector:");
    auto responds = reinterpret_cast<signed char (*)(Id, Sel, Sel)>(send);
    if (!screen || !responds(screen, resp, minSel) || !responds(screen, resp, maxSel)) return -1;
    const double mn = reinterpret_cast<double (*)(Id, Sel)>(send)(screen, minSel), mx = reinterpret_cast<double (*)(Id, Sel)>(send)(screen, maxSel);
    if (!(mn > 0) || !(mx > 0)) return -1;
    return mn < mx * 0.99 ? 1 : 0;
}
#elif defined(_WIN32) && defined(_MSC_VER)
// the primary display (a portable route does not know the player's window): NVAPI's profile scope / capability, AMD FreeSync
inline int VrrActive(int fullscreen)
{
    MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi)) return -1;
    const pm::VrrProbe p = pm::ProbeVrr(nullptr, mi, true);
    if (!p.known) return -1;
    const bool covers = p.scope == 2 || (p.scope == 1 && fullscreen == 1);
    return (p.possible || p.caps) && covers ? 1 : 0;
}
#else
inline int VrrActive(int) { return -1; }
#endif
#endif

} // namespace pmvs
