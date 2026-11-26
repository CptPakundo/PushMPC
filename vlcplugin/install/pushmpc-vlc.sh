#!/bin/sh
# PushMPC for VLC 3.0.x: per-user install for Linux (native VLC and the Flathub VLC) and macOS (VLC.app). No root.
#   pushmpc-vlc.sh install   [--plugin <libpushmpc_plugin.so|.dylib>] [--target auto|native|flatpak|snap|macos]
#                            [--no-hw-selftest] [--clip <test clip>]
#   pushmpc-vlc.sh uninstall [--target ...]
#   pushmpc-vlc.sh selftest-hw [--target ...] [--clip <test clip>]
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
#        [avcodec]  avcodec-hw=none on Linux to start with (software decoding always keeps PushMPC on)
#        [pushmpc]  pushmpc-smoother=0 (Auto)
#   macOS: also PushMPC's OpenGL output with FSR 1 upscaling (libpushmpc_macosx_plugin.dylib, [core] vout=pushmpc_macosx,any)
#      and MoltenVK (libMoltenVK.dylib, the GPU engine's Vulkan driver) from next to the plugin, then a self-test: VLC.app
#      plays a generated 2 s clip headless with PushMPC at x2; the log line names the engine and the generated frames.
#   vlcrc: uninstall puts the file back byte for byte (or removes it when install created it) as long as nothing else
#      changed it since; otherwise only PushMPC's own settings are taken out.
#   4. Linux: the hardware self-test. PushMPC keeps VA-API / VDPAU pictures on the GPU (its own surfaces on the
#      decoder's device, after a round-trip check of the driver). The installer plays a 2 s clip (pushmpc-selftest.mkv,
#      next to this script) with PushMPC on: with a display, through VLC's OpenGL output and its VA-API / VDPAU interop
#      in a small window (the GUI's own path; pass: the user's own avcodec-hw comes back); else, or if that fails,
#      headless through VLC's DRM VA-API decoder, then VDPAU (pass: avcodec-hw is set to that decoder). Otherwise
#      software decoding stays: the automatic off switch. The result is
#      kept in <plugin folder>/hw-selftest.txt; "selftest-hw" runs it again (e.g. after a driver update).
# Snap VLC: the wrapper unsets VLC_PLUGIN_PATH, so a Lua interface script (pushmpc_boot.lua, lua-intf + extraintf=luaintf)
# loads the plugin and hands it VLC's postproc slot after checking VLC's layout (or changes nothing). Switch it off in VLC:
# Tools > Effects and Filters > Video Effects > Advanced > Post processing, or Preferences > PushMPC > Smoother: Off.
set -eu

ACTION="${1:-status}"; [ $# -gt 0 ] && shift
PLUGIN=""; TARGET="auto"; HWTEST=1; CLIP=""
while [ $# -gt 0 ]; do
  case "$1" in
    --plugin) PLUGIN="$2"; shift 2 ;;
    --target) TARGET="$2"; shift 2 ;;
    --no-hw-selftest) HWTEST=0; shift ;;
    --clip) CLIP="$2"; shift 2 ;;
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
    # Snap VLC: through the Lua bootstrap (see the header)
    if command -v snap >/dev/null 2>&1 && snap list vlc >/dev/null 2>&1; then t="$t snap"; fi
  fi
  printf '%s' "$t"
}

paths_for() { # sets DIR, RC for a target
  case "$1" in
    native)  DIR="$HOME/.local/share/pushmpc/vlc"; RC="${XDG_CONFIG_HOME:-$HOME/.config}/vlc/vlcrc" ;;
    flatpak) DIR="$HOME/.var/app/$APPID/data/pushmpc/vlc"; RC="$HOME/.var/app/$APPID/config/vlc/vlcrc" ;;
    # the snap's wrapper runs vlc --config=$SNAP_USER_COMMON/vlcrc and leaves VLC_PLUGIN_PATH alone
    snap)    DIR="$HOME/snap/vlc/common/pushmpc"; RC="$HOME/snap/vlc/common/vlcrc" ;;
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
  if [ "$tgt" = macos ]; then   # the FSR output and MoltenVK, when they ship next to the plugin
    for x in libpushmpc_macosx_plugin.dylib libMoltenVK.dylib; do
      [ -f "$(dirname "$plug")/$x" ] || continue
      cp -f "$(dirname "$plug")/$x" "$DIR/"; xattr -d com.apple.quarantine "$DIR/$x" 2>/dev/null || true
    done
  fi
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
    snap)
      # the Lua bootstrap: VLC's user Lua folder (per snap revision; snapd copies it on refresh) plus a seed copy
      lua="$here/snap/pushmpc_boot.lua"; [ -f "$lua" ] || lua="$here/pushmpc_boot.lua"
      [ -f "$lua" ] || { echo "pushmpc_boot.lua not found next to the installer" >&2; exit 1; }
      cp -f "$lua" "$DIR/pushmpc_boot.lua"
      rev="$(snap list vlc 2>/dev/null | awk 'NR==2{print $3}')"
      [ -n "$rev" ] || rev=current
      [ -d "$HOME/snap/vlc/$rev" ] || (timeout 60 snap run vlc --version >/dev/null 2>&1 || true)
      mkdir -p "$HOME/snap/vlc/$rev/.local/share/vlc/lua/intf"
      cp -f "$lua" "$HOME/snap/vlc/$rev/.local/share/vlc/lua/intf/pushmpc_boot.lua"
      [ -f "$DIR/pushmpc.conf" ] || printf 'smoother=0\n' > "$DIR/pushmpc.conf" ;;
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
  if [ ! -f "$RC.pushmpc-backup" ] && [ ! -f "$RC.pushmpc-absent" ]; then
    if [ -f "$RC" ]; then cp "$RC" "$RC.pushmpc-backup"; else mkdir -p "$(dirname "$RC")"; : > "$RC.pushmpc-absent"; fi
  fi
  vlcrc_set "$RC" core video-filter "$(filters_add "$(vlcrc_get "$RC" core video-filter)")"
  vlcrc_set "$RC" pushmpc pushmpc-smoother 0
  [ "$tgt" != macos ] && vlcrc_set "$RC" avcodec avcodec-hw none
  [ "$tgt" = macos ] && [ -f "$DIR/libpushmpc_macosx_plugin.dylib" ] && vlcrc_set "$RC" core vout "pushmpc_macosx,any"
  if [ "$tgt" = snap ]; then
    intfs="$(vlcrc_get "$RC" core extraintf)"
    case ":$intfs:" in *:luaintf:*) ;; *) vlcrc_set "$RC" core extraintf "${intfs:+$intfs:}luaintf" ;; esac
    vlcrc_set "$RC" core lua-intf pushmpc_boot
  fi
  sha_of "$RC" > "$RC.pushmpc-installed"
  say "PushMPC for VLC installed ($tgt): plugin in $DIR, settings in $RC"
}

sha_of() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }

# selftest_mac: VLC.app plays a generated clip (Y4M, 320x180, 2 s) headless with PushMPC at x2. Never fails the install.
selftest_mac() {
  paths_for macos
  vlc=/Applications/VLC.app/Contents/MacOS/VLC; [ -x "$vlc" ] || vlc="$HOME/Applications/VLC.app/Contents/MacOS/VLC"
  [ -x "$vlc" ] || { say "self-test: VLC.app not found"; return 0; }
  clip="$DIR/pushmpc-selftest.y4m"; log="$DIR/pushmpc-selftest.log"
  perl -e 'open(my $f, ">", $ARGV[0]) or die; binmode $f; print $f "YUV4MPEG2 W320 H180 F24:1 Ip A1:1 C420jpeg\n";
           for my $n (0..47) { print $f "FRAME\n"; for my $y (0..179) { print $f pack("C*", map { ($_ + int($y / 2) + 4 * $n) & 255 } 0..319); }
                               print $f chr(128) x (160 * 90 * 2); }' "$clip"
  rm -f "$log"
  env VLC_PLUGIN_PATH="$DIR" perl -e 'alarm 60; exec @ARGV' "$vlc" -I dummy --vout=dummy --no-audio --play-and-exit \
    --video-filter=postproc:pushmpc_out --pushmpc-smoother=2 --file-logging --logfile="$log" --log-verbose=2 "$clip" >/dev/null 2>&1 || true
  gen="$(sed -n 's/.*PushMPC: close, in=[0-9]* out=[0-9]* generated=\([0-9]*\).*/\1/p' "$log" 2>/dev/null | tail -1)"
  eng="$(grep -o 'PushMPC [0-9][^:]*: .*-> x[0-9]* ([^;)]*' "$log" 2>/dev/null | head -1 | sed 's/.*(//')"
  if [ "${gen:-0}" -gt 0 ]; then say "self-test (macos): PushMPC generated $gen frames at x2 (engine: ${eng:-unknown})"
  else say "self-test (macos): no generated frames ($(grep -o 'PushMPC[^\r]*' "$log" 2>/dev/null | tail -2 | tr '\n' ' '))"; fi
  rm -f "$clip"
}

# st_run <hw> <interop 0|1>: one self-test playback; 0 = PushMPC's hardware path worked and generated frames. Sets
# st_result. Interop: VLC's OpenGL output in a small window on the user's own display (DISPLAY: X11 or XWayland, as the
# Qt interface uses), so the real driver's GL interop is what is tested; otherwise headless (no display involved).
st_run() {
  hw="$1"; interop="$2"
  rm -f "$log"
  case "$tgt" in
    native)  set -- env VLC_PLUGIN_PATH="$DIR" "${VLC_BIN:-vlc}" --pushmpc-smoother=2 ;;
    flatpak) set -- flatpak run "$APPID" --pushmpc-smoother=2 ;;
    snap)    set -- snap run vlc ;;
  esac
  if [ "$interop" = 1 ]; then
    # X11 or XWayland, as the Qt interface uses (with WAYLAND_DISPLAY set, VLC would pick its Wayland window instead)
    set -- env -u WAYLAND_DISPLAY "$@" --vout=gl --width=160 --height=90 --no-video-deco --no-embedded-video
  else
    set -- "$@" --vout=dummy
  fi
  st_rc=0
  timeout 45 "$@" -I dummy --no-audio --play-and-exit --no-video-title-show --avcodec-hw="$hw" \
    --video-filter=postproc:pushmpc_out --file-logging --logfile="$log" --log-verbose=2 "$cdir/pushmpc-selftest.mkv" \
    >/dev/null 2>&1 || st_rc=$?
  st_result="$hw: no hardware decoding"
  [ -f "$log" ] || return 1
  grep -q "for hardware decoding" "$log" || return 1
  if [ "$interop" = 1 ] && ! grep -qE 'using glconv module "glconv_(vaapi|vdpau)' "$log"; then
    st_result="$hw: decoded, but the OpenGL output has no hardware interop here"; return 1
  fi
  gen="$(sed -n 's/.*PushMPC: close, in=[0-9]* out=[0-9]* generated=\([0-9]*\).*/\1/p' "$log" | tail -1)"
  if grep -q "PushMPC: hw path ok" "$log" && ! grep -q "hardware path unavailable" "$log" && [ "$st_rc" = 0 ] && [ "${gen:-0}" -gt 0 ]; then
    st_result="$(grep -o 'PushMPC: hw path ok.*' "$log" | head -1)"
    grep -q "PushMPC: zero copy on" "$log" && st_result="$st_result; zero copy"
    return 0
  fi
  st_result="$hw: $(grep -o 'hardware path unavailable.*' "$log" | head -1)"; [ "$st_rc" = 0 ] || st_result="$st_result (VLC exit $st_rc)"
  return 1
}

# selftest_hw <target>: the hardware self-test (see the header). Never fails the install. Tiers:
#   1. with a display: VA-API, then VDPAU, through VLC's OpenGL output with its hardware interop (the GUI's own path)
#      -> pass: the user's own avcodec-hw comes back (VLC picks the decoder as it always does)
#   2. headless: VA-API through VLC's DRM decoder (VLC downloads for the display), then VDPAU with X
#      -> pass: avcodec-hw is set to that decoder explicitly (the path that was proven)
#   3. neither: avcodec-hw=none (software decoding keeps PushMPC on)
selftest_hw() {
  tgt="$1"; paths_for "$tgt"
  [ "$tgt" = macos ] && return 0
  clip="$CLIP"; [ -n "$clip" ] || clip="$here/pushmpc-selftest.mkv"
  state="$DIR/hw-selftest.txt"
  if [ ! -f "$clip" ]; then
    say "hardware self-test ($tgt): no test clip next to the installer; software decoding stays on"
    return 0
  fi
  cdir="$DIR"   # inside what the (confined) VLC may read and write for all three targets
  mkdir -p "$cdir"; cp -f "$clip" "$cdir/pushmpc-selftest.mkv"
  log="$cdir/pushmpc-selftest.log"
  conf="$DIR/pushmpc.conf"
  # the snap has no registered PushMPC options (the Lua bootstrap): x2 goes through its pushmpc.conf for the test
  if [ "$tgt" = snap ]; then cp -f "$conf" "$conf.selftest" 2>/dev/null || : > "$conf.selftest"; printf 'smoother=2\n' >> "$conf"; fi
  tier=""; ok=""; result="no hardware decoder took the clip"; notes=""
  if [ -n "${DISPLAY:-}" ]; then
    say "hardware self-test ($tgt): a small VLC window opens for a few seconds (the test of VLC's own display path)"
    for hw in vaapi vdpau; do
      if st_run "$hw" 1; then tier=interop; ok="$hw"; result="$st_result"; break; fi
      notes="$notes; $st_result"
    done
  fi
  if [ -z "$tier" ]; then
    for hw in vaapi_drm vdpau; do
      [ "$hw" = vdpau ] && [ -z "${DISPLAY:-}" ] && continue   # VLC's VDPAU decoder needs X11
      if st_run "$hw" 0; then tier=direct; ok="$hw"; result="$st_result"; break; fi
      notes="$notes; $st_result"
    done
  fi
  [ "$tgt" = snap ] && mv -f "$conf.selftest" "$conf"
  rm -f "$cdir/pushmpc-selftest.mkv"
  now="$(date -u +%Y-%m-%dT%H:%MZ 2>/dev/null)"
  if [ "$tier" = interop ]; then
    old="$(vlcrc_get "$RC.pushmpc-backup" avcodec avcodec-hw)"
    # the user's own setting comes back (one that was already "none" stays: their choice)
    if [ -n "$old" ]; then vlcrc_set "$RC" avcodec avcodec-hw "$old"; else vlcrc_del "$RC" avcodec avcodec-hw; fi
    printf 'ok interop %s %s\n%s\n' "$ok" "$now" "$result" > "$state"
    say "hardware self-test ($tgt): passed through $ok with VLC's OpenGL interop; hardware decoding is on with PushMPC ($result)"
  elif [ "$tier" = direct ]; then
    old="$(vlcrc_get "$RC.pushmpc-backup" avcodec avcodec-hw)"
    if [ "$old" = none ]; then vlcrc_set "$RC" avcodec avcodec-hw none; else vlcrc_set "$RC" avcodec avcodec-hw "$ok"; fi
    printf 'ok direct %s %s\n%s\n%s\n' "$ok" "$now" "$result" "${notes#; }" > "$state"
    say "hardware self-test ($tgt): passed through $ok (decoder only, VLC copies for the display); avcodec-hw=$ok ($result)"
  else
    vlcrc_set "$RC" avcodec avcodec-hw none
    printf 'failed %s\n%s\n' "$now" "${notes#; }" > "$state"
    say "hardware self-test ($tgt): not passed (${notes#; }); software decoding stays on"
  fi
  return 0
}

uninstall_one() {
  tgt="$1"; paths_for "$tgt"
  rm -f "$DIR/libpushmpc_plugin.so" "$DIR/libpushmpc_plugin.dylib" "$DIR/hw-selftest.txt" "$DIR/pushmpc-selftest.log" \
        "$DIR/libpushmpc_macosx_plugin.dylib" "$DIR/libMoltenVK.dylib" "$DIR/pushmpc-selftest.y4m"
  rmdir "$DIR" 2>/dev/null || true
  case "$tgt" in
    native)
      rm -f "$HOME/.config/environment.d/60-pushmpc-vlc.conf"
      grep -q '^X-PushMPC=1' "$HOME/.local/share/applications/vlc.desktop" 2>/dev/null && rm -f "$HOME/.local/share/applications/vlc.desktop" ;;
    flatpak) flatpak override --user --unset-env=VLC_PLUGIN_PATH "$APPID" 2>/dev/null || true ;;
    snap)
      rm -f "$DIR/pushmpc_boot.lua" "$DIR/pushmpc.conf"; rmdir "$DIR" 2>/dev/null || true
      for d in "$HOME"/snap/vlc/*/.local/share/vlc/lua/intf; do rm -f "$d/pushmpc_boot.lua"; done
      if [ -f "$RC" ]; then
        rest="$(vlcrc_get "$RC" core extraintf | tr ':' '\n' | grep -v '^luaintf$' | paste -s -d: - || true)"
        if [ -n "$rest" ]; then vlcrc_set "$RC" core extraintf "$rest"; else vlcrc_del "$RC" core extraintf; fi
        vlcrc_del "$RC" core lua-intf
      fi ;;
    macos)   rm -f "$HOME/Library/LaunchAgents/$LAUNCH_LABEL.plist"; launchctl unsetenv VLC_PLUGIN_PATH 2>/dev/null || true ;;
  esac
  # nothing changed vlcrc since install: the file as it was, byte for byte (or no file, when install created it)
  if [ -f "$RC" ] && [ -f "$RC.pushmpc-installed" ] && [ "$(sha_of "$RC")" = "$(cat "$RC.pushmpc-installed")" ]; then
    if [ -f "$RC.pushmpc-absent" ]; then rm -f "$RC"; elif [ -f "$RC.pushmpc-backup" ]; then mv -f "$RC.pushmpc-backup" "$RC"; fi
    rm -f "$RC.pushmpc-installed" "$RC.pushmpc-absent" "$RC.pushmpc-backup"
    rmdir "$(dirname "$RC")" 2>/dev/null || true
    say "PushMPC for VLC removed ($tgt); $RC as it was"
    return 0
  fi
  rm -f "$RC.pushmpc-installed" "$RC.pushmpc-absent"
  if [ -f "$RC" ]; then
    [ "$tgt" = macos ] && [ "$(vlcrc_get "$RC" core vout)" = "pushmpc_macosx,any" ] && vlcrc_del "$RC" core vout
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
             for t in $TARGETS; do
               install_one "$t"
               if [ "$HWTEST" = 1 ] && [ "$OS" != Darwin ]; then selftest_hw "$t"; fi
               if [ "$HWTEST" = 1 ] && [ "$t" = macos ]; then selftest_mac; fi
             done ;;
  selftest-hw) for t in $TARGETS; do selftest_hw "$t"; done ;;
  uninstall) for t in ${TARGETS:-native flatpak macos}; do uninstall_one "$t"; done ;;
  status)    for t in $TARGETS; do paths_for "$t"; say "$t: plugin $(ls "$DIR"/libpushmpc_plugin.* 2>/dev/null || echo missing); video-filter=$(vlcrc_get "$RC" core video-filter)"; done ;;
  *) echo "usage: $0 install|uninstall|status [--plugin file] [--target auto|native|flatpak|macos]" >&2; exit 2 ;;
esac
