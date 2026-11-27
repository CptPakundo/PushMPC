// SPDX-License-Identifier: GPL-3.0-or-later
// PushMPC: pushcore's own upscaler as the output upscaler of PushMPC's fork of VLC 3's Direct3D 11 video output. VLC 3
// cannot enlarge a picture in its filter chain (the chain's output is converted back to the decoder's size), so the
// anime upscaler runs here, at the end. The output keeps its scaler (the driver's Super Resolution or NIS) and that
// scaler's window-size texture for the whole stream; when pushcore has an upscaled picture for the picture shown, it is
// drawn into that texture instead (at the texture's own bit depth: RGB10A2 for 10-bit sources).
//   Auto  (the default): the scaler for every picture; pushcore with the anime model only while ContentDetect measures
//         the picture as drawn animation (measured on the output's device without pushcore), switched as the Kodi
//         add-on does (the first switch at once, then at most one per 30 s). SDR sources only (the anime models are
//         made for SDR pictures; HDR keeps the driver's Super Resolution).
//   Anime (asked for): pushcore's upscaler for every picture (its Video Super Resolution until the picture is measured
//         as drawn, then the anime model), on the NIS scaler.
// The logic is VLC-free (vlcplugin/pushcore_bridge/pm_pcsr_core.h, tested on WARP with the stub core: pm_pcsr_test):
// one pushcore per player, shared with PushMPC's frame generation filter, which wins it whenever it generates frames;
// the output opens pushcore only while no one holds it and tries again after a capped backoff when it lost it.
#ifndef PM_PCSR_D3D11_H
#define PM_PCSR_D3D11_H

#include "../../video_chroma/d3d11_fmt.h"
#include <vlc_vout_display.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pm_pcsr;

#define PM_PCSR_AUTO  0   /* the anime model on measured drawn pictures only (the scaler otherwise) */
#define PM_PCSR_ANIME 1   /* pushcore's upscaler for every picture */

/* NULL when pushcore's upscaler cannot run for this stream (the reason is logged: no bridge or an older one, an older
 * bridge with PushMPC's frame generation filter in the chain, a software-decoded or SD / BT.601 or cropped source, a
 * screen not larger than the video, no anime model, HDR in Auto). source: the stream's format (vd->source); chroma: the
 * pictures' (opaque D3D11 only); display_width / display_height: the window now; window: the video window (its screen
 * sets pushcore's output size); mode: PM_PCSR_AUTO or PM_PCSR_ANIME (logged as a warning when it cannot run) */
struct pm_pcsr *PM_PcsrCreate(vlc_object_t *, d3d11_device_t *, const video_format_t *source, vlc_fourcc_t chroma,
                              unsigned display_width, unsigned display_height, HWND window, int mode);
void PM_PcsrDestroy(struct pm_pcsr *);
/* Without VLC's context lock held (the bridge takes it itself and waits outside it): measures the picture, hands it to
 * pushcore while pushcore runs, and waits a little (at most 50 ms) for its upscaled picture. date: the picture's (a
 * repeat of the last one is not handed over again). VLC_EGENERIC: pushcore does not keep up here (Anime mode); the
 * caller destroys this and the scaler runs from then on */
int PM_PcsrFeed(vlc_object_t *, struct pm_pcsr *, d3d11_device_t *, picture_sys_t *, mtime_t date);
/* Under VLC's context lock, after PM_PcsrFeed for the same picture: pushcore's picture into place of target (the
 * scaler's window-size texture, a render target). VLC_EGENERIC: pushcore has no picture for it (the scaler draws it) */
int PM_PcsrDraw(vlc_object_t *, struct pm_pcsr *, d3d11_device_t *, ID3D11Texture2D *target, const vout_display_place_t *place);

#ifdef __cplusplus
}
#endif

#endif
