#!/bin/sh
# pushmpc-apps-mac.sh: PushMPC inside Mac players that bring their own libmpv and read no mpv.conf (Stremio, Jellyfin
# Desktop / Jellyfin Media Player), and in the Jellyfin / Plex MPV shims (switched to Homebrew's mpv, which has
# VapourSynth). For pushmpc-setup (Homebrew) or by hand. POSIX sh, only tools every Mac has.
#
#   pushmpc-apps-mac.sh install|uninstall|status [--target auto|<name>] [--from DIR] [--bridge PATH] [--mpv PATH]
#                       [--integration PATH] [--vpy PATH] [--vsscript PATH] [--extra LINE]... [--agent] [--if-needed]
#                       [--dry-run] [--json]
#
#   --from        the Mac bundle (pushmpc-setup unpacks it to ~/.local/share/pushmpc/plex-mac): apps/libmpv-proxy.dylib
#                 and pushmpc-plex-selftest (default: this folder)
#   --bridge      PushMPC's mpv bridge, an mpv C plugin that runs PushMPC's engines inside a libmpv (Homebrew:
#                 <prefix>/lib/pushmpc/pushmpc-iina.so; default: looked up there)
#   --mpv --integration --vpy --vsscript --extra   for the shims: Homebrew's mpv, mpv-integration.sh, pushmpc.vpy,
#                 Homebrew mpv's VSScript helper script, extra lines for the block (pushmpc-setup passes them)
#   --agent       (install) also the LaunchAgent com.pushmpc.apps-reapply: at login and whenever one of these apps is
#                 replaced (an update) it runs "install --if-needed"; uninstall removes it
#   --json        one JSON object per target on stdout ({"target","action","state","detail"}), the log on stderr
#
# Targets:
#   stremio            /Applications/Stremio.app (Stremio 5)
#   jellyfin-desktop   /Applications/Jellyfin Desktop.app, or Jellyfin Media Player.app
#        The app's own libmpv is found in Contents/Frameworks: a libmpv*.dylib, else any library or framework binary
#        (Mpv.framework/Versions/A/Mpv, ...) that carries libmpv's code (exports mpv_create: nm when the Command Line
#        Tools are there, else libmpv's own copyright line). It is renamed pushmpc-orig-libmpv.dylib and a small stand-in
#        takes its name (apps/libmpv-proxy.dylib: every libmpv function goes to the original; after the app starts its
#        player, the stand-in loads the bridge and adds the filter, and puts it back if the app replaces the filter
#        list). Its settings are pushmpc-autoload.conf beside it. The original is backed up with its SHA-256 in
#        ~/Library/Application Support/PushMPC/apps-mac/<target>. A self-test plays a generated clip through the app's
#        own libmpv and the bridge; a target that does not pass is put back as it was, with the reason.
#        An app whose player is built into the app itself (libmpv inside its main program, no separate library) cannot
#        take the stand-in: install says so and names capture mode instead (pushmpc-capture --player "<app>", the
#        Homebrew formula's capture host: smoother motion over the app's window, nothing in the app changes).
#        Signature: an app that loads ad-hoc signed libraries (no hardened runtime, or library validation off) is
#        changed in place. An app with the hardened runtime and library validation on is backed up whole and signed
#        again ad hoc with its own entitlements (without the hardened runtime); uninstall puts the backup back. An app
#        whose entitlements an ad-hoc signature cannot carry (team-bound ones) is refused, with the reason.
#        What signing again means for you: macOS then sees the app as a different app from the same name. It may ask
#        once more for permissions you gave it before (network, folders, notifications), saved passwords or logins the
#        app kept in your Keychain may ask to be allowed again or need a new sign-in, and macOS no longer checks that
#        the app comes unchanged from its maker. The app's next update replaces it with the maker's signed version
#        again (the agent then puts PushMPC back). Uninstall puts the original, maker-signed app back.
#   jellyfin-mpv-shim, plex-mpv-shim
#        their conf.json (~/Library/Application Support/<shim>/conf.json): mpv_ext=true, mpv_ext_path=Homebrew's mpv,
#        mpv_ext_no_ovr=false (the shim starts that mpv with its own folder as the config folder), and the PushMPC block
#        in the mpv.conf beside it (mpv-integration.sh, plus scripts-append of the VSScript helper: with its own config
#        folder mpv skips Homebrew's global scripts). The shim must be closed (it writes conf.json when it quits).
# Uninstall: the original libmpv back (checked against its SHA-256) or the whole app from its backup, conf.json as it
# was (byte for byte when the shim did not change it since; else only our keys back to their old values), the block out.
# An app update replaces the stand-in: status says needs-reapply; install (or the agent) puts it back.
# If macOS refuses writing into an app (App Management), allow the terminal in System Settings > Privacy & Security >
# App Management. Tests: PUSHMPC_APPS_MAC_ROOT replaces / for the app folders.
# Exit: 0 done, 1 error, 2 usage, 3 none of these players found, or a target not supported / not passing its self-test.
# shellcheck disable=SC2329  # the do_* functions run through run_t
set -u

action=${1:-}; [ $# -gt 0 ] && shift
target=auto; from=''; bridge=''; mpv=''; integ=''; vpy=''; vsscript=''; extra=''; agent=0; ifneeded=0; dry=0; json=0
while [ $# -gt 0 ]; do
    case $1 in
        --target|--from|--bridge|--mpv|--integration|--vpy|--vsscript|--extra)
            [ $# -ge 2 ] || { echo "pushmpc-apps-mac.sh: $1 needs a value" >&2; exit 2; } ;;
    esac
    case $1 in
        --target) target=$2; shift 2 ;;
        --from) from=$2; shift 2 ;;
        --bridge) bridge=$2; shift 2 ;;
        --mpv) mpv=$2; shift 2 ;;
        --integration) integ=$2; shift 2 ;;
        --vpy) vpy=$2; shift 2 ;;
        --vsscript) vsscript=$2; shift 2 ;;
        --extra) extra="$extra$2
"; shift 2 ;;
        --agent) agent=1; shift ;;
        --if-needed) ifneeded=1; shift ;;
        --dry-run) dry=1; shift ;;
        --json) json=1; shift ;;
        *) echo "pushmpc-apps-mac.sh: unknown option $1" >&2; exit 2 ;;
    esac
done
case $action in install|uninstall|status) ;; *) sed -n '2,17p' "$0" >&2; exit 2 ;; esac
here=$(cd "$(dirname "$0")" && pwd)
[ -n "$from" ] || from=$here
R=${PUSHMPC_APPS_MAC_ROOT:-}
if [ -z "$bridge" ]; then for b in /opt/homebrew/lib/pushmpc/pushmpc-iina.so /usr/local/lib/pushmpc/pushmpc-iina.so; do [ -f "$b" ] && { bridge=$b; break; }; done; fi
if [ -z "$mpv" ]; then for b in /opt/homebrew/bin/mpv /usr/local/bin/mpv; do [ -x "$b" ] && { mpv=$b; break; }; done; fi
ST="$HOME/Library/Application Support/PushMPC/apps-mac"
LOGS="$HOME/Library/Logs/PushMPC"
VF='@pushmpc:lavfi=[null@pushmpc]'   # the bridge's marked graph

log() { printf '%s\n' "$*" >&2; }
jesc() { printf '%s' "$1" | tr '\n' ' ' | sed 's/\\/\\\\/g; s/"/\\"/g'; }
emit() { if [ "$json" = 1 ]; then printf '{"target":"%s","action":"%s","state":"%s","detail":"%s"}\n' "$1" "$action" "$2" "$(jesc "$3")"; else log "$1: $2${3:+ ($3)}"; fi; }
want() { [ "$target" = auto ] || [ "$target" = "$1" ]; }
sha() { [ -f "$1" ] && shasum -a 256 "$1" | awk '{print $1}'; }
is_proxy() { [ -f "$1" ] && grep -aq pushmpc_mpv_proxy_status "$1"; }

# ---------------------------------------------------------------------------------------------- apps: the libmpv stand-in
have_nm=0; [ "$(uname -s)" = Darwin ] && xcode-select -p >/dev/null 2>&1 && command -v nm >/dev/null 2>&1 && have_nm=1
is_libmpv() {   # is_libmpv <file>: libmpv's code is in it (it exports mpv_create; a program that only calls libmpv does not)
    if [ "$have_nm" = 1 ] && nm -gU "$1" 2>/dev/null | grep -q ' _mpv_create$'; then return 0; fi
    # without the Command Line Tools (nm would ask to install them), or a file nm cannot read (a stripped or unusual
    # binary): libmpv's own version banner, in no other file
    grep -a -q -F 'mpv/MPlayer/mplayer2 projects' "$1" 2>/dev/null && grep -a -q -F 'mpv_create' "$1" 2>/dev/null
}
app_lib() {   # the app's libmpv file (the real file, not a symlink): our stand-in, a libmpv*.dylib, or a framework binary
    fl=$(find "$1/Contents/Frameworks" -type f ! -name 'pushmpc-*' ! -name '*.plist' ! -name '*.conf' ! -name '*.png' \
        ! -name '*.icns' ! -name '*.nib' ! -name '*.strings' ! -path '*/_CodeSignature/*' 2>/dev/null | LC_ALL=C sort)
    while IFS= read -r f; do [ -n "$f" ] && is_proxy "$f" && { printf '%s\n' "$f"; return 0; }; done <<EOF
$fl
EOF
    while IFS= read -r f; do case $f in */libmpv*.dylib) printf '%s\n' "$f"; return 0 ;; esac; done <<EOF
$fl
EOF
    while IFS= read -r f; do [ -n "$f" ] && is_libmpv "$f" && { printf '%s\n' "$f"; return 0; }; done <<EOF
$fl
EOF
    return 1
}
app_static() {   # app_static <bundle>: the main program carries libmpv itself (nothing to stand in for)
    for f in "$1"/Contents/MacOS/*; do [ -f "$f" ] && is_libmpv "$f" && return 0; done; return 1
}
capture_bin() { for c in "$(command -v pushmpc-capture 2>/dev/null)" /opt/homebrew/bin/pushmpc-capture /usr/local/bin/pushmpc-capture; do [ -n "$c" ] && [ -x "$c" ] && { printf '%s\n' "$c"; return 0; }; done; return 1; }
sig_kind() {   # sig_kind <bundle>: plain (loads ad-hoc libraries) | resign (hardened, library validation on) | team:<entitlement>
    fl=$(codesign -dv "$1" 2>&1 | sed -n 's/.*flags=0x[0-9a-f]*(\([^)]*\)).*/\1/p' | head -n 1)
    case $fl in *runtime*) ;; *) echo plain; return ;; esac
    ent=$(codesign -d --entitlements - --xml "$1" 2>/dev/null || codesign -d --entitlements :- "$1" 2>/dev/null)
    if printf '%s' "$ent" | grep -q 'com.apple.security.cs.disable-library-validation'; then echo plain; return; fi
    tb=$(printf '%s' "$ent" | grep -o -E '<key>(com\.apple\.developer\.[^<]*|keychain-access-groups|com\.apple\.application-identifier|application-identifier)</key>' | head -n 1 | sed 's/<[^>]*>//g')
    if [ -n "$tb" ]; then echo "team:$tb"; else echo resign; fi
}
# bounded <seconds> <command...>: the command's output; killed after that many seconds (exit 143 then). macOS has no
# timeout(1), and perl's alarm would run through /usr/bin/perl, which SIP starts without DYLD_* variables
bounded() {
    _s=$1; shift; _o=$(mktemp "${TMPDIR:-/tmp}/pushmpc-st.XXXXXX")
    "$@" > "$_o" 2>&1 & _p=$!
    ( sleep "$_s"; kill "$_p" 2>/dev/null ) & _w=$!
    wait "$_p"; _r=$?
    kill "$_w" 2>/dev/null; wait "$_w" 2>/dev/null
    cat "$_o"; rm -f "$_o"
    return $_r
}
selftest_app() {   # selftest_app <libmpv path> <bundle>: 0 frames generated, 3 not, 2 no test possible; sets st_detail
    host=$from/pushmpc-plex-selftest
    [ -x "$host" ] || { st_detail='no self-test (pushmpc-plex-selftest not in the bundle)'; return 2; }
    blog=$LOGS/iina-bridge.log; mark=0; [ -f "$blog" ] && mark=$(wc -l < "$blog")
    # bounded: an engine that never finishes the clip must not hold the installer (exit 143)
    out=$(PUSHMPC_HOST_VF='' PUSHMPC_HOST_ACCEPT_EOF=1 DYLD_LIBRARY_PATH="$(dirname "$1"):$2/Contents/Frameworks" \
        bounded "${PUSHMPC_SELFTEST_SECONDS:-60}" "$host" "$1"); r=$?
    new=''; [ -f "$blog" ] && new=$(tail -n +$((mark + 1)) "$blog")
    eng=$(printf '%s\n' "$new" | sed -n 's/^pushmpc-iina: engine: \([^:(]*\).*/\1/p' | head -n 1 | sed 's/ *$//')
    fr=$(printf '%s\n' "$new" | sed -n 's/.*graph closed: \([0-9]*\) frames in, \([0-9]*\) out.*/\1 \2/p' | tail -n 1)
    fin=${fr%% *}; fout=${fr##* }
    if [ "$r" = 0 ] && [ -n "$fr" ] && [ "${fout:-0}" -gt "${fin:-0}" ]; then st_detail="$fin frames in, $fout out (engine: ${eng:-unknown})"; return 0; fi
    [ "$r" = 143 ] && out="$out
host: timed out after ${PUSHMPC_SELFTEST_SECONDS:-60} s (playback never reached the end of the clip)"
    st_detail="no frames generated (exit $r; $(printf '%s\n%s\n' "$out" "$new" | grep -E 'host: |not active|pass-through|refus|engine' | tail -n 4 | tr '\n' ' '))"
    [ "$r" = 2 ] && return 2
    return 3
}
write_conf() {   # write_conf <folder>
    # engine: the bundle's engine module (VideoToolbox, Vulkan on MoltenVK, CPU) for the bridge (PUSHMPC_ENGINE)
    { printf 'vf=%s\n' "$VF"; printf 'script=%s\n' "$bridge"; [ ! -f "$from/libpushmpc_engine.dylib" ] || printf 'engine=%s\n' "$from/libpushmpc_engine.dylib"; } > "$1/pushmpc-autoload.conf"
}
app_put_back() {   # app_put_back <bundle> <lib> <bk>: the original libmpv (or the whole app) as it was
    b=$1; l=$2; k=$3; d=$(dirname "$l")
    if [ -d "$k/app-backup.app" ]; then
        if [ "$(defaults read "$b/Contents/Info" CFBundleVersion 2>/dev/null)" = "$(cat "$k/app-version" 2>/dev/null)" ]; then
            rm -rf "$b.pushmpc-old" && mv "$b" "$b.pushmpc-old" || return 1
            if ! ditto "$k/app-backup.app" "$b"; then rm -rf "$b"; mv "$b.pushmpc-old" "$b"; return 1; fi
            rm -rf "$b.pushmpc-old"
            pb_detail='the app as it was (from its backup)'
        else pb_detail='the app was updated since: left as it is'; fi
        return 0
    fi
    if [ -f "$d/pushmpc-orig-libmpv.dylib" ] && { is_proxy "$l" || [ ! -f "$l" ]; }; then
        rm -f "$l" && mv "$d/pushmpc-orig-libmpv.dylib" "$l" || return 1
        if [ -f "$k/libmpv.sha256" ] && [ "$(sha "$l")" = "$(cat "$k/libmpv.sha256")" ]; then pb_detail="original $(basename "$l") back (SHA-256 matches)"
        else pb_detail="original $(basename "$l") back"; fi
    elif [ -f "$d/pushmpc-orig-libmpv.dylib" ]; then rm -f "$d/pushmpc-orig-libmpv.dylib"; pb_detail='the app was updated since: left as it is'
    else pb_detail='nothing of PushMPC in the app'; fi
    rm -f "$d/pushmpc-autoload.conf"
    return 0
}
do_app() {   # do_app <target> <bundle>
    name=$1; bundle=$2; bk=$ST/$name
    lib=$(app_lib "$bundle")
    installed=0
    [ -n "$lib" ] && is_proxy "$lib" && [ -f "$(dirname "$lib")/pushmpc-orig-libmpv.dylib" ] && installed=1
    case $action in
    status)
        if [ "$installed" = 1 ]; then emit "$name" installed "$bundle ($lib)"
        elif [ -d "$bk" ]; then emit "$name" needs-reapply "$bundle: the app was replaced (an update); install again"
        else emit "$name" not-installed "$bundle"; fi ;;
    install)
        if [ -z "$lib" ]; then
            an=$(defaults read "$bundle/Contents/Info" CFBundleName 2>/dev/null || basename "$bundle" .app)
            if app_static "$bundle"; then why="its player is built into the app itself, so PushMPC cannot sit inside it"
            else why="no mpv player found in the app"; fi
            if cb=$(capture_bin); then emit "$name" capture-mode "$why: use capture mode instead: $cb --player \"$an\" (smoother motion over the app's window; the app is not changed)"; return 0; fi
            emit "$name" capture-mode "$why: capture mode works instead (brew install CptPakundo/pushmpc/pushmpc, then pushmpc-capture --player \"$an\")"; return 3
        fi
        [ -f "$from/apps/libmpv-proxy.dylib" ] || { emit "$name" error "the files are not in $from"; return 1; }
        [ -n "$bridge" ] && [ -f "$bridge" ] || { emit "$name" unsupported "PushMPC's mpv bridge (pushmpc-iina.so) is not installed: brew install CptPakundo/pushmpc/pushmpc"; return 3; }
        if [ "$ifneeded" = 1 ]; then
            [ "$installed" = 1 ] && { emit "$name" installed "in place"; return 0; }
            [ -d "$bk" ] || { emit "$name" not-installed "not chosen"; return 0; }
        fi
        kind=plain; [ "$installed" = 1 ] || kind=$(sig_kind "$bundle")
        case $kind in team:*) emit "$name" refused "$bundle carries ${kind#team:}, which only its maker's signature can carry: it cannot load PushMPC"; return 3 ;; esac
        [ "$dry" = 1 ] && { emit "$name" "$([ "$installed" = 1 ] && echo installed || echo would-install)" "$bundle ($lib; signature: $kind)"; return 0; }
        d=$(dirname "$lib")
        if ! touch "$d/.pushmpc-write-test" 2>/dev/null; then
            emit "$name" refused "macOS does not allow writing into $bundle: allow this terminal (or Setup) in System Settings > Privacy & Security > App Management"
            return 1
        fi
        rm -f "$d/.pushmpc-write-test"
        mkdir -p "$bk" "$LOGS" || { emit "$name" error "cannot write $bk"; return 1; }
        if [ "$installed" = 1 ]; then
            cmp -s "$from/apps/libmpv-proxy.dylib" "$lib" || cp "$from/apps/libmpv-proxy.dylib" "$lib" || { emit "$name" error "cannot write $lib"; return 1; }
        else
            rm -rf "$bk/app-backup.app" "$bk/app-version"
            if [ "$kind" = resign ]; then
                ditto "$bundle" "$bk/app-backup.app" || { rm -rf "$bk/app-backup.app"; emit "$name" error "cannot back up $bundle"; return 1; }
                defaults read "$bundle/Contents/Info" CFBundleVersion > "$bk/app-version" 2>/dev/null
            fi
            rm -f "$d/pushmpc-orig-libmpv.dylib"   # an original from before an app update
            sha "$lib" > "$bk/libmpv.sha256"; printf '%s\n' "${lib#"$bundle"/}" > "$bk/libmpv.rel"
            mv "$lib" "$d/pushmpc-orig-libmpv.dylib" && cp "$from/apps/libmpv-proxy.dylib" "$lib" \
                || { [ -f "$lib" ] || mv "$d/pushmpc-orig-libmpv.dylib" "$lib"; emit "$name" error "cannot replace $lib"; return 1; }
        fi
        write_conf "$d"
        xattr -d com.apple.quarantine "$lib" 2>/dev/null
        if [ "$kind" = resign ]; then
            # the app's own entitlements, without the hardened runtime: it then loads ad-hoc signed libraries
            codesign -f -s - --deep --preserve-metadata=entitlements "$bundle" >/dev/null 2>&1 \
                || { app_put_back "$bundle" "$lib" "$bk"; rm -rf "$bk"; emit "$name" error "signing $bundle again failed; the app is as it was"; return 1; }
        fi
        selftest_app "$lib" "$bundle"; s=$?
        if [ "$s" = 0 ]; then emit "$name" installed "$bundle ($(basename "$lib")$([ "$kind" = resign ] && echo ', signed again ad hoc; backup kept')); selftest: $st_detail"; return 0; fi
        app_put_back "$bundle" "$lib" "$bk"; rm -rf "$bk"
        emit "$name" not-active "$bundle: put back as it was; ${st_detail:-the self-test could not run}"; return 3 ;;
    uninstall)
        [ "$dry" = 1 ] && { emit "$name" would-uninstall "$bundle"; return 0; }
        # the stand-in gone (the app was updated or the file removed): the path the original had
        [ -n "$lib" ] || { [ ! -f "$bk/libmpv.rel" ] || lib=$bundle/$(cat "$bk/libmpv.rel"); }
        pb_detail='nothing of PushMPC in the app'
        if [ -n "$lib" ]; then app_put_back "$bundle" "$lib" "$bk" || { emit "$name" error "cannot write $bundle"; return 1; }; fi
        rm -rf "$bk"; emit "$name" uninstalled "$pb_detail" ;;
    esac
}

# ---------------------------------------------------------------------------------------------- shims: conf.json
# conf.json through JavaScript for Automation (every Mac has it; plutil cannot read JSON with null values)
json_edit() {   # json_edit <file> set <mpv> | restore <old values JSON>: prints the old values of our keys
    osascript -l JavaScript - "$@" <<'JXA'
ObjC.import('Foundation');
function run(argv) {
    var path = argv[0], mode = argv[1], arg = argv[2];
    var s = $.NSString.stringWithContentsOfFileEncodingError(path, $.NSUTF8StringEncoding, null);
    if (s.isNil()) throw new Error('cannot read ' + path);
    var o = JSON.parse(s.js), keys = ['mpv_ext', 'mpv_ext_path', 'mpv_ext_no_ovr'], old = {};
    keys.forEach(function (k) { old[k] = (k in o) ? o[k] : '__absent__'; });
    if (mode === 'set') { o.mpv_ext = true; o.mpv_ext_path = arg; o.mpv_ext_no_ovr = false; }
    else { var p = JSON.parse(arg); keys.forEach(function (k) { if (p[k] === '__absent__') delete o[k]; else o[k] = p[k]; }); }
    var out = $(JSON.stringify(o, null, 4));
    if (!out.writeToFileAtomicallyEncodingError(path, true, $.NSUTF8StringEncoding, null)) throw new Error('cannot write ' + path);
    return JSON.stringify(old);
}
JXA
}
do_shim() {   # do_shim <target>
    name=$1; dir="$HOME/Library/Application Support/$name"; cj=$dir/conf.json; bk=$ST/$name
    case $action in
    status)
        if [ -f "$bk/conf.old.json" ]; then emit "$name" installed "$cj (mpv_ext_path=$(sed -n 's/.*"mpv_ext_path": *"\([^"]*\)".*/\1/p' "$cj" | head -n 1))"
        else emit "$name" not-installed "$cj"; fi ;;
    install)
        [ -n "$mpv" ] && [ -x "$mpv" ] || { emit "$name" unsupported "no Homebrew mpv (brew install mpv)"; return 3; }
        [ -n "$integ" ] && [ -f "$integ" ] && [ -n "$vpy" ] || { emit "$name" error "mpv-integration.sh / pushmpc.vpy not given (pushmpc-setup passes them)"; return 1; }
        # the shim's process (jellyfin-mpv-shim, jellyfin_mpv_shim, "Jellyfin MPV Shim"), not this script
        if pgrep -i -f "$(printf '%s' "$name" | sed 's/-/[-_ ]/g')" 2>/dev/null | grep -v -x -q "$$"; then emit "$name" refused "$name is running: quit it and run install again (it writes conf.json when it quits)"; return 1; fi
        [ "$dry" = 1 ] && { emit "$name" would-install "$cj; $dir/mpv.conf"; return 0; }
        mkdir -p "$bk" || { emit "$name" error "cannot write $bk"; return 1; }
        [ -f "$bk/conf.json.backup" ] || cp -p "$cj" "$bk/conf.json.backup" || { emit "$name" error "cannot back up $cj"; return 1; }
        old=$(json_edit "$cj" set "$mpv" 2>&1) || { emit "$name" error "cannot change $cj: $old"; return 1; }
        [ -f "$bk/conf.old.json" ] || printf '%s\n' "$old" > "$bk/conf.old.json"
        sha "$cj" > "$bk/conf.json.sha256"
        set --
        [ -z "$vsscript" ] || set -- --extra "scripts-append=$vsscript"
        while IFS= read -r x; do [ -z "$x" ] || set -- "$@" --extra "$x"; done <<EOF
$extra
EOF
        if ! o=$(/bin/sh "$integ" install --conf "$dir/mpv.conf" --script "$vpy" "$@" 2>&1); then
            json_edit "$cj" restore "$(cat "$bk/conf.old.json")" >/dev/null 2>&1; cmp -s "$cj" "$bk/conf.json.backup" || cp -p "$bk/conf.json.backup" "$cj"
            rm -rf "$bk"; emit "$name" error "the mpv.conf block failed: $(printf '%s' "$o" | tail -n 2)"; return 1
        fi
        emit "$name" installed "$cj: mpv_ext_path=$mpv; block in $dir/mpv.conf" ;;
    uninstall)
        [ "$dry" = 1 ] && { emit "$name" would-uninstall "$cj"; return 0; }
        [ -f "$bk/conf.old.json" ] || { emit "$name" uninstalled "nothing of PushMPC in $cj"; return 0; }
        [ -z "$integ" ] || [ ! -f "$integ" ] || /bin/sh "$integ" uninstall --conf "$dir/mpv.conf" >/dev/null 2>&1
        if [ -f "$cj" ] && [ "$(sha "$cj")" = "$(cat "$bk/conf.json.sha256" 2>/dev/null)" ]; then cp -p "$bk/conf.json.backup" "$cj"; detail="$cj as it was"
        elif [ -f "$cj" ]; then json_edit "$cj" restore "$(cat "$bk/conf.old.json")" >/dev/null 2>&1; detail="$cj: mpv_ext, mpv_ext_path, mpv_ext_no_ovr as they were (the shim changed the file since)"
        else detail="$cj is gone"; fi
        rm -rf "$bk"; emit "$name" uninstalled "$detail" ;;
    esac
}

# ---------------------------------------------------------------------------------------------- the reapply agent
AGENT=$HOME/Library/LaunchAgents/com.pushmpc.apps-reapply.plist
agent_write() {   # agent_write <bundle>...: watches their Info.plist
    mkdir -p "$HOME/Library/LaunchAgents" "$LOGS"
    w=''; for b in "$@"; do w="$w<string>$b/Contents/Info.plist</string>"; done
    cat > "$AGENT" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>com.pushmpc.apps-reapply</string>
  <key>ProgramArguments</key>
  <array><string>/bin/sh</string><string>$from/pushmpc-apps-mac.sh</string><string>install</string><string>--from</string><string>$from</string><string>--bridge</string><string>$bridge</string><string>--target</string><string>auto</string><string>--if-needed</string><string>--json</string></array>
  <key>EnvironmentVariables</key>
  <dict><key>HOME</key><string>$HOME</string></dict>
  <key>RunAtLoad</key><true/>
  <key>WatchPaths</key>
  <array>$w</array>
  <key>StandardOutPath</key><string>$LOGS/apps-reapply.log</string>
  <key>StandardErrorPath</key><string>$LOGS/apps-reapply.log</string>
</dict>
</plist>
PLIST
    launchctl bootout "gui/$(id -u)/com.pushmpc.apps-reapply" 2>/dev/null
    launchctl bootstrap "gui/$(id -u)" "$AGENT" 2>/dev/null || log "the reapply agent is written; it starts at the next login"
}
agent_remove() {
    [ -f "$AGENT" ] || return 0
    launchctl bootout "gui/$(id -u)/com.pushmpc.apps-reapply" 2>/dev/null
    rm -f "$AGENT"
}

# ---------------------------------------------------------------------------------------------- main
rc=0; found=0; apps=''
run_t() { found=1; "$@" || { r=$?; [ "$r" = 3 ] && [ "$rc" = 0 ] && rc=3; [ "$r" = 1 ] && rc=1; }; return 0; }
first_app() { for a in "$@"; do [ -d "$R$a" ] && { printf '%s\n' "$R$a"; return 0; }; done; return 1; }
# the agent passes --if-needed: only apps that had PushMPC; the shims are not touched by it
if want stremio && b=$(first_app /Applications/Stremio.app "$HOME/Applications/Stremio.app"); then apps="$apps$b
"; run_t do_app stremio "$b"; fi
if want jellyfin-desktop && b=$(first_app "/Applications/Jellyfin Desktop.app" "/Applications/Jellyfin Media Player.app" "$HOME/Applications/Jellyfin Desktop.app" "$HOME/Applications/Jellyfin Media Player.app"); then
    apps="$apps$b
"; run_t do_app jellyfin-desktop "$b"
fi
if [ "$ifneeded" = 0 ]; then
    for s in jellyfin-mpv-shim plex-mpv-shim; do
        if want "$s" && { [ -f "$HOME/Library/Application Support/$s/conf.json" ] || [ -d "$ST/$s" ]; }; then run_t do_shim "$s"; fi
    done
fi
if [ "$dry" != 1 ]; then
    if [ "$action" = install ] && [ "$agent" = 1 ] && [ -n "$apps" ] && { [ -d "$ST/stremio" ] || [ -d "$ST/jellyfin-desktop" ]; }; then
        set --; while IFS= read -r a; do [ -z "$a" ] || set -- "$@" "$a"; done <<EOF
$apps
EOF
        agent_write "$@"
    fi
    if [ "$action" = uninstall ] && [ "$target" = auto ]; then
        agent_remove
        rmdir "$ST" "$(dirname "$ST")" 2>/dev/null
    fi
fi
[ "$found" = 1 ] || { emit "$target" unsupported "none of these players is installed"; exit 3; }
exit $rc
