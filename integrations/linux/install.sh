#!/bin/sh
# PushMPC for Linux: user-level installer (no root, nothing outside your home folder). It puts PushMPC's VapourSynth
# plugin (pushmpc.Enhance: frame generation, scene-cut guard) into every mpv-based player it finds and takes it out
# again exactly.
#
#   ./install.sh [install]   [--flatpak ask|yes|no] [--yes] [--dry-run]
#   ./install.sh uninstall   [--keep-flatpak-mpv]
#   ./install.sh status
#
# Files: the plugin and pushmpc.vpy in ${XDG_DATA_HOME:-~/.local/share}/pushmpc/vsplugin; this script and
# mpv-integration.sh are copied to ${XDG_DATA_HOME:-~/.local/share}/pushmpc so "uninstall" works without the download.
# What it changes, each with a backup and an exact restore on uninstall (a file that changed since is only cleaned of
# PushMPC's part, and its backup is kept):
#   mpv (native)      ~/.config/mpv/mpv.conf: the marked PushMPC block (mpv-integration.sh), only when this mpv has
#                     VapourSynth (Arch, Fedora with RPM Fusion, ...; Ubuntu and Debian build mpv without it)
#   mpv (Flathub)     io.mpv.Mpv (it bundles VapourSynth): ~/.var/app/io.mpv.Mpv/config/mpv/mpv.conf, plus a Flatpak
#                     override that lets it read the plugin folder (read-only); the override file is backed up
#   no usable mpv     offers once to install io.mpv.Mpv from Flathub for your user (--flatpak yes|no answers ahead;
#                     the default without a terminal is no). Uninstall removes it again only if PushMPC installed it.
#   Celluloid         its "Load mpv configuration file" setting (GSettings mpv-config-enable / mpv-config-file)
#   Haruna            startup custom commands in ~/.config/haruna/custom-commands.conf (groups PushMPC_*)
#   SMPlayer          [advanced] mplayer_additional_options in ~/.config/smplayer/smplayer.ini
#   MPC-QT            Options > Tweaks "mpv options file" in ~/.config/mpc-qt/settings.json
#   Jellyfin MPV Shim, Plex-MPV-Shim   the block in the mpv.conf beside their conf.json
#   Jellyfin Desktop  the block in ~/.local/share/jellyfin-desktop/profiles/<id>/mpv.conf (system libmpv)
#   Kodi (native)     ~/.kodi/userdata/playercorefactory.xml: mpv (or the Flathub mpv) as the external player
#                     for every video, in marked parts
# Environment for tests: HOME, XDG_*; PUSHMPC_SRC (folder with vsplugin/ and mpv-integration.sh; default: next to
# this script); PUSHMPC_NO_PATH=1 ignores players on PATH. Exit code 0 = done (players that could not be set up are
# reported, not errors).
set -u

BEGIN_MARK='# >>> PushMPC managed: do not edit between these lines (PushMPC uninstall removes them)'
XML_BEGIN='<!-- >>> PushMPC managed: PushMPC uninstall removes this -->'
XML_END='<!-- <<< PushMPC managed -->'
FLATPAK_MPV=io.mpv.Mpv

action=install
case ${1:-} in install|uninstall|status) action=$1; shift ;; esac
flatpak_answer=ask; yes=0; dry=0; keep_fp=0
while [ $# -gt 0 ]; do
    case $1 in
        --flatpak) flatpak_answer=$2; shift 2 ;;
        --yes|-y) yes=1; shift ;;
        --dry-run) dry=1; shift ;;
        --keep-flatpak-mpv) keep_fp=1; shift ;;
        -h|--help) sed -n '2,32p' "$0"; exit 0 ;;
        *) echo "install.sh: unknown option $1" >&2; exit 2 ;;
    esac
done

here=$(cd "$(dirname "$0")" && pwd)
src=${PUSHMPC_SRC:-$here}
cfg=${XDG_CONFIG_HOME:-}; case $cfg in /*) ;; *) cfg=$HOME/.config ;; esac
data=${XDG_DATA_HOME:-}; case $data in /*) ;; *) data=$HOME/.local/share ;; esac
pm=$data/pushmpc
plugdir=$pm/vsplugin
script=$plugdir/pushmpc.vpy
state=$pm/install-state
mi=$pm/mpv-integration.sh
fpdata=$HOME/.var/app

say() { printf '%s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*"; }
run() { if [ "$dry" = 1 ]; then say "would: $*"; else "$@"; fi; }
have() { [ "${PUSHMPC_NO_PATH:-0}" = 1 ] && [ "${2:-}" != stub ] && return 1; command -v "$1" >/dev/null 2>&1; }
sha() { sha256sum "$1" 2>/dev/null | cut -d' ' -f1; }
record() { [ "$dry" = 1 ] && return 0; mkdir -p "$pm"; grep -q -F -x "$*" "$state" 2>/dev/null || printf '%s\n' "$*" >> "$state"; }
recorded() { [ -f "$state" ] && grep -q -F -x "$*" "$state"; }

# ------------------------------------------------------------------------------------------------ backups
# backup_once FILE: FILE.pushmpc-backup (a copy) or FILE.pushmpc-absent (file | created-dir), once
backup_once() {
    [ -e "$1.pushmpc-backup" ] || [ -e "$1.pushmpc-absent" ] && return 0
    [ "$dry" = 1 ] && return 0
    if [ -f "$1" ]; then cp -p "$1" "$1.pushmpc-backup"
    else d=$(dirname "$1"); how=file; [ -d "$d" ] || { how=created-dir; mkdir -p "$d"; }; printf '%s' "$how" > "$1.pushmpc-absent"; fi
}
mark_written() { [ "$dry" = 1 ] || sha "$1" > "$1.pushmpc-written"; }
# restore_file FILE REVERT_CMD: the exact copy when FILE is still what PushMPC wrote, else REVERT_CMD FILE (PushMPC's
# part only; the backup stays)
restore_file() {
    f=$1; rev=$2
    if [ ! -e "$f.pushmpc-written" ]; then return 0; fi
    if [ -f "$f" ] && [ "$(sha "$f")" = "$(cat "$f.pushmpc-written")" ]; then
        if [ -f "$f.pushmpc-backup" ]; then run cp -p "$f.pushmpc-backup" "$f"; run rm -f "$f.pushmpc-backup"; say "  $f: restored exactly"
        elif [ -f "$f.pushmpc-absent" ]; then
            how=$(cat "$f.pushmpc-absent"); run rm -f "$f" "$f.pushmpc-absent"
            [ "$how" = created-dir ] && [ "$dry" = 0 ] && rmdir "$(dirname "$f")" 2>/dev/null
            say "  $f: removed (PushMPC had created it)"
        fi
    elif [ -f "$f" ]; then
        [ "$dry" = 1 ] || "$rev" "$f"
        run rm -f "$f.pushmpc-absent"
        say "  $f: PushMPC's part removed (the file changed since; backup kept at $f.pushmpc-backup if any)"
    fi
    run rm -f "$f.pushmpc-written"
}

# ------------------------------------------------------------------------------------------------ the block as options
vf_option() {
    case $1 in *']'*) n=$(printf '%s' "$1" | wc -c | tr -d ' '); q="%$n%$1" ;; *) q="[$1]" ;; esac
    printf '@pushmpc:vapoursynth=file=%s:buffered-frames=4:concurrent-frames=2' "$q"
}
mpvconf() { sh "$mi" "$1" --conf "$2" --script "$3" $( [ "$dry" = 1 ] && echo --dry-run ) | sed 's/^/  /'; }

# ------------------------------------------------------------------------------------------------ detection
native_mpv_vs() { have mpv && mpv --no-config --vf=help 2>/dev/null | grep -q vapoursynth; }
flatpak_mpv() { have flatpak && flatpak info "$FLATPAK_MPV" >/dev/null 2>&1; }

# ------------------------------------------------------------------------------------------------ VapourSynth's Python link
# VapourSynth R73 and newer: VSScript (the library mpv loads) finds Python through
# ${XDG_CONFIG_HOME:-~/.config}/vapoursynth/vapoursynth.toml, which `vapoursynth config` writes. VSScript tries to run
# that itself, but where the command is not on PATH (seen on Arch in CI) mpv fails with "Python executable and library
# path couldn't be determined". So the installer runs it (the file is backed up and restored like the others).
vs_config() {
    if have vapoursynth stub; then c='vapoursynth config'
    elif python3 -c 'import vapoursynth, vapoursynth._utils' >/dev/null 2>&1; then c='python3 -m vapoursynth config'
    else return 0; fi
    t=$cfg/vapoursynth/vapoursynth.toml
    [ -f "$t" ] && [ ! -e "$t.pushmpc-written" ] && grep -q . "$t" && before=$(sha "$t") || before=''
    backup_once "$t"
    [ "$dry" = 1 ] && { say "would: run $c"; return 0; }
    XDG_CONFIG_HOME=$cfg $c >/dev/null 2>&1 || { say "  VapourSynth: '$c' failed; mpv may not find Python (run it yourself if mpv says so)"; }
    # `vapoursynth config` keys its entry by the VSScript library inside the Python package; a distribution's mpv may
    # load another copy (e.g. /usr/lib/libvapoursynth-script.so). VSScript looks up the library it was loaded from,
    # so the same Python entry is added under that library's path too.
    if [ -f "$t" ] && have mpv; then
        vs_lib=$(ldd "$(command -v mpv)" 2>/dev/null | sed -n 's/^.*=> \(\/[^ ]*\(vsscript\|vapoursynth-script\)[^ ]*\) .*$/\1/p' | head -n 1)
        # mpv usually opens it at run time (dlopen), so it is not in ldd's list: the one the loader would find
        [ -n "$vs_lib" ] || vs_lib=$( { /sbin/ldconfig -p 2>/dev/null || ldconfig -p 2>/dev/null; } | sed -n 's/^.*=> \(\/.*lib\(vapoursynth-script\|vsscript\)\.so[^ ]*\)$/\1/p' | head -n 1)
        if [ -n "$vs_lib" ]; then
            key=$(readlink -f "$vs_lib" | sed 's#/lib64/#/lib/#')
            rest=$(grep '^"' "$t" | tail -n 1 | sed 's/^"[^"]*"//')
            if [ -n "$rest" ] && ! grep -qF "\"$key\"" "$t" && ! grep -qF "\"$vs_lib\"" "$t"; then
                printf '"%s"%s\n' "$key" "$rest" >> "$t"
                say "  VapourSynth: the same Python entry for mpv's VSScript ($key)"
            fi
        fi
    fi
    if [ -f "$t" ] && [ "$(sha "$t")" != "$before" ]; then mark_written "$t"; record "file|vsconfig|$t"; say "  VapourSynth: Python registered for VSScript ($t)"
    else rm -f "$t.pushmpc-backup"; [ -f "$t.pushmpc-absent" ] && { how=$(cat "$t.pushmpc-absent"); rm -f "$t.pushmpc-absent"; [ "$how" = created-dir ] && rmdir "$(dirname "$t")" 2>/dev/null; }; fi
    return 0
}
vsconfig_revert() { return 0; }
# mpv's VapourSynth filter on a generated picture: 0 = it runs, 1 = it cannot start (VSScript / Python), 2 = unknown
vs_selftest() {
    d=$(mktemp -d 2>/dev/null || echo "${TMPDIR:-/tmp}/pushmpc-vstest.$$"); mkdir -p "$d"
    printf 'video_in.set_output()\n' > "$d/t.vpy"
    mpv --no-config --vo=null --ao=null --frames=2 --vf="vapoursynth=file=[$d/t.vpy]" 'av://lavfi:testsrc=size=160x120:rate=24' > "$d/out" 2>&1; rc=$?
    vs_msg=$(grep -i 'vapoursynth\]' "$d/out" | grep -iv 'concurrent requests' | head -n 1)
    r=2
    if [ "$rc" = 0 ]; then r=0
    elif grep -qi 'Failed to initialize VapourSynth\|Creating filter .vapoursynth. failed\|libvapoursynth' "$d/out"; then r=1; fi
    rm -rf "$d"
    return $r
}
vs_hint() {
    say "  mpv's VapourSynth cannot start Python: ${vs_msg:-unknown error}"
    say "  Install your distribution's VapourSynth Python module (Fedora: sudo dnf install python3-vapoursynth;"
    say "  Debian/Ubuntu: python3-vapoursynth), run 'vapoursynth config' once, then run this installer again."
}

# ------------------------------------------------------------------------------------------------ Flatpak override
fp_overrides() { printf '%s/flatpak/overrides/%s' "$data" "$FLATPAK_MPV"; }
fp_allow() {
    o=$(fp_overrides)
    backup_once "$o"
    run flatpak override --user --filesystem="$pm:ro" "$FLATPAK_MPV"
    [ -f "$o" ] && mark_written "$o"
}
fp_revert() { flatpak override --user --nofilesystem="$pm" "$FLATPAK_MPV" 2>/dev/null; sed -i "s#;\?!\?$pm\(:ro\)\?;\?##" "$1" 2>/dev/null; return 0; }

# ------------------------------------------------------------------------------------------------ Celluloid (GSettings)
CEL=io.github.celluloid-player.Celluloid
celluloid_install() {
    conf=$1
    gsettings list-keys "$CEL" >/dev/null 2>&1 || { say "  Celluloid: its settings schema is not installed (Flatpak Celluloid?): not changed"; return 0; }
    en=$(gsettings get "$CEL" mpv-config-enable); file=$(gsettings get "$CEL" mpv-config-file)
    if [ "$file" = "'file://$conf'" ]; then say "  Celluloid: already set up"; return 0; fi
    if [ "$en" = true ] && [ "$file" != "''" ]; then
        path=$(printf '%s' "$file" | sed "s/^'//; s/'\$//; s#^file://##")
        say "  Celluloid already loads $path: the block goes there"
        mpvconf install "$path" "$script"; record "celluloid-userfile|$path"; return 0
    fi
    recorded "celluloid|$en|$file" || record "celluloid|$en|$file"
    run gsettings set "$CEL" mpv-config-file "file://$conf"
    run gsettings set "$CEL" mpv-config-enable true
    say "  Celluloid: Load mpv configuration file = $conf"
}
celluloid_uninstall() {
    en=$1; file=$2
    if [ "$(gsettings get "$CEL" mpv-config-file 2>/dev/null)" = "'file://$cfg/mpv/mpv.conf'" ]; then
        run gsettings set "$CEL" mpv-config-enable "$en"
        if [ "$file" = "''" ]; then run gsettings reset "$CEL" mpv-config-file; else run gsettings set "$CEL" mpv-config-file "$(printf '%s' "$file" | sed "s/^'//; s/'\$//")"; fi
        say "  Celluloid: settings put back"
    else say "  Celluloid: its mpv config setting changed since: left as it is"; fi
}

# ------------------------------------------------------------------------------------------------ Haruna (custom commands)
haruna_cmds() {
    printf '[PushMPC_1]\nCommand=vf append %s\nOrder=900\nOsdMessage=\nSetOnStartup=true\nType=startup\n\n' "$(vf_option "$script")"
    printf '[PushMPC_2]\nCommand=set hwdec auto-copy\nOrder=901\nOsdMessage=\nSetOnStartup=true\nType=startup\n\n'
    printf '[PushMPC_3]\nCommand=set hr-seek-framedrop no\nOrder=902\nOsdMessage=\nSetOnStartup=true\nType=startup\n\n'
    printf '[PushMPC_4]\nCommand=set interpolation no\nOrder=903\nOsdMessage=\nSetOnStartup=true\nType=startup\n'
}
haruna_strip() { awk '/^\[/ { skip = ($0 ~ /^\[PushMPC_[0-9]+\]$/) } !skip' "$1"; }
haruna_install() {
    f=$cfg/haruna/custom-commands.conf
    backup_once "$f"
    [ "$dry" = 1 ] && { say "would: add the PushMPC startup commands to $f"; return 0; }
    tmp=$f.pushmpc-tmp
    { if [ -f "$f" ] && [ -s "$f" ]; then haruna_strip "$f" | awk '{ l[NR] = $0 } END { n = NR; while (n > 0 && l[n] == "") n--; for (i = 1; i <= n; i++) print l[i]; if (n) print "" }'; fi; haruna_cmds; } > "$tmp" && mv "$tmp" "$f"
    mark_written "$f"; record "file|haruna|$f"
    say "  Haruna: startup commands PushMPC_1..4 in $f"
}
haruna_revert() { haruna_strip "$1" > "$1.pushmpc-tmp" && mv "$1.pushmpc-tmp" "$1"; }

# ------------------------------------------------------------------------------------------------ SMPlayer (ini)
sm_flags() { printf -- '--vf-append=%s --hwdec=auto-copy --hr-seek-framedrop=no --interpolation=no' "$(vf_option "$script")"; }
smplayer_install() {
    f=$cfg/smplayer/smplayer.ini
    [ -f "$f" ] || { say "  SMPlayer: start it once (it writes $f), then run this again"; return 0; }
    backup_once "$f"
    [ "$dry" = 1 ] && { say "would: add the PushMPC options to $f"; return 0; }
    flags=$(sm_flags)
    awk -v flags="$flags" '
        function out_key() { print "mplayer_additional_options=" flags; done = 1 }
        /^\[/ { if (insec && !done) out_key(); insec = ($0 == "[advanced]"); seen = seen || insec; print; next }
        insec && /^mplayer_additional_options=/ { v = substr($0, 28); i = index(v, flags); if (i) v = substr(v, 1, i - 1) substr(v, i + length(flags));
            gsub(/^ +| +$/, "", v); print "mplayer_additional_options=" (v == "" ? "" : v " ") flags; done = 1; next }
        { print }
        END { if (insec && !done) out_key(); else if (!seen) { print ""; print "[advanced]"; out_key() } }' "$f" > "$f.pushmpc-tmp" && mv "$f.pushmpc-tmp" "$f"
    mark_written "$f"; record "file|smplayer|$f"
    say "  SMPlayer: options added to $f"
}
smplayer_revert() {
    flags=$(sm_flags)
    awk -v flags="$flags" '/^mplayer_additional_options=/ { i = index($0, flags); if (i) { $0 = substr($0, 1, i - 1) substr($0, i + length(flags)); sub(/ +$/, "") } } { print }' "$1" > "$1.pushmpc-tmp" && mv "$1.pushmpc-tmp" "$1"
}

# ------------------------------------------------------------------------------------------------ MPC-QT (settings.json)
qt_own=$pm/mpc-qt-mpv.conf
mpcqt_install() {
    f=$cfg/mpc-qt/settings.json
    if [ -f "$f" ] && grep -q '"tweaksMpvOptionsFileChkBox": *true' "$f"; then
        path=$(sed -n 's/.*"tweaksMpvOptionsFilePath": *"\([^"]*\)".*/\1/p' "$f" | head -n 1)
        [ -n "$path" ] || path=$cfg/mpv/mpv.conf
        if [ "$path" = "$qt_own" ]; then mpvconf install "$qt_own" "$script"; say "  MPC-QT: already set up"; return 0; fi
        say "  MPC-QT already reads $path: the block goes there"
        mpvconf install "$path" "$script"; record "mpcqt-userfile|$path"; return 0
    fi
    mpvconf install "$qt_own" "$script"; record "mpvconf|$qt_own"
    backup_once "$f"
    [ "$dry" = 1 ] && { say "would: turn on MPC-QT's mpv options file ($qt_own)"; return 0; }
    if [ -f "$f" ] && grep -q '"tweaksMpvOptionsFileChkBox"' "$f"; then
        sed -e 's/"tweaksMpvOptionsFileChkBox": *false/"tweaksMpvOptionsFileChkBox": true/' \
            -e "s#\"tweaksMpvOptionsFilePath\": *\"[^\"]*\"#\"tweaksMpvOptionsFilePath\": \"$qt_own\"#" "$f" > "$f.pushmpc-tmp" && mv "$f.pushmpc-tmp" "$f"
        grep -q '"tweaksMpvOptionsFilePath"' "$f" || sed -i "s#\"tweaksMpvOptionsFileChkBox\": true#\"tweaksMpvOptionsFileChkBox\": true,\n    \"tweaksMpvOptionsFilePath\": \"$qt_own\"#" "$f"
    elif [ -f "$f" ] && grep -q '{' "$f"; then
        sed "0,/{/s#{#{\n    \"tweaksMpvOptionsFileChkBox\": true,\n    \"tweaksMpvOptionsFilePath\": \"$qt_own\",#" "$f" > "$f.pushmpc-tmp" && mv "$f.pushmpc-tmp" "$f"
    else
        printf '{\n    "tweaksMpvOptionsFileChkBox": true,\n    "tweaksMpvOptionsFilePath": "%s"\n}\n' "$qt_own" > "$f"
    fi
    mark_written "$f"; record "file|mpcqt|$f"
    say "  MPC-QT: Options > Tweaks > mpv options file = $qt_own"
}
mpcqt_revert() {
    # the values before PushMPC, from the backup (absent keys: the switch off, no path)
    b=$1.pushmpc-backup
    oc=false; op=''
    if [ -f "$b" ]; then
        grep -q '"tweaksMpvOptionsFileChkBox": *true' "$b" && oc=true
        op=$(sed -n 's/.*"tweaksMpvOptionsFilePath": *"\([^"]*\)".*/\1/p' "$b" | head -n 1)
    fi
    sed -e "s/\"tweaksMpvOptionsFileChkBox\": *true/\"tweaksMpvOptionsFileChkBox\": $oc/" \
        -e "s#\"tweaksMpvOptionsFilePath\": *\"$qt_own\"#\"tweaksMpvOptionsFilePath\": \"$op\"#" "$1" > "$1.pushmpc-tmp" && mv "$1.pushmpc-tmp" "$1"
}

# ------------------------------------------------------------------------------------------------ Kodi (playercorefactory.xml)
kodi_player() {
    # $1 = executable, $2 = arguments before the file
    printf '%s\n' "$XML_BEGIN" \
        '<player name="PushMPC mpv" type="ExternalPlayer" audio="false" video="true">' \
        "  <filename>$1</filename>" \
        "  <args>$2\"{1}\" --fs --force-window=immediate</args>" \
        '  <hidexbmc>false</hidexbmc>' '  <hideconsole>true</hideconsole>' '  <warpcursor>none</warpcursor>' \
        '</player>' "$XML_END"
}
kodi_rules() { printf '%s\n' "$XML_BEGIN" '<rules action="prepend">' '  <rule video="true" player="PushMPC mpv"/>' '</rules>' "$XML_END"; }
kodi_strip() { awk -v b="$XML_BEGIN" -v e="$XML_END" '{ t = $0; sub(/^[ \t]+/, "", t) } t == b { skip = 1; next } skip && t == e { skip = 0; next } !skip' "$1"; }
kodi_install() {
    exe=$1; pre=$2
    f=$HOME/.kodi/userdata/playercorefactory.xml
    backup_once "$f"
    [ "$dry" = 1 ] && { say "would: add the PushMPC mpv player to $f"; return 0; }
    pl=$(kodi_player "$exe" "$pre" | sed 's/^/    /')
    ru=$(kodi_rules | sed 's/^/  /')
    if [ ! -s "$f" ]; then
        printf '<?xml version="1.0" encoding="UTF-8"?>\n<playercorefactory>\n  <players>\n%s\n  </players>\n%s\n</playercorefactory>\n' "$pl" "$ru" > "$f"
    else
        base=$(kodi_strip "$f")
        if ! printf '%s\n' "$base" | grep -q '<playercorefactory'; then say "  Kodi: $f has no <playercorefactory>: not changed"; return 0; fi
        if printf '%s\n' "$base" | grep -q '<players>'; then
            printf '%s\n' "$base" | awk -v pl="$pl" -v ru="$ru" '
                !p && /<players>/ { print; print pl; p = 1; next }
                /<\/playercorefactory>/ { print ru } { print }' > "$f.pushmpc-tmp"
        else
            plw=$(printf '  %s\n  <players>\n%s\n  </players>\n  %s' "$XML_BEGIN" "$(kodi_player "$exe" "$pre" | sed '1d;$d' | sed 's/^/    /')" "$XML_END")
            printf '%s\n' "$base" | awk -v plw="$plw" -v ru="$ru" '/<\/playercorefactory>/ { print plw; print ru } { print }' > "$f.pushmpc-tmp"
        fi
        mv "$f.pushmpc-tmp" "$f"
    fi
    mark_written "$f"; record "file|kodi|$f"
    say "  Kodi: videos play in $exe $pre(external player, $f)"
}
kodi_revert() { kodi_strip "$1" > "$1.pushmpc-tmp" && mv "$1.pushmpc-tmp" "$1"; }

# ------------------------------------------------------------------------------------------------ install
copy_files() {
    for f in pushmpc_vs.so pushmpc.vpy; do [ -f "$src/vsplugin/$f" ] || { echo "install.sh: $src/vsplugin/$f is missing (run install.sh from the unpacked PushMPC folder)" >&2; exit 1; }; done
    run mkdir -p "$plugdir"
    run cp "$src/vsplugin/pushmpc_vs.so" "$src/vsplugin/pushmpc.vpy" "$plugdir/"
    [ -f "$src/LICENSE" ] && run cp "$src/LICENSE" "$pm/"
    [ "$here/install.sh" = "$pm/install.sh" ] || run cp "$here/install.sh" "$pm/install.sh"
    if [ -f "$src/mpv-integration.sh" ]; then run cp "$src/mpv-integration.sh" "$mi"; elif [ -f "$src/integrations/mpv/mpv-integration.sh" ]; then run cp "$src/integrations/mpv/mpv-integration.sh" "$mi"; fi
    [ "$dry" = 1 ] && mi=$src/mpv-integration.sh && [ -f "$mi" ] || true
    say "PushMPC plugin: $plugdir"
}

ask_flatpak() {
    case $flatpak_answer in yes) return 0 ;; no) return 1 ;; esac
    [ "$yes" = 1 ] && return 0
    [ -t 0 ] || { say "  (not asked: no terminal; run again with --flatpak yes to install it)"; return 1; }
    say ''
    say "Your mpv cannot run VapourSynth filters (Ubuntu, Debian and their derivatives build it without them),"
    say "so PushMPC cannot plug into it. PushMPC can install mpv from Flathub for your user only: no administrator"
    say "password, about 400 MB the first time, removed again when you uninstall PushMPC."
    printf 'Install the Flathub mpv now? [Y/n] '
    read -r a || a=n
    case $a in ''|y|Y|yes|Yes) return 0 ;; *) return 1 ;; esac
}
install_flatpak_mpv() {
    have flatpak || { say "  Flatpak is not installed. Install it first (Ubuntu/Debian: sudo apt install flatpak), then run this again."; return 1; }
    if ! flatpak remotes --user 2>/dev/null | grep -q '^flathub'; then
        run flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo || return 1
        record "flathub-remote-added"
    fi
    say "  installing $FLATPAK_MPV from Flathub (user)..."
    run flatpak install --user -y --noninteractive flathub "$FLATPAK_MPV" || return 1
    record "flatpak-mpv-installed"
}

do_install() {
    copy_files
    targets=0
    nat=0; native_mpv_vs && nat=1
    conf=$cfg/mpv/mpv.conf
    if [ "$nat" = 1 ]; then
        say "mpv ($(command -v mpv)) has VapourSynth:"
        vs_config
        vs_selftest; st=$?
        if [ "$st" = 1 ]; then
            # mpv would refuse to play anything with a filter that cannot start: its mpv.conf is left alone
            vs_hint; nat=0; nat_broken=1
        else
            [ "$st" = 2 ] && say "  (mpv's VapourSynth self-test could not run here; set up anyway)"
            mpvconf install "$conf" "$script"; record "mpvconf|$conf"; targets=$((targets + 1))
        fi
    elif have mpv; then say "mpv ($(command -v mpv)) has no VapourSynth: its mpv.conf is not changed"
    fi
    nat_broken=${nat_broken:-0}
    fp=0; flatpak_mpv && fp=1
    if [ "$fp" = 0 ] && [ "$nat" = 0 ]; then
        say "No mpv with VapourSynth found."
        if ask_flatpak && install_flatpak_mpv; then fp=1; fi
    fi
    fpconf=$fpdata/$FLATPAK_MPV/config/mpv/mpv.conf
    if [ "$fp" = 1 ]; then
        say "Flathub mpv ($FLATPAK_MPV):"
        mpvconf install "$fpconf" "$script"; record "mpvconf|$fpconf"
        fp_allow; record "flatpak-override"
        say "  Flatpak override: $FLATPAK_MPV may read $pm (read-only)"
        targets=$((targets + 1))
    fi
    # frontends on the system libmpv need its VapourSynth too
    if [ "$nat" = 1 ]; then
        if have celluloid || [ -d "$cfg/celluloid" ]; then say "Celluloid:"; celluloid_install "$conf"; fi
        if have haruna || [ -d "$cfg/haruna" ]; then say "Haruna:"; haruna_install; fi
        if have smplayer || [ -d "$cfg/smplayer" ]; then say "SMPlayer:"; smplayer_install; fi
        if have mpc-qt || [ -d "$cfg/mpc-qt" ]; then say "MPC-QT:"; mpcqt_install; fi
        for pr in "$data"/jellyfin-desktop/profiles/*/; do
            [ -d "$pr" ] || continue
            say "Jellyfin Desktop ($(basename "$pr")):"; mpvconf install "${pr}mpv.conf" "$script"; record "mpvconf|${pr}mpv.conf"
        done
        for s in jellyfin-mpv-shim plex-mpv-shim; do
            if [ -f "$cfg/$s/conf.json" ]; then say "$s:"; mpvconf install "$cfg/$s/mpv.conf" "$script"; record "mpvconf|$cfg/$s/mpv.conf"; fi
        done
    else
        for p in celluloid haruna smplayer mpc-qt; do have "$p" && say "$p: uses the system mpv library, whose VapourSynth cannot run here: not changed (use mpv from Flathub)"; done
    fi
    for a in io.github.celluloid_player.Celluloid org.kde.haruna io.github.mpc_qt.mpc-qt com.github.iwalton3.jellyfin-mpv-shim org.jellyfin.JellyfinDesktop; do
        [ -d "$fpdata/$a" ] && say "Flatpak $a: its own mpv build, not changed (no VapourSynth plugin slot)"
    done
    if [ -d "$HOME/.kodi" ] || have kodi; then
        say "Kodi:"
        if [ "$nat" = 1 ]; then kodi_install "$(command -v mpv)" ''
        elif [ "$fp" = 1 ]; then kodi_install "$(command -v flatpak || echo /usr/bin/flatpak)" "run $FLATPAK_MPV "
        else say "  no mpv with VapourSynth: not changed"; fi
    fi
    [ -d "$fpdata/tv.kodi.Kodi" ] && say "Flatpak Kodi: its sandbox cannot start mpv: not changed"
    say ''
    if [ "$targets" = 0 ]; then say "PushMPC is installed, but no player can use it yet (see above)."
    else say "PushMPC is installed. Play a video in mpv: it starts by itself. Settings: $cfg/pushmpc/settings.ini (Smoother=Auto by default)."; fi
    say "Remove it with: sh $pm/install.sh uninstall"
}

# ------------------------------------------------------------------------------------------------ uninstall
do_uninstall() {
    [ -f "$state" ] || { say "PushMPC: nothing recorded in $state"; }
    [ -f "$mi" ] || mi=$src/mpv-integration.sh
    if [ -f "$state" ]; then
        # newest first
        awk '{ a[NR] = $0 } END { for (i = NR; i > 0; i--) print a[i] }' "$state" | while IFS='|' read -r kind a b; do
            case $kind in
                mpvconf) say "mpv.conf $a:"; sh "$mi" uninstall --conf "$a" | sed 's/^/  /'
                         [ "$a" = "$qt_own" ] && [ "$dry" = 0 ] && rmdir "$(dirname "$a")" 2>/dev/null ;;
                celluloid-userfile|mpcqt-userfile) say "$a:"; sh "$mi" uninstall --conf "$a" | sed 's/^/  /' ;;
                celluloid) celluloid_uninstall "$a" "$b" ;;
                file) say "$a:"; restore_file "$b" "${a}_revert" ;;
                flatpak-override) restore_file "$(fp_overrides)" fp_revert ;;
                flatpak-mpv-installed)
                    if [ "$keep_fp" = 1 ]; then say "Flathub mpv kept (--keep-flatpak-mpv)"
                    else say "Removing the Flathub mpv PushMPC installed"; run flatpak uninstall --user -y --noninteractive "$FLATPAK_MPV"; fi ;;
                flathub-remote-added) [ "$keep_fp" = 1 ] || { flatpak list --user 2>/dev/null | grep -q . || run flatpak remote-delete --user flathub; } ;;
            esac
        done
    fi
    run rm -rf "$plugdir"
    run rm -f "$state" "$pm/LICENSE" "$pm/mpv-integration.sh"
    [ "$here" = "$pm" ] || run rm -f "$pm/install.sh"
    say "PushMPC removed."
    [ "$dry" = 1 ] || { rm -f "$pm/install.sh" 2>/dev/null; rmdir "$pm" 2>/dev/null; }
    return 0
}

do_status() {
    say "plugin: $plugdir $( [ -f "$plugdir/pushmpc_vs.so" ] && echo '(installed)' || echo '(not installed)')"
    if native_mpv_vs; then say "mpv: $(command -v mpv), VapourSynth yes"; elif have mpv; then say "mpv: $(command -v mpv), VapourSynth NO"; else say "mpv: not on PATH"; fi
    flatpak_mpv && say "Flathub mpv: installed"
    [ -f "$state" ] && { say "changes recorded in $state:"; sed 's/^/  /' "$state"; }
    return 0
}

case $action in
    install) do_install ;;
    uninstall) do_uninstall ;;
    status) do_status ;;
esac
