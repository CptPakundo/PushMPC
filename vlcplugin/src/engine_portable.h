/*
 * engine_portable.h: the held-drawing repair on the VLC module's portable engines (engine_portable.cpp), on top of
 * pm_engine.h.
 *
 * Animation often holds one part of the picture between two of its drawings while the rest moves (a character drawn
 * on twos over a pan drawn on ones). Held frames (pm_engine_set_dedup) only re-time whole repeated frames; the repair
 * lays each held part in from its own pair at its own phase (PushMPC's HeldFrames.h, the same scheduling as the other
 * portable routes), so the part moves evenly too. It has to see the part's next drawing first: a frame is repaired
 * when the frame pm_engine_repair_lag() source frames later has been checked, and only then can its pairs be made.
 *
 * With the repair set (pm_engine_set_repair returned 1), the engine runs LAGGED:
 *   - every source frame goes through pm_engine_check_dup (1 = a repeat, not pushed; 0 = a new picture, push the SAME
 *     frame next as before; the check already took it in). The push itself makes no pair any more;
 *   - after each check, pm_engine_pairs_ready() pairs can be taken, oldest first, with pm_engine_next_pair(): each one
 *     advances the engine by one new picture, in the order they were pushed. After an advance, pm_engine_have_pair,
 *     pm_engine_pair_is_cut, pm_engine_generate and pm_engine_generate_dmabuf refer to the pair (the picture advanced
 *     before, the picture just advanced), exactly as they referred to (previous, current) after a push before;
 *   - the real frames: pm_engine_repair_real(e, 0, out) writes the pair's first picture as repaired (0), or returns 1
 *     when it was not changed (the source picture is right as it is); which = 1: the second picture;
 *   - a non-contiguous check makes every picture before it ready (unrepaired where its lookahead was cut), so the
 *     caller takes those pairs before the new picture's; pm_engine_drain does the same at the end of a stream;
 *     pm_engine_forget drops everything (a seek).
 * The GPU engine repairs in its own source slots (Cells and RepairSlot); the CPU engine and the apple engine get their
 * frames repaired on the CPU (16-bit copies, each part made by a second CPU engine from its pair, then composited).
 */
#ifndef PM_ENGINE_PORTABLE_H
#define PM_ENGINE_PORTABLE_H
#include "pm_engine.h"
#ifdef __cplusplus
extern "C" {
#endif

/* on: 1 lay the held parts in (Auto: on with held frames, for every class, as the other portable routes; the
 * scheduler's own animation gate keeps live action untouched), 0 stop laying them in (the lag stays, so the caller's
 * queue keeps working). The first call with on = 1 must come after pm_engine_set_dedup(e, 1) and before the first
 * check / push: it may reopen the GPU engine with the repair's slots. Returns 1 when the parts are laid in from now on,
 * else 0 with the reason in why. */
int pm_engine_set_repair(pm_engine *e, int on, char *why, size_t whysize);
int pm_engine_repair_lag(const pm_engine *e);    /* 0: not lagged (pairs follow the pushes); else source frames */
/* a probe that pushes one picture twice and generates at once (the zero-copy round trip): 1 sets the lag and held frames
 * aside (nothing waiting is kept), 0 forgets the probe's frames and puts them back */
void pm_engine_plain(pm_engine *e, int on);
/* lagged on the CPU, with a worker thread: 1 = a check only keeps the books (the held-frame test, the copy, the cell
 * grid) and leaves the frame's repair for pm_engine_run_deferred, which the worker calls (next_pair and repair_real
 * call it too, so nothing advances unrepaired). The checks stay on the caller's thread: they say what comes out. */
void pm_engine_defer_repair(pm_engine *e, int on);
void pm_engine_run_deferred(pm_engine *e);
int pm_engine_pairs_ready(const pm_engine *e);   /* lagged: the pairs pm_engine_next_pair can take now */
/* lagged: 1 = advanced by one picture (then pm_engine_have_pair says whether a pair runs), 0 = none ready, -1 = error */
int pm_engine_next_pair(pm_engine *e, char *err, size_t errsize);
void pm_engine_drain(pm_engine *e);              /* lagged: every picture checked so far becomes ready */
/* lagged, after an advance: the pair's first (which 0) or second (1) picture as repaired. 0 written, 1 not repaired
 * (keep the source picture), -1 error. The dmabuf form needs a pair (1 without one). */
int pm_engine_repair_real(pm_engine *e, int which, const pm_yuv_out *out);
int pm_engine_repair_real_dmabuf(pm_engine *e, int which, const pm_dmabuf *dst, char *err, size_t errsize);

typedef struct pm_repair_stats {
    uint64_t frames, parts;    /* frames repaired, parts laid in */
    uint64_t gated;            /* held spans the animation gate kept out (live action) */
    uint64_t over_budget;      /* held spans left out because the repair cost more than half a source frame */
    uint64_t failed;           /* repairs that failed (the frame stays as it was) */
    double ms;                 /* total repair time */
    int lag, on, gpu;          /* the lag (source frames), parts laid in, repaired on the GPU (else the CPU) */
} pm_repair_stats;
void pm_engine_repair_stats_get(const pm_engine *e, pm_repair_stats *out);

#ifdef __cplusplus
}
#endif
#endif
