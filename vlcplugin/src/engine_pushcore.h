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
#endif
