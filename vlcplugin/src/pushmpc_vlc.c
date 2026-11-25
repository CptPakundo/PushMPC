/*
 * PushMPC for VLC 3.0.x: frame generation in VLC's video output.
 *
 * Where it sits: VLC 3.0.x builds two filter chains in the video output.
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
#include "engine_pushcore.h"

#ifdef _WIN32
# include <stdatomic.h>
# include "pm_d3d11_picsys.h"   /* zero copy with VLC's Direct3D 11 hardware decoding */

/* Zero copy above x4: VLC's private pool holds 4 pictures, so generated pictures come from a ring of our own textures on
 * VLC's device, made like the pictures of VLC's Direct3D 11 pool (same size, format, bind flags; one texture each, with
 * the luma and chroma shader views VLC's outputs draw from). A picture holds its slot and the ring until VLC releases
 * it, so pictures may outlive the filter. */
#define PM_RING_MAX 32
typedef struct pm_ring {
    atomic_int refs;
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    D3D11_TEXTURE2D_DESC desc;
    DXGI_FORMAT view_fmt[2];
    struct { ID3D11Texture2D *tex; ID3D11ShaderResourceView *srv[2]; atomic_bool busy; } slot[PM_RING_MAX];
    int size;
} pm_ring;
typedef struct { pm_d3d11_picsys sys; pm_ring *ring; int idx; } pm_ring_pic;   /* sys first: VLC reads it as its own */

static void RingRelease(pm_ring *r)
{
    if (atomic_fetch_sub(&r->refs, 1) != 1) return;
    for (int i = 0; i < r->size; i++) {
        for (int v = 0; v < 2; v++) if (r->slot[i].srv[v]) ID3D11ShaderResourceView_Release(r->slot[i].srv[v]);
        if (r->slot[i].tex) ID3D11Texture2D_Release(r->slot[i].tex);
    }
    ID3D11DeviceContext_Release(r->ctx);
    ID3D11Device_Release(r->dev);
    free(r);
}

static pm_ring *RingNew(ID3D11Texture2D *like, ID3D11DeviceContext *ctx, int size)
{
    pm_ring *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    atomic_init(&r->refs, 1);
    ID3D11Texture2D_GetDesc(like, &r->desc);
    r->desc.ArraySize = 1;
    r->desc.MipLevels = 1;
    r->desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    r->desc.Usage = D3D11_USAGE_DEFAULT;
    r->desc.CPUAccessFlags = 0;
    r->desc.MiscFlags = 0;
    const bool p010 = r->desc.Format == DXGI_FORMAT_P010;
    r->view_fmt[0] = p010 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    r->view_fmt[1] = p010 ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
    ID3D11DeviceContext_GetDevice(ctx, &r->dev);
    r->ctx = ctx;
    ID3D11DeviceContext_AddRef(ctx);
    r->size = size > PM_RING_MAX ? PM_RING_MAX : size;
    for (int i = 0; i < r->size; i++) atomic_init(&r->slot[i].busy, false);
    return r;
}

static void RingPicDestroy(picture_t *p)
{
    pm_ring_pic *rp = (pm_ring_pic *)p->p_sys;
    atomic_store(&rp->ring->slot[rp->idx].busy, false);
    RingRelease(rp->ring);
    free(rp);
    free(p);
}

/* a free slot as a 'DX11'/'DX10' picture, NULL when all are on their way to the display */
static picture_t *RingGet(pm_ring *r, const video_format_t *fmt)
{
    for (int i = 0; i < r->size; i++) {
        bool expected = false;
        if (!atomic_compare_exchange_strong(&r->slot[i].busy, &expected, true)) continue;
        if (!r->slot[i].tex) {   /* made on first use */
            if (FAILED(ID3D11Device_CreateTexture2D(r->dev, &r->desc, NULL, &r->slot[i].tex))) {
                r->slot[i].tex = NULL; atomic_store(&r->slot[i].busy, false); return NULL;
            }
            for (int v = 0; v < 2; v++) {
                D3D11_SHADER_RESOURCE_VIEW_DESC sd;
                memset(&sd, 0, sizeof(sd));
                sd.Format = r->view_fmt[v];
                sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                sd.Texture2D.MipLevels = 1;
                if (FAILED(ID3D11Device_CreateShaderResourceView(r->dev, (ID3D11Resource *)r->slot[i].tex, &sd, &r->slot[i].srv[v])))
                    r->slot[i].srv[v] = NULL;
            }
        }
        pm_ring_pic *rp = calloc(1, sizeof(*rp));
        if (!rp) { atomic_store(&r->slot[i].busy, false); return NULL; }
        rp->sys.texture[0] = rp->sys.texture[1] = r->slot[i].tex;   /* as VLC's pool: one texture for both planes */
        rp->sys.context = r->ctx;
        rp->sys.slice_index = 0;
        rp->sys.resourceView[0] = r->slot[i].srv[0];
        rp->sys.resourceView[1] = r->slot[i].srv[1];
        rp->sys.formatTexture = r->desc.Format;
        rp->ring = r;
        rp->idx = i;
        picture_resource_t res;
        memset(&res, 0, sizeof(res));
        res.p_sys = (picture_sys_t *)rp;
        res.pf_destroy = RingPicDestroy;
        picture_t *p = picture_NewFromResource(fmt, &res);
        if (!p) { free(rp); atomic_store(&r->slot[i].busy, false); return NULL; }
        atomic_fetch_add(&r->refs, 1);
        return p;
    }
    return NULL;
}
#endif

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

/* Options: when the plugin was loaded through VLC's plugin bank they are registered config items; when it was brought
 * in by the Snap bootstrap (pm_snap_boot.c) they are not, and the values come from pushmpc.conf (or the defaults). */
const char *pm_snap_conf(const char *key);   /* pm_snap_boot.c; NULL when not in snap mode or unset */
static int64_t OptInt(vlc_object_t *o, const char *name, int64_t def)
{
    if (config_GetType(name)) return var_InheritInteger(o, name);
    const char *v = pm_snap_conf(name + sizeof(CFG) - 1);
    return v ? strtoll(v, NULL, 10) : def;
}
static double OptFloat(vlc_object_t *o, const char *name, double def)
{
    if (config_GetType(name)) return var_InheritFloat(o, name);
    const char *v = pm_snap_conf(name + sizeof(CFG) - 1);
    return v ? strtod(v, NULL) : def;
}
#define OptInt(o, n, d) OptInt(VLC_OBJECT(o), n, d)
#define OptFloat(o, n, d) OptFloat(VLC_OBJECT(o), n, d)
/* the filter callbacks, for the Snap bootstrap */
int pm_filter_open(vlc_object_t *o) { return Open(o); }
void pm_filter_close(vlc_object_t *o) { Close(o); }
int pm_out_open(vlc_object_t *o) { return OpenOut(o); }
void pm_out_close(vlc_object_t *o) { CloseOut(o); }

/* ---- the worker (generation off VLC's video output thread)
 * With pushmpc_out in the interactive chain, the static filter returns the real frame plus k - 1 still-empty
 * pictures at once; a worker thread fills them in display order, and pushmpc_out, which runs for each picture just
 * before it is shown, waits until that picture is filled (at most until its display time; if it is not ready by
 * then, the real frame is shown in its place). So generated frame j has j/k of the frame interval to be made, instead
 * of everything having to be ready before VLC's video thread may go on. */
typedef struct pm_job {
    vlc_mutex_t lock;
    vlc_cond_t cond;
    int refs;
    int n;                      /* pictures to generate */
    int done;                   /* filled so far, in order */
    bool finished, cancel, contiguous;
    picture_t *src;             /* the newest source frame (pushed to the engine by the worker) */
    picture_t *real;            /* the real frame shown before them (the fallback) */
    picture_t *outs[16];
    float t[16];
} pm_job;

static pm_job *JobNew(void)
{
    pm_job *j = calloc(1, sizeof(*j));
    if (!j) return NULL;
    vlc_mutex_init(&j->lock);
    vlc_cond_init(&j->cond);
    j->refs = 1;
    return j;
}
static void JobHold(pm_job *j) { vlc_mutex_lock(&j->lock); j->refs++; vlc_mutex_unlock(&j->lock); }
static void JobRelease(pm_job *j)
{
    vlc_mutex_lock(&j->lock);
    const int left = --j->refs;
    vlc_mutex_unlock(&j->lock);
    if (left) return;
    for (int i = 0; i < j->n; i++) if (j->outs[i]) picture_Release(j->outs[i]);
    if (j->src) picture_Release(j->src);
    if (j->real) picture_Release(j->real);
    vlc_cond_destroy(&j->cond);
    vlc_mutex_destroy(&j->lock);
    free(j);
}

/* generated pictures on their way to pushmpc_out (a ring; a stale entry only costs one needless copy) */
typedef struct { picture_t *pic; pm_job *job; int idx; } reg_entry;
static vlc_mutex_t reg_lock = VLC_STATIC_MUTEX;
static reg_entry reg[512];
static unsigned reg_next;
static int out_stages;          /* pushmpc_out instances alive: the worker mode needs one */
static void RegAdd(picture_t *p, pm_job *job, int idx)
{
    vlc_mutex_lock(&reg_lock);
    reg_entry *e = &reg[reg_next++ % (sizeof(reg) / sizeof(reg[0]))];
    pm_job *old = e->job;
    e->pic = p; e->job = job; e->idx = idx;
    vlc_mutex_unlock(&reg_lock);
    if (old) JobRelease(old);   /* an entry pushmpc_out never took (a seek dropped the picture) */
}
static bool RegTake(picture_t *p, pm_job **job, int *idx)
{
    bool hit = false;
    *job = NULL;
    vlc_mutex_lock(&reg_lock);
    for (size_t i = 0; i < sizeof(reg) / sizeof(reg[0]); i++)
        if (reg[i].pic == p) { *job = reg[i].job; *idx = reg[i].idx; reg[i].pic = NULL; reg[i].job = NULL; hit = true; break; }
    vlc_mutex_unlock(&reg_lock);
    return hit;
}
static int OutStages(void) { vlc_mutex_lock(&reg_lock); int n = out_stages; vlc_mutex_unlock(&reg_lock); return n; }

static const int smoother_values[] = { 0, 1, 2, 3, 4, 5, 6, 8 };
static const char *const smoother_texts[] = { "Auto (display refresh, CPU budget)", "Off", "x2", "x3", "x4", "x5", "x6", "x8" };
static const int quality_values[] = { 0, 1 };
static const char *const quality_texts[] = { "Fast", "Balanced" };
static const int engine_values[] = { 0, 1, 2, 3 };
static const char *const engine_texts[] = { "Auto", "Portable on the CPU", "pushcore (Windows)", "Portable on the GPU (Vulkan)" };
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
    add_integer(CFG "engine", 0, "Engine", "Auto: pushcore on Windows when PushMPC's core is installed (NVIDIA VFG, "
                "DLSS-G, PushFlow, FSR FG, AMD FRC, with the scene-cut guard; HD and UHD sources), else the portable "
                "engine on the GPU (Vulkan; MoltenVK on macOS), else on the CPU.", true)
        change_integer_list(engine_values, engine_texts)
    add_bool(CFG "warp", false, "pushcore on WARP (tests)", NULL, true)
        change_private()
    add_bool(CFG "worker", true, "Generate on a worker thread", "Frames are made off VLC's video thread and each one "
             "has until its display time (needs pushmpc_out after postproc).", true)
    add_integer(CFG "alloc", 0, "Output pictures", "Where generated pictures come from (diagnostics)", true)
        change_integer_list(alloc_values, alloc_texts)
    add_bool(CFG "zerocopy", true, "Zero copy with Direct3D 11 decoding", "Windows, pushcore: hardware-decoded "
             "pictures stay on the GPU (pushcore works on VLC's own D3D11 device); off = they are copied to the CPU "
             "and back, as before.", true)
    add_bool(CFG "zerocopy-ring", true, "Own output pictures with zero copy", "Generated pictures come from textures "
             "of the module's own (up to x8); off = VLC's 4-picture pool (at most x4).", true)

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
    pm_pc *pc;                 /* Windows: pushcore through pm_pushcore.dll (else the portable engine, eng) */
    bool zc;                   /* pc runs on VLC's D3D11 device with 'DX11'/'DX10' pictures (no download) */
    uint64_t zc_fallback;      /* generated pictures pushcore did not deliver in time (the real frame copied in) */
#ifdef _WIN32
    pm_ring *ring;             /* zero copy: our own output pictures (beyond VLC's 4-picture pool) */
#endif
    uint64_t ring_out;
    uint64_t pc_errors, pc_routed, pc_unrouted;
    char pc_err[240];
    int64_t pc_miss_pts, pc_miss_want;
    pm_engine *eng;
    int k;                 /* outputs per source frame */
    int alloc;
    int bits, bps;
    picture_t *prev;
    mtime_t nominal_dt;
    uint64_t in, out, generated, pool_out, heap_out, alloc_fail, restarts, async_jobs, cancelled;
    char why[256];
    /* worker */
    bool worker_on, worker_exit;
    vlc_thread_t worker;
    vlc_mutex_t wlock;
    vlc_cond_t wcond;
    pm_job *queued;             /* handed to the worker, not yet taken */
    pm_job *active;             /* the last job submitted (the worker may still run it) */
    int bps_w;
    video_format_t wfmt;
};

/* the worker thread: one job at a time, in order (the engine keeps the pair state) */
static void PlanesIn(const picture_t *p, const video_format_t *f, int bps, pm_yuv *y);
static void PlanesOut(picture_t *p, const video_format_t *f, int bps, pm_yuv_out *y);
/* pushcore hands out generated frames by pts: route each into the job's picture with that time, in display order */
typedef struct { struct filter_sys_t *sys; pm_job *j; bool filled[16]; pm_pc_out dst; pm_pc_tex tex; } pc_route;
static int RouteIndex(pc_route *r, int64_t pts100ns)
{
    pm_job *j = r->j;
    for (int i = 0; i < j->n; i++) {
        const int64_t want = (int64_t)j->outs[i]->date * 10;
        const int64_t tol = j->n > 0 ? ((int64_t)(j->outs[0]->date - j->real->date) * 10) / 2 : 0;
        if (!r->filled[i] && (want - pts100ns <= tol && pts100ns - want <= tol)) return i;
    }
    return -1;
}
static const pm_pc_out *RouteDest(void *user, int64_t pts100ns)
{
    pc_route *r = user;
    const int i = RouteIndex(r, pts100ns);
    if (i < 0) {
        if (!r->sys->pc_unrouted++) { r->sys->pc_miss_pts = pts100ns; r->sys->pc_miss_want = r->j->n ? (int64_t)r->j->outs[0]->date * 10 : 0; }
        return NULL;
    }
    r->sys->pc_routed++;
    pm_yuv_out o;
    PlanesOut(r->j->outs[i], &r->sys->wfmt, r->sys->bps_w, &o);
    for (int p = 0; p < 3; p++) { r->dst.data[p] = o.data[p]; r->dst.stride[p] = o.stride[p]; }
    return &r->dst;
}
static void RouteDone(void *user, int64_t pts100ns)
{
    pc_route *r = user;
    const int i = RouteIndex(r, pts100ns);
    if (i < 0) return;
    r->filled[i] = true;
    int run = 0;
    while (run < r->j->n && r->filled[run]) run++;
    vlc_mutex_lock(&r->j->lock);
    r->j->done = run;
    vlc_cond_broadcast(&r->j->cond);
    vlc_mutex_unlock(&r->j->lock);
}

/* one job: push the newest source frame, then make the job's pictures in display order */
#ifdef _WIN32
/* zero copy: generated frames go straight into the job's pictures (textures of VLC's D3D11 pool) */
static const pm_pc_tex *RouteDestTex(void *user, int64_t pts100ns)
{
    pc_route *r = user;
    const int i = RouteIndex(r, pts100ns);
    if (i < 0) {
        if (!r->sys->pc_unrouted++) { r->sys->pc_miss_pts = pts100ns; r->sys->pc_miss_want = r->j->n ? (int64_t)r->j->outs[0]->date * 10 : 0; }
        return NULL;
    }
    pm_d3d11_picsys *ps = PicSys(r->j->outs[i]);
    if (!ps || !ps->texture[0]) return NULL;
    r->sys->pc_routed++;
    r->tex.texture = ps->texture[0];
    r->tex.slice = ps->slice_index;
    return &r->tex;
}

static void RunJobZc(struct filter_sys_t *sys, pm_job *j)
{
    char err[200];
    pm_d3d11_picsys *in = PicSys(j->src);
    if (!in || !in->texture[0]) {
        if (!sys->pc_errors++) snprintf(sys->pc_err, sizeof(sys->pc_err), "a picture without a D3D11 texture");
        return;
    }
    if (pmx_push_d3d11(sys->pc, in->texture[0], in->slice_index, (int64_t)j->src->date * 10, !j->contiguous, err, sizeof(err)) != 0) {
        if (!sys->pc_errors++) snprintf(sys->pc_err, sizeof(sys->pc_err), "push: %s", err);
        return;
    }
    pc_route r = { .sys = sys, .j = j };
    /* inline: the pictures go out when this returns, so wait at most until the last one is due */
    const mtime_t deadline = j->n > 0 ? j->outs[j->n - 1]->date : 0;
    while (j->n > 0) {
        vlc_mutex_lock(&j->lock);
        const bool stop = j->cancel || j->done >= j->n;
        vlc_mutex_unlock(&j->lock);
        if (stop || mdate() > deadline) break;
        if (pmx_poll_d3d11(sys->pc, 5, RouteDestTex, RouteDone, &r, err, sizeof(err)) < 0) {
            if (!sys->pc_errors++) snprintf(sys->pc_err, sizeof(sys->pc_err), "poll: %s", err);
            break;
        }
    }
    for (int guard = 0; guard < 64; guard++)
        if (pmx_poll_d3d11(sys->pc, 0, RouteDestTex, RouteDone, &r, err, sizeof(err)) <= 0) break;
    /* what pushcore did not deliver in time shows the real frame (copied on the GPU), never a stale pool texture */
    pm_d3d11_picsys *real = j->real ? PicSys(j->real) : NULL;
    for (int i = 0; i < j->n; i++) {
        if (r.filled[i]) continue;
        pm_d3d11_picsys *o = PicSys(j->outs[i]);
        if (real && real->texture[0] && o && o->texture[0])
            pmx_copy_d3d11(sys->pc, real->texture[0], real->slice_index, o->texture[0], o->slice_index);
        sys->zc_fallback++;
    }
    vlc_mutex_lock(&j->lock);
    j->done = j->n;
    vlc_cond_broadcast(&j->cond);
    vlc_mutex_unlock(&j->lock);
}
#endif

static void RunJob(struct filter_sys_t *sys, pm_job *j)
{
    pm_yuv src;
    char err[200];
#ifdef _WIN32
    if (sys->zc) RunJobZc(sys, j);
    else
#endif
    if (sys->pc) {
        PlanesIn(j->src, &sys->wfmt, sys->bps_w, &src);
        pm_pc_planes pl;
        for (int p = 0; p < 3; p++) { pl.data[p] = src.data[p]; pl.stride[p] = src.stride[p]; }
        if (pmx_push(sys->pc, &pl, (int64_t)j->src->date * 10, !j->contiguous, err, sizeof(err)) != 0) {
            if (!sys->pc_errors++) snprintf(sys->pc_err, sizeof(sys->pc_err), "push: %s", err);
        } else {
            /* collect until every picture is filled or the last one's display time has passed, then take whatever else
             * pushcore has finished (its real frames above all: they hold its queue until they are returned) */
            pc_route r = { .sys = sys, .j = j };
            const mtime_t deadline = j->n > 0 ? j->outs[j->n - 1]->date : 0;
            while (j->n > 0) {
                vlc_mutex_lock(&j->lock);
                const bool stop = j->cancel || j->done >= j->n;
                vlc_mutex_unlock(&j->lock);
                if (stop || mdate() > deadline) break;
                if (pmx_poll(sys->pc, 5, RouteDest, RouteDone, &r, err, sizeof(err)) < 0) {
                    if (!sys->pc_errors++) snprintf(sys->pc_err, sizeof(sys->pc_err), "poll: %s", err);
                    break;
                }
            }
            for (int guard = 0; guard < 64; guard++)
                if (pmx_poll(sys->pc, 0, RouteDest, RouteDone, &r, err, sizeof(err)) <= 0) break;
        }
    } else {
        PlanesIn(j->src, &sys->wfmt, sys->bps_w, &src);
        if (pm_engine_push(sys->eng, &src, j->contiguous, err, sizeof(err)) != 0)
            pm_engine_forget(sys->eng);
        const bool pair = pm_engine_have_pair(sys->eng);
        for (int i = 0; i < j->n; i++) {
            vlc_mutex_lock(&j->lock);
            const bool stop = j->cancel;
            vlc_mutex_unlock(&j->lock);
            if (stop || !pair) break;
            pm_yuv_out dst;
            PlanesOut(j->outs[i], &sys->wfmt, sys->bps_w, &dst);
            pm_engine_generate(sys->eng, j->t[i], &dst);
            vlc_mutex_lock(&j->lock);
            j->done = i + 1;
            vlc_cond_broadcast(&j->cond);
            vlc_mutex_unlock(&j->lock);
        }
    }
    vlc_mutex_lock(&j->lock);
    j->finished = true;
    vlc_cond_broadcast(&j->cond);
    vlc_mutex_unlock(&j->lock);
}

static void *Worker(void *data)
{
    struct filter_sys_t *sys = data;
    for (;;) {
        vlc_mutex_lock(&sys->wlock);
        while (!sys->queued && !sys->worker_exit)
            vlc_cond_wait(&sys->wcond, &sys->wlock);
        if (sys->worker_exit && !sys->queued) { vlc_mutex_unlock(&sys->wlock); break; }
        pm_job *j = sys->queued;
        sys->queued = NULL;
        vlc_mutex_unlock(&sys->wlock);
        RunJob(sys, j);
        JobRelease(j);          /* the worker's reference */
    }
    return NULL;
}

/* wait until the worker is idle (cancelling what is left of the last job when cancel is set) */
static void WorkerIdle(struct filter_sys_t *sys, bool cancel)
{
    if (!sys->worker_on) return;
    vlc_mutex_lock(&sys->wlock);
    pm_job *q = sys->queued;
    sys->queued = NULL;
    vlc_mutex_unlock(&sys->wlock);
    if (q) {                    /* never started: finish it as empty */
        vlc_mutex_lock(&q->lock); q->finished = true; vlc_cond_broadcast(&q->cond); vlc_mutex_unlock(&q->lock);
        JobRelease(q);
    }
    pm_job *a = sys->active;
    if (!a) return;
    vlc_mutex_lock(&a->lock);
    if (cancel && !a->finished) { a->cancel = true; sys->cancelled++; }
    while (!a->finished)
        vlc_cond_wait(&a->cond, &a->lock);
    vlc_mutex_unlock(&a->lock);
    JobRelease(a);
    sys->active = NULL;
}

static void Submit(struct filter_sys_t *sys, pm_job *j)
{
    JobHold(j);                 /* the worker's reference */
    sys->active = j;            /* the filter's reference (from JobNew) */
    vlc_mutex_lock(&sys->wlock);
    sys->queued = j;
    vlc_cond_signal(&sys->wcond);
    vlc_mutex_unlock(&sys->wlock);
    sys->async_jobs++;
}

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
#ifdef _WIN32
    if (sys->ring) {            /* zero copy: our own D3D11 pictures first, VLC's pool when the ring is all in use */
        picture_t *r = RingGet(sys->ring, &f->fmt_out.video);
        if (r) { sys->ring_out++; return r; }
    }
#endif
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
    if (sys->k <= 1 || (!sys->eng && !sys->pc)) { sys->out++; return cur; }

    const bool async = sys->worker_on && OutStages() > 0 && !sys->zc;
    if (async || sys->pc) {     /* pushcore always runs through jobs (inline when there is no worker) */
        /* worker mode: the previous job has had most of a frame interval; its last picture may still be on its way
         * (VLC asks for the next source frame while that picture is queued as "next"), so wait for it to finish */
        WorkerIdle(sys, false);
        picture_t *prev = sys->prev;
        sys->prev = cur;
        const mtime_t dt = prev ? cur->date - prev->date : 0;
        const bool contiguous = prev && dt > 0 && dt <= 4 * sys->nominal_dt && dt <= CLOCK_FREQ / 4;
        pm_job *j = JobNew();
        if (!j) { if (prev) { sys->out++; prev->p_next = NULL; } return prev; }
        j->src = picture_Hold(cur);
        j->contiguous = contiguous;
        picture_t *head = prev, *tail = prev;
        if (prev && contiguous) {
            prev->p_next = NULL;
            j->real = picture_Hold(prev);
            for (int i = 0; i < sys->k - 1 && i < 16; i++) {
                picture_t *o = NewOut(f, sys);
                if (!o) break;
                picture_CopyProperties(o, prev);
                o->date = prev->date + dt * (i + 1) / sys->k;
                o->b_force = false;
                o->p_next = NULL;
                j->outs[i] = picture_Hold(o);       /* the worker writes into it */
                j->t[i] = (float)(i + 1) / (float)sys->k;
                j->n = i + 1;
                if (async) {                        /* pushmpc_out waits for j->done >= i + 1 */
                    JobHold(j);
                    RegAdd(o, j, i + 1);
                }                                   /* inline: nobody takes the entry, so none (it would hold the pool) */
                tail->p_next = o;
                tail = o;
                sys->generated++;
            }
        } else if (prev) {
            sys->restarts++;
            prev->p_next = NULL;
        }
        if (async) Submit(sys, j);
        else { RunJob(sys, j); JobRelease(j); }
        if (!prev) return NULL;
        for (picture_t *p = head; p; p = p->p_next) sys->out++;
        return head;
    }
    WorkerIdle(sys, false);     /* synchronous mode below; the engine must not be shared */

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
            RegAdd(o, NULL, 0);
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
    WorkerIdle(sys, true);
    if (sys->prev) { picture_Release(sys->prev); sys->prev = NULL; }
    if (sys->eng) pm_engine_forget(sys->eng);
    if (sys->pc) pmx_flush(sys->pc);
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

static void FillPcc(pm_pc_cfg *pcc, const video_format_t *v, int bits, int smoother, double hz, int warp)
{
    const int w = (int)v->i_visible_width, h = (int)v->i_visible_height;
    memset(pcc, 0, sizeof(*pcc));
    pcc->abi = PM_PC_ABI;
    pcc->width = (uint32_t)w; pcc->height = (uint32_t)h; pcc->bits = bits;
    pcc->fps_num = (int)v->i_frame_rate; pcc->fps_den = (int)v->i_frame_rate_base;
    const bool uhd = w >= 3200 || h >= 1800;
    pcc->matrix = v->space == COLOR_SPACE_BT2020 ? 9 : v->space == COLOR_SPACE_BT601 ? 6 :
                  v->space == COLOR_SPACE_BT709 ? 1 : (w >= 1024 || h >= 600 ? (uhd && bits > 8 ? 9 : 1) : 6);
    pcc->transfer = v->transfer == TRANSFER_FUNC_SMPTE_ST2084 ? 16 : v->transfer == TRANSFER_FUNC_HLG ? 18 : 1;
    pcc->primaries = v->primaries == COLOR_PRIMARIES_BT2020 ? 9 : pcc->matrix == 9 ? 9 : 1;
    pcc->range = v->b_color_range_full ? 2 : 1;
    pcc->smoother = smoother;
    pcc->display_hz = hz;
    pcc->warp = warp;
}

#ifdef _WIN32
/* zero copy: pushcore on the D3D11 device of VLC's own pictures (a picture of the vout's pool tells which), with VLC's
 * context lock */
static pm_pc *ZcOpen(filter_t *f, const pm_pc_cfg *pcc, int *k, pm_ring **ring, char *err, size_t n)
{
    *ring = NULL;
    picture_t *p = filter_NewPicture(f);
    pm_d3d11_picsys *ps = p ? PicSys(p) : NULL;
    if (!ps || !ps->context || !ps->texture[0]) {
        snprintf(err, n, "no D3D11 picture from VLC's pool");
        if (p) picture_Release(p);
        return NULL;
    }
    const DXGI_FORMAT want = pcc->bits > 8 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc(ps->texture[0], &td);
    if (td.Format != want) {
        snprintf(err, n, "VLC's pictures are DXGI format %d, not %s", (int)td.Format, pcc->bits > 8 ? "P010" : "NV12");
        picture_Release(p);
        return NULL;
    }
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext_GetDevice(ps->context, &dev);
    HANDLE mutex = NULL;
    UINT size = sizeof(mutex);
    if (FAILED(ID3D11DeviceContext_GetPrivateData(ps->context, &PM_GUID_CONTEXT_MUTEX, &size, &mutex)) || size != sizeof(mutex))
        mutex = NULL;
    if (!dev) { picture_Release(p); snprintf(err, n, "no D3D11 device"); return NULL; }
    pm_pc *pc = pmx_open_d3d11(pcc, dev, mutex, k, err, n);
    ID3D11Device_Release(dev);   /* pushcore holds its own reference */
    /* our own output pictures, so the multiplier is not capped by VLC's 4-picture pool (pushmpc-zerocopy-ring=0: the
     * pool only) */
    if (pc && *k > 1 && OptInt(VLC_OBJECT(f), CFG "zerocopy-ring", 1))
        *ring = RingNew(ps->texture[0], ps->context, 3 * *k + 4);
    picture_Release(p);
    return pc;
}
#endif

static int Open(vlc_object_t *obj)
{
    filter_t *f = (filter_t *)obj;
    const video_format_t *v = &f->fmt_in.video;
    int bits = 8;
    struct filter_sys_t *sys = calloc(1, sizeof(*sys));
    if (!sys) return VLC_ENOMEM;
    f->fmt_out.video = f->fmt_in.video;
    f->fmt_out.i_codec = f->fmt_in.i_codec;
#ifdef _WIN32
    {   /* hardware-decoded D3D11 pictures with pushcore: no copies at all (pushmpc-zerocopy=0 = the copy path) */
        const int engine = (int)OptInt(f, CFG "engine", 0), smoother = (int)OptInt(f, CFG "smoother", 0);
        if ((v->i_chroma == VLC_CODEC_D3D11_OPAQUE || v->i_chroma == VLC_CODEC_D3D11_OPAQUE_10B) &&
            (engine == 0 || engine == 2) && smoother != 1 && OptInt(f, CFG "zerocopy", 1) && pmx_has_d3d11()) {
            double zhz = OptFloat(f, CFG "display-hz", 0.0);
            if (zhz <= 0) zhz = pm_display_hz();
            pm_pc_cfg pcc;
            FillPcc(&pcc, v, v->i_chroma == VLC_CODEC_D3D11_OPAQUE_10B ? 10 : 8, smoother, zhz, OptInt(f, CFG "warp", 0) ? 1 : 0);
            int pk = 1;
            char err[300] = "";
            sys->pc = ZcOpen(f, &pcc, &pk, &sys->ring, err, sizeof(err));
            if (sys->pc && pk > 1) {
                sys->zc = true;
                bits = pcc.bits;
                sys->k = pk > 8 ? 8 : pk;
                snprintf(sys->why, sizeof(sys->why), "pushcore: %s", pmx_describe(sys->pc));
            } else {
                if (sys->pc) { pmx_close(sys->pc); sys->pc = NULL; snprintf(err, sizeof(err), "pushcore runs x1 here"); }
                msg_Dbg(f, "PushMPC: zero copy not used (%s): the copy path", err);
            }
        }
    }
#endif
    if (!sys->zc && !Supported(v->i_chroma, &bits)) {
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
    sys->alloc = (int)OptInt(f, CFG "alloc", 0);
    const int smoother = (int)OptInt(f, CFG "smoother", 0);
    const int quality = (int)OptInt(f, CFG "quality", 1);
    const int threads = (int)OptInt(f, CFG "threads", 0);
    double hz = OptFloat(f, CFG "display-hz", 0.0);
    if (hz <= 0) hz = pm_display_hz();
    const double fps = (v->i_frame_rate && v->i_frame_rate_base) ? (double)v->i_frame_rate / v->i_frame_rate_base : 0.0;
    sys->nominal_dt = fps > 0 ? (mtime_t)(CLOCK_FREQ / fps) : CLOCK_FREQ / 24;
    const int w = (int)v->i_visible_width, h = (int)v->i_visible_height;

    sys->wfmt = f->fmt_out.video;
    sys->bps_w = sys->bps;
    const int engine = (int)OptInt(f, CFG "engine", 0);
    if (!sys->zc && (engine == 0 || engine == 2)) {   /* pushcore first (Windows, PushMPC's core installed); else the portable engines */
        pm_pc_cfg pcc;
        FillPcc(&pcc, v, bits, smoother, hz, OptInt(f, CFG "warp", 0) ? 1 : 0);
        int pk = 1;
        char err[300] = "";
        sys->pc = pmx_open(&pcc, &pk, err, sizeof(err));
        if (sys->pc) {
            sys->k = pk < 1 ? 1 : pk > 8 ? 8 : pk;
            snprintf(sys->why, sizeof(sys->why), "pushcore: %s", pmx_describe(sys->pc));
            if (sys->k <= 1) { pmx_close(sys->pc); sys->pc = NULL; sys->k = 1; }
        } else if (engine == 2) {
            msg_Warn(f, "PushMPC: pushcore unavailable (%s)", err);
        } else {
            msg_Dbg(f, "PushMPC: pushcore not used (%s): portable engine", err);
        }
    }
    if (!sys->pc && smoother != 1) {
        /* the portable engines: the GPU one (Vulkan) when a device answers, else the CPU one; Auto then measures the
         * engine that runs */
        pm_engine_cfg c = { w, h, bits, threads, quality, engine == 1 ? PM_ENGINE_CPU : engine == 3 ? PM_ENGINE_GPU : PM_ENGINE_AUTO };
        char err[200] = "";
        sys->eng = pm_engine_open(&c, err, sizeof(err));
        if (err[0]) msg_Dbg(f, "PushMPC: %s", err);
        if (!sys->eng) {
            msg_Warn(f, "PushMPC: engine unavailable (%s): pass-through", err);
            sys->k = 1;
        } else {
            char why[256] = "";
            sys->k = pm_engine_decide(sys->eng, smoother, fps, hz, why, sizeof(why));
            snprintf(sys->why, sizeof(sys->why), "%s; %s", pm_engine_name(sys->eng), why);
            if (sys->k <= 1) { pm_engine_close(sys->eng); sys->eng = NULL; sys->k = 1; }
        }
    } else if (!sys->pc) {
        sys->k = 1;
        snprintf(sys->why, sizeof(sys->why), "Smoother off: no frame generation");
    }

    if (f->fmt_out.video.i_frame_rate)
        f->fmt_out.video.i_frame_rate *= (unsigned)sys->k;
    sys->wfmt = f->fmt_out.video;
    if ((sys->eng || sys->pc) && sys->k > 1 && OptInt(f, CFG "worker", 1)) {
        vlc_mutex_init(&sys->wlock);
        vlc_cond_init(&sys->wcond);
        if (vlc_clone(&sys->worker, Worker, sys, VLC_THREAD_PRIORITY_VIDEO) == 0) sys->worker_on = true;
        else { vlc_cond_destroy(&sys->wcond); vlc_mutex_destroy(&sys->wlock); }
    }
    f->p_sys = (void *)sys;
    f->pf_video_filter = Filter;
    f->pf_flush = Flush;
    msg_Info(f, "PushMPC %s: %4.4s %dx%d %d-bit %.3f fps, display %.2f Hz -> x%d (%s)%s", PUSHMPC_VLC_VERSION,
             (const char *)&v->i_chroma, w, h, bits, fps, hz, sys->k, sys->why,
             (sys->eng || sys->pc) ? "" : " [pass-through]");
    return VLC_SUCCESS;
}

static void Close(vlc_object_t *obj)
{
    filter_t *f = (filter_t *)obj;
    struct filter_sys_t *sys = (struct filter_sys_t *)f->p_sys;
    pm_engine_stats st = { 0 };
    Flush(f);                   /* the worker is idle after this */
    if (sys->eng) pm_engine_stats_get(sys->eng, &st);
    if (sys->worker_on) {
        vlc_mutex_lock(&sys->wlock);
        sys->worker_exit = true;
        vlc_cond_signal(&sys->wcond);
        vlc_mutex_unlock(&sys->wlock);
        vlc_join(sys->worker, NULL);
        vlc_cond_destroy(&sys->wcond);
        vlc_mutex_destroy(&sys->wlock);
    }
    msg_Info(f, "PushMPC: close, in=%" PRIu64 " out=%" PRIu64 " generated=%" PRIu64 " (pool %" PRIu64 ", heap %" PRIu64
             ", failed %" PRIu64 ") restarts=%" PRIu64 " cuts=%" PRIu64 " motion %.1f ms, frame %.1f ms, worker %s "
             "(jobs %" PRIu64 ", cut short %" PRIu64 ")",
             sys->in, sys->out, sys->generated, sys->pool_out, sys->heap_out, sys->alloc_fail, sys->restarts,
             st.cuts, st.me_ms, st.gen_ms, sys->worker_on ? "on" : "off", sys->async_jobs, sys->cancelled);
    if (sys->eng) pm_engine_close(sys->eng);
    if (sys->pc) {
        msg_Info(f, "PushMPC: pushcore routed %" PRIu64 " generated frames, %" PRIu64 " not matched (first at %" PRId64
                 " for %" PRId64 "), %" PRIu64 " errors%s%s", sys->pc_routed, sys->pc_unrouted, sys->pc_miss_pts, sys->pc_miss_want,
                 sys->pc_errors, sys->pc_errors ? ", first: " : "", sys->pc_err);
        if (sys->zc)
            msg_Info(f, "PushMPC: zero copy on VLC's D3D11 device; %" PRIu64 " generated pictures not delivered in time "
                     "(the real frame shown); %" PRIu64 " from our own texture ring", sys->zc_fallback, sys->ring_out);
        pmx_close(sys->pc);
#ifdef _WIN32
        if (sys->ring) RingRelease(sys->ring);   /* pictures still on their way keep it alive */
#endif
    }
    if (sys->download) filter_chain_Delete(sys->download);
    free(sys);
}

/* ---------------------------------------------------------------- pushmpc_out (interactive chain) */
typedef struct { uint64_t copied, passed, failed, waited, late; mtime_t wait_us; } out_sys_t;

static picture_t *FilterOut(filter_t *f, picture_t *p)
{
    out_sys_t *s = (out_sys_t *)f->p_sys;
    pm_job *job; int idx = 0;
    if (!RegTake(p, &job, &idx)) { s->passed++; return p; }
    const picture_t *from = p;
    if (job) {                            /* worker mode: wait for this picture, at most until its display time */
        const mtime_t t0 = mdate();
        const mtime_t deadline = p->date > t0 ? p->date : t0;
        vlc_mutex_lock(&job->lock);
        if (job->done < idx) s->waited++;
        while (job->done < idx && !job->finished)
            if (vlc_cond_timedwait(&job->cond, &job->lock, deadline)) break;
        const bool ready = job->done >= idx;
        vlc_mutex_unlock(&job->lock);
        s->wait_us += mdate() - t0;
        if (!ready) { from = job->real; s->late++; }  /* not made in time: the real frame instead */
    }
    picture_t *o = filter_NewPicture(f);  /* the last interactive filter allocates from the vout's private pool */
    if (!o) { s->failed++; if (job) JobRelease(job); return p; }
    picture_Copy(o, from);                /* pixels and properties */
    o->date = p->date;
    picture_Release(p);
    if (job) JobRelease(job);
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
    vlc_mutex_lock(&reg_lock); out_stages++; vlc_mutex_unlock(&reg_lock);
    msg_Dbg(f, "PushMPC output stage on %4.4s", (const char *)&f->fmt_in.video.i_chroma);
    return VLC_SUCCESS;
}

static void CloseOut(vlc_object_t *obj)
{
    filter_t *f = (filter_t *)obj;
    out_sys_t *s = (out_sys_t *)f->p_sys;
    vlc_mutex_lock(&reg_lock); out_stages--; vlc_mutex_unlock(&reg_lock);
    msg_Info(f, "PushMPC output stage: copied=%" PRIu64 " passed=%" PRIu64 " failed=%" PRIu64 " waited=%" PRIu64
             " (%.2f ms on average) not ready=%" PRIu64, s->copied, s->passed, s->failed, s->waited,
             s->waited ? (double)s->wait_us / 1000.0 / (double)s->waited : 0.0, s->late);
    free(s);
}
