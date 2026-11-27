// SPDX-License-Identifier: GPL-3.0-or-later
// PushMPC: pushcore's own upscaler as the output upscaler of PushMPC's fork of VLC 3's Direct3D 11 video output. VLC 3
// cannot enlarge a picture in its filter chain (the chain's output is converted back to the decoder's size), so the
// anime upscaler runs here, at the end: the decoder's texture slices go to pushcore through its bridge
// (pm_pushcore.dll, loaded at run time from the plugin's folder) on the output's own D3D11 device, pushcore upscales
// to the screen (Video Super Resolution; the anime upscaler while ContentDetect measures the picture as drawn, switched
// as the Kodi add-on does: win_anime_route.h), and its NV12 / P010 picture is converted into the NIS scaler's
// window-size texture (pm_nis_d3d11.h) that the output's quad draws. Any picture pushcore does not deliver in time is
// NIS's; when pushcore fails, NIS takes over for the rest of the stream.
#ifndef PM_PCSR_D3D11_H
#define PM_PCSR_D3D11_H

#include "../../video_chroma/d3d11_fmt.h"
#include <vlc_vout_display.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pm_pcsr;
struct pm_nis;

/* NULL when pushcore's upscaler does not run for this stream (the reason is logged: PushMPC's frame generation filter
 * may hold pushcore, no bridge or an older one, a software-decoded or SD / BT.601 or cropped source, a screen not larger
 * than the video, no anime model). source: the stream's format (vd->source); chroma: the pictures' (opaque D3D11 only);
 * display_width / display_height: the window now; window: the video window (its screen sets pushcore's output size);
 * forced: the user asked for it (logged as a warning when it cannot run) */
struct pm_pcsr *PM_PcsrCreate(vlc_object_t *, d3d11_device_t *, const video_format_t *source, vlc_fourcc_t chroma,
                              unsigned display_width, unsigned display_height, HWND window, bool forced);
void PM_PcsrDestroy(struct pm_pcsr *);
/* Without VLC's context lock held (the bridge takes it itself and waits outside it): hands the picture to pushcore,
 * waits a little for its upscaled picture, and follows the measured class (the anime model on drawn pictures, at most
 * one switch per 30 s). date: the picture's (a repeat of the last one is not handed over again). VLC_EGENERIC: pushcore
 * failed for good (logged); the caller destroys this and NIS runs from then on */
int PM_PcsrFeed(vlc_object_t *, struct pm_pcsr *, d3d11_device_t *, picture_sys_t *, mtime_t date);
/* Under VLC's context lock, after PM_PcsrFeed for the same picture: pushcore's picture into the NIS scaler's output at
 * place. VLC_EGENERIC: pushcore has no picture for it (NIS draws this one) */
int PM_PcsrDraw(vlc_object_t *, struct pm_pcsr *, struct pm_nis *, d3d11_device_t *, const vout_display_place_t *place);

#ifdef __cplusplus
}
#endif

#endif
