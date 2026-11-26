/*
 * libmpv_host <libmpv.so> <media file> [<more files>...]: loads another program's libmpv (Plex's, inside its Flatpak
 * sandbox) and plays each file with vf=pushmpc=smoother=2 on vo=null until one plays to the end; after the given
 * files it tries a Y4M it writes itself (rawvideo: no decoder download needed). Prints whether the filter was
 * accepted, the filter's own log line, the end-of-file reason and, on failure, the log's error lines. Exit 0:
 * accepted, the x2 filter line seen, played to the end; 2: libmpv not usable; 3: otherwise. Self-tests only.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct { int event_id; int error; uint64_t reply_userdata; void *data; } mpv_event_t;
typedef struct { int reason; int error; } mpv_end_file_t;

static int write_y4m(const char *path)
{
    unsigned char px[64 * 64 * 3 / 2];
    int n, k;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fputs("YUV4MPEG2 W64 H64 F24:1 Ip A1:1 C420jpeg\n", f);
    for (n = 0; n < 48; n++) {
        for (k = 0; k < 64 * 64; k++) px[k] = (unsigned char)((k + n * 5) & 255);
        memset(px + 64 * 64, 128, 64 * 64 / 2);
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
    for (f = 2; f < argc && nf < 63; f++) files[nf++] = argv[f];
    snprintf(y4m, sizeof(y4m), "/tmp/libmpv-host-%d.y4m", (int)getpid());
    if (write_y4m(y4m) == 0) files[nf++] = y4m;
    for (f = 0; f < nf && !ok; f++) {
        int vfrc, reason = -1, i, seen = 0, shown = 0;
        FILE *lf;
        snprintf(log, sizeof(log), "/tmp/libmpv-host-%d-%d.log", (int)getpid(), f);
        ctx = create();
        set(ctx, "vo", "null"); set(ctx, "ao", "null"); set(ctx, "log-file", log); set(ctx, "msg-level", "all=v");
        vfrc = set(ctx, "vf", "pushmpc=smoother=2");
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
        while (lf && fgets(line, sizeof(line), lf))
            if (strstr(line, "frames") && strstr(line, " -> ") && strstr(line, ", x2;")) { seen = 1; printf("host: %s", line); }
        printf("host: %s: vf %s; end of file reason %d (0 = eof)\n", strrchr(files[f], '/') ? strrchr(files[f], '/') + 1 : files[f],
               vfrc >= 0 ? "accepted" : (errstr ? errstr(vfrc) : "refused"), reason);
        ok = vfrc >= 0 && seen && reason == 0;
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
