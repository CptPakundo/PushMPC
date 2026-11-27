/* pm_content.c: loads PushMPC's content library at run time (see pm_content.h). Loaded once per process and kept (as
 * pm_pushcore.dll is): its threads belong to the contexts, which end in pm_content_close. */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1   /* Dl_info / dladdr in glibc's <dlfcn.h>; must come before any system header */
#endif
#include "pm_content.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t (*pmc_abi_fn)(void);
typedef pmc_ctx *(*pmc_create_fn)(const pmc_config *);
typedef void (*pmc_destroy_fn)(pmc_ctx *);
typedef int (*pmc_push_fn)(pmc_ctx *, const void *, ptrdiff_t, const void *, const void *, ptrdiff_t, int, int, int, int, double, int);
typedef int (*pmc_state_fn)(pmc_ctx *, pmc_state *);
typedef void (*pmc_evidence_fn)(pmc_ctx *, double, double, double);
typedef const char *(*pmc_name_fn)(int);

static struct {
    int tried, ok;
    pmc_abi_fn abi;
    pmc_create_fn create;
    pmc_destroy_fn destroy;
    pmc_push_fn push;
    pmc_state_fn state;
    pmc_evidence_fn evidence;
    pmc_name_fn name;
    char why[300];
    char where[600];
} L;

struct pm_content { pmc_ctx *ctx; };

#ifdef _WIN32
#include <windows.h>
static SRWLOCK lockL = SRWLOCK_INIT;
static void Lock(void) { AcquireSRWLockExclusive(&lockL); }
static void Unlock(void) { ReleaseSRWLockExclusive(&lockL); }
#define PM_CONTENT_FILE L"pushmpc_content.dll"
#define PM_CONTENT_NAME "pushmpc_content.dll"
static HMODULE TryLoad(const wchar_t *dir)
{
    wchar_t path[MAX_PATH * 2];
    if (!dir || !*dir || wcslen(dir) + wcslen(PM_CONTENT_FILE) + 2 > MAX_PATH * 2) return NULL;
    wcscpy(path, dir);
    const size_t n = wcslen(path);
    if (path[n - 1] != L'\\' && path[n - 1] != L'/') wcscat(path, L"\\");
    wcscat(path, PM_CONTENT_FILE);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return NULL;
    HMODULE h = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (h) WideCharToMultiByte(CP_UTF8, 0, path, -1, L.where, (int)sizeof(L.where), NULL, NULL);
    return h;
}
static void *Sym(HMODULE h, const char *n) { return (void *)GetProcAddress(h, n); }
static HMODULE Find(void)
{
    /* the module's own folder (where pushmpc-vlc.ps1 installs the module, pm_pushcore.dll and this library) */
    HMODULE self = NULL, h = NULL;
    wchar_t dir[MAX_PATH * 2];
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)(void *)&Find, &self) && GetModuleFileNameW(self, dir, MAX_PATH * 2)) {
        wchar_t *slash = wcsrchr(dir, L'\\');
        if (slash) { slash[1] = 0; h = TryLoad(dir); }
    }
    /* PushMPC's VLC folder (a module copied into VLC's own plugins folder by hand) */
    if (!h) {
        wchar_t la[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
        if (n > 0 && n < MAX_PATH - 20) { wcscpy(dir, la); wcscat(dir, L"\\PushMPC\\vlc"); h = TryLoad(dir); }
    }
    return h;
}
#else
#include <dlfcn.h>
#include <pthread.h>
static pthread_mutex_t lockL = PTHREAD_MUTEX_INITIALIZER;
static void Lock(void) { pthread_mutex_lock(&lockL); }
static void Unlock(void) { pthread_mutex_unlock(&lockL); }
#ifdef __APPLE__
#define PM_CONTENT_FILE "libpushmpc_content.dylib"
#else
#define PM_CONTENT_FILE "libpushmpc_content.so"
#endif
#define PM_CONTENT_NAME PM_CONTENT_FILE
static void *TryLoad(const char *dir)
{
    char path[1024];
    if (!dir || !*dir) return NULL;
    const size_t n = strlen(dir);
    if (snprintf(path, sizeof(path), "%s%s%s", dir, dir[n - 1] == '/' ? "" : "/", PM_CONTENT_FILE) >= (int)sizeof(path)) return NULL;
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (h) snprintf(L.where, sizeof(L.where), "%s", path);
    else if (!L.why[0]) { const char *e = dlerror(); if (e && !strstr(e, "No such file")) snprintf(L.why, sizeof(L.why), "%s", e); }
    return h;
}
static void *Sym(void *h, const char *n) { return dlsym(h, n); }
static void *Find(void)
{
    void *h = NULL;
    Dl_info di;
    if (dladdr((void *)&Find, &di) && di.dli_fname) {   /* the module's own folder */
        char dir[1024];
        snprintf(dir, sizeof(dir), "%s", di.dli_fname);
        char *slash = strrchr(dir, '/');
        if (slash) { slash[1] = 0; h = TryLoad(dir); }
    }
    if (!h) {   /* PushMPC's VLC folder */
        char dir[1024];
        const char *home = getenv("HOME");
#ifdef __APPLE__
        if (home && *home) { snprintf(dir, sizeof(dir), "%s/Library/Application Support/PushMPC/vlc", home); h = TryLoad(dir); }
#else
        const char *xdg = getenv("XDG_DATA_HOME");
        if (xdg && *xdg) { snprintf(dir, sizeof(dir), "%s/pushmpc/vlc", xdg); h = TryLoad(dir); }
        if (!h && home && *home) { snprintf(dir, sizeof(dir), "%s/.local/share/pushmpc/vlc", home); h = TryLoad(dir); }
#endif
    }
    return h;
}
#endif

static int Load(void)
{
    Lock();
    if (!L.tried) {
        L.tried = 1;
#ifdef _WIN32
        HMODULE h = Find();
#else
        void *h = Find();
#endif
        if (!h) {
            if (!L.why[0]) snprintf(L.why, sizeof(L.why), "%s is not next to the module", PM_CONTENT_NAME);
        } else {
            L.abi = (pmc_abi_fn)Sym(h, "pmc_abi");
            L.create = (pmc_create_fn)Sym(h, "pmc_create");
            L.destroy = (pmc_destroy_fn)Sym(h, "pmc_destroy");
            L.push = (pmc_push_fn)Sym(h, "pmc_push_yuv420");
            L.state = (pmc_state_fn)Sym(h, "pmc_get_state");
            L.evidence = (pmc_evidence_fn)Sym(h, "pmc_set_image_evidence");
            L.name = (pmc_name_fn)Sym(h, "pmc_class_name");
            if (!L.abi || !L.create || !L.destroy || !L.push || !L.state || !L.evidence || !L.name)
                snprintf(L.why, sizeof(L.why), "%s is incomplete", L.where);
            else if (L.abi() != PMC_ABI)
                snprintf(L.why, sizeof(L.why), "%s has interface %u, the module needs %u (they come from different releases)", L.where, L.abi(), PMC_ABI);
            else L.ok = 1;
            /* not unloaded on a failure either: a library that loaded once stays mapped for the process (as the module's
             * other sidecars) */
        }
    }
    const int ok = L.ok;
    Unlock();
    return ok;
}

pm_content *pm_content_open(pmc_log_fn log, void *log_user, uint32_t flags, char *why, size_t whysize, char *where, size_t wheresize)
{
    if (why && whysize) why[0] = 0;
    if (where && wheresize) where[0] = 0;
    if (!Load()) { if (why && whysize) snprintf(why, whysize, "%s", L.why); return NULL; }
    if (where && wheresize) snprintf(where, wheresize, "%s", L.where);
    pmc_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.struct_size = sizeof(cfg); cfg.abi = PMC_ABI; cfg.log = log; cfg.log_user = log_user; cfg.flags = flags;
    pm_content *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->ctx = L.create(&cfg);
    if (!c->ctx) { if (why && whysize) snprintf(why, whysize, "pmc_create failed"); free(c); return NULL; }
    return c;
}

void pm_content_close(pm_content *c)
{
    if (!c) return;
    L.destroy(c->ctx);
    free(c);
}

int pm_content_push(pm_content *c, const void *y, ptrdiff_t ystride, const void *u, const void *v, ptrdiff_t cstride,
                    int w, int h, int bits, int limited, double pts, int discontinuity)
{
    return c ? L.push(c->ctx, y, ystride, u, v, cstride, w, h, bits, limited, pts, discontinuity) : -1;
}

int pm_content_state(pm_content *c, pmc_state *out)
{
    if (!c || !out) return -1;
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    return L.state(c->ctx, out);
}

void pm_content_evidence(pm_content *c, double pdrawn, double pts, double pcg) { if (c) L.evidence(c->ctx, pdrawn, pts, pcg); }

const char *pm_content_class_name(pm_content *c, int cls) { return c && L.name ? L.name(cls) : "measuring"; }

void pm_content_policy_text(uint32_t policy, char *out, size_t n)
{
    static const struct { uint32_t bit; const char *name; } k[] = {
        { PMC_POLICY_HOLD_REPAIR, "held-drawing repair" }, { PMC_POLICY_ANIME_UPSCALER, "anime upscaler" },
        { PMC_POLICY_ANIME_UPSCALER_DS, "anime upscaler _DS" }, { PMC_POLICY_SIGN_HOLD, "sign hold" },
        { PMC_POLICY_LINE_ART, "line art" }, { PMC_POLICY_ANIME_DEBAND, "anime deband" },
    };
    if (!out || !n) return;
    out[0] = 0;
    size_t len = 0;
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        if (!(policy & k[i].bit)) continue;
        const int w = snprintf(out + len, n - len, "%s%s", len ? ", " : "", k[i].name);
        if (w < 0 || (size_t)w >= n - len) break;
        len += (size_t)w;
    }
    if (!len) snprintf(out, n, "none");
}
