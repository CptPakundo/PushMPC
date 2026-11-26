/*
 * mpv_proxy_mac.c: a stand-in libmpv for Mac apps that bundle libmpv as a dylib and read no mpv.conf (Stremio 5,
 * Jellyfin Desktop / Jellyfin Media Player). pushmpc-apps-mac.sh renames the app's own libmpv (whatever its name:
 * libmpv.2.dylib, libmpv.dylib, ...) to pushmpc-orig-libmpv.dylib in the same folder and puts this library under the
 * original name. It re-exports every symbol of the original (linked with -reexport_library against a stub with the
 * install name @rpath/pushmpc-orig-libmpv.dylib; LC_RPATH @loader_path finds it next to this file) and defines
 * mpv_initialize itself: after the real one it
 *   - loads the C plugins named in pushmpc-autoload.conf next to this file ("script=<path>", e.g. PushMPC's mpv bridge
 *     pushmpc-iina.so, which runs PushMPC's engines inside this libmpv),
 *   - appends the filter ("vf=<vf_append argument>", e.g. @pushmpc:lavfi=[null@pushmpc]) unless it is there already,
 *   - and puts it back whenever the app replaces the vf list (a client of the app's mpv core watches "vf").
 * "engine=<folder>" sets PUSHMPC_ENGINE for the bridge. No conf file, or "off" in it: the proxy only forwards.
 * Nothing else changes: every other call goes straight to the app's libmpv. Logs to ~/Library/Logs/PushMPC/apps-mac.log.
 * Self-test: the host (libmpv_host.c) loads this file in place of the app's libmpv and calls mpv_initialize through it.
 * Needs the app to allow ad-hoc signed libraries (no hardened runtime, or disable-library-validation): the installer
 * checks the signature first.
 */
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct { int event_id; int error; uint64_t reply_userdata; void *data; } pm_event;
typedef int (*init_fn)(void *);
typedef int (*cmd_fn)(void *, const char **);
typedef void *(*client_fn)(void *, const char *);
typedef int (*observe_fn)(void *, uint64_t, const char *, int);
typedef pm_event *(*wait_fn)(void *, double);
typedef char *(*getstr_fn)(void *, const char *);
typedef void (*free_fn)(void *);

static void *g_orig;
static init_fn g_init;
static cmd_fn g_cmd;
static client_fn g_client;
static observe_fn g_observe;
static wait_fn g_wait;
static getstr_fn g_getstr;
static free_fn g_free, g_destroy;
static char g_dir[1024], g_vf[1024], g_scripts[4][1024];
static int g_nscripts, g_off;
static char g_status[512] = "not initialised";
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void plog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void plog(const char *fmt, ...)
{
    char path[1100];
    const char *home = getenv("HOME");
    FILE *f;
    va_list ap;
    snprintf(path, sizeof(path), "%s/Library/Logs/PushMPC", home && *home ? home : "/tmp");
    mkdir(path, 0755);
    strncat(path, "/apps-mac.log", sizeof(path) - strlen(path) - 1);
    if (!(f = fopen(path, "a"))) return;
    fprintf(f, "pushmpc-mpv-proxy: ");
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
}

static void init(void)
{
    Dl_info di;
    char path[1200], line[1200];
    char *slash;
    FILE *f;
    if (!dladdr((void *)&init, &di) || !di.dli_fname) { snprintf(g_status, sizeof(g_status), "dladdr failed"); return; }
    snprintf(g_dir, sizeof(g_dir), "%s", di.dli_fname);
    if ((slash = strrchr(g_dir, '/'))) *slash = 0;
    snprintf(path, sizeof(path), "%s/pushmpc-orig-libmpv.dylib", g_dir);
    g_orig = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
    if (!g_orig) g_orig = dlopen(path, RTLD_NOW);
    if (!g_orig) { snprintf(g_status, sizeof(g_status), "the app's libmpv %s is not usable: %s", path, dlerror()); plog("%s", g_status); return; }
    g_init = (init_fn)dlsym(g_orig, "mpv_initialize");
    g_cmd = (cmd_fn)dlsym(g_orig, "mpv_command");
    g_client = (client_fn)dlsym(g_orig, "mpv_create_client");
    g_observe = (observe_fn)dlsym(g_orig, "mpv_observe_property");
    g_wait = (wait_fn)dlsym(g_orig, "mpv_wait_event");
    g_getstr = (getstr_fn)dlsym(g_orig, "mpv_get_property_string");
    g_free = (free_fn)dlsym(g_orig, "mpv_free");
    g_destroy = (free_fn)dlsym(g_orig, "mpv_destroy");
    snprintf(path, sizeof(path), "%s/pushmpc-autoload.conf", g_dir);
    if ((f = fopen(path, "r"))) {
        while (fgets(line, sizeof(line), f)) {
            trim(line);
            if (!strcmp(line, "off")) g_off = 1;
            else if (!strncmp(line, "vf=", 3)) snprintf(g_vf, sizeof(g_vf), "%s", line + 3);
            else if (!strncmp(line, "script=", 7) && g_nscripts < 4) snprintf(g_scripts[g_nscripts++], sizeof(g_scripts[0]), "%s", line + 7);
            else if (!strncmp(line, "engine=", 7) && !getenv("PUSHMPC_ENGINE")) setenv("PUSHMPC_ENGINE", line + 7, 0);
        }
        fclose(f);
    } else g_off = 1;
    snprintf(g_status, sizeof(g_status), "%s; vf %s; %d script(s)", g_off ? "forwarding only" : "active",
             g_vf[0] ? g_vf : "(none)", g_nscripts);
}

static int vf_has_ours(void *h)
{
    char *v = g_getstr ? g_getstr(h, "vf") : NULL;
    const int has = v && strstr(v, "@pushmpc") != NULL;
    if (v && g_free) g_free(v);
    return has;
}
static void vf_add(void *h, const char *why)
{
    const char *args[] = { "vf", "append", g_vf, NULL };
    const int r = g_cmd(h, args);
    plog("vf append %s (%s) -> %d", g_vf, why, r);
}
static void *vf_watch(void *client)
{
    int again = 0;
    for (;;) {
        pm_event *ev = g_wait(client, -1);
        if (ev->event_id == 1)   /* MPV_EVENT_SHUTDOWN */
            break;
        if (ev->event_id == 22 && !vf_has_ours(client) && again++ < 200)   /* MPV_EVENT_PROPERTY_CHANGE "vf" */
            vf_add(client, "the app replaced the filter list");
    }
    g_destroy(client);
    return NULL;
}

__attribute__((visibility("default"))) int mpv_initialize(void *h)
{
    int r, i;
    pthread_once(&g_once, init);
    if (!g_init) return -1;   /* MPV_ERROR_NOMEM is -1: the app reports it; the log says why */
    r = g_init(h);
    if (r < 0 || g_off || !g_cmd) return r;
    for (i = 0; i < g_nscripts; i++) {
        const char *args[] = { "load-script", g_scripts[i], NULL };
        const int s = g_cmd(h, args);
        plog("load-script %s -> %d", g_scripts[i], s);
    }
    if (g_vf[0]) {
        if (!vf_has_ours(h)) vf_add(h, "after mpv_initialize");
        if (g_client && g_observe && g_wait && g_destroy) {
            void *c = g_client(h, "pushmpc");
            pthread_t t;
            if (c && g_observe(c, 1, "vf", 0) >= 0 && pthread_create(&t, NULL, vf_watch, c) == 0) pthread_detach(t);
            else if (c) g_destroy(c);
        }
    }
    return r;
}

/* the installer finds its proxy by this name (grep -a), the self-test prints it */
__attribute__((visibility("default"))) const char *pushmpc_mpv_proxy_status(void)
{
    pthread_once(&g_once, init);
    return g_status;
}
