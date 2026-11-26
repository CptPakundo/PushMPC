#!/bin/sh
# PushMPC capture mode for Linux and macOS: smoother (and upscaled) video for players PushMPC cannot plug into, through
# window capture (pushmpc-capture). The player plays as usual; capture mode shows its picture with frame generation in
# a window right over it that lets every click through.
#
#   pushmpc-capture.sh launch <player> [player arguments]   start the player with capture mode (it ends with the player)
#   pushmpc-capture.sh start [<player>] [-- capture options] capture mode for a player that is already open
#   pushmpc-capture.sh stop                                  end capture mode
#   pushmpc-capture.sh enable <player>     a menu entry "<Player> with PushMPC" that starts the player with capture mode
#   pushmpc-capture.sh disable <player>|--all
#   pushmpc-capture.sh status | list
# Players it knows (more by name: any program's name or its window class works with start / launch):
#   Linux: celluloid, stremio, plex, jellyfin, totem, clapper, dragon, firefox, chromium, chrome, brave, vlc-snap
#   macOS: quicktime, infuse, plex, vlc, iina, safari, chrome, firefox
# On Wayland your desktop asks once which window to capture (it remembers the choice). Players built on mpv (Plex,
# Jellyfin Media Player) also get their sound delayed to match the picture (a marked block in their mpv.conf, removed
# again when capture mode ends). Logs: ~/.local/state/pushmpc/logs/capture.log (macOS: ~/Library/Logs/PushMPC).
# Copy-protected video (Netflix in a browser, Apple TV+ in QuickTime) shows black to any capture: capture mode then
# steps aside and the player's own picture stays.
set -u

os=$(uname -s)
here=$(cd "$(dirname "$0")" && pwd)
data=${XDG_DATA_HOME:-}; case $data in /*) ;; *) data=$HOME/.local/share ;; esac
state=${XDG_STATE_HOME:-}; case $state in /*) ;; *) state=$HOME/.local/state ;; esac
apps=$data/applications
if [ "$os" = Darwin ]; then logdir=$HOME/Library/Logs/PushMPC; else logdir=$state/pushmpc/logs; fi
run=${XDG_RUNTIME_DIR:-${TMPDIR:-/tmp}}
pidfile=$run/pushmpc-capture-$(id -u).pid

say() { printf '%s\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }

# the capture host: next to this script, the installed copy, or on PATH. macOS: Homebrew's pushmpc-capture (it starts
# the "PushMPC Capture" app, which holds the Screen Recording permission)
capbin() {
    if [ "$os" = Darwin ]; then command -v pushmpc-capture 2>/dev/null && return 0; fi
    for b in "$here/pushmpc-capture" "$data/pushmpc/capture/pushmpc-capture"; do
        [ -x "$b" ] && { printf '%s\n' "$b"; return 0; }
    done
    command -v pushmpc-capture 2>/dev/null && return 0
    return 1
}

# player table: key | window match | Linux commands (first found wins; flatpak:<id>, snap:<name>) | macOS app | mpv.conf
players() {
    cat <<'EOF'
celluloid|celluloid|celluloid flatpak:io.github.celluloid_player.Celluloid|-|-
stremio|stremio|stremio flatpak:com.stremio.Stremio|Stremio|-
plex|plex|plex-desktop flatpak:tv.plex.PlexDesktop flatpak:tv.plex.PlexHTPC snap:plex-desktop|Plex|~/.var/app/tv.plex.PlexDesktop/data/plex/mpv.conf
jellyfin|jellyfin|jellyfinmediaplayer flatpak:com.github.iwalton3.jellyfin-media-player flatpak:org.jellyfin.JellyfinDesktop|Jellyfin Media Player|~/.local/share/jellyfinmediaplayer/mpv.conf ~/.var/app/com.github.iwalton3.jellyfin-media-player/data/jellyfinmediaplayer/mpv.conf
totem|totem|totem flatpak:org.gnome.Totem|-|-
clapper|clapper|clapper flatpak:com.github.rafostar.Clapper|-|-
dragon|dragon|dragon flatpak:org.kde.dragonplayer|-|-
firefox|firefox|firefox flatpak:org.mozilla.firefox snap:firefox|Firefox|-
chromium|chromium|chromium chromium-browser flatpak:org.chromium.Chromium snap:chromium|Chromium|-
chrome|chrome|google-chrome google-chrome-stable flatpak:com.google.Chrome|Google Chrome|-
brave|brave|brave-browser brave flatpak:com.brave.Browser|Brave Browser|-
vlc-snap|vlc|snap:vlc|-|-
quicktime|quicktime|-|QuickTime Player|-
infuse|infuse|-|Infuse 8|-
vlc|vlc|-|VLC|-
iina|iina|-|IINA|-
safari|safari|-|Safari|-
EOF
}
field() { players | awk -F'|' -v k="$1" -v n="$2" '$1 == k { print $n; exit }'; }

# the command that starts player $1 here (or nothing)
player_cmd() {
    if [ "$os" = Darwin ]; then
        app=$(field "$1" 4); [ -n "$app" ] && [ "$app" != - ] || return 1
        { [ -d "/Applications/$app.app" ] || [ -d "$HOME/Applications/$app.app" ]; } || return 1
        printf '%s\n' "$app"; return 0   # launch opens it with: open -W -a "<app>"
    fi
    for c in $(field "$1" 3); do
        case $c in
            -) ;;
            flatpak:*) have flatpak && flatpak info "${c#flatpak:}" >/dev/null 2>&1 && { printf 'flatpak run %s\n' "${c#flatpak:}"; return 0; } ;;
            snap:*) have snap && snap list "${c#snap:}" >/dev/null 2>&1 && { printf 'snap run %s\n' "${c#snap:}"; return 0; } ;;
            *) have "$c" && { printf '%s\n' "$c"; return 0; } ;;
        esac
    done
    return 1
}

# the player's mpv.conf for lip sync (the first that exists, else the first one's folder if it exists)
player_conf() {
    for c in $(field "$1" 5); do
        [ "$c" = - ] && continue
        f=$(printf '%s' "$c" | sed "s|^~|$HOME|")
        [ -f "$f" ] && { printf '%s\n' "$f"; return 0; }
        [ -d "$(dirname "$f")" ] && { printf '%s\n' "$f"; return 0; }
    done
    return 1
}

# the window name to look for (macOS: the app's name)
player_match() {
    m=$(field "$1" 2)
    if [ "$os" = Darwin ]; then a=$(field "$1" 4); [ -n "$a" ] && [ "$a" != - ] && m=$a; fi
    printf '%s\n' "${m:-$1}"
}

start_capture() {   # <player key or name> [capture options...]
    p=$1; shift
    bin=$(capbin) || { say "pushmpc-capture is not installed (run PushMPC's setup again)"; return 1; }
    mkdir -p "$logdir"
    set -- --player "$(player_match "$p")" --log "$logdir/capture.log" "$@"
    conf=$(player_conf "$p") && set -- "$@" --audio-conf "$conf"
    "$bin" "$@" &
    cpid=$!
    printf '%s\n' "$cpid" > "$pidfile"
    say "capture mode started for $p (pid $cpid; stop it with: sh $0 stop)"
}

# waits until the player's window is there, then starts capture mode; capture ends with the player
launch() {
    p=$1; shift
    cmd=$(player_cmd "$p") || { say "$p was not found on this computer"; return 1; }
    say "starting $p"
    if [ "$os" = Darwin ]; then open -W -a "$cmd" --args "$@" &
    else
        # shellcheck disable=SC2086
        $cmd "$@" &
    fi
    ppid_=$!
    bin=$(capbin) || { say "pushmpc-capture is not installed: $p runs without it"; wait "$ppid_"; return 0; }
    # the window takes a moment: try for up to 30 s (on Wayland the desktop's picker opens instead)
    i=0
    while [ $i -lt 30 ]; do
        if [ -n "${WAYLAND_DISPLAY:-}" ] || [ "$os" = Darwin ] || "$bin" --list 2>/dev/null | grep -qi "$(player_match "$p")"; then break; fi
        sleep 1; i=$((i + 1))
    done
    [ "$os" = Darwin ] && sleep 2
    start_capture "$p"
    # the player ends: capture mode ends too (it also ends by itself when the window closes)
    wait "$ppid_" 2>/dev/null
    [ -n "${cpid:-}" ] && kill "$cpid" 2>/dev/null
    rm -f "$pidfile"
}

stop() {
    if [ -f "$pidfile" ]; then kill "$(cat "$pidfile")" 2>/dev/null && say "capture mode stopped"; rm -f "$pidfile"
    elif pkill -x pushmpc-capture 2>/dev/null; then say "capture mode stopped"
    else say "capture mode is not running"; fi
}

desktop_file() { printf '%s/pushmpc-capture-%s.desktop\n' "$apps" "$1"; }
enable_entry() {
    p=$1
    [ "$os" = Darwin ] && { say "macOS: start it with: sh $0 launch $p (or start, with the player open)"; return 0; }
    player_cmd "$p" >/dev/null || { say "$p was not found on this computer"; return 1; }
    mkdir -p "$apps" "$data/pushmpc/capture"
    # a stable copy of this script (the download folder may go away)
    [ "$here/pushmpc-capture.sh" = "$data/pushmpc/capture/pushmpc-capture.sh" ] || cp "$0" "$data/pushmpc/capture/pushmpc-capture.sh"
    name=$(printf '%s' "$p" | awk '{ print toupper(substr($0, 1, 1)) substr($0, 2) }')
    icon=$(field "$p" 3 | tr ' ' '\n' | sed -n 's/^flatpak://p' | head -n 1)
    cat > "$(desktop_file "$p")" <<EOF
[Desktop Entry]
Type=Application
Name=$name with PushMPC
Comment=$name with smoother motion (PushMPC capture mode)
Exec=sh "$data/pushmpc/capture/pushmpc-capture.sh" launch $p %U
Icon=${icon:-video-x-generic}
Categories=AudioVideo;Video;Player;
Terminal=false
EOF
    say "menu entry: $name with PushMPC"
}
disable_entry() {
    if [ "$1" = --all ]; then rm -f "$apps"/pushmpc-capture-*.desktop; say "all capture-mode menu entries removed"
    else rm -f "$(desktop_file "$1")"; say "menu entry for $1 removed"; fi
}

status() {
    if b=$(capbin); then say "pushmpc-capture: $b"; else say "pushmpc-capture: not installed"; fi
    if [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then say "running (pid $(cat "$pidfile"))"; else say "not running"; fi
    for f in "$apps"/pushmpc-capture-*.desktop; do [ -f "$f" ] && say "menu entry: $(sed -n 's/^Name=//p' "$f")"; done
    found=''
    for k in $(players | cut -d'|' -f1); do player_cmd "$k" >/dev/null && found="$found${found:+, }$k"; done
    say "players found for capture mode: ${found:-none}"
}

case ${1:-} in
    launch) shift; [ $# -ge 1 ] || { sed -n '2,20p' "$0"; exit 2; }; launch "$@" ;;
    start) shift; p=${1:-}; [ $# -gt 0 ] && shift; [ "${1:-}" = -- ] && shift
           if [ -z "$p" ]; then bin=$(capbin) || exit 1; exec "$bin" "$@"; fi
           start_capture "$p" "$@" ;;
    stop) stop ;;
    enable) [ $# -ge 2 ] || { say "enable <player>"; exit 2; }; enable_entry "$2" ;;
    disable) [ $# -ge 2 ] || { say "disable <player>|--all"; exit 2; }; disable_entry "$2" ;;
    status) status ;;
    list) bin=$(capbin) || exit 1; exec "$bin" --list ;;
    *) sed -n '2,20p' "$0"; exit 2 ;;
esac
