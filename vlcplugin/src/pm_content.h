/*
 * pm_content: ContentDetect for the VLC module, through PushMPC's content library (pushmpc/content-shim:
 * pushmpc_content.dll / libpushmpc_content.so / libpushmpc_content.dylib), loaded at run time from the module's own
 * folder, else from PushMPC's VLC folder. The library measures what the picture is (drawn animation or not; rendered 3D
 * CG or hand-drawn) with the image model when its runtime is installed, else with the hand-built cues alone. When the
 * library is missing or does not match, pm_content_open says why and the module runs without it, as before.
 * Plain C (no VLC headers), so the library's own test uses the same loader.
 */
#ifndef PM_CONTENT_H
#define PM_CONTENT_H
#include <stddef.h>
#include <stdint.h>
#include "pushmpc_content.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct pm_content pm_content;

/* NULL when the library is not there, is incomplete or has another ABI (why: the reason); where: the file loaded */
pm_content *pm_content_open(pmc_log_fn log, void *log_user, uint32_t flags, char *why, size_t whysize, char *where, size_t wheresize);
void pm_content_close(pm_content *c);
/* pmc_push_yuv420 (pushmpc_content.h) */
int pm_content_push(pm_content *c, const void *y, ptrdiff_t ystride, const void *u, const void *v, ptrdiff_t cstride,
                    int w, int h, int bits, int limited, double pts, int discontinuity);
int pm_content_state(pm_content *c, pmc_state *out);
/* a GPU tap's packed cells / the image model's thumbnail (pmc_push_cells / pmc_offer_clip, optional in the library):
 * has_cells 0 = an older library (or PUSHMPC_CONTENT_NO_CELLS=1): zero-copy pictures stay unmeasured */
int pm_content_has_cells(const pm_content *c);
int pm_content_push_cells(pm_content *c, const uint32_t *words, ptrdiff_t stride_words, int cw, int ch, int limited, double pts, int discontinuity);
void pm_content_offer_clip(pm_content *c, const uint32_t *words, ptrdiff_t stride_words, int hd, int limited, double pts);
void pm_content_evidence(pm_content *c, double pdrawn, double pts, double pcg);
const char *pm_content_class_name(pm_content *c, int cls);
/* "hold-repair, anime-upscaler, ..." for the log ("none" when no bit is set) */
void pm_content_policy_text(uint32_t policy, char *out, size_t n);

#ifdef __cplusplus
}
#endif
#endif
