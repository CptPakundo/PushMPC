/*
 * pushspike: PushMPC's VLC 3.0.x feasibility spike (track T3, internal test module).
 *
 * Two submodules in one plugin:
 *   1. a "video filter" that registers the shortcuts "pushspike" and "postproc" with a score above VLC's own
 *      postproc (0). VLC 3.0.x's vout puts a filter named "postproc" (and, from 3.0.22, "amf_frc") into its STATIC
 *      chain (src/video_output/video_output.c, ThreadChangeFilters), the only chain whose extra output pictures
 *      (picture->p_next) are drained one per display. The filter outputs `mult` pictures per input (the source frame
 *      plus mult-1 linear blends towards the next frame, so every output is a distinct image) with timestamps spread
 *      evenly over the source frame interval, and can multiply the output size by `scale` (nearest neighbour).
 *   2. a "vout display" ("pushdump") that writes one CSV line per displayed picture (index, picture date, wall clock
 *      at display, visible size, chroma, luma checksum), so a headless CI run can count frames and check timestamps.
 *
 * Options (also accepted per filter: --video-filter="postproc{mult=4,scale=2}" sets pushspike-mult/-scale):
 *   pushspike-mult  output frames per input frame (1..16, default 2)
 *   pushspike-scale output size factor (1..4, default 1)
 *   pushspike-alloc 0 = filter_NewPicture (the vout's private pool), 1 = picture_NewFromFormat (heap)
 *   pushdump-file   CSV path for the dump display
 *
 * Licence: GPL-3.0-or-later (as PushMPC). Builds against the VLC 3.0.x plugin SDK headers.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_filter.h>
#include <vlc_picture.h>
#include <vlc_vout_display.h>
#include <vlc_fs.h>

#define FCFG "pushspike-"
#define DCFG "pushdump-"

static int  OpenFilter(vlc_object_t *);
static void CloseFilter(vlc_object_t *);
static int  OpenDump(vlc_object_t *);
static void CloseDump(vlc_object_t *);

static const char *const filter_options[] = { "mult", "scale", "alloc", NULL };

vlc_module_begin()
    set_shortname("PushMPC spike")
    set_description("PushMPC VLC spike: frame multiplier and size change in the static chain")
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VFILTER)
    set_capability("video filter", 100)
    add_shortcut("pushspike", "postproc")
    add_integer_with_range(FCFG "mult", 2, 1, 16, "Frames out per frame in", NULL, false)
    add_integer_with_range(FCFG "scale", 1, 1, 4, "Output size factor", NULL, false)
    add_integer_with_range(FCFG "alloc", 0, 0, 1, "0 = vout pool, 1 = heap", NULL, false)
    set_callbacks(OpenFilter, CloseFilter)

    add_submodule()
    set_shortname("pushdump")
    set_description("PushMPC spike: CSV dump video output")
    set_subcategory(SUBCAT_VIDEO_VOUT)
    set_capability("vout display", 0)
    add_shortcut("pushdump")
    add_string(DCFG "file", "pushdump.csv", "CSV file", NULL, false)
    set_callbacks(OpenDump, CloseDump)
vlc_module_end()

/* ---- D3D11 opaque pictures (Windows): layout replicated from VLC 3.0.x modules/video_chroma/d3d11_fmt.h and
 * modules/codec/avcodec/va_surface.h (internal headers, not in the SDK). Read-only here: it only proves that the
 * filter receives the decoder's ID3D11Texture2D and slice index. */
struct spike_d3d11_picsys {
    void    *decoder;          /* ID3D11VideoDecoderOutputView* */
    void    *texture[4];       /* ID3D11Texture2D*, D3D11_MAX_SHADER_VIEW = 4 */
    void    *context;          /* ID3D11DeviceContext* */
    unsigned slice_index;
    void    *processorInput;
    void    *processorOutput;
    void    *resourceView[4];
    int      formatTexture;    /* DXGI_FORMAT */
};
struct spike_va_pic_context {
    picture_context_t s;
    void *va_surface;
    struct spike_d3d11_picsys picsys;
};
static const struct spike_d3d11_picsys *SpikeActivePicSys(const picture_t *p)
{
    if (p->context)
        return &((const struct spike_va_pic_context *)p->context)->picsys;
    return (const struct spike_d3d11_picsys *)p->p_sys;
}
static bool IsD3D11Opaque(vlc_fourcc_t c)
{
    return c == VLC_FOURCC('d','v','1','1') || c == VLC_FOURCC('d','v','1','0') ||
           c == VLC_FOURCC('d','a','g','r') || c == VLC_FOURCC('d','a','r','g') ||
           c == VLC_FOURCC('d','x','1','0'); /* VLC_CODEC_D3D11_OPAQUE(_10B/_RGBA/_BGRA), D3D9 'dx10'? logged only */
}

/* ---------------------------------------------------------------- filter */
struct filter_sys_t { /* typedef'd in vlc_common.h */
    int mult, scale, alloc;
    bool opaque;
    picture_t *prev;
    uint64_t in, out, alloc_fail;
    mtime_t fallback_dt;
};

static picture_t *NewOut(filter_t *f, filter_sys_t *sys)
{
    picture_t *p = sys->alloc ? picture_NewFromFormat(&f->fmt_out.video) : filter_NewPicture(f);
    if (!p)
        sys->alloc_fail++;
    return p;
}

/* out = scale-up(a * (m-k)/m + b * k/m), per 8-bit plane, nearest neighbour */
static void Render(const picture_t *a, const picture_t *b, int k, int m, int s, picture_t *out)
{
    for (int i = 0; i < out->i_planes && i < a->i_planes; i++) {
        const plane_t *pa = &a->p[i], *pb = &b->p[i];
        plane_t *po = &out->p[i];
        const int px = pa->i_pixel_pitch > 0 ? pa->i_pixel_pitch : 1;
        const int lines = po->i_visible_lines, bytes = po->i_visible_pitch;
        for (int y = 0; y < lines; y++) {
            const uint8_t *ra = pa->p_pixels + (size_t)(y / s) * pa->i_pitch;
            const uint8_t *rb = pb->p_pixels + (size_t)(y / s) * pb->i_pitch;
            uint8_t *ro = po->p_pixels + (size_t)y * po->i_pitch;
            if (s == 1 && k == 0) { memcpy(ro, ra, bytes); continue; }
            for (int x = 0; x < bytes; x++) {
                const int sx = ((x / px) / s) * px + (x % px);
                ro[x] = (uint8_t)((ra[sx] * (m - k) + rb[sx] * k + m / 2) / m);
            }
        }
    }
}

static picture_t *Filter(filter_t *f, picture_t *cur)
{
    filter_sys_t *sys = (filter_sys_t *)f->p_sys;
    sys->in++;

    if (sys->mult == 1 && sys->scale == 1) { sys->out++; return cur; } /* pure pass-through */

    if (sys->opaque) {
        const struct spike_d3d11_picsys *ps = SpikeActivePicSys(cur);
        if (sys->in <= 3 || sys->in % 100 == 0)
            msg_Info(f, "pushspike: D3D11 opaque picture %" PRIu64 ": texture=%p slice=%u context=%p fmt=%d",
                     sys->in, ps ? ps->texture[0] : NULL, ps ? ps->slice_index : 0, ps ? ps->context : NULL,
                     ps ? ps->formatTexture : -1);
        /* pass-through with mult-1 clones sharing the texture (timestamps spread like the CPU path) */
        if (!sys->prev) { sys->prev = cur; return NULL; }
        picture_t *head = sys->prev, *tail = head;
        mtime_t dt = cur->date - head->date;
        if (dt <= 0 || dt > CLOCK_FREQ) dt = sys->fallback_dt;
        for (int k = 1; k < sys->mult; k++) {
            picture_t *c = picture_Clone(head);
            if (!c) { sys->alloc_fail++; break; }
            picture_CopyProperties(c, head);
            c->date = head->date + dt * k / sys->mult;
            tail->p_next = c; tail = c;
        }
        sys->prev = cur;
        for (picture_t *p = head; p; p = p->p_next) sys->out++;
        return head;
    }

    if (!sys->prev) { sys->prev = cur; return NULL; } /* one frame of latency, as a real interpolator */

    picture_t *a = sys->prev;
    mtime_t dt = cur->date - a->date;
    if (dt <= 0 || dt > CLOCK_FREQ) dt = sys->fallback_dt;

    picture_t *head = NULL, *tail = NULL;
    for (int k = 0; k < sys->mult; k++) {
        picture_t *o;
        if (k == 0 && sys->scale == 1) {
            o = picture_Hold(a); /* the source frame itself, no copy */
        } else {
            o = NewOut(f, sys);
            if (!o) break;
            Render(sys->prev, cur, k, sys->mult, sys->scale, o);
            picture_CopyProperties(o, sys->prev);
        }
        o->date = sys->prev->date + dt * k / sys->mult;
        o->p_next = NULL;
        if (tail) tail->p_next = o; else head = o;
        tail = o;
        sys->out++;
    }
    if (sys->in % 50 == 0)
        msg_Info(f, "pushspike: in=%" PRIu64 " out=%" PRIu64 " alloc_fail=%" PRIu64, sys->in, sys->out, sys->alloc_fail);
    picture_Release(sys->prev);
    sys->prev = cur;
    return head;
}

static void Flush(filter_t *f)
{
    filter_sys_t *sys = (filter_sys_t *)f->p_sys;
    if (sys->prev) { picture_Release(sys->prev); sys->prev = NULL; }
}

static int OpenFilter(vlc_object_t *obj)
{
    filter_t *f = (filter_t *)obj;
    const vlc_fourcc_t chroma = f->fmt_in.video.i_chroma;
    const vlc_chroma_description_t *d = vlc_fourcc_GetChromaDescription(chroma);
    const bool opaque = IsD3D11Opaque(chroma);

    msg_Info(f, "pushspike: open, input %4.4s %ux%u (visible %ux%u) %u/%u fps", (const char *)&chroma,
             f->fmt_in.video.i_width, f->fmt_in.video.i_height, f->fmt_in.video.i_visible_width,
             f->fmt_in.video.i_visible_height, f->fmt_in.video.i_frame_rate, f->fmt_in.video.i_frame_rate_base);

    if (!opaque && (!d || d->plane_count == 0 || d->pixel_size != 1)) {
        msg_Warn(f, "pushspike: unsupported chroma %4.4s", (const char *)&chroma);
        return VLC_EGENERIC;
    }

    config_ChainParse(f, FCFG, filter_options, f->p_cfg);
    filter_sys_t *sys = calloc(1, sizeof(*sys));
    if (!sys) return VLC_ENOMEM;
    sys->mult  = var_CreateGetInteger(f, FCFG "mult");
    sys->scale = opaque ? 1 : var_CreateGetInteger(f, FCFG "scale");
    sys->alloc = var_CreateGetInteger(f, FCFG "alloc");
    sys->opaque = opaque;
    sys->fallback_dt = (f->fmt_in.video.i_frame_rate && f->fmt_in.video.i_frame_rate_base)
        ? (mtime_t)CLOCK_FREQ * f->fmt_in.video.i_frame_rate_base / f->fmt_in.video.i_frame_rate
        : CLOCK_FREQ / 24;

    f->fmt_out.video = f->fmt_in.video;
    f->fmt_out.i_codec = f->fmt_in.i_codec;
    if (f->fmt_out.video.i_frame_rate)
        f->fmt_out.video.i_frame_rate *= sys->mult;
    if (sys->scale > 1) {
        video_format_t *v = &f->fmt_out.video;
        v->i_width *= sys->scale; v->i_height *= sys->scale;
        v->i_visible_width *= sys->scale; v->i_visible_height *= sys->scale;
        v->i_x_offset *= sys->scale; v->i_y_offset *= sys->scale;
    }
    f->p_sys = (void *)sys;
    f->pf_video_filter = Filter;
    f->pf_flush = Flush;
    msg_Info(f, "pushspike: active, mult=%d scale=%d alloc=%s, output %ux%u %s", sys->mult, sys->scale,
             sys->alloc ? "heap" : "pool", f->fmt_out.video.i_visible_width, f->fmt_out.video.i_visible_height,
             opaque ? "(D3D11 opaque, pass-through clones)" : "");
    return VLC_SUCCESS;
}

static void CloseFilter(vlc_object_t *obj)
{
    filter_t *f = (filter_t *)obj;
    filter_sys_t *sys = (filter_sys_t *)f->p_sys;
    msg_Info(f, "pushspike: close, in=%" PRIu64 " out=%" PRIu64 " alloc_fail=%" PRIu64, sys->in, sys->out,
             sys->alloc_fail);
    Flush(f);
    free(sys);
}

/* ---------------------------------------------------------------- dump display */
struct vout_display_sys_t {
    FILE *f;
    picture_pool_t *pool;
    uint64_t n;
};

static picture_pool_t *DumpPool(vout_display_t *vd, unsigned count)
{
    vout_display_sys_t *sys = vd->sys;
    if (!sys->pool) {
        sys->pool = picture_pool_NewFromFormat(&vd->fmt, count);
        msg_Info(vd, "pushdump: pool of %u pictures, %ux%u", count, vd->fmt.i_visible_width, vd->fmt.i_visible_height);
    }
    return sys->pool;
}

static void DumpDisplay(vout_display_t *vd, picture_t *pic, subpicture_t *sub)
{
    vout_display_sys_t *sys = vd->sys;
    const mtime_t now = mdate();
    uint64_t sum = 0;
    if (pic->i_planes > 0 && pic->p[0].p_pixels) {
        const plane_t *p = &pic->p[0];
        for (int y = 0; y < p->i_visible_lines; y += 3)
            for (int x = 0; x < p->i_visible_pitch; x += 7)
                sum = sum * 31 + p->p_pixels[(size_t)y * p->i_pitch + x];
    }
    fprintf(sys->f, "%" PRIu64 ",%" PRId64 ",%" PRId64 ",%u,%u,%4.4s,%016" PRIx64 "\n", sys->n++, (int64_t)pic->date,
            (int64_t)now, pic->format.i_visible_width, pic->format.i_visible_height,
            (const char *)&pic->format.i_chroma, sum);
    picture_Release(pic);
    if (sub) subpicture_Delete(sub);
}

static int DumpControl(vout_display_t *vd, int q, va_list ap)
{
    (void)vd; (void)ap;
    switch (q) {
    case VOUT_DISPLAY_CHANGE_DISPLAY_SIZE:
    case VOUT_DISPLAY_CHANGE_DISPLAY_FILLED:
    case VOUT_DISPLAY_CHANGE_ZOOM:
    case VOUT_DISPLAY_CHANGE_SOURCE_ASPECT:
    case VOUT_DISPLAY_CHANGE_SOURCE_CROP:
        return VLC_SUCCESS;
    default:
        return VLC_EGENERIC;
    }
}

static int OpenDump(vlc_object_t *obj)
{
    vout_display_t *vd = (vout_display_t *)obj;
    vout_display_sys_t *sys = calloc(1, sizeof(*sys));
    if (!sys) return VLC_ENOMEM;
    char *name = var_InheritString(vd, DCFG "file");
    sys->f = name ? vlc_fopen(name, "wb") : NULL;
    if (!sys->f) { msg_Err(vd, "pushdump: cannot open %s", name ? name : "(null)"); free(name); free(sys); return VLC_EGENERIC; }
    msg_Info(vd, "pushdump: writing %s, source %4.4s %ux%u", name, (const char *)&vd->fmt.i_chroma,
             vd->fmt.i_visible_width, vd->fmt.i_visible_height);
    free(name);
    fprintf(sys->f, "n,date,shown,w,h,chroma,sum\n");
    if (IsD3D11Opaque(vd->fmt.i_chroma) || !vlc_fourcc_GetChromaDescription(vd->fmt.i_chroma))
        vd->fmt.i_chroma = VLC_CODEC_I420; /* a CPU format; VLC converts */
    vd->sys = sys;
    vd->pool = DumpPool;
    vd->prepare = NULL;
    vd->display = DumpDisplay;
    vd->control = DumpControl;
    vout_display_DeleteWindow(vd, NULL);
    return VLC_SUCCESS;
}

static void CloseDump(vlc_object_t *obj)
{
    vout_display_t *vd = (vout_display_t *)obj;
    vout_display_sys_t *sys = vd->sys;
    msg_Info(vd, "pushdump: close after %" PRIu64 " pictures", sys->n);
    if (sys->pool) picture_pool_Release(sys->pool);
    fclose(sys->f);
    free(sys);
}
