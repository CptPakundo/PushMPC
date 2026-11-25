/*
 * d3d11_warp_harness: vf_pushmpc's D3D11 zero-copy path on a machine without a video-capable GPU (hosted runners).
 * FFmpeg's own d3d11va device creation asks for D3D11_CREATE_DEVICE_VIDEO_SUPPORT and the video interfaces, which
 * WARP lacks; the filter and the engine never use them. So this harness creates the WARP device itself, hands FFmpeg
 * stub video interfaces (IUnknown only; any other call aborts with a message), builds an NV12 frame pool laid out
 * like a decoder's (one array texture, slice indices), and runs
 *     buffer (D3D11 NV12) -> pushmpc (engine module + stub pushcore) -> buffersink
 * on synthetic grey frames (luma 16 + 16 s). Checks: "zero copy" in the filter's log, 1 + (N - 1) x 2 outputs with
 * pts on the half-interval grid, and the real frames' pixels after a download (BGRA ~ (Y - 16) x 255 / 219).
 *   d3d11_warp_harness <stub pushcore folder>      (PUSHMPC_ENGINE names the engine module)
 */
#define COBJMACROS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <d3d11.h>

#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>

static int g_zero_copy = 0;
static void log_cb(void *avcl, int level, const char *fmt, va_list vl)
{
    char line[1024];
    int prefix = 1;
    av_log_format_line(avcl, level, fmt, vl, line, sizeof(line), &prefix);
    if (level <= AV_LOG_INFO) fputs(line, stdout);
    if (strstr(line, "pushcore") && strstr(line, "zero copy")) g_zero_copy = 1;
}

/* ---- a COM object with IUnknown only, standing in for ID3D11VideoDevice / ID3D11VideoContext */
typedef struct Stub { void **vtbl; LONG refs; const char *name; } Stub;
static HRESULT STDMETHODCALLTYPE stub_qi(Stub *s, REFIID r, void **o) { (void)s; (void)r; *o = NULL; return E_NOINTERFACE; }
static ULONG STDMETHODCALLTYPE stub_addref(Stub *s) { return (ULONG)InterlockedIncrement(&s->refs); }
static ULONG STDMETHODCALLTYPE stub_release(Stub *s) { return (ULONG)InterlockedDecrement(&s->refs); }
static void STDMETHODCALLTYPE stub_abort(void) { fprintf(stderr, "harness: a video interface method was called (not supported on WARP)\n"); abort(); }
static void *g_vtbl[128];
static Stub g_video_device = { g_vtbl, 1, "ID3D11VideoDevice" }, g_video_context = { g_vtbl, 1, "ID3D11VideoContext" };

int main(int argc, char **argv)
{
    const char *stub_dir = argc > 1 ? argv[1] : ".";
    const int N = 12, W = 256, H = 144;
    ID3D11Device *dev = NULL; ID3D11DeviceContext *ctx = NULL;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
    AVBufferRef *devref = NULL, *frames = NULL;
    AVFilterGraph *graph = NULL;
    AVFilterContext *src = NULL, *flt = NULL, *sink = NULL;
    AVBufferSrcParameters *par;
    int i, ret, outputs = 0, reals = 0, bad_pts = 0, bad_px = 0;
    int64_t last_pts = -1;
    char opts[1024];

    for (i = 0; i < 128; i++) g_vtbl[i] = (void *)stub_abort;
    g_vtbl[0] = (void *)stub_qi; g_vtbl[1] = (void *)stub_addref; g_vtbl[2] = (void *)stub_release;
    av_log_set_callback(log_cb);

    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, &fl, 1, D3D11_SDK_VERSION, &dev, NULL, &ctx))) {
        fprintf(stderr, "harness: no WARP device\n"); return 2;
    }
    {
        UINT fs = 0;
        ID3D11Device_CheckFormatSupport(dev, DXGI_FORMAT_NV12, &fs);
        printf("harness: WARP NV12 support 0x%x\n", fs);
    }
    devref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    {
        AVD3D11VADeviceContext *h = ((AVHWDeviceContext *)devref->data)->hwctx;
        void *vd = NULL;
        h->device = dev;   /* FFmpeg releases it */
        h->device_context = ctx;
        if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_ID3D11VideoDevice, &vd))) {
            printf("harness: WARP has a video device\n");
            h->video_device = vd;
            ID3D11DeviceContext_QueryInterface(ctx, &IID_ID3D11VideoContext, (void **)&h->video_context);
        }
        if (!h->video_device) h->video_device = (ID3D11VideoDevice *)&g_video_device;
        if (!h->video_context) h->video_context = (ID3D11VideoContext *)&g_video_context;
    }
    if ((ret = av_hwdevice_ctx_init(devref)) < 0) { fprintf(stderr, "harness: av_hwdevice_ctx_init %d\n", ret); return 1; }

    {   /* a decoder's layout (one array texture, slices) first; WARP may refuse some bind flags for NV12 arrays */
        static const UINT binds[] = { 0, D3D11_BIND_SHADER_RESOURCE, D3D11_BIND_RENDER_TARGET };
        static const int sizes[] = { 16, 0 };
        int b, z;
        ret = -1;
        for (z = 0; z < 2 && ret < 0; z++)
            for (b = 0; b < 3 && ret < 0; b++) {
                AVHWFramesContext *fc;
                av_buffer_unref(&frames);
                frames = av_hwframe_ctx_alloc(devref);
                fc = (AVHWFramesContext *)frames->data;
                fc->format = AV_PIX_FMT_D3D11; fc->sw_format = AV_PIX_FMT_NV12; fc->width = W; fc->height = H;
                fc->initial_pool_size = sizes[z];
                ((AVD3D11VAFramesContext *)fc->hwctx)->BindFlags = binds[b];
                ret = av_hwframe_ctx_init(frames);
                printf("harness: NV12 pool, %s, bind flags 0x%x: %s\n", sizes[z] ? "array of 16" : "single textures", binds[b], ret < 0 ? "refused" : "ok");
            }
    }
    if (ret < 0) { fprintf(stderr, "harness: no NV12 pool on this adapter\n"); return 1; }

    graph = avfilter_graph_alloc();
    src = avfilter_graph_alloc_filter(graph, avfilter_get_by_name("buffer"), "src");
    par = av_buffersrc_parameters_alloc();
    par->format = AV_PIX_FMT_D3D11; par->width = W; par->height = H; par->time_base = (AVRational){ 1, 24 };
    par->frame_rate = (AVRational){ 24, 1 }; par->hw_frames_ctx = frames; par->sample_aspect_ratio = (AVRational){ 1, 1 };
    av_buffersrc_parameters_set(src, par);
    av_free(par);
    if ((ret = avfilter_init_str(src, NULL)) < 0) { fprintf(stderr, "harness: buffer %d\n", ret); return 1; }
    snprintf(opts, sizeof(opts), "smoother=2:display_fps=48:extra=force_multiplier\\=2");
    flt = avfilter_graph_alloc_filter(graph, avfilter_get_by_name("pushmpc"), "pushmpc");
    if (!flt) { fprintf(stderr, "harness: this FFmpeg has no pushmpc filter\n"); return 1; }
    av_opt_set(flt->priv, "core_dir", stub_dir, 0);
    av_opt_set(flt->priv, "passthrough", "0", 0);
    if ((ret = avfilter_init_str(flt, opts)) < 0) { fprintf(stderr, "harness: pushmpc init %d\n", ret); return 1; }
    sink = avfilter_graph_alloc_filter(graph, avfilter_get_by_name("buffersink"), "sink");
    if ((ret = avfilter_init_str(sink, NULL)) < 0) return 1;
    if (avfilter_link(src, 0, flt, 0) < 0 || avfilter_link(flt, 0, sink, 0) < 0) return 1;
    if ((ret = avfilter_graph_config(graph, NULL)) < 0) { fprintf(stderr, "harness: graph config %d\n", ret); return 1; }

    for (i = 0; i <= N; i++) {
        if (i < N) {
            AVFrame *sw = av_frame_alloc(), *hw = av_frame_alloc();
            int y;
            sw->format = AV_PIX_FMT_NV12; sw->width = W; sw->height = H;
            av_frame_get_buffer(sw, 0);
            for (y = 0; y < H; y++) memset(sw->data[0] + y * sw->linesize[0], 16 + 16 * i, W);
            for (y = 0; y < H / 2; y++) memset(sw->data[1] + y * sw->linesize[1], 128, W);
            av_hwframe_get_buffer(frames, hw, 0);
            if ((ret = av_hwframe_transfer_data(hw, sw, 0)) < 0) { fprintf(stderr, "harness: upload %d\n", ret); return 1; }
            hw->pts = i;
            hw->color_range = AVCOL_RANGE_MPEG; hw->colorspace = AVCOL_SPC_BT709; hw->color_primaries = AVCOL_PRI_BT709; hw->color_trc = AVCOL_TRC_BT709;
            if ((ret = av_buffersrc_add_frame(src, hw)) < 0) { fprintf(stderr, "harness: add frame %d\n", ret); return 1; }
            av_frame_free(&sw); av_frame_free(&hw);
        } else {
            if (av_buffersrc_add_frame(src, NULL) < 0) return 1;   /* EOF */
        }
        for (;;) {
            AVFrame *out = av_frame_alloc();
            ret = av_buffersink_get_frame(sink, out);
            if (ret < 0) { av_frame_free(&out); break; }
            outputs++;
            if (last_pts >= 0 && out->pts - last_pts != 1) bad_pts++;   /* time base 1/48 */
            last_pts = out->pts;
            if (out->pts % 2 == 0) {   /* a real frame: source pts / 2 */
                AVFrame *dl = av_frame_alloc();
                const int s = (int)(out->pts / 2);
                dl->format = AV_PIX_FMT_BGRA;
                if (av_hwframe_transfer_data(dl, out, 0) >= 0) {
                    const uint8_t *px = dl->data[0] + (H / 2) * dl->linesize[0] + (W / 2) * 4;
                    const int want = (int)(16.0 * s * 255.0 / 219.0 + 0.5);
                    if (abs(px[1] - want) > 6) { bad_px++; printf("harness: real frame %d: G %d, expected ~%d\n", s, px[1], want); }
                } else bad_px++;
                reals++;
                av_frame_free(&dl);
            }
            av_frame_free(&out);
        }
    }
    printf("harness: %d outputs (%d real), %d pts gaps, %d pixel errors, zero copy %s\n", outputs, reals, bad_pts, bad_px, g_zero_copy ? "yes" : "no");
    avfilter_graph_free(&graph);
    av_buffer_unref(&frames);
    av_buffer_unref(&devref);
    if (!g_zero_copy || outputs != 1 + (N - 1) * 2 || reals != N || bad_pts || bad_px) { printf("harness: FAILED\n"); return 1; }
    printf("harness: PASSED (vf_pushmpc D3D11 zero copy on WARP)\n");
    return 0;
}
