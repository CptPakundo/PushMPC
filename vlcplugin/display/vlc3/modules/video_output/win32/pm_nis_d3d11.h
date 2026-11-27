// SPDX-License-Identifier: GPL-3.0-or-later
// PushMPC: NVIDIA Image Scaling (NIS 1.0.3, MIT) as the output upscaler of PushMPC's fork of VLC 3's Direct3D 11 video
// output, for every GPU: the decoded picture's planes (NV12 / P010, or 3 planes of 8 bits) are converted to RGB on the
// GPU (with a bilinear pre-scale when the window is more than 2x the source, NIS's own range), then NIS scales and
// sharpens into the window-size texture the output's quad draws: RGBA8 for 8-bit sources, RGB10A2 for 10-bit ones (an
// RGBA8 texture cut P010 / HDR10 / HLG pictures to 8 bits, banding along the PQ curve).
#ifndef PM_NIS_D3D11_H
#define PM_NIS_D3D11_H

#include "../../video_chroma/d3d11_fmt.h"
#include <vlc_vout_display.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pm_nis;

/* the output RGBA8 (8-bit sources) */
struct pm_nis *PM_NisCreate(vlc_object_t *, d3d11_device_t *);
/* the output in fmt: DXGI_FORMAT_R10G10B10A2_UNORM keeps 10-bit sources (P010, HDR10 / HLG) at 10 bits per channel;
 * NULL when this device cannot write it from a compute shader (PM_NisFormatOk) */
struct pm_nis *PM_NisCreateFormat(vlc_object_t *, d3d11_device_t *, DXGI_FORMAT fmt);
bool PM_NisFormatOk(d3d11_device_t *, DXGI_FORMAT fmt);
void PM_NisDestroy(struct pm_nis *);
/* output: the window-size texture in the scaler's format (UAV + SRV + render target); place: where the picture goes in
 * it (letterbox) */
int PM_NisResize(vlc_object_t *, struct pm_nis *, d3d11_device_t *, unsigned out_width, unsigned out_height,
                 ID3D11Texture2D **out_texture, ID3D11ShaderResourceView **out_srv);
/* planes: the picture's shader views (1 = packed RGB, 2 = NV12 / P010, 3 = 8-bit planar); src: visible area and colour
 * (space / range); returns VLC_SUCCESS, or VLC_EGENERIC to fall back to the plain quad */
int PM_NisProcess(vlc_object_t *, struct pm_nis *, d3d11_device_t *, ID3D11ShaderResourceView *const planes[],
                  int plane_count, const video_format_t *src, const vout_display_place_t *place, float sharpness);
#ifdef __cplusplus
}
#endif

#endif
