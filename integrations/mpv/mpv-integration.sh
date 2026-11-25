#!/bin/sh
# mpv-integration.sh: puts PushMPC's VapourSynth filter (pushmpc.Enhance through pushmpc.vpy) into mpv on Linux and
# macOS and takes it out again exactly. The POSIX counterpart of mpv-integration.ps1 (same block, same backups).
#
#   mpv-integration.sh install|uninstall|status [--script <pushmpc.vpy>] [--conf <mpv.conf>] [--buffered 4]
#                      [--concurrent 2] [--extra <mpv.conf line>]... [--dry-run]
#   --extra adds a line inside the managed block (global options only, e.g. include="<file>"; the Homebrew formula's
#   pushmpc-setup adds the ArtCNN gpu-next profile this way). Uninstall removes it with the block.
#
# What it changes (install), with a backup and an exact restore (uninstall):
#   ${XDG_CONFIG_HOME:-~/.config}/mpv/mpv.conf (mpv on Linux and macOS; Celluloid and IINA can read the same file, see
#   below). A marked block goes at the very top (before any [profile], so it is global):
#     # >>> PushMPC managed ...
#     vf-append=@pushmpc:vapoursynth=file=[<pushmpc.vpy>]:buffered-frames=4:concurrent-frames=2
#     hwdec=auto-copy            (VapourSynth gets CPU frames only: copy-back decoding)
#     hr-seek-framedrop=no       (no dropped frames around a seek: the filter sees a continuous stream)
#     interpolation=no           (mpv's own frame blending would fight the generated frames)
#     # <<< PushMPC managed
#   The original file is copied to mpv.conf.pushmpc-backup (or mpv.conf.pushmpc-absent when there was none). Uninstall
#   removes the block; when nothing else changed since, the file is byte-identical to the backup and the backup goes.
# Detected and explained, never changed (status):
#   Celluloid  reads an mpv.conf only with Preferences > Config Files > "Load mpv configuration file" on, pointing at
#              the file above (GSettings io.github.celluloid_player.Celluloid mpv-config-enable / mpv-config-file;
#              key names to verify on a real install).
#   Haruna     has no documented switch for ~/.config/mpv/mpv.conf; to verify on a real install (its own settings are
#              ~/.config/haruna/haruna.conf).
#   IINA       reads the folder above only with Settings > Advanced > "Use config directory" on (~/.config/mpv), but
#              its own libmpv is built without VapourSynth: the PushMPC IINA plugin (integrations/iina) is its route.
#   Flatpak / Snap players have their own config folder and may lack VapourSynth: reported, not changed.
# Environment for tests: HOME and XDG_CONFIG_HOME as usual; PUSHMPC_OS=linux|darwin overrides uname; PUSHMPC_NO_PATH=1
# skips the PATH search for players.
set -u

BEGIN_MARK='# >>> PushMPC managed: do not edit between these lines (PushMPC uninstall removes them)'
END_MARK='# <<< PushMPC managed'

action=${1:-}
[ $# -gt 0 ] && shift
script=''; conf=''; buffered=4; concurrent=2; dry=0; extra=''; direct=0
while [ $# -gt 0 ]; do
    case $1 in
        --script) script=$2; shift 2 ;;
        --conf) conf=$2; shift 2 ;;
        --buffered) buffered=$2; shift 2 ;;
        --concurrent) concurrent=$2; shift 2 ;;
        --extra) extra="$extra$2
"; shift 2 ;;
        --dry-run) dry=1; shift ;;
        --direct) direct=1; shift ;;
        *) echo "mpv-integration.sh: unknown option $1" >&2; exit 2 ;;
    esac
done

os=${PUSHMPC_OS:-}
[ -n "$os" ] || case $(uname -s 2>/dev/null) in Darwin) os=darwin ;; *) os=linux ;; esac
cfg=${XDG_CONFIG_HOME:-}
case $cfg in /*) ;; *) cfg=$HOME/.config ;; esac
[ -n "$conf" ] || conf=$cfg/mpv/mpv.conf
if [ -z "$script" ]; then
    if [ "$os" = darwin ]; then script="$HOME/Library/Application Support/PushMPC/vsplugin/pushmpc.vpy"
    else
        data=${XDG_DATA_HOME:-}
        case $data in /*) ;; *) data=$HOME/.local/share ;; esac
        script=$data/pushmpc/vsplugin/pushmpc.vpy
    fi
fi
backup=$conf.pushmpc-backup
absent=$conf.pushmpc-absent

say() { printf '%s\n' "$*"; }
run() { if [ "$dry" = 1 ]; then say "would: $*"; else "$@"; fi; }

# mpv's [...] quoting keeps ':' and spaces; a path with ']' needs %n% (byte length) quoting
vf_option() {
    case $script in
        *']'*) n=$(printf '%s' "$script" | wc -c | tr -d ' '); q="%$n%$script" ;;
        *) q="[$script]" ;;
    esac
    printf '@pushmpc:vapoursynth=file=%s:buffered-frames=%s:concurrent-frames=%s' "$q" "$buffered" "$concurrent"
}
# the block, with the file's line ending ($1 = crlf or lf)
# a value for mpv's config files and key-value lists: plain when simple, else %n% (byte length) quoted
cfg_quote() {
    case $1 in
        *[!A-Za-z0-9_./:-]*) printf '%%%s%%%s' "$(printf '%s' "$1" | wc -c | tr -d ' ')" "$1" ;;
        *) printf '%s' "$1" ;;
    esac
}
block() {
    if [ "$1" = crlf ]; then nl=$(printf '\r\n_'); nl=${nl%_}; else nl='
'; fi
    printf '%s%s' "$BEGIN_MARK" "$nl"
    if [ "$direct" = 1 ]; then
        printf 'vf-append=%s%s' "$(vf_option)" "$nl"
    else
        # pushmpc.lua (next to pushmpc.vpy) adds the filter with "vf append" once video runs: a filter that cannot
        # start only logs a warning instead of stopping all playback, and hosts that reset vf get it back
        printf 'scripts-append=%s%s' "$(cfg_quote "$(dirname "$script")/pushmpc.lua")" "$nl"
        printf 'script-opts-append=pushmpc-vpy=%s%s' "$(cfg_quote "$script")" "$nl"
        printf 'script-opts-append=pushmpc-buffered=%s%sscript-opts-append=pushmpc-concurrent=%s%s' "$buffered" "$nl" "$concurrent" "$nl"
    fi
    printf 'hwdec=auto-copy%shr-seek-framedrop=no%sinterpolation=no%s' "$nl" "$nl" "$nl"
    if [ -n "$extra" ]; then
        printf '%s' "$extra" | while IFS= read -r x; do [ -n "$x" ] && printf '%s%s' "$x" "$nl"; done
    fi
    printf '%s%s' "$END_MARK" "$nl"
}
line_ending() { if [ -f "$1" ] && [ "$(tr -dc '\r' < "$1" | wc -c | tr -d ' ')" != 0 ]; then echo crlf; else echo lf; fi; }
# line numbers of the block's first and last line (empty when absent)
block_lines() {
    b=$(grep -n -F -x -e "$BEGIN_MARK" -e "$BEGIN_MARK$(printf '\r')" "$1" 2>/dev/null | head -n 1 | cut -d: -f1)
    e=''
    if [ -n "$b" ]; then
        e=$(tail -n +"$b" "$1" | grep -n -F -x -e "$END_MARK" -e "$END_MARK$(printf '\r')" | head -n 1 | cut -d: -f1)
        [ -n "$e" ] && e=$((b + e - 1))
    fi
    [ -n "$e" ] || b=''
}
# the file without the block, byte for byte (head/tail keep a missing last newline)
strip_block() {
    block_lines "$1"
    if [ -z "$b" ]; then cat "$1"; return; fi
    [ "$b" -gt 1 ] && head -n $((b - 1)) "$1"
    tail -n +$((e + 1)) "$1"
}
# lines outside the block that would fight it (global section only, before the first [profile])
conflicts() {
    strip_block "$1" | tr -d '\r' | awk -v q="'" '
        BEGIN { global = 1 }
        /^[ \t]*\[/ { global = 0 }
        { t = $0; sub(/^[ \t]+/, "", t); sub(/[ \t]+$/, "", t) }
        t ~ /^#/ || t == "" { next }
        t ~ /^-?-?(vf|vf-append|vf-add|vf-pre)[ \t]*=.*vapoursynth/ { print "another VapourSynth filter is set (" t "): two filters would both run (SVP?)" }
        global && t ~ /^hwdec[ \t]*=/ { v = t; sub(/^hwdec[ \t]*=[ \t]*/, "", v); if (v !~ /-copy$/ && v != "no") print "a later global " q t q " overrides hwdec=auto-copy: VapourSynth needs copy-back decoding" }
        global && t ~ /^interpolation[ \t]*=[ \t]*yes/ { print "a later global " q t q " turns mpv" q "s frame blending back on" }'
}

install_conf() {
    dir=$(dirname "$conf")
    tmp=$conf.pushmpc-tmp.$$
    if [ -f "$conf" ]; then
        le=$(line_ending "$conf")
        block_lines "$conf"
        if [ -n "$b" ]; then
            { [ "$b" -gt 1 ] && head -n $((b - 1)) "$conf"; block "$le"; tail -n +$((e + 1)) "$conf"; } > "$tmp.new" 2>/dev/null || true
            if cmp -s "$tmp.new" "$conf"; then rm -f "$tmp.new"; say "mpv.conf: the PushMPC block is up to date ($conf)"
            else
                if [ "$dry" = 1 ]; then rm -f "$tmp.new"; say "would: update the PushMPC block in $conf"; else mv "$tmp.new" "$conf"; say "mpv.conf: PushMPC block updated ($conf)"; fi
            fi
        else
            [ -f "$backup" ] || run cp -p "$conf" "$backup"
            if [ "$dry" = 1 ]; then say "would: put the PushMPC block at the top of $conf"
            else { block "$le"; cat "$conf"; } > "$tmp" && mv "$tmp" "$conf"; say "mpv.conf: PushMPC block added ($conf; backup $backup)"; fi
        fi
        c=$(conflicts "$conf")
        [ -n "$c" ] && printf '%s\n' "$c" | sed 's/^/warning: /'
    else
        how=file
        [ -d "$dir" ] || how=created-dir
        if [ "$dry" = 1 ]; then say "would: create $conf with the PushMPC block"; return 0; fi
        [ "$how" = created-dir ] && mkdir -p "$dir"
        printf '%s' "$how" > "$absent"
        block lf > "$conf"
        say "mpv.conf: created with the PushMPC block ($conf)"
    fi
    [ -f "$script" ] || say "note: $script does not exist yet (install the plugin and pushmpc.vpy there, or pass --script)"
    return 0
}

uninstall_conf() {
    if [ ! -f "$conf" ]; then
        [ -f "$absent" ] && run rm -f "$absent"
        say "mpv.conf: none ($conf)"; return 0
    fi
    tmp=$conf.pushmpc-tmp.$$
    strip_block "$conf" > "$tmp"
    if [ -f "$absent" ]; then
        how=$(cat "$absent")
        if [ -z "$(tr -d ' \t\r\n' < "$tmp")" ]; then
            rm -f "$tmp"
            run rm -f "$conf" "$absent"
            dir=$(dirname "$conf")
            if [ "$how" = created-dir ] && [ "$dry" = 0 ] && [ -z "$(ls -A "$dir" 2>/dev/null)" ]; then rmdir "$dir"; fi
            say "mpv.conf: removed (PushMPC had created it)"; return 0
        fi
        if [ "$dry" = 1 ]; then rm -f "$tmp"; say "would: remove the PushMPC block from $conf"; return 0; fi
        mv "$tmp" "$conf"; rm -f "$absent"
        say "mpv.conf: block removed; your later additions kept"; return 0
    fi
    if [ -f "$backup" ]; then
        if cmp -s "$tmp" "$backup"; then
            rm -f "$tmp"
            if [ "$dry" = 1 ]; then say "would: restore $conf from $backup"; return 0; fi
            cp -p "$backup" "$conf" && rm -f "$backup"
            say "mpv.conf: restored exactly from the backup"; return 0
        fi
        if [ "$dry" = 1 ]; then rm -f "$tmp"; say "would: remove the PushMPC block from $conf"; return 0; fi
        mv "$tmp" "$conf"
        say "mpv.conf: block removed; mpv.conf changed since the install, so the backup stays at $backup"; return 0
    fi
    if cmp -s "$tmp" "$conf"; then rm -f "$tmp"; say "mpv.conf: no PushMPC block"; return 0; fi
    if [ "$dry" = 1 ]; then rm -f "$tmp"; say "would: remove the PushMPC block from $conf"; return 0; fi
    mv "$tmp" "$conf"
    say "mpv.conf: block removed (no backup was found)"
}

have() { [ "${PUSHMPC_NO_PATH:-0}" = 1 ] && return 1; command -v "$1" >/dev/null 2>&1; }

players() {
    if have mpv; then say "player: mpv ($(command -v mpv)) reads $conf"; fi
    if have celluloid || [ -d "$cfg/celluloid" ]; then
        say "player: Celluloid: turn on Preferences > Config Files > 'Load mpv configuration file' and choose $conf"
    fi
    if have haruna || [ -f "$cfg/haruna/haruna.conf" ]; then
        say "player: Haruna: no documented switch for $conf (to verify); add the block's options in Haruna's own mpv settings if it ignores the file"
    fi
    for fp in io.mpv.Mpv io.github.celluloid_player.Celluloid org.kde.haruna; do
        [ -d "$HOME/.var/app/$fp" ] && say "player: Flatpak $fp: its config is $HOME/.var/app/$fp/config/mpv/mpv.conf and its mpv may lack VapourSynth (not changed)"
    done
    [ -d "$HOME/snap/mpv" ] && say "player: Snap mpv: its own config folder, may lack VapourSynth (not changed)"
    if [ "$os" = darwin ]; then
        if [ -d /Applications/IINA.app ] || [ -d "$HOME/Applications/IINA.app" ]; then
            say "player: IINA: its own libmpv has no VapourSynth, so it cannot run this filter; use the PushMPC IINA plugin (shaders + smooth motion) instead"
        else
            say "note: IINA reads $(dirname "$conf") only with Settings > Advanced > 'Use config directory' on"
        fi
    fi
    return 0
}

status() {
    say "mpv.conf: $conf"
    if [ -f "$conf" ]; then
        block_lines "$conf"
        if [ -n "$b" ]; then say "  PushMPC block: installed (lines $b-$e)"; else say "  PushMPC block: not installed"; fi
        c=$(conflicts "$conf")
        [ -n "$c" ] && printf '%s\n' "$c" | sed 's/^/  warning: /'
    else say "  (no file)"; fi
    [ -f "$backup" ] && say "  backup: $backup"
    [ -f "$absent" ] && say "  created by PushMPC ($absent)"
    say "pushmpc.vpy: $script$( [ -f "$script" ] || printf ' (missing)')"
    players
}

case $action in
    install) install_conf; players ;;
    uninstall) uninstall_conf ;;
    status) status ;;
    *) echo "usage: mpv-integration.sh install|uninstall|status [--script <pushmpc.vpy>] [--conf <mpv.conf>] [--buffered N] [--concurrent N] [--extra LINE]... [--dry-run]" >&2; exit 2 ;;
esac
