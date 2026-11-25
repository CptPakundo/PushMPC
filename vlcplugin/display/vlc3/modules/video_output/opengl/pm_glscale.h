// SPDX-License-Identifier: GPL-3.0-or-later
// pm_glscale: the upscaler of PushMPC's OpenGL output fork (pushmpc_gl). AMD FidelityFX Super Resolution 1 (EASU, edge
// adaptive upscaling, then RCAS, contrast adaptive sharpening) as GLSL 1.20 / ES 1.00 fragment shaders, so it runs on
// any GPU with OpenGL 2.1 + framebuffer objects. VLC's own shaders first draw the picture (any chroma, any glconv) into
// a source-size RGBA texture; EASU scales that to the placement size and RCAS sharpens it into the window.
#ifndef PM_GLSCALE_H
#define PM_GLSCALE_H

#include <vlc_common.h>
#include <vlc_opengl.h>
#include "converter.h"

typedef struct pm_glscale pm_glscale;

/* mode: 0 = off (VLC's bilinear), 1 = FSR 1; sharpness 0..1 (1 = strongest); NULL when the GL lacks what it needs */
pm_glscale *PM_GlScaleCreate(vlc_object_t *, vlc_gl_t *, const opengl_vtable_t *, unsigned glsl_version,
                             const char *precision_header, float sharpness);
void PM_GlScaleDestroy(pm_glscale *);

/* Begin: true when this frame is upscaled (src_w x src_h smaller than the current viewport and usable): VLC's draw that
 * follows goes into the source-size texture. End: EASU + RCAS into the framebuffer and viewport that were bound. */
bool PM_GlScaleBegin(pm_glscale *, unsigned src_w, unsigned src_h);
void PM_GlScaleEnd(pm_glscale *);

#endif
