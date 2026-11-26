/*
 * libmpv_host <libmpv.so> <media file> [<more files>...]: loads another program's libmpv (Plex's, inside its Flatpak
 * sandbox) and plays each file with vf=pushmpc=smoother=2 on vo=null until one plays to the end; after the given
 * files it tries a Y4M it writes itself (rawvideo: no decoder download needed). Prints whether the filter was
 * accepted, the filter's own log line, the end-of-file reason and, on failure, the log's error lines. Exit 0:
 * accepted, the x2 filter line seen, played to the end; 2: libmpv not usable; 3: otherwise. Self-tests only.
 * PUSHMPC_HOST_SCRIPT=<pushmpc_hook.so>: the C-plugin route instead of the preload: the host loads the hook as an mpv
 * script (script-opts pushmpc-vf=smoother=2) and sets no vf itself; the hook appends the filter (Snap Plex).
 * PUSHMPC_HOST_TMP=<dir>: where the Y4M and the logs go (default /tmp; a Snap's /tmp is private anyway).
 * PUSHMPC_HOST_VF='' sets no filter (a PushMPC libmpv stand-in on macOS adds it inside mpv_initialize);
 * PUSHMPC_HOST_ACCEPT_EOF=1: playing to the end is enough (the filter reports in its own log).
 * PUSHMPC_HOST_PROBE_SCRIPT=<pushmpc_hook.so>: the host sets the vf itself and also loads the hook as a C plugin
 * (script-opts pushmpc-append=no), to find out whether this libmpv runs C plugins (pushmpc-plex-linux.sh's route choice).
 * With either one it reports whether this libmpv ran the hook ("host: C plugin: loaded" / "not loaded").
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct { int event_id; int error; uint64_t reply_userdata; void *data; } mpv_event_t;
typedef struct { int reason; int error; } mpv_end_file_t;

#ifdef __APPLE__
/* Plex's libmpv.2.dylib on macOS leaves mpv's Cocoa event hooks to the host executable (looked up in the flat
 * namespace, normally defined by Plex's app): no-op stand-ins so the self-test can load it. */
#define PM_STUB(n) __attribute__((visibility("default"), used)) int n(void) { return 0; }
PM_STUB(cocoa_init_media_keys)
PM_STUB(cocoa_uninit_media_keys)
PM_STUB(cocoa_set_input_context)
PM_STUB(cocoa_set_mpv_handle)
PM_STUB(cocoa_put_key)
PM_STUB(cocoa_put_key_with_modifiers)
PM_STUB(cocoa_init_cocoa_cb)
PM_STUB(cocoa_main)
PM_STUB(cocoa_start_event_monitor)
PM_STUB(cocoa_set_istty)
#define PM_DLOPEN_FLAGS (RTLD_LAZY | RTLD_GLOBAL)
#else
#define PM_DLOPEN_FLAGS (RTLD_NOW | RTLD_GLOBAL)
#endif

static int write_y4m(const char *path)
{
    enum { W = 320, H = 180 };   /* VideoToolbox's interpolators refuse tiny pictures */
    static unsigned char px[W * H * 3 / 2];
    int n, k;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "YUV4MPEG2 W%d H%d F24:1 Ip A1:1 C420jpeg\n", (int)W, (int)H);
    for (n = 0; n < 48; n++) {
        for (k = 0; k < W * H; k++) px[k] = (unsigned char)(((k % W) + (k / W) / 2 + n * 4) & 255);   /* a moving ramp */
        memset(px + W * H, 128, W * H / 2);
        fputs("FRAME\n", f);
        fwrite(px, 1, sizeof(px), f);
    }
    return fclose(f);
}

int main(int argc, char **argv)
{
    void *h, *ctx;
    void *(*create)(void);
    int (*init)(void *), (*set)(void *, const char *, const char *), (*cmd)(void *, const char **);
    mpv_event_t *(*wait)(void *, double);
    void (*destroy)(void *);
    const char *(*errstr)(int);
    char log[512], line[1024], y4m[256];
    const char *files[64];
    int nf = 0, f, ok = 0;
    if (argc < 2) { fprintf(stderr, "usage: libmpv_host <libmpv.so> [<media file>...]\n"); return 2; }
    h = dlopen(argv[1], PM_DLOPEN_FLAGS);
    if (!h) { printf("host: cannot load %s: %s\n", argv[1], dlerror()); return 2; }
    create = (void *(*)(void))dlsym(h, "mpv_create");
    init = (int (*)(void *))dlsym(h, "mpv_initialize");
    set = (int (*)(void *, const char *, const char *))dlsym(h, "mpv_set_option_string");
    cmd = (int (*)(void *, const char **))dlsym(h, "mpv_command");
    wait = (mpv_event_t *(*)(void *, double))dlsym(h, "mpv_wait_event");
    destroy = (void (*)(void *))dlsym(h, "mpv_terminate_destroy");
    errstr = (const char *(*)(int))dlsym(h, "mpv_error_string");
    if (!create || !init || !set || !cmd || !wait || !destroy) { printf("host: %s lacks the client API\n", argv[1]); return 2; }
    for (f = 2; f < argc && nf < 63; f++) files[nf++] = argv[f];
    const char *tmp = getenv("PUSHMPC_HOST_TMP") && *getenv("PUSHMPC_HOST_TMP") ? getenv("PUSHMPC_HOST_TMP") : "/tmp";
    snprintf(y4m, sizeof(y4m), "%s/libmpv-host-%d.y4m", tmp, (int)getpid());
    if (write_y4m(y4m) == 0) files[nf++] = y4m;
    for (f = 0; f < nf && !ok; f++) {
        int vfrc, reason = -1, i, seen = 0, shown = 0, script = 0;
        FILE *lf;
        snprintf(log, sizeof(log), "%s/libmpv-host-%d-%d.log", tmp, (int)getpid(), f);
        ctx = create();
        set(ctx, "vo", "null"); set(ctx, "ao", "null"); set(ctx, "log-file", log); set(ctx, "msg-level", "all=v"); set(ctx, "input-media-keys", "no");
        if (getenv("PUSHMPC_HOST_SCRIPT") && *getenv("PUSHMPC_HOST_SCRIPT")) {   /* the hook appends the filter itself */
            vfrc = set(ctx, "scripts", getenv("PUSHMPC_HOST_SCRIPT"));
            set(ctx, "script-opts", getenv("PUSHMPC_HOST_VF") ? getenv("PUSHMPC_HOST_VF") : "pushmpc-vf=smoother=2");
        } else {
            vfrc = set(ctx, "vf", getenv("PUSHMPC_HOST_VF") ? getenv("PUSHMPC_HOST_VF") : "pushmpc=smoother=2");   /* CI: a forced backend */
            if (getenv("PUSHMPC_HOST_PROBE_SCRIPT") && *getenv("PUSHMPC_HOST_PROBE_SCRIPT")) {   /* does this libmpv run C plugins? */
                set(ctx, "scripts", getenv("PUSHMPC_HOST_PROBE_SCRIPT"));
                set(ctx, "script-opts", "pushmpc-append=no");
            }
        }
        if (init(ctx) < 0) { printf("host: mpv_initialize failed\n"); return 2; }
        {
            const char *args[] = { "loadfile", files[f], NULL };
            cmd(ctx, args);
        }
        for (i = 0; i < 120; i++) {
            mpv_event_t *ev = wait(ctx, 1.0);
            if (ev->event_id == 7) { reason = ((mpv_end_file_t *)ev->data)->reason; break; }
            if (ev->event_id == 1) break;
        }
        destroy(ctx);
        lf = fopen(log, "r");
        while (lf && fgets(line, sizeof(line), lf)) {
            if (strstr(line, "frames") && strstr(line, " -> ") && strstr(line, ", x2;")) { seen = 1; printf("host: %s", line); }
            /* mpv's "Loading cplugin script .../pushmpc_hook.so" or the plugin's own client tag; never its refusal
               ("Can't load unknown script") */
            if (strstr(line, "pushmpc_hook") && !strstr(line, "unknown") && !strstr(line, "annot") && !strstr(line, "rror")
                && (strstr(line, "Loading") || strstr(line, "[pushmpc_hook]")))
                script = 1;
        }
        if ((getenv("PUSHMPC_HOST_SCRIPT") && *getenv("PUSHMPC_HOST_SCRIPT")) || (getenv("PUSHMPC_HOST_PROBE_SCRIPT") && *getenv("PUSHMPC_HOST_PROBE_SCRIPT")))
            printf("host: C plugin: %s\n", script ? "loaded" : "not loaded");
        printf("host: %s: vf %s; end of file reason %d (0 = eof)\n", strrchr(files[f], '/') ? strrchr(files[f], '/') + 1 : files[f],
               vfrc >= 0 ? "accepted" : (errstr ? errstr(vfrc) : "refused"), reason);
        /* PUSHMPC_HOST_ACCEPT_EOF: the filter reports elsewhere (the IINA bridge: its own log), played to the end is enough */
        ok = vfrc >= 0 && reason == 0 && (seen || getenv("PUSHMPC_HOST_ACCEPT_EOF"));
        if (lf && !ok) {
            rewind(lf);
            while (fgets(line, sizeof(line), lf) && shown < 6)
                if (strstr(line, "rror") || strstr(line, "ailed") || strstr(line, "annot") || strstr(line, "pushmpc") || strstr(line, "No video"))
                    { printf("host:   log: %s", line); shown++; }
        }
        if (lf) fclose(lf);
        unlink(log);
    }
    unlink(y4m);
    return ok ? 0 : 3;
}
