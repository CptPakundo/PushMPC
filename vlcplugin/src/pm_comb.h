/* pm_comb.h: interlaced input for the VLC plugin, measured in the picture with the meter every PushMPC route uses
 * (pushmpc/content_detect.h CombMeter: field combing on moving areas of the native luma, 24 moving frames, half or more
 * combed = interlaced). C interface over the C++ meter (pm_comb.cpp). Licence: GPL-3.0-or-later. */
#ifndef PM_COMB_H
#define PM_COMB_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define PM_COMB_MEASURING 0
#define PM_COMB_PROGRESSIVE 1
#define PM_COMB_INTERLACED 2
#define PM_COMB_GIVE_UP 240   /* CombMeter::kGiveUp: frames without a verdict (a still) count as progressive */
void *pm_comb_new(void);
/* one frame's luma (bps 1 or 2 bytes per sample; shift: what takes a sample to 8 bits); returns the verdict so far */
int pm_comb_push(void *c, const void *luma, ptrdiff_t stride_bytes, int width, int height, int bps, int shift);
void pm_comb_counts(void *c, int *measured, int *combed);
/* telecine (2:3 pulldown, pushmpc/cadence_detect.h), measured on the same frames: PM_CADENCE_*; phase: the first of the two
 * combed positions of the cycle of five (frame index % 5, counted from the first frame pushed); tff: the field order
 * measured; frames: the frames pushed so far (the next one has position frames % 5) */
#define PM_CADENCE_MEASURING 0
#define PM_CADENCE_NONE 1
#define PM_CADENCE_TELECINE 2
int pm_comb_cadence(void *c, int *measured, int *combed, int *phase, int *tff, int *frames);
void pm_comb_free(void *c);
#ifdef __cplusplus
}
#endif
#endif
