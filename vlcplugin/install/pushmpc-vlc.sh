#!/bin/sh
# PushMPC for VLC 3.0.x: per-user install for Linux (native VLC and the Flathub VLC) and macOS (VLC.app). No root.
#   pushmpc-vlc.sh install   [--plugin <libpushmpc_plugin.so|.dylib>] [--target auto|native|flatpak|macos]
#   pushmpc-vlc.sh uninstall [--target ...]
#   pushmpc-vlc.sh status
# What install does:
#   1. copies the plugin into a per-user folder (never into VLC's own folders, so VLC updates keep it):
#        native Linux  ~/.local/share/pushmpc/vlc
#        Flatpak       ~/.var/app/org.videolan.VLC/data/pushmpc/vlc
#        macOS         ~/Library/Application Support/PushMPC/vlc
#   2. points VLC at it with VLC_PLUGIN_PATH (VLC 3 scans that list after its own plugins folder; no cache to rebuild):
#        native Linux  ~/.config/environment.d/60-pushmpc-vlc.conf (graphical sessions, from the next login) and a
#                      ~/.local/share/applications/vlc.desktop copy whose Exec sets it (works at once from the menu)
#        Flatpak       flatpak override --user --env=VLC_PLUGIN_PATH=... org.videolan.VLC
#        macOS         a LaunchAgent that runs launchctl setenv at login (and once now)
#   3. switches it on in VLC's vlcrc (a backup is kept next to it as vlcrc.pushmpc-backup):
#        [core]     video-filter gains postproc:pushmpc_out (the static-chain slot, then the output stage)
#        [avcodec]  avcodec-hw=none on Linux: VLC 3 cannot hand VA-API/VDPAU pictures back to the display after a CPU
#                   filter and would drop every filter; software decoding keeps PushMPC on
#        [pushmpc]  pushmpc-smoother=0 (Auto)
# Snap VLC: not supported (strict confinement cannot load plugins from the home folder). Switch it off in VLC:
# Tools > Effects and Filters > Video Effects > Advanced > Post processing, or Preferences > PushMPC > Smoother: Off.
set -eu

ACTION="${1:-status}"; [ $# -gt 0 ] && shift
PLUGIN=""; TARGET="auto"
while [ $# -gt 0 ]; do
  case "$1" in
    --plugin) PLUGIN="$2"; shift 2 ;;
    --target) TARGET="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
OS="$(uname -s)"
here="$(cd "$(dirname "$0")" && pwd)"
APPID=org.videolan.VLC
LAUNCH_LABEL=io.github.pushmpc.vlc-env

say() { printf '%s\n' "$*"; }

# vlcrc_set <file> <section> <key> <value>: set key=value inside [section] (uncommenting a "#key=" default line)
vlcrc_set() {
  f="$1"; sec="$2"; key="$3"; val="$4"
  mkdir -p "$(dirname "$f")"; [ -f "$f" ] || : > "$f"
  awk -v sec="[$sec]" -v key="$key" -v val="$val" '
    BEGIN { insec = 0; done = 0; seen = 0 }
    /^\[.*\]/ { if (insec && !done) { print key "=" val; done = 1 } insec = ($0 == sec); if (insec) seen = 1; print; next }
    insec && !done && ($0 ~ "^#?" key "=") { print key "=" val; done = 1; next }
    { print }
    END { if (!done) { if (!seen) print sec; print key "=" val } }' "$f" > "$f.tmp" && mv "$f.tmp" "$f"
}
# vlcrc_get <file> <section> <key>: the active value or nothing
vlcrc_get() {
  [ -f "$1" ] || return 0
  awk -v sec="[$2]" -v key="$3" '/^\[.*\]/ { insec = ($0 == sec); next } insec && index($0, key "=") == 1 { print substr($0, length(key) + 2); exit }' "$1"
}
# vlcrc_del <file> <section> <key>: comment the key out again (VLC's own default applies)
vlcrc_del() {
  [ -f "$1" ] || return 0
  awk -v sec="[$2]" -v key="$3" '/^\[.*\]/ { insec = ($0 == sec) } insec && index($0, key "=") == 1 { print "#" $0; next } { print }' "$1" > "$1.tmp" && mv "$1.tmp" "$1"
}
filters_add() { # current list -> list with postproc:pushmpc_out (kept once, in that order, after the user's own)
  printf '%s' "$1" | tr ':' '\n' | grep -v -e '^postproc$' -e '^pushmpc_out$' -e '^$' | { cat; printf 'postproc\npushmpc_out\n'; } | paste -s -d: -
}
filters_del() { printf '%s' "$1" | tr ':' '\n' | grep -v -e '^postproc$' -e '^pushmpc_out$' -e '^$' | paste -s -d: - || true; }

detect_targets() {
  t=""
  if [ "$OS" = Darwin ]; then
    [ -d /Applications/VLC.app ] || [ -d "$HOME/Applications/VLC.app" ] && t="macos"
  else
    if command -v vlc >/dev/null 2>&1 && ! readlink -f "$(command -v vlc)" | grep -q '/snap/'; then t="native"; fi
    if command -v flatpak >/dev/null 2>&1 && flatpak info "$APPID" >/dev/null 2>&1; then t="$t flatpak"; fi
    if command -v snap >/dev/null 2>&1 && snap list vlc >/dev/null 2>&1; then say "note: the Snap VLC cannot load PushMPC (strict confinement); use the distro or Flathub VLC"; fi
  fi
  printf '%s' "$t"
}

paths_for() { # sets DIR, RC for a target
  case "$1" in
    native)  DIR="$HOME/.local/share/pushmpc/vlc"; RC="${XDG_CONFIG_HOME:-$HOME/.config}/vlc/vlcrc" ;;
    flatpak) DIR="$HOME/.var/app/$APPID/data/pushmpc/vlc"; RC="$HOME/.var/app/$APPID/config/vlc/vlcrc" ;;
    macos)   DIR="$HOME/Library/Application Support/PushMPC/vlc"; RC="$HOME/Library/Preferences/org.videolan.vlc/vlcrc" ;;
  esac
}

install_one() {
  tgt="$1"; paths_for "$tgt"
  plug="$PLUGIN"
  if [ -z "$plug" ]; then
    for c in "$here/libpushmpc_plugin.so" "$here/libpushmpc_plugin.dylib" "$here/../lib/libpushmpc_plugin.so"; do [ -f "$c" ] && plug="$c" && break; done
  fi
  [ -f "$plug" ] || { echo "plugin not found (use --plugin)" >&2; exit 1; }
  mkdir -p "$DIR"
  cp -f "$plug" "$DIR/"
  [ "$OS" = Darwin ] && xattr -d com.apple.quarantine "$DIR/$(basename "$plug")" 2>/dev/null || true
  case "$tgt" in
    native)
      mkdir -p "$HOME/.config/environment.d"
      printf '# PushMPC for VLC\nVLC_PLUGIN_PATH=%s\n' "$DIR" > "$HOME/.config/environment.d/60-pushmpc-vlc.conf"
      for src in /usr/share/applications/vlc.desktop /usr/local/share/applications/vlc.desktop; do
        if [ -f "$src" ]; then
          mkdir -p "$HOME/.local/share/applications"
          sed "s|^Exec=|Exec=env VLC_PLUGIN_PATH=$DIR |; s|^TryExec=.*||" "$src" > "$HOME/.local/share/applications/vlc.desktop"
          printf 'X-PushMPC=1\n' >> "$HOME/.local/share/applications/vlc.desktop"
          break
        fi
      done ;;
    flatpak)
      flatpak override --user --env=VLC_PLUGIN_PATH="$DIR" "$APPID" ;;
    macos)
      la="$HOME/Library/LaunchAgents/$LAUNCH_LABEL.plist"; mkdir -p "$(dirname "$la")"
      cat > "$la" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>$LAUNCH_LABEL</string>
  <key>ProgramArguments</key><array><string>/bin/launchctl</string><string>setenv</string><string>VLC_PLUGIN_PATH</string><string>$DIR</string></array>
  <key>RunAtLoad</key><true/>
</dict></plist>
EOF
      launchctl setenv VLC_PLUGIN_PATH "$DIR" 2>/dev/null || true ;;
  esac
  [ -f "$RC" ] && [ ! -f "$RC.pushmpc-backup" ] && cp "$RC" "$RC.pushmpc-backup"
  vlcrc_set "$RC" core video-filter "$(filters_add "$(vlcrc_get "$RC" core video-filter)")"
  vlcrc_set "$RC" pushmpc pushmpc-smoother 0
  [ "$tgt" != macos ] && vlcrc_set "$RC" avcodec avcodec-hw none
  say "PushMPC for VLC installed ($tgt): plugin in $DIR, settings in $RC"
}

uninstall_one() {
  tgt="$1"; paths_for "$tgt"
  rm -f "$DIR/libpushmpc_plugin.so" "$DIR/libpushmpc_plugin.dylib"; rmdir "$DIR" 2>/dev/null || true
  case "$tgt" in
    native)
      rm -f "$HOME/.config/environment.d/60-pushmpc-vlc.conf"
      grep -q '^X-PushMPC=1' "$HOME/.local/share/applications/vlc.desktop" 2>/dev/null && rm -f "$HOME/.local/share/applications/vlc.desktop" ;;
    flatpak) flatpak override --user --unset-env=VLC_PLUGIN_PATH "$APPID" 2>/dev/null || true ;;
    macos)   rm -f "$HOME/Library/LaunchAgents/$LAUNCH_LABEL.plist"; launchctl unsetenv VLC_PLUGIN_PATH 2>/dev/null || true ;;
  esac
  if [ -f "$RC" ]; then
    rest="$(filters_del "$(vlcrc_get "$RC" core video-filter)")"
    if [ -n "$rest" ]; then vlcrc_set "$RC" core video-filter "$rest"; else vlcrc_del "$RC" core video-filter; fi
    vlcrc_del "$RC" pushmpc pushmpc-smoother
    if [ "$tgt" != macos ]; then
      old="$(vlcrc_get "$RC.pushmpc-backup" avcodec avcodec-hw)"
      if [ -n "$old" ]; then vlcrc_set "$RC" avcodec avcodec-hw "$old"; else vlcrc_del "$RC" avcodec avcodec-hw; fi
    fi
  fi
  say "PushMPC for VLC removed ($tgt)"
}

if [ "$TARGET" = auto ]; then TARGETS="$(detect_targets)"; else TARGETS="$TARGET"; fi
case "$ACTION" in
  install)   [ -n "$TARGETS" ] || { echo "no supported VLC found (distro VLC, Flathub org.videolan.VLC, VLC.app)" >&2; exit 1; }
             for t in $TARGETS; do install_one "$t"; done ;;
  uninstall) for t in ${TARGETS:-native flatpak macos}; do uninstall_one "$t"; done ;;
  status)    for t in $TARGETS; do paths_for "$t"; say "$t: plugin $(ls "$DIR"/libpushmpc_plugin.* 2>/dev/null || echo missing); video-filter=$(vlcrc_get "$RC" core video-filter)"; done ;;
  *) echo "usage: $0 install|uninstall|status [--plugin file] [--target auto|native|flatpak|macos]" >&2; exit 2 ;;
esac
