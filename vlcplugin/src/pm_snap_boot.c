/*
 * pm_snap_boot.c: bringing PushMPC into the Snap VLC (Linux).
 *
 * The Snap's wrapper unsets VLC_PLUGIN_PATH and VLC 3 has no user plugin folder, so the plugin bank never sees this
 * plugin. What the confined VLC does read is the user's Lua folder and its vlcrc: the installer adds a Lua interface
 * script (lua-intf=pushmpc_boot, extraintf=luaintf) that calls
 *     package.loadlib(<SNAP_USER_COMMON>/pushmpc/libpushmpc_plugin.so, "luaopen_pushmpc_boot")()
 * and this function then hands VLC's existing "postproc" video filter module (the static-chain slot) and one rarely
 * used video filter (for pushmpc_out) our callbacks.
 *
 * It touches VLC's internal module_t / vlc_plugin_t (src/modules/modules.h, 3.0.x), so it first checks that layout
 * against VLC's exported getters (module_get_object / _name / _capability / _score must return exactly the fields read
 * at those offsets) and does nothing at all on any mismatch. Off switch: PUSHMPC_SNAP_BOOT=0, or remove the script.
 * Settings in this mode: <SNAP_USER_COMMON>/pushmpc/pushmpc.conf (smoother=, engine=, worker=, ... one per line).
 *
 * Licence: GPL-3.0-or-later.
 */
#ifdef __linux__
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vlc_common.h>
#include <vlc_modules.h>
#include <vlc_configuration.h>

int pm_filter_open(vlc_object_t *);
void pm_filter_close(vlc_object_t *);
int pm_out_open(vlc_object_t *);
void pm_out_close(vlc_object_t *);

/* VLC 3.0.x src/modules/modules.h (HAVE_DYNAMIC_PLUGINS), checked at run time below */
typedef struct pm_vlc_plugin {
    struct pm_vlc_plugin *next;
    struct pm_module *module;
    unsigned modules_count;
    const char *textdomain;
    struct { void *items; size_t size; size_t count; size_t booleans; } conf;
    atomic_bool loaded;
    bool unloadable;
    void *handle;
    char *abspath;
    char *path;
    int64_t mtime;
    uint64_t size;
} pm_vlc_plugin;
typedef struct pm_module {
    pm_vlc_plugin *plugin;
    struct pm_module *next;
    unsigned i_shortcuts;
    const char **pp_shortcuts;
    const char *psz_shortname;
    const char *psz_longname;
    const char *psz_help;
    const char *psz_capability;
    int i_score;
    const char *activate_name;
    const char *deactivate_name;
    void *pf_activate;
    void *pf_deactivate;
} pm_module;

static char conf_text[4096];
static bool snap_mode;

const char *pm_snap_conf(const char *key)
{
    static __thread char value[128];
    if (!snap_mode) return NULL;
    const size_t n = strlen(key);
    for (const char *line = conf_text; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
        if (!strncmp(line, key, n) && line[n] == '=') {
            const char *v = line + n + 1;
            size_t len = strcspn(v, "\r\n");
            if (len >= sizeof(value)) len = sizeof(value) - 1;
            memcpy(value, v, len); value[len] = '\0';
            return value;
        }
    }
    return NULL;
}

static void LoadConf(void)
{
    const char *base = getenv("SNAP_USER_COMMON");
    char path[1024];
    snprintf(path, sizeof(path), "%s/pushmpc/pushmpc.conf", base ? base : ".");
    FILE *f = fopen(path, "r");
    if (!f) return;
    size_t got = fread(conf_text, 1, sizeof(conf_text) - 1, f);
    conf_text[got] = '\0';
    fclose(f);
}

/* the layout check: the fields at our offsets must be exactly what VLC's own getters return */
static bool Plausible(module_t *mod, const char *want_cap)
{
    pm_module *m = (pm_module *)mod;
    if (!m || !m->plugin || m->i_shortcuts == 0 || m->i_shortcuts > 64 || !m->pp_shortcuts) return false;
    if (module_get_object(mod) != m->pp_shortcuts[0]) return false;
    if (module_get_name(mod, false) != (m->psz_shortname ? m->psz_shortname : m->pp_shortcuts[0])) return false;
    if (m->psz_longname && module_get_name(mod, true) != m->psz_longname) return false;
    if (!m->psz_capability || module_get_capability(mod) != m->psz_capability) return false;
    if (module_get_score(mod) != m->i_score) return false;
    if (strcmp(m->psz_capability, want_cap) != 0) return false;
    /* the plug-in owns this module */
    bool found = false;
    unsigned n = 0;
    for (pm_module *p = m->plugin->module; p && n < 1024; p = p->next, n++) if (p == m) { found = true; break; }
    return found && m->plugin->modules_count >= 1 && m->plugin->modules_count <= 1024;
}

static void TakeOver(pm_module *m, const char *name, void *open, void *close)
{
    /* VLC frees the shortcut array when it tears the module bank down (not the strings), so it must be heap memory;
     * the module's old array is left alone (a few bytes) */
    const char **shortcuts = malloc(sizeof(*shortcuts));
    if (!shortcuts) return;
    shortcuts[0] = name;
    m->pf_activate = open;
    m->pf_deactivate = close;
    m->activate_name = NULL;
    m->deactivate_name = NULL;
    m->pp_shortcuts = shortcuts;
    m->i_shortcuts = 1;
    m->plugin->unloadable = false;                     /* never dlclose a handle that is not VLC's */
    atomic_store(&m->plugin->loaded, true);            /* module_Map returns early: VLC keeps our callbacks */
}

__attribute__((visibility("default"))) int luaopen_pushmpc_boot(void *L)
{
    (void)L;
    const char *off = getenv("PUSHMPC_SNAP_BOOT");
    if (off && off[0] == '0') { fprintf(stderr, "PushMPC snap boot: off (PUSHMPC_SNAP_BOOT=0)\n"); return 0; }
    /* CI only: the snap's launcher overwrites VDPAU_DRIVER_PATH; a test VDPAU driver gets in through this variable
     * (libvdpau reads it when a decoder opens, after this runs). Without the variable nothing changes. */
    const char *vdp = getenv("PUSHMPC_TEST_VDPAU_DRIVER_PATH");
    if (vdp && *vdp) { setenv("VDPAU_DRIVER_PATH", vdp, 1); fprintf(stderr, "PushMPC snap boot: VDPAU_DRIVER_PATH=%s (test)\n", vdp); }
    static bool done;
    if (done) return 0;
    done = true;
    /* stay mapped after the Lua state that loaded us closes */
    Dl_info di;
    if (dladdr((void *)luaopen_pushmpc_boot, &di) && di.dli_fname) dlopen(di.dli_fname, RTLD_NOW | RTLD_NODELETE);

    /* the static-chain slot: VLC's own postproc module, else a donor filter renamed to postproc */
    static const char *donors[] = { "postproc", "psychedelic", "puzzle", "ripple", "wave", "motionblur", "antiflicker", NULL };
    pm_module *slot = NULL, *out = NULL;
    for (int i = 0; donors[i] && (!slot || !out); i++) {
        module_t *mod = module_find(donors[i]);
        if (!mod || !Plausible(mod, "video filter")) continue;
        if (!slot) slot = (pm_module *)mod;
        else if ((pm_module *)mod != slot) out = (pm_module *)mod;
    }
    if (!slot || !out) {
        fprintf(stderr, "PushMPC snap boot: VLC's module layout or filters are not as expected: nothing changed\n");
        return 0;
    }
    snap_mode = true;
    LoadConf();
    const char *slot_name = slot->pp_shortcuts[0], *out_name = out->pp_shortcuts[0];
    TakeOver(slot, "postproc", (void *)pm_filter_open, (void *)pm_filter_close);
    TakeOver(out, "pushmpc_out", (void *)pm_out_open, (void *)pm_out_close);
    fprintf(stderr, "PushMPC snap boot: slot taken (postproc <- %s, pushmpc_out <- %s)\n", slot_name, out_name);
    return 0;
}
#else
const char *pm_snap_conf(const char *key);
const char *pm_snap_conf(const char *key) { (void)key; return 0; }
#endif
