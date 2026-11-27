#!/bin/sh
# pushmpc-vlc-test.sh: pushmpc-vlc.sh's vlcrc handling on synthetic files, no VLC needed (POSIX sh: run it under dash
# and bash). Each case: a user's vlcrc before install (or none), install, then VLC rewrites the file as VLC does (a
# UTF-8 BOM, "[section] # description" headers, every unset option a commented default) and the user may change it,
# then uninstall; the case checks the keys the user had and that no PushMPC key is left.
# Exit 0 when every case passes.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
fails=0
PUSHMPC_VLC_LIB=1
. "$here/pushmpc-vlc.sh"
set +e

# vlc_rewrite <file>: the file as VLC writes it back: BOM + banner, each section header with a comment, its keys, and
# a commented default for every option it names in $VLC_DEFAULTS that is not set
vlc_rewrite() {
  { printf '\357\273\277###\n### vlc 3.0.24\n###\n\n'
    awk 'BEGIN { sec = "" } /^\[[^]]*\]/ { name = $0; sub(/^\[/, "", name); sub(/\].*$/, "", name); print "[" name "] # " name " module"; next } { print }' "$1"
    printf '\n[auhal] # HAL AudioUnit output\n#auhal-volume=256\nauhal-audio-device=0\n'
  } > "$1.vlc" && mv "$1.vlc" "$1"
}
# check <name> <section> <key> <want: a value, or "-" for no active key>
check() {
  got=$(vlcrc_get "$RC" "$2" "$3"); has=0; vlcrc_has "$RC" "$2" "$3" && has=1
  if [ "$4" = - ]; then
    if [ "$has" = 0 ]; then echo "ok   $1: [$2] $3 absent"; else echo "FAIL $1: [$2] $3=$got (want absent)"; fails=$((fails + 1)); fi
  elif [ "$has" = 1 ] && [ "$got" = "$4" ]; then echo "ok   $1: [$2] $3=$got"
  else echo "FAIL $1: [$2] $3=$got (has $has; want $4)"; fails=$((fails + 1)); fi
}
no_pushmpc_keys() {
  if [ -f "$RC" ] && vlcrc_active "$RC" | grep -qE '^pushmpc-|^vout=.*pushmpc|^video-filter=.*pushmpc_out'; then
    echo "FAIL $1: PushMPC keys left: $(vlcrc_active "$RC" | grep -E 'pushmpc' | tr '\n' ' ')"; fails=$((fails + 1))
  else echo "ok   $1: no PushMPC key left"; fi
}
# run_case <name> <target> <pre-install vlcrc content, or "none"> <after-install edit: a shell snippet, or ":">
run_case() {
  name=$1; tgt=$2; pre=$3; edit=$4
  HOME="$work/$name"; XDG_CONFIG_HOME="$HOME/.config"; export HOME XDG_CONFIG_HOME
  mkdir -p "$HOME/plug"; : > "$HOME/plug/libpushmpc_plugin.so"; : > "$HOME/plug/libpushmpc_macosx_plugin.dylib"
  PLUGIN="$HOME/plug/libpushmpc_plugin.so"
  paths_for "$tgt"
  if [ "$pre" != none ]; then mkdir -p "$(dirname "$RC")"; printf '%s\n' "$pre" > "$RC"; fi
  ( install_one "$tgt" ) > /dev/null 2>&1 || { echo "FAIL $name: install"; fails=$((fails + 1)); return; }
  vlc_rewrite "$RC"
  eval "$edit"
  ( uninstall_one "$tgt" ) > "$work/$name.out" 2>&1 || { echo "FAIL $name: uninstall"; cat "$work/$name.out"; fails=$((fails + 1)); return; }
}

# 1. the user had video-filter=postproc (VLC's own post-processing): it stays
run_case postproc native '[core]
video-filter=postproc' :
check postproc core video-filter postproc; no_pushmpc_keys postproc
# 2. the user had x:postproc
run_case xpostproc native '[core]
video-filter=sharpen:postproc' :
check xpostproc core video-filter sharpen:postproc; no_pushmpc_keys xpostproc
# 3. no video-filter before: the key goes
run_case nofilter native '[core]
fullscreen=1' :
check nofilter core video-filter -; check nofilter core fullscreen 1; no_pushmpc_keys nofilter
# 4. the user added a filter after install: it stays, PushMPC's go
run_case added native '[core]
video-filter=sharpen' 'vlcrc_set "$RC" core video-filter "$(vlcrc_get "$RC" core video-filter):gradfun"'
check added core video-filter sharpen:gradfun; no_pushmpc_keys added
# 5. no vlcrc before; VLC wrote its own setting since: the file stays with it, no PushMPC key
run_case novlcrc native none :
check novlcrc core video-filter -; check novlcrc auhal auhal-audio-device 0; no_pushmpc_keys novlcrc
# 6. a macOS user with their own vout and video-filter=postproc
run_case macvout macos '[core]
vout=caopengllayer
video-filter=postproc' :
check macvout core vout caopengllayer; check macvout core video-filter postproc; no_pushmpc_keys macvout
# 7. macOS, no vout before: PushMPC's output is taken out
run_case macnovout macos '[core]
fullscreen=1' :
check macnovout core vout -; no_pushmpc_keys macnovout
# 8. Linux: the user's avcodec-hw comes back
run_case hw native '[avcodec]
avcodec-hw=vaapi_drm' :
check hw avcodec avcodec-hw vaapi_drm; no_pushmpc_keys hw
# 9. the user changed vout after install (macOS): their choice stays
run_case macuser macos '[core]
fullscreen=1' 'vlcrc_set "$RC" core vout macosx'
check macuser core vout macosx; no_pushmpc_keys macuser

[ "$fails" = 0 ] && echo "pushmpc-vlc-test: all cases passed" || echo "pushmpc-vlc-test: $fails check(s) failed"
[ "$fails" = 0 ]
