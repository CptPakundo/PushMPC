/*
 * avfilter_proxy_mac.c: libavfilter.8.dylib for Plex for Mac (desktop and HTPC). Plex's libmpv.2.dylib links
 * @rpath/libavfilter.8.dylib; this proxy takes that name in Contents/Frameworks, re-exports every symbol of Plex's
 * original (renamed libavfilter.8-plex.dylib, linked with -reexport_library) and defines avfilter_get_by_name
 * itself: "pushmpc" comes from pushmpc-avfilter-<maj.min.micro>.dylib (a libavfilter of the same FFmpeg release
 * with only vf_pushmpc, which binds Plex's libavutil.57.dylib by name), everything else from the original.
 * Plex's entitlements allow it: hardened runtime with com.apple.security.cs.disable-library-validation, so ad-hoc
 * signed libraries load. PUSHMPC_PROXY_LOG=<file> logs what happened.
 */
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct AVFilter AVFilter;
/* from the re-exported original (linked by name, so these calls go to Plex's libavfilter) */
extern unsigned avfilter_version(void);

static const AVFilter *(*g_real)(const char *);
static const AVFilter *g_pushmpc;
static char g_status[256] = "not initialised";
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void plog(const char *fmt, ...)
{
    const char *p = getenv("PUSHMPC_PROXY_LOG");
    FILE *f;
    va_list ap;
    if (!p || !*p || !(f = fopen(p, "a"))) return;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void init(void)
{
    Dl_info di;
    char dir[1024], path[1200];
    unsigned v = avfilter_version();
    void *orig, *h;
    const AVFilter *(*gbn)(const char *);
    char *slash;
    /* the original's own avfilter_get_by_name (this image defines one too, so look it up in the original only) */
    if (!dladdr((void *)&init, &di) || !di.dli_fname) { snprintf(g_status, sizeof(g_status), "dladdr failed"); return; }
    snprintf(dir, sizeof(dir), "%s", di.dli_fname);
    if ((slash = strrchr(dir, '/'))) *slash = 0;
    snprintf(path, sizeof(path), "%s/libavfilter.8-plex.dylib", dir);
    orig = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
    if (!orig) orig = dlopen(path, RTLD_NOW);
    g_real = orig ? (const AVFilter *(*)(const char *))dlsym(orig, "avfilter_get_by_name") : NULL;
    if (!g_real) { snprintf(g_status, sizeof(g_status), "original %s not usable: %s", path, dlerror()); plog("%s", g_status); return; }
    snprintf(path, sizeof(path), "%s/pushmpc-avfilter-%u.%u.%u.dylib", dir, v >> 16, (v >> 8) & 255, v & 255);
    h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { snprintf(g_status, sizeof(g_status), "libavfilter %u.%u.%u, no build (%s)", v >> 16, (v >> 8) & 255, v & 255, dlerror()); plog("%s", g_status); return; }
    gbn = (const AVFilter *(*)(const char *))dlsym(h, "avfilter_get_by_name");
    g_pushmpc = gbn ? gbn("pushmpc") : NULL;
    snprintf(g_status, sizeof(g_status), "libavfilter %u.%u.%u, pushmpc %s", v >> 16, (v >> 8) & 255, v & 255, g_pushmpc ? "active" : "missing in the build");
    plog("pushmpc proxy: %s", g_status);
}

__attribute__((visibility("default"))) const AVFilter *avfilter_get_by_name(const char *name)
{
    pthread_once(&g_once, init);
    if (name && g_pushmpc && !strcmp(name, "pushmpc")) return g_pushmpc;
    return g_real ? g_real(name) : NULL;
}

__attribute__((visibility("default"))) const char *pushmpc_proxy_status(void)
{
    pthread_once(&g_once, init);
    return g_status;
}
