/*
 * pm_dmabuf_test: the zero-copy path without VLC. Surfaces like a VA-API driver exports them (NV12 / P010, linear,
 * two layers in one dmabuf) are made from memfds through /dev/udmabuf; two of them with a moving box go into the GPU
 * engine as dmabufs and the generated frame comes back into a third one. The same two frames through the copy path
 * (pm_engine_push / pm_engine_generate) must give the identical frame.
 *   pm_dmabuf_test            exit 0 = both depths identical; 3 = zero copy unavailable here (the reason is printed)
 *   pm_dmabuf_test --expect-unavailable   (run with PUSHMPC_VK_NO_DMABUF=1): the engine must report it unavailable
 * Needs PUSHMPC_VK_ALLOW_CPU=1 on lavapipe. Licence: GPL-3.0-or-later.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/udmabuf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "pm_engine.h"

#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

typedef struct { int memfd, dbfd, w, h, bpc, bits; size_t size, pitch, uvoff; uint8_t *map; } surf;

static int MakeSurf(surf *s, int w, int h, int bits)
{
    memset(s, 0, sizeof(*s));
    s->w = w; s->h = h; s->bits = bits; s->bpc = bits > 8 ? 2 : 1;
    s->pitch = ((size_t)w * s->bpc + 63) & ~(size_t)63;
    s->uvoff = s->pitch * (size_t)h;
    const size_t pg = (size_t)sysconf(_SC_PAGESIZE);
    s->size = (s->uvoff + s->pitch * (size_t)(h / 2) + pg - 1) / pg * pg;
    s->memfd = memfd_create("pm_dmabuf_test", MFD_ALLOW_SEALING);
    if (s->memfd < 0 || ftruncate(s->memfd, (off_t)s->size) != 0 || fcntl(s->memfd, F_ADD_SEALS, F_SEAL_SHRINK) != 0) { perror("memfd"); return -1; }
    s->map = mmap(NULL, s->size, PROT_READ | PROT_WRITE, MAP_SHARED, s->memfd, 0);
    if (s->map == MAP_FAILED) { perror("mmap"); return -1; }
    const int dev = open("/dev/udmabuf", O_RDWR);
    if (dev < 0) { perror("/dev/udmabuf"); return -1; }
    struct udmabuf_create c = { .memfd = (uint32_t)s->memfd, .flags = UDMABUF_FLAGS_CLOEXEC, .offset = 0, .size = s->size };
    s->dbfd = ioctl(dev, UDMABUF_CREATE, &c);
    close(dev);
    if (s->dbfd < 0) { perror("UDMABUF_CREATE"); return -1; }
    return 0;
}
static void Desc(const surf *s, pm_dmabuf *d)
{
    memset(d, 0, sizeof(*d));
    d->width = s->w; d->height = s->h; d->bits = s->bits;
    d->nobjects = 1; d->fd[0] = s->dbfd; d->size[0] = s->size; d->modifier[0] = 0;   /* DRM_FORMAT_MOD_LINEAR */
    d->nlayers = 2;
    d->layer[0].drm_format = s->bits > 8 ? FOURCC('R', '1', '6', ' ') : FOURCC('R', '8', ' ', ' ');
    d->layer[0].object = 0; d->layer[0].offset = 0; d->layer[0].pitch = (uint32_t)s->pitch;
    d->layer[1].drm_format = s->bits > 8 ? FOURCC('G', 'R', '3', '2') : FOURCC('G', 'R', '8', '8');
    d->layer[1].object = 0; d->layer[1].offset = (uint32_t)s->uvoff; d->layer[1].pitch = (uint32_t)s->pitch;
}
static void Put(surf *s, uint8_t *row, int x, unsigned v8)   /* an 8-bit level, as P010 holds it for 10 bits */
{
    if (s->bpc == 1) row[x] = (uint8_t)v8;
    else { const unsigned w = (v8 << 2) << 6; row[2 * x] = (uint8_t)(w & 0xff); row[2 * x + 1] = (uint8_t)(w >> 8); }
}
static void Draw(surf *s, int x0)
{
    const int y0 = s->h / 2 - 12;
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++)
            Put(s, s->map + (size_t)y * s->pitch, x, (y >= y0 && y < y0 + 24 && x >= x0 && x < x0 + 24) ? 235u : 16u);
    for (int y = 0; y < s->h / 2; y++)
        for (int x = 0; x < s->w; x++) Put(s, s->map + s->uvoff + (size_t)y * s->pitch, x, (x & 1) ? 100u + (unsigned)(y & 15) : 140u);
}
static unsigned Get(const surf *s, const uint8_t *row, int x)
{
    return s->bpc == 1 ? row[x] : ((unsigned)row[2 * x] | ((unsigned)row[2 * x + 1] << 8)) >> 6;
}
/* the surface as planar I420 / I0AL (what the copy path reads and writes) */
static void ToPlanar(const surf *s, uint8_t *p[3], ptrdiff_t st[3])
{
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++) {
            const unsigned v = Get(s, s->map + (size_t)y * s->pitch, x);
            if (s->bpc == 1) p[0][y * st[0] + x] = (uint8_t)v; else ((uint16_t *)(p[0] + y * st[0]))[x] = (uint16_t)v;
        }
    for (int y = 0; y < s->h / 2; y++)
        for (int x = 0; x < s->w / 2; x++) {
            const uint8_t *r = s->map + s->uvoff + (size_t)y * s->pitch;
            const unsigned u = Get(s, r, 2 * x), v = Get(s, r, 2 * x + 1);
            if (s->bpc == 1) { p[1][y * st[1] + x] = (uint8_t)u; p[2][y * st[2] + x] = (uint8_t)v; }
            else { ((uint16_t *)(p[1] + y * st[1]))[x] = (uint16_t)u; ((uint16_t *)(p[2] + y * st[2]))[x] = (uint16_t)v; }
        }
}

static int Run(int bits)
{
    const int w = 256, h = 144;
    char err[300] = "";
    pm_engine_cfg cfg = { w, h, bits, 0, 1, PM_ENGINE_GPU };
    pm_engine *e = pm_engine_open(&cfg, err, sizeof(err));
    if (!e) { printf("GPU engine unavailable: %s\n", err); return 3; }
    const char *why = "";
    if (!pm_engine_dmabuf_ok(e, &why)) { printf("ZEROCOPY-UNAVAILABLE (%d-bit, %s): %s\n", bits, pm_engine_name(e), why); pm_engine_close(e); return 3; }
    surf a, b, c;
    if (MakeSurf(&a, w, h, bits) || MakeSurf(&b, w, h, bits) || MakeSurf(&c, w, h, bits)) { pm_engine_close(e); return 3; }
    Draw(&a, 40); Draw(&b, 56);
    memset(c.map, 0, c.size);
    pm_dmabuf da, db, dc;
    Desc(&a, &da); Desc(&b, &db); Desc(&c, &dc);
    if (pm_engine_push_dmabuf(e, &da, 0, err, sizeof(err)) || pm_engine_push_dmabuf(e, &db, 1, err, sizeof(err)) ||
        pm_engine_generate_dmabuf(e, 0.5f, &dc, err, sizeof(err))) {
        printf("FAIL (%d-bit): zero-copy push/generate: %s\n", bits, err);
        pm_engine_close(e);
        return 1;
    }
    /* the copy path, same engine, same frames */
    const int bps = bits > 8 ? 2 : 1;
    ptrdiff_t st[3] = { (ptrdiff_t)w * bps, (ptrdiff_t)(w / 2) * bps, (ptrdiff_t)(w / 2) * bps };
    uint8_t *pa[3], *pb[3], *pr[3], *pc[3];
    for (int i = 0; i < 3; i++) {
        const size_t n = (size_t)st[i] * (size_t)(i ? h / 2 : h);
        pa[i] = calloc(1, n); pb[i] = calloc(1, n); pr[i] = calloc(1, n); pc[i] = calloc(1, n);
    }
    ToPlanar(&a, pa, st); ToPlanar(&b, pb, st); ToPlanar(&c, pc, st);
    pm_engine_forget(e);
    pm_yuv ya = { { pa[0], pa[1], pa[2] }, { st[0], st[1], st[2] } }, yb = { { pb[0], pb[1], pb[2] }, { st[0], st[1], st[2] } };
    pm_yuv_out yr = { { pr[0], pr[1], pr[2] }, { st[0], st[1], st[2] } };
    if (pm_engine_push(e, &ya, 0, err, sizeof(err)) || pm_engine_push(e, &yb, 1, err, sizeof(err))) { printf("FAIL: copy path: %s\n", err); return 1; }
    pm_engine_generate(e, 0.5f, &yr);
    int bad = 0, maxd = 0;
    double sx = 0, cnt = 0;
    for (int i = 0; i < 3; i++) {
        const int pw = i ? w / 2 : w, ph = i ? h / 2 : h;
        for (int y = 0; y < ph; y++)
            for (int x = 0; x < pw; x++) {
                const int u = bps == 1 ? pc[i][y * st[i] + x] : ((uint16_t *)(pc[i] + y * st[i]))[x];
                const int v = bps == 1 ? pr[i][y * st[i] + x] : ((uint16_t *)(pr[i] + y * st[i]))[x];
                const int d = abs(u - v);
                if (d) bad++;
                if (d > maxd) maxd = d;
                if (i == 0 && u > (bits > 8 ? 512 : 128)) { sx += x; cnt += 1; }
            }
    }
    const double cx = cnt ? sx / cnt : -1;
    printf("%s (%d-bit, %s): zero copy vs copy path: %d samples differ, max %d; box centre %.2f (real frames 51.5 / 67.5)\n",
           bad ? "FAIL" : "PASS", bits, pm_engine_name(e), bad, maxd, cx);
    pm_engine_close(e);
    return bad || cx < 52.0 || cx > 67.0 ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--expect-unavailable")) {
        char err[300] = "";
        pm_engine_cfg cfg = { 256, 144, 8, 0, 1, PM_ENGINE_GPU };
        pm_engine *e = pm_engine_open(&cfg, err, sizeof(err));
        if (!e) { printf("GPU engine unavailable (%s): the copy path is the only one, as expected\n", err); return 0; }
        const char *why = "";
        const int ok = pm_engine_dmabuf_ok(e, &why);
        printf("%s: zero copy %s (%s)\n", ok ? "FAIL" : "PASS", ok ? "still offered" : "unavailable, the copy path is chosen", why);
        pm_engine_close(e);
        return ok ? 1 : 0;
    }
    const int r8 = Run(8);
    if (r8 == 3) return 3;
    const int r10 = Run(10);
    return r8 || r10 ? 1 : 0;
}
