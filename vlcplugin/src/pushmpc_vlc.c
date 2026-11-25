/*
 * PushMPC for VLC 3.0.x: frame generation in VLC's video output.
 *
 * Where it sits (see track-vlc notes and vlcplugin/README.md): VLC 3.0.x builds two filter chains in the video output.
 * Only the STATIC chain may output more pictures than it receives (extra pictures are linked through
 * picture->p_next and drained one per display), and VLC puts a filter there only by name: "postproc" (every 3.0.x),
 * "amf_frc" (3.0.22+) and the deinterlacer (src/video_output/video_output.c, ThreadChangeFilters). So the filter is a
 * submodule named "postproc" with a score above VLC's own postproc module (0): with video-filter=postproc in vlcrc (the
 * installer writes it; in the GUI: Tools > Effects and Filters > Video Effects > Advanced > "Post processing", or the
 * "postproc" box under Preferences > All > Video > Filters) VLC loads PushMPC there.
 *
 * What it does per source frame pair (prev, cur): the real frame prev at its own time, then k - 1 generated frames at
 * prev + j/k of the interval, from the engine behind pm_engine.h (PushFPS's portable core: motion both ways,
 * motion-compensated interpolation, PushFPS's scene-cut rule; a cut pair repeats the nearer real frame). One source
 * frame of latency, as every interpolator. A seek or a timestamp gap restarts the pair.
 *
 * Output size cannot change here: VLC converts a static filter's output back to the source size (es_format_IsSimilar
 * -> filter_chain_AppendConverter), so upscaling is the display's job (Windows: d3d11-upscale-mode=super, i.e. RTX /
 * Intel Video Super Resolution in VLC's own Direct3D 11 output; the installer sets it).
 *
 * Pictures: the vout's private pool holds 4 pictures, which caps a static filter at x4 (3 new + the source picture
 * itself). Heap pictures sent straight to a direct-rendering display crash some of them (VLC's OpenGL output with
 * persistent buffers, CI run-display.sh). VLC's own answer is in VoutVideoFilterStaticNewPicture: once the interactive
 * chain is not empty, static filters get heap pictures and the last interactive filter hands the display a pool
 * picture. So the plugin has a second, tiny filter "pushmpc_out" for the interactive chain: it copies each generated
 * picture into a display-pool picture (one memcpy) and passes everything else through. The installer writes
 * video-filter=postproc:pushmpc_out; without pushmpc_out the multiplier is capped at x4.
 *
 * Licence: GPL-3.0-or-later.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_filter.h>
#include <vlc_picture.h>

#include "pm_engine.h"

double pm_display_hz(void);

#ifndef PUSHMPC_VLC_VERSION
# define PUSHMPC_VLC_VERSION "0.6.0-dev"
#endif

/* Debian/Ubuntu's VLC 3.0.20+ (time64 transition build) looks up vlc_entry__3_0_0ft64; upstream, Flatpak, Windows and
 * macOS look up vlc_entry__3_0_0f. Same ABI on 64-bit targets: export both (the build passes the missing name). */
#ifdef PM_ALT_ENTRY
# define PM_STR2(x) #x
# define PM_STR(x) PM_STR2(x)
__asm__(".globl " PM_STR(PM_ALT_ENTRY) "\n\t.set " PM_STR(PM_ALT_ENTRY) ", vlc_entry__" PM_STR(MODULE_SYMBOL) "\n");
#endif

#define CFG "pushmpc-"

static int  Open(vlc_object_t *);
static void Close(vlc_object_t *);
static int  OpenOut(vlc_object_t *);
static void CloseOut(vlc_object_t *);

/* generated pictures on their way to pushmpc_out (a small ring; a stale entry only costs one needless copy) */
static vlc_mutex_t reg_lock = VLC_STATIC_MUTEX;
static picture_t *reg[256];
static unsigned reg_next;
static void RegAdd(picture_t *p)
{
    vlc_mutex_lock(&reg_lock);
    reg[reg_next++ % (sizeof(reg) / sizeof(reg[0]))] = p;
    vlc_mutex_unlock(&reg_lock);
}
static bool RegTake(picture_t *p)
{
    bool hit = false;
    vlc_mutex_lock(&reg_lock);
    for (size_t i = 0; i < sizeof(reg) / sizeof(reg[0]); i++)
        if (reg[i] == p) { reg[i] = NULL; hit = true; break; }
    vlc_mutex_unlock(&reg_lock);
    return hit;
}

static const int smoother_values[] = { 0, 1, 2, 3, 4, 5, 6, 8 };
static const char *const smoother_texts[] = { "Auto (display refresh, CPU budget)", "Off", "x2", "x3", "x4", "x5", "x6", "x8" };
static const int quality_values[] = { 0, 1 };
static const char *const quality_texts[] = { "Fast", "Balanced" };
static const int alloc_values[] = { 0, 1, 2 };
static const char *const alloc_texts[] = { "Auto (VLC's allocator)", "Pool only (at most x4)", "Heap (unsafe with some displays)" };

vlc_module_begin()
    set_shortname("PushMPC")
    set_description("PushMPC frame generation (PushFPS's video enhancement in VLC)")
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VFILTER)
    add_integer(CFG "smoother", 0, "Smoother (frame generation)",
                "Auto picks the multiplier from the display refresh rate and what this computer can generate in time; "
                "Off passes frames through; x2..x8 force a multiplier.", false)
        change_integer_list(smoother_values, smoother_texts)
        change_safe()
    add_integer(CFG "quality", 1, "Motion search quality", "Fast searches on a quarter-size picture, Balanced on a "
                "half-size picture (PushFPS's own levels).", true)
        change_integer_list(quality_values, quality_texts)
        change_safe()
    add_float(CFG "display-hz", 0.0, "Display refresh rate (Hz)", "0 = detect (the primary display)", true)
        change_safe()
    add_integer_with_range(CFG "threads", 0, 0, 64, "Engine threads", "0 = all hardware threads (at most 16)", true)
    add_integer(CFG "alloc", 0, "Output pictures", "Where generated pictures come from (diagnostics)", true)
        change_integer_list(alloc_values, alloc_texts)

    add_submodule()
    /* the submodule's first shortcut is its name: "postproc", the name VLC 3 puts into the static chain */
    add_shortcut("postproc", "pushmpc")
    set_description("PushMPC frame generation (as postproc: tick 'Post processing' to switch it on)")
    set_capability("video filter", 100)
    set_callbacks(Open, Close)

    add_submodule()
    add_shortcut("pushmpc_out")
    set_description("PushMPC output stage (keep it after postproc; needed above x4)")
    set_capability("video filter", 0)
    set_callbacks(OpenOut, CloseOut)
vlc_module_end()

struct filter_sys_t {
    filter_chain_t *download;  /* hardware pictures (DX11, CVPX, ...) -> I420 / I0AL with VLC's own converters */
    pm_engine *eng;
    int k;                 /* outputs per source frame */
    int alloc;
    int bits, bps;
    picture_t *prev;
    mtime_t nominal_dt;
    uint64_t in, out, generated, pool_out, heap_out, alloc_fail, restarts;
    char why[256];
};

static bool Supported(vlc_fourcc_t c, int *bits)
{
    switch (c) {
    case VLC_CODEC_I420: case VLC_CODEC_J420: case VLC_CODEC_YV12:
        *bits = 8; return true;
    case VLC_CODEC_I420_10L:
        *bits = 10; return true;
    case VLC_CODEC_I420_12L:
        *bits = 12; return true;
    case VLC_CODEC_I420_16L:
        *bits = 16; return true;
    default:
        return false;
    }
}

static void PlanesIn(const picture_t *p, const video_format_t *f, int bps, pm_yuv *y)
{
    for (int i = 0; i < 3; i++) {
        const int sx = i ? f->i_x_offset / 2 : f->i_x_offset, sy = i ? f->i_y_offset / 2 : f->i_y_offset;
        y->data[i] = p->p[i].p_pixels + (ptrdiff_t)sy * p->p[i].i_pitch + (ptrdiff_t)sx * bps;
        y->stride[i] = p->p[i].i_pitch;
    }
}

static void PlanesOut(picture_t *p, const video_format_t *f, int bps, pm_yuv_out *y)
{
    for (int i = 0; i < 3; i++) {
        const int sx = i ? f->i_x_offset / 2 : f->i_x_offset, sy = i ? f->i_y_offset / 2 : f->i_y_offset;
        y->data[i] = p->p[i].p_pixels + (ptrdiff_t)sy * p->p[i].i_pitch + (ptrdiff_t)sx * bps;
        y->stride[i] = p->p[i].i_pitch;
    }
}

static picture_t *NewOut(filter_t *f, struct filter_sys_t *sys)
{
    /* filter_NewPicture: the vout's private pool, or the heap once an interactive filter (pushmpc_out) follows */
    picture_t *p = sys->alloc == 2 ? picture_NewFromFormat(&f->fmt_out.video) : filter_NewPicture(f);
    if (!p) { sys->alloc_fail++; return NULL; }
    if (sys->alloc == 2) sys->heap_out++; else sys->pool_out++;
    return p;
}

static picture_t *Filter(filter_t *f, picture_t *cur)
{
    struct filter_sys_t *sys = (struct filter_sys_t *)f->p_sys;
    sys->in++;
    if (sys->download) {        /* our own converter in front, so our extra pictures reach the static chain intact */
        cur = filter_chain_VideoFilter(sys->download, cur);
        if (!cur) return NULL;
    }
    if (sys->k <= 1 || !sys->eng) { sys->out++; return cur; }

    pm_yuv src;
    PlanesIn(cur, &f->fmt_out.video, sys->bps, &src);

    picture_t *prev = sys->prev;
    sys->prev = cur;
    mtime_t dt = prev ? cur->date - prev->date : 0;
    const bool contiguous = prev && dt > 0 && dt <= 4 * sys->nominal_dt && dt <= CLOCK_FREQ / 4;
    char err[160] = "";
    if (pm_engine_push(sys->eng, &src, contiguous, err, sizeof(err)) != 0) {
        msg_Warn(f, "engine: %s", err);
        pm_engine_forget(sys->eng);
    }
    if (!prev) return NULL;                       /* the first frame waits for its successor */
    if (!contiguous) {                            /* a gap or a step back: the real frame alone, the pair restarts */
        sys->restarts++;
        sys->out++;
        return prev;
    }

    picture_t *head = prev, *tail = prev;         /* the real frame first, untouched */
    prev->p_next = NULL;
    sys->out++;
    if (pm_engine_have_pair(sys->eng)) {
        for (int j = 1; j < sys->k; j++) {
            picture_t *o = NewOut(f, sys);
            if (!o) break;
            picture_CopyProperties(o, prev);
            pm_yuv_out dst;
            PlanesOut(o, &f->fmt_out.video, sys->bps, &dst);
            pm_engine_generate(sys->eng, (float)j / (float)sys->k, &dst);
            o->date = prev->date + dt * j / sys->k;
            o->b_force = false;
            o->p_next = NULL;
            RegAdd(o);
            tail->p_next = o;
            tail = o;
            sys->out++;
            sys->generated++;
        }
    }
    return head;
}

static void Flush(filter_t *f)
{
    struct filter_sys_t *sys = (struct filter_sys_t *)f->p_sys;
    if (sys->prev) { picture_Release(sys->prev); sys->prev = NULL; }
    if (sys->eng) pm_engine_forget(sys->eng);
    if (sys->download) filter_chain_VideoFlush(sys->download);
}

static picture_t *DownloadBufferNew(filter_t *inner)
{
    filter_t *f = (filter_t *)inner->owner.sys;
    return picture_NewFromFormat(&f->fmt_out.video);
}

/* Hardware decoder output (D3D11 'DX11'/'DX10', VideoToolbox 'CVPN'/'CVPP', D3D9, ...): if VLC refused us, it would
 * put its own converter in front of us inside a "chain" filter, and that wrapper returns one picture per input, so
 * every generated frame would be dropped ("dropping pictures"). Instead we run VLC's converter ourselves and output
 * I420 / I0AL; VLC then converts our output back for the display after the static chain (CPU upload). */
static bool SetupDownload(filter_t *f, struct filter_sys_t *sys, int *bits)
{
    const vlc_fourcc_t c = f->fmt_in.video.i_chroma;
    const vlc_chroma_description_t *d = vlc_fourcc_GetChromaDescription(c);
    const bool ten = (d && d->pixel_bits > 8) || c == VLC_CODEC_D3D11_OPAQUE_10B || c == VLC_CODEC_D3D9_OPAQUE_10B ||
                     c == VLC_CODEC_CVPX_P010 || c == VLC_CODEC_VAAPI_420_10BPP;
    const vlc_fourcc_t target = ten ? VLC_CODEC_I420_10L : VLC_CODEC_I420;
    es_format_t out;
    es_format_Copy(&out, &f->fmt_in);
    out.i_codec = out.video.i_chroma = target;
    filter_owner_t owner = { .sys = f, .video = { .buffer_new = DownloadBufferNew } };
    f->fmt_out.i_codec = f->fmt_out.video.i_chroma = target;   /* DownloadBufferNew allocates in this format */
    sys->download = filter_chain_NewVideo(f, false, &owner);
    if (!sys->download) { es_format_Clean(&out); return false; }
    filter_chain_Reset(sys->download, &f->fmt_in, &out);
    const int r = filter_chain_AppendConverter(sys->download, &f->fmt_in, &out);
    es_format_Clean(&out);
    if (r != 0) {
        filter_chain_Delete(sys->download);
        sys->download = NULL;
        return false;
    }
    *bits = ten ? 10 : 8;
    return true;
}

static int Open(vlc_object_t *obj)
{
    filter_t *f = (filter_t *)obj;
    const video_format_t *v = &f->fmt_in.video;
    int bits = 8;
    struct filter_sys_t *sys = calloc(1, sizeof(*sys));
    if (!sys) return VLC_ENOMEM;
    f->fmt_out.video = f->fmt_in.video;
    f->fmt_out.i_codec = f->fmt_in.i_codec;
    if (!Supported(v->i_chroma, &bits)) {
        /* NV12 and hardware surfaces: convert in front of the engine with VLC's own converters (SetupDownload) */
        if (!SetupDownload(f, sys, &bits)) {
            msg_Dbg(f, "PushMPC: no converter from %4.4s", (const char *)&v->i_chroma);
            f->fmt_out.video = f->fmt_in.video;
            f->fmt_out.i_codec = f->fmt_in.i_codec;
            free(sys);
            return VLC_EGENERIC;
        }
        msg_Dbg(f, "PushMPC: %4.4s converted to %4.4s in front of the engine", (const char *)&v->i_chroma,
                (const char *)&f->fmt_out.video.i_chroma);
    }
    sys->bits = bits;
    sys->bps = bits > 8 ? 2 : 1;
    sys->alloc = (int)var_InheritInteger(f, CFG "alloc");
    const int smoother = (int)var_InheritInteger(f, CFG "smoother");
    const int quality = (int)var_InheritInteger(f, CFG "quality");
    const int threads = (int)var_InheritInteger(f, CFG "threads");
    double hz = var_InheritFloat(f, CFG "display-hz");
    if (hz <= 0) hz = pm_display_hz();
    const double fps = (v->i_frame_rate && v->i_frame_rate_base) ? (double)v->i_frame_rate / v->i_frame_rate_base : 0.0;
    sys->nominal_dt = fps > 0 ? (mtime_t)(CLOCK_FREQ / fps) : CLOCK_FREQ / 24;
    const int w = (int)v->i_visible_width, h = (int)v->i_visible_height;

    sys->k = pm_engine_decide_k(smoother, fps, hz, w, h, bits, threads, quality, sys->why, sizeof(sys->why));
    if (sys->k > 1) {
        pm_engine_cfg c = { w, h, bits, threads, quality };
        char err[200] = "";
        sys->eng = pm_engine_open(&c, err, sizeof(err));
        if (!sys->eng) {
            msg_Warn(f, "PushMPC: engine unavailable (%s): pass-through", err);
            sys->k = 1;
        }
    }

    if (f->fmt_out.video.i_frame_rate)
        f->fmt_out.video.i_frame_rate *= (unsigned)sys->k;
    f->p_sys = (void *)sys;
    f->pf_video_filter = Filter;
    f->pf_flush = Flush;
    msg_Info(f, "PushMPC %s: %4.4s %dx%d %d-bit %.3f fps, display %.2f Hz -> x%d (%s)%s", PUSHMPC_VLC_VERSION,
             (const char *)&v->i_chroma, w, h, bits, fps, hz, sys->k, sys->why,
             sys->eng ? "" : " [pass-through]");
    return VLC_SUCCESS;
}

static void Close(vlc_object_t *obj)
{
    filter_t *f = (filter_t *)obj;
    struct filter_sys_t *sys = (struct filter_sys_t *)f->p_sys;
    pm_engine_stats st = { 0 };
    if (sys->eng) pm_engine_stats_get(sys->eng, &st);
    msg_Info(f, "PushMPC: close, in=%" PRIu64 " out=%" PRIu64 " generated=%" PRIu64 " (pool %" PRIu64 ", heap %" PRIu64
             ", failed %" PRIu64 ") restarts=%" PRIu64 " cuts=%" PRIu64 " motion %.1f ms, frame %.1f ms",
             sys->in, sys->out, sys->generated, sys->pool_out, sys->heap_out, sys->alloc_fail, sys->restarts,
             st.cuts, st.me_ms, st.gen_ms);
    Flush(f);
    if (sys->eng) pm_engine_close(sys->eng);
    if (sys->download) filter_chain_Delete(sys->download);
    free(sys);
}

/* ---------------------------------------------------------------- pushmpc_out (interactive chain) */
typedef struct { uint64_t copied, passed, failed; } out_sys_t;

static picture_t *FilterOut(filter_t *f, picture_t *p)
{
    out_sys_t *s = (out_sys_t *)f->p_sys;
    if (!RegTake(p)) { s->passed++; return p; }
    picture_t *o = filter_NewPicture(f);  /* the last interactive filter allocates from the vout's private pool */
    if (!o) { s->failed++; return p; }
    picture_Copy(o, p);                   /* pixels and properties (date) */
    picture_Release(p);
    s->copied++;
    return o;
}

static int OpenOut(vlc_object_t *obj)
{
    filter_t *f = (filter_t *)obj;
    const vlc_chroma_description_t *d = vlc_fourcc_GetChromaDescription(f->fmt_in.video.i_chroma);
    if (!d || d->plane_count == 0 || f->fmt_in.video.i_chroma != f->fmt_out.video.i_chroma)
        return VLC_EGENERIC;
    out_sys_t *s = calloc(1, sizeof(*s));
    if (!s) return VLC_ENOMEM;
    f->p_sys = (void *)s;
    f->pf_video_filter = FilterOut;
    msg_Dbg(f, "PushMPC output stage on %4.4s", (const char *)&f->fmt_in.video.i_chroma);
    return VLC_SUCCESS;
}

static void CloseOut(vlc_object_t *obj)
{
    filter_t *f = (filter_t *)obj;
    out_sys_t *s = (out_sys_t *)f->p_sys;
    msg_Info(f, "PushMPC output stage: copied=%" PRIu64 " passed=%" PRIu64 " failed=%" PRIu64, s->copied, s->passed, s->failed);
    free(s);
}
