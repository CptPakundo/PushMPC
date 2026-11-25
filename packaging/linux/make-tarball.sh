#!/bin/sh
# Builds the Linux release tarball pushmpc-linux-<arch>-<version>.tar.gz (user-level install, no root):
#   pushmpc-linux-<arch>-<version>/
#     install.sh                 integrations/linux/install.sh (install | uninstall | status)
#     mpv-integration.sh         integrations/mpv/mpv-integration.sh (the mpv.conf block)
#     vsplugin/pushmpc_vs.so     the VapourSynth plugin (portable backend)
#     vsplugin/pushmpc.vpy       the script mpv runs (vsplugin/scripts/pushmpc.vpy)
#     LICENSE, README.txt
#   make-tarball.sh <pushmpc_vs.so> <out dir> <arch> <version>      (run from the repository root)
set -eu
so=$1; out=$2; arch=$3; ver=$4
name=pushmpc-linux-$arch-$ver
tmp=$(mktemp -d)
d=$tmp/$name
mkdir -p "$d/vsplugin" "$out"
install -m 755 integrations/linux/install.sh "$d/install.sh"
install -m 755 integrations/mpv/mpv-integration.sh "$d/mpv-integration.sh"
install -m 755 "$so" "$d/vsplugin/pushmpc_vs.so"
install -m 644 vsplugin/scripts/pushmpc.vpy "$d/vsplugin/pushmpc.vpy"
install -m 644 LICENSE "$d/LICENSE"
cat > "$d/README.txt" <<EOF
PushMPC $ver for Linux ($arch): frame generation and the scene-cut guard for mpv and mpv-based players.

Install (for your user only, no administrator password):
    sh install.sh
Remove:
    sh ~/.local/share/pushmpc/install.sh uninstall

mpv needs VapourSynth. Arch, Manjaro and EndeavourOS have it; Ubuntu, Debian and Mint build mpv without it, so the
installer offers mpv from Flathub (io.mpv.Mpv, user install) instead. Players set up when found: mpv, mpv from
Flathub, Celluloid, Haruna, SMPlayer, MPC-QT, Jellyfin MPV Shim, Plex-MPV-Shim and Kodi (mpv as its external player).
Every change is backed up and undone exactly by the uninstall.

Settings: ~/.config/pushmpc/settings.ini (Smoother=Auto, 2..8 or Off). Logs: ~/.local/state/pushmpc/logs.
https://github.com/CptPakundo/PushMPC
EOF
# reproducible: fixed owner, sorted, the commit time as mtime when available
mt=${SOURCE_DATE_EPOCH:-$(git log -1 --format=%ct 2>/dev/null || date +%s)}
(cd "$tmp" && tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$mt" -czf "$out/$name.tar.gz" "$name")
rm -rf "$tmp"
( cd "$out" && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256" )
echo "$out/$name.tar.gz"
