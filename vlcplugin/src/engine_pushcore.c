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
            }
        }
    }
    const int ok = B.dll != NULL;
    ReleaseSRWLockExclusive(&lockB);
    if (!ok && err && n) snprintf(err, n, "pm_pushcore.dll is not next to the plugin (or incomplete)");
    return ok;
}

pm_pc *pmx_open(const pm_pc_cfg *cfg, int *k, char *err, size_t n)
{
    if (!LoadBridge(err, n)) return NULL;
    return B.open(cfg, k, err, n);
}
int pmx_push(pm_pc *pc, const pm_pc_planes *src, int64_t pts, int disc, char *err, size_t n) { return B.push(pc, src, pts, disc, err, n); }
int pmx_poll(pm_pc *pc, uint32_t ms, pm_pc_dest_fn d, pm_pc_done_fn f, void *u, char *err, size_t n) { return B.poll(pc, ms, d, f, u, err, n); }
int pmx_flush(pm_pc *pc) { return B.flush(pc); }
void pmx_close(pm_pc *pc) { if (pc) B.close(pc); }
const char *pmx_describe(pm_pc *pc) { return B.describe ? B.describe(pc) : ""; }

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

#endif
