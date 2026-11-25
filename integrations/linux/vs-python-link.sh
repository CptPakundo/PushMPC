#!/bin/sh
# vs-python-link.sh: makes VapourSynth's script library (VSScript, which mpv loads) find Python, for the current user.
# VapourSynth R73 and newer read ${XDG_CONFIG_HOME:-~/.config}/vapoursynth/vapoursynth.toml, written by
# `vapoursynth config`; that entry names the library inside the Python package, while a distribution's mpv loads
# another copy (/usr/lib/libvapoursynth-script.so.0, /usr/lib64/libvsscript.so.0, ...), and mpv then stops with
# "Python executable and library path couldn't be determined". This runs `vapoursynth config` and adds the same Python
# entry for every VSScript library on the system. Used by the AUR package's pushmpc-setup (install.sh has the same
# step with backup and restore). Nothing to do for VapourSynth before R73.
set -u
cfg=${XDG_CONFIG_HOME:-}; case $cfg in /*) ;; *) cfg=$HOME/.config ;; esac
t=$cfg/vapoursynth/vapoursynth.toml
if command -v vapoursynth >/dev/null 2>&1; then vapoursynth config >/dev/null 2>&1
elif python3 -c 'import vapoursynth, vapoursynth._utils' >/dev/null 2>&1; then python3 -m vapoursynth config >/dev/null 2>&1
else exit 0; fi
[ -f "$t" ] || exit 0
rest=$(grep '^"' "$t" | tail -n 1 | sed 's/^"[^"]*"//')
[ -n "$rest" ] || exit 0
for vs_lib in $( { /sbin/ldconfig -p 2>/dev/null || ldconfig -p 2>/dev/null; } | grep -o '/[^ ]*lib\(vapoursynth-script\|vsscript\)\.so[^ ]*' ) \
              $(find /usr/lib /usr/lib64 /usr/local/lib -maxdepth 4 \( -name 'libvapoursynth-script.so*' -o -name 'libvsscript.so*' \) 2>/dev/null); do
    for key in "$vs_lib" "$(readlink -f "$vs_lib")"; do
        key=$(printf '%s' "$key" | sed 's#/lib64/#/lib/#')
        [ -n "$key" ] || continue
        grep -qF "\"$key\"" "$t" && continue
        printf '"%s"%s\n' "$key" "$rest" >> "$t"
    done
done
exit 0
