/* pushmpc_preload.so: the hook as an LD_PRELOAD library (no mpv C plugin, no GOT rebinding): it defines
 * avfilter_get_by_name ahead of libavfilter, for programs that bundle libmpv and FFmpeg as separate libraries and that
 * PushMPC cannot reach with a cplugin (Plex desktop / HTPC on Linux: Flatpak tv.plex.PlexDesktop, the official tarball).
 * Same files next to it as the hook (libpushmpc-avfilter-<ver>.so, libpushmpc_engine.so). */
#define PUSHMPC_PRELOAD 1
#include "pushmpc_hook.c"
