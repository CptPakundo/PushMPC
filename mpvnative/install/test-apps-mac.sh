#!/bin/sh
# test-apps-mac.sh: pushmpc-apps-mac.sh's app route against fake app bundles (stub codesign / defaults / xattr and a stub
# self-test host that writes the bridge's log lines): libmpv as libmpv.2.dylib (Stremio-like), libmpv inside a
# framework (Mpv.framework/Versions/A/Mpv), a player built into the app (capture mode is named, nothing changes), an
# app with team-bound entitlements (refused). Install, status, uninstall; every bundle byte-identical afterwards.
# Runs anywhere with a POSIX sh (macOS CI; also Linux and Git Bash). Exit 0 = all checks passed.
set -u
here=$(cd "$(dirname "$0")" && pwd)
T=${TMPDIR:-/tmp}/pm-apps-mac-test.$$; mkdir -p "$T/bin" "$T/from/apps" "$T/root/Applications" "$T/home"
fails=0
ok() { echo "ok: $*"; }
bad() { echo "FAIL: $*"; fails=$((fails + 1)); }

# the stand-in (only its marker matters here) and the bridge
printf 'stand-in pushmpc_mpv_proxy_status\n' > "$T/from/apps/libmpv-proxy.dylib"
: > "$T/bridge.so"
BANNER='Copyright 2000-2026 mpv/MPlayer/mplayer2 projects'
# stub self-test host: "plays" only through our stand-in, and logs what the bridge would
cat > "$T/from/pushmpc-plex-selftest" <<'EOF'
#!/bin/sh
grep -q pushmpc_mpv_proxy_status "$1" || { echo "host: $1 is not the stand-in"; exit 3; }
[ -f "$(dirname "$1")/pushmpc-orig-libmpv.dylib" ] || { echo "host: cannot load the original"; exit 2; }
grep -q '^script=' "$(dirname "$1")/pushmpc-autoload.conf" || { echo "host: no autoload conf"; exit 3; }
mkdir -p "$HOME/Library/Logs/PushMPC"
# the bridge's FFmpeg-major guard: an app that bundles libavutil.<N>.dylib runs only pushmpc-iina-av<N>.so
c=$(dirname "$1"); while [ "$(basename "$c")" != Contents ] && [ "$c" != / ]; do c=$(dirname "$c"); done
m=$(find "$c" -name 'libavutil.*.dylib' | sed -n 's/.*libavutil\.\([0-9][0-9]*\)\..*dylib$/\1/p' | head -n 1)
br=$(sed -n 's/^script=//p' "$(dirname "$1")/pushmpc-autoload.conf")
if [ -n "$m" ] && [ "$(basename "$br")" != "pushmpc-iina-av$m.so" ]; then
    printf 'pushmpc-iina: not active: FFmpeg major versions differ from this build (libavutil %s vs 61)\n' "$m" >> "$HOME/Library/Logs/PushMPC/iina-bridge.log"
    echo "host: y4m: vf accepted; end of file reason 0 (0 = eof)"; exit 0
fi
printf 'pushmpc-iina: engine: portable-cpu (built in)\npushmpc-iina: graph closed: 48 frames in, 96 out (x2)\n' >> "$HOME/Library/Logs/PushMPC/iina-bridge.log"
echo "host: y4m: vf accepted; end of file reason 0 (0 = eof)"
EOF
chmod +x "$T/from/pushmpc-plex-selftest"
# stubs: codesign (flags / entitlements from a file in the bundle), defaults, xattr, shasum when missing
cat > "$T/bin/codesign" <<'EOF'
#!/bin/sh
for a; do b=$a; done
case "$*" in
*--entitlements*) cat "$b/Contents/fake-entitlements.plist" 2>/dev/null ;;
*-dv*) echo "CodeDirectory v=20500 size=1 flags=$(cat "$b/Contents/fake-flags" 2>/dev/null || echo '0x0(none)') hashes=1" >&2 ;;
esac
exit 0
EOF
printf '#!/bin/sh\n[ "$1" = read ] && [ "$3" = CFBundleName ] && { basename "${2%%%%/Contents/Info}" .app; exit 0; }\nexit 1\n' > "$T/bin/defaults"
printf '#!/bin/sh\nexit 0\n' > "$T/bin/xattr"
chmod +x "$T/bin/codesign" "$T/bin/defaults" "$T/bin/xattr"
if ! command -v shasum >/dev/null 2>&1; then printf '#!/bin/sh\nshift 2; sha256sum "$@"\n' > "$T/bin/shasum"; chmod +x "$T/bin/shasum"; fi

A=$T/root/Applications
# Stremio-like: Contents/Frameworks/libmpv.2.dylib plus a symlink, no hardened runtime
mkdir -p "$A/Stremio.app/Contents/Frameworks" "$A/Stremio.app/Contents/MacOS"
printf 'libmpv %s mpv_create\n' "$BANNER" > "$A/Stremio.app/Contents/Frameworks/libmpv.2.dylib"
ln -s libmpv.2.dylib "$A/Stremio.app/Contents/Frameworks/libmpv.dylib"
haslink=0; [ -L "$A/Stremio.app/Contents/Frameworks/libmpv.dylib" ] && haslink=1   # Git Bash copies instead
printf 'calls mpv_create\n' > "$A/Stremio.app/Contents/MacOS/Stremio"
printf 'ffmpeg\n' > "$A/Stremio.app/Contents/Frameworks/libavfilter.10.dylib"
# Jellyfin-like: libmpv inside a framework, hardened runtime with library validation off
J="$A/Jellyfin Desktop.app/Contents"
mkdir -p "$J/Frameworks/Mpv.framework/Versions/A" "$J/MacOS"
printf 'framework %s mpv_create\n' "$BANNER" > "$J/Frameworks/Mpv.framework/Versions/A/Mpv"
printf 'qt\n' > "$J/Frameworks/QtCore"
printf 'calls mpv_create\n' > "$J/MacOS/Jellyfin Desktop"
printf '0x10000(runtime)' > "$J/fake-flags"
printf '<plist><dict><key>com.apple.security.cs.disable-library-validation</key><true/></dict></plist>' > "$J/fake-entitlements.plist"

sums() { (cd "$A" && find . -type f | LC_ALL=C sort | while IFS= read -r f; do printf '%s %s\n' "$(cksum < "$f")" "$f"; done; find . -type l | LC_ALL=C sort); }
before=$(sums)
export HOME=$T/home PATH="$T/bin:$PATH" PUSHMPC_APPS_MAC_ROOT=$T/root
I="sh $here/pushmpc-apps-mac.sh"

out=$($I install --from "$T/from" --bridge "$T/bridge.so" --json 2>"$T/log"); rc=$?
echo "$out"
for t in stremio jellyfin-desktop; do
    if echo "$out" | grep -q "\"target\":\"$t\".*\"state\":\"installed\".*48 frames in, 96 out"; then ok "$t installed"; else bad "$t not installed"; fi
done
[ "$rc" = 0 ] && ok "install exit 0" || bad "install exit $rc"
grep -q pushmpc_mpv_proxy_status "$J/Frameworks/Mpv.framework/Versions/A/Mpv" && [ -f "$J/Frameworks/Mpv.framework/Versions/A/pushmpc-orig-libmpv.dylib" ] \
    && ok "framework binary: the stand-in in its place, the original beside it" || bad "framework binary not replaced"
grep -q "^script=$T/bridge.so$" "$A/Stremio.app/Contents/Frameworks/pushmpc-autoload.conf" && ok "autoload conf names the bridge" || bad "autoload conf"
if [ "$haslink" = 1 ]; then [ -L "$A/Stremio.app/Contents/Frameworks/libmpv.dylib" ] && ok "symlink kept" || bad "symlink lost"; fi
st=$($I status --json 2>/dev/null)
[ "$(echo "$st" | grep -c '"state":"installed"')" = 2 ] && ok "status: 2 installed" || { echo "$st"; bad "status"; }
$I install --from "$T/from" --bridge "$T/bridge.so" --json >/dev/null 2>&1
[ ! -f "$A/Stremio.app/Contents/Frameworks/pushmpc-orig-pushmpc-orig-libmpv.dylib" ] && grep -q "$BANNER" "$A/Stremio.app/Contents/Frameworks/pushmpc-orig-libmpv.dylib" \
    && ok "reinstall keeps the original" || bad "reinstall lost the original"
$I uninstall --json >/dev/null 2>&1
[ "$(sums)" = "$before" ] && ok "uninstall: both bundles byte-identical" || { echo "$before" > "$T/b"; sums > "$T/a"; diff "$T/b" "$T/a"; bad "uninstall left differences"; }
[ ! -d "$HOME/Library/Application Support/PushMPC/apps-mac" ] && ok "no state left" || bad "state left"

# a player built into the app: capture mode is named, nothing changes
rm -rf "$A/Stremio.app/Contents/Frameworks"/libmpv*
printf 'static %s mpv_create\n' "$BANNER" > "$A/Stremio.app/Contents/MacOS/Stremio"
b2=$(sums)
out=$($I install --target stremio --from "$T/from" --bridge "$T/bridge.so" --json 2>/dev/null); rc=$?
echo "$out" | grep -q '"state":"capture-mode".*built into the app.*pushmpc-capture --player \\"Stremio\\"' && ok "static: capture mode named" || { echo "$out"; bad "static: no capture mode"; }
[ "$rc" = 3 ] && ok "static without the capture host: exit 3" || bad "static: exit $rc"
printf '#!/bin/sh\nexit 0\n' > "$T/bin/pushmpc-capture"; chmod +x "$T/bin/pushmpc-capture"
out=$($I install --target stremio --from "$T/from" --bridge "$T/bridge.so" --json 2>/dev/null); rc=$?
echo "$out" | grep -q "\"state\":\"capture-mode\".*$T/bin/pushmpc-capture --player" && [ "$rc" = 0 ] && ok "static with the capture host: its command, exit 0" || { echo "$out"; bad "static with capture host (exit $rc)"; }
[ "$(sums)" = "$b2" ] && ok "static: nothing changed" || bad "static: files changed"

# libmpv.2.dylib next to the program in Contents/MacOS, nothing in Frameworks (as Stremio 5.1.26 was taken for a built-in
# player, players run 45958850658): found, installed, undone byte for byte
printf 'calls mpv_create\n' > "$A/Stremio.app/Contents/MacOS/Stremio"
printf 'libmpv %s mpv_create\n' "$BANNER" > "$A/Stremio.app/Contents/MacOS/libmpv.2.dylib"
b4=$(sums)
out=$($I install --target stremio --from "$T/from" --bridge "$T/bridge.so" --json 2>/dev/null)
echo "$out" | grep -q '"target":"stremio".*"state":"installed".*48 frames in, 96 out' && grep -q pushmpc_mpv_proxy_status "$A/Stremio.app/Contents/MacOS/libmpv.2.dylib" \
    && ok "libmpv in Contents/MacOS: installed" || { echo "$out"; bad "libmpv in Contents/MacOS: not installed"; }
$I uninstall --json >/dev/null 2>&1
[ "$(sums)" = "$b4" ] && ok "libmpv in Contents/MacOS: uninstall byte-identical" || bad "libmpv in Contents/MacOS: uninstall left differences"

# one bridge per FFmpeg major: the app's bundled libavutil.<N>.dylib picks pushmpc-iina-av<N>.so (Stremio 5.1.26 bundles
# FFmpeg 7 = libavutil 59, Homebrew has 61: players run 45964178745); no build for its major: not active, unchanged
mkdir -p "$T/br"; : > "$T/br/pushmpc-iina.so"
for n in 56 58 59 60 61; do : > "$T/br/pushmpc-iina-av$n.so"; done   # Homebrew's bottled majors (ffmpeg@4, @6, @7, @8, ffmpeg)
rm -f "$A/Stremio.app/Contents/MacOS/libmpv.2.dylib"
mkdir -p "$A/Stremio.app/Contents/Frameworks"
printf 'libmpv %s mpv_create\n' "$BANNER" > "$A/Stremio.app/Contents/Frameworks/libmpv.2.dylib"
for n in 56 58 59 60 61; do
    rm -f "$A/Stremio.app/Contents/Frameworks"/libavutil.*.dylib; printf 'avutil\n' > "$A/Stremio.app/Contents/Frameworks/libavutil.$n.dylib"
    b5=$(sums)
    out=$($I install --target stremio --from "$T/from" --bridge "$T/br/pushmpc-iina.so" --json 2>/dev/null)
    echo "$out" | grep -q "\"target\":\"stremio\".*\"state\":\"installed\".*bridge pushmpc-iina-av$n.so for libavutil $n.*48 frames in" \
        && grep -q "^script=$T/br/pushmpc-iina-av$n.so$" "$A/Stremio.app/Contents/Frameworks/pushmpc-autoload.conf" \
        && ok "libavutil $n: its bridge, active" || { echo "$out"; bad "libavutil $n: not installed with its bridge"; }
    $I uninstall --json >/dev/null 2>&1
    [ "$(sums)" = "$b5" ] && ok "libavutil $n: uninstall byte-identical" || bad "libavutil $n: uninstall left differences"
done
rm -f "$A/Stremio.app/Contents/Frameworks"/libavutil.*.dylib; printf 'avutil\n' > "$A/Stremio.app/Contents/Frameworks/libavutil.57.dylib"
b6=$(sums)
out=$($I install --target stremio --from "$T/from" --bridge "$T/br/pushmpc-iina.so" --json 2>/dev/null); rc=$?
echo "$out" | grep -q '"target":"stremio".*"state":"not-active".*libavutil in the app: 57.*FFmpeg major versions differ' && [ "$rc" = 3 ] \
    && ok "libavutil 57 (ffmpeg@5, deprecated in Homebrew: no bridge built for it): not active, the reason named" || { echo "$out"; bad "libavutil 57: no clean not-active (exit $rc)"; }
[ "$(sums)" = "$b6" ] && ok "libavutil 57: the app unchanged" || bad "libavutil 57: files changed"

# team-bound entitlements on a hardened app: refused, nothing changed
printf '<plist><dict><key>keychain-access-groups</key><array/></dict></plist>' > "$J/fake-entitlements.plist"
b3=$(sums)
out=$($I install --target jellyfin-desktop --from "$T/from" --bridge "$T/bridge.so" --json 2>/dev/null)
echo "$out" | grep -q '"state":"refused".*keychain-access-groups' && [ "$(sums)" = "$b3" ] && ok "team-bound entitlements: refused, unchanged" || { echo "$out"; bad "team-bound entitlements"; }

[ "${KEEP:-0}" = 1 ] || rm -rf "${T:?}"
[ "$fails" = 0 ] && echo "test-apps-mac: all checks passed" || { echo "test-apps-mac: $fails failed"; exit 1; }
