/* pm_ivtc.h: pulldown removal (inverse telecine) for the VLC module's CPU pictures (pm_ivtc.cpp). 24p film carried as
 * 60i by 2:3 pulldown repeats fields in a cycle of five frames, AA BB BC CD DD: the film frame C is split over the two
 * combed frames (TFF: the top field of CD, the bottom field of BC). Given the cycle's phase and the field order that
 * pushmpc/cadence_detect.h measured, the frame BC is held, CD is woven back into C from its own top field and BC's
 * bottom field (BFF: the other way round), and every other frame passes as it is: four film frames out of five,
 * bit-exact for a hard telecine. Planar 4:2:0, 8 bits or 9..16 bits in 16-bit words. Licence: GPL-3.0-or-later. */
#ifndef PM_IVTC_H
#define PM_IVTC_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct pm_ivtc pm_ivtc;
/* phase: the position (0..4) of the first combed frame of the cycle (BC); pos: the position of the next frame pushed;
 * tff: 1 top field first */
pm_ivtc *pm_ivtc_new(int w, int h, int bits, int phase, int pos, int tff);
void pm_ivtc_free(pm_ivtc *v);
/* one telecined frame: 1 = a film frame is ready (pm_ivtc_get), 0 = held (the first combed frame of the pair) */
int pm_ivtc_push(pm_ivtc *v, const uint8_t *const plane[3], const ptrdiff_t stride[3]);
void pm_ivtc_get(pm_ivtc *v, uint8_t *const plane[3], const ptrdiff_t stride[3]);
#ifdef __cplusplus
}
#endif
#endif
