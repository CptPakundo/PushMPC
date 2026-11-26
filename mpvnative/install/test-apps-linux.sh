#!/bin/sh
# test-apps-linux.sh [<folder with the real pushmpc_hook.so, libpushmpc-avfilter-*.so, libpushmpc_engine.so>]
# pushmpc-apps-linux.sh against fake Snap / Flatpak / Emby Theater installs (stub snap and flatpak commands and a stub
# self-test that "plays" only when the libavfilter build it asks for was copied): install, status, uninstall, and that
# uninstall leaves every file and override exactly as before; a player whose FFmpeg has no build is left unchanged.
# Runs anywhere with a POSIX sh (CI on Linux; also Git Bash). Exit 0 = all checks passed.
set -u
here=$(cd "$(dirname "$0")" && pwd)
T=${TMPDIR:-/tmp}/pm-apps-test.$$;mkdir -p "$T/bin" "$T/from" "$T/kit" "$T/home"
fails=0
ok() { echo "ok: $*"; }
bad() { echo "FAIL: $*"; fails=$((fails + 1)); }

# the files (real ones when given, else empty stand-ins)
if [ -n "${1:-}" ] && [ -f "$1/pushmpc_hook.so" ]; then cp "$1"/pushmpc_hook.so "$1"/libpushmpc_engine.so "$1"/libpushmpc-avfilter-*.so "$T/from/"
else for f in pushmpc_hook.so libpushmpc_engine.so libpushmpc-avfilter-8.29.100.so libpushmpc-avfilter-10.4.100.so; do : > "$T/from/$f"; done; fi
: > "$T/from/libpushmpc-avfilter-10.4.100.so"
: > "$T/kit/pushmpc_preload.so"
cat > "$T/kit/pushmpc-plex-selftest" <<'EOF'
#!/bin/sh
v=${FAKE_VER:-10.4.100}
if [ -n "${PUSHMPC_HOST_SCRIPT:-}" ]; then d=$(dirname "$PUSHMPC_HOST_SCRIPT"); tag=pushmpc_hook; else p=${FAKE_LD_PRELOAD:-${LD_PRELOAD:-}}; d=$(dirname "${p%%:*}"); tag=pushmpc_preload; fi
[ -f "$1" ] || [ "${1#/app/}" != "$1" ] || { echo "host: cannot load $1"; exit 2; }
if [ -f "$d/libpushmpc-avfilter-$v.so" ]; then echo "[$tag] libavfilter $v, libpushmpc-avfilter-$v.so" >&2; echo "host: [pushmpc] 48 frames -> 96, x2;"; exit 0; fi
echo "[$tag] not active: no build for this mpv's libavfilter $v (cannot open shared object file); " >&2; exit 3
EOF
chmod +x "$T/kit/pushmpc-plex-selftest"

# stub snap: snaps in $FAKE_SNAPS; run --shell <snap> -c <cmd> runs <cmd> with the Snap's variables
cat > "$T/bin/snap" <<'EOF'
#!/bin/sh
case $1 in
list) for s in $FAKE_SNAPS; do [ "$s" = "$2" ] && exit 0; done; exit 1 ;;
run) [ "$2" = --shell ] || exit 1; sn=$3; shift 4; SNAP_USER_COMMON=$HOME/snap/$sn/common exec sh -c "$1" ;;
esac
exit 1
EOF
# stub flatpak: apps in $FAKE_FLATPAKS; overrides in the usual keyfile; run --command=sh [--env=K=V]... <id> -c <cmd>
cat > "$T/bin/flatpak" <<'EOF'
#!/bin/sh
ovd=${XDG_DATA_HOME:-$HOME/.local/share}/flatpak/overrides
has() { for a in $FAKE_FLATPAKS; do [ "$a" = "$1" ] && return 0; done; return 1; }
case $1 in
info) shift; [ "$1" = --user ] && shift; has "$1" ;;
list) for a in $FAKE_FLATPAKS; do echo "$a"; done ;;
override)
    shift; [ "$1" = --user ] && shift
    if [ "$1" = --show ]; then cat "$ovd/$2" 2>/dev/null; exit 0; fi
    envs=''; while [ "${1#--env=}" != "$1" ]; do envs="$envs ${1#--env=}"; shift; done
    f=$ovd/$1; mkdir -p "$ovd"; [ -f "$f" ] || printf '[Environment]\n' > "$f"
    for kv in $envs; do k=${kv%%=*}; grep -v "^$k=" "$f" > "$f.t"; mv "$f.t" "$f"; echo "$kv" >> "$f"; done ;;
run)
    shift; [ "$1" = --command=sh ] && shift
    # LD_PRELOAD goes in under another name: the stand-in preload is not a real library
    while [ "${1#--env=}" != "$1" ]; do kv=${1#--env=}; case $kv in LD_PRELOAD=*) kv=FAKE_$kv ;; esac; export "$kv"; shift; done
    shift; [ "$1" = -c ] && shift
    case $1 in *'find /app'*) echo /app/lib/libmpv.so.2; exit 0 ;; esac
    exec sh -c "$1" ;;
esac
EOF
chmod +x "$T/bin/snap" "$T/bin/flatpak"

export HOME=$T/home PATH="$T/bin:$PATH" PUSHMPC_SNAP_ROOT=$T/snaproot PUSHMPC_EMBY_DIR=$T/emby PUSHMPC_STREMIO_BIN=$T/stremio/stremio PUSHMPC_STREMIO_LIB=$T/stremio/libmpv.so.1 PUSHMPC_APPS_DESKTOP_DIRS=$T/apps PUSHMPC_PRELOAD_VAR=FAKE_LD_PRELOAD
unset XDG_CONFIG_HOME XDG_DATA_HOME
export FAKE_SNAPS="plex-desktop emby-theater" FAKE_FLATPAKS="com.stremio.Stremio org.jellyfin.JellyfinDesktop media.emby.EmbyTheater"
for s in plex-desktop emby-theater; do mkdir -p "$T/snaproot/$s/current/lib" "$HOME/snap/$s/current" "$HOME/snap/$s/common"; : > "$T/snaproot/$s/current/lib/libmpv.so.2"; done
mkdir -p "$T/emby/lib" && : > "$T/emby/lib/libmpv.so.1"
mkdir -p "$T/stremio" "$T/apps" && : > "$T/stremio/stremio" && : > "$T/stremio/libmpv.so.1" && printf '[Desktop Entry]
Name=Stremio
Exec=/opt/stremio/stremio %%U
' > "$T/apps/smartcode-stremio.desktop"
# a user mpv.conf in Plex's Snap folder and in ~/.config/mpv (kept byte for byte), and one other override key
mkdir -p "$HOME/snap/plex-desktop/current/.local/share/plex" "$HOME/.config/mpv" "$HOME/.local/share/flatpak/overrides"
printf 'sub-font-size=40\r\n' > "$HOME/snap/plex-desktop/current/.local/share/plex/mpv.conf"
printf 'volume=80\n' > "$HOME/.config/mpv/mpv.conf"
printf '[Context]\nfilesystems=xdg-videos;\n' > "$HOME/.local/share/flatpak/overrides/com.stremio.Stremio"

snapshot() { (cd "$HOME" && find . -type f | LC_ALL=C sort | while IFS= read -r f; do printf '%s %s\n' "$(cksum < "$f")" "$f"; done); }
I="sh $here/pushmpc-apps-linux.sh"
before=$(snapshot)

out=$($I install --from "$T/from" --kit "$T/kit" --json 2>"$T/log"); rc=$?
echo "$out"
for t in plex-snap emby-snap stremio-flatpak jellyfin-desktop-flatpak emby-flatpak emby stremio; do
    echo "$out" | grep -q "\"target\":\"$t\".*\"state\":\"installed\"" && ok "$t installed" || bad "$t not installed"
done
[ "$rc" = 0 ] && ok "install exit 0" || bad "install exit $rc"
grep -q "scripts-append=$HOME/snap/plex-desktop/common/pushmpc/pushmpc_hook.so" "$HOME/snap/plex-desktop/current/.local/share/plex/mpv.conf" && ok "Snap Plex mpv.conf loads the hook" || bad "Snap Plex mpv.conf"
grep -q "sub-font-size=40" "$HOME/snap/plex-desktop/current/.local/share/plex/mpv.conf" && ok "user line kept" || bad "user line lost"
grep -q "scripts-append=$HOME/snap/plex-desktop/common/pushmpc/pushmpc_hook.so" "$HOME/snap/plex-desktop/common/mpv.conf" && ok "Snap Plex common/mpv.conf loads the hook" || bad "Snap Plex common/mpv.conf"
[ -f "$HOME/snap/plex-desktop/common/pushmpc/libpushmpc-avfilter-10.4.100.so" ] && ok "the matching build copied" || bad "build not copied"
[ "$(ls "$HOME/snap/plex-desktop/common/pushmpc"/libpushmpc-avfilter-*.so | wc -l)" = 1 ] && ok "only that build" || bad "more than one build copied"
grep -q "^LD_PRELOAD=$HOME/.var/app/com.stremio.Stremio/pushmpc/pushmpc_preload.so$" "$HOME/.local/share/flatpak/overrides/com.stremio.Stremio" \
    && grep -q '^PUSHMPC_PRELOAD_VF=auto$' "$HOME/.local/share/flatpak/overrides/com.stremio.Stremio" && ok "Stremio override" || bad "Stremio override"
grep -q "^# >>> PushMPC (Emby Theater)" "$HOME/.config/mpv/mpv.conf" && ok "Emby block in ~/.config/mpv/mpv.conf" || bad "Emby block"
grep -q "^Exec=env LD_PRELOAD=$HOME/.local/share/pushmpc/stremio/pushmpc_preload.so PUSHMPC_PRELOAD_VF=auto /opt/stremio/stremio %U$" "$HOME/.local/share/applications/smartcode-stremio.desktop" && ok "Stremio menu entry" || bad "Stremio menu entry"

st=$($I status --json 2>/dev/null)
[ "$(echo "$st" | grep -c '"state":"installed"')" = 7 ] && ok "status: 7 installed" || { echo "$st"; bad "status"; }

# a second install is idempotent (one block, backups kept)
$I install --from "$T/from" --kit "$T/kit" --json >/dev/null 2>&1
[ "$(grep -c '^# >>> PushMPC (Plex Snap)' "$HOME/snap/plex-desktop/current/.local/share/plex/mpv.conf")" = 1 ] && ok "reinstall: one block" || bad "reinstall: duplicate block"

$I uninstall --json >/dev/null 2>&1
after=$(snapshot)
[ "$before" = "$after" ] && ok "uninstall restored every file and override exactly" || { echo "$before" > "$T/b.txt"; echo "$after" > "$T/a.txt"; diff "$T/b.txt" "$T/a.txt"; bad "uninstall left differences"; }

# no build for the player's FFmpeg: nothing changes, the reason is reported
out=$(FAKE_VER=99.1.100 $I install --from "$T/from" --kit "$T/kit" --json 2>/dev/null); rc=$?
echo "$out" | grep -q '"target":"stremio-flatpak".*"state":"not-active".*libavfilter 99.1.100' && ok "no build: reported" || { echo "$out"; bad "no build: not reported"; }
[ "$rc" = 3 ] && ok "no build: exit 3" || bad "no build: exit $rc"
[ "$(snapshot)" = "$before" ] && ok "no build: nothing changed" || bad "no build: files changed"

# the native hook block already in ~/.config/mpv/mpv.conf: Emby Theater shares it, nothing written
mkdir -p "$HOME/.local/share/pushmpc/mpvnative"; cp "$T/from/"* "$HOME/.local/share/pushmpc/mpvnative/"
printf '# >>> PushMPC native: do not edit between these lines (PushMPC uninstall removes them)\nscripts-append=x\n# <<< PushMPC native\nvolume=80\n' > "$HOME/.config/mpv/mpv.conf"
c0=$(cksum < "$HOME/.config/mpv/mpv.conf")
out=$($I install --target emby --from "$T/from" --kit "$T/kit" --json 2>/dev/null)
echo "$out" | grep -q '"state":"installed".*shares the native hook block' && [ "$(cksum < "$HOME/.config/mpv/mpv.conf")" = "$c0" ] && ok "Emby shares the native block" || { echo "$out"; bad "Emby with the native block"; }

[ "${KEEP:-0}" = 1 ] || rm -rf "${T:?}"
[ "$fails" = 0 ] && echo "test-apps-linux: all checks passed" || { echo "test-apps-linux: $fails failed"; exit 1; }
