/* engine_pushcore.c: loads pm_pushcore.dll (next to this plugin) at run time. Windows only; elsewhere every call says
 * "not available" and the module uses the portable engine. */
#include "engine_pushcore.h"
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>

static struct {
    int tried;
    HMODULE dll;
    pm_pc_open_fn open;
    pm_pc_push_fn push;
    pm_pc_poll_fn poll;
    pm_pc_flush_fn flush;
    pm_pc_close_fn close;
    pm_pc_describe_fn describe;
    /* optional (zero copy) */
    pm_pc_open_d3d11_fn open_d3d11;
    pm_pc_push_d3d11_fn push_d3d11;
    pm_pc_poll_d3d11_fn poll_d3d11;
    pm_pc_copy_d3d11_fn copy_d3d11;
    /* optional (ABI 3: settings update, what runs) */
    pm_pc_update_fn update;
    pm_pc_output_fn output;
    uint32_t abi;
    /* optional (sharing the core with the Direct3D 11 output) */
    pm_pc_decide_fn decide;
} B;
static SRWLOCK lockB = SRWLOCK_INIT;

static int LoadBridge(char *err, size_t n)
{
    AcquireSRWLockExclusive(&lockB);
    if (!B.tried) {
        B.tried = 1;
        HMODULE self = NULL;
        wchar_t path[MAX_PATH * 2];
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)(void *)&LoadBridge, &self) &&
            GetModuleFileNameW(self, path, MAX_PATH * 2)) {
            wchar_t *slash = wcsrchr(path, L'\\');
            if (slash) {
                wcscpy(slash + 1, L"pm_pushcore.dll");
                B.dll = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
            }
        }
        if (B.dll) {
            B.open = (pm_pc_open_fn)(void *)GetProcAddress(B.dll, "pm_pc_open");
            B.push = (pm_pc_push_fn)(void *)GetProcAddress(B.dll, "pm_pc_push");
            B.poll = (pm_pc_poll_fn)(void *)GetProcAddress(B.dll, "pm_pc_poll");
            B.flush = (pm_pc_flush_fn)(void *)GetProcAddress(B.dll, "pm_pc_flush");
            B.close = (pm_pc_close_fn)(void *)GetProcAddress(B.dll, "pm_pc_close");
            B.describe = (pm_pc_describe_fn)(void *)GetProcAddress(B.dll, "pm_pc_describe");
            if (!B.open || !B.push || !B.poll || !B.flush || !B.close || !B.describe) {
                FreeLibrary(B.dll);
                B.dll = NULL;
            } else {
                B.open_d3d11 = (pm_pc_open_d3d11_fn)(void *)GetProcAddress(B.dll, "pm_pc_open_d3d11");
                B.push_d3d11 = (pm_pc_push_d3d11_fn)(void *)GetProcAddress(B.dll, "pm_pc_push_d3d11");
                B.poll_d3d11 = (pm_pc_poll_d3d11_fn)(void *)GetProcAddress(B.dll, "pm_pc_poll_d3d11");
                B.copy_d3d11 = (pm_pc_copy_d3d11_fn)(void *)GetProcAddress(B.dll, "pm_pc_copy_d3d11");
                if (!B.open_d3d11 || !B.push_d3d11 || !B.poll_d3d11 || !B.copy_d3d11)
                    B.open_d3d11 = NULL;   /* an older bridge: the copy path only */
                pm_pc_abi_fn abi = (pm_pc_abi_fn)(void *)GetProcAddress(B.dll, "pm_pc_abi");
                B.update = (pm_pc_update_fn)(void *)GetProcAddress(B.dll, "pm_pc_update");
                B.output = (pm_pc_output_fn)(void *)GetProcAddress(B.dll, "pm_pc_output");
                B.abi = abi && B.update && B.output ? abi() : 2u;   /* an older bridge: ABI 2 (no update) */
                if (B.abi > PM_PC_ABI) B.abi = PM_PC_ABI;
                B.decide = (pm_pc_decide_fn)(void *)GetProcAddress(B.dll, "pm_pc_decide");
            }
        }
    }
    const int ok = B.dll != NULL;
    ReleaseSRWLockExclusive(&lockB);
    if (!ok && err && n) snprintf(err, n, "pm_pushcore.dll is not next to the plugin (or incomplete)");
    return ok;
}

/* an older bridge takes the ABI 2 part of the configuration (the fields it knows come first) */
static const pm_pc_cfg *ForBridge(const pm_pc_cfg *cfg, pm_pc_cfg *tmp)
{
    if (B.abi >= 3 || !cfg) return cfg;
    *tmp = *cfg;
    tmp->abi = 2;
    return tmp;
}
pm_pc *pmx_open(const pm_pc_cfg *cfg, int *k, char *err, size_t n)
{
    if (!LoadBridge(err, n)) return NULL;
    pm_pc_cfg tmp;
    return B.open(ForBridge(cfg, &tmp), k, err, n);
}
int pmx_push(pm_pc *pc, const pm_pc_planes *src, int64_t pts, int disc, char *err, size_t n) { return B.push(pc, src, pts, disc, err, n); }
int pmx_poll(pm_pc *pc, uint32_t ms, pm_pc_dest_fn d, pm_pc_done_fn f, void *u, char *err, size_t n) { return B.poll(pc, ms, d, f, u, err, n); }
int pmx_flush(pm_pc *pc) { return B.flush(pc); }
void pmx_close(pm_pc *pc) { if (pc) B.close(pc); }
const char *pmx_describe(pm_pc *pc) { return B.describe ? B.describe(pc) : ""; }
int pmx_has_d3d11(void) { char e[8]; return LoadBridge(e, sizeof(e)) && B.open_d3d11 != NULL; }
pm_pc *pmx_open_d3d11(const pm_pc_cfg *cfg, void *dev, void *mutex, int *k, char *err, size_t n)
{
    if (k) *k = 1;
    if (!pmx_has_d3d11()) { if (err && n) snprintf(err, n, "pm_pushcore.dll has no zero-copy path"); return NULL; }
    pm_pc_cfg tmp;
    return B.open_d3d11(ForBridge(cfg, &tmp), dev, mutex, k, err, n);
}
int pmx_push_d3d11(pm_pc *pc, void *t, unsigned s, int64_t pts, int disc, char *err, size_t n) { return B.push_d3d11(pc, t, s, pts, disc, err, n); }
int pmx_poll_d3d11(pm_pc *pc, uint32_t ms, pm_pc_dest_tex_fn d, pm_pc_done_fn f, void *u, char *err, size_t n) { return B.poll_d3d11(pc, ms, d, f, u, err, n); }
int pmx_copy_d3d11(pm_pc *pc, void *src, unsigned ss, void *dst, unsigned ds) { return B.copy_d3d11(pc, src, ss, dst, ds); }
unsigned pmx_abi(void) { char e[8]; return LoadBridge(e, sizeof(e)) ? B.abi : 0u; }
int pmx_update(pm_pc *pc, const pm_pc_cfg *cfg, char *err, size_t n)
{
    if (B.abi < 3 || !B.update) { if (err && n) snprintf(err, n, "pm_pushcore.dll is older than ABI 3 (no settings update)"); return -1; }
    return B.update(pc, cfg, err, n);
}
int pmx_output(pm_pc *pc, pm_pc_info *info) { return B.abi >= 3 && B.output ? B.output(pc, info) : -1; }
int pmx_decide(const pm_pc_cfg *cfg, int *k, char *err, size_t n)
{
    if (k) *k = 1;
    if (!LoadBridge(err, n)) return -1;
    if (!B.decide) return 0;
    pm_pc_cfg tmp;
    return B.decide(ForBridge(cfg, &tmp), k, err, n) == 0 ? 1 : -1;
}

#else

pm_pc *pmx_open(const pm_pc_cfg *cfg, int *k, char *err, size_t n)
{
    (void)cfg; if (k) *k = 1;
    if (err && n) snprintf(err, n, "pushcore is Windows-only");
    return NULL;
}
int pmx_push(pm_pc *pc, const pm_pc_planes *src, int64_t pts, int disc, char *err, size_t n) { (void)pc; (void)src; (void)pts; (void)disc; (void)err; (void)n; return -1; }
int pmx_poll(pm_pc *pc, uint32_t ms, pm_pc_dest_fn d, pm_pc_done_fn f, void *u, char *err, size_t n) { (void)pc; (void)ms; (void)d; (void)f; (void)u; (void)err; (void)n; return -1; }
int pmx_flush(pm_pc *pc) { (void)pc; return -1; }
void pmx_close(pm_pc *pc) { (void)pc; }
const char *pmx_describe(pm_pc *pc) { (void)pc; return ""; }
int pmx_has_d3d11(void) { return 0; }
pm_pc *pmx_open_d3d11(const pm_pc_cfg *cfg, void *dev, void *mutex, int *k, char *err, size_t n)
{
    (void)cfg; (void)dev; (void)mutex; if (k) *k = 1;
    if (err && n) snprintf(err, n, "pushcore is Windows-only");
    return NULL;
}
int pmx_push_d3d11(pm_pc *pc, void *t, unsigned s, int64_t pts, int disc, char *err, size_t n) { (void)pc; (void)t; (void)s; (void)pts; (void)disc; (void)err; (void)n; return -1; }
int pmx_poll_d3d11(pm_pc *pc, uint32_t ms, pm_pc_dest_tex_fn d, pm_pc_done_fn f, void *u, char *err, size_t n) { (void)pc; (void)ms; (void)d; (void)f; (void)u; (void)err; (void)n; return -1; }
int pmx_copy_d3d11(pm_pc *pc, void *src, unsigned ss, void *dst, unsigned ds) { (void)pc; (void)src; (void)ss; (void)dst; (void)ds; return -1; }
unsigned pmx_abi(void) { return 0u; }
int pmx_update(pm_pc *pc, const pm_pc_cfg *cfg, char *err, size_t n) { (void)pc; (void)cfg; if (err && n) snprintf(err, n, "pushcore is Windows-only"); return -1; }
int pmx_output(pm_pc *pc, pm_pc_info *info) { (void)pc; (void)info; return -1; }
int pmx_decide(const pm_pc_cfg *cfg, int *k, char *err, size_t n) { (void)cfg; if (k) *k = 1; if (err && n) snprintf(err, n, "pushcore is Windows-only"); return -1; }

#endif
