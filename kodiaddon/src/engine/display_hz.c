/* display_hz: the refresh rate of the display the video is most likely on (Auto's multiplier target).
 * Windows: the primary display's current mode (EnumDisplaySettings). macOS: the main display (CoreGraphics).
 * Linux/BSD: X11 RandR (also XWayland), loaded at run time so the plugin has no X11 link dependency; the highest rate
 * among active CRTCs. Override: environment variable PUSHMPC_DISPLAY_HZ. Returns 0 when unknown. */
#include <stdlib.h>
#include <string.h>

double pm_display_hz(void);

static double FromEnv(void)
{
    const char *s = getenv("PUSHMPC_DISPLAY_HZ");
    return s ? atof(s) : 0.0;
}

#if defined(_WIN32)
#include <windows.h>
double pm_display_hz(void)
{
    double e = FromEnv();
    if (e > 0) return e;
    DEVMODEW dm;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        return (double)dm.dmDisplayFrequency; /* integer Hz; 59/23 mean the 1000/1001 rates */
    return 0.0;
}
#elif defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
double pm_display_hz(void)
{
    double e = FromEnv();
    if (e > 0) return e;
    CGDisplayModeRef m = CGDisplayCopyDisplayMode(CGMainDisplayID());
    if (!m) return 0.0;
    double hz = CGDisplayModeGetRefreshRate(m);
    CGDisplayModeRelease(m);
    return hz; /* 0 on some built-in panels (ProMotion reports 120 on recent macOS) */
}
#else
#include <dlfcn.h>
/* the few Xlib / Xrandr declarations used, so no X11 headers are needed at build time */
typedef struct _XDisplay XDisplay;
typedef unsigned long XID_;
typedef struct { XID_ id; unsigned int width, height; unsigned long dotClock; unsigned int hSyncStart, hSyncEnd, hTotal,
                 hSkew, vSyncStart, vSyncEnd, vTotal; char *name; unsigned int nameLength; unsigned long modeFlags; } XRRModeInfo_;
typedef struct { unsigned long timestamp, configTimestamp; int ncrtc; XID_ *crtcs; int noutput; XID_ *outputs; int nmode;
                 XRRModeInfo_ *modes; } XRRScreenResources_;
typedef struct { unsigned long timestamp; int x, y; unsigned int width, height; XID_ mode; } XRRCrtcInfoHead_;
double pm_display_hz(void)
{
    double e = FromEnv();
    if (e > 0) return e;
    void *x11 = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
    void *xrr = dlopen("libXrandr.so.2", RTLD_LAZY | RTLD_LOCAL);
    double best = 0.0;
    if (x11 && xrr && getenv("DISPLAY")) {
        XDisplay *(*open_)(const char *) = (XDisplay *(*)(const char *))dlsym(x11, "XOpenDisplay");
        int (*close_)(XDisplay *) = (int (*)(XDisplay *))dlsym(x11, "XCloseDisplay");
        XID_ (*root_)(XDisplay *) = (XID_ (*)(XDisplay *))dlsym(x11, "XDefaultRootWindow");
        XRRScreenResources_ *(*res_)(XDisplay *, XID_) =
            (XRRScreenResources_ *(*)(XDisplay *, XID_))dlsym(xrr, "XRRGetScreenResourcesCurrent");
        void (*freeres_)(XRRScreenResources_ *) = (void (*)(XRRScreenResources_ *))dlsym(xrr, "XRRFreeScreenResources");
        XRRCrtcInfoHead_ *(*crtc_)(XDisplay *, XRRScreenResources_ *, XID_) =
            (XRRCrtcInfoHead_ *(*)(XDisplay *, XRRScreenResources_ *, XID_))dlsym(xrr, "XRRGetCrtcInfo");
        void (*freecrtc_)(XRRCrtcInfoHead_ *) = (void (*)(XRRCrtcInfoHead_ *))dlsym(xrr, "XRRFreeCrtcInfo");
        XDisplay *d = (open_ && close_ && root_ && res_ && freeres_ && crtc_ && freecrtc_) ? open_(NULL) : NULL;
        if (d) {
            XRRScreenResources_ *r = res_(d, root_(d));
            for (int c = 0; r && c < r->ncrtc; c++) {
                XRRCrtcInfoHead_ *ci = crtc_(d, r, r->crtcs[c]);
                if (!ci) continue;
                for (int m = 0; ci->mode && m < r->nmode; m++) {
                    const XRRModeInfo_ *mi = &r->modes[m];
                    if (mi->id == ci->mode && mi->hTotal && mi->vTotal) {
                        double hz = (double)mi->dotClock / ((double)mi->hTotal * (double)mi->vTotal);
                        if (mi->modeFlags & 0x10) hz *= 2;   /* RR_Interlace */
                        if (mi->modeFlags & 0x20) hz /= 2;   /* RR_DoubleScan */
                        if (hz > best) best = hz;
                    }
                }
                freecrtc_(ci);
            }
            if (r) freeres_(r);
            close_(d);
        }
    }
    if (xrr) dlclose(xrr);
    if (x11) dlclose(x11);
    return best;
}
#endif
