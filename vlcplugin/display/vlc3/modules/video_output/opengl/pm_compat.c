/* pm_compat.c: see pm_compat.h. GPL-3.0-or-later. */
#include <ctype.h>
#include <math.h>
#include <stddef.h>

int pm_strverscmp(const char *a, const char *b);
void pm_sincosf(float x, float *s, float *c);

void pm_sincosf(float x, float *s, float *c)
{
    *s = sinf(x);
    *c = cosf(x);
}

/* version order: runs of digits compare by value (with GNU's rule that a run with more leading zeros is smaller,
 * as a fractional part), everything else byte by byte */
int pm_strverscmp(const char *a, const char *b)
{
    const unsigned char *p = (const unsigned char *)a, *q = (const unsigned char *)b;
    while (*p && *q) {
        if (isdigit(*p) && isdigit(*q)) {
            const unsigned char *p0 = p, *q0 = q;
            size_t zp = 0, zq = 0;
            while (*p == '0') { p++; zp++; }
            while (*q == '0') { q++; zq++; }
            if (zp != zq && (zp || zq)) return zp > zq ? -1 : 1;   /* more leading zeros sorts first */
            const unsigned char *ps = p, *qs = q;
            while (isdigit(*p)) p++;
            while (isdigit(*q)) q++;
            const size_t lp = (size_t)(p - ps), lq = (size_t)(q - qs);
            if (zp == 0 && lp != lq) return lp < lq ? -1 : 1;     /* integers: the longer is larger */
            for (size_t i = 0; i < lp && i < lq; i++)
                if (ps[i] != qs[i]) return ps[i] < qs[i] ? -1 : 1;
            if (lp != lq) return lp < lq ? -1 : 1;
            (void)p0; (void)q0;
            continue;
        }
        if (*p != *q) return *p < *q ? -1 : 1;
        p++; q++;
    }
    return (*p > *q) - (*p < *q);
}
