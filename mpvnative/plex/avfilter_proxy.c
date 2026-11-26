/*
 * avfilter-8.dll (proxy): puts vf_pushmpc into a program whose libmpv loads FFmpeg's libavfilter as a separate DLL
 * (Plex HTPC, Plex for Windows: libmpv-2.dll imports avfilter-8.dll, FFmpeg 5.x), without replacing its libmpv.
 *   - every export forwards to the program's own libavfilter, renamed avfilter-8-plex.dll (avfilter_proxy.def);
 *   - avfilter_get_by_name is ours: "pushmpc" comes from pushmpc-avfilter-<maj>.<min>.<micro>.dll, a libavfilter of
 *     the same release with only vf_pushmpc (it imports avutil-57.dll by name, so it binds to the program's own), picked
 *     by the original's avfilter_version(); every other name goes to the original.
 * libmpv resolves `vf=pushmpc` through avfilter_get_by_name (its direct lavfi bridge), so the name works in that
 * program. Without a matching build the proxy is a pure pass-through. PUSHMPC_PROXY_LOG=<file> logs what it did.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

typedef const void *(*get_by_name_fn)(const char *);
typedef unsigned (*version_fn)(void);

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static get_by_name_fn g_real;
static const void *g_filter;
static char g_status[512] = "not loaded";

static void plog(const char *msg)
{
    char path[MAX_PATH];
    snprintf(g_status, sizeof(g_status), "%s", msg);
    if (GetEnvironmentVariableA("PUSHMPC_PROXY_LOG", path, MAX_PATH)) {
        FILE *f = fopen(path, "ab");
        if (f) { fprintf(f, "pushmpc proxy: %s\n", msg); fclose(f); }
    }
    OutputDebugStringA(msg);
}

static BOOL CALLBACK init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    HMODULE self = NULL, orig, priv;
    wchar_t dir[MAX_PATH], path[MAX_PATH + 64];
    DWORD n;
    version_fn ver, pver;
    unsigned v;
    char msg[512];
    (void)once; (void)param; (void)ctx;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)(void *)init, &self) ||
        !(n = GetModuleFileNameW(self, dir, MAX_PATH)) || n >= MAX_PATH) { plog("cannot find its own folder"); return TRUE; }
    while (n > 0 && dir[n - 1] != L'\\' &&dir[n - 1] != L'/') dir[--n] = 0;
    swprintf(path, MAX_PATH + 64, L"%lsavfilter-8-plex.dll", dir);
    orig = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!orig) { plog("avfilter-8-plex.dll (the original) is missing"); return TRUE; }
    g_real = (get_by_name_fn)(void *)GetProcAddress(orig, "avfilter_get_by_name");
    ver = (version_fn)(void *)GetProcAddress(orig, "avfilter_version");
    if (!g_real || !ver) { plog("the original lacks avfilter_get_by_name / avfilter_version"); return TRUE; }
    v = ver();
    swprintf(path, MAX_PATH + 64, L"%lspushmpc-avfilter-%u.%u.%u.dll", dir, v >> 16, (v >> 8) & 255, v & 255);
    priv = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!priv) {
        snprintf(msg, sizeof(msg), "no build for libavfilter %u.%u.%u (pushmpc-avfilter-%u.%u.%u.dll, error %lu): pass-through",
                 v >> 16, (v >> 8) & 255, v & 255, v >> 16, (v >> 8) & 255, v & 255, GetLastError());
        plog(msg);
        return TRUE;
    }
    pver = (version_fn)(void *)GetProcAddress(priv, "avfilter_version");
    if (!pver || pver() != v) { plog("version mismatch between the original and the build: pass-through"); return TRUE; }
    {
        get_by_name_fn pg = (get_by_name_fn)(void *)GetProcAddress(priv, "avfilter_get_by_name");
        g_filter = pg ? pg("pushmpc") : NULL;
    }
    snprintf(msg, sizeof(msg), "libavfilter %u.%u.%u, pushmpc %s", v >> 16, (v >> 8) & 255, v & 255, g_filter ? "active" : "missing from the build");
    plog(msg);
    return TRUE;
}

__declspec(dllexport) const void *avfilter_get_by_name(const char *name)
{
    InitOnceExecuteOnce(&g_once, init, NULL, NULL);
    if (name && g_filter && !strcmp(name, "pushmpc"))
        return g_filter;
    return g_real ? g_real(name) : NULL;
}

/* for the installer's self-test and Setup: what the proxy found */
__declspec(dllexport) const char *pushmpc_proxy_status(void)
{
    InitOnceExecuteOnce(&g_once, init, NULL, NULL);
    return g_status;
}
