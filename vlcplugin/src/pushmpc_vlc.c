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
 * Intel Video Super Resolution in VLC's own Direct3D 11 output; the installer sets it). In VLC 3.0.24:
 * src/video_output/video_output.c:789 takes the target format from the decoder's (fmt_target), :820-824 appends a
 * converter back to it whenever the chains' output is not similar, and src/misc/es_format.c:400-404 (video_format_IsSimilar)
 * counts any other width / height as not similar; for hardware pictures (DX11) no converter scales them, so VLC would
 * remove every filter ("Failed to compensate for the format changes"). So pushcore's upscaler (and with it Auto's anime
 * upscaler, which only runs in that upscaler slot) cannot run in this filter: a bigger picture never reaches the display.
 *
 * The measured content (ContentDetect) on the pushcore route: zero copy, the bridge measures VLC's textures on the GPU
 * (pm_pushcore.h, measure); else the content library on the CPU planes. A class change makes Auto decide again
 * (pm_pc_update, warm, at most once per 10 s): at the same multiplier it picks its frame generation method for drawn
 * pictures (PushMpcAuto.h: DLSS-G after PushFlow on animation). Once the picture is measured as drawn (DrawnPolicy
 * holdRepair: hand-drawn and drawn CG) the same update turns on pushcore's held-frame re-timing (and with it the
 * animation mode, the line-art and sign holds and the anime deband preset), at most once per 30 s, and the filter
 * switches to its lag mode (PcLagFilter): every picture, the real ones too, then comes from pushcore (real_out), up to
 * PM_PC_LAG source frames after its source frame was pushed (pushcore queues up to 12, PushCoreGpu.cpp kMaxQueued).
 * The decoded picture is not kept for that: the bridge copies it at the push, so no decoder surface is held (VP9's
 * pool is 6 surfaces, modules/codec/avcodec/directx_va.c:335,359-360); a copy of it in an output picture stands in
 * for any picture pushcore did not deliver. That the pictures still come out in time rests on VLC's vout: when the
 * static chain returns nothing it pops the next decoded picture at once (video_output.c ThreadDisplayPreparePicture,
 * :857-897), and the decoder runs up to file-caching (1000 ms by default) ahead of the display, so a lag of a dozen
 * source frames (500 ms at 24 fps) is read ahead; only the pictures popped are checked for lateness (:864-879).
 * Lag mode needs output pictures beyond VLC's pool of 4: the zero-copy ring (grown when it starts) or heap pictures
 * (pushmpc_out in the chain); without either, held frames stay off.
 *
 * Pictures: the vout's private pool holds 4 pictures, which caps a static filter at x4 (3 new + the source picture
 * itself). Heap pictures sent straight to a direct-rendering display crash some of them (VLC's OpenGL output with
 * persistent buffers, CI run-display.sh). VLC's own answer is in VoutVideoFilterStaticNewPicture: once the interactive
 * chain is not empty, static filters get heap pictures and the last interactive filter hands the display a pool
 * picture. So the plugin has a second, tiny filter "pushmpc_out" for the interactive chain: it copies each generated
 * picture into a display-pool picture (one memcpy) and passes everything else through. The installer writes
 * video-filter=postproc:pushmpc_out; without pushmpc_out the multiplier is capped at x4.
 *
 * Hardware-decoded video on Linux (VA-API, VDPAU): the filter keeps the hardware format on both sides, so VLC never
 * has to convert back after the static chain (it cannot, for a filter that adds pictures). Real frames pass untouched;
 * the engine reads a download of each source surface, and generated frames are uploaded into PushMPC's own surface
 * pool on the decoder's device (pm_hw.c, with a round-trip check of the driver first; pass-through if it fails).
 *
 * Licence: GPL-3.0-or-later.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_filter.h>
#include <vlc_picture.h>

#include "pm_engine.h"
#include "engine_pushcore.h"
#include "engine_portable.h"
#include "pm_hw.h"
#include "pm_content.h"   /* what the picture is: PushMPC's content library, loaded at run time */
#ifdef __APPLE__
#include "pm_cvpx.h"   /* VideoToolbox pictures zero copy into the apple engine and back (guarded, see pm_cvpx.h) */
#define PM_CVPX_ON(s) ((s)->cvpx != NULL)
#else
#define PM_CVPX_ON(s) false
#endif

#ifdef _WIN32
# include <stdatomic.h>
# include "pm_d3d11_picsys.h"   /* zero copy with VLC's Direct3D 11 hardware decoding */

/* Zero copy above x4: VLC's private pool holds 4 pictures, so generated pictures come from a ring of our own textures on
 * VLC's device, made like the pictures of VLC's Direct3D 11 pool (same size, format, bind flags; one texture each, with
 * the luma and chroma shader views VLC's outputs draw from). A picture holds its slot and the ring until VLC releases
 * it, so pictures may outlive the filter. */
#define PM_RING_MAX 128  /* 3 x 16 + 4: VFG 16x; pushcore's lag mode grows the ring up to this (RingGrow) */
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

/* more slots (textures are made on first use); only the filter's thread changes the size */
static void RingGrow(pm_ring *r, int size)
{
    if (size > PM_RING_MAX) size = PM_RING_MAX;
    for (int i = r->size; i < size; i++) atomic_init(&r->slot[i].busy, false);
    if (size > r->size) r->size = size;
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
#define PM_LAGQ 16              /* held-drawing repair: source pictures waiting for their pair (lag 3 + repeats) */
#define PM_HOLD 3               /* held frames: repeats that wait for the next drawing (twos, threes, fours) */
#define PM_JOB_MAX 64           /* pictures per job: a held span of (PM_HOLD + 1) x 16 - 1 */
#define PM_PC_LAG 14            /* pushcore's lag mode: a picture is given up this many source frames after its pair */
#define PM_PCQ 256              /* pushcore's lag mode: pictures waiting for pushcore, in display order */
typedef struct pm_job {
    vlc_mutex_t lock;
    vlc_cond_t cond;
    int refs;
    int n;                      /* pictures to generate */
    int done;                   /* filled so far, in order */
    bool finished, cancel, contiguous;
    bool lag, advance;          /* the repair's lag mode: the frame's deferred repair, then (advance) the next pair, then outs */
    bool failed[PM_JOB_MAX];    /* hardware formats: the upload failed, show the real frame instead */
    picture_t *src;             /* the newest source frame (pushed to the engine by the worker) */
    picture_t *real;            /* the real frame shown before them (the fallback) */
    picture_t *outs[PM_JOB_MAX];
    picture_t *stage[PM_JOB_MAX]; /* hardware formats: the CPU picture the engine writes, uploaded into outs[i] */
    float t[PM_JOB_MAX];
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
    for (int i = 0; i < j->n; i++) {
        if (j->outs[i]) picture_Release(j->outs[i]);
        if (j->stage[i]) picture_Release(j->stage[i]);
    }
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

static const int smoother_values[] = { 0, 1, 2, 3, 4, 5, 6, 8, 10, 12, 16 };
static const char *const smoother_texts[] = { "Auto (display refresh, CPU budget)", "Off", "x2", "x3", "x4", "x5", "x6", "x8", "x10", "x12", "x16" };
static const int quality_values[] = { 0, 1 };
static const char *const quality_texts[] = { "Fast", "Balanced" };
static const int engine_values[] = { 0, 1, 2, 3 };
static const char *const engine_texts[] = { "Auto", "Portable on the CPU", "pushcore (Windows)", "Portable on the GPU (Vulkan)" };
static const int alloc_values[] = { 0, 1, 2 };
static const char *const alloc_texts[] = { "Auto (VLC's allocator)", "Pool only (at most x4)", "Heap (unsafe with some displays)" };
static const int hw_values[] = { 0, 1 };
static const char *const hw_texts[] = { "Auto (keep the hardware format)", "Convert (VLC's converter)" };

vlc_module_begin()
    set_shortname("PushMPC")
    set_description("PushMPC frame generation (PushFPS's video enhancement in VLC)")
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VFILTER)
    add_integer(CFG "smoother", 0, "Smoother (frame generation)",
                "Auto picks the multiplier from the display refresh rate and what this computer can generate in time; "
                "Off passes frames through; x2..x16 force a multiplier.", false)
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
    add_bool(CFG "hold-repair", true, "Held-drawing repair (animation)", "A part held between two of its drawings while the rest moves is laid in at its in-between position", false)
    add_bool(CFG "dedup", true, "Held frames (animation)", "Animation drawn on twos, threes or fours: a repeated frame "
             "waits for the next drawing, and the generated frames move evenly over the whole held span (portable "
             "engines).", true)
    add_bool(CFG "content", true, "Measure what the picture is", "Drawn animation or not (rendered 3D CG or hand-drawn), "
             "measured per shot from the decoded frames by PushMPC's content library next to the module, with PushMPC's "
             "image model when it is installed (else the hand-built cues); shown in VLC's log. Off: not measured.", true)
    add_integer(CFG "alloc", 0, "Output pictures", "Where generated pictures come from (diagnostics)", true)
        change_integer_list(alloc_values, alloc_texts)
    add_bool(CFG "zerocopy", true, "Zero copy with hardware decoding", "Windows, pushcore: Direct3D 11 pictures; macOS, Apple's interpolation: VideoToolbox pictures. Hardware-decoded "
             "pictures stay on the GPU (pushcore works on VLC's own D3D11 device); off = they are copied to the CPU "
             "and back, as before.", true)
    add_bool(CFG "zerocopy-ring", true, "Own output pictures with zero copy", "Generated pictures come from textures "
             "of the module's own (up to x16); off = VLC's 4-picture pool (at most x4).", true)
    add_integer(CFG "hw", 0, "Hardware-decoded video (Linux)", "Auto keeps VA-API / VDPAU pictures on the GPU: real frames "
                "pass untouched, generated frames go into PushMPC's own surfaces on the decoder's device, after a "
                "round-trip check of the driver (pass-through if it fails). Convert hands them to VLC's converter "
                "instead (VLC then often cannot show them).", true)
        change_integer_list(hw_values, hw_texts)

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
    pm_hw *hw;                 /* Linux VA-API / VDPAU: the hardware format kept on both sides (pm_hw.h) */
    bool hw_ready, hw_failed;
    uint64_t hw_down_fail, hw_up_fail, hw_no_surface;
    bool zc_hw;                /* VA-API zero copy: surfaces as dmabufs into the GPU engine and back (no CPU copies) */
    uint64_t zc_in, zc_out, zc_fail;
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
    pm_pc_cfg pc_cfg;          /* what pushcore was opened with (ABI 3: the measured class goes back through pm_pc_update) */
    int pc_cls, pc_cg;         /* the class pushcore was last given */
    mtime_t pc_class_at;       /* ... and when (at most once per 10 s) */
    uint64_t pc_updates;
    /* pushcore's lag mode (PcLagFilter): every picture from pushcore (real_out), held frames while drawn is measured */
    bool pc_lag;               /* on (stays on: the pictures waiting keep their order) */
    bool pc_held;              /* pushcore re-times held frames now */
    mtime_t pc_held_at;        /* the last held-frame switch (at most once per 30 s) */
    struct { mtime_t date; picture_t *pic, *routing, *fb; int age; bool real; } pcq[PM_PCQ];
    int pcn;
    picture_t *pc_fb;          /* the last source frame's copy (stands in for its interval's missing pictures) */
    mtime_t pc_prev_date;
    bool pc_have_prev;
    uint64_t pc_lag_out, pc_lag_missed, pc_held_switches;
    pm_engine *eng;
    int k;                 /* outputs per source frame */
    int k_open;            /* k at open: the output rate and the pools were made for it (a later decision stays at or below) */
    int fs;                /* the vout's live "fullscreen" (our parent object), re-read every 30 input pictures */
    int fs_smoother;       /* what Auto decided with, for the re-decision on a fullscreen toggle */
    double fs_fps, fs_hz;
    /* the source rate measured from the picture dates (VLC 3 shows pictures by their date; the format's rate only sets
     * the late-drop threshold, src/video_output/video_output.c ThreadDisplayPreparePicture): k is re-decided from it,
     * within the pools made at open (sized for the rate 20 % below the declared one) */
    mtime_t rate_last; int rate_n; mtime_t rate_steps[16]; double rate_fps; int rate_done;
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
#ifdef __APPLE__
    pm_cvpx_eng *cvpx;          /* the apple engine on VLC's CVPX pictures, zero copy (else the one-copy path) */
    bool cvpx_off;              /* a picture failed the checks: pass-through from there on */
    uint64_t cvpx_gen, cvpx_err;
#endif
    /* held frames (HeldStep): repeats of sys->prev waiting for the next drawing, a still, the last frame's date */
    bool dedup, dedup_set, still;
    int held;
    mtime_t held_last;
    /* held-drawing repair (engine_portable.h): pairs lag this many source frames (0: off); the waiting pictures */
    int lag;
    struct { picture_t *pic; int span; bool contig; mtime_t base; } lagq[PM_LAGQ];
    int lagn;
    picture_t *lag_a;                 /* the picture advanced last (the next pair's first) */
    bool lag_seen;
    uint64_t lag_jobs, lag_errors;    /* pairs advanced and generated on the worker; advances that failed there */
    bool rep_tried;                   /* the repair was set (PortableRepairOn): once per stream */
    mtime_t held_base;
    picture_t *still_pic;             /* held-drawing repair: the still's picture (as shown), cloned for each repeat */
    uint64_t still_out;               /* pictures that showed a still at the output rate */
    uint64_t lag_repaired;
    uint64_t repeats, stills;
    /* what the picture is (ContentMeasure): the content library's context, the last state seen, the frame dates */
    pm_content *content;
    bool content_tried, content_disc;
    mtime_t content_last;
    uint32_t content_serial;
    pmc_state content_state;
    uint64_t content_gpu;       /* frames not measured: zero-copy GPU pictures (no CPU planes) */
    /* VA-API zero copy: the GPU engine's tap (ContentTap) */
    bool tap_ready, tap_off;
    uint32_t *tap_words;
    size_t tap_cap;
    double tap_clip_next;
    uint64_t tap_frames, tap_fail;
};

/* the worker thread: one job at a time, in order (the engine keeps the pair state) */
static void PlanesIn(const picture_t *p, const video_format_t *f, int bps, pm_yuv *y);
static void PlanesOut(picture_t *p, const video_format_t *f, int bps, pm_yuv_out *y);
/* where generated picture i is written (hardware formats: a CPU picture, then uploaded into the job's surface) */
static picture_t *JobDst(pm_job *j, int i) { return j->stage[i] ? j->stage[i] : j->outs[i]; }
static void JobUpload(struct filter_sys_t *sys, pm_job *j, int i)
{
    if (!j->stage[i] || pm_hw_upload(sys->hw, j->stage[i], j->outs[i]) == 0) return;
    sys->hw_up_fail++;
    vlc_mutex_lock(&j->lock);
    j->failed[i] = true;
    vlc_mutex_unlock(&j->lock);
}
/* the engine's input: the surface as a dmabuf (VA-API zero copy), else its CPU planes; a surface that does not export
 * goes through the copy path for that frame */
static int EnginePush(struct filter_sys_t *sys, picture_t *src, bool contiguous, char *err, size_t n)
{
    if (sys->zc_hw) {
        pm_dmabuf d;
        if (pm_hw_export(sys->hw, src, &d) == 0) {
            const int r = pm_engine_push_dmabuf(sys->eng, &d, contiguous, err, n);
            pm_hw_dmabuf_close(&d);
            if (r == 0) { sys->zc_in++; return 0; }
        }
        sys->zc_fail++;
        picture_t *c = pm_hw_cpu_picture(sys->hw);
        if (!c || pm_hw_download(sys->hw, src, c) != 0) { if (c) picture_Release(c); snprintf(err, n, "surface unreadable"); return -1; }
        pm_yuv y;
        PlanesIn(c, &sys->wfmt, sys->bps, &y);
        const int r = pm_engine_push(sys->eng, &y, contiguous, err, n);
        picture_Release(c);
        return r;
    }
    pm_yuv y;
    PlanesIn(src, &sys->wfmt, sys->bps_w, &y);
    return pm_engine_push(sys->eng, &y, contiguous, err, n);
}
/* a generated frame straight into an output surface (zero copy), else into a CPU picture and uploaded */
static int EngineGenerateHw(struct filter_sys_t *sys, float t, picture_t *out)
{
    char err[200];
    pm_dmabuf d;
    if (pm_hw_export(sys->hw, out, &d) == 0) {
        const int r = pm_engine_generate_dmabuf(sys->eng, t, &d, err, sizeof(err));
        pm_hw_dmabuf_close(&d);
        if (r == 0) { sys->zc_out++; return 0; }
    }
    sys->zc_fail++;
    picture_t *c = pm_hw_cpu_picture(sys->hw);
    if (!c) return -1;
    pm_yuv_out o;
    PlanesOut(c, &sys->wfmt, sys->bps, &o);
    pm_engine_generate(sys->eng, t, &o);
    const int r = pm_hw_upload(sys->hw, c, out);
    picture_Release(c);
    return r;
}
/* zero copy is used only after a round trip through it: a pattern into a pool surface (the proven copy path), twice
 * into the engine as a dmabuf, the frame half-way (= the pattern) out into a second surface, read back, compared */
static void ZcProbe(filter_t *f, struct filter_sys_t *sys)
{
    const char *why = "";
    if (!sys->eng || !OptInt(f, CFG "zerocopy", 1)) return;
    if (!pm_engine_dmabuf_ok(sys->eng, &why)) { msg_Info(f, "PushMPC: zero copy unavailable (%s): the hardware path copies", why); return; }
    pm_engine_plain(sys->eng, 1);   /* the round trip pushes one picture twice: no held-frame test, no repair lag */
    picture_t *a = pm_hw_new_picture(sys->hw), *b = pm_hw_new_picture(sys->hw);
    picture_t *ca = pm_hw_cpu_picture(sys->hw), *cb = pm_hw_cpu_picture(sys->hw);
    char err[200] = "";
    bool ok = false;
    picture_t *cr = NULL;
    if (a && b && ca && cb) {
        const int w = (int)sys->wfmt.i_visible_width, h = (int)sys->wfmt.i_visible_height;
        for (int p = 0; p < 3; p++)
            for (int y = 0; y < (p ? (h + 1) / 2 : h); y++) {
                uint8_t *row = ca->p[p].p_pixels + (size_t)y * ca->p[p].i_pitch;
                for (int x = 0; x < (p ? (w + 1) / 2 : w); x++) {
                    const unsigned v = 16u + (unsigned)((x * 3 + y * 5 + p * 40) % 200);
                    if (sys->bps == 1) row[x] = (uint8_t)v; else ((uint16_t *)row)[x] = (uint16_t)v;
                }
            }
        /* the reference: the same frames through the copy path (the engine's own rounding is not the transport's) */
        cr = pm_hw_cpu_picture(sys->hw);
        if (cr) {
            pm_yuv ya; pm_yuv_out yr;
            PlanesIn(ca, &sys->wfmt, sys->bps, &ya);
            PlanesOut(cr, &sys->wfmt, sys->bps, &yr);
            if (pm_engine_push(sys->eng, &ya, 0, err, sizeof(err)) == 0 && pm_engine_push(sys->eng, &ya, 1, err, sizeof(err)) == 0)
                pm_engine_generate(sys->eng, 0.5f, &yr);
            pm_engine_forget(sys->eng);
            err[0] = 0;
        }
        pm_dmabuf da, db;
        if (pm_hw_upload(sys->hw, ca, a) != 0) snprintf(err, sizeof(err), "pattern upload failed");
        else if (pm_hw_export(sys->hw, a, &da) != 0) snprintf(err, sizeof(err), "the driver does not export surfaces (vaExportSurfaceHandle)");
        else {
            if (pm_hw_export(sys->hw, b, &db) != 0) snprintf(err, sizeof(err), "the driver does not export surfaces (vaExportSurfaceHandle)");
            else {
                if (pm_engine_push_dmabuf(sys->eng, &da, 0, err, sizeof(err)) == 0 && pm_engine_push_dmabuf(sys->eng, &da, 1, err, sizeof(err)) == 0 &&
                    pm_engine_generate_dmabuf(sys->eng, 0.5f, &db, err, sizeof(err)) == 0) {
                    if (!cr || pm_hw_download(sys->hw, b, cb) != 0) snprintf(err, sizeof(err), "read back failed");
                    else {
                        int maxd = 0;
                        for (int p = 0; p < 3; p++)
                            for (int y = 0; y < (p ? (h + 1) / 2 : h); y++)
                                for (int x = 0; x < (p ? (w + 1) / 2 : w); x++) {
                                    const uint8_t *ra = cr->p[p].p_pixels + (size_t)y * cr->p[p].i_pitch, *rb = cb->p[p].p_pixels + (size_t)y * cb->p[p].i_pitch;
                                    const int va = sys->bps == 1 ? ra[x] : ((const uint16_t *)ra)[x], vb = sys->bps == 1 ? rb[x] : ((const uint16_t *)rb)[x];
                                    if (abs(va - vb) > maxd) maxd = abs(va - vb);
                                }
                        ok = maxd <= 1;
                        if (!ok) snprintf(err, sizeof(err), "the round trip differs by up to %d", maxd);
                    }
                }
                pm_hw_dmabuf_close(&db);
            }
            pm_hw_dmabuf_close(&da);
        }
    } else snprintf(err, sizeof(err), "no surface for the round trip");
    if (cr) picture_Release(cr);
    if (a) picture_Release(a);
    if (b) picture_Release(b);
    if (ca) picture_Release(ca);
    if (cb) picture_Release(cb);
    pm_engine_plain(sys->eng, 0);   /* forgets the probe's frames */
    sys->zc_hw = ok;
    if (ok) msg_Info(f, "PushMPC: zero copy on (VA-API surfaces as dmabufs into the GPU engine and back)");
    else msg_Info(f, "PushMPC: zero copy not used (%s): the hardware path copies", err);
}
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
    PlanesOut(JobDst(r->j, i), &r->sys->wfmt, r->sys->bps_w, &o);
    for (int p = 0; p < 3; p++) { r->dst.data[p] = o.data[p]; r->dst.stride[p] = o.stride[p]; }
    return &r->dst;
}
static void RouteDone(void *user, int64_t pts100ns)
{
    pc_route *r = user;
    const int i = RouteIndex(r, pts100ns);
    if (i < 0) return;
    r->filled[i] = true;
    JobUpload(r->sys, r->j, i);
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
        bool advanced = true;
        if (j->lag) {               /* lagged (LagFilter): the checked frame's repair, then its pair advances */
            pm_engine_run_deferred(sys->eng);
            if (j->advance && pm_engine_next_pair(sys->eng, err, sizeof(err)) != 1) { sys->lag_errors++; advanced = false; }
        } else if (EnginePush(sys, j->src, j->contiguous, err, sizeof(err)) != 0)
            pm_engine_forget(sys->eng);
        /* a failed advance leaves the previous pair: nothing from it (the real frame stands in) */
        const bool pair = advanced && (!j->lag || j->contiguous) && pm_engine_have_pair(sys->eng);
        for (int i = 0; i < j->n; i++) {
            vlc_mutex_lock(&j->lock);
            const bool stop = j->cancel;
            vlc_mutex_unlock(&j->lock);
            if (stop || !pair) break;
            if (sys->zc_hw) {           /* straight into the output surface */
                if (EngineGenerateHw(sys, j->t[i], j->outs[i]) != 0) { vlc_mutex_lock(&j->lock); j->failed[i] = true; vlc_mutex_unlock(&j->lock); }
            } else {
                pm_yuv_out dst;
                PlanesOut(JobDst(j, i), &sys->wfmt, sys->bps_w, &dst);
                pm_engine_generate(sys->eng, j->t[i], &dst);
                JobUpload(sys, j, i);
            }
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
    if (q) {                    /* never started: finish it as empty, but a lag job here (its pair already left the queue) */
        if (q->lag && !cancel) RunJob(sys, q);
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
    /* hardware formats: a surface of PushMPC's own pool on the decoder's device */
    if (sys->hw) {
        picture_t *p = pm_hw_new_picture(sys->hw);
        if (!p) { sys->hw_no_surface++; sys->alloc_fail++; }
        return p;
    }
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

/* hardware formats: the CPU copy of cur the engine reads (NULL: none; hw_failed says whether for good) */
static picture_t *HwIn(filter_t *f, struct filter_sys_t *sys, picture_t *cur)
{
    if (!sys->hw_ready) {
        if (sys->hw_failed) return NULL;
        char why[300] = "";
        if (pm_hw_bind(sys->hw, cur, why, sizeof(why)) != 0) {
            msg_Warn(f, "PushMPC: hardware path unavailable (%s): pass-through x1", why);
            sys->hw_failed = true;
            return NULL;
        }
        sys->wfmt = *pm_hw_work_format(sys->hw);
        sys->hw_ready = true;
        msg_Info(f, "PushMPC: hw path ok, x%d (%s)", sys->k, pm_hw_describe(sys->hw));
        ZcProbe(f, sys);
    }
    pm_hw_anchor(sys->hw, cur);
    if (sys->zc_hw) return cur;      /* zero copy: the engine imports the surface itself */
    picture_t *c = pm_hw_cpu_picture(sys->hw);
    if (!c || pm_hw_download(sys->hw, cur, c) != 0) {
        if (c) picture_Release(c);
        if (!sys->hw_down_fail++) msg_Warn(f, "PushMPC: a hardware surface could not be read");
        return NULL;
    }
    picture_CopyProperties(c, cur);
    return c;
}

#ifdef __APPLE__
/* VideoToolbox pictures through the apple engine with no copy: the real frame is VLC's own picture, the generated ones
 * are CVPX pictures that own the engine's CVPixelBuffers. A picture that fails the checks turns this off for good:
 * the frames then pass through unchanged (playback never breaks). */
static picture_t *FilterCvpx(filter_t *f, struct filter_sys_t *sys, picture_t *cur)
{
    void *pb = NULL;
    if (!sys->cvpx_off) {
        const char *why = "";
        pb = pm_cvpx_in(cur, &f->fmt_in.video, &why);
        if (!pb) { sys->cvpx_off = true; msg_Warn(f, "PushMPC: CVPX zero copy stopped (%s): pass-through", why); }
    }
    if (sys->cvpx_off) {
        picture_t *p = sys->prev;
        sys->prev = NULL;
        cur->p_next = NULL;
        if (p) { p->p_next = cur; sys->out += 2; return p; }
        sys->out++;
        return cur;
    }
    picture_t *prev = sys->prev;
    sys->prev = cur;
    const mtime_t dt = prev ? cur->date - prev->date : 0;
    const bool contiguous = prev && dt > 0 && dt <= 4 * sys->nominal_dt && dt <= CLOCK_FREQ / 4;
    void *gen[16];   /* up to x16: 15 generated pictures per pair */
    int ngen = 0;
    bool pair = false;
    char err[200] = "";
    if (pm_cvpx_eng_process(sys->cvpx, pb, (int64_t)cur->date * 10, contiguous, gen, 15, &ngen, &pair, err, sizeof(err)) != 0) {
        if (sys->cvpx_err++ == 0) msg_Warn(f, "PushMPC: apple engine: %s (this pair shows the real frames)", err);
        pm_cvpx_eng_forget(sys->cvpx);
        ngen = 0;
    }
    if (!prev) { for (int i = 0; i < ngen; i++) pm_cvpx_release(gen[i]); return NULL; }
    picture_t *head = prev, *tail = prev;
    prev->p_next = NULL;
    sys->out++;
    if (!contiguous) sys->restarts++;
    for (int i = 0; i < ngen; i++) {
        if (!contiguous || !pair) { pm_cvpx_release(gen[i]); continue; }
        picture_t *o = pm_cvpx_out(&f->fmt_out.video, gen[i]);   /* owns gen[i] from here */
        if (!o) { sys->alloc_fail++; continue; }
        picture_CopyProperties(o, prev);
        o->date = prev->date + dt * (i + 1) / sys->k;
        o->b_force = false;
        o->p_next = NULL;
        tail->p_next = o;
        tail = o;
        sys->out++;
        sys->generated++;
        sys->cvpx_gen++;
    }
    return head;
}
#endif

/* ---- held frames (animation drawn on twos, threes, fours; portable engines). A frame that repeats the last distinct
 * one (the engine's duplicate test) is dropped: it shows the same picture, so nothing changes on screen. Up to PM_HOLD
 * of them wait for the next distinct frame, whose pair then makes the whole held span: the last distinct frame, then
 * k x span - 1 generated pictures dated evenly up to the new frame (t = j / (k x span)). One more repeat makes a still:
 * the last distinct frame goes out at once, and from then on at the output rate (StillFill: k clones per source frame,
 * as a moving picture would have, so the frame rate stays constant and a still at the end of a file is shown for its
 * whole length); when the still ends only its last interval moves (the base becomes a clone dated at the last repeat). */
/* n clones of src dated evenly over [from, to) (the first at from), as one list; *count += n made */
static picture_t *StillFill(picture_t *src, mtime_t from, mtime_t to, int n, uint64_t *count)
{
    picture_t *head = NULL, *tail = NULL;
    if (!src || n < 1 || to <= from) n = src ? 1 : 0;
    for (int i = 0; i < n; i++) {
        picture_t *c = picture_Clone(src);
        if (!c) break;
        picture_CopyProperties(c, src);
        c->date = from + (to > from ? (to - from) * i / n : 0);
        c->b_force = false;
        c->p_next = NULL;
        if (tail) tail->p_next = c; else head = c;
        tail = c;
        (*count)++;
    }
    return head;
}
/* the duplicate test of the frame the engine reads next (as EnginePush reads it); 1 = a repeat. Failed: a new picture */
static int EngineCheck(struct filter_sys_t *sys, picture_t *src, bool contiguous)
{
    char err[200];
    if (sys->zc_hw) {
        pm_dmabuf d;
        if (pm_hw_export(sys->hw, src, &d) == 0) {
            const int r = pm_engine_check_dup_dmabuf(sys->eng, &d, contiguous, err, sizeof(err));
            pm_hw_dmabuf_close(&d);
            if (r >= 0) return r;
        }
        picture_t *c = pm_hw_cpu_picture(sys->hw);
        if (!c || pm_hw_download(sys->hw, src, c) != 0) { if (c) picture_Release(c); return 0; }
        pm_yuv y;
        PlanesIn(c, &sys->wfmt, sys->bps, &y);
        const int r = pm_engine_check_dup(sys->eng, &y, contiguous, err, sizeof(err));
        picture_Release(c);
        return r > 0;
    }
    pm_yuv y;
    PlanesIn(src, &sys->wfmt, sys->bps_w, &y);
    return pm_engine_check_dup(sys->eng, &y, contiguous, err, sizeof(err)) > 0;
}
/* true: cur was a repeat and is consumed (*out: what to return, NULL or the still's picture). Else *span: the source
 * intervals from sys->prev to cur, *contig: cur follows the last frame received (-1: dedup off, the caller decides) */
static bool HeldStep(struct filter_sys_t *sys, picture_t *cur, picture_t *work, int *span, int *contig, picture_t **out)
{
    *span = 1; *contig = -1; *out = NULL;
    if (!sys->dedup || !sys->eng) return false;
    if (!sys->dedup_set) { pm_engine_set_dedup(sys->eng, 1); sys->dedup_set = true; }
    picture_t *prev = sys->prev;
    const mtime_t last = sys->held_last, dt = prev ? cur->date - last : 0;
    const bool contiguous = prev && dt > 0 && dt <= 4 * sys->nominal_dt && dt <= CLOCK_FREQ / 4;
    const int dup = EngineCheck(sys, work, contiguous);
    sys->held_last = cur->date;
    *contig = contiguous;
    if (dup && contiguous) {
        sys->repeats++;
        if (!sys->still && ++sys->held > PM_HOLD) {   /* a still: the last distinct frame goes out now, for the held span */
            sys->still = true;
            sys->stills++;
            *out = StillFill(prev, prev->date, cur->date, sys->k * sys->held, &sys->still_out);
        } else if (sys->still)                        /* then for each repeat's interval before this one */
            *out = StillFill(prev, last, cur->date, sys->k, &sys->still_out);
        if (work != cur) picture_Release(work);
        picture_Release(cur);
        return true;
    }
    if (contiguous && sys->still) {   /* only the still's last interval moves: the base is its last repeat */
        picture_t *c = picture_Clone(prev);
        if (c) { picture_CopyProperties(c, prev); c->date = last; c->p_next = NULL; picture_Release(prev); sys->prev = c; }
    }
    *span = contiguous && !sys->still ? sys->held + 1 : 1;
    sys->held = 0;
    sys->still = false;
    return false;
}

/* ---- what the picture is: ContentDetect through PushMPC's content library (pm_content.h), on the frames the engine
 * reads (CPU planes; zero-copy GPU pictures are not read back for it), from the first such frame on (nothing is loaded
 * for a pass-through or a zero-copy stream). The class and Auto's drawn-animation policy
 * (pushmpc/content_detect.h PolicyFor) come from the picture only. In this module the held-frame step runs for every
 * class (as pushcore's re-timing does: held frames are re-timed whatever the picture is); the class goes to pushcore's
 * Auto on the pushcore route (PcContent) and is logged with the policy. */
static void ContentLog(void *user, const char *line) { msg_Info((filter_t *)user, "PushMPC: %s", line); }
static void ContentOpen(filter_t *f, struct filter_sys_t *sys)
{
    if (!OptInt(f, CFG "content", 1)) { msg_Info(f, "PushMPC: content not measured (pushmpc-content off)"); return; }
    char why[320], where[640];
    sys->content = pm_content_open(ContentLog, f, 0, why, sizeof(why), where, sizeof(where));
    if (sys->content) msg_Info(f, "PushMPC: content measured in the picture by %s", where);
    else msg_Info(f, "PushMPC: content not measured (%s); everything else runs as before", why);
}
static void ContentState(filter_t *f, struct filter_sys_t *sys);
/* VA-API zero copy: the GPU engine's tap reads the surface (only the cells come back, and about once a second the
 * image model's thumbnail) when the engine has it and the content library takes cells (pm_content_has_cells; an older
 * library leaves these pictures unmeasured, as before). true: measured this way */
static bool ContentTap(filter_t *f, struct filter_sys_t *sys, picture_t *cur)
{
    if (!sys->zc_hw || !sys->eng || sys->tap_off) return false;
    if (!sys->tap_ready) {
        sys->tap_ready = true;
        if (!pm_engine_tap_ok(sys->eng)) { sys->tap_off = true; return false; }
        if (!sys->content_tried) { sys->content_tried = true; ContentOpen(f, sys); }
        if (!sys->content || !pm_content_has_cells(sys->content)) {
            if (sys->content) msg_Info(f, "PushMPC: content not measured on zero-copy pictures: this content library has no GPU tap (pmc_push_cells)");
            sys->tap_off = true; return false;
        }
        const int w = (int)sys->wfmt.i_visible_width, h = (int)sys->wfmt.i_visible_height;
        sys->tap_cap = (size_t)480 * (size_t)(480 * h / (w > 0 ? w : 1) + 2) + 224 * 224;
        sys->tap_words = malloc(sys->tap_cap * sizeof(uint32_t));
        if (!sys->tap_words) { sys->tap_off = true; return false; }
        msg_Info(f, "PushMPC: content measured on the GPU (zero copy: the engine's tap, only the cells are read back)");
    }
    WorkerIdle(sys, false);   /* the engine is not shared with the worker */
    pm_dmabuf d;
    if (pm_hw_export(sys->hw, cur, &d) != 0) { sys->tap_fail++; return true; }
    pmc_state ms;
    const bool model = pm_content_state(sys->content, &ms) == 0 && (ms.model == PMC_MODEL_RUNNING || ms.model == PMC_MODEL_STARTING);
    const double pts = (double)cur->date / CLOCK_FREQ;
    const bool clip = model && (sys->tap_clip_next <= 0 || pts >= sys->tap_clip_next || pts < sys->tap_clip_next - 5.0);
    int cw = 0, ch = 0;
    char err[200] = "";
    const int n = pm_engine_tap_dmabuf(sys->eng, &d, clip ? 1 : 0, sys->tap_words, sys->tap_cap, &cw, &ch, err, sizeof(err));
    pm_hw_dmabuf_close(&d);
    if (n < 0) {
        if (!sys->tap_fail++) msg_Warn(f, "PushMPC: the content tap failed (%s)", err);
        return true;
    }
    const video_format_t *wf = &sys->wfmt;
    const int limited = !(wf->b_color_range_full || wf->i_chroma == VLC_CODEC_J420);
    const int disc = sys->content_disc || sys->content_last <= 0 || cur->date <= sys->content_last || cur->date - sys->content_last > CLOCK_FREQ;
    sys->content_disc = false;
    sys->content_last = cur->date;
    pm_content_push_cells(sys->content, sys->tap_words, cw, cw, ch, limited, pts, disc);
    if (clip && n >= cw * ch + 224 * 224) {
        sys->tap_clip_next = pts + 1.0;
        pm_content_offer_clip(sys->content, sys->tap_words + (size_t)cw * (size_t)ch, 224, (int)wf->i_visible_height >= 720 ? 1 : 0, limited, pts);
    }
    sys->tap_frames++;
    ContentState(f, sys);
    return true;
}

static void ContentMeasure(filter_t *f, struct filter_sys_t *sys, picture_t *cur, const picture_t *work)
{
    if (ContentTap(f, sys, cur)) return;
    if (sys->zc || sys->zc_hw || PM_CVPX_ON(sys)) {
        if (!sys->content_gpu++)
            msg_Info(f, sys->zc && sys->pc_cfg.measure && pmx_abi() >= 3
                     ? "PushMPC: content measured on the GPU by pushcore's bridge (VLC's textures; only the cells are read back)"
                     : "PushMPC: content not measured on zero-copy GPU pictures (no CPU copy is made for it)");
        return;
    }
    if (!sys->content_tried) { sys->content_tried = true; ContentOpen(f, sys); }   /* at the first frame it can read */
    if (!sys->content) return;
    const video_format_t *wf = &sys->wfmt;
    pm_yuv y;
    PlanesIn(work, wf, sys->bps_w, &y);
    const bool yv12 = wf->i_chroma == VLC_CODEC_YV12;   /* V before U */
    const int limited = !(wf->b_color_range_full || wf->i_chroma == VLC_CODEC_J420);
    const int disc = sys->content_disc || sys->content_last <= 0 || cur->date <= sys->content_last || cur->date - sys->content_last > CLOCK_FREQ;
    sys->content_disc = false;
    sys->content_last = cur->date;
    pm_content_push(sys->content, y.data[0], y.stride[0], y.data[yv12 ? 2 : 1], y.data[yv12 ? 1 : 2], y.stride[1],
                    (int)wf->i_visible_width, (int)wf->i_visible_height, sys->bits, limited, (double)cur->date / CLOCK_FREQ, disc);
    ContentState(f, sys);
}
/* the content library's latest state: logged when the class changes */
static void ContentState(filter_t *f, struct filter_sys_t *sys)
{
    pmc_state st;
    if (pm_content_state(sys->content, &st) != 0 || st.serial == sys->content_serial) return;
    sys->content_serial = st.serial;
    if (st.cls != sys->content_state.cls || st.cg != sys->content_state.cg) {
        char pol[200];
        pm_content_policy_text(st.policy, pol, sizeof(pol));
        msg_Info(f, "PushMPC: content %s%s (%.0f %%, %s); Auto's drawn-animation policy: %s", pm_content_class_name(sys->content, st.cls),
                 st.cls == PMC_CLASS_DRAWN ? (st.cg ? ", rendered 3D CG" : ", hand-drawn") : "", 100.0 * st.confidence,
                 st.image_evidence ? "with the image model" : "hand-built cues", pol);
    }
    sys->content_state = st;
}

/* the held-drawing repair on the portable engines follows the measured class, as pushcore's held frames do (PcContent):
 * the lag (three source frames), a copy and a cell grid per frame are only for pictures measured as drawn (zero-copy VA-API
 * pictures through the GPU tap, ContentTap; pictures that are not measured stay without it). PUSHMPC_VLC_DRAWN: unset
 * follows the class, 0 never, at:N from input frame N (tests), else from the start. Once on, it stays on
 * for the stream (the pictures waiting in the lag would have to be handed back to switch it off). */
static bool PortableRepairWanted(filter_t *f, const struct filter_sys_t *sys)
{
    if (sys->pc || !sys->eng || !sys->dedup || sys->lag || sys->rep_tried || !OptInt(f, CFG "hold-repair", 1)) return false;
    const char *ov = getenv("PUSHMPC_VLC_DRAWN");
    if (ov && !strncmp(ov, "at:", 3)) return sys->in >= (uint64_t)atoi(ov + 3);   /* tests: on from that input frame (a switch mid-stream) */
    if (ov && *ov) return ov[0] != '0';
    return sys->content_state.cls == PMC_CLASS_DRAWN;
}
static void PortableRepairOn(filter_t *f, struct filter_sys_t *sys)
{
    sys->rep_tried = true;
    if (!sys->dedup_set) { pm_engine_set_dedup(sys->eng, 1); sys->dedup_set = true; }
    char rwhy[200] = "";
    sys->lag = pm_engine_set_repair(sys->eng, 1, rwhy, sizeof(rwhy)) ? pm_engine_repair_lag(sys->eng) : 0;
    msg_Info(f, "PushMPC: held-drawing repair %s", rwhy);
}

/* the measured class on the pushcore route (see the header): Auto decides again with it, warm (pm_pc_update), at most
 * once per 10 s, with no job in flight; true when pushcore was reloaded (its pair starts over: the next frame has none) */
static const char *ClassName(int cls)
{
    static const char *const n[] = { "measuring", "drawn animation", "3D CG animation", "not drawn", "game footage" };
    return n[cls >= 0 && cls <= 4 ? cls : 0];
}
static picture_t *LagCopy(filter_t *f, struct filter_sys_t *sys, picture_t *src);
/* pushmpc_out in the interactive chain: the static filter then gets heap pictures (VoutVideoFilterStaticNewPicture) */
static bool OutFilterInChain(filter_t *f)
{
    char *chain = var_InheritString(f, "video-filter");
    const bool in = chain && strstr(chain, "pushmpc_out") != NULL;
    free(chain);
    return in;
}
static bool PcContent(filter_t *f, struct filter_sys_t *sys)
{
    if (!sys->pc || pmx_abi() < 3) return false;
    int cls = sys->content_state.cls, cg = sys->content_state.cg ? 1 : 0;
    if (sys->zc) {
        pm_pc_info i;
        memset(&i, 0, sizeof(i));
        i.struct_size = sizeof(i);
        if (pmx_output(sys->pc, &i) != 0) return false;
        cls = i.measured_class; cg = i.measured_cg;
        if (cls && (cls != sys->content_state.cls || (cg != 0) != (sys->content_state.cg != 0))) {   /* the log, as ContentMeasure */
            msg_Info(f, "PushMPC: content %s%s, measured on the GPU", ClassName(cls), cls == 1 ? (cg ? ", rendered 3D CG" : ", hand-drawn") : "");
            sys->content_state.cls = cls; sys->content_state.cg = cg;
        }
    }
    /* PUSHMPC_VLC_DRAWN: unset follows the measured class, 0 off, anything else on (tests; also before anything is
     * measured, e.g. without the content library, which the test runner does not install next to the module) */
    const char *ov = getenv("PUSHMPC_VLC_DRAWN");
    const bool forced = ov && *ov && ov[0] != '0';
    if (cls == 0 && !forced) return false;
    const mtime_t now = mdate();
    /* held frames follow the measured class (DrawnPolicy holdRepair: drawn, hand-drawn or CG), at most once per 30 s,
     * where the output pictures can wait (the zero-copy ring, or heap pictures with pushmpc_out) */
#ifdef _WIN32
    const bool ring = sys->ring != NULL;
#else
    const bool ring = false;
#endif
    const bool can_lag = OptInt(f, CFG "hold-repair", 1) != 0 && (sys->zc ? ring : sys->alloc == 2 || OutFilterInChain(f));
    const bool want = can_lag && (ov && *ov ? forced : cls == 1);
    const bool held_change = want != sys->pc_held && (!sys->pc_held_at || now - sys->pc_held_at >= 30 * CLOCK_FREQ);
    if (!held_change) {
        if (cls == sys->pc_cls && cg == sys->pc_cg) return false;
        if (sys->pc_class_at && now - sys->pc_class_at < 10 * CLOCK_FREQ) return false;
    }
    const bool held = held_change ? want : sys->pc_held;
    sys->pc_class_at = now;
    sys->pc_cls = cls; sys->pc_cg = cg;
    pm_pc_cfg c = sys->pc_cfg;
    c.content_class = cls; c.content_cg = cg;
    c.real_out = sys->pc_lag || held ? 1 : 0;
    c.held_frames = held ? PM_PC_AUTO : 0; c.held_wait = held ? 1 : 0; c.deband = held ? PM_PC_AUTO : 0;
    char err[200] = "";
    const int r = pmx_update(sys->pc, &c, err, sizeof(err));
    if (held_change) sys->pc_held_at = now;
    if (r >= 0) {
        pm_pc_info i;
        memset(&i, 0, sizeof(i));
        i.struct_size = sizeof(i);
        sys->pc_cfg = c;
        if (c.real_out && !sys->pc_lag) {   /* lag mode from the next picture on */
            sys->pc_lag = true;
            WorkerIdle(sys, false);
#ifdef _WIN32
            if (sys->ring) RingGrow(sys->ring, (PM_PC_LAG + 2) * (sys->k + 1) + 8);
#endif
            if (sys->prev) {   /* the frame still waiting: lag mode's first stand-in (pushcore dropped its pair in the reload) */
                sys->pc_fb = LagCopy(f, sys, sys->prev);
                sys->pc_prev_date = sys->prev->date;
                sys->pc_have_prev = sys->pc_fb != NULL;
                picture_Release(sys->prev);
                sys->prev = NULL;
            }
        }
        const bool was = sys->pc_held;
        sys->pc_held = held && pmx_output(sys->pc, &i) == 0 && i.held_frames > 0;
        if (was != sys->pc_held) {
            sys->pc_held_switches++;
            msg_Info(f, "PushMPC: content %s: pushcore %s", ClassName(cls),
                     sys->pc_held ? "re-times held frames from here (every picture is pushcore's, up to 14 source frames later)" : "no longer re-times held frames");
        }
    }
    if (r == 0) {
        sys->pc_updates++;
        msg_Info(f, "PushMPC: content %s%s: pushcore's Auto decided again: %s", ClassName(cls), cg ? " (rendered 3D CG)" : "", pmx_describe(sys->pc));
        return true;
    }
    if (r == 1) msg_Dbg(f, "PushMPC: content %s%s: pushcore's settings stay", ClassName(cls), cg ? " (rendered 3D CG)" : "");
    else msg_Warn(f, "PushMPC: content %s: pushcore's settings not changed (%s)", ClassName(cls), err);
    return false;
}

/* held-drawing repair: the waiting pictures dropped (flush, close; the engine side goes with pm_engine_forget) */
static void LagDrop(struct filter_sys_t *sys)
{
    for (int i = 0; i < sys->lagn; i++) picture_Release(sys->lagq[i].pic);
    sys->lagn = 0;
    if (sys->lag_a) { picture_Release(sys->lag_a); sys->lag_a = NULL; }
    if (sys->still_pic) { picture_Release(sys->still_pic); sys->still_pic = NULL; }
    sys->lag_seen = false;
}
/* the pair's picture as repaired (a new output picture), or a itself when the engine did not change it */
static picture_t *LagReal(filter_t *f, struct filter_sys_t *sys, picture_t *a, int which)
{
    picture_t *o = NewOut(f, sys);
    if (!o) return a;
    int r = -1;
    if (sys->zc_hw) {
        pm_dmabuf d; char err[200];
        if (pm_hw_export(sys->hw, o, &d) == 0) { r = pm_engine_repair_real_dmabuf(sys->eng, which, &d, err, sizeof(err)); pm_hw_dmabuf_close(&d); }
    }
    if (r != 0) {
        picture_t *dst = sys->hw ? pm_hw_cpu_picture(sys->hw) : o;
        if (!dst) { picture_Release(o); return a; }
        pm_yuv_out y; PlanesOut(dst, &sys->wfmt, sys->bps, &y);
        r = pm_engine_repair_real(sys->eng, which, &y);
        if (r == 0 && dst != o) r = pm_hw_upload(sys->hw, dst, o) == 0 ? 0 : -1;
        if (dst != o) picture_Release(dst);
    }
    if (r != 0) { picture_Release(o); return a; }
    picture_CopyProperties(o, a); o->date = a->date; o->b_force = a->b_force; o->p_next = NULL;
    picture_Release(a); sys->lag_repaired++;
    return o;
}
/* the portable engines with the held-drawing repair: pairs lag pm_engine_repair_lag() source frames (a held part is laid
 * in once its next drawing has been seen), so the pictures wait here and each ready pair is shown and generated */
static picture_t *LagFilter(filter_t *f, struct filter_sys_t *sys, picture_t *cur, picture_t *work, bool async)
{
    char err[200] = "";
    /* async (the worker, with pushmpc_out): the check below only keeps the books; the repair, the pair's advance and
     * its pictures are the worker's (pushmpc_out waits for each); the caller made the worker idle */
    pm_engine_defer_repair(sys->eng, async ? 1 : 0);
    if (!async) pm_engine_run_deferred(sys->eng);
    const mtime_t dt0 = sys->lag_seen ? cur->date - sys->held_last : 0;
    const bool contiguous = sys->lag_seen && dt0 > 0 && dt0 <= 4 * sys->nominal_dt && dt0 <= CLOCK_FREQ / 4;
    sys->lag_seen = true; sys->held_last = cur->date;
    const mtime_t base0 = sys->held_base;   /* a still's last repeat before this picture */
    picture_t *head = NULL, *tail = NULL;
#define LAG_EMIT(p) do { (p)->p_next = NULL; if (tail) tail->p_next = (p); else head = (p); tail = (p); sys->out++; } while (0)
    if (EngineCheck(sys, work, contiguous) && contiguous) {   /* a repeat: consumed, the engine counts it */
        sys->repeats++;
        if (work != cur) picture_Release(work);
        picture_Release(cur);
        sys->held++;
        if (sys->still && sys->still_pic) {                     /* a still runs: the last repeat's interval at the output rate */
            for (picture_t *p = StillFill(sys->still_pic, base0, sys->held_last, sys->k, &sys->still_out), *nx; p; p = nx) {
                nx = p->p_next; LAG_EMIT(p);
            }
        }
    } else {
        if (EnginePush(sys, work, contiguous, err, sizeof(err)) != 0) msg_Warn(f, "engine: %s", err);
        if (work != cur) picture_Release(work);
        if (sys->lagn == PM_LAGQ) { picture_Release(cur); return NULL; }   /* cannot happen with lag 3 */
        sys->lagq[sys->lagn].pic = cur;
        sys->lagq[sys->lagn].span = contiguous && !sys->still ? sys->held + 1 : 1;
        sys->lagq[sys->lagn].contig = contiguous;
        sys->lagq[sys->lagn].base = contiguous && sys->still ? sys->held_base : 0;   /* after a still: only its last interval moves */
        sys->lagn++;
        sys->held = 0; sys->still = false;
        if (sys->still_pic) { picture_Release(sys->still_pic); sys->still_pic = NULL; }
    }
    const int ready = pm_engine_pairs_ready(sys->eng);
    if (async && (ready == 0 || (ready == 1 && sys->lag_a && sys->lagn > 0 && sys->k * sys->lagq[0].span - 1 <= PM_JOB_MAX))) {
        /* the usual step: at most one pair ready. Its real picture (repaired when its turn came) goes out now, its
         * generated pictures go out empty and the worker fills them in order; the frame's deferred repair runs first */
        pm_job *j = JobNew();
        if (j) {
            j->lag = true;
            if (ready == 1) {
                const int span = sys->lagq[0].span; const bool contig = sys->lagq[0].contig; const mtime_t base = sys->lagq[0].base;
                picture_t *b = sys->lagq[0].pic;
                memmove(&sys->lagq[0], &sys->lagq[1], (size_t)(--sys->lagn) * sizeof(sys->lagq[0]));
                picture_t *a = sys->lag_a;
                sys->lag_a = b;
                if (base) { picture_t *c = picture_Clone(a); if (c) { picture_CopyProperties(c, a); c->date = base; picture_Release(a); a = c; } }
                a = LagReal(f, sys, a, 1);   /* before the advance: the pair's B is a */
                LAG_EMIT(a);
                j->advance = true; j->contiguous = contig;
                if (!contig) sys->restarts++;
                else {
                    j->real = picture_Hold(a);
                    const mtime_t dt = b->date - a->date;
                    for (int i = 0; i < sys->k * span - 1; i++) {
                        picture_t *o = NewOut(f, sys);
                        if (!o) break;
                        picture_t *st = NULL;
                        if (sys->hw && !sys->zc_hw && !(st = pm_hw_cpu_picture(sys->hw))) { picture_Release(o); break; }
                        picture_CopyProperties(o, a);
                        o->date = a->date + dt * (i + 1) / (sys->k * span);
                        o->b_force = false;
                        j->outs[i] = picture_Hold(o);
                        j->stage[i] = st;
                        j->t[i] = (float)(i + 1) / (float)(sys->k * span);
                        j->n = i + 1;
                        JobHold(j);
                        RegAdd(o, j, i + 1);
                        LAG_EMIT(o); sys->generated++;
                    }
                }
                sys->lag_jobs++;
            }
            Submit(sys, j);
        } else pm_engine_run_deferred(sys->eng);
    } else {
        if (async) pm_engine_run_deferred(sys->eng);   /* several pairs at once (after a gap): here, in order */
        while (pm_engine_pairs_ready(sys->eng) > 0) {
            if (pm_engine_next_pair(sys->eng, err, sizeof(err)) != 1) { msg_Warn(f, "engine: %s", err); break; }
            const int span = sys->lagq[0].span; const bool contig = sys->lagq[0].contig; const mtime_t base = sys->lagq[0].base;
            picture_t *b = sys->lagq[0].pic;
            memmove(&sys->lagq[0], &sys->lagq[1], (size_t)(--sys->lagn) * sizeof(sys->lagq[0]));
            picture_t *a = sys->lag_a;
            sys->lag_a = b;
            if (!a) continue;                                         /* the first picture shows with the next pair */
            if (base) { picture_t *c = picture_Clone(a); if (c) { picture_CopyProperties(c, a); c->date = base; picture_Release(a); a = c; } }
            a = LagReal(f, sys, a, 0);
            LAG_EMIT(a);
            if (!contig || !pm_engine_have_pair(sys->eng)) { sys->restarts++; continue; }
            const mtime_t dt = sys->lag_a->date - a->date;
            for (int j = 1; j < sys->k * span; j++) {             /* as the synchronous path, with (a, lag_a) for (prev, cur) */
                picture_t *o = NewOut(f, sys); if (!o) break;
                picture_t *dstpic = o;
                picture_CopyProperties(o, a);
                const float t = (float)j / (float)(sys->k * span);
                if (sys->zc_hw) { if (EngineGenerateHw(sys, t, o) != 0) { sys->hw_up_fail++; picture_Release(o); continue; } }
                else {
                    if (sys->hw && !(dstpic = pm_hw_cpu_picture(sys->hw))) { picture_Release(o); break; }
                    pm_yuv_out dst; PlanesOut(dstpic, &sys->wfmt, sys->bps, &dst);
                    pm_engine_generate(sys->eng, t, &dst);
                }
                if (dstpic != o) { const int up = pm_hw_upload(sys->hw, dstpic, o); picture_Release(dstpic); if (up != 0) { sys->hw_up_fail++; picture_Release(o); continue; } }
                o->date = a->date + dt * j / (sys->k * span); o->b_force = false;
                RegAdd(o, NULL, 0);
                LAG_EMIT(o); sys->generated++;
            }
        }
    }
    /* a still (more repeats than PM_HOLD): the last picture now, as repaired, when nothing older still waits */
    if (sys->held > PM_HOLD && !sys->still && sys->lagn == 0 && sys->lag_a) {
        WorkerIdle(sys, false);   /* its advance may be the worker's job just submitted */
        sys->still = true; sys->stills++; sys->held_base = sys->held_last;
        picture_t *c = LagReal(f, sys, picture_Hold(sys->lag_a), 1);
        if (c == sys->lag_a) { picture_Release(c); c = picture_Clone(sys->lag_a); if (c) picture_CopyProperties(c, sys->lag_a); }
        if (c) {   /* the held span so far (from the last drawing to this repeat) at the output rate; kept for the next repeats */
            if (sys->still_pic) picture_Release(sys->still_pic);
            sys->still_pic = c;
            for (picture_t *p = StillFill(c, c->date, sys->held_last, sys->k * sys->held, &sys->still_out), *nx; p; p = nx) {
                nx = p->p_next; LAG_EMIT(p);
            }
        }
    }
    if (sys->still) sys->held_base = sys->held_last;          /* the base moves to the newest repeat */
#undef LAG_EMIT
    return head;
}

/* ---- pushcore's lag mode (see the header): every picture from pushcore, routed by its time into an output picture,
 * given out in order once it is in, or as the stand-in copy once pushcore cannot deliver it any more */
typedef struct { filter_t *f; struct filter_sys_t *sys; pm_pc_out dst; pm_pc_tex tex; int idx; } pc_lag_route;
static int LagIndex(struct filter_sys_t *sys, int64_t pts100ns)
{
    const int64_t tol = (int64_t)sys->nominal_dt * 10 / (2 * (sys->k > 0 ? sys->k : 1));
    int best = -1; int64_t bd = INT64_MAX;
    for (int i = 0; i < sys->pcn; i++) {
        if (sys->pcq[i].pic || sys->pcq[i].routing) continue;
        const int64_t d = llabs((int64_t)sys->pcq[i].date * 10 - pts100ns);
        if (d <= tol && d < bd) { best = i; bd = d; }
    }
    return best;
}
static picture_t *LagRouteNew(pc_lag_route *r, int64_t pts100ns)
{
    const int i = LagIndex(r->sys, pts100ns);
    if (i < 0) { r->sys->pc_unrouted++; return NULL; }
    picture_t *o = NewOut(r->f, r->sys);
    if (!o) return NULL;
    r->sys->pcq[i].routing = o;
    r->idx = i;
    r->sys->pc_routed++;
    return o;
}
static const pm_pc_out *LagDest(void *user, int64_t pts100ns)
{
    pc_lag_route *r = user;
    picture_t *o = LagRouteNew(r, pts100ns);
    if (!o) return NULL;
    pm_yuv_out y;
    PlanesOut(o, &r->sys->wfmt, r->sys->bps_w, &y);
    for (int p = 0; p < 3; p++) { r->dst.data[p] = y.data[p]; r->dst.stride[p] = y.stride[p]; }
    return &r->dst;
}
#ifdef _WIN32
static const pm_pc_tex *LagDestTex(void *user, int64_t pts100ns)
{
    pc_lag_route *r = user;
    picture_t *o = LagRouteNew(r, pts100ns);
    if (!o) return NULL;
    pm_d3d11_picsys *ps = PicSys(o);
    if (!ps || !ps->texture[0]) { r->sys->pcq[r->idx].routing = NULL; picture_Release(o); return NULL; }
    r->tex.texture = ps->texture[0];
    r->tex.slice = ps->slice_index;
    return &r->tex;
}
#endif
static void LagDone(void *user, int64_t pts100ns)
{
    pc_lag_route *r = user;
    struct filter_sys_t *sys = r->sys;
    for (int i = 0; i < sys->pcn; i++)
        if (sys->pcq[i].routing) {
            const int64_t d = llabs((int64_t)sys->pcq[i].date * 10 - pts100ns);
            if (d <= (int64_t)sys->nominal_dt * 10 / (2 * (sys->k > 0 ? sys->k : 1))) { sys->pcq[i].pic = sys->pcq[i].routing; sys->pcq[i].routing = NULL; return; }
        }
}
/* a copy of src into an output picture (the stand-in); NULL when there is no picture or no copy */
static picture_t *LagCopy(filter_t *f, struct filter_sys_t *sys, picture_t *src)
{
    if (!src) return NULL;
    picture_t *o = NewOut(f, sys);
    if (!o) return NULL;
#ifdef _WIN32
    if (sys->zc) {
        pm_d3d11_picsys *a = PicSys(src), *b = PicSys(o);
        if (!a || !b || !a->texture[0] || !b->texture[0] ||
            pmx_copy_d3d11(sys->pc, a->texture[0], a->slice_index, b->texture[0], b->slice_index) != 0) { picture_Release(o); return NULL; }
    } else
#endif
    picture_Copy(o, src);
    picture_CopyProperties(o, src);
    o->date = src->date;
    o->p_next = NULL;
    return o;
}
static void PcLagDrop(struct filter_sys_t *sys)
{
    for (int i = 0; i < sys->pcn; i++) {
        if (sys->pcq[i].pic) picture_Release(sys->pcq[i].pic);
        if (sys->pcq[i].routing) picture_Release(sys->pcq[i].routing);
        if (sys->pcq[i].fb) picture_Release(sys->pcq[i].fb);
    }
    sys->pcn = 0;
    if (sys->pc_fb) { picture_Release(sys->pc_fb); sys->pc_fb = NULL; }
    sys->pc_have_prev = false;
}
static picture_t *PcLagFilter(filter_t *f, struct filter_sys_t *sys, picture_t *cur, picture_t *work, bool reloaded)
{
    char err[200] = "";
    const mtime_t dt = sys->pc_have_prev ? cur->date - sys->pc_prev_date : 0;
    const bool contiguous = !reloaded && sys->pc_have_prev && dt > 0 && dt <= 4 * sys->nominal_dt && dt <= CLOCK_FREQ / 4;
    picture_t *fb = LagCopy(f, sys, work);     /* before the push: what this frame's interval shows where pushcore misses */
    if (fb) { picture_CopyProperties(fb, cur); fb->date = cur->date; }
    int pushed = -1;
#ifdef _WIN32
    if (sys->zc) {
        pm_d3d11_picsys *in = PicSys(cur);
        if (in && in->texture[0]) pushed = pmx_push_d3d11(sys->pc, in->texture[0], in->slice_index, (int64_t)cur->date * 10, !contiguous, err, sizeof(err));
    } else
#endif
    {
        pm_yuv src;
        PlanesIn(work, &sys->wfmt, sys->bps_w, &src);
        pm_pc_planes pl;
        for (int p = 0; p < 3; p++) { pl.data[p] = src.data[p]; pl.stride[p] = src.stride[p]; }
        pushed = pmx_push(sys->pc, &pl, (int64_t)cur->date * 10, !contiguous, err, sizeof(err));
    }
    if (pushed != 0 && !sys->pc_errors++) snprintf(sys->pc_err, sizeof(sys->pc_err), "push: %s", err);
    if (work != cur) picture_Release(work);
    picture_Release(cur);                       /* the bridge copied it: no decoder surface is kept */
    if (sys->pc_have_prev) {                    /* the interval that ended: its real picture and k - 1 generated ones */
        if (!contiguous) sys->restarts++;
        const int n = contiguous ? sys->k : 1;
        for (int j = 0; j < n && sys->pcn < PM_PCQ; j++) {
            sys->pcq[sys->pcn].date = sys->pc_prev_date + (contiguous ? dt * j / sys->k : 0);
            sys->pcq[sys->pcn].pic = sys->pcq[sys->pcn].routing = NULL;
            sys->pcq[sys->pcn].fb = sys->pc_fb ? picture_Hold(sys->pc_fb) : NULL;
            sys->pcq[sys->pcn].age = 0;
            sys->pcq[sys->pcn].real = j == 0;
            sys->pcn++;
        }
    }
    if (sys->pc_fb) picture_Release(sys->pc_fb);
    sys->pc_fb = fb;
    sys->pc_prev_date = cur->date;
    sys->pc_have_prev = true;
    /* route what pushcore has finished; wait (2.5 source frames at most) only when the oldest picture is due */
    const int lag = sys->pc_held ? PM_PC_LAG : 1;
    pc_lag_route r = { .f = f, .sys = sys };
    bool wait = sys->pcn > 0 && !sys->pcq[0].pic && sys->pcq[0].age >= lag;
    mtime_t full = sys->nominal_dt * 5 / 2;
    full = full < 40000 ? 40000 : full > 250000 ? 250000 : full;
    const mtime_t deadline = mdate() + full;
    for (int guard = 0; guard < 256; guard++) {
        int got;
#ifdef _WIN32
        if (sys->zc) got = pmx_poll_d3d11(sys->pc, wait ? 5 : 0, LagDestTex, LagDone, &r, err, sizeof(err));
        else
#endif
        got = pmx_poll(sys->pc, wait ? 5 : 0, LagDest, LagDone, &r, err, sizeof(err));
        if (got < 0) { if (!sys->pc_errors++) snprintf(sys->pc_err, sizeof(sys->pc_err), "poll: %s", err); break; }
        if (wait && (sys->pcq[0].pic || mdate() > deadline)) wait = false;
        if (!wait && got == 0) break;
    }
    for (int i = 0; i < sys->pcn; i++)          /* routed but never completed: not delivered */
        if (sys->pcq[i].routing) { picture_Release(sys->pcq[i].routing); sys->pcq[i].routing = NULL; }
    for (int i = 0; i < sys->pcn; i++) sys->pcq[i].age++;
    /* give out in order: what is in; a missing one once it is too old or a later one is in (pushcore keeps order) */
    picture_t *head = NULL, *tail = NULL;
    while (sys->pcn > 0) {
        bool later = false;
        for (int i = 1; i < sys->pcn && !later; i++) later = sys->pcq[i].pic != NULL;
        if (!sys->pcq[0].pic && sys->pcq[0].age <= lag && !later) break;
        picture_t *o = sys->pcq[0].pic;
        if (o) { if (sys->pcq[0].fb) picture_CopyProperties(o, sys->pcq[0].fb); if (!sys->pcq[0].real) sys->generated++; }
        else { o = LagCopy(f, sys, sys->pcq[0].fb); sys->pc_lag_missed++; }
        if (o) {
            o->date = sys->pcq[0].date;
            o->b_force = false;
            o->p_next = NULL;
            if (tail) tail->p_next = o; else head = o;
            tail = o;
            sys->out++; sys->pc_lag_out++;
        }
        if (sys->pcq[0].fb) picture_Release(sys->pcq[0].fb);
        memmove(&sys->pcq[0], &sys->pcq[1], (size_t)(--sys->pcn) * sizeof(sys->pcq[0]));
    }
    return head;
}

static picture_t *Filter(filter_t *f, picture_t *cur)
{
    struct filter_sys_t *sys = (struct filter_sys_t *)f->p_sys;
    sys->in++;
#ifdef __APPLE__
    if (sys->cvpx) return FilterCvpx(f, sys, cur);
#endif
    if (sys->download) {        /* our own converter in front, so our extra pictures reach the static chain intact */
        cur = filter_chain_VideoFilter(sys->download, cur);
        if (!cur) return NULL;
    }
    if (sys->k <= 1 || (!sys->eng && !sys->pc)) { sys->out++; return cur; }
    if (!sys->rate_done && sys->eng) {   /* the measured source rate (16 date steps, the median) */
        if (sys->rate_last > 0 && cur->date > sys->rate_last && cur->date - sys->rate_last < 250000) sys->rate_steps[sys->rate_n++] = cur->date - sys->rate_last;
        sys->rate_last = cur->date;
        if (sys->rate_n == 16) {
            mtime_t v[16]; memcpy(v, sys->rate_steps, sizeof(v));
            for (int a = 1; a < 16; a++) { mtime_t x = v[a]; int b = a - 1; while (b >= 0 && v[b] > x) { v[b + 1] = v[b]; b--; } v[b + 1] = x; }
            const double fps = 1e6 / (double)v[8];
            sys->rate_done = 1; sys->rate_fps = fps;
            if (sys->fs_fps <= 0 || fabs(fps - sys->fs_fps) / fps > 0.05) {
                char why[256] = "";
                int k = pm_engine_decide(sys->eng, sys->fs_smoother, fps, sys->fs_hz, why, sizeof(why));
                k = k < 2 ? 2 : k > sys->k_open ? sys->k_open : k;
                msg_Info(f, "PushMPC: source rate measured from the picture dates %.3f fps (the format says %.3f): x%d (%s)", fps, sys->fs_fps, k, why);
                sys->fs_fps = fps; sys->k = k;
            }
        }
    }
    if (sys->in % 30 == 0) {   /* a fullscreen toggle mid-file: var_Inherit reads the vout's live variable */
        const int fs = var_InheritBool(f, "fullscreen") ? 1 : 0;
        if (fs != sys->fs) {
            sys->fs = fs;
            if (sys->eng) {
                char why[256] = "";
                pm_engine_set_fullscreen(sys->eng, fs);
                int k = pm_engine_decide(sys->eng, sys->fs_smoother, sys->fs_fps, sys->fs_hz, why, sizeof(why));
                k = k < 2 ? 2 : k > sys->k_open ? sys->k_open : k;
                msg_Info(f, "PushMPC: %s: x%d (%s)", fs ? "full screen" : "windowed", k, why);
                sys->k = k;
            } else msg_Info(f, "PushMPC: %s; pushcore keeps x%d until the next file", fs ? "full screen" : "windowed", sys->k);
        }
    }

    /* what the engine reads: cur itself, or for hardware formats a CPU copy of its surface */
    picture_t *work = cur;
    if (sys->hw) {
        work = HwIn(f, sys, cur);
        if (!work) {
            WorkerIdle(sys, false);
            if (sys->hw_failed) {   /* for good: pass-through (decided on the first picture, so nothing is held) */
                sys->out++;
                return cur;
            }
            /* this surface could not be read: show the real frame held so far, then start the pair over */
            picture_t *p = sys->prev;
            sys->prev = cur;
            sys->held = 0; sys->still = false; sys->held_last = cur->date;   /* held frames start over too */
            if (sys->eng) pm_engine_forget(sys->eng);
            sys->restarts++;
            if (p) { p->p_next = NULL; sys->out++; }
            return p;
        }
    }

    ContentMeasure(f, sys, cur, work);   /* before the held-frame step: repeats are part of what is measured */

    if (PortableRepairWanted(f, sys)) {   /* measured as drawn: the pairs lag from this frame on */
        WorkerIdle(sys, false);
        PortableRepairOn(f, sys);
        if (sys->still && sys->prev) { picture_Release(sys->prev); sys->prev = NULL; }   /* a still showed it already */
        sys->held = 0; sys->still = false;
        if (sys->still_pic) { picture_Release(sys->still_pic); sys->still_pic = NULL; }
    }
    const bool async = sys->worker_on && OutStages() > 0 && !sys->zc && !sys->lag;
    if (sys->lag && !sys->pc) {
        WorkerIdle(sys, false);
        picture_t *pre = sys->prev;   /* the switch above: the frame still waiting goes out alone (its pair restarts) */
        sys->prev = NULL;
        /* the worker too, where pushmpc_out can wait for its pictures (as above) */
        picture_t *r = LagFilter(f, sys, cur, work, sys->worker_on && OutStages() > 0 && !sys->zc);
        if (pre) { pre->p_next = r; sys->out++; sys->restarts++; r = pre; }
        return r;
    }
    if (async || sys->pc) {     /* pushcore always runs through jobs (inline when there is no worker) */
        /* worker mode: the previous job has had most of a frame interval; its last picture may still be on its way
         * (VLC asks for the next source frame while that picture is queued as "next"), so wait for it to finish */
        WorkerIdle(sys, false);
        const bool reloaded = PcContent(f, sys);   /* the measured class to pushcore's Auto (no job in flight now) */
        if (sys->pc_lag) return PcLagFilter(f, sys, cur, work, reloaded);
        int span, hc;                               /* held frames: a repeat waits (HeldStep) */
        picture_t *still;
        if (HeldStep(sys, cur, work, &span, &hc, &still)) { for (picture_t *p = still; p; p = p->p_next) sys->out++; return still; }
        picture_t *prev = sys->prev;
        sys->prev = cur;
        const mtime_t dt = prev ? cur->date - prev->date : 0;
        const bool contiguous = !reloaded && (hc >= 0 ? hc != 0 : prev && dt > 0 && dt <= 4 * sys->nominal_dt && dt <= CLOCK_FREQ / 4);
        pm_job *j = JobNew();
        if (!j) {
            if (work != cur) picture_Release(work);
            if (prev) { sys->out++; prev->p_next = NULL; }
            return prev;
        }
        j->src = work == cur ? picture_Hold(cur) : work;
        j->contiguous = contiguous;
        picture_t *head = prev, *tail = prev;
        if (prev && contiguous) {
            prev->p_next = NULL;
            j->real = picture_Hold(prev);
            for (int i = 0; i < sys->k * span - 1 && i < PM_JOB_MAX; i++) {
                picture_t *o = NewOut(f, sys);
                if (!o) break;
                picture_t *st = NULL;
                if (sys->hw && !sys->zc_hw && !(st = pm_hw_cpu_picture(sys->hw))) { picture_Release(o); break; }
                picture_CopyProperties(o, prev);
                o->date = prev->date + dt * (i + 1) / (sys->k * span);
                o->b_force = false;
                o->p_next = NULL;
                j->outs[i] = picture_Hold(o);       /* the worker writes into it */
                j->stage[i] = st;
                j->t[i] = (float)(i + 1) / (float)(sys->k * span);
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
        else {
            RunJob(sys, j);
            /* inline: a hardware picture whose upload failed is taken out of the list */
            for (int i = 0; i < j->n && sys->hw; i++) {
                if (!j->failed[i]) continue;
                for (picture_t *p = head; p && p->p_next; p = p->p_next)
                    if (p->p_next == j->outs[i]) {
                        p->p_next = j->outs[i]->p_next;
                        j->outs[i]->p_next = NULL;
                        picture_Release(j->outs[i]);   /* the list's reference; the job keeps its own */
                        break;
                    }
            }
            JobRelease(j);
        }
        if (!prev) return NULL;
        for (picture_t *p = head; p; p = p->p_next) sys->out++;
        return head;
    }
    WorkerIdle(sys, false);     /* synchronous mode below; the engine must not be shared */

    int span, hc;                                 /* held frames: a repeat waits (HeldStep) */
    picture_t *still;
    if (HeldStep(sys, cur, work, &span, &hc, &still)) { for (picture_t *p = still; p; p = p->p_next) sys->out++; return still; }
    picture_t *prev = sys->prev;
    sys->prev = cur;
    mtime_t dt = prev ? cur->date - prev->date : 0;
    const bool contiguous = hc >= 0 ? hc != 0 : prev && dt > 0 && dt <= 4 * sys->nominal_dt && dt <= CLOCK_FREQ / 4;
    char err[160] = "";
    const int pushed = EnginePush(sys, work, contiguous, err, sizeof(err));
    if (work != cur) picture_Release(work);        /* the engine keeps its own copy */
    if (pushed != 0) {
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
        for (int j = 1; j < sys->k * span; j++) {
            picture_t *o = NewOut(f, sys);
            if (!o) break;
            picture_t *dstpic = o;
            picture_CopyProperties(o, prev);
            if (sys->zc_hw) {           /* straight into the output surface */
                if (EngineGenerateHw(sys, (float)j / (float)(sys->k * span), o) != 0) { sys->hw_up_fail++; picture_Release(o); continue; }
            } else {
                if (sys->hw && !(dstpic = pm_hw_cpu_picture(sys->hw))) { picture_Release(o); break; }
                pm_yuv_out dst;
                PlanesOut(dstpic, &sys->wfmt, sys->bps, &dst);
                pm_engine_generate(sys->eng, (float)j / (float)(sys->k * span), &dst);
            }
            if (dstpic != o) {
                const int up = pm_hw_upload(sys->hw, dstpic, o);
                picture_Release(dstpic);
                if (up != 0) { sys->hw_up_fail++; picture_Release(o); continue; }
            }
            o->date = prev->date + dt * j / (sys->k * span);
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
    sys->held = 0; sys->still = false;   /* held frames: the repeats waiting are dropped */
    LagDrop(sys);
    PcLagDrop(sys);
    sys->content_disc = true;            /* the detector's window starts over (its decision stays) */
    if (sys->eng) pm_engine_forget(sys->eng);
    if (sys->pc) pmx_flush(sys->pc);
    if (sys->download) filter_chain_VideoFlush(sys->download);
#ifdef __APPLE__
    if (sys->cvpx) pm_cvpx_eng_forget(sys->cvpx);
#endif
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
    /* ABI 3: the source size, VLC's own real frames (the module's header says why), Auto told the measured class later */
    pcc->struct_size = sizeof(*pcc);
    pcc->sr = 0;
    pcc->anime_model = PM_PC_ANIME_NONE;
    pcc->held_frames = 0;
    pcc->deband = 0;
    pcc->real_out = 0;
    pcc->held_wait = 0;
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
            pcc.fullscreen = var_InheritBool(f, "fullscreen") ? 1 : 0;   /* Auto: the renderer's share per shown frame, G-SYNC fullscreen-only */
            pcc.measure = OptInt(f, CFG "content", 1) ? 1 : 0;   /* ContentDetect on VLC's textures, on the GPU (in the bridge) */
            int pk = 1;
            char err[300] = "";
            /* Auto at x1 here: pushcore is not opened at all (the Direct3D 11 output's upscaler may hold it: one pushcore
             * per player, and an open takes it over); an older bridge cannot say: open and see, as before */
            int dk = 1;
            if (pmx_decide(&pcc, &dk, err, sizeof(err)) > 0 && dk <= 1)
                snprintf(err, sizeof(err), "pushcore's Auto runs x1 here (decided without opening it)");
            else
                sys->pc = ZcOpen(f, &pcc, &pk, &sys->ring, err, sizeof(err));
            if (sys->pc && pk > 1) {
                sys->zc = true;
                sys->pc_cfg = pcc;
                bits = pcc.bits;
                sys->k = pk > 16 ? 16 : pk;
                snprintf(sys->why, sizeof(sys->why), "pushcore: %s", pmx_describe(sys->pc));
            } else {
                if (sys->pc) { pmx_close(sys->pc); sys->pc = NULL; snprintf(err, sizeof(err), "pushcore runs x1 here"); }
                msg_Dbg(f, "PushMPC: zero copy not used (%s): the copy path", err);
            }
        }
    }
#endif
#ifdef __APPLE__
    {   /* VideoToolbox pictures with Apple's interpolation: zero copy when this VLC's CVPX layout is the tested one and
         * the apple engine runs here; else VLC's converter in front of the engine (one copy) */
        const int cbits = pm_cvpx_bits(v->i_chroma);
        const int engine = (int)OptInt(f, CFG "engine", 0), smoother = (int)OptInt(f, CFG "smoother", 0);
        if (cbits && engine == 0 && smoother != 1 && OptInt(f, CFG "zerocopy", 1) && !getenv("PUSHMPC_VLC_NO_APPLE")) {
            char why[300] = "";
            if (pm_cvpx_vlc_ok(why, sizeof(why))) {
                double zhz = OptFloat(f, CFG "display-hz", 0.0);
                if (zhz <= 0) zhz = pm_display_hz();
                const double zfps = (v->i_frame_rate && v->i_frame_rate_base) ? (double)v->i_frame_rate / v->i_frame_rate_base : 0.0;
                int pk = 1;
                sys->cvpx = pm_cvpx_eng_open(v, cbits, smoother, zfps, zhz, &pk, why, sizeof(why));
                if (sys->cvpx && pk > 1) {
                    sys->k = pk > 16 ? 16 : pk;   /* the apple engine's ceiling (apple_convert.h kAppleMaxK) */
                    bits = cbits;
                    snprintf(sys->why, sizeof(sys->why), "apple engine (VideoToolbox), zero copy on CVPX: %s", pm_cvpx_eng_describe(sys->cvpx));
                } else if (sys->cvpx) {
                    pm_cvpx_eng_close(sys->cvpx); sys->cvpx = NULL; snprintf(why, sizeof(why), "x1 here");
                }
            }
            if (!sys->cvpx) msg_Info(f, "PushMPC: CVPX zero copy not used (%s): the one-copy path", why);
        }
    }
#endif
    /* Linux VA-API / VDPAU pictures: kept as they are on both sides (pm_hw.h); the converter route is the fallback */
    if (!sys->zc && !PM_CVPX_ON(sys) && pm_hw_chroma(v->i_chroma, &bits) && OptInt(f, CFG "hw", 0) == 0)
        sys->hw = pm_hw_new(obj, v, 18);   /* up to 16x: 15 generated frames of a source frame in flight, plus 3 */
    if (sys->hw) {
        msg_Dbg(f, "PushMPC: %4.4s kept as it is (generated frames go into PushMPC's own surfaces)", (const char *)&v->i_chroma);
    } else if (!sys->zc && !PM_CVPX_ON(sys) && !Supported(v->i_chroma, &bits)) {
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
    sys->dedup = OptInt(f, CFG "dedup", 1) != 0;   /* held frames (portable engines; set on the engine at the first frame) */
    const int smoother = (int)OptInt(f, CFG "smoother", 0);
    const int quality = (int)OptInt(f, CFG "quality", 1);
    const int threads = (int)OptInt(f, CFG "threads", 0);
    double hz = OptFloat(f, CFG "display-hz", 0.0);
    if (hz <= 0) hz = pm_display_hz();
    const double fps = (v->i_frame_rate && v->i_frame_rate_base) ? (double)v->i_frame_rate / v->i_frame_rate_base : 0.0;
    sys->nominal_dt = fps > 0 ? (mtime_t)(CLOCK_FREQ / fps) : CLOCK_FREQ / 24;
    const int w = (int)v->i_visible_width, h = (int)v->i_visible_height;

    sys->wfmt = sys->hw ? *pm_hw_work_format(sys->hw) : f->fmt_out.video;
    sys->bps_w = sys->bps;
    const int engine = (int)OptInt(f, CFG "engine", 0);
    if (!sys->hw && !sys->zc && !PM_CVPX_ON(sys) && (engine == 0 || engine == 2)) {   /* pushcore first (Windows, PushMPC's core installed); else the portable engines */
        pm_pc_cfg pcc;
        FillPcc(&pcc, v, bits, smoother, hz, OptInt(f, CFG "warp", 0) ? 1 : 0);
        pcc.fullscreen = var_InheritBool(f, "fullscreen") ? 1 : 0;   /* Auto: the renderer's share per shown frame, G-SYNC fullscreen-only */
        int pk = 1;
        char err[300] = "";
        int dk = 1;
        const int dr = pmx_decide(&pcc, &dk, err, sizeof(err));   /* x1: not opened (see the zero-copy path above) */
        if (dr > 0 && dk <= 1) {
            sys->k = 1;
            snprintf(sys->why, sizeof(sys->why), "pushcore: %s; x1, not opened", err);
        } else
            sys->pc = pmx_open(&pcc, &pk, err, sizeof(err));
        if (sys->pc) {
            sys->pc_cfg = pcc;
            sys->k = pk < 1 ? 1 : pk > 16 ? 16 : pk;
            snprintf(sys->why, sizeof(sys->why), "pushcore: %s", pmx_describe(sys->pc));
            if (sys->k <= 1) { pmx_close(sys->pc); sys->pc = NULL; sys->k = 1; }
        } else if (dr > 0 && dk <= 1) {
            msg_Dbg(f, "PushMPC: %s", sys->why);
        } else if (engine == 2) {
            msg_Warn(f, "PushMPC: pushcore unavailable (%s)", err);
        } else {
            msg_Dbg(f, "PushMPC: pushcore not used (%s): portable engine", err);
        }
    }
    if (!sys->pc && !PM_CVPX_ON(sys) && smoother != 1) {
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
            pm_engine_set_fullscreen(sys->eng, var_InheritBool(f, "fullscreen") ? 1 : 0);   /* the vout's state now; Filter() follows toggles */
            sys->k = pm_engine_decide(sys->eng, smoother, fps, hz, why, sizeof(why));
            snprintf(sys->why, sizeof(sys->why), "%s; %s", pm_engine_name(sys->eng), why);
            if (sys->k <= 1) { pm_engine_close(sys->eng); sys->eng = NULL; sys->k = 1; }
            if (sys->eng && sys->dedup) {   /* held frames (Dedup) from the start; the repair once the picture is measured as drawn */
                pm_engine_set_dedup(sys->eng, 1); sys->dedup_set = true;
                if (PortableRepairWanted(f, sys)) PortableRepairOn(f, sys);
                else if (OptInt(f, CFG "hold-repair", 1))
                    msg_Info(f, "PushMPC: held-drawing repair waits for the picture to be measured as drawn (until then the pairs follow the frames)");
            }
        }
    } else if (!sys->pc && !PM_CVPX_ON(sys)) {   /* (the CVPX zero-copy path set k and why above) */
        sys->k = 1;
        snprintf(sys->why, sizeof(sys->why), "Smoother off: no frame generation");
    }

    /* x1 (Smoother off, Auto's x1, no engine): nothing to do, so the decoder's pictures pass through as they are; the
     * converter in front (SetupDownload) would only copy every hardware picture to the CPU and back */
    if (sys->k <= 1 && !sys->eng && !sys->pc && !PM_CVPX_ON(sys) && sys->download) {
        filter_chain_Delete(sys->download);
        sys->download = NULL;
        f->fmt_out.video = f->fmt_in.video;
        f->fmt_out.i_codec = f->fmt_in.i_codec;
        msg_Info(f, "PushMPC: x1: %4.4s passes through untouched (no conversion)", (const char *)&v->i_chroma);
    }
    sys->k_open = sys->k;
    if (sys->eng && sys->k > 1 && fps > 1.0) {   /* pools for a measured rate up to 20 % below the declared one (29.97 tag, 24p body) */
        char why2[256] = "";
        const int klo = pm_engine_decide(sys->eng, smoother, fps * 0.8, hz, why2, sizeof(why2));
        if (klo > sys->k_open) sys->k_open = klo > 16 ? 16 : klo;
    }
    if (sys->hw) {
        if (sys->k > 1) pm_hw_set_pool_size(sys->hw, (sys->dedup && sys->eng ? 2 + PM_HOLD : 3) * (sys->k_open > sys->k ? sys->k_open : sys->k) + 4);   /* the job's pictures (a held span: up to (PM_HOLD + 1) x k), those queued for display, a spare */
        else { pm_hw_release(sys->hw); sys->hw = NULL; }
    }
    sys->fs = var_InheritBool(f, "fullscreen") ? 1 : 0;
    sys->fs_smoother = smoother; sys->fs_fps = fps; sys->fs_hz = hz;
    if (f->fmt_out.video.i_frame_rate)
        f->fmt_out.video.i_frame_rate *= (unsigned)sys->k;
    sys->wfmt = sys->hw ? *pm_hw_work_format(sys->hw) : f->fmt_out.video;
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
             (sys->eng || sys->pc || PM_CVPX_ON(sys)) ? "" : " [pass-through]");
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
    if (sys->eng && sys->dedup_set)
        msg_Info(f, "PushMPC: held frames: %" PRIu64 " repeats waited for the next drawing, %" PRIu64 " stills (%" PRIu64 " pictures at the output rate)", sys->repeats, sys->stills, sys->still_out);
    if (sys->content) {
        pmc_state st;
        if (pm_content_state(sys->content, &st) == 0) {
            static const char *const model[] = { "not in this build", "off", "starting", "used", "not available" };
            msg_Info(f, "PushMPC: content %s%s; %" PRIu64 " frames measured (%" PRIu64 " dropped: the detector fell behind; %" PRIu64
                     " zero-copy frames not read), class changes %u; image model %s%s; per frame %.2f ms on VLC's video thread, "
                     "%.2f ms (max %.2f) on the detector's thread", pm_content_class_name(sys->content, st.cls),
                     st.cls == PMC_CLASS_DRAWN ? (st.cg ? ", rendered 3D CG" : ", hand-drawn") : "", st.processed, st.dropped,
                     sys->content_gpu, st.changes, model[st.model >= 0 && st.model <= 4 ? st.model : 4],
                     st.model == PMC_MODEL_RUNNING ? "" : " (the hand-built cues decided)", st.caller_ms, st.detect_ms, st.detect_ms_max);
        }
        if (sys->tap_frames || sys->tap_fail)
            msg_Info(f, "PushMPC: content tap: %" PRIu64 " zero-copy frames measured on the GPU, %" PRIu64 " failed", sys->tap_frames, sys->tap_fail);
        pm_content_close(sys->content);   /* its threads end here, with their last log line */
    }
    if (sys->eng && sys->lag) {
        pm_repair_stats rs; pm_engine_repair_stats_get(sys->eng, &rs);
        msg_Info(f, "PushMPC: held-drawing repair: %" PRIu64 " frames, %" PRIu64 " parts, %" PRIu64 " spans gated, %" PRIu64 " over budget (%s); pairs on the worker %" PRIu64 ", failed there %" PRIu64,
                 rs.frames, rs.parts, rs.gated, rs.over_budget, rs.gpu ? "GPU" : "CPU", sys->lag_jobs, sys->lag_errors);
    }
    free(sys->tap_words);
    LagDrop(sys);
    PcLagDrop(sys);
    if (sys->pc_lag)
        msg_Info(f, "PushMPC: pushcore's lag mode: %" PRIu64 " pictures, %" PRIu64 " not delivered (the decoded frame stood in), held frames switched %" PRIu64 " times",
                 sys->pc_lag_out, sys->pc_lag_missed, sys->pc_held_switches);
    if (sys->eng) pm_engine_close(sys->eng);
    if (sys->pc) {
        msg_Info(f, "PushMPC: pushcore routed %" PRIu64 " generated frames, %" PRIu64 " not matched (first at %" PRId64
                 " for %" PRId64 "), %" PRIu64 " errors%s%s", sys->pc_routed, sys->pc_unrouted, sys->pc_miss_pts, sys->pc_miss_want,
                 sys->pc_errors, sys->pc_errors ? ", first: " : "", sys->pc_err);
        if (sys->pc_updates) msg_Info(f, "PushMPC: pushcore's Auto decided again %" PRIu64 " times with the measured content", sys->pc_updates);
        if (sys->zc)
            msg_Info(f, "PushMPC: zero copy on VLC's D3D11 device; %" PRIu64 " generated pictures not delivered in time "
                     "(the real frame shown); %" PRIu64 " from our own texture ring", sys->zc_fallback, sys->ring_out);
        pmx_close(sys->pc);
#ifdef _WIN32
        if (sys->ring) RingRelease(sys->ring);   /* pictures still on their way keep it alive */
#endif
    }
    if (sys->hw) {
        msg_Info(f, "PushMPC: hardware path %s: %s; surface read failures %" PRIu64 ", upload failures %" PRIu64
                 ", no free surface %" PRIu64 "; zero copy %s (in %" PRIu64 ", out %" PRIu64 ", fell back %" PRIu64 ")",
                 sys->hw_ready ? "used" : sys->hw_failed ? "unavailable" : "not started", pm_hw_describe(sys->hw),
                 sys->hw_down_fail, sys->hw_up_fail, sys->hw_no_surface, sys->zc_hw ? "on" : "off", sys->zc_in, sys->zc_out, sys->zc_fail);
        pm_hw_release(sys->hw);
    }
    if (sys->download) filter_chain_Delete(sys->download);
#ifdef __APPLE__
    if (sys->cvpx) {
        msg_Info(f, "PushMPC: CVPX zero copy %s: %" PRIu64 " generated CVPX pictures, %" PRIu64 " engine errors",
                 sys->cvpx_off ? "stopped (pass-through)" : "used", sys->cvpx_gen, sys->cvpx_err);
        pm_cvpx_eng_close(sys->cvpx);
    }
#endif
    free(sys);
}

/* ---------------------------------------------------------------- pushmpc_out (interactive chain) */
typedef struct { uint64_t copied, passed, failed, waited, late; mtime_t wait_us; bool opaque; } out_sys_t;

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
        const bool ready = job->done >= idx && !job->failed[idx - 1];
        vlc_mutex_unlock(&job->lock);
        s->wait_us += mdate() - t0;
        if (!ready) { from = job->real; s->late++; }  /* not made in time (or not uploaded): the real frame instead */
    }
    if (s->opaque) {                      /* hardware surfaces (our own pool): no copy; a stand-in shares the real frame's */
        picture_t *o = p;
        if (from != p && (o = picture_Clone(job->real)) != NULL) { picture_CopyProperties(o, p); picture_Release(p); }
        else if (!o) o = p;
        if (job) JobRelease(job);
        s->copied++;
        return o;
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
    int hwbits;
    bool opaque = pm_hw_chroma(f->fmt_in.video.i_chroma, &hwbits);   /* VA-API / VDPAU: passed on, not copied */
#ifdef __APPLE__
    /* CVPX pictures (the zero-copy VideoToolbox path hands them on as they are): passed on too. Refusing them would make
     * VLC's "chain" fallback put a CVPX -> software converter in front of this stage and the vout */
    if (pm_cvpx_bits(f->fmt_in.video.i_chroma)) opaque = true;
#endif
    /* D3D11 pictures (the zero-copy path, and the static filter passing them through at x1): passed on as they are; a
     * refusal could make VLC convert them to the CPU around this stage */
    if (f->fmt_in.video.i_chroma == VLC_CODEC_D3D11_OPAQUE || f->fmt_in.video.i_chroma == VLC_CODEC_D3D11_OPAQUE_10B) opaque = true;
    if ((!opaque && (!d || d->plane_count == 0)) || f->fmt_in.video.i_chroma != f->fmt_out.video.i_chroma)
        return VLC_EGENERIC;
    out_sys_t *s = calloc(1, sizeof(*s));
    if (!s) return VLC_ENOMEM;
    f->p_sys = (void *)s;
    s->opaque = opaque;
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
