/*
 * pm_cvpx.c: see pm_cvpx.h. macOS only.
 *
 * The CVPX picture context below mirrors VLC 3.0.x's private struct cvpxpic_ctx (modules/codec/vt_utils.c,
 * Copyright (C) 2017 VLC authors and VideoLAN, LGPL-2.1-or-later; picture_context_t is VLC's public type from
 * vlc_picture.h). VLC's own CVPX modules read and write pictures through it (cvpxpic_get_ref / cvpxpic_attach); it is
 * not part of libvlccore's exported API, so build-mac-extras.sh compares this definition with vt_utils.c at every VLC
 * tag in the allow-list and the build fails when they differ.
 */
#ifdef __APPLE__
#include "pm_cvpx.h"
#include "pm_comb.h"
#include <CoreFoundation/CoreFoundation.h>
#include <CoreVideo/CoreVideo.h>
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PM_CVPX_LAYOUT_BEGIN (compared with VLC's vt_utils.c by build-mac-extras.sh) */
struct cvpxpic_ctx
{
    picture_context_t s;
    CVPixelBufferRef cvpx;
    unsigned nb_fields;

    atomic_uint ref_count;
    void (*on_released_cb)(CVPixelBufferRef, void *, unsigned);
    void *on_released_data;
};
/* PM_CVPX_LAYOUT_END */

/* the VLC versions the layout was checked for (build-mac-extras.sh PM_CVPX_TAGS) */
#define PM_CVPX_MIN_REV 20
#define PM_CVPX_MAX_REV 24

/* VLC 3.0's libvlccore exports no version call (VLC_Version is not in its libvlccore.sym); the host's libvlc has
 * libvlc_get_version() ("3.0.24 Vetinari"), and every VLC 3 player has libvlc loaded before it loads modules */
bool pm_cvpx_vlc_ok(char *why, size_t n)
{
    const char *(*ver)(void) = (const char *(*)(void))dlsym(RTLD_DEFAULT, "libvlc_get_version");
    const char *v = ver ? ver() : NULL;
    int a = 0, b = 0, c = 0;
    if (!v || sscanf(v, "%d.%d.%d", &a, &b, &c) != 3) { snprintf(why, n, "VLC version unknown"); return false; }
    if (a != 3 || b != 0 || c < PM_CVPX_MIN_REV || c > PM_CVPX_MAX_REV) {
        snprintf(why, n, "VLC %d.%d.%d is outside the tested range 3.0.%d-3.0.%d", a, b, c, PM_CVPX_MIN_REV, PM_CVPX_MAX_REV);
        return false;
    }
    return true;
}

int pm_cvpx_bits(vlc_fourcc_t chroma)
{
    if (chroma == VLC_CODEC_CVPX_NV12) return 8;
#ifdef VLC_CODEC_CVPX_P010
    if (chroma == VLC_CODEC_CVPX_P010) return 10;
#endif
    return 0;
}

static bool FormatMatches(vlc_fourcc_t chroma, OSType t)
{
    if (chroma == VLC_CODEC_CVPX_NV12)
        return t == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange || t == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
#ifdef VLC_CODEC_CVPX_P010
    if (chroma == VLC_CODEC_CVPX_P010)
        return t == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange || t == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;
#endif
    return false;
}

void *pm_cvpx_in(const picture_t *p, const video_format_t *f, const char **why)
{
    if (!p->context) { *why = "the picture has no CVPX context"; return NULL; }
    const struct cvpxpic_ctx *ctx = (const struct cvpxpic_ctx *)p->context;
    CVPixelBufferRef pb = ctx->cvpx;
    if (!pb) { *why = "no CVPixelBuffer"; return NULL; }
    if (CFGetTypeID(pb) != CVPixelBufferGetTypeID()) { *why = "not a CVPixelBuffer (the picture layout differs)"; return NULL; }
    const size_t w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb);
    if ((w != f->i_width && w != f->i_visible_width) || (h != f->i_height && h != f->i_visible_height)) { *why = "CVPixelBuffer size differs from the picture's"; return NULL; }
    if (!FormatMatches(f->i_chroma, CVPixelBufferGetPixelFormatType(pb))) { *why = "CVPixelBuffer pixel format differs from the picture's"; return NULL; }
    return (void *)pb;
}

/* our own contexts for generated pictures, in VLC's layout (VLC's CVPX readers take ->cvpx) */
static void CtxDestroy(picture_context_t *c)
{
    struct cvpxpic_ctx *ctx = (struct cvpxpic_ctx *)c;
    if (atomic_fetch_sub(&ctx->ref_count, 1) == 1) {
        CVPixelBufferRelease(ctx->cvpx);
        free(ctx);
    }
}
static picture_context_t *CtxCopy(picture_context_t *c)
{
    struct cvpxpic_ctx *ctx = (struct cvpxpic_ctx *)c;
    atomic_fetch_add(&ctx->ref_count, 1);
    return &ctx->s;
}

picture_t *pm_cvpx_out(const video_format_t *f, void *pb)
{
    struct cvpxpic_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) { CVPixelBufferRelease((CVPixelBufferRef)pb); return NULL; }
    ctx->s.destroy = CtxDestroy;
    ctx->s.copy = CtxCopy;
    ctx->cvpx = (CVPixelBufferRef)pb;   /* owned: released with the last reference */
    ctx->nb_fields = 1;
    atomic_init(&ctx->ref_count, 1);
    picture_resource_t res;
    memset(&res, 0, sizeof(res));
    picture_t *p = picture_NewFromResource(f, &res);
    if (!p) { CtxDestroy(&ctx->s); return NULL; }
    p->context = &ctx->s;
    return p;
}

void pm_cvpx_release(void *pb) { if (pb) CVPixelBufferRelease((CVPixelBufferRef)pb); }

/* ---- the apple engine (libpushmpc_engine_apple.dylib, pme_apple_cvpx_*) */
struct pm_cvpx_eng {
    void *h;
    int (*process)(void *, void *, int64_t, int, void **, int, int *, int *, char *, size_t);
    void (*forget)(void *);
    const char *(*describe)(void *);
    void (*close)(void *);
};

pm_cvpx_eng *pm_cvpx_eng_open(const video_format_t *f, int bits, int smoother, double fps, double hz, int *k, char *why, size_t n)
{
    Dl_info di;
    char path[1024] = "";
    if (dladdr((void *)&pm_cvpx_eng_open, &di) && di.dli_fname) {
        snprintf(path, sizeof(path), "%s", di.dli_fname);
        char *slash = strrchr(path, '/');
        if (slash) slash[1] = 0; else path[0] = 0;
    }
    strncat(path, "libpushmpc_engine_apple.dylib", sizeof(path) - strlen(path) - 1);
    void *m = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!m) { const char *d = dlerror(); snprintf(why, n, "apple engine not loaded (%s)", d ? d : "?"); return NULL; }
    void *(*op)(int, int, int, int, double, double, int, int, int, int, int *, char *, size_t) =
        (void *(*)(int, int, int, int, double, double, int, int, int, int, int *, char *, size_t))dlsym(m, "pme_apple_cvpx_open");
    pm_cvpx_eng *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->process = (int (*)(void *, void *, int64_t, int, void **, int, int *, int *, char *, size_t))dlsym(m, "pme_apple_cvpx_process");
    e->forget = (void (*)(void *))dlsym(m, "pme_apple_cvpx_forget");
    e->describe = (const char *(*)(void *))dlsym(m, "pme_apple_cvpx_describe");
    e->close = (void (*)(void *))dlsym(m, "pme_apple_cvpx_close");
    if (!op || !e->process || !e->forget || !e->describe || !e->close) { snprintf(why, n, "apple engine: no CVPixelBuffer interface"); free(e); return NULL; }
    const int full = f->b_color_range_full ? 1 : 0;   /* VLC 3.0's video_format_t field */
    /* VLC's colour enums -> H.273 codes for the apple engine */
    const int matrix = f->space == COLOR_SPACE_BT2020 ? 9 : f->space == COLOR_SPACE_BT601 ? 6 : 1;
    const int transfer = f->transfer == TRANSFER_FUNC_SMPTE_ST2084 ? 16 : f->transfer == TRANSFER_FUNC_HLG ? 18 : 1;
    const int primaries = f->primaries == COLOR_PRIMARIES_BT2020 ? 9 : 1;
    e->h = op((int)f->i_visible_width, (int)f->i_visible_height, bits, smoother, fps, hz, full, matrix, transfer, primaries, k, why, n);
    if (!e->h) { free(e); return NULL; }
    return e;
}

int pm_cvpx_eng_process(pm_cvpx_eng *e, void *pb, int64_t pts100ns, bool contiguous, void **gen, int max, int *ngen, bool *pair,
                        char *err, size_t n)
{
    int hp = 0;
    const int r = e->process(e->h, pb, pts100ns, contiguous ? 1 : 0, gen, max, ngen, &hp, err, n);
    *pair = hp != 0;
    return r;
}
void pm_cvpx_eng_forget(pm_cvpx_eng *e) { e->forget(e->h); }
const char *pm_cvpx_eng_describe(pm_cvpx_eng *e) { return e->describe(e->h); }
void pm_cvpx_eng_close(pm_cvpx_eng *e) { if (e) { e->close(e->h); free(e); } }

int pm_cvpx_comb_push(void *pb, void *comb)
{
    CVPixelBufferRef b = (CVPixelBufferRef)pb;
    if (!b || !comb || CVPixelBufferGetPlaneCount(b) != 2 || CVPixelBufferLockBaseAddress(b, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return -1;
    const OSType t = CVPixelBufferGetPixelFormatType(b);
    const int p10 = t == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange || t == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;
    /* P010-style words: the sample in the high bits */
    const int v = pm_comb_push(comb, CVPixelBufferGetBaseAddressOfPlane(b, 0), (ptrdiff_t)CVPixelBufferGetBytesPerRowOfPlane(b, 0),
                               (int)CVPixelBufferGetWidth(b), (int)CVPixelBufferGetHeight(b), p10 ? 2 : 1, p10 ? 8 : 0);
    CVPixelBufferUnlockBaseAddress(b, kCVPixelBufferLock_ReadOnly);
    return v;
}
#endif
