#!/bin/sh
# pushmpc-mac-players.sh: PushMPC in IINA and VLC on the Mac, from the Mac bundle (the Homebrew formula's plex-mac
# resource, unpacked by pushmpc-setup). Neither app is modified: IINA.app and VLC.app stay as they are (their updates
# keep PushMPC; nothing to re-apply).
#
#   pushmpc-mac-players.sh install|uninstall|status [--from DIR] [--bridge PATH] [--only iina|vlc] [--no-selftest]
#
#   IINA  the PushMPC IINA plugin (iina/PushMPC-<version>.iinaplgz) into IINA's plugin folder
#         (~/Library/Application Support/com.colliderli.iina/plugins/com.pushmpc.iina.iinaplugin) and switched on
#         (IINA's PluginEnabled.com.pushmpc.iina default). The plugin loads the bridge (--bridge: Homebrew's
#         lib/pushmpc/pushmpc-iina.so) into IINA's own libmpv; the bridge runs PushMPC's engines, best first: apple
#         (VideoToolbox), then the engine module from this bundle (apple, portable-vulkan on MoltenVK, portable-cpu).
#         Self-test: IINA's libmpv plays a generated clip through the bridge (pushmpc-plex-selftest); the bridge's log
#         names the engine and counts frames in and out.
#   VLC   vlc/pushmpc-vlc.sh install --target macos (frame generation, the FSR output, MoltenVK; its own self-test).
# uninstall reverses both: the plugin folder and IINA's settings as they were, VLC's vlcrc as it was.
# Exit: 0 done, 1 error, 2 usage, 3 neither IINA nor VLC is installed.
set -u
action=${1:-}; [ $# -gt 0 ] && shift
from=''; bridge=''; only=''; selftest=1
while [ $# -gt 0 ]; do
    case $1 in
        --from) from=$2; shift 2 ;;
        --bridge) bridge=$2; shift 2 ;;
        --only) only=$2; shift 2 ;;
        --no-selftest) selftest=0; shift ;;
        *) echo "pushmpc-mac-players.sh: unknown option $1" >&2; exit 2 ;;
    esac
done
case $action in install|uninstall|status) ;; *) sed -n '2,18p' "$0" >&2; exit 2 ;; esac
here=$(cd "$(dirname "$0")" && pwd); [ -n "$from" ] || from=$here
if [ -z "$bridge" ]; then
    for b in /opt/homebrew/lib/pushmpc/pushmpc-iina.so /usr/local/lib/pushmpc/pushmpc-iina.so; do [ -f "$b" ] && bridge=$b && break; done
fi
state="$HOME/Library/Application Support/PushMPC/mac-players"
plugins="$HOME/Library/Application Support/com.colliderli.iina/plugins"
pid=com.pushmpc.iina; pdir="$plugins/$pid.iinaplugin"
rc=0; found=0

iina_app() { for a in /Applications/IINA.app "$HOME/Applications/IINA.app"; do [ -d "$a" ] && { echo "$a"; return 0; }; done; return 1; }
vlc_app() { for a in /Applications/VLC.app "$HOME/Applications/VLC.app"; do [ -d "$a" ] && { echo "$a"; return 0; }; done; return 1; }

iina_selftest() {   # IINA's own libmpv, the bridge, the engines: a generated clip at x2
    app=$1
    [ -f "$bridge" ] || { echo "IINA: self-test skipped (no bridge; brew install CptPakundo/pushmpc/pushmpc installs it)"; return 0; }
    host="$from/pushmpc-plex-selftest"; [ -x "$host" ] || { echo "IINA: self-test skipped (no pushmpc-plex-selftest)"; return 0; }
    log="$HOME/Library/Logs/PushMPC/iina-bridge.log"; mark=$( [ -f "$log" ] && wc -l < "$log" || echo 0 )
    out=$(PUSHMPC_HOST_VF='@pushmpc:lavfi=[null@pushmpc]' PUSHMPC_HOST_SCRIPTS="$bridge" PUSHMPC_HOST_ACCEPT_EOF=1 \
          DYLD_LIBRARY_PATH="$app/Contents/Frameworks" "$host" "$app/Contents/Frameworks/libmpv.2.dylib" 2>&1); r=$?
    new=$( [ -f "$log" ] && tail -n +$((mark + 1)) "$log" )
    eng=$(printf '%s\n' "$new" | sed -n 's/^pushmpc-iina: engine: \([^:(]*\).*/\1/p' | head -1 | sed 's/ *$//')
    fr=$(printf '%s\n' "$new" | sed -n 's/.*graph closed: \([0-9]*\) frames in, \([0-9]*\) out.*/\1 \2/p' | tail -1)
    set -- $fr
    if [ "$r" = 0 ] && [ -n "${1:-}" ] && [ "${2:-0}" -gt "${1:-0}" ]; then
        echo "IINA: self-test: ${1} frames in, ${2} out (engine: ${eng:-unknown})"
    else
        echo "IINA: self-test: no frames generated (exit $r; $(printf '%s\n' "$new" | grep -E 'not active|pass-through|engine' | tail -3 | tr '\n' ' '))"
    fi
}

do_iina() {
    app=$(iina_app) || return 0
    found=1
    case $action in
    install)
        pkg=$(ls "$from"/iina/*.iinaplgz 2>/dev/null | head -1)
        [ -n "$pkg" ] || { echo "IINA: no plugin in $from/iina"; rc=1; return; }
        mkdir -p "$state" "$plugins"
        if [ ! -f "$state/iina-enabled" ]; then   # IINA's own setting, as it was before PushMPC
            defaults read com.colliderli.iina "PluginEnabled.$pid" > "$state/iina-enabled" 2>/dev/null || echo absent > "$state/iina-enabled"
            [ -d "$plugins" ] && [ -n "$(ls -A "$plugins" 2>/dev/null)" ] || : > "$state/iina-plugins-created"
        fi
        rm -rf "$pdir.new" && mkdir -p "$pdir.new" && ditto -x -k "$pkg" "$pdir.new" && rm -rf "$pdir" && mv "$pdir.new" "$pdir" \
            || { echo "IINA: could not unpack the plugin into $plugins"; rc=1; return; }
        defaults write com.colliderli.iina "PluginEnabled.$pid" -bool true
        echo "IINA: PushMPC plugin in $pdir (switched on)$( [ -f "$bridge" ] && echo "; bridge $bridge" || echo "; no bridge: shaders and smooth motion only")"
        [ "$selftest" = 0 ] || iina_selftest "$app" ;;
    uninstall)
        rm -rf "$pdir" "$plugins/.preferences/$pid.plist" "$plugins/.data/$pid"
        rmdir "$plugins/.preferences" "$plugins/.data" 2>/dev/null
        if [ -f "$state/iina-enabled" ]; then
            old=$(cat "$state/iina-enabled")
            case $old in
                absent) defaults delete com.colliderli.iina "PluginEnabled.$pid" 2>/dev/null ;;
                0|false) defaults write com.colliderli.iina "PluginEnabled.$pid" -bool false ;;
                *) defaults write com.colliderli.iina "PluginEnabled.$pid" -bool true ;;
            esac
            if [ -f "$state/iina-plugins-created" ]; then rmdir "$plugins" 2>/dev/null; fi
            rm -f "$state/iina-enabled" "$state/iina-plugins-created"
        fi
        echo "IINA: PushMPC plugin removed" ;;
    status)
        if [ -d "$pdir" ]; then echo "IINA: plugin installed ($(defaults read com.colliderli.iina "PluginEnabled.$pid" 2>/dev/null | sed 's/1/on/;s/0/off/'))"; else echo "IINA: not installed"; fi ;;
    esac
}

do_vlc() {
    vlc_app >/dev/null || return 0
    found=1
    v="$from/vlc/pushmpc-vlc.sh"
    [ -f "$v" ] || { echo "VLC: no VLC module in $from/vlc"; rc=1; return; }
    case $action in
    install)
        if [ "$selftest" = 1 ]; then sh "$v" install --target macos --plugin "$from/vlc/libpushmpc_plugin.dylib" || rc=1
        else sh "$v" install --target macos --plugin "$from/vlc/libpushmpc_plugin.dylib" --no-hw-selftest || rc=1; fi ;;
    uninstall) sh "$v" uninstall --target macos || rc=1 ;;
    status) sh "$v" status --target macos ;;
    esac
}

[ "$only" = vlc ] || do_iina
[ "$only" = iina ] || do_vlc
if [ "$action" = uninstall ]; then rmdir "$state" "$(dirname "$state")" 2>/dev/null; fi
[ "$found" = 1 ] || { echo "neither IINA nor VLC is installed"; exit 3; }
exit $rc
