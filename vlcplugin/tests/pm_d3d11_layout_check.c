/* pm_d3d11_layout_check.c: compile-time check (Windows build, with the fetched VLC 3.0.24 sources) that
 * src/pm_d3d11_picsys.h matches VLC's own picture_sys_t and va_pic_context. Never linked. */
#include <stddef.h>
#include "../src/pm_d3d11_picsys.h"
#include "video_chroma/d3d11_fmt.h"

#define SAME(a, b, f) _Static_assert(offsetof(a, f) == offsetof(b, f), #f " moved")
_Static_assert(sizeof(pm_d3d11_picsys) == sizeof(struct picture_sys_t), "picture_sys_t size changed");
SAME(pm_d3d11_picsys, struct picture_sys_t, decoder);
SAME(pm_d3d11_picsys, struct picture_sys_t, texture);
SAME(pm_d3d11_picsys, struct picture_sys_t, context);
SAME(pm_d3d11_picsys, struct picture_sys_t, slice_index);
SAME(pm_d3d11_picsys, struct picture_sys_t, resourceView);
SAME(pm_d3d11_picsys, struct picture_sys_t, formatTexture);
_Static_assert(offsetof(pm_va_pic_context, picsys) == offsetof(struct va_pic_context, picsys), "va_pic_context changed");
_Static_assert(D3D11_MAX_SHADER_VIEW == 4, "D3D11_MAX_SHADER_VIEW changed");
int pm_d3d11_layout_ok;
