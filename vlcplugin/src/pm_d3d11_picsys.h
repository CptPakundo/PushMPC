/* pm_d3d11_picsys.h: what the module reads of VLC 3.0.x's Direct3D 11 pictures ('DX11' NV12 / 'DX10' P010) for the
 * zero-copy path: the layout of picture_sys_t (modules/video_chroma/d3d11_fmt.h) and va_pic_context
 * (modules/codec/avcodec/va_surface.h), unchanged since 3.0.0. The Windows build checks it against the 3.0.24 headers
 * (tests/pm_d3d11_layout_check.c). Windows only. Licence: GPL-3.0-or-later. */
#ifndef PM_D3D11_PICSYS_H
#define PM_D3D11_PICSYS_H
#ifndef COBJMACROS
# define COBJMACROS
#endif
#include <initguid.h>
#include <d3d11.h>
#include <vlc_common.h>
#include <vlc_picture.h>

typedef struct pm_d3d11_picsys {
    void *decoder;
    ID3D11Texture2D *texture[4];       /* D3D11_MAX_SHADER_VIEW */
    ID3D11DeviceContext *context;
    unsigned slice_index;
    void *processorInput, *processorOutput;
    void *resourceView[4];
    DXGI_FORMAT formatTexture;
} pm_d3d11_picsys;

typedef struct pm_va_pic_context { picture_context_t s; void *va_surface; pm_d3d11_picsys picsys; } pm_va_pic_context;

/* d3d11_fmt.c ActivePictureSys: decoder pictures carry theirs in the context, pool pictures in p_sys */
static inline pm_d3d11_picsys *PicSys(picture_t *p)
{
    return p->context ? &((pm_va_pic_context *)p->context)->picsys : (pm_d3d11_picsys *)p->p_sys;
}

/* VLC's lock on the device's immediate context (d3d11_fmt.h GUID_CONTEXT_MUTEX) */
DEFINE_GUID(PM_GUID_CONTEXT_MUTEX, 0x472e8835, 0x3f8e, 0x4f93, 0xa0, 0xcb, 0x25, 0x79, 0x77, 0x6c, 0xed, 0x86);
#endif
