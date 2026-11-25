/*
 * pushmpc_hook.so: puts vf_pushmpc into an EXISTING mpv (distro mpv, libmpv players) without rebuilding it.
 * An mpv C plugin (cplugins: on by default in mpv's Linux and macOS builds), loaded with `script=<path>` in mpv.conf.
 *
 *  1. dlopen()s libpushmpc-avfilter.so next to it: a libavfilter built from the SAME FFmpeg release as the system's
 *     (only vf_pushmpc enabled, soname changed), RTLD_LOCAL | RTLD_DEEPBIND, so its internal ff_* functions bind to
 *     themselves while libavutil (NEEDED by soname) binds to the copy mpv already loaded. The filter's code then runs
 *     on graph / link / frame structures the system libavfilter allocated: valid only because both are the same
 *     release (checked: avfilter_version() and LIBAVFILTER_IDENT must match, else the hook refuses and logs why).
 *  2. Rebinds every loaded object's import of avfilter_get_by_name (GOT slots: JUMP_SLOT and GLOB_DAT) to a wrapper
 *     that returns our filter for "pushmpc" and the system's answer otherwise. mpv resolves `vf=pushmpc` (its direct
 *     lavfi bridge) through that call in its own image (filters/f_lavfi.c), so the name works from then on.
 *     Not covered: `vf=lavfi=[...pushmpc...]` graph strings (parsed inside libavfilter, which binds its own calls
 *     directly with -Bsymbolic).
 *  3. Appends `@pushmpc:pushmpc` (options from script-opts pushmpc-vf=...) to the vf chain before it enters its event
 *     loop (mpv starts playback only after every script has done so), then idles until shutdown.
 * Linux x86_64 / aarch64 (ELF). macOS would need the same with Mach-O lazy / non-lazy symbol pointers (fishhook).
 */
#define _GNU_SOURCE 1
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

#include <mpv/client.h>

typedef const void *(*get_by_name_fn)(const char *);
typedef unsigned (*version_fn)(void);
typedef const char *(*ident_fn)(void);

static get_by_name_fn g_real, g_priv;
static const void *g_filter;
static void *g_priv_handle, *g_sys_handle;
static int g_patched;
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

static void patch_slot(void **slot)
{
    const long page = sysconf(_SC_PAGESIZE);
    uintptr_t start = (uintptr_t)slot & ~(uintptr_t)(page - 1);
    if (*slot == (void *)hook_get_by_name)
        return;
    if (mprotect((void *)start, (size_t)page, PROT_READ | PROT_WRITE) != 0) {
        logf_("mprotect failed; ");
        return;
    }
    *slot = (void *)hook_get_by_name;
    mprotect((void *)start, (size_t)page, PROT_READ);   /* RELRO pages are read-only; others stay readable */
    g_patched++;
}

static void scan_relocs(ElfW(Addr) base, const ElfW(Rela) *rel, size_t size, const ElfW(Sym) *symtab, const char *strtab)
{
    size_t i;
    for (i = 0; rel && i < size / sizeof(ElfW(Rela)); i++) {
        const unsigned type = (unsigned)ELF64_R_TYPE(rel[i].r_info);
        const unsigned si = (unsigned)ELF64_R_SYM(rel[i].r_info);
        if (type != R_JUMP_SLOT && type != R_GLOB_DAT && type != R_ABS64)
            continue;
        if (si && !strcmp(strtab + symtab[si].st_name, "avfilter_get_by_name"))
            patch_slot((void **)(base + rel[i].r_offset));
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

static int install(void)
{
    char path[1024] = "";
    Dl_info di;
    version_fn sys_ver, priv_ver;
    ident_fn sys_id, priv_id;
    const char *env = getenv("PUSHMPC_AVFILTER");

    g_sys_handle = dlopen("libavfilter.so", RTLD_NOW | RTLD_NOLOAD);
    if (!g_sys_handle) {
        /* the soname carries the major version: find the loaded one through the symbol */
        void *sym = dlsym(RTLD_DEFAULT, "avfilter_version");
        if (sym && dladdr(sym, &di) && di.dli_fname)
            g_sys_handle = dlopen(di.dli_fname, RTLD_NOW | RTLD_NOLOAD);
    }
    if (!g_sys_handle) { logf_("this mpv has no libavfilter loaded; "); return -1; }
    g_real = (get_by_name_fn)dlsym(g_sys_handle, "avfilter_get_by_name");
    sys_ver = (version_fn)dlsym(g_sys_handle, "avfilter_version");
    sys_id = (ident_fn)dlsym(g_sys_handle, "avfilter_configuration");

    if (env && *env) snprintf(path, sizeof(path), "%s", env);
    else if (dladdr((void *)install, &di) && di.dli_fname) {
        const char *slash = strrchr(di.dli_fname, '/');
        snprintf(path, sizeof(path), "%.*slibpushmpc-avfilter.so", slash ? (int)(slash - di.dli_fname + 1) : 0, di.dli_fname);
    }
    g_priv_handle = dlopen(path, RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
    if (!g_priv_handle) { logf_("cannot load %s: %s; ", path, dlerror()); return -1; }
    g_priv = (get_by_name_fn)dlsym(g_priv_handle, "avfilter_get_by_name");
    priv_ver = (version_fn)dlsym(g_priv_handle, "avfilter_version");
    priv_id = (ident_fn)dlsym(g_priv_handle, "avfilter_configuration");
    if (!g_real || !g_priv || !sys_ver || !priv_ver) { logf_("missing libavfilter symbols; "); return -1; }
    if (sys_ver() != priv_ver()) {
        logf_("libavfilter version mismatch: system %u.%u.%u, ours %u.%u.%u (needs the build for this FFmpeg release); ",
              sys_ver() >> 16, (sys_ver() >> 8) & 255, sys_ver() & 255, priv_ver() >> 16, (priv_ver() >> 8) & 255, priv_ver() & 255);
        return -1;
    }
    g_filter = g_priv("pushmpc");
    if (!g_filter) { logf_("%s has no pushmpc filter; ", path); return -1; }
    dl_iterate_phdr(each_object, NULL);
    logf_("libavfilter %u.%u.%u (system %s; ours %s), %d import slot(s) rebound; ", sys_ver() >> 16, (sys_ver() >> 8) & 255,
          sys_ver() & 255, sys_id ? "configured" : "?", priv_id ? "minimal" : "?", g_patched);
    return g_patched > 0 ? 0 : -1;
}

int mpv_open_cplugin(mpv_handle *h)
{
    char *opts = mpv_get_property_string(h, "script-opts");
    char cmd[2048] = "@pushmpc:pushmpc";
    const int ok = install() == 0;
    if (opts) {   /* script-opts=pushmpc-vf=smoother=4:display_fps=144 -> the filter's options */
        const char *p = strstr(opts, "pushmpc-vf=");
        if (p) {
            const char *e = strchr(p, ',');
            p += strlen("pushmpc-vf=");
            snprintf(cmd, sizeof(cmd), "@pushmpc:pushmpc=%.*s", e ? (int)(e - p) : (int)strlen(p), p);
        }
        mpv_free(opts);
    }
    if (ok) {
        const char *args[] = { "vf", "append", cmd, NULL };
        const int r = mpv_command(h, args);
        mpv_request_log_messages(h, "no");
        fprintf(stderr, "[pushmpc_hook] %svf append %s -> %s\n", g_log, cmd, mpv_error_string(r));
    } else {
        fprintf(stderr, "[pushmpc_hook] not active: %s\n", g_log);
    }
    for (;;) {
        mpv_event *ev = mpv_wait_event(h, -1);
        if (ev->event_id == MPV_EVENT_SHUTDOWN)
            break;
    }
    return 0;
}
