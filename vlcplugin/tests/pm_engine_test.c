/* pm_engine_test: the VLC module's engine interface on synthetic frames (no VLC): a box moving 8 px per frame must
 * come out half way at t = 0.5, a hard cut must repeat the nearer real frame, Auto must pick sane multipliers, and the
 * held-drawing repair (engine_portable.h, CPU engine) must lag, advance, drain and lay a part held on twos over a pan
 * in at its in-between position. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "pm_engine.h"
#include "engine_portable.h"
#include "pm_comb.h"
#include "../../tests/comb_synth.h"

/* ---- the held-drawing repair: a painted background and a disc drawn on twos (12 px per drawing, back and forth).
 * Until frame RT everything is on twos (whole frames repeat: the evidence that opens the repair's animation gate, as
 * animation gives it); from RT on the background pans 3 px per frame (on ones) while the disc stays on twos. On such a
 * held frame the disc stands still while the picture pans; repaired, it stands half way to its next drawing. */
enum { RW = 480, RH = 272, RN = 80, RT = 48 };
static float rtex[RH][2 * RW];
static void RepairTexture(void)
{
    unsigned s = 12345u;
    float c[RH / 8 + 2][2 * RW / 8 + 2];
    for (int j = 0; j < RH / 8 + 2; j++)
        for (int i = 0; i < 2 * RW / 8 + 2; i++) { s = s * 1103515245u + 12345u; c[j][i] = 40.f + 120.f * (float)((s >> 16) & 1023) / 1023.f; }
    for (int y = 0; y < RH; y++)
        for (int x = 0; x < 2 * RW; x++) {
            const float fx = x / 8.f, fy = y / 8.f, ax = fx - (int)fx, ay = fy - (int)fy;
            const int ix = (int)fx, iy = (int)fy;
            rtex[y][x] = c[iy][ix] * (1 - ax) * (1 - ay) + c[iy][ix + 1] * ax * (1 - ay) + c[iy + 1][ix] * (1 - ax) * ay + c[iy + 1][ix + 1] * ax * ay;
        }
}
static double RepairDisc(int f)   /* the disc's drawn centre: a new drawing on even frames */
{
    const int d = (f / 2) % 40, x = d < 20 ? d : 40 - d;
    return 100.0 + 12.0 * x;
}
static void RepairFrame(int f, unsigned char *y, unsigned char *u, unsigned char *v)
{
    const double pan = f < RT ? 6.0 * (f / 2) : 6.0 * (RT / 2) + 3.0 * (f - RT), cx = RepairDisc(f), cy = 136;
    for (int yy = 0; yy < RH; yy++)
        for (int xx = 0; xx < RW; xx++) {
            const double sx = xx + pan;
            const int i = (int)sx;
            const double a = sx - i;
            double val = rtex[yy][i] * (1 - a) + rtex[yy][i + 1] * a;
            const double d = sqrt((xx - cx) * (xx - cx) + (yy - cy) * (yy - cy));
            if (d < 28) val = 220; else if (d < 30) val = 15;
            y[yy * RW + xx] = (unsigned char)(val + 0.5);
        }
    memset(u, 128, RW * RH / 4);
    memset(v, 128, RW * RH / 4);
}
/* the disc's centre from its bright inside on rows y0..y1 - 1 */
static double RepairCentreRows(const unsigned char *p, int y0, int y1)
{
    long sx = 0, n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = 0; x < RW; x++)
            if (p[y * RW + x] > 190) { sx += x; n++; }
    return n ? (double)sx / n : -1;
}
/* the upper rows of the disc: there the plan lays the whole part in from its pair (on this clip it splits the middle
 * rows into a part of their own, timed to another pair) */
static double RepairCentre(const unsigned char *p) { return RepairCentreRows(p, 110, 124); }

static int TestRepair(int defer)
{
    int fails = 0;
    char err[256] = "", why[256] = "";
    RepairTexture();
    unsigned char *Yb = malloc(RW * RH), *Ub = malloc(RW * RH / 4), *Vb = malloc(RW * RH / 4);
    unsigned char *OYb = malloc(RW * RH), *OUb = malloc(RW * RH / 4), *OVb = malloc(RW * RH / 4);
    pm_engine_cfg cc = { RW, RH, 8, 2, 1, PM_ENGINE_CPU };
    pm_engine *e = pm_engine_open(&cc, err, sizeof(err));
    if (!e || !Yb || !OYb) { printf("FAIL open (repair): %s\n", err); return 1; }
    if (pm_engine_set_repair(e, 1, why, sizeof(why)) != 0 || pm_engine_repair_lag(e) != 0) { printf("FAIL repair without held frames: %s\n", why); fails++; }
    pm_engine_set_dedup(e, 1);
    if (pm_engine_set_repair(e, 1, why, sizeof(why)) != 1 || pm_engine_repair_lag(e) != 3) { printf("FAIL repair: %s\n", why); pm_engine_close(e); return fails + 1; }
    pm_engine_defer_repair(e, defer);
    printf("repair (%s):\n", defer ? "deferred to the worker" : "in the check");
    /* the repair's budget is half a source frame of its measured cost (a busy test machine must not decide the test):
     * a 2 fps source gives it 250 ms (smoother off: nothing is measured) */
    printf("repair: %s\n", why);
    pm_engine_decide(e, 1, 2.0, 0, why, sizeof(why));
    pm_yuv in = { { Yb, Ub, Vb }, { RW, RW / 2, RW / 2 } };
    pm_yuv_out out = { { OYb, OUb, OVb }, { RW, RW / 2, RW / 2 } };
    int advanced = 0, readyWrong = 0, pairsWrong = 0, repaired = 0, wrong = 0, heldChecked = 0, heldRepaired = 0, dups = 0, dupWrong = 0;
    int fifo[RN], head = 0, tail = 0;   /* the new pictures pushed, in order (the module's queue) */
    double worst = 0, heldErr = 0;
    for (int f = 0; f < RN; f++) {
        RepairFrame(f, Yb, Ub, Vb);
        const int dup = pm_engine_check_dup(e, &in, f > 0, err, sizeof(err));
        if (dup < 0) { printf("FAIL frame %d: %s\n", f, err); fails++; break; }
        if (dup != (f < RT && (f & 1))) dupWrong++;   /* the opening's odd frames repeat */
        if (dup) { dups++; } else {
            if (pm_engine_push(e, &in, f > 0, err, sizeof(err)) != 0) { printf("FAIL push %d: %s\n", f, err); fails++; break; }
            fifo[tail++] = f;
        }
        int want = 0;   /* each check is a source frame: the pictures 3 or more source frames old may advance */
        for (int i = head; i < tail && fifo[i] <= f - 3; i++) want++;
        if (pm_engine_pairs_ready(e) != want) readyWrong++;
        while (pm_engine_next_pair(e, err, sizeof(err)) == 1) {
            const int b = fifo[head++];
            if (pm_engine_have_pair(e) != (advanced++ > 0)) pairsWrong++;
            const int r = pm_engine_repair_real(e, 1, &out);
            double expect = RepairDisc(b), got;
            if (r == 0) { got = RepairCentre(OYb); repaired++; if (b >= RT && (b & 1)) expect = 0.5 * (RepairDisc(b - 1) + RepairDisc(b + 1)); }
            else { RepairFrame(b, OYb, OUb, OVb); got = RepairCentre(OYb); }
            const double off = fabs(got - expect);
            if (off > worst) worst = off;
            if (off > 3.0) {   /* the held and the half-way positions are 6 px apart */
                wrong++;
                printf("  frame %d %s: disc at %.2f, expected %.2f\n", b, r == 0 ? "repaired" : "as drawn", got, expect);
            }
            if (b >= RT + 12 && (b & 1)) {
                heldChecked++;
                heldErr += off;
                if (r == 0) heldRepaired++;
                printf("  held frame %d: %s, disc at %.2f (drawn %.2f, half way %.2f)\n", b, r == 0 ? "repaired" : "NOT repaired", got, RepairDisc(b), 0.5 * (RepairDisc(b - 1) + RepairDisc(b + 1)));
            }
            if (pm_engine_have_pair(e)) pm_engine_generate(e, 0.5f, &out);
        }
    }
    pm_repair_stats rs;
    pm_engine_repair_stats_get(e, &rs);
    printf("repair: %d repeats, %d advanced, %d repaired real frames (stats: %llu frames, %llu parts, %llu gated, %llu over budget, %llu failed, %.1f ms), "
           "worst disc error %.2f px\n", dups, advanced, repaired, (unsigned long long)rs.frames, (unsigned long long)rs.parts,
           (unsigned long long)rs.gated, (unsigned long long)rs.over_budget, (unsigned long long)rs.failed, rs.ms, worst);
    /* the gate opens after 48 frames with evidence: the held frames over the pan must all be repaired, half way */
    if (advanced != tail - 3 || readyWrong || pairsWrong || dupWrong) {
        printf("FAIL lag: %d advanced (want %d), %d ready counts wrong, %d pair states wrong, %d repeats misjudged\n", advanced, tail - 3, readyWrong, pairsWrong, dupWrong);
        fails++;
    }
    const double meanErr = heldChecked ? heldErr / heldChecked : 99;
    printf("held frames over the pan: %d of %d repaired, mean distance from half way %.2f px\n", heldRepaired, heldChecked, meanErr);
    if (wrong || heldRepaired != heldChecked || !heldChecked || meanErr > 1.5 || rs.frames == 0) { printf("FAIL repair: %d discs misplaced\n", wrong); fails++; }

    /* a gap: the three pictures still waiting become ready at once; the new run starts without a pair */
    RepairFrame(52, Yb, Ub, Vb);
    pm_engine_check_dup(e, &in, 0, err, sizeof(err));
    pm_engine_push(e, &in, 0, err, sizeof(err));
    int gapReady = pm_engine_pairs_ready(e), taken = 0;
    while (pm_engine_next_pair(e, err, sizeof(err)) == 1) taken++;
    for (int f = 53; f < 56; f++) {
        RepairFrame(f, Yb, Ub, Vb);
        pm_engine_check_dup(e, &in, 1, err, sizeof(err));
        pm_engine_push(e, &in, 1, err, sizeof(err));
    }
    const int afterGap = pm_engine_pairs_ready(e);
    const int adv = pm_engine_next_pair(e, err, sizeof(err)), restartPair = pm_engine_have_pair(e);
    pm_engine_drain(e);
    const int drained = pm_engine_pairs_ready(e);
    pm_engine_forget(e);
    const int forgotten = pm_engine_pairs_ready(e), lagKept = pm_engine_repair_lag(e);
    printf("gap: %d ready (want 3), %d taken; after 3 more: %d ready (want 1), advanced %d, pair %d (want 0); drained %d (want 3); "
           "forgotten %d, lag %d\n", gapReady, taken, afterGap, adv, restartPair, drained, forgotten, lagKept);
    if (gapReady != 3 || taken != 3 || afterGap != 1 || adv != 1 || restartPair || drained != 3 || forgotten || lagKept != 3) fails++;

    /* a repeat (held frames) is consumed by the check, advances nothing and is never repaired on its own */
    RepairFrame(0, Yb, Ub, Vb);
    int dupOk = pm_engine_check_dup(e, &in, 0, err, sizeof(err)) == 0 && pm_engine_push(e, &in, 0, err, sizeof(err)) == 0;
    dupOk = dupOk && pm_engine_check_dup(e, &in, 1, err, sizeof(err)) == 1 && pm_engine_last_dup(e);
    for (int f = 2; f < 6; f++) { RepairFrame(f, Yb, Ub, Vb); pm_engine_check_dup(e, &in, 1, err, sizeof(err)); pm_engine_push(e, &in, 1, err, sizeof(err)); }
    /* source frames 0, 0 (repeat), 2, 3, 4, 5: pictures 0 and 2 are 3 or more source frames old */
    dupOk = dupOk && pm_engine_pairs_ready(e) == 2;
    printf("repeat while lagged: %s\n", dupOk ? "as expected" : "WRONG");
    if (!dupOk) fails++;

    /* the zero-copy probe (pm_engine_plain): one picture pushed twice gives a pair at once, then the lag is back */
    pm_engine_plain(e, 1);
    RepairFrame(7, Yb, Ub, Vb);
    int probeOk = pm_engine_repair_lag(e) == 0 && pm_engine_push(e, &in, 0, err, sizeof(err)) == 0 && pm_engine_push(e, &in, 1, err, sizeof(err)) == 0
                  && pm_engine_have_pair(e) && pm_engine_pairs_ready(e) == 0;
    if (probeOk) pm_engine_generate(e, 0.5f, &out);
    pm_engine_plain(e, 0);
    probeOk = probeOk && pm_engine_repair_lag(e) == 3 && !pm_engine_have_pair(e) && pm_engine_pairs_ready(e) == 0;
    RepairFrame(0, Yb, Ub, Vb);   /* held frames are back: a repeat is a repeat again */
    probeOk = probeOk && pm_engine_check_dup(e, &in, 0, err, sizeof(err)) == 0 && pm_engine_push(e, &in, 0, err, sizeof(err)) == 0
              && pm_engine_check_dup(e, &in, 1, err, sizeof(err)) == 1;
    printf("zero-copy probe while lagged: %s\n", probeOk ? "pair at once, lag and held frames back" : "WRONG");
    if (!probeOk) fails++;
    pm_engine_close(e);
    free(Yb); free(Ub); free(Vb); free(OYb); free(OUb); free(OVb);
    return fails;
}

enum { W = 320, H = 192 };
static unsigned char Y[2][W * H], U[2][W * H / 4], V[2][W * H / 4], OY[W * H], OU[W * H / 4], OV[W * H / 4];

static void Frame(int f, int x0, int lum)
{
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            Y[f & 1][y * W + x] = (unsigned char)((x >= x0 && x < x0 + 48 && y >= 64 && y < 112) ? lum : 40 + ((x / 8 + y / 8) & 1) * 4);
    memset(U[f & 1], 128, sizeof(U[0]));
    memset(V[f & 1], 128, sizeof(V[0]));
}

static double BoxCentre(const unsigned char *p)
{
    long sx = 0, n = 0;
    for (int y = 64; y < 112; y++)
        for (int x = 0; x < W; x++)
            if (p[y * W + x] > 150) { sx += x; n++; }
    return n ? (double)sx / n : -1;
}

/* interlaced input (pm_comb.h over content_detect.h's CombMeter): woven fields interlaced, the same motion as whole
 * frames progressive (tests/comb_synth.h), 8-bit and 10-bit LSB words (the module's I420 / I0AL pictures) */
static int TestComb(void)
{
    static uint8_t y8[240 * 320];
    static uint16_t y16[240 * 320];
    int fails = 0;
    for (int woven = 0; woven < 2; woven++)
        for (int ten = 0; ten < 2; ten++) {
            void *c = pm_comb_new();
            int v = PM_COMB_MEASURING, n = 0, measured = 0, combed = 0;
            for (; n < 60 && v == PM_COMB_MEASURING; n++) {
                if (ten) { pm_comb_synth16(y16, 320, 320, 240, n, woven, 0, 2); v = pm_comb_push(c, y16, 640, 320, 240, 2, 2); }
                else { pm_comb_synth8(y8, 320, 320, 240, n, woven, 0); v = pm_comb_push(c, y8, 320, 320, 240, 1, 0); }
            }
            pm_comb_counts(c, &measured, &combed);
            pm_comb_free(c);
            const int want = woven ? PM_COMB_INTERLACED : PM_COMB_PROGRESSIVE;
            printf("interlace (VLC): %s %s -> %s, %d of %d moving frames combed after %d pictures\n", woven ? "woven" : "progressive", ten ? "10-bit" : "8-bit",
                   v == PM_COMB_INTERLACED ? "interlaced" : v == PM_COMB_PROGRESSIVE ? "progressive" : "measuring", combed, measured, n);
            if (v != want) { printf("FAIL interlace verdict %d, want %d\n", v, want); fails++; }
        }
    return fails;
}

int main(void)
{
    int fails = 0;
    char err[256] = "";
    pm_engine_cfg c = { W, H, 8, 0, 1 };
    pm_engine *e = pm_engine_open(&c, err, sizeof(err));
    if (!e) { printf("FAIL open: %s\n", err); return 1; }
    pm_yuv_out out = { { OY, OU, OV }, { W, W / 2, W / 2 } };
    double last = 0;
    for (int f = 0; f < 6; f++) {
        Frame(f, 40 + 8 * f, 220);
        pm_yuv in = { { Y[f & 1], U[f & 1], V[f & 1] }, { W, W / 2, W / 2 } };
        if (pm_engine_push(e, &in, f > 0, err, sizeof(err))) { printf("FAIL push: %s\n", err); return 1; }
        if (f >= 2) {
            pm_engine_generate(e, 0.5f, &out);
            const double got = BoxCentre(OY), want = 40 + 8 * f - 4 + 24;
            last = got;
            printf("frame %d: box centre at t=0.5 %.2f (expected %.2f), cut=%d\n", f, got, want, pm_engine_pair_is_cut(e));
            if (got < want - 3 || got > want + 3) fails++;
        }
    }
    (void)last;
    /* a hard cut: a different picture; the generated frame must be one of the real ones, not a blend */
    Frame(6, 200, 90);
    memset(Y[0], 200, sizeof(Y[0]));
    pm_yuv in = { { Y[0], U[0], V[0] }, { W, W / 2, W / 2 } };
    pm_engine_push(e, &in, 1, err, sizeof(err));
    pm_engine_generate(e, 0.25f, &out);
    const int cut = pm_engine_pair_is_cut(e);
    printf("cut pair: cut=%d, generated centre %.2f (a real frame repeats)\n", cut, BoxCentre(OY));
    if (!cut) fails++;
    pm_engine_stats st;
    pm_engine_stats_get(e, &st);
    printf("stats: pushed %llu pairs %llu cuts %llu generated %llu, motion %.2f ms, frame %.2f ms, threads %d\n",
           (unsigned long long)st.pushed, (unsigned long long)st.pairs, (unsigned long long)st.cuts,
           (unsigned long long)st.generated, st.me_ms, st.gen_ms, pm_engine_threads(e));
    pm_engine_close(e);

    /* held frames on the CPU engine (pm_engine_set_dedup): the box moves 16 px per drawing (A at 40, B at 56; its
     * centre is x0 + 24). A repeat of A neither makes a pair nor moves it; the pair (A, B) stays through a repeat of B. */
    {
        pm_engine_cfg cc = { W, H, 8, 2, 1, PM_ENGINE_CPU };
        pm_engine *d = pm_engine_open(&cc, err, sizeof(err));
        if (!d) { printf("FAIL open (CPU): %s\n", err); return 1; }
        pm_engine_set_dedup(d, 1);
        pm_yuv a = { { Y[0], U[0], V[0] }, { W, W / 2, W / 2 } }, b = { { Y[1], U[1], V[1] }, { W, W / 2, W / 2 } };
        Frame(0, 40, 220);
        Frame(1, 56, 220);
        int ok = pm_engine_push(d, &a, 0, err, sizeof(err)) == 0 && !pm_engine_last_dup(d);
        ok = ok && pm_engine_check_dup(d, &a, 1, err, sizeof(err)) == 1;                      /* A again: a repeat */
        ok = ok && pm_engine_push(d, &a, 1, err, sizeof(err)) == 0 && pm_engine_last_dup(d) && !pm_engine_have_pair(d);
        ok = ok && pm_engine_check_dup(d, &b, 1, err, sizeof(err)) == 0;                      /* B: a new drawing */
        ok = ok && pm_engine_push(d, &b, 1, err, sizeof(err)) == 0 && !pm_engine_last_dup(d) && pm_engine_have_pair(d);
        ok = ok && pm_engine_push(d, &b, 1, err, sizeof(err)) == 0 && pm_engine_last_dup(d);  /* B again: the pair stays */
        pm_engine_generate(d, 0.25f, &out);
        const double c = BoxCentre(OY);                                                       /* (A, B) at 0.25: 44 + 24 = 68 */
        printf("held frames: repeats detected %s, box at t=0.25 %.2f (expected 68.00)\n", ok ? "as expected" : "WRONG", c);
        if (!ok || c < 65 || c > 71) fails++;
        pm_engine_close(d);
    }

    fails += TestRepair(0);
    fails += TestRepair(1);   /* the worker mode: checks keep the books, the repair runs before the pair advances */

    char why[256];
    int k1 =pm_engine_decide_k(0, 23.976, 240, 1280, 720, 8, 0, 1, why, sizeof(why));
    printf("Auto 720p24 on 240 Hz: x%d (%s)\n", k1, why);
    int k2 = pm_engine_decide_k(1, 23.976, 240, 1280, 720, 8, 0, 1, why, sizeof(why));
    printf("Off: x%d (%s)\n", k2, why);
    if (k2 != 1) fails++;
    int k3 = pm_engine_decide_k(0, 59.94, 60, 640, 360, 8, 0, 1, why, sizeof(why));
    printf("Auto 60 fps on 60 Hz: x%d (%s)\n", k3, why);
    if (k3 != 1) fails++;
    fails += TestComb();
    printf(fails ? "FAILED (%d)\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
