#!/bin/sh
# Tests for mpv-integration.sh against a fake HOME (no real config is touched). Run: sh mpv-integration.tests.sh
set -u
here=$(cd "$(dirname "$0")" && pwd)
tool=$here/../mpv-integration.sh
pass=0; fail=0
check() {
    desc=$1; shift
    if "$@"; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL: $desc"; fi
}
fresh() {
    HOME=$(mktemp -d "${TMPDIR:-/tmp}/pmhome.XXXXXX"); export HOME
    unset XDG_CONFIG_HOME XDG_DATA_HOME
    PUSHMPC_OS=linux; PUSHMPC_NO_PATH=1; export PUSHMPC_OS PUSHMPC_NO_PATH
    conf=$HOME/.config/mpv/mpv.conf
    out=$HOME/out.txt
}
run() { sh "$tool" "$@" > "$out" 2>&1; }
blocks() { grep -c '^# >>> PushMPC managed' "$1" | tr -d ' '; }
has() { grep -q -- "$1" "$2"; }
hasnot() { ! grep -q -- "$1" "$2"; }
same() { [ "$1" = "$2" ]; }

BEGIN_LINE='# >>> PushMPC managed: do not edit between these lines (PushMPC uninstall removes them)'

# 1. no mpv.conf and no folder: created, then removed with the folder
fresh
mkdir -p "$HOME/.config"
run install; check "install (absent) exits 0" same $? 0
check "absent: mpv.conf created" test -f "$conf"
check "absent: marker says created-dir" same "$(cat "$conf.pushmpc-absent")" created-dir
check "absent: block at line 1" same "$(head -n 1 "$conf")" "$BEGIN_LINE"
check "absent: scripts-append names pushmpc.lua next to the vpy" same "$(sed -n 2p "$conf")" "scripts-append=$HOME/.local/share/pushmpc/vsplugin/pushmpc.lua"
check "absent: script-opts carry the default script path" same "$(sed -n 3p "$conf")" "script-opts-append=pushmpc-vpy=$HOME/.local/share/pushmpc/vsplugin/pushmpc.vpy"
check "absent: no vf= line (a filter that cannot start would stop all playback)" hasnot '^vf-append=' "$conf"
check "absent: hwdec" has '^hwdec=auto-copy$' "$conf"
check "absent: hr-seek-framedrop" has '^hr-seek-framedrop=no$' "$conf"
check "absent: interpolation" has '^interpolation=no$' "$conf"
check "absent: missing script noted" has 'does not exist yet' "$out"
run uninstall; check "uninstall (absent) exits 0" same $? 0
check "absent: file removed" test ! -e "$conf"
check "absent: marker removed" test ! -e "$conf.pushmpc-absent"
check "absent: created folder removed" test ! -e "$HOME/.config/mpv"
check "absent: parent kept" test -d "$HOME/.config"

# 2. an existing mpv.conf without a final newline: backup, block on top, exact restore
fresh
mkdir -p "$HOME/.config/mpv"
printf 'profile=gpu-hq\n[slow]\nhwdec=vaapi\nvo=gpu-next' > "$conf"
cp "$conf" "$HOME/orig"
run install
check "existing: backup is the original" cmp -s "$conf.pushmpc-backup" "$HOME/orig"
check "existing: one block" same "$(blocks "$conf")" 1
check "existing: original kept below the block" same "$(sed -n 10p "$conf")" 'profile=gpu-hq'
check "existing: hwdec in a profile is no conflict" hasnot warning "$out"
run install
check "reinstall: still one block" same "$(blocks "$conf")" 1
check "reinstall: up to date" has 'up to date' "$out"
run install --buffered 6
check "new options: block replaced in place" same "$(blocks "$conf"):$(sed -n 4p "$conf" | grep -c 'pushmpc-buffered=6')" 1:1
run uninstall
check "existing: restored byte for byte" cmp -s "$conf" "$HOME/orig"
check "existing: backup removed" test ! -e "$conf.pushmpc-backup"
check "existing: restore reported" has 'restored exactly' "$out"

# 3. the user edits mpv.conf after the install: block removed, edits kept, backup kept
fresh
mkdir -p "$HOME/.config/mpv"
printf 'volume=80\n' > "$conf"
run install
printf 'sub-auto=fuzzy\n' >> "$conf"
run uninstall
check "edited: block gone" hasnot 'PushMPC managed' "$conf"
check "edited: edits kept" same "$(cat "$conf")" "$(printf 'volume=80\nsub-auto=fuzzy')"
check "edited: backup kept" test -f "$conf.pushmpc-backup"
check "edited: reported" has 'backup stays' "$out"

# 4. created by PushMPC, then the user added lines: block removed, additions kept, marker gone
fresh
mkdir -p "$HOME/.config/mpv"
run install
check "created in an existing folder: marker says file" same "$(cat "$conf.pushmpc-absent")" file
printf 'keep-open=yes\n' >> "$conf"
run uninstall
check "created+added: additions kept" same "$(cat "$conf")" 'keep-open=yes'
check "created+added: marker gone" test ! -e "$conf.pushmpc-absent"

# 5. conflicts are warned (global section only)
fresh
mkdir -p "$HOME/.config/mpv"
printf 'vf-append=vapoursynth=file=/opt/svp/svp.vpy\nhwdec=vaapi\ninterpolation=yes\n[x]\ninterpolation=yes\n' > "$conf"
run install
check "conflict: other VapourSynth filter" has 'another VapourSynth filter' "$out"
check "conflict: later global hwdec" has "later global 'hwdec=vaapi'" "$out"
check "conflict: global interpolation warned once" same "$(grep -c "interpolation=yes' turns" "$out" | tr -d ' ')" 1
sh "$tool" status > "$HOME/st.txt" 2>&1
check "status: installed" has 'PushMPC block: installed (lines 1-9)' "$HOME/st.txt"
check "status: warned" has 'warning: another VapourSynth' "$HOME/st.txt"
printf 'hwdec=vaapi-copy\n' > "$HOME/c2.conf"
run install --conf "$HOME/c2.conf"
check "no conflict: hwdec=vaapi-copy" hasnot warning "$out"

# 6. quoting: a path with spaces, a path with ]
fresh
run install --script "$HOME/my dir/push mpc.vpy" --conf "$HOME/a.conf"
p="$HOME/my dir/push mpc.vpy"; n=$(printf '%s' "$p" | wc -c | tr -d ' ')
check "spaces: script-opts value %n% quoted" same "$(sed -n 3p "$HOME/a.conf")" "script-opts-append=pushmpc-vpy=%$n%$p"
run install --direct --script "$HOME/my dir/push mpc.vpy" --conf "$HOME/a2.conf"
check "--direct: vf line, spaces quoted with []" same "$(sed -n 2p "$HOME/a2.conf")" "vf-append=@pushmpc:vapoursynth=file=[$HOME/my dir/push mpc.vpy]:buffered-frames=4:concurrent-frames=2"
p="$HOME/x]y/p.vpy"; n=$(printf '%s' "$p" | wc -c | tr -d ' ')
run install --direct --script "$p" --conf "$HOME/b.conf"
check "--direct: ] quoted with %n%" same "$(sed -n 2p "$HOME/b.conf")" "vf-append=@pushmpc:vapoursynth=file=%$n%$p:buffered-frames=4:concurrent-frames=2"

# 7. CRLF files keep CRLF in the block and restore exactly
fresh
mkdir -p "$HOME/.config/mpv"
printf 'volume=70\r\nmute=no\r\n' > "$conf"; cp "$conf" "$HOME/orig"
run install
check "crlf: block line ends in CR" same "$(head -n 1 "$conf" | tr -d '\n' | tail -c 1 | od -An -c | tr -d ' ')" '\r'
check "crlf: one block" same "$(blocks "$conf")" 1
run uninstall
check "crlf: exact restore" cmp -s "$conf" "$HOME/orig"

# 8. dry run changes nothing; uninstall without a block; XDG_CONFIG_HOME is honoured
fresh
mkdir -p "$HOME/.config/mpv"; printf 'a=1\n' > "$conf"; cp "$conf" "$HOME/orig"
run install --dry-run
check "dry-run: file unchanged" cmp -s "$conf" "$HOME/orig"
check "dry-run: no backup" test ! -e "$conf.pushmpc-backup"
check "dry-run: says what it would do" has '^would:' "$out"
run uninstall
check "no block: nothing to do" has 'no PushMPC block' "$out"
check "no block: file unchanged" cmp -s "$conf" "$HOME/orig"
XDG_CONFIG_HOME=$HOME/xdg; export XDG_CONFIG_HOME
mkdir -p "$XDG_CONFIG_HOME"
run install
check "XDG_CONFIG_HOME honoured" test -f "$HOME/xdg/mpv/mpv.conf"
unset XDG_CONFIG_HOME

# 9. players and notes
fresh
mkdir -p "$HOME/.config/celluloid" "$HOME/.config/haruna" "$HOME/.var/app/io.mpv.Mpv"
: > "$HOME/.config/haruna/haruna.conf"
sh "$tool" status > "$HOME/st.txt" 2>&1
check "status: Celluloid instruction" has "Celluloid: turn on Preferences > Config Files" "$HOME/st.txt"
check "status: Haruna note" has "Haruna: no documented switch" "$HOME/st.txt"
check "status: Flatpak mpv reported" has "Flatpak io.mpv.Mpv" "$HOME/st.txt"
check "status: no file" has '(no file)' "$HOME/st.txt"
check "status: no IINA note on Linux" hasnot IINA "$HOME/st.txt"
PUSHMPC_OS=darwin sh "$tool" status > "$HOME/st.txt" 2>&1
check "darwin: IINA note" has "IINA reads $HOME/.config/mpv only with Settings > Advanced > 'Use config directory'" "$HOME/st.txt"
check "darwin: default script in Application Support" has "Library/Application Support/PushMPC/vsplugin/pushmpc.vpy" "$HOME/st.txt"
sh "$tool" bogus > /dev/null 2>&1
check "bad action exits 2" same $? 2

echo "mpv-integration.tests.sh: $pass passed, $fail failed"
[ "$fail" = 0 ]
