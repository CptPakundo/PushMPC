/*
 * pushmpc_hook.so: puts vf_pushmpc into an EXISTING mpv (distro mpv, libmpv players) without rebuilding it.
 * An mpv C plugin (cplugins: on by default in mpv's Linux and macOS builds), loaded with `script=<path>` in mpv.conf.
 *
 *  1. Reads the system's avfilter_version() and dlopen()s libpushmpc-avfilter-<maj>.<min>.<micro>.so next to it: the
 *     libavfilter of that FFmpeg branch with only vf_pushmpc and the parse2 glue (CI builds one per branch, 6.0 .. 9.1,
 *     x86_64 and aarch64), RTLD_LOCAL | RTLD_DEEPBIND, so its internal ff_* functions bind to themselves while
 *     libavutil (NEEDED by soname) binds to the copy mpv already loaded. The filter's code then runs on graph / link /
 *     frame structures the system libavfilter allocated: valid because both are the same branch (the version must
 *     match exactly; the system's libavutil must be the same major and at least the minor the copy was built with).
 *  2. Rebinds every loaded object's imports (GOT slots: JUMP_SLOT, GLOB_DAT, ABS64) of
 *       avfilter_get_by_name   -> our filter for "pushmpc" (mpv's direct `vf=pushmpc`, filters/f_lavfi.c);
 *       avfilter_graph_parse2  -> for strings naming pushmpc, the system's segment parser with our filter pre-created,
 *                                 then the system's apply (graph strings: `vf=lavfi=[...pushmpc...]`).
 *  3. Appends `@pushmpc:pushmpc` (options from script-opts pushmpc-vf=...; pushmpc-append=no skips it) to the vf
 *     chain before it enters its event loop (mpv starts playback only after every script has done so).
 * Linux x86_64 / aarch64 (ELF). macOS would need the same with Mach-O lazy / non-lazy symbol pointers (fishhook).
 */
#define _GNU_SOURCE 1
#include <dirent.h>
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef PUSHMPC_PRELOAD
#include <mpv/client.h>
#endif

typedef const void *(*get_by_name_fn)(const char *);
typedef unsigned (*version_fn)(void);
typedef int (*parse2_fn)(void *graph, const char *filters, void **inputs, void **outputs);

/* must match pushmpc_hook_glue.c */
typedef struct PushmpcSysApi {
    void *segment_parse, *segment_apply, *segment_free, *alloc_filter, *filter_free;
} PushmpcSysApi;
typedef int (*glue_parse2_fn)(void *graph, const char *filters, void **inputs, void **outputs, const PushmpcSysApi *api,
                              const void *pushmpc);

static get_by_name_fn g_real, g_priv;
static parse2_fn g_real_parse2;
static glue_parse2_fn g_glue_parse2;
static PushmpcSysApi g_api;
static const void *g_filter;
static void *g_priv_handle, *g_sys_handle;
static int g_patched_name, g_patched_parse;
static char g_log[2048];

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
    va_list ap;
    size_t n = strlen(g_log);
    va_start(ap, fmt);
    vsnprintf(g_log + n, sizeof(g_log) - n, fmt, ap);
    va_end(ap);
}

static const void *hook_get_by_name(const char *name)
{
    if (name && g_filter && !strcmp(name, "pushmpc"))
        return g_filter;
    return g_real ? g_real(name) : NULL;
}

static int hook_parse2(void *graph, const char *filters, void **inputs, void **outputs)
{
    if (filters && g_glue_parse2 && g_filter && strstr(filters, "pushmpc"))
        return g_glue_parse2(graph, filters, inputs, outputs, &g_api, g_filter);
    return g_real_parse2(graph, filters, inputs, outputs);
}

#if defined(__x86_64__)
#define R_JUMP_SLOT R_X86_64_JUMP_SLOT
#define R_GLOB_DAT  R_X86_64_GLOB_DAT
#define R_ABS64     R_X86_64_64
#elif defined(__aarch64__)
#define R_JUMP_SLOT R_AARCH64_JUMP_SLOT
#define R_GLOB_DAT  R_AARCH64_GLOB_DAT
#define R_ABS64     R_AARCH64_ABS64
#else
#error "pushmpc_hook: x86_64 and aarch64 only"
#endif

static int patch_slot(void **slot, void *to)
{
    const long page = sysconf(_SC_PAGESIZE);
    uintptr_t start = (uintptr_t)slot & ~(uintptr_t)(page - 1);
    if (*slot == to)
        return 0;
    if (mprotect((void *)start, (size_t)page, PROT_READ | PROT_WRITE) != 0) {
        logf_("mprotect failed; ");
        return 0;
    }
    *slot = to;
    mprotect((void *)start, (size_t)page, PROT_READ);   /* RELRO pages are read-only; others stay readable */
    return 1;
}

static void scan_relocs(ElfW(Addr) base, const ElfW(Rela) *rel, size_t size, const ElfW(Sym) *symtab, const char *strtab)
{
    size_t i;
    for (i = 0; rel && i < size / sizeof(ElfW(Rela)); i++) {
        const unsigned type = (unsigned)ELF64_R_TYPE(rel[i].r_info);
        const unsigned si = (unsigned)ELF64_R_SYM(rel[i].r_info);
        const char *sym;
        if (!si || (type != R_JUMP_SLOT && type != R_GLOB_DAT && type != R_ABS64))
            continue;
        sym = strtab + symtab[si].st_name;
        if (!strcmp(sym, "avfilter_get_by_name"))
            g_patched_name += patch_slot((void **)(base + rel[i].r_offset), (void *)hook_get_by_name);
        else if (g_glue_parse2 && !strcmp(sym, "avfilter_graph_parse2"))
            g_patched_parse += patch_slot((void **)(base + rel[i].r_offset), (void *)hook_parse2);
    }
}

static int each_object(struct dl_phdr_info *info, size_t size, void *data)
{
    const char *n = info->dlpi_name ? info->dlpi_name : "";
    const ElfW(Dyn) *dyn = NULL;
    const ElfW(Rela) *jmprel = NULL, *rela = NULL;
    size_t jmpsz = 0, relasz = 0;
    const ElfW(Sym) *symtab = NULL;
    const char *strtab = NULL;
    int i;
    (void)size; (void)data;
    /* never inside libavfilter itself (system or ours) or the hook */
    if (strstr(n, "libavfilter.so") || strstr(n, "libpushmpc-avfilter") || strstr(n, "pushmpc_hook"))
        return 0;
    for (i = 0; i < info->dlpi_phnum; i++)
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC)
            dyn = (const ElfW(Dyn) *)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
    if (!dyn)
        return 0;
    for (; dyn->d_tag != DT_NULL; dyn++) {
        switch (dyn->d_tag) {
        case DT_JMPREL:   jmprel = (const ElfW(Rela) *)dyn->d_un.d_ptr; break;
        case DT_PLTRELSZ: jmpsz = dyn->d_un.d_val; break;
        case DT_RELA:     rela = (const ElfW(Rela) *)dyn->d_un.d_ptr; break;
        case DT_RELASZ:   relasz = dyn->d_un.d_val; break;
        case DT_SYMTAB:   symtab = (const ElfW(Sym) *)dyn->d_un.d_ptr; break;
        case DT_STRTAB:   strtab = (const char *)dyn->d_un.d_ptr; break;
        }
    }
    if (!symtab || !strtab)
        return 0;
    /* glibc relocates these d_ptr entries in place for most objects; the vDSO is the exception (no imports anyway) */
    scan_relocs(info->dlpi_addr, jmprel, jmpsz, symtab, strtab);
    scan_relocs(info->dlpi_addr, rela, relasz, symtab, strtab);
    return 0;
}

/* the shipped builds next to the hook, for the log when none matches */
static void list_builds(const char *dir)
{
    DIR *d = opendir(dir[0] ? dir : ".");
    struct dirent *e;
    int n = 0;
    if (!d)
        return;
    logf_("shipped builds:");
    while ((e = readdir(d)))
        if (!strncmp(e->d_name, "libpushmpc-avfilter-", 20)) {
            logf_(" %s", e->d_name + 20);
            n++;
        }
    closedir(d);
    logf_(n ? "; " : " none; ");
}

/* a loaded library by its file-name stem ("libavfilter.so"), also when it sits in a private (RTLD_LOCAL) namespace:
 * python-mpv in the shims and Emby Theater's plugin dlopen libmpv locally, so dlsym(RTLD_DEFAULT) cannot see it */
struct pm_find { const char *stem; char path[1024]; };
static int find_one(struct dl_phdr_info *info, size_t size, void *data)
{
    struct pm_find *f = data;
    const char *n = info->dlpi_name ? info->dlpi_name : "";
    (void)size;
    if (!f->path[0] && strstr(n, f->stem) && !strstr(n, "libpushmpc-avfilter"))
        snprintf(f->path, sizeof(f->path), "%s", n);
    return 0;
}
static void *loaded_lib(const char *stem, const char *sym)
{
    struct pm_find f = { stem, "" };
    Dl_info di;
    void *s = dlsym(RTLD_DEFAULT, sym);
    if (s && dladdr(s, &di) && di.dli_fname && strstr(di.dli_fname, stem))
        return dlopen(di.dli_fname, RTLD_NOW | RTLD_NOLOAD);
    dl_iterate_phdr(find_one, &f);
    return f.path[0] ? dlopen(f.path, RTLD_NOW | RTLD_NOLOAD) : NULL;
}

static int install(void)
{
    char dir[1024] = "", path[1200];
    Dl_info di;
    version_fn sys_ver, priv_ver, sys_avutil, priv_avutil;
    const char *env = getenv("PUSHMPC_AVFILTER");
    unsigned v;

    g_sys_handle = dlopen("libavfilter.so", RTLD_NOW | RTLD_NOLOAD);
    if (!g_sys_handle)
        g_sys_handle = loaded_lib("libavfilter.so", "avfilter_version");
    if (!g_sys_handle) { logf_("this mpv has no libavfilter loaded; "); return -1; }
    g_real = (get_by_name_fn)dlsym(g_sys_handle, "avfilter_get_by_name");
    g_real_parse2 = (parse2_fn)dlsym(g_sys_handle, "avfilter_graph_parse2");
    sys_ver = (version_fn)dlsym(g_sys_handle, "avfilter_version");
    {
        void *u = loaded_lib("libavutil.so", "avutil_version");
        sys_avutil = (version_fn)(u ? dlsym(u, "avutil_version") : dlsym(RTLD_DEFAULT, "avutil_version"));
    }
    if (!g_real || !sys_ver) { logf_("missing libavfilter symbols; "); return -1; }
    v = sys_ver();

    if (dladdr((void *)install, &di) && di.dli_fname) {
        const char *slash = strrchr(di.dli_fname, '/');
        if (slash)
            snprintf(dir, sizeof(dir), "%.*s", (int)(slash - di.dli_fname + 1), di.dli_fname);
    }
    if (env && *env)
        snprintf(path, sizeof(path), "%s", env);
    else
        snprintf(path, sizeof(path), "%slibpushmpc-avfilter-%u.%u.%u.so", dir, v >> 16, (v >> 8) & 255, v & 255);
    g_priv_handle = dlopen(path, RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
    if (!g_priv_handle) {
        logf_("no build for this mpv's libavfilter %u.%u.%u (%s); ", v >> 16, (v >> 8) & 255, v & 255, dlerror());
        list_builds(dir);
        return -1;
    }
    g_priv = (get_by_name_fn)dlsym(g_priv_handle, "avfilter_get_by_name");
    priv_ver = (version_fn)dlsym(g_priv_handle, "avfilter_version");
    priv_avutil = (version_fn)dlsym(g_priv_handle, "avfilter_pushmpc_avutil_built");
    if (!g_priv || !priv_ver) { logf_("%s lacks libavfilter symbols; ", path); return -1; }
    if (priv_ver() != v) {
        logf_("libavfilter version mismatch: system %u.%u.%u, %s is %u.%u.%u; ", v >> 16, (v >> 8) & 255, v & 255, path,
              priv_ver() >> 16, (priv_ver() >> 8) & 255, priv_ver() & 255);
        return -1;
    }
    if (sys_avutil && priv_avutil) {
        const unsigned s = sys_avutil(), b = priv_avutil();
        if ((s >> 16) != (b >> 16) || ((s >> 8) & 255) < ((b >> 8) & 255)) {
            logf_("libavutil %u.%u is older than the %u.%u the build expects; ", s >> 16, (s >> 8) & 255, b >> 16, (b >> 8) & 255);
            return -1;
        }
    }
    g_filter = g_priv("pushmpc");
    if (!g_filter) { logf_("%s has no pushmpc filter; ", path); return -1; }
    /* graph strings: the system's public segment API, driven by the glue */
    g_glue_parse2 = (glue_parse2_fn)dlsym(g_priv_handle, "avfilter_pushmpc_parse2");
    g_api.segment_parse = dlsym(g_sys_handle, "avfilter_graph_segment_parse");
    g_api.segment_apply = dlsym(g_sys_handle, "avfilter_graph_segment_apply");
    g_api.segment_free = dlsym(g_sys_handle, "avfilter_graph_segment_free");
    g_api.alloc_filter = dlsym(g_sys_handle, "avfilter_graph_alloc_filter");
    g_api.filter_free = dlsym(g_sys_handle, "avfilter_free");
    if ((v >> 16) < 9)   /* FFmpeg 5.x: no segment API, graph strings stay unhooked */
        g_glue_parse2 = NULL;
    if (!g_real_parse2 || !g_api.segment_parse || !g_api.segment_apply || !g_api.segment_free || !g_api.alloc_filter ||
        !g_api.filter_free)
        g_glue_parse2 = NULL;
#ifdef PUSHMPC_PRELOAD
    g_glue_parse2 = NULL;   /* the preload interposes avfilter_get_by_name only */
    logf_("libavfilter %u.%u.%u, %s; avfilter_get_by_name interposed (LD_PRELOAD); ", v >> 16, (v >> 8) & 255, v & 255,
          strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
    return 0;
#else
    dl_iterate_phdr(each_object, NULL);
    logf_("libavfilter %u.%u.%u, %s; rebound avfilter_get_by_name x%d, avfilter_graph_parse2 x%d%s; ", v >> 16,
          (v >> 8) & 255, v & 255, strrchr(path, '/') ? strrchr(path, '/') + 1 : path, g_patched_name, g_patched_parse,
          g_glue_parse2 ? "" : " (graph strings: unavailable)");
    return g_patched_name > 0 ? 0 : -1;
#endif
}

#ifdef PUSHMPC_PRELOAD
#include <pthread.h>
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void preload_init(void)
{
    if (install() == 0) fprintf(stderr, "[pushmpc_preload] %s\n", g_log);
    else fprintf(stderr, "[pushmpc_preload] not active: %s\n", g_log);
    if (!g_real) g_real = (get_by_name_fn)dlsym(RTLD_NEXT, "avfilter_get_by_name");   /* never break the program */
}
/* LD_PRELOAD: this definition comes before libavfilter's in the global scope, so libmpv's calls land here */
__attribute__((visibility("default"))) const void *avfilter_get_by_name(const char *name)
{
    pthread_once(&g_once, preload_init);
    return hook_get_by_name(name);
}

/* PUSHMPC_PRELOAD_VF=auto|<vf_pushmpc options>: for libmpv programs that read no mpv.conf (Stremio, Jellyfin Desktop
 * Flatpaks): mpv_initialize is interposed too; after the real one the filter is appended ("vf append"), and a client of
 * the program's own mpv core puts it back whenever the program replaces the vf list (Stremio does on display changes).
 * Programs that dlopen libmpv privately and call it through dlsym on that handle bypass this (they use mpv.conf). */
typedef struct { int event_id; int error; uint64_t reply_userdata; void *data; } pm_event;
typedef int (*pm_init_fn)(void *);
typedef int (*pm_cmd_fn)(void *, const char **);
typedef void *(*pm_client_fn)(void *, const char *);
typedef int (*pm_observe_fn)(void *, uint64_t, const char *, int);
typedef pm_event *(*pm_wait_fn)(void *, double);
typedef char *(*pm_getstr_fn)(void *, const char *);
typedef void (*pm_free_fn)(void *);
static pm_cmd_fn pm_cmd;
static pm_getstr_fn pm_getstr;
static pm_free_fn pm_free, pm_destroy;
static pm_wait_fn pm_wait;
static char g_vfarg[1024];

static int vf_has_ours(void *h)
{
    char *v = pm_getstr(h, "vf");
    const int has = v && strstr(v, "@pushmpc") != NULL;
    if (v) pm_free(v);
    return has;
}
static void vf_add(void *h, const char *why)
{
    const char *args[] = { "vf", "append", g_vfarg, NULL };
    const int r = pm_cmd(h, args);
    fprintf(stderr, "[pushmpc_preload] vf append %s (%s) -> %d\n", g_vfarg, why, r);
}
static void *vf_watch(void *client)
{
    int again = 0;
    for (;;) {
        pm_event *ev = pm_wait(client, -1);
        if (ev->event_id == 1)   /* MPV_EVENT_SHUTDOWN */
            break;
        if (ev->event_id == 22 && !vf_has_ours(client) && again++ < 200)   /* MPV_EVENT_PROPERTY_CHANGE "vf" */
            vf_add(client, "the program replaced the filter list");
    }
    pm_destroy(client);
    return NULL;
}
__attribute__((visibility("default"))) int mpv_initialize(void *h)
{
    static pm_init_fn real;
    const char *opt = getenv("PUSHMPC_PRELOAD_VF");
    int r;
    if (!real)
        real = (pm_init_fn)dlsym(RTLD_NEXT, "mpv_initialize");
    if (!real)
        return -1;
    r = real(h);
    if (r < 0 || !opt || !*opt || !strcmp(opt, "no"))
        return r;
    pthread_once(&g_once, preload_init);
    pm_cmd = (pm_cmd_fn)dlsym(RTLD_NEXT, "mpv_command");
    pm_getstr = (pm_getstr_fn)dlsym(RTLD_NEXT, "mpv_get_property_string");
    pm_free = (pm_free_fn)dlsym(RTLD_NEXT, "mpv_free");
    pm_destroy = (pm_free_fn)dlsym(RTLD_NEXT, "mpv_destroy");
    pm_wait = (pm_wait_fn)dlsym(RTLD_NEXT, "mpv_wait_event");
    if (!g_filter || !pm_cmd || !pm_getstr || !pm_free || !pm_destroy || !pm_wait)
        return r;
    if (!strcmp(opt, "auto") || !strcmp(opt, "yes"))
        snprintf(g_vfarg, sizeof(g_vfarg), "@pushmpc:pushmpc");
    else
        snprintf(g_vfarg, sizeof(g_vfarg), "@pushmpc:pushmpc=%s", opt);
    if (!vf_has_ours(h))
        vf_add(h, "after mpv_initialize");
    {
        pm_client_fn mk = (pm_client_fn)dlsym(RTLD_NEXT, "mpv_create_client");
        pm_observe_fn obs = (pm_observe_fn)dlsym(RTLD_NEXT, "mpv_observe_property");
        void *c = mk ? mk(h, "pushmpc") : NULL;
        pthread_t t;
        if (c && obs && obs(c, 1, "vf", 0) >= 0 && pthread_create(&t, NULL, vf_watch, c) == 0)
            pthread_detach(t);
        else if (c)
            pm_destroy(c);
    }
    return r;
}
#else
/* The hook calls libmpv only through these pointers and references no mpv_* symbol, so it also loads where libmpv sits
 * in a private namespace (python-mpv in the shims, Emby Theater's plugin: dlopen RTLD_LOCAL). mpv 0.36+ fills them
 * right after loading a C plugin (the MPV_CPLUGIN_DYNAMIC_SYM protocol: it writes &mpv_xxx into pfn_mpv_xxx); older
 * mpv: the global namespace. */
#define PM_SYM(ret, name, args) __attribute__((visibility("default"))) ret (*pfn_##name) args = NULL;
PM_SYM(char *, mpv_get_property_string, (mpv_handle *, const char *))
PM_SYM(void, mpv_free, (void *))
PM_SYM(int, mpv_command, (mpv_handle *, const char **))
PM_SYM(const char *, mpv_error_string, (int))
PM_SYM(mpv_event *, mpv_wait_event, (mpv_handle *, double))
#define PM_RESOLVE(name) do { if (!pfn_##name) pfn_##name = (__typeof__(pfn_##name))dlsym(RTLD_DEFAULT, #name); } while (0)

int mpv_open_cplugin(mpv_handle *h)
{
    char *opts;
    PM_RESOLVE(mpv_get_property_string); PM_RESOLVE(mpv_free); PM_RESOLVE(mpv_command); PM_RESOLVE(mpv_error_string);
    PM_RESOLVE(mpv_wait_event);
    if (!pfn_mpv_get_property_string || !pfn_mpv_free || !pfn_mpv_command || !pfn_mpv_error_string || !pfn_mpv_wait_event) {
        fprintf(stderr, "[pushmpc_hook] not active: this libmpv gives C plugins no client API (mpv older than 0.36 in a private namespace)\n");
        return -1;
    }
    opts = pfn_mpv_get_property_string(h, "script-opts");
    char cmd[2048] = "@pushmpc:pushmpc";
    int append = 1;
    const int ok = install() == 0;
    if (opts) {   /* script-opts=pushmpc-vf=smoother=4:display_fps=144 -> the filter's options */
        const char *p = strstr(opts, "pushmpc-vf=");
        if (p) {
            const char *e = strchr(p, ',');
            p += strlen("pushmpc-vf=");
            snprintf(cmd, sizeof(cmd), "@pushmpc:pushmpc=%.*s", e ? (int)(e - p) : (int)strlen(p), p);
        }
        if (strstr(opts, "pushmpc-append=no"))
            append = 0;
        pfn_mpv_free(opts);
    }
    if (ok && append) {   /* another copy of the hook (a second mpv.conf block) may have added it already */
        char *vf = pfn_mpv_get_property_string(h, "vf");
        if (vf && strstr(vf, "@pushmpc"))
            append = 0;
        if (vf)
            pfn_mpv_free(vf);
    }
    if (ok && append) {
        const char *args[] = { "vf", "append", cmd, NULL };
        const int r = pfn_mpv_command(h, args);
        fprintf(stderr, "[pushmpc_hook] %svf append %s -> %s\n", g_log, cmd, pfn_mpv_error_string(r));
    } else if (ok) {
        fprintf(stderr, "[pushmpc_hook] %s(no vf append)\n", g_log);
    } else {
        fprintf(stderr, "[pushmpc_hook] not active: %s\n", g_log);
    }
    for (;;) {
        mpv_event *ev = pfn_mpv_wait_event(h, -1);
        if (ev->event_id == MPV_EVENT_SHUTDOWN)
            break;
    }
    return 0;
}
#endif
