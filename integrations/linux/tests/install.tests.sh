#!/bin/sh
# Tests for integrations/linux/install.sh against a fake HOME with stub players (mpv, flatpak, gsettings, Celluloid,
# Haruna, SMPlayer, MPC-QT, Kodi): nothing outside the temporary HOME is touched. Every uninstall must give back the
# fake HOME byte for byte. Run: sh install.tests.sh   (also under dash; CI: integrations.yml)
set -u
here=$(cd "$(dirname "$0")" && pwd)
inst=$here/../install.sh
mi=$here/../../mpv/mpv-integration.sh
pass=0; fail=0
check() { desc=$1; shift; if "$@"; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL: $desc"; fi; }
same() { [ "$1" = "$2" ]; }
has() { grep -q -- "$1" "$2" 2>/dev/null; }
hasnot() { ! grep -q -- "$1" "$2" 2>/dev/null; }
REALPATH=$PATH

stubs() {
    b=$HOME/.stubbin; mkdir -p "$b"
    cat > "$b/mpv" <<'EOF'
#!/bin/sh
for a in "$@"; do case $a in --vf=help) echo "Available libavfilter filters:"; [ "${MPV_VS:-0}" = 1 ] && echo "  vapoursynth      VapourSynth bridge"; exit 0 ;; esac; done
exit 0
EOF
    cat > "$b/flatpak" <<'EOF'
#!/bin/sh
o=$HOME/.local/share/flatpak/overrides/io.mpv.Mpv
case $1 in
    info) [ -f "$HOME/.fp-installed" ] ;;
    remotes) [ -f "$HOME/.fp-remote" ] && echo "flathub	user" ; exit 0 ;;
    remote-add) touch "$HOME/.fp-remote" ;;
    remote-delete) rm -f "$HOME/.fp-remote" ;;
    install) touch "$HOME/.fp-installed" ;;
    uninstall) rm -f "$HOME/.fp-installed" ;;
    list) [ -f "$HOME/.fp-installed" ] && echo "io.mpv.Mpv"; exit 0 ;;
    override)
        for a in "$@"; do case $a in
            --filesystem=*) mkdir -p "$(dirname "$o")"; if [ -f "$o" ]; then sed -i "s#^filesystems=\(.*\)#filesystems=\1${a#--filesystem=};#" "$o"; else printf '[Context]\nfilesystems=%s;\n' "${a#--filesystem=}" > "$o"; fi ;;
        esac; done ;;
esac
EOF
    cat > "$b/gsettings" <<'EOF'
#!/bin/sh
f=$HOME/.gs
[ "${GS_SCHEMA:-0}" = 1 ] || exit 1
case $1 in
    list-keys) echo mpv-config-enable; echo mpv-config-file ;;
    get) v=$(grep "^$3=" "$f" 2>/dev/null | cut -d= -f2-); if [ -n "$v" ]; then echo "$v"; elif [ "$3" = mpv-config-enable ]; then echo false; else echo "''"; fi ;;
    set) grep -v "^$3=" "$f" 2>/dev/null > "$f.t"; case $4 in true|false) echo "$3=$4" ;; *) echo "$3='$4'" ;; esac >> "$f.t"; mv "$f.t" "$f" ;;
    reset) grep -v "^$3=" "$f" 2>/dev/null > "$f.t"; mv "$f.t" "$f" ;;
esac
EOF
    for p in celluloid haruna smplayer mpc-qt kodi; do printf '#!/bin/sh\nexit 0\n' > "$b/$p"; done
    chmod +x "$b"/*
}
fresh() {
    HOME=$(mktemp -d "${TMPDIR:-/tmp}/pminst.XXXXXX"); export HOME
    unset XDG_CONFIG_HOME XDG_DATA_HOME
    stubs
    PATH=$HOME/.stubbin:$REALPATH; export PATH
    MPV_VS=0; GS_SCHEMA=0; export MPV_VS GS_SCHEMA
    pkg=$HOME/pkg; mkdir -p "$pkg/vsplugin"
    printf 'ELF-fake' > "$pkg/vsplugin/pushmpc_vs.so"; printf '# vpy' > "$pkg/vsplugin/pushmpc.vpy"
    cp "$inst" "$pkg/install.sh"; cp "$mi" "$pkg/mpv-integration.sh"
    out=$HOME/out.txt
}
# every file under HOME except the test's own (stubs, package, stub state)
snap() { (cd "$HOME" && find . -type f ! -path './.stubbin/*' ! -path './pkg/*' ! -name out.txt ! -name '.gs' ! -name '.fp-*' | LC_ALL=C sort | while read -r f; do printf '%s %s\n' "$(sha256sum "$f" | cut -d' ' -f1)" "$f"; done); }
runi() { sh "$pkg/install.sh" "$@" > "$out" 2>&1; }

# 1. native mpv with VapourSynth, every frontend with existing settings
fresh
MPV_VS=1; GS_SCHEMA=1
mkdir -p "$HOME/.config/mpv" "$HOME/.config/haruna" "$HOME/.config/smplayer" "$HOME/.config/mpc-qt" "$HOME/.config/jellyfin-mpv-shim" "$HOME/.kodi/userdata"
printf 'volume=70\n' > "$HOME/.config/mpv/mpv.conf"
printf 'Counter=1\n\n[Command_0]\nCommand=cycle pause\nType=shortcut\n' > "$HOME/.config/haruna/custom-commands.conf"
printf '[%%General]\nversion=1\n\n[advanced]\nmplayer_additional_options=--volume-max=150\nother=1\n' > "$HOME/.config/smplayer/smplayer.ini"
printf '{\n    "playerOpenMode": 0,\n    "tweaksMpvOptionsFileChkBox": false,\n    "tweaksMpvOptionsFilePath": ""\n}\n' > "$HOME/.config/mpc-qt/settings.json"
printf '{"mpv_ext": false}\n' > "$HOME/.config/jellyfin-mpv-shim/conf.json"
printf '<playercorefactory>\n  <players>\n    <player name="VLC" type="ExternalPlayer"><filename>/usr/bin/vlc</filename></player>\n  </players>\n</playercorefactory>\n' > "$HOME/.kodi/userdata/playercorefactory.xml"
before=$(snap)
runi install; check "1 install exits 0" same $? 0
d=$HOME/.local/share/pushmpc
check "1 plugin copied" test -f "$d/vsplugin/pushmpc_vs.so"
check "1 vpy copied" test -f "$d/vsplugin/pushmpc.vpy"
check "1 install.sh kept for uninstall" test -f "$d/install.sh"
check "1 mpv.conf block on top" same "$(head -n 1 "$HOME/.config/mpv/mpv.conf")" '# >>> PushMPC managed: do not edit between these lines (PushMPC uninstall removes them)'
check "1 mpv.conf vf line" has "vf-append=@pushmpc:vapoursynth=file=\[$d/vsplugin/pushmpc.vpy\]" "$HOME/.config/mpv/mpv.conf"
check "1 Celluloid enabled" same "$(gsettings get io.github.celluloid-player.Celluloid mpv-config-enable)" true
check "1 Celluloid file URI" same "$(gsettings get io.github.celluloid-player.Celluloid mpv-config-file)" "'file://$HOME/.config/mpv/mpv.conf'"
check "1 Haruna vf command" has "^Command=vf append @pushmpc:vapoursynth=file=\[$d/vsplugin/pushmpc.vpy\]" "$HOME/.config/haruna/custom-commands.conf"
check "1 Haruna user command kept" has '^Command=cycle pause' "$HOME/.config/haruna/custom-commands.conf"
check "1 Haruna startup type" has '^Type=startup' "$HOME/.config/haruna/custom-commands.conf"
check "1 SMPlayer flags appended" has '^mplayer_additional_options=--volume-max=150 --vf-append=@pushmpc' "$HOME/.config/smplayer/smplayer.ini"
check "1 MPC-QT options file on" has '"tweaksMpvOptionsFileChkBox": true' "$HOME/.config/mpc-qt/settings.json"
check "1 MPC-QT path = own file" has "\"tweaksMpvOptionsFilePath\": \"$d/mpc-qt-mpv.conf\"" "$HOME/.config/mpc-qt/settings.json"
check "1 MPC-QT own file has the block" has 'vf-append=@pushmpc' "$d/mpc-qt-mpv.conf"
check "1 Jellyfin shim mpv.conf" has 'vf-append=@pushmpc' "$HOME/.config/jellyfin-mpv-shim/mpv.conf"
check "1 Kodi player added" has '<player name="PushMPC mpv"' "$HOME/.kodi/userdata/playercorefactory.xml"
check "1 Kodi uses mpv" has "<filename>$HOME/.stubbin/mpv</filename>" "$HOME/.kodi/userdata/playercorefactory.xml"
check "1 Kodi VLC kept" has '<player name="VLC"' "$HOME/.kodi/userdata/playercorefactory.xml"
check "1 Kodi prepend rule" has '<rules action="prepend">' "$HOME/.kodi/userdata/playercorefactory.xml"
s1=$(snap)
runi install; check "1 reinstall exits 0" same $? 0
check "1 reinstall changes nothing" same "$(snap)" "$s1"
check "1 one Kodi player after reinstall" same "$(grep -c 'name="PushMPC mpv"' "$HOME/.kodi/userdata/playercorefactory.xml")" 1
check "1 one Haruna vf command after reinstall" same "$(grep -c '^\[PushMPC_1\]' "$HOME/.config/haruna/custom-commands.conf")" 1
runi uninstall; check "1 uninstall exits 0" same $? 0
check "1 everything restored byte for byte" same "$(snap)" "$before"
check "1 Celluloid back to off" same "$(gsettings get io.github.celluloid-player.Celluloid mpv-config-enable)" false
check "1 pushmpc folder gone" test ! -e "$d"

# 2. distro mpv without VapourSynth, Flathub mpv present: its config and the override; frontends left alone
fresh
touch "$HOME/.fp-installed"
mkdir -p "$HOME/.config/haruna"; printf 'x=1\n' > "$HOME/.config/haruna/custom-commands.conf"
before=$(snap)
runi install; check "2 install exits 0" same $? 0
check "2 native mpv reported without VapourSynth" has 'has no VapourSynth' "$out"
check "2 native mpv.conf not created" test ! -e "$HOME/.config/mpv/mpv.conf"
fc=$HOME/.var/app/io.mpv.Mpv/config/mpv/mpv.conf
check "2 Flatpak mpv.conf has the block" has 'vf-append=@pushmpc' "$fc"
check "2 Flatpak override for the plugin folder" has "filesystems=$HOME/.local/share/pushmpc:ro;" "$HOME/.local/share/flatpak/overrides/io.mpv.Mpv"
check "2 Haruna untouched (system libmpv lacks VapourSynth)" same "$(cat "$HOME/.config/haruna/custom-commands.conf")" 'x=1'
runi uninstall; check "2 uninstall exits 0" same $? 0
check "2 restored byte for byte (override file removed)" same "$(snap)" "$before"
check "2 Flathub mpv kept (the user had it)" test -f "$HOME/.fp-installed"

# 3. existing Flatpak override file: backed up and restored exactly
fresh
touch "$HOME/.fp-installed"
mkdir -p "$HOME/.local/share/flatpak/overrides"; printf '[Context]\nfilesystems=xdg-videos;\n' > "$HOME/.local/share/flatpak/overrides/io.mpv.Mpv"
before=$(snap)
runi install
check "3 override appended" has "filesystems=xdg-videos;$HOME/.local/share/pushmpc:ro;" "$HOME/.local/share/flatpak/overrides/io.mpv.Mpv"
runi uninstall
check "3 override file restored byte for byte" same "$(snap)" "$before"

# 4. no usable mpv: --flatpak yes installs the Flathub mpv (user), uninstall removes it and the remote PushMPC added
fresh
before=$(snap)
runi install --flatpak yes; check "4 install exits 0" same $? 0
check "4 Flathub remote added" test -f "$HOME/.fp-remote"
check "4 Flathub mpv installed" test -f "$HOME/.fp-installed"
check "4 Flatpak mpv.conf written" has 'vf-append=@pushmpc' "$HOME/.var/app/io.mpv.Mpv/config/mpv/mpv.conf"
runi uninstall; check "4 uninstall exits 0" same $? 0
check "4 Flathub mpv removed" test ! -e "$HOME/.fp-installed"
check "4 Flathub remote removed" test ! -e "$HOME/.fp-remote"
check "4 restored byte for byte" same "$(snap)" "$before"

# 5. no usable mpv, --flatpak no (and no terminal): nothing configured, clear message
fresh
runi install --flatpak no; check "5 install exits 0" same $? 0
check "5 says no player can use it" has 'no player can use it yet' "$out"
check "5 nothing installed from Flathub" test ! -e "$HOME/.fp-installed"
runi install < /dev/null; check "5 no terminal: not asked" has 'not asked: no terminal' "$out"
runi uninstall

# 6. edits after the install: only PushMPC's parts go, the user's edits stay
fresh
MPV_VS=1
mkdir -p "$HOME/.config/haruna" "$HOME/.kodi/userdata" "$HOME/.config/smplayer" "$HOME/.config/mpc-qt"
printf 'Counter=0\n' > "$HOME/.config/haruna/custom-commands.conf"
printf '[advanced]\nmplayer_additional_options=\n' > "$HOME/.config/smplayer/smplayer.ini"
runi install
printf '\n[Command_9]\nCommand=set volume 50\nType=startup\n' >> "$HOME/.config/haruna/custom-commands.conf"
sed -i 's#</playercorefactory>#<!-- mine -->\n</playercorefactory>#' "$HOME/.kodi/userdata/playercorefactory.xml"
sed -i 's#^mplayer_additional_options=\(.*\)#mplayer_additional_options=--fs \1#' "$HOME/.config/smplayer/smplayer.ini"
sed -i 's/"playerOpenMode"/"x"/' "$HOME/.config/mpc-qt/settings.json" 2>/dev/null
runi uninstall; check "6 uninstall exits 0" same $? 0
check "6 Haruna: PushMPC groups gone" hasnot 'PushMPC_' "$HOME/.config/haruna/custom-commands.conf"
check "6 Haruna: the user's later command kept" has 'Command=set volume 50' "$HOME/.config/haruna/custom-commands.conf"
check "6 Kodi: our parts gone" hasnot 'PushMPC' "$HOME/.kodi/userdata/playercorefactory.xml"
check "6 Kodi: the user's edit kept" has '<!-- mine -->' "$HOME/.kodi/userdata/playercorefactory.xml"
check "6 SMPlayer: our flags gone, --fs kept" same "$(grep '^mplayer_additional_options=' "$HOME/.config/smplayer/smplayer.ini")" 'mplayer_additional_options=--fs'
check "6 Kodi: created file kept as the user changed it" test -f "$HOME/.kodi/userdata/playercorefactory.xml"

# 7. dry run writes nothing
fresh
MPV_VS=1
before=$(snap)
runi install --dry-run; check "7 dry run exits 0" same $? 0
check "7 dry run changes nothing" same "$(snap)" "$before"

echo "install.sh tests: $pass passed, $fail failed"
[ "$fail" = 0 ]
