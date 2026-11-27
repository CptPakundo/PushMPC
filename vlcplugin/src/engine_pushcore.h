/* engine_pushcore.h: the VLC module's side of pm_pushcore.dll (see ../pushcore_bridge/pm_pushcore.h). */
#ifndef ENGINE_PUSHCORE_H
#define ENGINE_PUSHCORE_H
#ifndef _WIN32
# define __declspec(x)
#endif
#include "../pushcore_bridge/pm_pushcore.h"

pm_pc *pmx_open(const pm_pc_cfg *cfg, int *k, char *err, size_t n);
int pmx_push(pm_pc *pc, const pm_pc_planes *src, int64_t pts100ns, int discontinuity, char *err, size_t n);
int pmx_poll(pm_pc *pc, uint32_t timeout_ms, pm_pc_dest_fn dest, pm_pc_done_fn done, void *user, char *err, size_t n);
int pmx_flush(pm_pc *pc);
void pmx_close(pm_pc *pc);
const char *pmx_describe(pm_pc *pc);
/* zero copy (Windows, pm_pushcore.dll with the D3D11 exports): pushcore on VLC's own D3D11 device */
int pmx_has_d3d11(void);
pm_pc *pmx_open_d3d11(const pm_pc_cfg *cfg, void *device, void *mutex, int *k, char *err, size_t n);
int pmx_push_d3d11(pm_pc *pc, void *texture, unsigned slice, int64_t pts100ns, int discontinuity, char *err, size_t n);
int pmx_poll_d3d11(pm_pc *pc, uint32_t timeout_ms, pm_pc_dest_tex_fn dest, pm_pc_done_fn done, void *user, char *err, size_t n);
int pmx_copy_d3d11(pm_pc *pc, void *src, unsigned src_slice, void *dst, unsigned dst_slice);
/* ABI 3 (pm_pushcore.h): the bridge's ABI (0 = none loaded, 2 = no update), a settings update for the running stream
 * (the measured class), what runs */
unsigned pmx_abi(void);
int pmx_update(pm_pc *pc, const pm_pc_cfg *cfg, char *err, size_t n);
int pmx_output(pm_pc *pc, pm_pc_info *info);
/* sharing the one pushcore with the Direct3D 11 output's upscaler: Auto's multiplier without opening (pm_pc_decide).
 * 1 = decided (*k), 0 = the bridge cannot say (an older one: open to find out), < 0 = the open would fail too (err) */
int pmx_decide(const pm_pc_cfg *cfg, int *k, char *err, size_t n);
#endif
