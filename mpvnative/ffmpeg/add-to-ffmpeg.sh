#!/bin/sh
# add-to-ffmpeg.sh <ffmpeg source tree>: adds vf_pushmpc to an FFmpeg tree (6.1 .. 9.1 and master), idempotent.
# Copies vf_pushmpc.c and pmeng.h into libavfilter/, registers the filter in allfilters.c (configure finds filters
# there) and the Makefile. The filter needs nothing at build time beyond libavutil; the engine module is loaded at run
# time. configure: nothing to add (--enable-filter=pushmpc only if the build uses --disable-filters).
# perl, not sed -i, so it runs the same with GNU and BSD tools.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
tree=${1:?usage: add-to-ffmpeg.sh <ffmpeg tree>}
af=$tree/libavfilter
[ -f "$af/allfilters.c" ] || { echo "add-to-ffmpeg.sh: $tree is not an FFmpeg source tree" >&2; exit 1; }
cp "$here/vf_pushmpc.c" "$af/vf_pushmpc.c"
cp "$here/../include/pmeng.h" "$af/pmeng.h"
if ! grep -q 'ff_vf_pushmpc' "$af/allfilters.c"; then
    perl -pi -e 's/^(extern const (FFFilter|AVFilter) ff_vf_pseudocolor;)\s*$/$1\nextern const $2 ff_vf_pushmpc;\n/' "$af/allfilters.c"
fi
grep -q 'ff_vf_pushmpc' "$af/allfilters.c" || { echo "add-to-ffmpeg.sh: could not register the filter in allfilters.c" >&2; exit 1; }
if ! grep -q 'CONFIG_PUSHMPC_FILTER' "$af/Makefile"; then
    perl -pi -e 's/^(OBJS-\S+CONFIG_PSEUDOCOLOR_FILTER\S+\s+\+= vf_pseudocolor\.o)\s*$/$1\nOBJS-\$(CONFIG_PUSHMPC_FILTER)              += vf_pushmpc.o\n/' "$af/Makefile"
fi
grep -q 'CONFIG_PUSHMPC_FILTER' "$af/Makefile" || { echo "add-to-ffmpeg.sh: could not add the filter to the Makefile" >&2; exit 1; }
echo "vf_pushmpc added to $tree"
