/*
 * pm_cvpx: VideoToolbox pictures (VLC's CVPX_NV12 / CVPX_P010) straight into the apple engine and generated frames
 * back as CVPX pictures, with no copy (macOS). Guarded so it can never break playback:
 *   - only on VLC versions whose private CVPX picture layout was checked against this file (pm_cvpx_vlc_ok);
 *   - every incoming picture is checked (pm_cvpx_in: a real CVPixelBuffer of the picture's size and pixel format);
 *   - anything else: the caller's one-copy path (VLC's CVPX converter in front of the engine) or pass-through.
 */
#ifndef PM_CVPX_H
#define PM_CVPX_H
#ifdef __APPLE__
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vlc_common.h>
#include <vlc_picture.h>

typedef struct pm_cvpx_eng pm_cvpx_eng;

/* the host VLC's version (libvlc_get_version) inside the tested range (the layout check in build-mac-extras.sh covers exactly these tags) */
bool pm_cvpx_vlc_ok(char *why, size_t n);
/* 8 / 10 for CVPX_NV12 / CVPX_P010, 0 otherwise */
int pm_cvpx_bits(vlc_fourcc_t chroma);
/* the picture's CVPixelBufferRef (borrowed) after the checks, or NULL with the reason */
void *pm_cvpx_in(const picture_t *p, const video_format_t *f, const char **why);
/* a CVPX picture of format f that owns pb (released with the picture); NULL on failure (pb released) */
picture_t *pm_cvpx_out(const video_format_t *f, void *pb);

/* the apple engine on CVPixelBuffers (libpushmpc_engine_apple.dylib next to this plugin) */
pm_cvpx_eng *pm_cvpx_eng_open(const video_format_t *f, int bits, int smoother, double fps, double hz, int *k, char *why, size_t n);
/* gen: up to max retained CVPixelBufferRefs between the previous source and this one; 0 = ok */
int pm_cvpx_eng_process(pm_cvpx_eng *e, void *pb, int64_t pts100ns, bool contiguous, void **gen, int max, int *ngen, bool *pair,
                        char *err, size_t n);
void pm_cvpx_eng_forget(pm_cvpx_eng *e);
const char *pm_cvpx_eng_describe(pm_cvpx_eng *e);
void pm_cvpx_eng_close(pm_cvpx_eng *e);
void pm_cvpx_release(void *pb);
/* interlacing (pm_comb.h): the buffer's luma plane into the meter (locked for reading); the verdict so far, -1: not readable */
int pm_cvpx_comb_push(void *pb, void *comb);
#endif
#endif
