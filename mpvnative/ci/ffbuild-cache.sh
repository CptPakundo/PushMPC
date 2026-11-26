# ffbuild-cache.sh (sourced): incremental FFmpeg builds across CI runs. The configured and built state of an FFmpeg tree
# (every file git does not track: config.h, ffbuild/config.mak, the objects, the libraries) is kept per tag in a cache
# folder as <label>.tar. The next run checks the same commit out, puts that state back and marks it newer than the
# checkout, so make only redoes what changed since: vf_pushmpc.c (copied in again after the restore) and the link.
# configure runs again only when its options or the commit differ (the stamp file .pm-configure).
#   ffb_restore <tree> <label> <cache dir> <stamp>  after the checkout: clean the tree, restore <label>'s state;
#                                                   returns 0 when the stamp matches (configure can be skipped)
#   ffb_store   <tree> <label> <cache dir> <stamp>  after the build: write the stamp, keep the tree's state
# Out-of-tree builds (a separate build folder): ffb_restore_dir / ffb_store_dir <build dir> <label> <cache dir> <stamp>.
# An empty <cache dir> turns both off (a tag build compiles everything fresh).

ffb_restore() {
    _t=$1; _l=$2; _c=$3; _s=$4
    git -C "$_t" clean -fdxq
    [ -n "$_c" ] && [ -f "$_c/$_l.tar" ] || return 1
    tar -xf "$_c/$_l.tar" -C "$_t" || { git -C "$_t" clean -fdxq; return 1; }
    # newer than the checkout: make keeps every object whose sources did not change
    (cd "$_t" && git ls-files -o -z | xargs -0 touch -c)
    if [ "$(cat "$_t/.pm-configure" 2>/dev/null)" = "$_s" ]; then echo "ffbuild-cache: $_l restored (configure skipped)"; return 0; fi
    echo "ffbuild-cache: $_l restored, but configured differently: configure runs"
    return 1
}

ffb_store() {
    _t=$1; _l=$2; _c=$3; _s=$4
    [ -n "$_c" ] || return 0
    mkdir -p "$_c"
    printf '%s' "$_s" > "$_t/.pm-configure"
    (cd "$_t" && git ls-files -o -z | tar --null -T - -cf "$_c/$_l.tar")
}

ffb_restore_dir() {
    _b=$1; _l=$2; _c=$3; _s=$4
    rm -rf "$_b"; mkdir -p "$_b"
    [ -n "$_c" ] && [ -f "$_c/$_l.tar" ] || return 1
    tar -xf "$_c/$_l.tar" -C "$_b" || { rm -rf "$_b"; mkdir -p "$_b"; return 1; }
    find "$_b" -type f -exec touch -c {} +
    if [ "$(cat "$_b/.pm-configure" 2>/dev/null)" = "$_s" ]; then echo "ffbuild-cache: $_l restored (configure skipped)"; return 0; fi
    echo "ffbuild-cache: $_l restored, but configured differently: configure runs"
    rm -rf "$_b"; mkdir -p "$_b"
    return 1
}

ffb_store_dir() {
    _b=$1; _l=$2; _c=$3; _s=$4
    [ -n "$_c" ] || return 0
    mkdir -p "$_c"
    printf '%s' "$_s" > "$_b/.pm-configure"
    tar -cf "$_c/$_l.tar" -C "$_b" .
}
