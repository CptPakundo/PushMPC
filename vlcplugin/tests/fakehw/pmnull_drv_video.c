/*
 * pmnull_drv_video.so: a null VA-API driver for CI (no GPU). Surfaces live in host memory (NV12, P010); "decoding"
 * draws a deterministic picture instead of reading the bitstream: on vaEndPicture the target surface gets a 32x32
 * bright box on a dark background at x = 16 + 4 * n, where n counts the pictures of that decode context (the test
 * clips have no B-frames, so decode order is display order). A frame interpolator in between must then put the box
 * half-way, which the CSV test display measures (its cx column).
 *
 * Use: LIBVA_DRIVERS_PATH=<dir> LIBVA_DRIVER_NAME=pmnull (libva loads <dir>/pmnull_drv_video.so).
 * Quirk switches (environment), to reproduce what real drivers differ in:
 *   PMNULL_DERIVE=0     vaDeriveImage fails (NVIDIA's nvidia-vaapi-driver, AMD tiled surfaces)
 *   PMNULL_PUTIMAGE=0   vaPutImage fails
 *   PMNULL_GETIMAGE=0   vaGetImage fails
 *   PMNULL_PITCH=odd    odd pitches and padded plane offsets
 *   PMNULL_BROKEN=1     uploads are silently lost (mapped derived images are a copy that is never written back,
 *                       vaPutImage does nothing): a driver whose writes do not reach the surface
 *   PMNULL_LOG=1        log calls to stderr
 * Licence: MIT.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <va/va.h>
#include <va/va_backend.h>
#ifndef VA_RT_FORMAT_YUV420_10
# define VA_RT_FORMAT_YUV420_10 0x00000100
#endif
#ifndef VA_FOURCC_P010
# define VA_FOURCC_P010 0x30313050
#endif

#define MAXOBJ 4096
enum { K_SURF = 1, K_BUF, K_IMG, K_CFG, K_CTX };
#define ID_BASE(k) ((VAGenericID)((k) << 24))

typedef struct {
    int w, h;
    uint32_t fourcc;            /* NV12 or P010 */
    int bpc;                    /* bytes per sample: 1 or 2 */
    uint32_t pitch, off_uv, size;
    uint8_t *data;
} surf_t;
typedef struct {
    VABufferType type;
    unsigned size, num;
    uint8_t *data;
    int owned;                  /* else it aliases a surface (derived image) */
    uint8_t *shadow;            /* PMNULL_BROKEN: a mapped derived image is a copy */
    VAImageID image;
} buf_t;
typedef struct { VAImage img; VASurfaceID derived_from; } img_t;
typedef struct { VAProfile profile; VAEntrypoint ep; unsigned rt; } cfg_t;
typedef struct { VAConfigID cfg; int w, h; VASurfaceID target; uint64_t pictures; } ctx_t;

typedef struct {
    pthread_mutex_t lock;
    void *obj[6][MAXOBJ];
    int derive, putimage, getimage, oddpitch, broken, log;
} drv_t;

#define LOG(d, ...) do { if ((d)->log) { fprintf(stderr, "pmnull: " __VA_ARGS__); fputc('\n', stderr); } } while (0)
static drv_t *D(VADriverContextP ctx) { return (drv_t *)ctx->pDriverData; }

static VAGenericID ObjAdd(drv_t *d, int kind, void *p)
{
    for (int i = 0; i < MAXOBJ; i++)
        if (!d->obj[kind][i]) { d->obj[kind][i] = p; return ID_BASE(kind) + (VAGenericID)i; }
    return VA_INVALID_ID;
}
static void *ObjGet(drv_t *d, int kind, VAGenericID id)
{
    if ((id >> 24) != (VAGenericID)kind) return NULL;
    const unsigned i = id & 0xffffff;
    return i < MAXOBJ ? d->obj[kind][i] : NULL;
}
static void ObjDel(drv_t *d, int kind, VAGenericID id)
{
    if ((id >> 24) == (VAGenericID)kind && (id & 0xffffff) < MAXOBJ) d->obj[kind][id & 0xffffff] = NULL;
}
static int EnvOn(const char *n, int def)
{
    const char *v = getenv(n);
    if (!v || !*v) return def;
    return !(v[0] == '0' || v[0] == 'n' || v[0] == 'N');
}

static const VAProfile kProfiles[] = { VAProfileH264ConstrainedBaseline, VAProfileH264Main, VAProfileH264High,
                                       VAProfileHEVCMain, VAProfileHEVCMain10 };
static int ProfileOk(VAProfile p)
{
    for (size_t i = 0; i < sizeof(kProfiles) / sizeof(kProfiles[0]); i++) if (kProfiles[i] == p) return 1;
    return 0;
}
static unsigned ProfileRt(VAProfile p) { return p == VAProfileHEVCMain10 ? VA_RT_FORMAT_YUV420_10 : VA_RT_FORMAT_YUV420; }

static VAStatus Terminate(VADriverContextP ctx)
{
    drv_t *d = D(ctx);
    for (int k = 1; k < 6; k++)
        for (int i = 0; i < MAXOBJ; i++) {
            void *p = d->obj[k][i];
            if (!p) continue;
            if (k == K_SURF) free(((surf_t *)p)->data);
            if (k == K_BUF) { if (((buf_t *)p)->owned) free(((buf_t *)p)->data); free(((buf_t *)p)->shadow); }
            free(p);
        }
    pthread_mutex_destroy(&d->lock);
    free(d);
    ctx->pDriverData = NULL;
    return VA_STATUS_SUCCESS;
}

static VAStatus QueryConfigProfiles(VADriverContextP ctx, VAProfile *list, int *num)
{
    (void)ctx;
    const int n = (int)(sizeof(kProfiles) / sizeof(kProfiles[0]));
    for (int i = 0; i < n; i++) list[i] = kProfiles[i];
    *num = n;
    return VA_STATUS_SUCCESS;
}
static VAStatus QueryConfigEntrypoints(VADriverContextP ctx, VAProfile p, VAEntrypoint *list, int *num)
{
    (void)ctx;
    if (!ProfileOk(p)) { *num = 0; return VA_STATUS_ERROR_UNSUPPORTED_PROFILE; }
    list[0] = VAEntrypointVLD;
    *num = 1;
    return VA_STATUS_SUCCESS;
}
static VAStatus GetConfigAttributes(VADriverContextP ctx, VAProfile p, VAEntrypoint ep, VAConfigAttrib *a, int n)
{
    (void)ctx;
    if (!ProfileOk(p)) return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    if (ep != VAEntrypointVLD) return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    for (int i = 0; i < n; i++) {
        if (a[i].type == VAConfigAttribRTFormat) a[i].value = ProfileRt(p);
        else a[i].value = VA_ATTRIB_NOT_SUPPORTED;
    }
    return VA_STATUS_SUCCESS;
}
static VAStatus CreateConfig(VADriverContextP ctx, VAProfile p, VAEntrypoint ep, VAConfigAttrib *a, int n, VAConfigID *id)
{
    drv_t *d = D(ctx);
    if (!ProfileOk(p)) return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    if (ep != VAEntrypointVLD) return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    cfg_t *c = calloc(1, sizeof(*c));
    if (!c) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    c->profile = p; c->ep = ep; c->rt = ProfileRt(p);
    for (int i = 0; i < n; i++)
        if (a[i].type == VAConfigAttribRTFormat && a[i].value) {
            if (!(a[i].value & ProfileRt(p))) { free(c); return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT; }
        }
    pthread_mutex_lock(&d->lock);
    *id = ObjAdd(d, K_CFG, c);
    pthread_mutex_unlock(&d->lock);
    LOG(d, "CreateConfig profile %d -> %#x", p, *id);
    return *id == VA_INVALID_ID ? (free(c), VA_STATUS_ERROR_ALLOCATION_FAILED) : VA_STATUS_SUCCESS;
}
static VAStatus DestroyConfig(VADriverContextP ctx, VAConfigID id)
{
    drv_t *d = D(ctx);
    pthread_mutex_lock(&d->lock);
    cfg_t *c = ObjGet(d, K_CFG, id);
    ObjDel(d, K_CFG, id);
    pthread_mutex_unlock(&d->lock);
    if (!c) return VA_STATUS_ERROR_INVALID_CONFIG;
    free(c);
    return VA_STATUS_SUCCESS;
}
static VAStatus QueryConfigAttributes(VADriverContextP ctx, VAConfigID id, VAProfile *p, VAEntrypoint *ep, VAConfigAttrib *a, int *n)
{
    drv_t *d = D(ctx);
    pthread_mutex_lock(&d->lock);
    cfg_t *c = ObjGet(d, K_CFG, id);
    pthread_mutex_unlock(&d->lock);
    if (!c) return VA_STATUS_ERROR_INVALID_CONFIG;
    *p = c->profile; *ep = c->ep;
    a[0].type = VAConfigAttribRTFormat; a[0].value = c->rt;
    *n = 1;
    return VA_STATUS_SUCCESS;
}

/* surfaces */
static surf_t *SurfNew(drv_t *d, int w, int h, uint32_t fourcc)
{
    surf_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->w = w; s->h = h; s->fourcc = fourcc;
    s->bpc = fourcc == VA_FOURCC_P010 ? 2 : 1;
    const uint32_t row = (uint32_t)w * (uint32_t)s->bpc;
    s->pitch = d->oddpitch ? row + 72 + 6 : (row + 63) & ~63u;      /* odd: not a multiple of 16 */
    const uint32_t hy = (uint32_t)((h + 1) & ~1);
    s->off_uv = s->pitch * hy + (d->oddpitch ? s->pitch * 3 + 40 : 0);
    s->size = s->off_uv + s->pitch * (hy / 2);
    s->data = malloc(s->size);
    if (!s->data) { free(s); return NULL; }
    /* a dark grey, not zero: an untouched surface is visibly different from a decoded one */
    if (s->bpc == 1) { memset(s->data, 16, s->off_uv); memset(s->data + s->off_uv, 128, s->size - s->off_uv); }
    else {
        for (uint32_t i = 0; i + 1 < s->off_uv; i += 2) { s->data[i] = (16 << 6) & 0xff; s->data[i + 1] = (16 << 6) >> 8; }
        for (uint32_t i = s->off_uv; i + 1 < s->size; i += 2) { s->data[i] = (128 << 6) & 0xff; s->data[i + 1] = (128 << 6) >> 8; }
    }
    return s;
}
static VAStatus CreateSurfaces2(VADriverContextP ctx, unsigned format, unsigned w, unsigned h, VASurfaceID *out,
                                unsigned num, VASurfaceAttrib *attr, unsigned nattr)
{
    drv_t *d = D(ctx);
    uint32_t fourcc = format == VA_RT_FORMAT_YUV420_10 ? VA_FOURCC_P010 : VA_FOURCC_NV12;
    if (format != VA_RT_FORMAT_YUV420 && format != VA_RT_FORMAT_YUV420_10) return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    for (unsigned i = 0; i < nattr; i++)
        if (attr[i].type == VASurfaceAttribPixelFormat && (attr[i].flags & VA_SURFACE_ATTRIB_SETTABLE) && attr[i].value.value.i) {
            const uint32_t f = (uint32_t)attr[i].value.value.i;
            if (f != VA_FOURCC_NV12 && f != VA_FOURCC_P010) return VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;
            fourcc = f;
        }
    pthread_mutex_lock(&d->lock);
    for (unsigned i = 0; i < num; i++) {
        surf_t *s = SurfNew(d, (int)w, (int)h, fourcc);
        out[i] = s ? ObjAdd(d, K_SURF, s) : VA_INVALID_ID;
        if (out[i] == VA_INVALID_ID) {
            if (s) { free(s->data); free(s); }
            for (unsigned j = 0; j < i; j++) { surf_t *t = ObjGet(d, K_SURF, out[j]); ObjDel(d, K_SURF, out[j]); if (t) { free(t->data); free(t); } }
            pthread_mutex_unlock(&d->lock);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
    }
    pthread_mutex_unlock(&d->lock);
    LOG(d, "CreateSurfaces %ux%u %.4s x%u", w, h, (const char *)&fourcc, num);
    return VA_STATUS_SUCCESS;
}
static VAStatus CreateSurfaces(VADriverContextP ctx, int w, int h, int format, int num, VASurfaceID *out)
{
    return CreateSurfaces2(ctx, (unsigned)format, (unsigned)w, (unsigned)h, out, (unsigned)num, NULL, 0);
}
static VAStatus DestroySurfaces(VADriverContextP ctx, VASurfaceID *list, int num)
{
    drv_t *d = D(ctx);
    pthread_mutex_lock(&d->lock);
    for (int i = 0; i < num; i++) {
        surf_t *s = ObjGet(d, K_SURF, list[i]);
        ObjDel(d, K_SURF, list[i]);
        if (s) { free(s->data); free(s); }
    }
    pthread_mutex_unlock(&d->lock);
    return VA_STATUS_SUCCESS;
}
static VAStatus QuerySurfaceAttributes(VADriverContextP ctx, VAConfigID cfg, VASurfaceAttrib *a, unsigned *n)
{
    drv_t *d = D(ctx);
    pthread_mutex_lock(&d->lock);
    cfg_t *c = ObjGet(d, K_CFG, cfg);
    pthread_mutex_unlock(&d->lock);
    if (!c) return VA_STATUS_ERROR_INVALID_CONFIG;
    const unsigned want = 6;
    if (!a) { *n = want; return VA_STATUS_SUCCESS; }
    if (*n < want) { *n = want; return VA_STATUS_ERROR_MAX_NUM_EXCEEDED; }
    memset(a, 0, sizeof(*a) * want);
    a[0].type = VASurfaceAttribPixelFormat; a[0].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
    a[0].value.type = VAGenericValueTypeInteger; a[0].value.value.i = c->rt == VA_RT_FORMAT_YUV420_10 ? VA_FOURCC_P010 : VA_FOURCC_NV12;
    a[1].type = VASurfaceAttribMinWidth;  a[1].flags = VA_SURFACE_ATTRIB_GETTABLE; a[1].value.type = VAGenericValueTypeInteger; a[1].value.value.i = 16;
    a[2].type = VASurfaceAttribMinHeight; a[2].flags = VA_SURFACE_ATTRIB_GETTABLE; a[2].value.type = VAGenericValueTypeInteger; a[2].value.value.i = 16;
    a[3].type = VASurfaceAttribMaxWidth;  a[3].flags = VA_SURFACE_ATTRIB_GETTABLE; a[3].value.type = VAGenericValueTypeInteger; a[3].value.value.i = 8192;
    a[4].type = VASurfaceAttribMaxHeight; a[4].flags = VA_SURFACE_ATTRIB_GETTABLE; a[4].value.type = VAGenericValueTypeInteger; a[4].value.value.i = 8192;
    a[5].type = VASurfaceAttribMemoryType; a[5].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
    a[5].value.type = VAGenericValueTypeInteger; a[5].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_VA;
    *n = want;
    return VA_STATUS_SUCCESS;
}

/* contexts */
static VAStatus CreateContext(VADriverContextP ctx, VAConfigID cfg, int w, int h, int flag, VASurfaceID *rt, int nrt, VAContextID *id)
{
    (void)flag; (void)rt; (void)nrt;
    drv_t *d = D(ctx);
    pthread_mutex_lock(&d->lock);
    if (!ObjGet(d, K_CFG, cfg)) { pthread_mutex_unlock(&d->lock); return VA_STATUS_ERROR_INVALID_CONFIG; }
    ctx_t *c = calloc(1, sizeof(*c));
    if (c) { c->cfg = cfg; c->w = w; c->h = h; c->target = VA_INVALID_ID; }
    *id = c ? ObjAdd(d, K_CTX, c) : VA_INVALID_ID;
    pthread_mutex_unlock(&d->lock);
    LOG(d, "CreateContext %dx%d -> %#x", w, h, *id);
    if (*id == VA_INVALID_ID) { free(c); return VA_STATUS_ERROR_ALLOCATION_FAILED; }
    return VA_STATUS_SUCCESS;
}
static VAStatus DestroyContext(VADriverContextP ctx, VAContextID id)
{
    drv_t *d = D(ctx);
    pthread_mutex_lock(&d->lock);
    ctx_t *c = ObjGet(d, K_CTX, id);
    ObjDel(d, K_CTX, id);
    pthread_mutex_unlock(&d->lock);
    if (!c) return VA_STATUS_ERROR_INVALID_CONTEXT;
    LOG(d, "DestroyContext %#x after %llu pictures", id, (unsigned long long)c->pictures);
    free(c);
    return VA_STATUS_SUCCESS;
}

/* buffers */
static VAStatus CreateBuffer(VADriverContextP ctx, VAContextID c, VABufferType type, unsigned size, unsigned num, void *data, VABufferID *id)
{
    (void)c;
    drv_t *d = D(ctx);
    buf_t *b = calloc(1, sizeof(*b));
    if (!b) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    b->type = type; b->size = size; b->num = num ? num : 1; b->owned = 1; b->image = VA_INVALID_ID;
    b->data = malloc((size_t)size * b->num + 1);
    if (!b->data) { free(b); return VA_STATUS_ERROR_ALLOCATION_FAILED; }
    if (data) memcpy(b->data, data, (size_t)size * b->num);
    pthread_mutex_lock(&d->lock);
    *id = ObjAdd(d, K_BUF, b);
    pthread_mutex_unlock(&d->lock);
    if (*id == VA_INVALID_ID) { free(b->data); free(b); return VA_STATUS_ERROR_ALLOCATION_FAILED; }
    return VA_STATUS_SUCCESS;
}
static VAStatus BufferSetNumElements(VADriverContextP ctx, VABufferID id, unsigned num)
{
    drv_t *d = D(ctx);
    pthread_mutex_lock(&d->lock);
    buf_t *b = ObjGet(d, K_BUF, id);
    pthread_mutex_unlock(&d->lock);
    if (!b) return VA_STATUS_ERROR_INVALID_BUFFER;
    if (num > b->num) return VA_STATUS_ERROR_INVALID_PARAMETER;
    b->num = num;
    return VA_STATUS_SUCCESS;
}
static VAStatus MapBuffer(VADriverContextP ctx, VABufferID id, void **p)
{
    drv_t *d = D(ctx);
    pthread_mutex_lock(&d->lock);
    buf_t *b = ObjGet(d, K_BUF, id);
    pthread_mutex_unlock(&d->lock);
    if (!b) return VA_STATUS_ERROR_INVALID_BUFFER;
    if (!b->owned && d->broken) {          /* a copy, as tiled surfaces are de-tiled into a staging buffer */
        free(b->shadow);
        b->shadow = malloc(b->size);
        if (!b->shadow) return VA_STATUS_ERROR_ALLOCATION_FAILED;
        memcpy(b->shadow, b->data, b->size);
        *p = b->shadow;
    } else *p = b->data;
    return VA_STATUS_SUCCESS;
}
static VAStatus UnmapBuffer(VADriverContextP ctx, VABufferID id)
{
    drv_t *d = D(ctx);
    pthread_mutex_lock(&d->lock);
    buf_t *b = ObjGet(d, K_BUF, id);
    pthread_mutex_unlock(&d->lock);
    if (!b) return VA_STATUS_ERROR_INVALID_BUFFER;
    free(b->shadow); b->shadow = NULL;     /* PMNULL_BROKEN: the writes are lost */
    return VA_STATUS_SUCCESS;
}
static VAStatus DestroyBuffer(VADriverContextP ctx, VABufferID id)
{
    drv_t *d = D(ctx);
    pthread_mutex_lock(&d->lock);
    buf_t *b = ObjGet(d, K_BUF, id);
    ObjDel(d, K_BUF, id);
    pthread_mutex_unlock(&d->lock);
    if (!b) return VA_STATUS_ERROR_INVALID_BUFFER;
    if (b->owned) free(b->data);
    free(b->shadow);
    free(b);
    return VA_STATUS_SUCCESS;
}

/* decoding: draw the picture for count n */
static void DrawPattern(surf_t *s, uint64_t n)
{
    const int box = 32;
    int x0 = 16 + (int)(4 * n);
    if (x0 + box > s->w) x0 = (x0 % (s->w - box > 0 ? s->w - box : 1));
    const int y0 = s->h / 2 - box / 2;
    for (int y = 0; y < s->h; y++) {
        uint8_t *row = s->data + (size_t)y * s->pitch;
        for (int x = 0; x < s->w; x++) {
            const int on = y >= y0 && y < y0 + box && x >= x0 && x < x0 + box;
            const unsigned v = on ? 235 : 16;
            if (s->bpc == 1) row[x] = (uint8_t)v;
            else { const unsigned w = v << 6; row[2 * x] = (uint8_t)(w & 0xff); row[2 * x + 1] = (uint8_t)(w >> 8); }
        }
    }
    for (int y = 0; y < (s->h + 1) / 2; y++) {
        uint8_t *row = s->data + s->off_uv + (size_t)y * s->pitch;
        for (int x = 0; x < s->w; x++) {          /* w/2 pairs of (U, V) */
            if (s->bpc == 1) row[x] = 128;
            else { const unsigned w = 128u << 6; row[2 * x] = (uint8_t)(w & 0xff); row[2 * x + 1] = (uint8_t)(w >> 8); }
        }
    }
}
static VAStatus BeginPicture(VADriverContextP ctx, VAContextID id, VASurfaceID target)
{
    drv_t *d = D(ctx);
    pthread_mutex_lock(&d->lock);
    ctx_t *c = ObjGet(d, K_CTX, id);
    surf_t *s = ObjGet(d, K_SURF, target);
    if (c && s) c->target = target;
    pthread_mutex_unlock(&d->lock);
    if (!c) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (!s) return VA_STATUS_ERROR_INVALID_SURFACE;
    return VA_STATUS_SUCCESS;
}
static VAStatus RenderPicture(VADriverContextP ctx, VAContextID id, VABufferID *bufs, int n)
{
    drv_t *d = D(ctx);
    pthread_mutex_lock(&d->lock);
    ctx_t *c = ObjGet(d, K_CTX, id);
    int bad = 0;
    for (int i = 0; i < n; i++) if (!ObjGet(d, K_BUF, bufs[i])) bad = 1;
    pthread_mutex_unlock(&d->lock);
    if (!c) return VA_STATUS_ERROR_INVALID_CONTEXT;
    return bad ? VA_STATUS_ERROR_INVALID_BUFFER : VA_STATUS_SUCCESS;
}
static VAStatus EndPicture(VADriverContextP ctx, VAContextID id)
{
    drv_t *d = D(ctx);
    pthread_mutex_lock(&d->lock);
    ctx_t *c = ObjGet(d, K_CTX, id);
    surf_t *s = c ? ObjGet(d, K_SURF, c->target) : NULL;
    if (s) DrawPattern(s, c->pictures++);
    pthread_mutex_unlock(&d->lock);
    if (!c) return VA_STATUS_ERROR_INVALID_CONTEXT;
    return s ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_SURFACE;
}
static VAStatus SyncSurface(VADriverContextP ctx, VASurfaceID id)
{
    drv_t *d = D(ctx);
    pthread_mutex_lock(&d->lock);
    surf_t *s = ObjGet(d, K_SURF, id);
    pthread_mutex_unlock(&d->lock);
    return s ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_INVALID_SURFACE;
}
static VAStatus QuerySurfaceStatus(VADriverContextP ctx, VASurfaceID id, VASurfaceStatus *st)
{
    *st = VASurfaceReady;
    return SyncSurface(ctx, id);
}
static VAStatus PutSurface(VADriverContextP ctx, VASurfaceID s, void *draw, short sx, short sy, unsigned short sw,
                           unsigned short sh, short dx, short dy, unsigned short dw, unsigned short dh, VARectangle *cr,
                           unsigned ncr, unsigned flags)
{
    (void)ctx; (void)s; (void)draw; (void)sx; (void)sy; (void)sw; (void)sh; (void)dx; (void)dy; (void)dw; (void)dh;
    (void)cr; (void)ncr; (void)flags;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

/* images */
static void FormatFor(uint32_t fourcc, VAImageFormat *f)
{
    memset(f, 0, sizeof(*f));
    f->fourcc = fourcc;
    f->byte_order = VA_LSB_FIRST;
    f->bits_per_pixel = fourcc == VA_FOURCC_P010 ? 24 : 12;
}
static VAStatus QueryImageFormats(VADriverContextP ctx, VAImageFormat *list, int *num)
{
    (void)ctx;
    FormatFor(VA_FOURCC_NV12, &list[0]);
    FormatFor(VA_FOURCC_P010, &list[1]);
    *num = 2;
    return VA_STATUS_SUCCESS;
}
static VAStatus ImageNew(drv_t *d, uint32_t fourcc, int w, int h, uint8_t *alias, const surf_t *like, VAImage *img)
{
    surf_t tmp;
    const surf_t *g = like;
    if (!g) {                                /* layout of a fresh image: as a surface of that size */
        surf_t *s = SurfNew(d, w, h, fourcc);
        if (!s) return VA_STATUS_ERROR_ALLOCATION_FAILED;
        tmp = *s; free(s->data); free(s); g = &tmp;
    }
    buf_t *b = calloc(1, sizeof(*b));
    img_t *im = calloc(1, sizeof(*im));
    if (!b || !im) { free(b); free(im); return VA_STATUS_ERROR_ALLOCATION_FAILED; }
    b->type = VAImageBufferType; b->size = g->size; b->num = 1;
    if (alias) { b->data = alias; b->owned = 0; }
    else {
        b->data = calloc(1, g->size); b->owned = 1;
        if (!b->data) { free(b); free(im); return VA_STATUS_ERROR_ALLOCATION_FAILED; }
    }
    memset(&im->img, 0, sizeof(im->img));
    FormatFor(fourcc, &im->img.format);
    im->img.width = (uint16_t)w; im->img.height = (uint16_t)h;
    im->img.data_size = g->size;
    im->img.num_planes = 2;
    im->img.pitches[0] = im->img.pitches[1] = g->pitch;
    im->img.offsets[0] = 0; im->img.offsets[1] = g->off_uv;
    im->derived_from = VA_INVALID_ID;
    pthread_mutex_lock(&d->lock);
    im->img.buf = ObjAdd(d, K_BUF, b);
    im->img.image_id = ObjAdd(d, K_IMG, im);
    b->image = im->img.image_id;
    pthread_mutex_unlock(&d->lock);
    if (im->img.buf == VA_INVALID_ID || im->img.image_id == VA_INVALID_ID) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    *img = im->img;
    return VA_STATUS_SUCCESS;
}
static VAStatus CreateImage(VADriverContextP ctx, VAImageFormat *f, int w, int h, VAImage *img)
{
    drv_t *d = D(ctx);
    if (f->fourcc != VA_FOURCC_NV12 && f->fourcc != VA_FOURCC_P010) return VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;
    return ImageNew(d, f->fourcc, w, h, NULL, NULL, img);
}
static VAStatus DeriveImage(VADriverContextP ctx, VASurfaceID sid, VAImage *img)
{
    drv_t *d = D(ctx);
    if (!d->derive) return VA_STATUS_ERROR_OPERATION_FAILED;
    pthread_mutex_lock(&d->lock);
    surf_t *s = ObjGet(d, K_SURF, sid);
    pthread_mutex_unlock(&d->lock);
    if (!s) return VA_STATUS_ERROR_INVALID_SURFACE;
    return ImageNew(d, s->fourcc, s->w, s->h, s->data, s, img);
}
static VAStatus DestroyImage(VADriverContextP ctx, VAImageID id)
{
    drv_t *d = D(ctx);
    pthread_mutex_lock(&d->lock);
    img_t *im = ObjGet(d, K_IMG, id);
    ObjDel(d, K_IMG, id);
    pthread_mutex_unlock(&d->lock);
    if (!im) return VA_STATUS_ERROR_INVALID_IMAGE;
    DestroyBuffer(ctx, im->img.buf);
    free(im);
    return VA_STATUS_SUCCESS;
}
static VAStatus SetImagePalette(VADriverContextP ctx, VAImageID id, unsigned char *p)
{
    (void)ctx; (void)id; (void)p;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}
/* copy a w x h region (luma rows, then the interleaved chroma rows) between two NV12/P010 layouts */
static void CopyRegion(uint8_t *dst, uint32_t dpitch, uint32_t doff, const uint8_t *src, uint32_t spitch, uint32_t soff,
                       int sx, int sy, int dx, int dy, int w, int h, int bpc)
{
    for (int y = 0; y < h; y++)
        memcpy(dst + (size_t)(dy + y) * dpitch + (size_t)dx * bpc, src + (size_t)(sy + y) * spitch + (size_t)sx * bpc, (size_t)w * bpc);
    for (int y = 0; y < h / 2; y++)
        memcpy(dst + doff + (size_t)(dy / 2 + y) * dpitch + (size_t)(dx & ~1) * bpc,
               src + soff + (size_t)(sy / 2 + y) * spitch + (size_t)(sx & ~1) * bpc, (size_t)w * bpc);
}
static VAStatus GetImage(VADriverContextP ctx, VASurfaceID sid, int x, int y, unsigned w, unsigned h, VAImageID iid)
{
    drv_t *d = D(ctx);
    if (!d->getimage) return VA_STATUS_ERROR_OPERATION_FAILED;
    pthread_mutex_lock(&d->lock);
    surf_t *s = ObjGet(d, K_SURF, sid);
    img_t *im = ObjGet(d, K_IMG, iid);
    buf_t *b = im ? ObjGet(d, K_BUF, im->img.buf) : NULL;
    VAStatus st = VA_STATUS_SUCCESS;
    if (!s) st = VA_STATUS_ERROR_INVALID_SURFACE;
    else if (!im || !b) st = VA_STATUS_ERROR_INVALID_IMAGE;
    else if (im->img.format.fourcc != s->fourcc) st = VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;
    else if (x < 0 || y < 0 || x + (int)w > s->w || y + (int)h > s->h || (int)w > im->img.width || (int)h > im->img.height)
        st = VA_STATUS_ERROR_INVALID_PARAMETER;
    else CopyRegion(b->data, im->img.pitches[0], im->img.offsets[1], s->data, s->pitch, s->off_uv, x, y, 0, 0, (int)w, (int)h, s->bpc);
    pthread_mutex_unlock(&d->lock);
    return st;
}
static VAStatus PutImage(VADriverContextP ctx, VASurfaceID sid, VAImageID iid, int sx, int sy, unsigned sw, unsigned sh,
                         int dx, int dy, unsigned dw, unsigned dh)
{
    drv_t *d = D(ctx);
    if (!d->putimage) return VA_STATUS_ERROR_OPERATION_FAILED;
    if (sw != dw || sh != dh) return VA_STATUS_ERROR_UNIMPLEMENTED;   /* no scaling */
    pthread_mutex_lock(&d->lock);
    surf_t *s = ObjGet(d, K_SURF, sid);
    img_t *im = ObjGet(d, K_IMG, iid);
    buf_t *b = im ? ObjGet(d, K_BUF, im->img.buf) : NULL;
    VAStatus st = VA_STATUS_SUCCESS;
    if (!s) st = VA_STATUS_ERROR_INVALID_SURFACE;
    else if (!im || !b) st = VA_STATUS_ERROR_INVALID_IMAGE;
    else if (im->img.format.fourcc != s->fourcc) st = VA_STATUS_ERROR_INVALID_IMAGE_FORMAT;
    else if (sx < 0 || sy < 0 || dx < 0 || dy < 0 || sx + (int)sw > im->img.width || sy + (int)sh > im->img.height ||
             dx + (int)dw > s->w || dy + (int)dh > s->h)
        st = VA_STATUS_ERROR_INVALID_PARAMETER;
    else if (!d->broken)
        CopyRegion(s->data, s->pitch, s->off_uv, b->data, im->img.pitches[0], im->img.offsets[1], sx, sy, dx, dy, (int)sw, (int)sh, s->bpc);
    pthread_mutex_unlock(&d->lock);
    return st;
}

/* what libva requires to exist; none of it is used by decoders */
static VAStatus QuerySubpictureFormats(VADriverContextP ctx, VAImageFormat *l, unsigned *flags, unsigned *n)
{ (void)ctx; (void)l; (void)flags; *n = 0; return VA_STATUS_SUCCESS; }
static VAStatus CreateSubpicture(VADriverContextP ctx, VAImageID i, VASubpictureID *s)
{ (void)ctx; (void)i; (void)s; return VA_STATUS_ERROR_UNIMPLEMENTED; }
static VAStatus DestroySubpicture(VADriverContextP ctx, VASubpictureID s)
{ (void)ctx; (void)s; return VA_STATUS_ERROR_UNIMPLEMENTED; }
static VAStatus SetSubpictureImage(VADriverContextP ctx, VASubpictureID s, VAImageID i)
{ (void)ctx; (void)s; (void)i; return VA_STATUS_ERROR_UNIMPLEMENTED; }
static VAStatus SetSubpictureChromakey(VADriverContextP ctx, VASubpictureID s, unsigned a, unsigned b, unsigned c)
{ (void)ctx; (void)s; (void)a; (void)b; (void)c; return VA_STATUS_ERROR_UNIMPLEMENTED; }
static VAStatus SetSubpictureGlobalAlpha(VADriverContextP ctx, VASubpictureID s, float a)
{ (void)ctx; (void)s; (void)a; return VA_STATUS_ERROR_UNIMPLEMENTED; }
static VAStatus AssociateSubpicture(VADriverContextP ctx, VASubpictureID s, VASurfaceID *t, int n, short a, short b,
                                    unsigned short c, unsigned short e, short f, short g, unsigned short h, unsigned short i, unsigned fl)
{ (void)ctx; (void)s; (void)t; (void)n; (void)a; (void)b; (void)c; (void)e; (void)f; (void)g; (void)h; (void)i; (void)fl; return VA_STATUS_ERROR_UNIMPLEMENTED; }
static VAStatus DeassociateSubpicture(VADriverContextP ctx, VASubpictureID s, VASurfaceID *t, int n)
{ (void)ctx; (void)s; (void)t; (void)n; return VA_STATUS_ERROR_UNIMPLEMENTED; }
static VAStatus QueryDisplayAttributes(VADriverContextP ctx, VADisplayAttribute *l, int *n)
{ (void)ctx; (void)l; *n = 0; return VA_STATUS_SUCCESS; }
static VAStatus GetDisplayAttributes(VADriverContextP ctx, VADisplayAttribute *l, int n)
{ (void)ctx; (void)l; (void)n; return VA_STATUS_ERROR_UNIMPLEMENTED; }
static VAStatus SetDisplayAttributes(VADriverContextP ctx, VADisplayAttribute *l, int n)
{ (void)ctx; (void)l; (void)n; return VA_STATUS_ERROR_UNIMPLEMENTED; }

/* libva looks up __vaDriverInit_<major>_<minor> for its own minor version down to 0: export the build's and 1_0 */
#define PM_CAT3(a, b, c) a##b##_##c
#define PM_XCAT3(a, b, c) PM_CAT3(a, b, c)
#define PM_INIT PM_XCAT3(__vaDriverInit_, VA_MAJOR_VERSION, VA_MINOR_VERSION)

__attribute__((visibility("default"))) VAStatus PM_INIT(VADriverContextP ctx);
VAStatus PM_INIT(VADriverContextP ctx)
{
    drv_t *d = calloc(1, sizeof(*d));
    if (!d) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    pthread_mutex_init(&d->lock, NULL);
    d->derive = EnvOn("PMNULL_DERIVE", 1);
    d->putimage = EnvOn("PMNULL_PUTIMAGE", 1);
    d->getimage = EnvOn("PMNULL_GETIMAGE", 1);
    d->broken = EnvOn("PMNULL_BROKEN", 0);
    d->log = EnvOn("PMNULL_LOG", 0);
    const char *pitch = getenv("PMNULL_PITCH");
    d->oddpitch = pitch && !strcmp(pitch, "odd");
    ctx->pDriverData = d;
    ctx->version_major = VA_MAJOR_VERSION;
    ctx->version_minor = VA_MINOR_VERSION;
    ctx->max_profiles = 16;
    ctx->max_entrypoints = 4;
    ctx->max_attributes = 8;
    ctx->max_image_formats = 4;
    ctx->max_subpic_formats = 1;
    ctx->max_display_attributes = 1;
    ctx->str_vendor = "PushMPC null VA-API driver (CI)";
    struct VADriverVTable *v = ctx->vtable;
    v->vaTerminate = Terminate;
    v->vaQueryConfigProfiles = QueryConfigProfiles;
    v->vaQueryConfigEntrypoints = QueryConfigEntrypoints;
    v->vaGetConfigAttributes = GetConfigAttributes;
    v->vaCreateConfig = CreateConfig;
    v->vaDestroyConfig = DestroyConfig;
    v->vaQueryConfigAttributes = QueryConfigAttributes;
    v->vaCreateSurfaces = CreateSurfaces;
    v->vaCreateSurfaces2 = CreateSurfaces2;
    v->vaDestroySurfaces = DestroySurfaces;
    v->vaQuerySurfaceAttributes = QuerySurfaceAttributes;
    v->vaCreateContext = CreateContext;
    v->vaDestroyContext = DestroyContext;
    v->vaCreateBuffer = CreateBuffer;
    v->vaBufferSetNumElements = BufferSetNumElements;
    v->vaMapBuffer = MapBuffer;
    v->vaUnmapBuffer = UnmapBuffer;
    v->vaDestroyBuffer = DestroyBuffer;
    v->vaBeginPicture = BeginPicture;
    v->vaRenderPicture = RenderPicture;
    v->vaEndPicture = EndPicture;
    v->vaSyncSurface = SyncSurface;
    v->vaQuerySurfaceStatus = QuerySurfaceStatus;
    v->vaPutSurface = PutSurface;
    v->vaQueryImageFormats = QueryImageFormats;
    v->vaCreateImage = CreateImage;
    v->vaDeriveImage = DeriveImage;
    v->vaDestroyImage = DestroyImage;
    v->vaSetImagePalette = SetImagePalette;
    v->vaGetImage = GetImage;
    v->vaPutImage = PutImage;
    v->vaQuerySubpictureFormats = QuerySubpictureFormats;
    v->vaCreateSubpicture = CreateSubpicture;
    v->vaDestroySubpicture = DestroySubpicture;
    v->vaSetSubpictureImage = SetSubpictureImage;
    v->vaSetSubpictureChromakey = SetSubpictureChromakey;
    v->vaSetSubpictureGlobalAlpha = SetSubpictureGlobalAlpha;
    v->vaAssociateSubpicture = AssociateSubpicture;
    v->vaDeassociateSubpicture = DeassociateSubpicture;
    v->vaQueryDisplayAttributes = QueryDisplayAttributes;
    v->vaGetDisplayAttributes = GetDisplayAttributes;
    v->vaSetDisplayAttributes = SetDisplayAttributes;
    LOG(d, "init (libva %d.%d), derive %d, putimage %d, getimage %d, odd pitch %d, broken %d", VA_MAJOR_VERSION,
        VA_MINOR_VERSION, d->derive, d->putimage, d->getimage, d->oddpitch, d->broken);
    return VA_STATUS_SUCCESS;
}
#if VA_MINOR_VERSION != 0
__attribute__((visibility("default"))) VAStatus PM_XCAT3(__vaDriverInit_, VA_MAJOR_VERSION, 0)(VADriverContextP ctx);
VAStatus PM_XCAT3(__vaDriverInit_, VA_MAJOR_VERSION, 0)(VADriverContextP ctx) { return PM_INIT(ctx); }
#endif
