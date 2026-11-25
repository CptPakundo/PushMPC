/*
 * libvdpau_pmnull.so.1: a null VDPAU driver for CI (no GPU). Video surfaces live in host memory (planar 4:2:0);
 * "decoding" draws the same deterministic picture as the null VA-API driver: on VdpDecoderRender the target surface
 * gets a 32x32 bright box at x = 16 + 4 * n, n counting the pictures of that decoder (clips without B-frames).
 * Only what decoders and VLC's surface export use is there (no mixer, no output surfaces, no presentation queue).
 *
 * Use (under X, e.g. Xvfb): VDPAU_DRIVER=pmnull VDPAU_DRIVER_PATH=<dir> (libvdpau loads <dir>/libvdpau_pmnull.so.1).
 * Quirks (environment): PMNULL_PUTBITS=0 (VdpVideoSurfacePutBitsYCbCr fails), PMNULL_BROKEN=1 (uploads are lost),
 * PMNULL_LOG=1.
 * Licence: MIT.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <vdpau/vdpau.h>
#include <vdpau/vdpau_x11.h>

#define MAXOBJ 1024
typedef struct { VdpChromaType type; uint32_t w, h; uint8_t *y, *u, *v; uint32_t py, pc; } surf_t;
typedef struct { VdpDecoderProfile profile; uint32_t w, h; uint64_t pictures; } dec_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static surf_t *surfs[MAXOBJ];
static dec_t *decs[MAXOBJ];
static int q_putbits = 1, q_broken, q_log;
#define LOG(...) do { if (q_log) { fprintf(stderr, "pmnull-vdpau: " __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define SURF_BASE 0x1000u
#define DEC_BASE 0x2000u

static surf_t *Surf(VdpVideoSurface s) { return s >= SURF_BASE && s < SURF_BASE + MAXOBJ ? surfs[s - SURF_BASE] : NULL; }
static dec_t *Dec(VdpDecoder d) { return d >= DEC_BASE && d < DEC_BASE + MAXOBJ ? decs[d - DEC_BASE] : NULL; }
static int EnvOn(const char *n, int def)
{
    const char *v = getenv(n);
    if (!v || !*v) return def;
    return !(v[0] == '0' || v[0] == 'n' || v[0] == 'N');
}

static const char *GetErrorString(VdpStatus s) { return s == VDP_STATUS_OK ? "OK" : "pmnull: error"; }
static VdpStatus GetApiVersion(uint32_t *v) { *v = 1; return VDP_STATUS_OK; }
static VdpStatus GetInformationString(const char **s) { *s = "PushMPC null VDPAU driver (CI)"; return VDP_STATUS_OK; }
static VdpStatus DeviceDestroy(VdpDevice d) { (void)d; return VDP_STATUS_OK; }
static VdpStatus PreemptionCallbackRegister(VdpDevice d, VdpPreemptionCallback cb, void *ctx)
{ (void)d; (void)cb; (void)ctx; return VDP_STATUS_OK; }

static VdpStatus SurfaceQueryCaps(VdpDevice d, VdpChromaType t, VdpBool *ok, uint32_t *mw, uint32_t *mh)
{
    (void)d;
    *ok = t == VDP_CHROMA_TYPE_420;
    *mw = 4096; *mh = 4096;
    return VDP_STATUS_OK;
}
static VdpStatus SurfaceQueryGetPutCaps(VdpDevice d, VdpChromaType t, VdpYCbCrFormat f, VdpBool *ok)
{
    (void)d;
    *ok = t == VDP_CHROMA_TYPE_420 && (f == VDP_YCBCR_FORMAT_YV12 || f == VDP_YCBCR_FORMAT_NV12);
    return VDP_STATUS_OK;
}
static VdpStatus SurfaceCreate(VdpDevice d, VdpChromaType t, uint32_t w, uint32_t h, VdpVideoSurface *out)
{
    (void)d;
    if (t != VDP_CHROMA_TYPE_420) return VDP_STATUS_INVALID_CHROMA_TYPE;
    if (!w || !h || w > 4096 || h > 4096) return VDP_STATUS_INVALID_SIZE;
    surf_t *s = calloc(1, sizeof(*s));
    if (!s) return VDP_STATUS_RESOURCES;
    s->type = t; s->w = w; s->h = h;
    s->py = (w + 31) & ~31u; s->pc = ((w + 1) / 2 + 15) & ~15u;
    const uint32_t hc = (h + 1) / 2;
    s->y = malloc((size_t)s->py * h); s->u = malloc((size_t)s->pc * hc); s->v = malloc((size_t)s->pc * hc);
    if (!s->y || !s->u || !s->v) { free(s->y); free(s->u); free(s->v); free(s); return VDP_STATUS_RESOURCES; }
    memset(s->y, 16, (size_t)s->py * h); memset(s->u, 128, (size_t)s->pc * hc); memset(s->v, 128, (size_t)s->pc * hc);
    pthread_mutex_lock(&lock);
    VdpStatus st = VDP_STATUS_RESOURCES;
    for (unsigned i = 0; i < MAXOBJ; i++)
        if (!surfs[i]) { surfs[i] = s; *out = SURF_BASE + i; st = VDP_STATUS_OK; break; }
    pthread_mutex_unlock(&lock);
    if (st != VDP_STATUS_OK) { free(s->y); free(s->u); free(s->v); free(s); }
    return st;
}
static VdpStatus SurfaceDestroy(VdpVideoSurface id)
{
    pthread_mutex_lock(&lock);
    surf_t *s = Surf(id);
    if (s) surfs[id - SURF_BASE] = NULL;
    pthread_mutex_unlock(&lock);
    if (!s) return VDP_STATUS_INVALID_HANDLE;
    free(s->y); free(s->u); free(s->v); free(s);
    return VDP_STATUS_OK;
}
static VdpStatus SurfaceGetParameters(VdpVideoSurface id, VdpChromaType *t, uint32_t *w, uint32_t *h)
{
    surf_t *s = Surf(id);
    if (!s) return VDP_STATUS_INVALID_HANDLE;
    *t = s->type; *w = s->w; *h = s->h;
    return VDP_STATUS_OK;
}
/* YV12 = Y, V, U planes; NV12 = Y, interleaved UV */
static VdpStatus SurfaceGetBits(VdpVideoSurface id, VdpYCbCrFormat f, void *const *data, const uint32_t *pitch)
{
    surf_t *s = Surf(id);
    if (!s) return VDP_STATUS_INVALID_HANDLE;
    const uint32_t hc = (s->h + 1) / 2, wc = (s->w + 1) / 2;
    pthread_mutex_lock(&lock);
    for (uint32_t y = 0; y < s->h; y++) memcpy((uint8_t *)data[0] + (size_t)y * pitch[0], s->y + (size_t)y * s->py, s->w);
    if (f == VDP_YCBCR_FORMAT_YV12) {
        for (uint32_t y = 0; y < hc; y++) {
            memcpy((uint8_t *)data[1] + (size_t)y * pitch[1], s->v + (size_t)y * s->pc, wc);
            memcpy((uint8_t *)data[2] + (size_t)y * pitch[2], s->u + (size_t)y * s->pc, wc);
        }
    } else if (f == VDP_YCBCR_FORMAT_NV12) {
        for (uint32_t y = 0; y < hc; y++) {
            uint8_t *d = (uint8_t *)data[1] + (size_t)y * pitch[1];
            for (uint32_t x = 0; x < wc; x++) { d[2 * x] = s->u[y * s->pc + x]; d[2 * x + 1] = s->v[y * s->pc + x]; }
        }
    } else { pthread_mutex_unlock(&lock); return VDP_STATUS_INVALID_Y_CB_CR_FORMAT; }
    pthread_mutex_unlock(&lock);
    return VDP_STATUS_OK;
}
static VdpStatus SurfacePutBits(VdpVideoSurface id, VdpYCbCrFormat f, const void *const *data, const uint32_t *pitch)
{
    if (!q_putbits) return VDP_STATUS_ERROR;
    surf_t *s = Surf(id);
    if (!s) return VDP_STATUS_INVALID_HANDLE;
    if (q_broken) return VDP_STATUS_OK;
    const uint32_t hc = (s->h + 1) / 2, wc = (s->w + 1) / 2;
    pthread_mutex_lock(&lock);
    for (uint32_t y = 0; y < s->h; y++) memcpy(s->y + (size_t)y * s->py, (const uint8_t *)data[0] + (size_t)y * pitch[0], s->w);
    if (f == VDP_YCBCR_FORMAT_YV12) {
        for (uint32_t y = 0; y < hc; y++) {
            memcpy(s->v + (size_t)y * s->pc, (const uint8_t *)data[1] + (size_t)y * pitch[1], wc);
            memcpy(s->u + (size_t)y * s->pc, (const uint8_t *)data[2] + (size_t)y * pitch[2], wc);
        }
    } else if (f == VDP_YCBCR_FORMAT_NV12) {
        for (uint32_t y = 0; y < hc; y++) {
            const uint8_t *d = (const uint8_t *)data[1] + (size_t)y * pitch[1];
            for (uint32_t x = 0; x < wc; x++) { s->u[y * s->pc + x] = d[2 * x]; s->v[y * s->pc + x] = d[2 * x + 1]; }
        }
    } else { pthread_mutex_unlock(&lock); return VDP_STATUS_INVALID_Y_CB_CR_FORMAT; }
    pthread_mutex_unlock(&lock);
    return VDP_STATUS_OK;
}

static int ProfileOk(VdpDecoderProfile p)
{
    return p == VDP_DECODER_PROFILE_H264_BASELINE || p == VDP_DECODER_PROFILE_H264_MAIN || p == VDP_DECODER_PROFILE_H264_HIGH
#ifdef VDP_DECODER_PROFILE_H264_CONSTRAINED_BASELINE
        || p == VDP_DECODER_PROFILE_H264_CONSTRAINED_BASELINE
#endif
        ;
}
static VdpStatus DecoderQueryCaps(VdpDevice d, VdpDecoderProfile p, VdpBool *ok, uint32_t *ml, uint32_t *mmb, uint32_t *mw, uint32_t *mh)
{
    (void)d;
    *ok = ProfileOk(p);
    *ml = 51; *mmb = 65536; *mw = 4096; *mh = 4096;
    return VDP_STATUS_OK;
}
static VdpStatus DecoderCreate(VdpDevice d, VdpDecoderProfile p, uint32_t w, uint32_t h, uint32_t refs, VdpDecoder *out)
{
    (void)d; (void)refs;
    if (!ProfileOk(p)) return VDP_STATUS_INVALID_DECODER_PROFILE;
    dec_t *dc = calloc(1, sizeof(*dc));
    if (!dc) return VDP_STATUS_RESOURCES;
    dc->profile = p; dc->w = w; dc->h = h;
    pthread_mutex_lock(&lock);
    VdpStatus st = VDP_STATUS_RESOURCES;
    for (unsigned i = 0; i < MAXOBJ; i++)
        if (!decs[i]) { decs[i] = dc; *out = DEC_BASE + i; st = VDP_STATUS_OK; break; }
    pthread_mutex_unlock(&lock);
    if (st != VDP_STATUS_OK) free(dc);
    LOG("DecoderCreate %ux%u profile %u", w, h, p);
    return st;
}
static VdpStatus DecoderDestroy(VdpDecoder id)
{
    pthread_mutex_lock(&lock);
    dec_t *dc = Dec(id);
    if (dc) decs[id - DEC_BASE] = NULL;
    pthread_mutex_unlock(&lock);
    if (!dc) return VDP_STATUS_INVALID_HANDLE;
    LOG("DecoderDestroy after %llu pictures", (unsigned long long)dc->pictures);
    free(dc);
    return VDP_STATUS_OK;
}
static VdpStatus DecoderGetParameters(VdpDecoder id, VdpDecoderProfile *p, uint32_t *w, uint32_t *h)
{
    dec_t *dc = Dec(id);
    if (!dc) return VDP_STATUS_INVALID_HANDLE;
    *p = dc->profile; *w = dc->w; *h = dc->h;
    return VDP_STATUS_OK;
}
static VdpStatus DecoderRender(VdpDecoder id, VdpVideoSurface target, const VdpPictureInfo *info, uint32_t n, const VdpBitstreamBuffer *bufs)
{
    (void)info; (void)n; (void)bufs;
    pthread_mutex_lock(&lock);
    dec_t *dc = Dec(id);
    surf_t *s = Surf(target);
    if (dc && s) {
        const uint32_t box = 32, cnt = (uint32_t)dc->pictures++;
        uint32_t x0 = 16 + 4 * cnt;
        if (x0 + box > s->w) x0 %= (s->w > box ? s->w - box : 1);
        const uint32_t y0 = s->h / 2 - box / 2;
        for (uint32_t y = 0; y < s->h; y++)
            for (uint32_t x = 0; x < s->w; x++)
                s->y[(size_t)y * s->py + x] = (y >= y0 && y < y0 + box && x >= x0 && x < x0 + box) ? 235 : 16;
        memset(s->u, 128, (size_t)s->pc * ((s->h + 1) / 2)); memset(s->v, 128, (size_t)s->pc * ((s->h + 1) / 2));
    }
    pthread_mutex_unlock(&lock);
    if (!dc) return VDP_STATUS_INVALID_HANDLE;
    return s ? VDP_STATUS_OK : VDP_STATUS_INVALID_HANDLE;
}

static VdpStatus GetProcAddress(VdpDevice d, VdpFuncId id, void **fn);
static VdpStatus GetProcAddress(VdpDevice d, VdpFuncId id, void **fn)
{
    (void)d;
    switch (id) {
    case VDP_FUNC_ID_GET_ERROR_STRING: *fn = (void *)GetErrorString; break;
    case VDP_FUNC_ID_GET_PROC_ADDRESS: *fn = (void *)GetProcAddress; break;
    case VDP_FUNC_ID_GET_API_VERSION: *fn = (void *)GetApiVersion; break;
    case VDP_FUNC_ID_GET_INFORMATION_STRING: *fn = (void *)GetInformationString; break;
    case VDP_FUNC_ID_DEVICE_DESTROY: *fn = (void *)DeviceDestroy; break;
    case VDP_FUNC_ID_PREEMPTION_CALLBACK_REGISTER: *fn = (void *)PreemptionCallbackRegister; break;
    case VDP_FUNC_ID_VIDEO_SURFACE_QUERY_CAPABILITIES: *fn = (void *)SurfaceQueryCaps; break;
    case VDP_FUNC_ID_VIDEO_SURFACE_QUERY_GET_PUT_BITS_Y_CB_CR_CAPABILITIES: *fn = (void *)SurfaceQueryGetPutCaps; break;
    case VDP_FUNC_ID_VIDEO_SURFACE_CREATE: *fn = (void *)SurfaceCreate; break;
    case VDP_FUNC_ID_VIDEO_SURFACE_DESTROY: *fn = (void *)SurfaceDestroy; break;
    case VDP_FUNC_ID_VIDEO_SURFACE_GET_PARAMETERS: *fn = (void *)SurfaceGetParameters; break;
    case VDP_FUNC_ID_VIDEO_SURFACE_GET_BITS_Y_CB_CR: *fn = (void *)SurfaceGetBits; break;
    case VDP_FUNC_ID_VIDEO_SURFACE_PUT_BITS_Y_CB_CR: *fn = (void *)SurfacePutBits; break;
    case VDP_FUNC_ID_DECODER_QUERY_CAPABILITIES: *fn = (void *)DecoderQueryCaps; break;
    case VDP_FUNC_ID_DECODER_CREATE: *fn = (void *)DecoderCreate; break;
    case VDP_FUNC_ID_DECODER_DESTROY: *fn = (void *)DecoderDestroy; break;
    case VDP_FUNC_ID_DECODER_GET_PARAMETERS: *fn = (void *)DecoderGetParameters; break;
    case VDP_FUNC_ID_DECODER_RENDER: *fn = (void *)DecoderRender; break;
    default: *fn = NULL; return VDP_STATUS_INVALID_FUNC_ID;
    }
    return VDP_STATUS_OK;
}

__attribute__((visibility("default"))) VdpStatus vdp_imp_device_create_x11(Display *dpy, int screen, VdpDevice *dev, VdpGetProcAddress **gpa);
VdpStatus vdp_imp_device_create_x11(Display *dpy, int screen, VdpDevice *dev, VdpGetProcAddress **gpa)
{
    (void)dpy; (void)screen;
    q_putbits = EnvOn("PMNULL_PUTBITS", 1);
    q_broken = EnvOn("PMNULL_BROKEN", 0);
    q_log = EnvOn("PMNULL_LOG", 0);
    *dev = 1;
    *gpa = GetProcAddress;
    LOG("device created (put bits %d, broken %d)", q_putbits, q_broken);
    return VDP_STATUS_OK;
}
