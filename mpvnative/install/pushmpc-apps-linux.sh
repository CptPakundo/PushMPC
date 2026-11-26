#!/bin/sh
# pushmpc-apps-linux.sh: PushMPC's filter (vf_pushmpc) inside Linux players that bring their own libmpv, without
# replacing anything in them. For Setup (integrations/linux/setup.sh) or by hand. POSIX sh.
#
#   pushmpc-apps-linux.sh install|uninstall|status [--from DIR] [--kit DIR] [--target auto|<name>] [--opts '...']
#                         [--dry-run] [--json]
#
#   --from   folder with pushmpc_hook.so, libpushmpc-avfilter-<ver>.so (one per FFmpeg release) and libpushmpc_engine.so
#            (Setup: the tarball's mpvnative/; default: ../mpvnative next to this script, then this folder)
#   --kit    folder with pushmpc_preload.so and pushmpc-plex-selftest (Setup: the tarball's plex/; default: here)
#   --target one of the names below, or auto (every one that is installed; default)
#   --opts   vf_pushmpc options (default none: the engine's Auto)
#   --json   one JSON object per target on stdout ({"target","action","state","detail"}), the log on stderr
#
# Targets and routes (each checked by a self-test through the player's own libmpv; a target that does not pass is
# undone again by install itself, with the reason):
#   plex-snap, plex-htpc-snap   Snap plex-desktop / plex-htpc. LD_PRELOAD cannot work there (snap-confine is setuid), so
#        the hook goes in as an mpv C plugin from Plex's own mpv.conf: files in ~/snap/<name>/common/pushmpc (a Snap may
#        map and run files from its own common folder), a marked block in ~/snap/<name>/current/.local/share/plex/mpv.conf
#        (scripts-append=<hook>). The self-test runs inside the Snap's confinement (snap run --shell).
#   stremio-flatpak             Flathub com.stremio.Stremio,
#   jellyfin-desktop-flatpak    Flathub org.jellyfin.JellyfinDesktop,
#   emby-flatpak                an Emby Theater Flatpak (any installed app id containing "emby"):
#        files in ~/.var/app/<id>/pushmpc, `flatpak override --user --env=LD_PRELOAD=<preload>
#        --env=PUSHMPC_PRELOAD_VF=auto`: the preload adds the filter after the program's mpv_initialize and puts it back
#        whenever the program replaces the filter list. Refused when the app already has another LD_PRELOAD override.
#   emby-snap                   Snap emby-theater: as Snap Plex, mpv.conf ~/snap/emby-theater/current/.config/mpv/mpv.conf
#   emby                        Emby Theater from its .deb / .rpm: its player plugin starts libmpv with config=yes, so it
#        reads ~/.config/mpv/mpv.conf; a marked block there loads the hook (files in ~/.local/share/pushmpc/emby). When
#        PushMPC's native hook block for mpv is already in that file, Emby Theater shares it and nothing more is written.
#   stremio                     Stremio 4.4 from its .deb / .rpm (system libmpv, no config file): the preload, started
#        from a marked copy of its menu entry in ~/.local/share/applications (files in ~/.local/share/pushmpc/stremio).
# Only the one libavfilter build that matches the player's FFmpeg is copied (found by a first self-test run).
# Uninstall: every block out (the file restored byte for byte when nothing else changed), our override keys out of the
# Flatpak keyfile (restored exactly), the files.
# Tests (test-apps-linux.sh): PUSHMPC_SNAP_ROOT (default /snap), PUSHMPC_EMBY_DIR, PUSHMPC_STREMIO_BIN / _LIB and
# PUSHMPC_APPS_DESKTOP_DIRS point at fake installs; PUSHMPC_PRELOAD_VAR renames LD_PRELOAD for the self-test.
# Exit: 0 done, 1 error, 2 usage, 3 nothing to do here (none of these players found) or a target not supported.
set -u

action=${1:-}; [ $# -gt 0 ] && shift
from=''; kit=''; target=auto; opts=''; dry=0; json=0
while [ $# -gt 0 ]; do
    case $1 in
        --from) from=$2; shift 2 ;;
        --kit) kit=$2; shift 2 ;;
        --target) target=$2; shift 2 ;;
        --opts) opts=$2; shift 2 ;;
        --dry-run) dry=1; shift ;;
        --json) json=1; shift ;;
        *) echo "pushmpc-apps-linux.sh: unknown option $1" >&2; exit 2 ;;
    esac
done
case $action in install|uninstall|status) ;; *) sed -n '2,14p' "$0" >&2; exit 2 ;; esac
here=$(cd "$(dirname "$0")" && pwd)
if [ -z "$from" ]; then if [ -f "$here/../mpvnative/pushmpc_hook.so" ]; then from=$here/../mpvnative; else from=$here; fi; fi
[ -n "$kit" ] || kit=$here

log() { printf '%s\n' "$*" >&2; }
jesc() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'; }
emit() { if [ "$json" = 1 ]; then printf '{"target":"%s","action":"%s","state":"%s","detail":"%s"}\n' "$1" "$action" "$2" "$(jesc "$3")"; else log "$1: $2${3:+ ($3)}"; fi; }
want() { [ "$target" = auto ] || [ "$target" = "$1" ]; }

# ---------------------------------------------------------------------------------------------- mpv.conf blocks
conf_strip() {   # conf_strip <file> <begin> <end>: the file without the block
    awk -v BINMODE=3 -v b="$2" -v e="$3" '$0 == b { s = 1; next } s && $0 == e { s = 0; next } !s' "$1"
}
conf_has() { [ -f "$1" ] && grep -q -x -F "$2" "$1"; }
conf_add() {   # conf_add <mpv.conf> <begin> <end> <body lines>
    c=$1; mkdir -p "$(dirname "$c")" || return 1
    if [ ! -f "$c.pushmpc-apps-backup" ] && [ ! -f "$c.pushmpc-apps-absent" ]; then
        if [ -f "$c" ]; then cp "$c" "$c.pushmpc-apps-backup"; else : > "$c.pushmpc-apps-absent"; fi
    fi
    { printf '%s\n%s\n%s\n' "$2" "$4" "$3"; [ ! -f "$c" ] || conf_strip "$c" "$2" "$3"; } > "$c.pushmpc-tmp" && mv "$c.pushmpc-tmp" "$c"
}
conf_remove() {   # conf_remove <mpv.conf> <begin> <end>
    c=$1; [ -f "$c" ] || { rm -f "$c.pushmpc-apps-absent"; return 0; }
    conf_strip "$c" "$2" "$3" > "$c.pushmpc-tmp" && mv "$c.pushmpc-tmp" "$c"
    if [ -f "$c.pushmpc-apps-backup" ] && cmp -s "$c" "$c.pushmpc-apps-backup"; then rm -f "$c.pushmpc-apps-backup"
    elif [ -f "$c.pushmpc-apps-absent" ] && [ ! -s "$c" ]; then rm -f "$c" "$c.pushmpc-apps-absent"
    else rm -f "$c.pushmpc-apps-backup" "$c.pushmpc-apps-absent"; fi
}
hook_body() {   # the mpv.conf lines that load the hook from folder $1
    printf 'scripts-append=%s/pushmpc_hook.so\n' "$1"
    [ -n "$opts" ] && printf 'script-opts-append=pushmpc-vf=%s\n' "$opts"
    printf 'interpolation=no'
}

# ---------------------------------------------------------------------------------------------- files and self-test
files_ok() { [ -f "$from/pushmpc_hook.so" ] && [ -f "$from/libpushmpc_engine.so" ] && [ -f "$kit/pushmpc_preload.so" ] && [ -x "$kit/pushmpc-plex-selftest" ] && ls "$from"/libpushmpc-avfilter-*.so >/dev/null 2>&1; }
copy_base() {   # copy_base <dest>: everything but the libavfilter builds
    mkdir -p "$1" && cp "$from/pushmpc_hook.so" "$from/libpushmpc_engine.so" "$kit/pushmpc_preload.so" "$1/" \
        && cp "$kit/pushmpc-plex-selftest" "$1/pushmpc-selftest" || return 1
    [ ! -f "$from/SOURCES.txt" ] || cp "$from/SOURCES.txt" "$1/"
}
remove_files() {
    rm -f "$1/pushmpc_hook.so" "$1/libpushmpc_engine.so" "$1/pushmpc_preload.so" "$1/pushmpc-selftest" "$1"/libpushmpc-avfilter-*.so \
        "$1/SOURCES.txt" "$1/override.backup" "$1/override.absent" "$1"/libmpv-host-* 2>/dev/null
    rmdir "$1" 2>/dev/null
}
# selftest <dest> <runner...>: runs <dest>/pushmpc-selftest through the runner (which adds the player's environment and
# its libmpv path); copies the libavfilter build the first run asks for and runs again. Sets st_detail; 0 active,
# 3 not active, 2 no test possible.
selftest() {
    d=$1; shift
    out=$("$@" 2>&1); r=$?
    ver=$(printf '%s\n' "$out" | sed -n "s/.*no build for this mpv's libavfilter \([0-9]*\.[0-9]*\.[0-9]*\).*/\1/p" | head -n 1)
    if [ -n "$ver" ]; then
        if [ -f "$from/libpushmpc-avfilter-$ver.so" ]; then
            cp "$from/libpushmpc-avfilter-$ver.so" "$d/" && { out=$("$@" 2>&1); r=$?; }
        else
            st_detail="no PushMPC build for this player's FFmpeg (libavfilter $ver)"; return 3
        fi
    fi
    st_detail=$(printf '%s\n' "$out" | grep -E '^host: |pushmpc_(hook|preload)\]' | sed 's/^host: //' | tr '\n' ' ' | sed 's/ *$//' | cut -c1-600)
    case $r in 0) return 0 ;; 2) return 2 ;; *) return 3 ;; esac
}

# ---------------------------------------------------------------------------------------------- Snap: C plugin
snap_has() { command -v snap >/dev/null 2>&1 && snap list "$1" >/dev/null 2>&1; }
do_snap() {   # do_snap <target> <snap> <mpv.conf below ~/snap/<snap>/current> <label>
    name=$1; sn=$2; rel=$3; label=$4
    S=${PUSHMPC_SNAP_ROOT:-/snap}/$sn/current; dest=$HOME/snap/$sn/common/pushmpc; conf=$HOME/snap/$sn/current/$rel
    bm="# >>> PushMPC ($label): do not edit between these lines (PushMPC uninstall removes them)"; em="# <<< PushMPC ($label)"
    case $action in
    status) if conf_has "$conf" "$bm" && [ -f "$dest/pushmpc_hook.so" ]; then emit "$name" installed "$conf"; else emit "$name" not-installed "snap $sn"; fi ;;
    install)
        files_ok || { emit "$name" error "the files are not in $from / $kit"; return 1; }
        lib=$(find -L "$S" -name 'libmpv.so*' 2>/dev/null | sort | head -n 1)
        [ -n "$lib" ] || { emit "$name" unsupported "no libmpv.so in snap $sn"; return 3; }
        [ "$dry" = 1 ] && { emit "$name" would-install "$dest; $conf"; return 0; }
        copy_base "$dest" || { emit "$name" error "cannot write $dest"; return 1; }
        conf_add "$conf" "$bm" "$em" "$(hook_body "$dest")" || { emit "$name" error "cannot write $conf"; return 1; }
        # inside the Snap's confinement, with Plex's own libmpv; a second try puts every library folder of the Snap on
        # the search path, for a libmpv that relies on its launcher's LD_LIBRARY_PATH
        selftest "$dest" snap run --shell "$sn" -c "PUSHMPC_HOST_TMP=\"\$SNAP_USER_COMMON\" PUSHMPC_HOST_SCRIPT='$dest/pushmpc_hook.so' '$dest/pushmpc-selftest' '$lib'"; s=$?
        if [ "$s" = 2 ]; then
            ldp=$(find -L "$S" -name '*.so*' -type f 2>/dev/null | sed 's|/[^/]*$||' | sort -u | tr '\n' ':')
            selftest "$dest" snap run --shell "$sn" -c "LD_LIBRARY_PATH='$ldp'\"\${LD_LIBRARY_PATH:-}\" PUSHMPC_HOST_TMP=\"\$SNAP_USER_COMMON\" PUSHMPC_HOST_SCRIPT='$dest/pushmpc_hook.so' '$dest/pushmpc-selftest' '$lib'"; s=$?
        fi
        if [ "$s" = 0 ]; then emit "$name" installed "snap $sn: $conf; selftest: $st_detail"; return 0; fi
        conf_remove "$conf" "$bm" "$em"; remove_files "$dest"
        emit "$name" not-active "snap $sn: undone; ${st_detail:-the self-test could not run}"; return 3 ;;
    uninstall)
        [ "$dry" = 1 ] && { emit "$name" would-uninstall "$dest"; return 0; }
        conf_remove "$conf" "$bm" "$em"; remove_files "$dest"; emit "$name" uninstalled "snap $sn" ;;
    esac
}

# ---------------------------------------------------------------------------------------------- Flatpak: preload
fp_has() { command -v flatpak >/dev/null 2>&1 && { flatpak info --user "$1" >/dev/null 2>&1 || flatpak info "$1" >/dev/null 2>&1; }; }
do_flatpak() {   # do_flatpak <target> <app id>
    name=$1; id=$2; base=$HOME/.var/app/$id; dest=$base/pushmpc; pre=$dest/pushmpc_preload.so
    ovf=${XDG_DATA_HOME:-$HOME/.local/share}/flatpak/overrides/$id
    cur=$(flatpak override --user --show "$id" 2>/dev/null | sed -n 's/^LD_PRELOAD=//p')
    case $action in
    status)
        if [ "$cur" = "$pre" ] && [ -f "$pre" ]; then emit "$name" installed "$id, LD_PRELOAD override set"
        else emit "$name" not-installed "$id${cur:+, another LD_PRELOAD override: $cur}"; fi ;;
    install)
        files_ok || { emit "$name" error "the files are not in $from / $kit"; return 1; }
        if [ -n "$cur" ] && [ "$cur" != "$pre" ]; then emit "$name" refused "$id already has an LD_PRELOAD override ($cur)"; return 1; fi
        lib=$(flatpak run --command=sh "$id" -c 'find /app /usr/lib -name "libmpv.so*" 2>/dev/null | sort | head -n 1' 2>/dev/null)
        [ -n "$lib" ] || { emit "$name" unsupported "$id: no libmpv.so in the app"; return 3; }
        [ "$dry" = 1 ] && { emit "$name" would-install "$dest"; return 0; }
        copy_base "$dest" || { emit "$name" error "cannot write $dest"; return 1; }
        selftest "$dest" flatpak run --command=sh --env=LD_PRELOAD="$pre" --env=PUSHMPC_HOST_TMP="$dest" "$id" -c "'$dest/pushmpc-selftest' '$lib'"; s=$?
        if [ "$s" != 0 ]; then remove_files "$dest"; emit "$name" not-active "$id: not changed; ${st_detail:-the self-test could not run}"; return 3; fi
        if [ ! -f "$dest/override.backup" ] && [ ! -f "$dest/override.absent" ]; then
            if [ -f "$ovf" ]; then cp "$ovf" "$dest/override.backup"; else : > "$dest/override.absent"; fi
        fi
        vfo=auto; [ -n "$opts" ] && vfo=$opts
        flatpak override --user --env=LD_PRELOAD="$pre" --env=PUSHMPC_PRELOAD_VF="$vfo" "$id" || { emit "$name" error "flatpak override failed"; remove_files "$dest"; return 1; }
        emit "$name" installed "$id: LD_PRELOAD=$pre; selftest: $st_detail" ;;
    uninstall)
        [ "$dry" = 1 ] && { emit "$name" would-uninstall "$dest"; return 0; }
        if [ "$cur" = "$pre" ] && [ -f "$ovf" ]; then
            # our keys out of the keyfile (flatpak --unset-env would write "LD_PRELOAD=" plus unset-environment instead),
            # then the keyfile as it was before install when nothing else changed meanwhile
            grep -v -x -F -e "LD_PRELOAD=$pre" "$ovf" | grep -v '^PUSHMPC_PRELOAD_VF=' > "$ovf.pushmpc-tmp"; mv "$ovf.pushmpc-tmp" "$ovf"
            keys() { grep -v -e "^\[" -e "^$" "$1" 2>/dev/null; }
            if [ -f "$dest/override.backup" ] && [ "$(keys "$ovf")" = "$(keys "$dest/override.backup")" ]; then cp "$dest/override.backup" "$ovf"
            elif [ -f "$dest/override.absent" ] && [ -z "$(keys "$ovf")" ]; then rm -f "$ovf"; fi
        fi
        remove_files "$dest"; emit "$name" uninstalled "$id" ;;
    esac
}

# ---------------------------------------------------------------------------------------------- Emby Theater (.deb / .rpm)
emby_dir() { for d in ${PUSHMPC_EMBY_DIR:-} /opt/emby-theater /usr/lib/emby-theater /usr/share/emby-theater /opt/Emby\ Theater; do [ -d "$d" ] && { printf '%s\n' "$d"; return 0; }; done; return 1; }
do_emby() {
    E=$(emby_dir) || return 0
    cfg=${XDG_CONFIG_HOME:-}; case $cfg in /*) ;; *) cfg=$HOME/.config ;; esac
    conf=$cfg/mpv/mpv.conf; dest=${XDG_DATA_HOME:-$HOME/.local/share}/pushmpc/emby
    bm='# >>> PushMPC (Emby Theater): do not edit between these lines (PushMPC uninstall removes them)'; em='# <<< PushMPC (Emby Theater)'
    native='# >>> PushMPC native: do not edit between these lines (PushMPC uninstall removes them)'
    case $action in
    status) if conf_has "$conf" "$bm"; then emit emby installed "$conf"; elif conf_has "$conf" "$native"; then emit emby installed "shares the native hook block in $conf"; else emit emby not-installed "$E"; fi ;;
    install)
        files_ok || { emit emby error "the files are not in $from / $kit"; return 1; }
        node=$(find "$E" -name 'mpv-linux-*.node' 2>/dev/null | head -n 1)
        lib=''
        [ -n "$node" ] && lib=$(ldd "$node" 2>/dev/null | sed -n 's/.*libmpv\.so[^ ]* => \([^ ]*\).*/\1/p' | head -n 1)
        [ -n "$lib" ] || lib=$(find "$E" -name 'libmpv.so*' 2>/dev/null | sort | head -n 1)
        [ -n "$lib" ] || { emit emby unsupported "$E: no libmpv found for its player plugin"; return 3; }
        [ "$dry" = 1 ] && { emit emby would-install "$conf"; return 0; }
        if conf_has "$conf" "$native"; then
            hd=${XDG_DATA_HOME:-$HOME/.local/share}/pushmpc/mpvnative
            selftest "$hd" env PUSHMPC_HOST_SCRIPT="$hd/pushmpc_hook.so" "$kit/pushmpc-plex-selftest" "$lib"; s=$?
            [ "$s" = 0 ] && { emit emby installed "shares the native hook block in $conf; selftest: $st_detail"; return 0; }
            emit emby not-active "the native hook block in $conf does not run with Emby Theater's libmpv ($lib): ${st_detail:-no self-test}"; return 3
        fi
        copy_base "$dest" || { emit emby error "cannot write $dest"; return 1; }
        selftest "$dest" env PUSHMPC_HOST_SCRIPT="$dest/pushmpc_hook.so" "$dest/pushmpc-selftest" "$lib"; s=$?
        if [ "$s" != 0 ]; then remove_files "$dest"; emit emby not-active "not changed ($lib): ${st_detail:-the self-test could not run}"; return 3; fi
        conf_add "$conf" "$bm" "$em" "$(hook_body "$dest")" || { emit emby error "cannot write $conf"; return 1; }
        emit emby installed "$conf ($lib); selftest: $st_detail" ;;
    uninstall)
        [ "$dry" = 1 ] && { emit emby would-uninstall "$conf"; return 0; }
        conf_remove "$conf" "$bm" "$em"; remove_files "$dest"; emit emby uninstalled "$E" ;;
    esac
}

# ---------------------------------------------------------------------------------------------- Stremio 4.4 (.deb / .rpm)
# Its shell starts libmpv with no config file, linked against the system's libmpv: the preload (PUSHMPC_PRELOAD_VF
# adds the filter after mpv_initialize), started from a copy of Stremio's menu entry in ~/.local/share/applications
# (the user's own entry of the same name wins over the system one, and goes again on uninstall).
stremio_bin() { for b in ${PUSHMPC_STREMIO_BIN:-} /opt/stremio/stremio "$(command -v stremio 2>/dev/null)"; do [ -n "$b" ] && [ -f "$b" ] && { printf '%s\n' "$b"; return 0; }; done; return 1; }
do_stremio() {
    B=$(stremio_bin) || return 0
    data=${XDG_DATA_HOME:-}; case $data in /*) ;; *) data=$HOME/.local/share ;; esac
    dest=$data/pushmpc/stremio; pre=$dest/pushmpc_preload.so
    src=''
    for d in ${PUSHMPC_APPS_DESKTOP_DIRS:-/usr/share/applications /usr/local/share/applications}; do
        for f in "$d"/*.desktop; do [ -f "$f" ] && grep -q -i '^Exec=.*stremio' "$f" && { src=$f; break 2; }; done
    done
    dst=$data/applications/$(basename "${src:-stremio.desktop}")
    case $action in
    status) if [ -f "$dst" ] && grep -q '^X-PushMPC=' "$dst"; then emit stremio installed "$dst"; else emit stremio not-installed "$B"; fi ;;
    install)
        files_ok || { emit stremio error "the files are not in $from / $kit"; return 1; }
        [ -n "$src" ] || { emit stremio unsupported "$B: no menu entry for Stremio found"; return 3; }
        if [ -f "$dst" ] && ! grep -q '^X-PushMPC=' "$dst"; then emit stremio refused "$dst is your own menu entry for Stremio"; return 1; fi
        lib=$(ldd "$B" 2>/dev/null | sed -n 's/.*libmpv\.so[^ ]* => \([^ ]*\).*/\1/p' | head -n 1)
        [ -n "$lib" ] || lib=${PUSHMPC_STREMIO_LIB:-}
        [ -n "$lib" ] || { emit stremio unsupported "$B does not link libmpv (not the Stremio 4 shell)"; return 3; }
        [ "$dry" = 1 ] && { emit stremio would-install "$dst"; return 0; }
        copy_base "$dest" || { emit stremio error "cannot write $dest"; return 1; }
        selftest "$dest" env "${PUSHMPC_PRELOAD_VAR:-LD_PRELOAD}=$pre" "$dest/pushmpc-selftest" "$lib"; s=$?
        if [ "$s" != 0 ]; then remove_files "$dest"; emit stremio not-active "not changed ($lib): ${st_detail:-the self-test could not run}"; return 3; fi
        vfo=auto; [ -n "$opts" ] && vfo=$opts
        mkdir -p "$(dirname "$dst")"
        awk -v p="env LD_PRELOAD=$pre PUSHMPC_PRELOAD_VF=$vfo " '/^Exec=/ { sub(/^Exec=/, "Exec=" p) } { print } /^\[Desktop Entry\]/ { print "X-PushMPC=PushMPC uninstall removes this copy of the menu entry" }' "$src" > "$dst" \
            || { remove_files "$dest"; emit stremio error "cannot write $dst"; return 1; }
        emit stremio installed "$dst ($lib); selftest: $st_detail" ;;
    uninstall)
        [ "$dry" = 1 ] && { emit stremio would-uninstall "$dst"; return 0; }
        if [ -f "$dst" ] && grep -q '^X-PushMPC=' "$dst"; then rm -f "$dst"; fi
        remove_files "$dest"; emit stremio uninstalled "$B" ;;
    esac
}

rc=0; found=0
run_t() { found=1; "$@" || { r=$?; [ "$r" = 3 ] && [ "$rc" = 0 ] && rc=3; [ "$r" = 1 ] && rc=1; }; return 0; }
want plex-snap && snap_has plex-desktop && run_t do_snap plex-snap plex-desktop .local/share/plex/mpv.conf 'Plex Snap'
want plex-htpc-snap && snap_has plex-htpc && run_t do_snap plex-htpc-snap plex-htpc .local/share/plex/mpv.conf 'Plex HTPC Snap'
want emby-snap && snap_has emby-theater && run_t do_snap emby-snap emby-theater .config/mpv/mpv.conf 'Emby Theater Snap'
want stremio-flatpak && fp_has com.stremio.Stremio && run_t do_flatpak stremio-flatpak com.stremio.Stremio
want jellyfin-desktop-flatpak && fp_has org.jellyfin.JellyfinDesktop && run_t do_flatpak jellyfin-desktop-flatpak org.jellyfin.JellyfinDesktop
if want emby-flatpak && command -v flatpak >/dev/null 2>&1; then
    for id in $(flatpak list --app --columns=application 2>/dev/null | grep -i emby); do run_t do_flatpak emby-flatpak "$id"; done
fi
want emby && emby_dir >/dev/null && run_t do_emby
want stremio && stremio_bin >/dev/null && run_t do_stremio
[ "$found" = 1 ] || { emit "$target" unsupported "none of these players is installed"; exit 3; }
exit $rc
