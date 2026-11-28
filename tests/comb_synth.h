/* comb_synth.h: synthetic luma for the interlace tests (C and C++). A diagonal sinusoid moving 4 pixels per frame to
 * the right: pm_comb_synth(..., woven = 0) is one progressive frame at time t; woven = 1 weaves two fields of it
 * from different moments (the top lines at t, the bottom lines half a frame later), as a 50i / 60i capture shows it.
 * shift: the samples are written << shift (0 for 8-bit, 2 for 10-bit LSB-aligned words, 8 for 16-bit / P010 MSB words). */
#ifndef PM_COMB_SYNTH_H
#define PM_COMB_SYNTH_H
#include <math.h>
#include <stddef.h>
#include <stdint.h>

static inline int pm_comb_synth_value(int x, int y, double t)
{
    const double v = 128.0 + 90.0 * sin(6.283185307179586 * ((double)x + 0.5 * (double)y - 4.0 * t) / 24.0);
    return v < 0 ? 0 : v > 255 ? 255 : (int)(v + 0.5);
}
/* 8-bit plane (stride in bytes) */
static inline void pm_comb_synth8(uint8_t *p, ptrdiff_t stride, int w, int h, int frame, int woven, int still)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const double t = still ? 0.0 : (double)frame + (woven && (y & 1) ? 0.5 : 0.0);
            p[(ptrdiff_t)y * stride + x] = (uint8_t)pm_comb_synth_value(x, y, t);
        }
}
/* 16-bit words (stride in words) */
static inline void pm_comb_synth16(uint16_t *p, ptrdiff_t stride, int w, int h, int frame, int woven, int still, int shift)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const double t = still ? 0.0 : (double)frame + (woven && (y & 1) ? 0.5 : 0.0);
            p[(ptrdiff_t)y * stride + x] = (uint16_t)(pm_comb_synth_value(x, y, t) << shift);
        }
}
#endif
