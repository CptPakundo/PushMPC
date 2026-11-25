/*
 * pm_hw: PushMPC's hardware-format path for VLC 3 on Linux (VA-API 'VAOP'/'VAO0' and VDPAU 'VDV0' pictures).
 *
 * The filter keeps the decoder's hardware format on both sides (fmt_out == fmt_in), so VLC never has to convert back
 * for the display after the static chain (which it cannot do for a frame-rate-changing filter). Real frames pass
 * through untouched; for the engine each source surface is downloaded once (derive, or get-image where derive fails),
 * and every generated frame is uploaded into a surface of PushMPC's own pool on the decoder's device (so there is no
 * cap from VLC's 4-picture pool). Output pictures carry a picture context with VLC 3.0.x's layout for that format
 * (hw/vaapi/vlc_vaapi.c's vaapi_pic_ctx, hw/vdpau/vlc_vdpau.h's vlc_vdp_video_field_t), so VLC's displays, converters
 * and hardware filters read them as their own.
 *
 * Safety: the device is taken from the first picture only after checks (the context's callbacks live in one of VLC's
 * VA-API / VDPAU modules, libva's display magic, VDPAU's own surface query), and before any frame is generated a
 * round trip (two pool surfaces, patterns up and back down, compared) proves the upload and download paths on this
 * driver. Any failure: the filter passes the frames through at x1; it never refuses the format (that would make VLC
 * drop every filter). libva and VLC's libvlc_vdpau are looked up at run time (no new dependency).
 * Licence: GPL-3.0-or-later.
 */
#ifndef PM_HW_H
#define PM_HW_H
#include <stdbool.h>
#include <vlc_common.h>
#include <vlc_picture.h>

typedef struct pm_hw pm_hw;

/* a hardware chroma this path handles; bits = the work format's depth (8 or 10) */
bool pm_hw_chroma(vlc_fourcc_t c, int *bits);
/* fmt: the filter's input format. NULL when this build has no hardware path (Windows, macOS) */
pm_hw *pm_hw_new(vlc_object_t *o, const video_format_t *fmt, int pool_size);
/* the first picture: take its device, make the pool, run the round trip. 0 = ready; else why says what failed */
int pm_hw_bind(pm_hw *h, picture_t *first, char *why, size_t whysize);
/* the work format (I420 / I420_10L, at least the surface size) and a picture in it, from a small recycled pool */
const video_format_t *pm_hw_work_format(const pm_hw *h);
picture_t *pm_hw_cpu_picture(pm_hw *h);
/* source surface -> CPU picture (the work format). 0 = ok */
int pm_hw_download(pm_hw *h, picture_t *src, picture_t *dst);
/* a new output picture (the hardware format) backed by a free pool surface; NULL when none is free */
picture_t *pm_hw_new_picture(pm_hw *h);
/* CPU picture -> the output picture's surface. 0 = ok */
int pm_hw_upload(pm_hw *h, const picture_t *src, picture_t *dst);
/* keep the decoder's device alive while output pictures exist: a reference on the newest source picture's context */
void pm_hw_anchor(pm_hw *h, picture_t *src);
const char *pm_hw_describe(const pm_hw *h);
/* the number of pool surfaces (before pm_hw_bind; 4..48) */
void pm_hw_set_pool_size(pm_hw *h, int n);
/* the filter is done; the pool goes away with the last output picture */
void pm_hw_release(pm_hw *h);

#endif
