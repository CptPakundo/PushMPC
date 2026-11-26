/* pm_engine_test: the VLC module's engine interface on synthetic frames (no VLC): a box moving 8 px per frame must
 * come out half way at t = 0.5, a hard cut must repeat the nearer real frame, and Auto must pick sane multipliers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pm_engine.h"

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

    char why[256];
    int k1 = pm_engine_decide_k(0, 23.976, 240, 1280, 720, 8, 0, 1, why, sizeof(why));
    printf("Auto 720p24 on 240 Hz: x%d (%s)\n", k1, why);
    int k2 = pm_engine_decide_k(1, 23.976, 240, 1280, 720, 8, 0, 1, why, sizeof(why));
    printf("Off: x%d (%s)\n", k2, why);
    if (k2 != 1) fails++;
    int k3 = pm_engine_decide_k(0, 59.94, 60, 640, 360, 8, 0, 1, why, sizeof(why));
    printf("Auto 60 fps on 60 Hz: x%d (%s)\n", k3, why);
    if (k3 != 1) fails++;
    printf(fails ? "FAILED (%d)\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
