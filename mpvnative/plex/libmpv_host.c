/*
 * libmpv_host <libmpv.so> <media file> [<more files>...]: loads another program's libmpv (Plex's, inside its Flatpak
 * sandbox) and plays each file with vf=pushmpc=smoother=2 on vo=null until one plays to the end. Prints whether the
 * filter was accepted, the filter's own log line and the end-of-file reason. Exit 0: accepted, x2 seen, played to the
 * end; 2: libmpv not usable; 3: otherwise. CI and installer self-tests only.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int event_id; int error; uint64_t reply_userdata; void *data; } mpv_event_t;
typedef struct { int reason; int error; } mpv_end_file_t;

int main(int argc, char **argv)
{
    void *h, *ctx;
    void *(*create)(void);
    int (*init)(void *), (*set)(void *, const char *, const char *), (*cmd)(void *, const char **);
    mpv_event_t *(*wait)(void *, double);
    void (*destroy)(void *);
    const char *(*errstr)(int);
    char log[512], line[1024];
    int f, ok = 0;
    if (argc < 3) { fprintf(stderr, "usage: libmpv_host <libmpv.so> <media file>...\n"); return 2; }
    h = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
    if (!h) { printf("host: cannot load %s: %s\n", argv[1], dlerror()); return 2; }
    create = (void *(*)(void))dlsym(h, "mpv_create");
    init = (int (*)(void *))dlsym(h, "mpv_initialize");
    set = (int (*)(void *, const char *, const char *))dlsym(h, "mpv_set_option_string");
    cmd = (int (*)(void *, const char **))dlsym(h, "mpv_command");
    wait = (mpv_event_t *(*)(void *, double))dlsym(h, "mpv_wait_event");
    destroy = (void (*)(void *))dlsym(h, "mpv_terminate_destroy");
    errstr = (const char *(*)(int))dlsym(h, "mpv_error_string");
    if (!create || !init || !set || !cmd || !wait || !destroy) { printf("host: %s lacks the client API\n", argv[1]); return 2; }
    for (f = 2; f < argc && !ok; f++) {
        int vfrc, reason = -1, i;
        FILE *lf;
        int seen = 0;
        snprintf(log, sizeof(log), "/tmp/libmpv-host-%d.log", f);
        ctx = create();
        set(ctx, "vo", "null"); set(ctx, "ao", "null"); set(ctx, "log-file", log); set(ctx, "msg-level", "all=v");
        vfrc = set(ctx, "vf", "pushmpc=smoother=2");
        if (init(ctx) < 0) { printf("host: mpv_initialize failed\n"); return 2; }
        {
            const char *args[] = { "loadfile", argv[f], NULL };
            cmd(ctx, args);
        }
        for (i = 0; i < 120; i++) {
            mpv_event_t *ev = wait(ctx, 1.0);
            if (ev->event_id == 7) { reason = ((mpv_end_file_t *)ev->data)->reason; break; }
            if (ev->event_id == 1) break;
        }
        destroy(ctx);
        lf = fopen(log, "r");
        while (lf && fgets(line, sizeof(line), lf))
            if (strstr(line, "portable-cpu") && strstr(line, "x2")) { seen = 1; printf("host: %s", line); }
        if (lf) fclose(lf);
        printf("host: %s: vf %s; end of file reason %d (0 = eof)\n", argv[f], vfrc >= 0 ? "accepted" : (errstr ? errstr(vfrc) : "refused"), reason);
        ok = vfrc >= 0 && seen && reason == 0;
    }
    return ok ? 0 : 3;
}
