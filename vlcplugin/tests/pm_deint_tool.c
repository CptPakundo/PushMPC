/* pm_deint_tool <in.yuv> <out.yuv> <width> <height> [tff 1|0]: the module's deinterlacer (pm_deint.h) on raw 8-bit
 * yuv420p woven frames, two progressive frames out per frame in (the last frame's fields are not written: the
 * deinterlacer looks one frame ahead).
 * pm_deint_tool ivtc <in.yuv> <out.yuv> <width> <height>: the module's pulldown removal on a telecined stream, as the
 * module runs it: the cadence measured first (pm_comb_cadence; frames pass as they are meanwhile), then pm_ivtc.h with
 * the phase and field order measured; only the frames after the measurement are written. With [master.yuv] (ivtc):
 * how many of them are bit-identical to a master frame, and whether they come in the master's order.
 * vlc-plugin.yml scores both against their masters. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pm_comb.h"
#include "pm_deint.h"
#include "pm_ivtc.h"

int main(int argc, char **argv)
{
    const int ivtc = argc > 1 && strcmp(argv[1], "ivtc") == 0;
    char **a = argv + ivtc;
    if (argc - ivtc < 5) { fprintf(stderr, "usage: pm_deint_tool [ivtc] <in.yuv> <out.yuv> <w> <h> [tff | master.yuv]\n"); return 2; }
    const int w = atoi(a[3]), h = atoi(a[4]), tff = !ivtc && argc > 5 ? atoi(a[5]) : 1;
    const char *master = ivtc && argc - ivtc > 5 ? a[5] : NULL;
    FILE *in = fopen(a[1], "rb"), *out = fopen(a[2], "wb");
    if (!in || !out) return 2;
    const size_t ys = (size_t)w * h, cs = ys / 4, fs = ys + 2 * cs;
    uint8_t *buf = malloc(fs), *o = malloc(fs);
    if (!buf || !o) return 2;
    const uint8_t *pl[3] = { buf, buf + ys, buf + ys + cs };
    uint8_t *ol[3] = { o, o + ys, o + ys + cs };
    const ptrdiff_t st[3] = { w, w / 2, w / 2 };
    int frames = 0, written = 0;
    if (!ivtc) {
        pm_deint *d = pm_deint_new(w, h, 8);
        if (!d) return 2;
        while (fread(buf, 1, fs, in) == fs) {
            frames++;
            if (pm_deint_push(d, pl, st, tff) == 2)
                for (int f = 0; f < 2; f++) { pm_deint_get(d, f, ol, st); fwrite(o, 1, fs, out); written++; }
        }
        pm_deint_free(d);
    } else {
        void *c = pm_comb_new();
        pm_ivtc *v = NULL;
        int cad = PM_CADENCE_MEASURING, ph = 0, ftff = 1, m = 0, cb = 0, fr = 0;
        while (fread(buf, 1, fs, in) == fs) {
            frames++;
            if (!v) {
                pm_comb_push(c, buf, w, w, h, 1, 0);
                cad = pm_comb_cadence(c, &m, &cb, &ph, &ftff, &fr);
                if (cad == PM_CADENCE_TELECINE) {
                    v = pm_ivtc_new(w, h, 8, ph, frames - 1, ftff);
                    printf("pm_deint_tool: telecine measured at frame %d (positions %d-%d, %s)\n", frames, ph, (ph + 1) % 5, ftff ? "top field first" : "bottom field first");
                } else continue;
            }
            if (pm_ivtc_push(v, pl, st)) { pm_ivtc_get(v, ol, st); fwrite(o, 1, fs, out); written++; }
        }
        if (!v) printf("pm_deint_tool: no telecine measured (cadence verdict %d)\n", cad);
        pm_ivtc_free(v);
        pm_comb_free(c);
        if (master && written) {   /* bit-exact against the master: a hash of every master frame, each output frame looked up */
            fclose(out);
            out = NULL;
            FILE *mf = fopen(master, "rb"), *of = fopen(a[2], "rb");
            uint64_t *mh = NULL;
            int mn = 0, cap = 0;
            while (mf && fread(buf, 1, fs, mf) == fs) {
                uint64_t hsh = 1469598103934665603ull;
                for (size_t i = 0; i < fs; i++) { hsh ^= buf[i]; hsh *= 1099511628211ull; }
                if (mn == cap) { cap = cap ? 2 * cap : 256; mh = realloc(mh, (size_t)cap * sizeof(*mh)); }
                mh[mn++] = hsh;
            }
            int exact = 0, ordered = 1, last = -1, first = -1;
            while (of && fread(buf, 1, fs, of) == fs) {
                uint64_t hsh = 1469598103934665603ull;
                for (size_t i = 0; i < fs; i++) { hsh ^= buf[i]; hsh *= 1099511628211ull; }
                int at = -1;
                for (int j = last + 1; j < mn && at < 0; j++) if (mh[j] == hsh) at = j;
                if (at >= 0) { exact++; if (first < 0) first = at; else if (at != last + 1) ordered = 0; last = at; }
            }
            printf("pm_deint_tool: %d of %d frames bit-exact against the master (film frames %d..%d, %s)\n", exact, written, first, last,
                   ordered ? "in order, none missing" : "with gaps");
            if (mf) fclose(mf);
            if (of) fclose(of);
            free(mh);
        }
    }
    fclose(in);
    if (out) fclose(out);
    printf("pm_deint_tool: %d frames in, %d frames out\n", frames, written);
    return 0;
}
