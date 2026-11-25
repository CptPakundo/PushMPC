// SPDX-License-Identifier: GPL-3.0-or-later
// PushMPC: NVIDIA Image Scaling (NIS 1.0.3, MIT) as the output upscaler of PushMPC's fork of VLC 3's Direct3D 11 video
// output, for every GPU: the decoded picture's planes (NV12 / P010, or 3 planes of 8 bits) are converted to RGB on the
// GPU (with a bilinear pre-scale when the window is more than 2x the source, NIS's own range), then NIS scales and
// sharpens into the window-size texture the output's quad draws.
#ifndef PM_NIS_D3D11_H
#define PM_NIS_D3D11_H

#include "../../video_chroma/d3d11_fmt.h"
#include <vlc_vout_display.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pm_nis;

struct pm_nis *PM_NisCreate(vlc_object_t *, d3d11_device_t *);
void PM_NisDestroy(struct pm_nis *);
/* output: the window-size RGBA8 texture (UAV + SRV); place: where the picture goes in it (letterbox) */
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
