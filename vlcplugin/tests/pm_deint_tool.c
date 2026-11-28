/* pm_deint_tool <in.yuv> <out.yuv> <width> <height> [tff 1|0]: the module's deinterlacer (pm_deint.h) on raw 8-bit
 * yuv420p woven frames, two progressive frames out per frame in (the last frame's fields are not written: the
 * deinterlacer looks one frame ahead). vlc-plugin.yml scores the output against the 60p master. */
#include <stdio.h>
#include <stdlib.h>
#include "pm_deint.h"

int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: pm_deint_tool <in.yuv> <out.yuv> <w> <h> [tff]\n"); return 2; }
    const int w = atoi(argv[3]), h = atoi(argv[4]), tff = argc > 5 ? atoi(argv[5]) : 1;
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");
    if (!in || !out) return 2;
    const size_t ys = (size_t)w * h, cs = ys / 4, fs = ys + 2 * cs;
    uint8_t *buf = malloc(fs), *o = malloc(fs);
    pm_deint *d = pm_deint_new(w, h, 8);
    if (!buf || !o || !d) return 2;
    const uint8_t *pl[3] = { buf, buf + ys, buf + ys + cs };
    uint8_t *ol[3] = { o, o + ys, o + ys + cs };
    const ptrdiff_t st[3] = { w, w / 2, w / 2 };
    int frames = 0, written = 0;
    while (fread(buf, 1, fs, in) == fs) {
        frames++;
        if (pm_deint_push(d, pl, st, tff) == 2)
            for (int f = 0; f < 2; f++) { pm_deint_get(d, f, ol, st); fwrite(o, 1, fs, out); written++; }
    }
    pm_deint_free(d);
    fclose(in); fclose(out);
    printf("pm_deint_tool: %d frames in, %d progressive frames out\n", frames, written);
    return 0;
}
