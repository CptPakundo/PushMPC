#!/bin/sh
# pushmpc-plex-linux.sh: vf_pushmpc inside Plex desktop / Plex HTPC on Linux, without replacing anything in Plex.
# Plex bundles libmpv and FFmpeg 5.x as separate libraries (libavfilter.so.8); pushmpc_preload.so, loaded through
# LD_PRELOAD, defines avfilter_get_by_name ahead of Plex's libavfilter and adds the "pushmpc" filter from a matching
# build (libpushmpc-avfilter-<ver>.so). For Setup (integrations/linux/setup.sh) or by hand. POSIX sh.
#
#   pushmpc-plex-linux.sh install|uninstall|status [--from DIR] [--target auto|desktop|htpc|tarball]
#                         [--tarball-dir DIR] [--opts 'smoother=4:...'] [--dry-run] [--json]
#
#   --from     folder with pushmpc_preload.so, libpushmpc-avfilter-*.so and libpushmpc_engine.so (default: here)
#   --target   desktop  Flathub tv.plex.PlexDesktop    htpc  Flathub tv.plex.PlexHTPC
#              tarball  Plex's official Linux tarball in --tarball-dir (where Plex.sh is)
#              auto     every Flatpak one installed, plus the tarball when --tarball-dir is given (default)
#   --opts     vf_pushmpc options for the mpv.conf line (default none: the engine's Auto)
#   --json     one JSON object per target on stdout ({"target","action","state","detail"}), the log on stderr
# Flatpak: files in ~/.var/app/<id>/pushmpc (the sandbox always sees them), `flatpak override --user
#   --env=LD_PRELOAD=...` (refused when another LD_PRELOAD override is set), a marked block in
#   ~/.var/app/<id>/data/plex/mpv.conf. Uninstall: our LD_PRELOAD key out of the override keyfile (restored exactly), the files, the block (exact restore).
# Tarball: files in <dir>/pushmpc and a launcher ~/.local/bin/plex-pushmpc (LD_PRELOAD + Plex.sh); mpv.conf in
#   ~/.local/share/plex/mpv.conf.
# Snap Plex is not supported: snap-confine is setuid, and glibc drops LD_PRELOAD for it.
# Exit: 0 done, 1 error, 2 usage, 3 not supported here (no Plex found).
set -u

BEGIN_MARK='# >>> PushMPC native (Plex): do not edit between these lines (PushMPC uninstall removes them)'
END_MARK='# <<< PushMPC native (Plex)'

action=${1:-}; [ $# -gt 0 ] && shift
from=''; target=auto; tdir=''; opts=''; dry=0; json=0
while [ $# -gt 0 ]; do
    case $1 in
        --from) from=$2; shift 2 ;;
        --target) target=$2; shift 2 ;;
        --tarball-dir) tdir=$2; shift 2 ;;
        --opts) opts=$2; shift 2 ;;
        --dry-run) dry=1; shift ;;
        --json) json=1; shift ;;
        *) echo "pushmpc-plex-linux.sh: unknown option $1" >&2; exit 2 ;;
    esac
done
case $action in install|uninstall|status) ;; *) sed -n '2,12p' "$0" >&2; exit 2 ;; esac
case $target in auto|desktop|htpc|tarball) ;; *) echo "pushmpc-plex-linux.sh: bad --target" >&2; exit 2 ;; esac
here=$(cd "$(dirname "$0")" && pwd)
[ -n "$from" ] || from=$here

log() { printf '%s\n' "$*" >&2; }
jesc() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'; }
emit() { if [ "$json" = 1 ]; then printf '{"target":"%s","action":"%s","state":"%s","detail":"%s"}\n' "$1" "$action" "$2" "$(jesc "$3")"; else log "$1: $2${3:+ ($3)}"; fi; }

conf_add() {   # conf_add <mpv.conf>
    c=$1; mkdir -p "$(dirname "$c")"
    if [ ! -f "$c.pushmpc-native-backup" ] && [ ! -f "$c.pushmpc-native-absent" ]; then
        if [ -f "$c" ]; then cp "$c" "$c.pushmpc-native-backup"; else : > "$c.pushmpc-native-absent"; fi
    fi
    vf='vf-append=@pushmpc:pushmpc'; [ -n "$opts" ] && vf="$vf=$opts"
    { printf '%s\n%s\ninterpolation=no\n%s\n' "$BEGIN_MARK" "$vf" "$END_MARK"; [ ! -f "$c" ] || conf_strip "$c"; } > "$c.tmp" && mv "$c.tmp" "$c"
}
conf_strip() {   # the file without the block
    awk -v b="$BEGIN_MARK" -v e="$END_MARK" '$0 == b { s = 1; next } s && $0 == e { s = 0; next } !s' "$1"
}
conf_remove() {
    c=$1; [ -f "$c" ] || { rm -f "$c.pushmpc-native-absent"; return 0; }
    conf_strip "$c" > "$c.tmp" && mv "$c.tmp" "$c"
    if [ -f "$c.pushmpc-native-backup" ] && cmp -s "$c" "$c.pushmpc-native-backup"; then rm -f "$c.pushmpc-native-backup"
    elif [ -f "$c.pushmpc-native-absent" ] && [ ! -s "$c" ]; then rm -f "$c" "$c.pushmpc-native-absent"
    else rm -f "$c.pushmpc-native-backup" "$c.pushmpc-native-absent"; fi
}
files_ok() { [ -f "$from/pushmpc_preload.so" ] && [ -f "$from/libpushmpc_engine.so" ] && ls "$from"/libpushmpc-avfilter-8.*.so >/dev/null 2>&1; }
copy_files() {
    mkdir -p "$1" && cp "$from/pushmpc_preload.so" "$from/libpushmpc_engine.so" "$from"/libpushmpc-avfilter-*.so "$1/" || return 1
    for x in SOURCES.txt pushmpc-plex-selftest; do [ ! -f "$from/$x" ] || cp "$from/$x" "$1/"; done
}
remove_files() { rm -f "$1/pushmpc_preload.so" "$1/libpushmpc_engine.so" "$1"/libpushmpc-avfilter-*.so "$1/SOURCES.txt" "$1/override.backup" "$1/override.absent" "$1/pushmpc-plex-selftest"; rmdir "$1" 2>/dev/null; }
# selftest <dir> <libmpv path> <runner...>: Plex's own libmpv plays a generated Y4M with vf=pushmpc through the
# preload (pushmpc-plex-selftest = mpvnative/hook/libmpv_host.c). Sets st_detail; returns 0 active, 3 not, 2 no test.
selftest() {
    d=$1; lib=$2; shift 2
    [ -x "$d/pushmpc-plex-selftest" ] || { st_detail="no self-test (pushmpc-plex-selftest not shipped)"; return 2; }
    [ -n "$lib" ] || { st_detail="no libmpv.so found in Plex"; return 2; }
    out=$("$@" "$d/pushmpc-plex-selftest" "$lib" 2>&1); r=$?
    st_detail=$(printf '%s\n' "$out" | sed -n 's/^host: //p' | tr '\n' ' ' | sed 's/ *$//')
    case $r in 0) return 0 ;; 2) st_detail="self-test could not use Plex's libmpv: $st_detail"; return 2 ;; *) return 3 ;; esac
}
report_install() {   # report_install <name> <rc of selftest> <where>
    case $2 in
        0) emit "$1" installed "$3; selftest: $st_detail" ;;
        3) emit "$1" installed-inactive "$3; selftest: $st_detail"; return 3 ;;
        *) emit "$1" installed "$3; $st_detail" ;;
    esac
}

do_flatpak() {   # do_flatpak <target name> <app id>
    name=$1; id=$2; base=$HOME/.var/app/$id; dest=$base/pushmpc; conf=$base/data/plex/mpv.conf
    cur=$(flatpak override --user --show "$id" 2>/dev/null | sed -n 's/^LD_PRELOAD=//p')
    case $action in
    status)
        if [ "$cur" = "$dest/pushmpc_preload.so" ] && [ -f "$dest/pushmpc_preload.so" ]; then emit "$name" installed "$id, LD_PRELOAD override set"
        else emit "$name" not-installed "$id${cur:+, another LD_PRELOAD override: $cur}"; fi ;;
    install)
        files_ok || { emit "$name" error "the files are not in $from"; return 1; }
        if [ -n "$cur" ] && [ "$cur" != "$dest/pushmpc_preload.so" ]; then emit "$name" refused "$id already has an LD_PRELOAD override ($cur)"; return 1; fi
        [ "$dry" = 1 ] && { emit "$name" would-install "$dest"; return 0; }
        copy_files "$dest" || { emit "$name" error "cannot write $dest"; return 1; }
        ovf=${XDG_DATA_HOME:-$HOME/.local/share}/flatpak/overrides/$id
        if [ ! -f "$dest/override.backup" ] && [ ! -f "$dest/override.absent" ]; then
            if [ -f "$ovf" ]; then cp "$ovf" "$dest/override.backup"; else : > "$dest/override.absent"; fi
        fi
        flatpak override --user --env=LD_PRELOAD="$dest/pushmpc_preload.so" "$id" || { emit "$name" error "flatpak override failed"; return 1; }
        conf_add "$conf"
        lib=$(flatpak run --command=sh "$id" -c 'find /app -name "libmpv.so*" 2>/dev/null | sort | head -1' 2>/dev/null)
        selftest "$dest" "$lib" flatpak run --command=sh "$id" -c 'exec "$@"' sh; s=$?
        report_install "$name" $s "$id: LD_PRELOAD=$dest/pushmpc_preload.so; $conf" ;;
    uninstall)
        [ "$dry" = 1 ] && { emit "$name" would-uninstall "$dest"; return 0; }
        ovf=${XDG_DATA_HOME:-$HOME/.local/share}/flatpak/overrides/$id
        if [ "$cur" = "$dest/pushmpc_preload.so" ]; then
            # our key out of the keyfile (flatpak --unset-env would write "LD_PRELOAD=" plus unset-environment instead),
            # then the keyfile as it was before install when nothing else changed meanwhile
            grep -vxF "LD_PRELOAD=$dest/pushmpc_preload.so" "$ovf" > "$ovf.pushmpc-tmp"; mv "$ovf.pushmpc-tmp" "$ovf"
            keys() { grep -v -e "^\[" -e "^$" "$1" 2>/dev/null; }
            if [ -f "$dest/override.backup" ] && [ "$(keys "$ovf")" = "$(keys "$dest/override.backup")" ]; then cp "$dest/override.backup" "$ovf"
            elif [ -f "$dest/override.absent" ] && [ -z "$(keys "$ovf")" ]; then rm -f "$ovf"; fi
        fi
        remove_files "$dest"; conf_remove "$conf"
        emit "$name" uninstalled "$id" ;;
    esac
}

do_tarball() {
    [ -n "$tdir" ] && [ -f "$tdir/Plex.sh" ] || { emit tarball unsupported "no Plex.sh in '${tdir}' (pass --tarball-dir)"; return 3; }
    dest=$tdir/pushmpc; launcher=$HOME/.local/bin/plex-pushmpc; conf=$HOME/.local/share/plex/mpv.conf
    case $action in
    status) if [ -f "$dest/pushmpc_preload.so" ] && [ -f "$launcher" ]; then emit tarball installed "$launcher"; else emit tarball not-installed "$tdir"; fi ;;
    install)
        files_ok || { emit tarball error "the files are not in $from"; return 1; }
        [ "$dry" = 1 ] && { emit tarball would-install "$dest"; return 0; }
        copy_files "$dest" || { emit tarball error "cannot write $dest"; return 1; }
        mkdir -p "$(dirname "$launcher")"
        printf '#!/bin/sh\n# PushMPC: Plex with vf_pushmpc (pushmpc-plex-linux.sh uninstall removes this)\nLD_PRELOAD="%s${LD_PRELOAD:+:$LD_PRELOAD}" exec "%s" "$@"\n' "$dest/pushmpc_preload.so" "$tdir/Plex.sh" > "$launcher"
        chmod +x "$launcher"; conf_add "$conf"
        lib=$(find "$tdir" -name "libmpv.so*" 2>/dev/null | sort | head -1)
        selftest "$dest" "$lib" env LD_PRELOAD="$dest/pushmpc_preload.so"; s=$?
        report_install tarball $s "launcher $launcher; $conf" ;;
    uninstall)
        [ "$dry" = 1 ] && { emit tarball would-uninstall "$dest"; return 0; }
        remove_files "$dest"; rm -f "$launcher"; conf_remove "$conf"
        emit tarball uninstalled "$tdir" ;;
    esac
}

rc=0; found=0
if command -v flatpak >/dev/null 2>&1; then
    for t in desktop:tv.plex.PlexDesktop htpc:tv.plex.PlexHTPC; do
        n=${t%%:*}; id=${t#*:}
        [ "$target" = auto ] || [ "$target" = "$n" ] || continue
        flatpak info --user "$id" >/dev/null 2>&1 || flatpak info "$id" >/dev/null 2>&1 || continue
        found=1; do_flatpak "$n" "$id" || rc=$?
    done
fi
if [ "$target" = tarball ] || { [ "$target" = auto ] && [ -n "$tdir" ]; }; then found=1; do_tarball || rc=$?; fi
[ "$found" = 1 ] || { emit "$target" unsupported "no Plex Flatpak installed (Snap Plex cannot take LD_PRELOAD)"; exit 3; }
exit $rc
