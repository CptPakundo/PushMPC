/* pm_compat.h: what VLC's own build takes from its compat layer (compat/, vlc_fixups.h) for the OpenGL output on systems
 * whose C library lacks it (macOS): strverscmp and sincosf. Force-included by the macOS fork's build. GPL-3.0-or-later. */
#ifndef PM_COMPAT_H
#define PM_COMPAT_H
#ifdef __cplusplus
extern "C" {
#endif
int pm_strverscmp(const char *a, const char *b);
void pm_sincosf(float x, float *s, float *c);
#ifdef __cplusplus
}
#endif
#define strverscmp pm_strverscmp
#define sincosf pm_sincosf
#endif
