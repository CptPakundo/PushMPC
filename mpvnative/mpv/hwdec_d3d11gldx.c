/*
 * hwdec_d3d11gldx.c (PushMPC addition to mpv, LGPL 2.1+ like mpv): D3D11 frames in a desktop OpenGL renderer through
 * WGL_NV_DX_interop2. Players that draw mpv with the OpenGL render API on Windows (MPC-QT, Jellyfin Desktop) have no
 * interop for D3D11 frames in stock mpv (only ANGLE's, or DXVA2 through dxinterop), so decoding falls back to copies
 * and PushMPC's filter (vf_pushmpc, which works on D3D11 textures) cannot get its zero-copy input. This interop owns a
 * D3D11 device on the GL context's GPU and offers it for d3d11va decoding and hwupload; each frame is copied on the GPU
 * into a texture shared with GL: RGB frames (vf_pushmpc's BGRA / X2BGR10 / RGBAF16 output) with one copy, YUV frames
 * (NV12 / P010 straight from the decoder) through the D3D11 video processor. Nothing goes through the CPU.
 * Built only for Windows with gl-dxinterop and d3d11 (mpvnative/ci/build-unix.sh adds it to mpv 0.42).
 */
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_4.h>

#include <libavutil/hwcontext.h>

#include "common/common.h"
#include "osdep/windows_utils.h"
#include "video/out/gpu/hwdec.h"
#include "video/out/opengl/ra_gl.h"
#include "video/hwdec.h"
#include "video/d3d.h"
#include "video/fmt-conversion.h"
#include "video/img_format.h"

#include <GL/wglext.h>

typedef HANDLE (WINAPI *open_fn)(void *);
typedef BOOL (WINAPI *close_fn)(HANDLE);
typedef HANDLE (WINAPI *reg_fn)(HANDLE, void *, GLuint, GLenum, GLenum);
typedef BOOL (WINAPI *unreg_fn)(HANDLE, HANDLE);
typedef BOOL (WINAPI *lock_fn)(HANDLE, GLint, HANDLE *);

struct priv_owner {
    struct mp_hwdec_ctx hwctx;
    ID3D11Device *device;
    ID3D11DeviceContext *ctx;
    HANDLE device_h;
    open_fn Open; close_fn Close; reg_fn Register; unreg_fn Unregister; lock_fn Lock, Unlock;
    int supported_fmts[8];
};

struct priv {
    ID3D11Texture2D *shared;      // the texture GL reads (BGRA8, RGB10A2 or RGBA16F)
    HANDLE shared_h;
    GLuint gltex;
    bool locked;
    DXGI_FORMAT fmt;
    // YUV input: the video processor into `shared`
    ID3D11VideoDevice *vdev;
    ID3D11VideoContext *vctx;
    ID3D11VideoProcessorEnumerator *venum;
    ID3D11VideoProcessor *vproc;
    ID3D11VideoProcessorOutputView *vout;
};

static int vendor_of(const char *gl_vendor)
{
    if (!gl_vendor) return 0;
    if (strstr(gl_vendor, "NVIDIA")) return 0x10DE;
    if (strstr(gl_vendor, "ATI") || strstr(gl_vendor, "AMD")) return 0x1002;
    if (strstr(gl_vendor, "Intel")) return 0x8086;
    return 0;
}

static void uninit(struct ra_hwdec *hw)
{
    struct priv_owner *p = hw->priv;
    hwdec_devices_remove(hw->devs, &p->hwctx);
    av_buffer_unref(&p->hwctx.av_device_ref);
    if (p->device_h && p->Close) p->Close(p->device_h);
    p->device_h = NULL;
    SAFE_RELEASE(p->ctx);
    SAFE_RELEASE(p->device);
}

static int init(struct ra_hwdec *hw)
{
    struct priv_owner *p = hw->priv;
    struct ra *ra = hw->ra_ctx->ra;
    HRESULT hr;

    if (!ra_is_gl(ra))
        return -1;
    GL *gl = ra_gl_get(ra);
    if (gl->es || !gl->get_fn)
        return -1;
    // the WGL functions straight from the host's loader: a libmpv host's GL_EXTENSIONS does not list WGL extensions
    p->Open = (open_fn)gl->get_fn(gl->fn_ctx, "wglDXOpenDeviceNV");
    p->Close = (close_fn)gl->get_fn(gl->fn_ctx, "wglDXCloseDeviceNV");
    p->Register = (reg_fn)gl->get_fn(gl->fn_ctx, "wglDXRegisterObjectNV");
    p->Unregister = (unreg_fn)gl->get_fn(gl->fn_ctx, "wglDXUnregisterObjectNV");
    p->Lock = (lock_fn)gl->get_fn(gl->fn_ctx, "wglDXLockObjectsNV");
    p->Unlock = (lock_fn)gl->get_fn(gl->fn_ctx, "wglDXUnlockObjectsNV");
    if (!p->Open || !p->Close || !p->Register || !p->Unregister || !p->Lock || !p->Unlock) {
        MP_VERBOSE(hw, "no WGL_NV_DX_interop2 in this OpenGL context\n");
        return -1;
    }

    // the DXGI adapter of the GL context's GPU (the interop works on one GPU only)
    const int want = vendor_of((const char *)gl->GetString(GL_VENDOR));
    IDXGIFactory1 *factory = NULL;
    IDXGIAdapter1 *adapter = NULL, *a = NULL;
    typedef HRESULT (WINAPI *create_factory_fn)(REFIID, void **);
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    create_factory_fn create_factory = dxgi ? (create_factory_fn)(void *)GetProcAddress(dxgi, "CreateDXGIFactory1") : NULL;
    if (create_factory && SUCCEEDED(create_factory(&IID_IDXGIFactory1, (void **)&factory))) {
        for (UINT i = 0; IDXGIFactory1_EnumAdapters1(factory, i, &a) == S_OK; i++) {
            DXGI_ADAPTER_DESC1 d;
            if (SUCCEEDED(IDXGIAdapter1_GetDesc1(a, &d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                (!want || (int)d.VendorId == want)) { adapter = a; break; }
            IDXGIAdapter1_Release(a);
        }
        IDXGIFactory1_Release(factory);
    }
    d3d_load_dlls();   // mpv's own d3d11.dll handle (video/d3d.h)
    PFN_D3D11_CREATE_DEVICE create = d3d11_D3D11CreateDevice;
    if (!create) { SAFE_RELEASE(adapter); return -1; }
    static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
    hr = create((IDXGIAdapter *)adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, NULL,
                D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, MP_ARRAY_SIZE(levels),
                D3D11_SDK_VERSION, &p->device, NULL, &p->ctx);
    SAFE_RELEASE(adapter);
    if (FAILED(hr)) {
        MP_VERBOSE(hw, "D3D11CreateDevice failed: %s\n", mp_HRESULT_to_str(hr));
        return -1;
    }
    ID3D10Multithread *mt = NULL;
    if (SUCCEEDED(ID3D11Device_QueryInterface(p->device, &IID_ID3D10Multithread, (void **)&mt))) {
        ID3D10Multithread_SetMultithreadProtected(mt, TRUE);
        ID3D10Multithread_Release(mt);
    }
    p->device_h = p->Open(p->device);
    if (!p->device_h) {
        MP_VERBOSE(hw, "wglDXOpenDeviceNV failed: %s\n", mp_LastError_to_str());
        return -1;
    }

    static const enum AVPixelFormat fmts[] = { AV_PIX_FMT_NV12, AV_PIX_FMT_P010, AV_PIX_FMT_BGRA, AV_PIX_FMT_X2BGR10, AV_PIX_FMT_RGBAF16 };
    int n = 0;
    for (int i = 0; i < MP_ARRAY_SIZE(fmts); i++) {
        int f = pixfmt2imgfmt(fmts[i]);
        if (f) p->supported_fmts[n++] = f;
    }
    p->supported_fmts[n] = 0;
    p->hwctx = (struct mp_hwdec_ctx){
        .driver_name = hw->driver->name,
        .av_device_ref = d3d11_wrap_device_ref(p->device),
        .supported_formats = p->supported_fmts,
        .hw_imgfmt = IMGFMT_D3D11,
    };
    if (!p->hwctx.av_device_ref) {
        MP_VERBOSE(hw, "Failed to create hwdevice_ctx\n");
        return -1;
    }
    hwdec_devices_add(hw->devs, &p->hwctx);
    MP_VERBOSE(hw, "D3D11 frames into OpenGL through WGL_NV_DX_interop2\n");
    return 0;
}

static void mapper_uninit(struct ra_hwdec_mapper *mapper)
{
    struct priv_owner *o = mapper->owner->priv;
    struct priv *p = mapper->priv;
    GL *gl = ra_gl_get(mapper->ra);
    if (p->shared_h) {
        if (p->locked) o->Unlock(o->device_h, 1, &p->shared_h);
        o->Unregister(o->device_h, p->shared_h);
    }
    p->shared_h = NULL; p->locked = false;
    if (p->gltex) gl->DeleteTextures(1, &p->gltex);
    p->gltex = 0;
    ra_tex_free(mapper->ra, &mapper->tex[0]);
    SAFE_RELEASE(p->vout);
    SAFE_RELEASE(p->vproc);
    SAFE_RELEASE(p->venum);
    SAFE_RELEASE(p->vctx);
    SAFE_RELEASE(p->vdev);
    SAFE_RELEASE(p->shared);
}

static int mapper_init(struct ra_hwdec_mapper *mapper)
{
    struct priv_owner *o = mapper->owner->priv;
    struct priv *p = mapper->priv;
    GL *gl = ra_gl_get(mapper->ra);
    HRESULT hr;
    const int sub = mapper->src_params.hw_subfmt;
    const char *rafmt;
    int dst;
    bool yuv = false;

    if (sub == pixfmt2imgfmt(AV_PIX_FMT_BGRA))         { p->fmt = DXGI_FORMAT_B8G8R8A8_UNORM;     rafmt = "rgba8";    dst = IMGFMT_RGB0; }
    else if (sub == pixfmt2imgfmt(AV_PIX_FMT_X2BGR10)) { p->fmt = DXGI_FORMAT_R10G10B10A2_UNORM;  rafmt = "rgb10_a2"; dst = sub; }
    else if (sub == pixfmt2imgfmt(AV_PIX_FMT_RGBAF16)) { p->fmt = DXGI_FORMAT_R16G16B16A16_FLOAT; rafmt = "rgba16f";  dst = sub; }   // GL's name (the D3D11 ra calls it rgba16hf)
    else if (sub == IMGFMT_NV12)                        { p->fmt = DXGI_FORMAT_B8G8R8A8_UNORM;     rafmt = "rgba8";    dst = IMGFMT_RGB0; yuv = true; }
    else if (sub == IMGFMT_P010)                        { p->fmt = DXGI_FORMAT_R10G10B10A2_UNORM;  rafmt = "rgb10_a2"; dst = pixfmt2imgfmt(AV_PIX_FMT_X2BGR10); yuv = true; }
    else {
        MP_ERR(mapper, "Unsupported D3D11 surface format: %s\n", mp_imgfmt_to_name(sub));
        return -1;
    }
    const struct ra_format *rf = ra_find_named_format(mapper->ra, rafmt);
    if (!rf) {
        MP_ERR(mapper, "No OpenGL format %s\n", rafmt);
        return -1;
    }

    const int w = mapper->src_params.w, h = mapper->src_params.h;
    D3D11_TEXTURE2D_DESC td = {
        .Width = w, .Height = h, .MipLevels = 1, .ArraySize = 1, .Format = p->fmt, .SampleDesc.Count = 1,
        .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
    };
    hr = ID3D11Device_CreateTexture2D(o->device, &td, NULL, &p->shared);
    if (FAILED(hr)) {
        MP_ERR(mapper, "Failed creating the shared texture: %s\n", mp_HRESULT_to_str(hr));
        return -1;
    }

    if (yuv) {
        if (FAILED(ID3D11Device_QueryInterface(o->device, &IID_ID3D11VideoDevice, (void **)&p->vdev)) ||
            FAILED(ID3D11DeviceContext_QueryInterface(o->ctx, &IID_ID3D11VideoContext, (void **)&p->vctx))) {
            MP_ERR(mapper, "No D3D11 video processor\n");
            return -1;
        }
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {
            .InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE,
            .InputWidth = w, .InputHeight = h, .OutputWidth = w, .OutputHeight = h,
            .Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL,
        };
        if (FAILED(ID3D11VideoDevice_CreateVideoProcessorEnumerator(p->vdev, &cd, &p->venum)) ||
            FAILED(ID3D11VideoDevice_CreateVideoProcessor(p->vdev, p->venum, 0, &p->vproc))) {
            MP_ERR(mapper, "Failed creating the D3D11 video processor\n");
            return -1;
        }
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = { .ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D };
        if (FAILED(ID3D11VideoDevice_CreateVideoProcessorOutputView(p->vdev, (ID3D11Resource *)p->shared, p->venum, &ovd, &p->vout))) {
            MP_ERR(mapper, "Failed creating the video processor output view\n");
            return -1;
        }
        // the stream's own colour space (BT.709 / BT.601, limited / full range)
        const struct mp_image_params *sp = &mapper->src_params;
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in = {
            .YCbCr_Matrix = sp->repr.sys == PL_COLOR_SYSTEM_BT_601 ? 0 : 1,
            .Nominal_Range = sp->repr.levels == PL_COLOR_LEVELS_FULL ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255 : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235,
        };
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE out = { .Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255 };
        ID3D11VideoContext_VideoProcessorSetStreamColorSpace(p->vctx, p->vproc, 0, &in);
        ID3D11VideoContext_VideoProcessorSetOutputColorSpace(p->vctx, p->vproc, &out);
        // BT.2020 and PQ / HLG need the DXGI colour spaces (ID3D11VideoContext1): the output keeps the stream's primaries
        // and transfer (dst_params carries them), only the matrix and the range are undone here
        ID3D11VideoContext1 *vctx1 = NULL;
        if (SUCCEEDED(ID3D11VideoContext_QueryInterface(p->vctx, &IID_ID3D11VideoContext1, (void **)&vctx1))) {
            const bool full = sp->repr.levels == PL_COLOR_LEVELS_FULL;
            const bool bt2020 = sp->repr.sys == PL_COLOR_SYSTEM_BT_2020_NC || sp->color.primaries == PL_COLOR_PRIM_BT_2020;
            // same transfer on both sides = no transfer conversion (HLG stays HLG-coded in "G22", as mpv expects)
            const bool pq = sp->color.transfer == PL_COLOR_TRC_PQ;
            DXGI_COLOR_SPACE_TYPE ics, ocs;
            if (pq)        { ics = DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020; ocs = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020; }
            else if (bt2020) { ics = full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020;
                             ocs = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020; }
            else if (sp->repr.sys == PL_COLOR_SYSTEM_BT_601) { ics = full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
                             ocs = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709; }
            else           { ics = full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
                             ocs = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709; }
            ID3D11VideoContext1_VideoProcessorSetStreamColorSpace1(vctx1, p->vproc, 0, ics);
            ID3D11VideoContext1_VideoProcessorSetOutputColorSpace1(vctx1, p->vproc, ocs);
            ID3D11VideoContext1_Release(vctx1);
        }
        ID3D11VideoContext_VideoProcessorSetStreamAutoProcessingMode(p->vctx, p->vproc, 0, FALSE);
    }

    gl->GenTextures(1, &p->gltex);
    gl->BindTexture(GL_TEXTURE_2D, p->gltex);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->BindTexture(GL_TEXTURE_2D, 0);
    p->shared_h = o->Register(o->device_h, p->shared, p->gltex, GL_TEXTURE_2D, WGL_ACCESS_READ_ONLY_NV);
    if (!p->shared_h) {
        MP_ERR(mapper, "Failed to register the D3D11 texture with OpenGL: %s\n", mp_LastError_to_str());
        return -1;
    }
    struct ra_tex_params params = {
        .dimensions = 2, .w = w, .h = h, .d = 1, .format = rf, .render_src = true, .src_linear = true,
    };
    mapper->tex[0] = ra_create_wrapped_tex(mapper->ra, &params, p->gltex);
    if (!mapper->tex[0])
        return -1;

    mapper->dst_params = mapper->src_params;
    mapper->dst_params.imgfmt = dst;
    mapper->dst_params.hw_subfmt = 0;
    if (yuv) {
        // the video processor wrote full-range RGB
        mapper->dst_params.repr.sys = PL_COLOR_SYSTEM_RGB;
        mapper->dst_params.repr.levels = PL_COLOR_LEVELS_FULL;
    }
    return 0;
}

static int mapper_map(struct ra_hwdec_mapper *mapper)
{
    struct priv_owner *o = mapper->owner->priv;
    struct priv *p = mapper->priv;
    ID3D11Texture2D *tex = (void *)mapper->src->planes[0];
    const UINT slice = (UINT)(intptr_t)mapper->src->planes[1];

    if (p->locked) {
        if (!o->Unlock(o->device_h, 1, &p->shared_h)) {
            MP_ERR(mapper, "Failed unlocking the shared texture: %s\n", mp_LastError_to_str());
            return -1;
        }
        p->locked = false;
    }
    if (p->vproc) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {
            .ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D, .Texture2D = { .ArraySlice = slice },
        };
        ID3D11VideoProcessorInputView *iv = NULL;
        HRESULT hr = ID3D11VideoDevice_CreateVideoProcessorInputView(p->vdev, (ID3D11Resource *)tex, p->venum, &ivd, &iv);
        if (FAILED(hr)) {
            MP_ERR(mapper, "Failed creating the video processor input view: %s\n", mp_HRESULT_to_str(hr));
            return -1;
        }
        D3D11_VIDEO_PROCESSOR_STREAM st = { .Enable = TRUE, .pInputSurface = iv };
        hr = ID3D11VideoContext_VideoProcessorBlt(p->vctx, p->vproc, p->vout, 0, 1, &st);
        ID3D11VideoProcessorInputView_Release(iv);
        if (FAILED(hr)) {
            MP_ERR(mapper, "VideoProcessorBlt failed: %s\n", mp_HRESULT_to_str(hr));
            return -1;
        }
    } else {
        D3D11_BOX box = { 0, 0, 0, mapper->src_params.w, mapper->src_params.h, 1 };
        ID3D11DeviceContext_CopySubresourceRegion(o->ctx, (ID3D11Resource *)p->shared, 0, 0, 0, 0,
                                                  (ID3D11Resource *)tex, slice, &box);
    }
    if (!o->Lock(o->device_h, 1, &p->shared_h)) {
        MP_ERR(mapper, "Failed locking the shared texture: %s\n", mp_LastError_to_str());
        return -1;
    }
    p->locked = true;
    return 0;
}

const struct ra_hwdec_driver ra_hwdec_d3d11gldx = {
    .name = "d3d11-dxinterop",
    .priv_size = sizeof(struct priv_owner),
    .imgfmts = {IMGFMT_D3D11, 0},
    .device_type = AV_HWDEVICE_TYPE_D3D11VA,
    .init = init,
    .uninit = uninit,
    .mapper = &(const struct ra_hwdec_mapper_driver){
        .priv_size = sizeof(struct priv),
        .init = mapper_init,
        .uninit = mapper_uninit,
        .map = mapper_map,
    },
};
