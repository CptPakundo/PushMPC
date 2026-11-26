// Whether variable refresh is running, for Auto's cadence rule on the routes outside the Windows filter (PushMpcAuto.h
// keeps NVIDIA's / AMD's own queries for Windows). Linux / BSD: the kernel's DRM state, read through libdrm loaded at run
// time (no build or link dependency): the CRTC property "VRR_ENABLED" is what the compositor (Wayland, GBM such as
// Kodi on LibreELEC) or the X driver (amdgpu, modesetting) has switched on for an output. It is display-wide, like
// NVIDIA's "VRR running" flag, and it says what runs NOW (a compositor that enables VRR only for full screen sets it once
// the player is full screen). Not covered: drivers that do not expose the property (NVIDIA's proprietary X11 driver),
// X11 RandR has no standard VRR output property. Everything else (Windows, macOS, no DRM access): unknown.
//   LinuxVrrActive(): 1 = VRR is on for an active output, 0 = the outputs have the property and none has it on,
//   -1 = unknown (Auto then assumes a fixed refresh, as before).
#pragma once
#include <cstdint>
#if defined(__linux__) || defined(__FreeBSD__)
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
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
#else
inline int LinuxVrrActive() { return -1; }
#endif

} // namespace pmvs
