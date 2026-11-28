/* pm_deint.h: a double-rate deinterlacer for the VLC module's CPU pictures (pm_deint.cpp): the YADIF algorithm
 * (spatial prediction with an edge-directed check, limited by the temporal neighbours), one progressive frame per
 * field. YADIF was the best on average of the deinterlacers scored against 60p masters in the deinterlace probe
 * (38.2 dB; bwdif 38.0). VLC 3's own deinterlacer runs after PushMPC's filter in the static chain, so the module
 * deinterlaces itself before generating frames. Planar 4:2:0, 8 bits or 9..16 bits in 16-bit words.
 * Licence: GPL-3.0-or-later. */
#ifndef PM_DEINT_H
#define PM_DEINT_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct pm_deint pm_deint;
/* w x h luma (even), bits 8..16; NULL when out of memory */
pm_deint *pm_deint_new(int w, int h, int bits);
void pm_deint_free(pm_deint *d);
/* one woven frame (3 planes and their strides in bytes; tff: 1 top field first). The deinterlacer looks one frame
 * ahead: from the second frame on, the previous frame's two fields are ready (returns 2), else 0 */
int pm_deint_push(pm_deint *d, const uint8_t *const plane[3], const ptrdiff_t stride[3], int tff);
/* the ready frame of field 0 (the first in time) or 1, written into 3 planes */
void pm_deint_get(pm_deint *d, int field, uint8_t *const plane[3], const ptrdiff_t stride[3]);
/* a seek or a gap: the next frame starts over (no temporal neighbours across it) */
void pm_deint_reset(pm_deint *d);
#ifdef __cplusplus
}
#endif
#endif
