/*
 * pm_hw.c: PushMPC's hardware-format path for VLC 3 on Linux (VA-API, VDPAU). See pm_hw.h.
 * Licence: GPL-3.0-or-later.
 */
#ifndef _GNU_SOURCE
# define _GNU_SOURCE   /* dladdr */
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pm_hw.h"

#if defined(__linux__) || defined(__FreeBSD__)
#include <dlfcn.h>
#include <stdatomic.h>
#include <vlc_picture_pool.h>
#include "pm_hw_va.h"

enum { HW_VA = 1, HW_VDPAU = 2 };
#define MAX_SLOTS 48

/* ---- libva, looked up at run time */
static struct {
    bool tried, ok;
    pmVAStatus (*DeriveImage)(pmVADisplay, pmVAGenericID, pmVAImage *);
    pmVAStatus (*CreateImage)(pmVADisplay, pmVAImageFormat *, int, int, pmVAImage *);
    pmVAStatus (*GetImage)(pmVADisplay, pmVAGenericID, int, int, unsigned, unsigned, pmVAGenericID);
    pmVAStatus (*PutImage)(pmVADisplay, pmVAGenericID, pmVAGenericID, int, int, unsigned, unsigned, int, int, unsigned, unsigned);
    pmVAStatus (*DestroyImage)(pmVADisplay, pmVAGenericID);
    pmVAStatus (*MapBuffer)(pmVADisplay, pmVAGenericID, void **);
    pmVAStatus (*UnmapBuffer)(pmVADisplay, pmVAGenericID);
    pmVAStatus (*SyncSurface)(pmVADisplay, pmVAGenericID);
    pmVAStatus (*CreateSurfaces)(pmVADisplay, unsigned, unsigned, unsigned, pmVAGenericID *, unsigned, void *, unsigned);
    pmVAStatus (*DestroySurfaces)(pmVADisplay, pmVAGenericID *, int);
    pmVAStatus (*QueryImageFormats)(pmVADisplay, pmVAImageFormat *, int *);
    int (*MaxNumImageFormats)(pmVADisplay);
    const char *(*QueryVendorString)(pmVADisplay);
    const char *(*ErrorStr)(pmVAStatus);
} va;

/* ---- VLC's libvlc_vdpau (hw/vdpau/vlc_vdpau.h), already loaded when VDPAU pictures exist */
static struct {
    bool tried, ok;
    int (*SurfaceCreate)(const void *vdp, uint32_t dev, uint32_t type, uint32_t w, uint32_t h, uint32_t *surf);
    int (*SurfaceDestroy)(const void *vdp, uint32_t surf);
    int (*SurfaceGetParameters)(const void *vdp, uint32_t surf, uint32_t *type, uint32_t *w, uint32_t *h);
    int (*GetBits)(const void *vdp, uint32_t surf, uint32_t fmt, void *const *data, const uint32_t *pitch);
    int (*PutBits)(const void *vdp, uint32_t surf, uint32_t fmt, const void *const *data, const uint32_t *pitch);
    void *(*Hold)(void *vdp, uint32_t *dev);
    void (*Release)(void *vdp);
} vdp;
#define PM_VDP_CHROMA_TYPE_420 0u
#define PM_VDP_YCBCR_FORMAT_YV12 1u
#define PM_VDP_PICTURE_STRUCTURE_FRAME 2u

static vlc_mutex_t load_lock = VLC_STATIC_MUTEX;

static void *Sym(void *h, const char *name)
{
    void *p = h ? dlsym(h, name) : NULL;
    return p ? p : dlsym(RTLD_DEFAULT, name);
}
static bool LoadVa(void)
{
    vlc_mutex_lock(&load_lock);
    if (!va.tried) {
        va.tried = true;
        void *h = dlopen("libva.so.2", RTLD_NOW | RTLD_NOLOAD);
        if (!h) h = dlopen("libva.so.2", RTLD_NOW);
        if (h) {
#define L(f, n) (*(void **)&va.f = Sym(h, n))
            va.ok = L(DeriveImage, "vaDeriveImage") && L(CreateImage, "vaCreateImage") && L(GetImage, "vaGetImage") &&
                    L(PutImage, "vaPutImage") && L(DestroyImage, "vaDestroyImage") && L(MapBuffer, "vaMapBuffer") &&
                    L(UnmapBuffer, "vaUnmapBuffer") && L(SyncSurface, "vaSyncSurface") &&
                    L(CreateSurfaces, "vaCreateSurfaces") && L(DestroySurfaces, "vaDestroySurfaces") &&
                    L(QueryImageFormats, "vaQueryImageFormats") && L(MaxNumImageFormats, "vaMaxNumImageFormats") &&
                    L(QueryVendorString, "vaQueryVendorString") && L(ErrorStr, "vaErrorStr");
#undef L
        }
    }
    const bool ok = va.ok;
    vlc_mutex_unlock(&load_lock);
    return ok;
}
static bool LoadVdp(void)
{
    vlc_mutex_lock(&load_lock);
    if (!vdp.tried) {
        vdp.tried = true;
        void *h = dlopen("libvlc_vdpau.so.0", RTLD_NOW | RTLD_NOLOAD);
#define L(f, n) (*(void **)&vdp.f = Sym(h, n))
        vdp.ok = L(SurfaceCreate, "vdp_video_surface_create") && L(SurfaceDestroy, "vdp_video_surface_destroy") &&
                 L(SurfaceGetParameters, "vdp_video_surface_get_parameters") &&
                 L(GetBits, "vdp_video_surface_get_bits_y_cb_cr") && L(PutBits, "vdp_video_surface_put_bits_y_cb_cr") &&
                 L(Hold, "vdp_hold_x11") && L(Release, "vdp_release_x11");
#undef L
    }
    const bool ok = vdp.ok;
    vlc_mutex_unlock(&load_lock);
    return ok;
}

/* ---- VLC 3.0.x layouts (modules/hw/vaapi/vlc_vaapi.c, modules/hw/vdpau/vlc_vdpau.h) */
struct pm_va_instance {          /* struct pic_sys_vaapi_instance (VLC reads va_dpy) */
    atomic_int pic_refcount;
    pmVADisplay va_dpy;
    void *va_inst;
    unsigned num_render_targets;
};
struct pm_va_picsys {            /* struct picture_sys_t of VLC's VA-API pools */
    struct pm_va_instance *instance;
    struct { picture_context_t s; pmVAGenericID surface; picture_t *picref; } ctx;
};
typedef struct pm_hw pm_hw;
typedef struct pm_slot pm_slot;
typedef struct {                 /* struct vaapi_pic_ctx { picture_context_t s; VASurfaceID surface; picture_t *picref; } */
    picture_context_t s;
    pmVAGenericID surface;
    picture_t *picref;
    pm_hw *h;                    /* ours, after VLC's part */
    pm_slot *slot;
} pm_va_ctx;
typedef struct {                 /* vlc_vdp_video_frame_t */
    uint32_t surface;
    uint32_t device;
    void *vdp;
    atomic_uintptr_t refs;
} pm_vdp_frame;
typedef struct {                 /* vlc_vdp_video_field_t */
    picture_context_t context;
    pm_vdp_frame *frame;
    uint32_t structure;
    struct { uint32_t struct_version; float brightness, contrast, saturation, hue; } procamp;
    float sharpen;
    pm_hw *h;                    /* ours */
    pm_slot *slot;
} pm_vdp_field;

struct pm_slot {
    uint32_t surface;
    atomic_uint refs;            /* output pictures (and context copies) using this surface */
    pm_vdp_frame frame;          /* VDPAU: what our fields point to */
};

struct pm_hw {
    vlc_object_t *obj;
    int kind;
    video_format_t fmt, wfmt;
    int bits, pool_size;
    atomic_int refs;             /* the filter + each busy slot */
    vlc_mutex_t lock;
    picture_pool_t *cpu_pool;
    int nslots;
    pm_slot slots[MAX_SLOTS];
    uint32_t sw, sh;             /* surface size */
    /* VA-API */
    pmVADisplay dpy;
    struct pm_va_instance inst;
    struct pm_va_picsys picsys;
    picture_t *refpic;           /* picref of our contexts: its p_sys leads VLC to the VADisplay */
    picture_context_t *anchor;   /* a reference on the decoder's newest picture: keeps its VADisplay alive */
    uint32_t va_fourcc, va_rt;
    pmVAImageFormat va_ifmt;
    bool up_derive, down_derive;
    pmVAImage dl_img, ul_img;
    bool dl_img_ok, ul_img_ok;
    vlc_mutex_t ul_lock;
    /* VDPAU */
    void *vdp;
    uint32_t device;
    char desc[200];
};

bool pm_hw_chroma(vlc_fourcc_t c, int *bits)
{
    switch (c) {
    case VLC_CODEC_VAAPI_420: case VLC_CODEC_VDPAU_VIDEO_420: *bits = 8; return true;
    case VLC_CODEC_VAAPI_420_10BPP: *bits = 10; return true;
    default: return false;
    }
}

static const char *ModuleOf(const void *fn)
{
    Dl_info di;
    if (!fn || !dladdr(fn, &di) || !di.dli_fname) return "";
    return di.dli_fname;
}

pm_hw *pm_hw_new(vlc_object_t *o, const video_format_t *fmt, int pool_size)
{
    int bits;
    if (!pm_hw_chroma(fmt->i_chroma, &bits)) return NULL;
    pm_hw *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->obj = o;
    h->kind = fmt->i_chroma == VLC_CODEC_VDPAU_VIDEO_420 ? HW_VDPAU : HW_VA;
    h->fmt = *fmt;
    h->bits = bits;
    h->pool_size = pool_size < 4 ? 4 : pool_size > MAX_SLOTS ? MAX_SLOTS : pool_size;
    atomic_init(&h->refs, 1);
    vlc_mutex_init(&h->lock);
    vlc_mutex_init(&h->ul_lock);
    h->wfmt = *fmt;
    h->wfmt.i_chroma = bits > 8 ? VLC_CODEC_I420_10L : VLC_CODEC_I420;
    for (int i = 0; i < MAX_SLOTS; i++) h->slots[i].surface = PM_VA_INVALID_ID;
    snprintf(h->desc, sizeof(h->desc), "%s (not bound)", h->kind == HW_VA ? "VA-API" : "VDPAU");
    return h;
}

/* ---- pool lifetime */
static void Destroy(pm_hw *h)
{
    if (h->kind == HW_VA) {
        for (int i = 0; i < h->nslots; i++)
            if (h->slots[i].surface != PM_VA_INVALID_ID) va.DestroySurfaces(h->dpy, &h->slots[i].surface, 1);
        if (h->dl_img_ok) va.DestroyImage(h->dpy, h->dl_img.image_id);
        if (h->ul_img_ok) va.DestroyImage(h->dpy, h->ul_img.image_id);
        if (h->refpic) picture_Release(h->refpic);
        if (h->anchor) h->anchor->destroy(h->anchor);   /* last: the decoder's VADisplay may go with it */
    } else if (h->kind == HW_VDPAU && h->vdp) {
        for (int i = 0; i < h->nslots; i++) vdp.SurfaceDestroy(h->vdp, h->slots[i].surface);
        vdp.Release(h->vdp);
    }
    vlc_mutex_destroy(&h->ul_lock);
    vlc_mutex_destroy(&h->lock);
    free(h);
}
static void Unref(pm_hw *h) { if (atomic_fetch_sub(&h->refs, 1) == 1) Destroy(h); }
static void SlotRef(pm_slot *s) { atomic_fetch_add(&s->refs, 1); }
static void SlotUnref(pm_hw *h, pm_slot *s) { if (atomic_fetch_sub(&s->refs, 1) == 1) Unref(h); }

void pm_hw_release(pm_hw *h)
{
    if (!h) return;
    vlc_mutex_lock(&h->lock);
    picture_pool_t *p = h->cpu_pool;
    h->cpu_pool = NULL;
    vlc_mutex_unlock(&h->lock);
    if (p) picture_pool_Release(p);
    Unref(h);
}

/* ---- contexts of our output pictures */
static void VaCtxDestroy(picture_context_t *c)
{
    pm_va_ctx *x = (pm_va_ctx *)c;
    pm_hw *h = x->h; pm_slot *s = x->slot;
    free(x);
    SlotUnref(h, s);
}
static picture_context_t *VaCtxCopy(picture_context_t *c)
{
    pm_va_ctx *x = (pm_va_ctx *)c, *n = malloc(sizeof(*n));
    if (!n) return NULL;
    *n = *x;
    SlotRef(x->slot);
    return &n->s;
}
static void VdpCtxDestroy(picture_context_t *c)
{
    pm_vdp_field *x = (pm_vdp_field *)c;
    pm_hw *h = x->h; pm_slot *s = x->slot;
    free(x);
    SlotUnref(h, s);
}
static picture_context_t *VdpCtxCopy(picture_context_t *c)
{
    pm_vdp_field *x = (pm_vdp_field *)c, *n = malloc(sizeof(*n));
    if (!n) return NULL;
    *n = *x;
    SlotRef(x->slot);
    return &n->context;
}

picture_t *pm_hw_new_picture(pm_hw *h)
{
    pm_slot *s = NULL;
    vlc_mutex_lock(&h->lock);
    for (int i = 0; i < h->nslots; i++) {
        unsigned zero = 0;
        if (atomic_compare_exchange_strong(&h->slots[i].refs, &zero, 1)) { s = &h->slots[i]; break; }
    }
    vlc_mutex_unlock(&h->lock);
    if (!s) return NULL;
    atomic_fetch_add(&h->refs, 1);       /* a busy slot keeps the pool */
    picture_context_t *ctx;
    if (h->kind == HW_VA) {
        pm_va_ctx *x = malloc(sizeof(*x));
        if (!x) { SlotUnref(h, s); return NULL; }
        x->s.destroy = VaCtxDestroy; x->s.copy = VaCtxCopy;
        x->surface = s->surface; x->picref = h->refpic; x->h = h; x->slot = s;
        ctx = &x->s;
    } else {
        pm_vdp_field *x = calloc(1, sizeof(*x));
        if (!x) { SlotUnref(h, s); return NULL; }
        x->context.destroy = VdpCtxDestroy; x->context.copy = VdpCtxCopy;
        x->frame = &s->frame;
        x->structure = PM_VDP_PICTURE_STRUCTURE_FRAME;
        x->procamp.struct_version = 0; x->procamp.contrast = 1.f; x->procamp.saturation = 1.f;
        x->h = h; x->slot = s;
        ctx = &x->context;
    }
    picture_resource_t rsc;
    memset(&rsc, 0, sizeof(rsc));
    picture_t *pic = picture_NewFromResource(&h->fmt, &rsc);
    if (!pic) { ctx->destroy(ctx); return NULL; }
    pic->context = ctx;
    return pic;
}

static pm_slot *SlotOf(pm_hw *h, picture_t *pic)
{
    if (!pic->context) return NULL;
    if (h->kind == HW_VA && pic->context->destroy == VaCtxDestroy) return ((pm_va_ctx *)pic->context)->slot;
    if (h->kind == HW_VDPAU && pic->context->destroy == VdpCtxDestroy) return ((pm_vdp_field *)pic->context)->slot;
    return NULL;
}

/* ---- CPU pictures (work format) */
const video_format_t *pm_hw_work_format(const pm_hw *h) { return &h->wfmt; }
picture_t *pm_hw_cpu_picture(pm_hw *h)
{
    vlc_mutex_lock(&h->lock);
    picture_t *p = h->cpu_pool ? picture_pool_Get(h->cpu_pool) : NULL;
    vlc_mutex_unlock(&h->lock);
    return p ? p : picture_NewFromFormat(&h->wfmt);
}

/* ---- semi-planar <-> planar copies (NV12 <-> I420, P010 <-> I420_10L) */
static void SpToP(picture_t *dst, const uint8_t *y, size_t py, const uint8_t *uv, size_t puv, int w, int hgt, int bits)
{
    const int cw = (w + 1) / 2, ch = (hgt + 1) / 2;
    if (bits == 8) {
        for (int r = 0; r < hgt; r++) memcpy(dst->p[0].p_pixels + (size_t)r * dst->p[0].i_pitch, y + (size_t)r * py, (size_t)w);
        for (int r = 0; r < ch; r++) {
            const uint8_t *s = uv + (size_t)r * puv;
            uint8_t *u = dst->p[1].p_pixels + (size_t)r * dst->p[1].i_pitch, *v = dst->p[2].p_pixels + (size_t)r * dst->p[2].i_pitch;
            for (int x = 0; x < cw; x++) { u[x] = s[2 * x]; v[x] = s[2 * x + 1]; }
        }
    } else {
        for (int r = 0; r < hgt; r++) {
            const uint16_t *s = (const uint16_t *)(y + (size_t)r * py);
            uint16_t *d = (uint16_t *)(dst->p[0].p_pixels + (size_t)r * dst->p[0].i_pitch);
            for (int x = 0; x < w; x++) d[x] = s[x] >> 6;
        }
        for (int r = 0; r < ch; r++) {
            const uint16_t *s = (const uint16_t *)(uv + (size_t)r * puv);
            uint16_t *u = (uint16_t *)(dst->p[1].p_pixels + (size_t)r * dst->p[1].i_pitch);
            uint16_t *v = (uint16_t *)(dst->p[2].p_pixels + (size_t)r * dst->p[2].i_pitch);
            for (int x = 0; x < cw; x++) { u[x] = s[2 * x] >> 6; v[x] = s[2 * x + 1] >> 6; }
        }
    }
}
static void PToSp(const picture_t *src, uint8_t *y, size_t py, uint8_t *uv, size_t puv, int w, int hgt, int bits)
{
    const int cw = (w + 1) / 2, ch = (hgt + 1) / 2;
    if (bits == 8) {
        for (int r = 0; r < hgt; r++) memcpy(y + (size_t)r * py, src->p[0].p_pixels + (size_t)r * src->p[0].i_pitch, (size_t)w);
        for (int r = 0; r < ch; r++) {
            uint8_t *d = uv + (size_t)r * puv;
            const uint8_t *u = src->p[1].p_pixels + (size_t)r * src->p[1].i_pitch, *v = src->p[2].p_pixels + (size_t)r * src->p[2].i_pitch;
            for (int x = 0; x < cw; x++) { d[2 * x] = u[x]; d[2 * x + 1] = v[x]; }
        }
    } else {
        for (int r = 0; r < hgt; r++) {
            uint16_t *d = (uint16_t *)(y + (size_t)r * py);
            const uint16_t *s = (const uint16_t *)(src->p[0].p_pixels + (size_t)r * src->p[0].i_pitch);
            for (int x = 0; x < w; x++) d[x] = (uint16_t)(s[x] << 6);
        }
        for (int r = 0; r < ch; r++) {
            uint16_t *d = (uint16_t *)(uv + (size_t)r * puv);
            const uint16_t *u = (const uint16_t *)(src->p[1].p_pixels + (size_t)r * src->p[1].i_pitch);
            const uint16_t *v = (const uint16_t *)(src->p[2].p_pixels + (size_t)r * src->p[2].i_pitch);
            for (int x = 0; x < cw; x++) { d[2 * x] = (uint16_t)(u[x] << 6); d[2 * x + 1] = (uint16_t)(v[x] << 6); }
        }
    }
}

/* ---- VA-API transfers */
static int VaSurfaceOf(picture_t *pic, pmVAGenericID *surf)
{
    if (!pic->context) return -1;
    *surf = ((pm_va_ctx *)pic->context)->surface;   /* VLC's vaapi_pic_ctx and ours share this prefix */
    return 0;
}
static int VaImageFor(pm_hw *h, pmVAImage *img, bool *ok)
{
    if (*ok) return 0;
    if (va.CreateImage(h->dpy, &h->va_ifmt, (int)h->sw, (int)h->sh, img) != 0) return -1;
    *ok = true;
    return 0;
}
static int VaDownload(pm_hw *h, pmVAGenericID surf, picture_t *dst)
{
    if (va.SyncSurface(h->dpy, surf) != 0) return -1;
    pmVAImage img;
    bool derived = false;
    if (h->down_derive) {
        if (va.DeriveImage(h->dpy, surf, &img) != 0) return -1;
        derived = true;
    } else {
        if (VaImageFor(h, &h->dl_img, &h->dl_img_ok) != 0) return -1;
        if (va.GetImage(h->dpy, surf, 0, 0, h->sw, h->sh, h->dl_img.image_id) != 0) return -1;
        img = h->dl_img;
    }
    int r = -1;
    if (img.format.fourcc == h->va_fourcc) {
        void *p = NULL;
        if (va.MapBuffer(h->dpy, img.buf, &p) == 0 && p) {
            const int w = (int)(img.width < h->wfmt.i_width ? img.width : h->wfmt.i_width);
            const int hh = (int)(img.height < h->wfmt.i_height ? img.height : h->wfmt.i_height);
            SpToP(dst, (const uint8_t *)p + img.offsets[0], img.pitches[0], (const uint8_t *)p + img.offsets[1], img.pitches[1], w, hh, h->bits);
            va.UnmapBuffer(h->dpy, img.buf);
            r = 0;
        }
    }
    if (derived) va.DestroyImage(h->dpy, img.image_id);
    return r;
}
static int VaUpload(pm_hw *h, const picture_t *src, pmVAGenericID surf)
{
    pmVAImage img;
    if (h->up_derive) {
        if (va.DeriveImage(h->dpy, surf, &img) != 0) return -1;
    } else {
        if (VaImageFor(h, &h->ul_img, &h->ul_img_ok) != 0) return -1;
        img = h->ul_img;
    }
    int r = -1;
    void *p = NULL;
    if (img.format.fourcc == h->va_fourcc && va.MapBuffer(h->dpy, img.buf, &p) == 0 && p) {
        const int w = (int)(img.width < h->wfmt.i_width ? img.width : h->wfmt.i_width);
        const int hh = (int)(img.height < h->wfmt.i_height ? img.height : h->wfmt.i_height);
        PToSp(src, (uint8_t *)p + img.offsets[0], img.pitches[0], (uint8_t *)p + img.offsets[1], img.pitches[1], w, hh, h->bits);
        r = va.UnmapBuffer(h->dpy, img.buf) == 0 ? 0 : -1;
        if (r == 0 && !h->up_derive)
            r = va.PutImage(h->dpy, surf, img.image_id, 0, 0, h->sw, h->sh, 0, 0, h->sw, h->sh) == 0 ? 0 : -1;
    }
    if (h->up_derive) va.DestroyImage(h->dpy, img.image_id);
    return r;
}

/* ---- VDPAU transfers (YV12 = Y, V, U planes) */
static int VdpDownload(pm_hw *h, uint32_t surf, picture_t *dst)
{
    void *planes[3] = { dst->p[0].p_pixels, dst->p[2].p_pixels, dst->p[1].p_pixels };
    const uint32_t pitches[3] = { (uint32_t)dst->p[0].i_pitch, (uint32_t)dst->p[2].i_pitch, (uint32_t)dst->p[1].i_pitch };
    return vdp.GetBits(h->vdp, surf, PM_VDP_YCBCR_FORMAT_YV12, planes, pitches) == 0 ? 0 : -1;
}
static int VdpUpload(pm_hw *h, const picture_t *src, uint32_t surf)
{
    const void *planes[3] = { src->p[0].p_pixels, src->p[2].p_pixels, src->p[1].p_pixels };
    const uint32_t pitches[3] = { (uint32_t)src->p[0].i_pitch, (uint32_t)src->p[2].i_pitch, (uint32_t)src->p[1].i_pitch };
    return vdp.PutBits(h->vdp, surf, PM_VDP_YCBCR_FORMAT_YV12, planes, pitches) == 0 ? 0 : -1;
}

int pm_hw_download(pm_hw *h, picture_t *src, picture_t *dst)
{
    if (!src->context) return -1;
    if (h->kind == HW_VA) {
        pmVAGenericID s;
        if (VaSurfaceOf(src, &s) != 0) return -1;
        return VaDownload(h, s, dst);
    }
    const pm_vdp_field *f = (const pm_vdp_field *)src->context;
    if (!f->frame) return -1;
    return VdpDownload(h, f->frame->surface, dst);
}

int pm_hw_upload(pm_hw *h, const picture_t *src, picture_t *dst)
{
    pm_slot *s = SlotOf(h, dst);
    if (!s) return -1;
    vlc_mutex_lock(&h->ul_lock);
    const int r = h->kind == HW_VA ? VaUpload(h, src, s->surface) : VdpUpload(h, src, s->surface);
    vlc_mutex_unlock(&h->ul_lock);
    return r;
}

void pm_hw_anchor(pm_hw *h, picture_t *src)
{
    if (h->kind != HW_VA || !src->context || !src->context->copy) return;
    picture_context_t *n = src->context->copy(src->context);
    if (!n) return;
    vlc_mutex_lock(&h->lock);
    picture_context_t *old = h->anchor;
    h->anchor = n;
    vlc_mutex_unlock(&h->lock);
    if (old) old->destroy(old);
}

/* ---- binding to the first picture's device */
static void Pattern(picture_t *p, const video_format_t *f, int bits, int seed)
{
    for (int pl = 0; pl < 3; pl++) {
        const int w = pl ? ((int)f->i_width + 1) / 2 : (int)f->i_width, hgt = pl ? ((int)f->i_height + 1) / 2 : (int)f->i_height;
        for (int y = 0; y < hgt; y++) {
            uint8_t *row = p->p[pl].p_pixels + (size_t)y * p->p[pl].i_pitch;
            for (int x = 0; x < w; x++) {
                const unsigned v = (unsigned)(x * 7 + y * 13 + seed * 31 + pl * 57);
                if (bits == 8) row[x] = (uint8_t)(16 + v % 220);
                else ((uint16_t *)row)[x] = (uint16_t)(64 + v % 876);
            }
        }
    }
}
static bool Same(const picture_t *a, const picture_t *b, const video_format_t *f, int bits)
{
    const int bps = bits > 8 ? 2 : 1;
    for (int pl = 0; pl < 3; pl++) {
        const int w = pl ? ((int)f->i_width + 1) / 2 : (int)f->i_width, hgt = pl ? ((int)f->i_height + 1) / 2 : (int)f->i_height;
        for (int y = 0; y < hgt; y++)
            if (memcmp(a->p[pl].p_pixels + (size_t)y * a->p[pl].i_pitch, b->p[pl].p_pixels + (size_t)y * b->p[pl].i_pitch, (size_t)w * bps))
                return false;
    }
    return true;
}

static void RefPicDestroy(picture_t *p) { free(p); }
static int BindVa(pm_hw *h, picture_t *first, char *why, size_t n)
{
    if (!LoadVa()) { snprintf(why, n, "libva.so.2 not found"); return -1; }
    const pm_va_ctx *c = (const pm_va_ctx *)first->context;
    const char *mod = ModuleOf((const void *)c->s.destroy);
    if (!strstr(mod, "vaapi")) { snprintf(why, n, "the picture context is not VLC's VA-API one (%s)", *mod ? mod : "unknown"); return -1; }
    const picture_t *ref = c->picref;
    const struct pm_va_picsys *ps = ref ? (const struct pm_va_picsys *)ref->p_sys : NULL;
    pmVADisplay dpy = ps && ps->instance ? ps->instance->va_dpy : NULL;
    if (!dpy || *(const int *)dpy != PM_VA_DISPLAY_MAGIC) { snprintf(why, n, "VLC's VA-API picture layout differs (no VADisplay)"); return -1; }
    h->dpy = dpy;
    h->va_rt = h->bits > 8 ? PM_VA_RT_FORMAT_YUV420_10 : PM_VA_RT_FORMAT_YUV420;
    h->va_fourcc = h->bits > 8 ? PM_VA_FOURCC_P010 : PM_VA_FOURCC_NV12;
    h->sw = h->fmt.i_width; h->sh = h->fmt.i_height;
    /* the image format entry the driver lists for NV12 / P010 (for vaCreateImage) */
    const int max = va.MaxNumImageFormats(dpy);
    pmVAImageFormat *fl = max > 0 ? calloc((size_t)max, sizeof(*fl)) : NULL;
    int cnt = 0;
    bool have = false;
    if (fl && va.QueryImageFormats(dpy, fl, &cnt) == 0)
        for (int i = 0; i < cnt && i < max; i++) if (fl[i].fourcc == h->va_fourcc) { h->va_ifmt = fl[i]; have = true; break; }
    free(fl);
    if (!have) { memset(&h->va_ifmt, 0, sizeof(h->va_ifmt)); h->va_ifmt.fourcc = h->va_fourcc; h->va_ifmt.bits_per_pixel = h->bits > 8 ? 24 : 12; }
    /* our pool, and the picref that leads VLC's code (vlc_vaapi_PicGetDisplay) to the display */
    pmVAGenericID ids[MAX_SLOTS];
    const pmVAStatus st = va.CreateSurfaces(dpy, h->va_rt, h->sw, h->sh, ids, (unsigned)h->pool_size, NULL, 0);
    if (st != 0) { snprintf(why, n, "vaCreateSurfaces: %s", va.ErrorStr(st)); return -1; }
    for (int i = 0; i < h->pool_size; i++) { h->slots[i].surface = ids[i]; atomic_init(&h->slots[i].refs, 0); }
    h->nslots = h->pool_size;
    atomic_init(&h->inst.pic_refcount, 1);
    h->inst.va_dpy = dpy;
    h->inst.num_render_targets = 0;
    h->picsys.instance = &h->inst;
    picture_resource_t rsc;
    memset(&rsc, 0, sizeof(rsc));
    rsc.p_sys = (picture_sys_t *)&h->picsys;
    rsc.pf_destroy = RefPicDestroy;     /* p_sys lives in h */
    h->refpic = picture_NewFromResource(&h->fmt, &rsc);
    if (!h->refpic) { snprintf(why, n, "out of memory"); return -1; }
    pm_hw_anchor(h, first);
    const char *vendor = va.QueryVendorString(dpy);
    snprintf(h->desc, sizeof(h->desc), "VA-API on %s", vendor ? vendor : "an unnamed driver");
    return 0;
}

static int BindVdp(pm_hw *h, picture_t *first, char *why, size_t n)
{
    const pm_vdp_field *f = (const pm_vdp_field *)first->context;
    const char *mod = ModuleOf((const void *)f->context.destroy);
    if (!strstr(mod, "vdpau")) { snprintf(why, n, "the picture context is not VLC's VDPAU one (%s)", *mod ? mod : "unknown"); return -1; }
    if (!LoadVdp()) { snprintf(why, n, "VLC's libvlc_vdpau is not loaded"); return -1; }
    if (!f->frame || !f->frame->vdp) { snprintf(why, n, "VLC's VDPAU picture layout differs"); return -1; }
    uint32_t type = 99, w = 0, hh = 0;
    if (vdp.SurfaceGetParameters(f->frame->vdp, f->frame->surface, &type, &w, &hh) != 0 || type != PM_VDP_CHROMA_TYPE_420 ||
        w < h->fmt.i_visible_width || hh < h->fmt.i_visible_height) {
        snprintf(why, n, "VLC's VDPAU picture layout differs (surface query)");
        return -1;
    }
    h->vdp = vdp.Hold(f->frame->vdp, &h->device);
    if (!h->vdp) { snprintf(why, n, "vdp_hold_x11 failed"); return -1; }
    h->sw = w; h->sh = hh;
    for (int i = 0; i < h->pool_size; i++) {
        uint32_t s;
        if (vdp.SurfaceCreate(h->vdp, h->device, PM_VDP_CHROMA_TYPE_420, w, hh, &s) != 0) break;
        pm_slot *sl = &h->slots[h->nslots++];
        sl->surface = s;
        atomic_init(&sl->refs, 0);
        sl->frame.surface = s; sl->frame.device = h->device; sl->frame.vdp = h->vdp;
        atomic_init(&sl->frame.refs, 1);
    }
    if (h->nslots < 4) { snprintf(why, n, "could not create VDPAU surfaces (%d)", h->nslots); return -1; }
    snprintf(h->desc, sizeof(h->desc), "VDPAU");
    return 0;
}

int pm_hw_bind(pm_hw *h, picture_t *first, char *why, size_t n)
{
    if (!first->context) { snprintf(why, n, "the picture carries no hardware context"); return -1; }
    const int r = h->kind == HW_VA ? BindVa(h, first, why, n) : BindVdp(h, first, why, n);
    if (r != 0) return r;
    /* the work format covers the whole surface (VDPAU reads and writes it entire) */
    if (h->wfmt.i_width < h->sw) h->wfmt.i_width = h->sw;
    if (h->wfmt.i_height < h->sh) h->wfmt.i_height = h->sh;
    h->cpu_pool = picture_pool_NewFromFormat(&h->wfmt, (unsigned)h->pool_size + 6);

    /* the round trip: two pool surfaces, two patterns up and back down, through each way the driver offers */
    picture_t *o[2] = { pm_hw_new_picture(h), pm_hw_new_picture(h) };
    picture_t *c[2] = { pm_hw_cpu_picture(h), pm_hw_cpu_picture(h) }, *r2[2] = { pm_hw_cpu_picture(h), pm_hw_cpu_picture(h) };
    int ok = -1;
    if (o[0] && o[1] && c[0] && c[1] && r2[0] && r2[1]) {
        Pattern(c[0], &h->fmt, h->bits, 1);
        Pattern(c[1], &h->fmt, h->bits, 2);
        static const struct { bool up, down; const char *name; } ways[] = {
            { true, true, "derive both ways" }, { true, false, "derive up, get-image down" },
            { false, true, "put-image up, derive down" }, { false, false, "put-image up, get-image down" } };
        const int nways = h->kind == HW_VA ? 4 : 1;
        for (int i = 0; i < nways && ok != 0; i++) {
            h->up_derive = ways[i].up; h->down_derive = ways[i].down;
            if (pm_hw_upload(h, c[0], o[0]) == 0 && pm_hw_upload(h, c[1], o[1]) == 0 &&
                pm_hw_download(h, o[0], r2[0]) == 0 && pm_hw_download(h, o[1], r2[1]) == 0 &&
                Same(c[0], r2[0], &h->fmt, h->bits) && Same(c[1], r2[1], &h->fmt, h->bits)) {
                ok = 0;
                if (h->kind == HW_VA) {
                    const size_t l = strlen(h->desc);
                    snprintf(h->desc + l, sizeof(h->desc) - l, ", %s", ways[i].name);
                }
            }
        }
        if (ok != 0) snprintf(why, n, "%s: the upload/download round trip failed (the driver does not keep what is written)", h->desc);
    } else snprintf(why, n, "%s: no pool surface or memory for the round trip", h->desc);
    for (int i = 0; i < 2; i++) {
        if (o[i]) picture_Release(o[i]);
        if (c[i]) picture_Release(c[i]);
        if (r2[i]) picture_Release(r2[i]);
    }
    if (ok == 0) {
        const size_t l = strlen(h->desc);
        snprintf(h->desc + l, sizeof(h->desc) - l, "; pool of %d %ux%u surfaces", h->nslots, h->sw, h->sh);
    }
    return ok;
}

const char *pm_hw_describe(const pm_hw *h) { return h->desc; }
void pm_hw_set_pool_size(pm_hw *h, int n) { h->pool_size = n < 4 ? 4 : n > MAX_SLOTS ? MAX_SLOTS : n; }

#else  /* no hardware-format path on this OS */
bool pm_hw_chroma(vlc_fourcc_t c, int *bits) { (void)c; (void)bits; return false; }
pm_hw *pm_hw_new(vlc_object_t *o, const video_format_t *fmt, int pool_size) { (void)o; (void)fmt; (void)pool_size; return NULL; }
int pm_hw_bind(pm_hw *h, picture_t *first, char *why, size_t n) { (void)h; (void)first; snprintf(why, n, "not on this OS"); return -1; }
const video_format_t *pm_hw_work_format(const pm_hw *h) { (void)h; return NULL; }
picture_t *pm_hw_cpu_picture(pm_hw *h) { (void)h; return NULL; }
int pm_hw_download(pm_hw *h, picture_t *src, picture_t *dst) { (void)h; (void)src; (void)dst; return -1; }
picture_t *pm_hw_new_picture(pm_hw *h) { (void)h; return NULL; }
int pm_hw_upload(pm_hw *h, const picture_t *src, picture_t *dst) { (void)h; (void)src; (void)dst; return -1; }
void pm_hw_anchor(pm_hw *h, picture_t *src) { (void)h; (void)src; }
const char *pm_hw_describe(const pm_hw *h) { (void)h; return ""; }
void pm_hw_set_pool_size(pm_hw *h, int n) { (void)h; (void)n; }
void pm_hw_release(pm_hw *h) { (void)h; }
#endif
