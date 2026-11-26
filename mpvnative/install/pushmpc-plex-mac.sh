#!/bin/sh
# pushmpc-plex-mac.sh: vf_pushmpc inside Plex for Mac and Plex HTPC for Mac, without replacing their libmpv. Their
# libmpv.2.dylib links FFmpeg 5.x as separate dylibs; this puts a proxy libavfilter.8.dylib into the app's
# Contents/Frameworks (every symbol re-exported from the original, renamed libavfilter.8-plex.dylib) that adds the
# "pushmpc" filter, plus the per-release filter build and the engine module, and a marked block in Plex's mpv.conf.
# Plex's signature allows it (hardened runtime with disable-library-validation). For Setup or by hand. POSIX sh.
#
#   pushmpc-plex-mac.sh install|uninstall|status [--app auto|desktop|htpc] [--app-path PATH] [--from DIR]
#                       [--opts 'smoother=4:...'] [--no-conf] [--dry-run] [--json]
#
#   --app       desktop = /Applications/Plex.app, htpc = /Applications/Plex HTPC.app, auto = each one there (default)
#   --app-path  the .app bundle when it is elsewhere (needs --app desktop or htpc)
#   --from      folder with libavfilter.8.dylib (the proxy), pushmpc-avfilter-8.*.dylib, libpushmpc_engine.dylib and
#               pushmpc-plex-selftest (default: here)
#   --json      one JSON object per app on stdout ({"target","action","state","detail"}), the log on stderr
# install backs up the original (~/Library/Application Support/PushMPC/plex-native/<app>, with its SHA-256), runs a
# self-test with that Plex's own libmpv and is idempotent (an installed proxy keeps; the conf block is made sure of).
# uninstall puts the original back (checked against the SHA-256) and removes exactly what was added. A Plex update
# replaces the app and with it the proxy: status says needs-reapply, install puts it back.
# If macOS refuses writing into the app (App Management), allow the terminal (or Setup) in System Settings > Privacy &
# Security > App Management. Exit: 0 done, 1 error, 2 usage, 3 not supported here / self-test failed.
set -u

BEGIN_MARK='# >>> PushMPC native (Plex): do not edit between these lines (PushMPC uninstall removes them)'
END_MARK='# <<< PushMPC native (Plex)'

action=${1:-}; [ $# -gt 0 ] && shift
app=auto; apath=''; from=''; opts=''; noconf=0; dry=0; json=0
while [ $# -gt 0 ]; do
    case $1 in
        --app) app=$2; shift 2 ;;
        --app-path) apath=$2; shift 2 ;;
        --from) from=$2; shift 2 ;;
        --opts) opts=$2; shift 2 ;;
        --no-conf) noconf=1; shift ;;
        --dry-run) dry=1; shift ;;
        --json) json=1; shift ;;
        *) echo "pushmpc-plex-mac.sh: unknown option $1" >&2; exit 2 ;;
    esac
done
case $action in install|uninstall|status) ;; *) sed -n '2,15p' "$0" >&2; exit 2 ;; esac
case $app in auto|desktop|htpc) ;; *) echo "pushmpc-plex-mac.sh: bad --app" >&2; exit 2 ;; esac
[ -z "$apath" ] || [ "$app" != auto ] || { echo "pushmpc-plex-mac.sh: --app-path needs --app desktop or htpc" >&2; exit 2; }
here=$(cd "$(dirname "$0")" && pwd)
[ -n "$from" ] || from=$here

log() { printf '%s\n' "$*" >&2; }
jesc() { printf '%s' "$1" | tr '\n' ' ' | sed 's/\\/\\\\/g; s/"/\\"/g'; }
emit() { if [ "$json" = 1 ]; then printf '{"target":"%s","action":"%s","state":"%s","detail":"%s"}\n' "$1" "$action" "$2" "$(jesc "$3")"; else log "$1: $2${3:+ ($3)}"; fi; }
sha() { [ -f "$1" ] && shasum -a 256 "$1" | awk '{print $1}'; }

conf_strip() { awk -v b="$BEGIN_MARK" -v e="$END_MARK" '$0 == b { s = 1; next } s && $0 == e { s = 0; next } !s' "$1"; }
conf_add() {   # conf_add <mpv.conf>: the block on top (replaced when present)
    [ "$noconf" = 1 ] && return 0
    c=$1; mkdir -p "$(dirname "$c")"
    if [ ! -f "$c.pushmpc-native-backup" ] && [ ! -f "$c.pushmpc-native-absent" ]; then
        if [ -f "$c" ]; then cp "$c" "$c.pushmpc-native-backup"; else : > "$c.pushmpc-native-absent"; fi
    fi
    vf='vf-append=@pushmpc:pushmpc'; [ -n "$opts" ] && vf="$vf=$opts"
    { printf '%s\n%s\ninterpolation=no\n%s\n' "$BEGIN_MARK" "$vf" "$END_MARK"; [ ! -f "$c" ] || conf_strip "$c"; } > "$c.tmp" && mv "$c.tmp" "$c"
}
conf_remove() {
    c=$1
    if [ -f "$c" ]; then
        conf_strip "$c" > "$c.tmp" && mv "$c.tmp" "$c"
        if [ -f "$c.pushmpc-native-backup" ] && cmp -s "$c" "$c.pushmpc-native-backup"; then :
        elif [ -f "$c.pushmpc-native-absent" ] && [ ! -s "$c" ]; then rm -f "$c"; fi
    fi
    rm -f "$c.pushmpc-native-backup" "$c.pushmpc-native-absent"
}

st_detail=''
selftest() {   # selftest <Frameworks dir>: 0 active, 3 not, 2 no test possible
    [ -x "$from/pushmpc-plex-selftest" ] || { st_detail='no self-test (pushmpc-plex-selftest not shipped)'; return 2; }
    out=$("$from/pushmpc-plex-selftest" "$1/libmpv.2.dylib" 2>&1); r=$?
    st_detail=$(printf '%s\n' "$out" | sed -n 's/^host: //p' | tr '\n' ' ' | sed 's/ *$//')
    case $r in 0) return 0 ;; 2) st_detail="self-test could not use Plex's libmpv: $st_detail"; return 2 ;; *) return 3 ;; esac
}
report() {   # report <name> <selftest rc> <where>
    case $2 in
        0) emit "$1" installed "$3; selftest: $st_detail" ;;
        3) emit "$1" installed-inactive "$3; selftest: $st_detail"; return 3 ;;
        *) emit "$1" installed "$3; $st_detail" ;;
    esac
}

do_app() {   # do_app <name> <bundle> <mpv.conf>
    name=$1; bundle=$2; conf=$3; fw=$bundle/Contents/Frameworks
    bk="$HOME/Library/Application Support/PushMPC/plex-native/$name"
    installed=0
    [ -f "$fw/libavfilter.8-plex.dylib" ] && [ -f "$bk/proxy.sha256" ] && [ "$(sha "$fw/libavfilter.8.dylib")" = "$(cat "$bk/proxy.sha256")" ] && installed=1
    case $action in
    status)
        if [ "$installed" = 1 ]; then selftest "$fw"; emit "$name" installed "$bundle; $st_detail"
        elif [ -d "$bk" ]; then emit "$name" needs-reapply "$bundle: the app was replaced (an update); install again"
        else emit "$name" not-installed "$bundle"; fi ;;
    install)
        [ -f "$fw/libmpv.2.dylib" ] && [ -f "$fw/libavfilter.8.dylib" ] || { emit "$name" unsupported "$bundle has no libmpv.2.dylib / libavfilter.8.dylib in Contents/Frameworks"; return 3; }
        [ -f "$from/libavfilter.8.dylib" ] && [ -f "$from/libpushmpc_engine.dylib" ] && ls "$from"/pushmpc-avfilter-8.*.dylib >/dev/null 2>&1 \
            || { emit "$name" error "the files are not in $from"; return 1; }
        [ "$dry" = 1 ] && { emit "$name" "$([ "$installed" = 1 ] && echo installed || echo would-install)" "$bundle"; return 0; }
        if [ "$installed" = 1 ]; then
            conf_add "$conf"; selftest "$fw"; s=$?
            report "$name" $s "already in place in $bundle; $conf"; return $?
        fi
        pkill -x Plex 2>/dev/null; pkill -x 'Plex HTPC' 2>/dev/null
        if ! touch "$fw/.pushmpc-write-test" 2>/dev/null; then
            emit "$name" refused "macOS does not allow writing into $bundle: allow this terminal (or Setup) in System Settings > Privacy & Security > App Management, or run as the user who installed Plex"
            return 1
        fi
        rm -f "$fw/.pushmpc-write-test"
        rm -rf "$bk"; mkdir -p "$bk" || { emit "$name" error "cannot write $bk"; return 1; }
        rm -f "$fw/libavfilter.8-plex.dylib"   # stale, from before a Plex update
        cp -p "$fw/libavfilter.8.dylib" "$bk/libavfilter.8.dylib" && sha "$fw/libavfilter.8.dylib" > "$bk/libavfilter.8.dylib.sha256" \
            && mv "$fw/libavfilter.8.dylib" "$fw/libavfilter.8-plex.dylib" && cp "$from/libavfilter.8.dylib" "$fw/libavfilter.8.dylib" \
            || { emit "$name" error "cannot replace $fw/libavfilter.8.dylib"; return 1; }
        sha "$fw/libavfilter.8.dylib" > "$bk/proxy.sha256"
        echo libavfilter.8-plex.dylib > "$bk/added.txt"
        for f in "$from"/pushmpc-avfilter-8.*.dylib "$from/libpushmpc_engine.dylib"; do
            b=${f##*/}; [ -e "$fw/$b" ] && continue   # never overwrite Plex's own files
            cp "$f" "$fw/$b" && echo "$b" >> "$bk/added.txt"
        done
        # files that came through a browser carry quarantine; ours are ad-hoc signed and need none of it
        while read -r b; do xattr -d com.apple.quarantine "$fw/$b" 2>/dev/null; done < "$bk/added.txt"
        xattr -d com.apple.quarantine "$fw/libavfilter.8.dylib" 2>/dev/null
        conf_add "$conf"
        selftest "$fw"; s=$?
        report "$name" $s "$bundle; $conf"; return $? ;;
    uninstall)
        [ "$dry" = 1 ] && { emit "$name" would-uninstall "$bundle"; return 0; }
        pkill -x Plex 2>/dev/null; pkill -x 'Plex HTPC' 2>/dev/null
        detail=''
        if [ -f "$fw/libavfilter.8-plex.dylib" ] && { [ "$installed" = 1 ] || [ ! -f "$fw/libavfilter.8.dylib" ]; }; then
            rm -f "$fw/libavfilter.8.dylib" && mv "$fw/libavfilter.8-plex.dylib" "$fw/libavfilter.8.dylib" || { emit "$name" error "cannot write $fw"; return 1; }
            if [ -f "$bk/libavfilter.8.dylib.sha256" ] && [ "$(sha "$fw/libavfilter.8.dylib")" = "$(cat "$bk/libavfilter.8.dylib.sha256")" ]; then detail='original libavfilter.8.dylib back (SHA-256 matches the backup)'
            else detail='original libavfilter.8.dylib back (no backup hash to compare)'; fi
        elif [ -f "$fw/libavfilter.8-plex.dylib" ]; then
            rm -f "$fw/libavfilter.8-plex.dylib"; detail='Plex replaced libavfilter.8.dylib since (an update): left as it is, the stale original removed'
        fi
        if [ -f "$bk/added.txt" ]; then
            while read -r b; do [ "$b" = libavfilter.8-plex.dylib ] || rm -f "$fw/$b"; done < "$bk/added.txt"
        fi
        conf_remove "$conf"
        rm -rf "$bk"; rmdir "$HOME/Library/Application Support/PushMPC/plex-native" 2>/dev/null
        emit "$name" uninstalled "$detail" ;;
    esac
}

rc=0; found=0
for t in 'desktop:/Applications/Plex.app:Plex' 'htpc:/Applications/Plex HTPC.app:Plex HTPC'; do
    n=${t%%:*}; rest=${t#*:}; b=${rest%:*}; cdir=${rest##*:}
    [ "$app" = auto ] || [ "$app" = "$n" ] || continue
    [ -z "$apath" ] || b=$apath
    [ -d "$b" ] || continue
    found=1
    do_app "$n" "$b" "$HOME/Library/Application Support/$cdir/mpv.conf" || { r=$?; [ $r -gt $rc ] && rc=$r; }
done
[ "$found" = 1 ] || { emit "$app" unsupported "no Plex for Mac / Plex HTPC app found"; exit 3; }
exit $rc
