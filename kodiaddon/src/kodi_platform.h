// PushMPC Kodi add-on: what Kodi's Linux renderers can take (dma-buf frame buffers, 16-bit textures).
// Linux only (not Android); elsewhere every check says "no" and the callers keep system memory.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

#if defined(__linux__) && !defined(__ANDROID__)
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace pmk {

constexpr uint32_t DrmFourcc(char a, char b, char c, char d)
{
    return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16) | (uint32_t(uint8_t(d)) << 24);
}
constexpr uint32_t kDrmYuv420 = DrmFourcc('Y', 'U', '1', '2');   // DRM_FORMAT_YUV420: Kodi's dma-buf pool for I420
constexpr uint32_t kDrmP010 = DrmFourcc('P', '0', '1', '0');     // DRM_FORMAT_P010: Kodi's dma-buf pool for P010

// Can Kodi allocate dma-bufs at all: the same devices its buffer objects open (UDMABufferObject, DMAHeapBufferObject).
// Kodi on GBM always has its dumb buffer object.
inline bool KodiHasDmaAllocator(bool gbm, std::string& which)
{
#if defined(__linux__) && !defined(__ANDROID__)
    if (gbm) { which = "GBM dumb buffers"; return true; }
    static const char* const devs[] = {"/dev/udmabuf", "/dev/dma_heap/reserved", "/dev/dma_heap/linux,cma", "/dev/dma_heap/system"};
    for (const char* d : devs) {
        const int fd = open(d, O_RDWR | O_CLOEXEC);
        if (fd >= 0) { close(fd); which = d; return true; }
    }
    which = "none (no access to /dev/udmabuf or a dma heap)";
    return false;
#else
    (void)gbm; which = "none";
    return false;
#endif
}

// Kodi's DRM-PRIME renderer is created only with videoplayer.useprimedecoder = true and videoplayer.useprimerenderer = 1
// (EGL). Read from guisettings.xml; an absent entry has Kodi's default (false, and 1).
inline bool KodiPrimeRendererOn(const std::string& guisettings, std::string& why)
{
    std::string xml;
    if (FILE* f = std::fopen(guisettings.c_str(), "rb")) {
        char buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) xml.append(buf, n);
        std::fclose(f);
    }
    auto value = [&](const char* id) -> std::string {
        const std::string key = std::string("id=\"") + id + "\"";
        const size_t at = xml.find(key);
        if (at == std::string::npos) return {};
        const size_t gt = xml.find('>', at);
        if (gt == std::string::npos || xml[gt - 1] == '/') return {};
        const size_t lt = xml.find('<', gt);
        if (lt == std::string::npos) return {};
        std::string v = xml.substr(gt + 1, lt - gt - 1);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\n' || v.back() == '\r' || v.back() == '\t')) v.pop_back();
        while (!v.empty() && (v.front() == ' ' || v.front() == '\n' || v.front() == '\r' || v.front() == '\t')) v.erase(0, 1);
        return v;
    };
    const std::string dec = value("videoplayer.useprimedecoder");
    const std::string ren = value("videoplayer.useprimerenderer");
    const bool decOn = dec == "true";
    const bool renOn = ren.empty() || ren == "1";
    why = std::string("useprimedecoder ") + (dec.empty() ? "default (off)" : dec) + ", useprimerenderer " + (ren.empty() ? "default (1)" : ren);
    return decOn && renOn;
}

// The GPU's EGL: the dma-buf formats it imports (eglQueryDmaBufFormatsEXT; Kodi's CEGLImage::SupportsFormatAndModifier
// takes LINEAR for any listed format) and whether GLES has GL_EXT_texture_norm16 (Kodi's 16-bit planar textures).
// Checked once per process on an EGL display of our own (surfaceless, else the first EGL device), loaded at run time.
struct EglCaps {
    bool queried = false;    // an EGL display answered
    bool formatsKnown = false;
    std::set<uint32_t> formats;
    int norm16 = -1;         // 1 yes, 0 no, -1 unknown
    std::string how;
};

inline const EglCaps& QueryEglCaps()
{
    static EglCaps caps;
    static bool done = false;
    if (done) return caps;
    done = true;
#if defined(__linux__) && !defined(__ANDROID__)
    using EGLDisplay = void*;
    using EGLConfig = void*;
    using EGLContext = void*;
    using EGLint = int32_t;
    using EGLBoolean = unsigned int;
    using EGLenum = unsigned int;
    using Proc = void (*)();
    const EGLint EGL_EXTENSIONS = 0x3055, EGL_NONE = 0x3038, EGL_RENDERABLE_TYPE = 0x3040, EGL_OPENGL_ES2_BIT = 0x0004,
                 EGL_OPENGL_ES3_BIT = 0x0040, EGL_CONTEXT_MAJOR_VERSION = 0x3098, EGL_SURFACE_TYPE = 0x3033;
    const EGLenum EGL_PLATFORM_SURFACELESS_MESA = 0x31DD, EGL_PLATFORM_DEVICE_EXT = 0x313F, EGL_OPENGL_ES_API = 0x30A0;
    const unsigned GL_EXTENSIONS_ = 0x1F03;
    void* egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!egl) egl = dlopen("libEGL.so", RTLD_NOW | RTLD_LOCAL);
    if (!egl) { caps.how = "no libEGL"; return caps; }
    auto getProc = reinterpret_cast<Proc (*)(const char*)>(dlsym(egl, "eglGetProcAddress"));
    auto queryString = reinterpret_cast<const char* (*)(EGLDisplay, EGLint)>(dlsym(egl, "eglQueryString"));
    auto initialize = reinterpret_cast<EGLBoolean (*)(EGLDisplay, EGLint*, EGLint*)>(dlsym(egl, "eglInitialize"));
    if (!getProc || !queryString || !initialize) { caps.how = "libEGL without its entry points"; return caps; }
    auto getPlatformDisplay = reinterpret_cast<EGLDisplay (*)(EGLenum, void*, const EGLint*)>(getProc("eglGetPlatformDisplayEXT"));
    auto queryDevices = reinterpret_cast<EGLBoolean (*)(EGLint, void**, EGLint*)>(getProc("eglQueryDevicesEXT"));
    auto queryFormats = reinterpret_cast<EGLBoolean (*)(EGLDisplay, EGLint, EGLint*, EGLint*)>(getProc("eglQueryDmaBufFormatsEXT"));
    const char* clientExt = queryString(nullptr, EGL_EXTENSIONS);
    auto has = [](const char* list, const char* e) { return list && std::strstr(list, e) != nullptr; };
    EGLDisplay dpy = nullptr;
    if (getPlatformDisplay && has(clientExt, "EGL_MESA_platform_surfaceless")) {
        dpy = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, nullptr, nullptr);
        if (dpy && !initialize(dpy, nullptr, nullptr)) dpy = nullptr;
        if (dpy) caps.how = "EGL surfaceless";
    }
    if (!dpy && getPlatformDisplay && queryDevices && has(clientExt, "EGL_EXT_platform_device")) {
        void* devs[8] = {};
        EGLint n = 0;
        if (queryDevices(8, devs, &n) && n > 0) {
            dpy = getPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, devs[0], nullptr);
            if (dpy && !initialize(dpy, nullptr, nullptr)) dpy = nullptr;
            if (dpy) caps.how = "EGL device";
        }
    }
    if (!dpy) { caps.how = "no EGL display of our own"; return caps; }
    caps.queried = true;
    const char* dpyExt = queryString(dpy, EGL_EXTENSIONS);
    if (queryFormats && has(dpyExt, "EGL_EXT_image_dma_buf_import_modifiers")) {
        EGLint n = 0;
        if (queryFormats(dpy, 0, nullptr, &n) && n > 0) {
            std::string list(size_t(n) * sizeof(EGLint), '\0');
            EGLint* f = reinterpret_cast<EGLint*>(&list[0]);
            if (queryFormats(dpy, n, f, &n)) {
                for (EGLint i = 0; i < n; ++i) caps.formats.insert(uint32_t(f[i]));
                caps.formatsKnown = true;
            }
        }
    }
    // GLES extensions: a context of our own made current without a surface on this thread (Kodi's is on its render thread)
    auto bindApi = reinterpret_cast<EGLBoolean (*)(EGLenum)>(dlsym(egl, "eglBindAPI"));
    auto chooseConfig = reinterpret_cast<EGLBoolean (*)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*)>(dlsym(egl, "eglChooseConfig"));
    auto createContext = reinterpret_cast<EGLContext (*)(EGLDisplay, EGLConfig, EGLContext, const EGLint*)>(dlsym(egl, "eglCreateContext"));
    auto makeCurrent = reinterpret_cast<EGLBoolean (*)(EGLDisplay, void*, void*, EGLContext)>(dlsym(egl, "eglMakeCurrent"));
    auto destroyContext = reinterpret_cast<EGLBoolean (*)(EGLDisplay, EGLContext)>(dlsym(egl, "eglDestroyContext"));
    auto getCurrentContext = reinterpret_cast<EGLContext (*)()>(dlsym(egl, "eglGetCurrentContext"));
    auto getString = reinterpret_cast<const unsigned char* (*)(unsigned)>(getProc("glGetString"));
    if (bindApi && chooseConfig && createContext && makeCurrent && destroyContext && getString && getCurrentContext &&
        !getCurrentContext() && has(dpyExt, "EGL_KHR_surfaceless_context") && bindApi(EGL_OPENGL_ES_API)) {
        for (EGLint major : {3, 2}) {
            const EGLint cfgAttr[] = {EGL_RENDERABLE_TYPE, major == 3 ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE};
            EGLConfig cfg = nullptr;
            EGLint nc = 0;
            if (!chooseConfig(dpy, cfgAttr, &cfg, 1, &nc) || nc < 1) continue;
            const EGLint ctxAttr[] = {EGL_CONTEXT_MAJOR_VERSION, major, EGL_NONE};
            EGLContext ctx = createContext(dpy, cfg, nullptr, ctxAttr);
            if (!ctx) continue;
            if (makeCurrent(dpy, nullptr, nullptr, ctx)) {
                const char* ext = reinterpret_cast<const char*>(getString(GL_EXTENSIONS_));
                if (ext) caps.norm16 = has(ext, "GL_EXT_texture_norm16") ? 1 : 0;
                makeCurrent(dpy, nullptr, nullptr, nullptr);
            }
            destroyContext(dpy, ctx);
            if (caps.norm16 >= 0) break;
        }
    }
    // the display stays initialised (once per process): terminating it could tear down state another user shares
#endif
    return caps;
}

}  // namespace pmk
