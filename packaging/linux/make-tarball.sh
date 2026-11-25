#!/bin/sh
# Builds the Linux release tarball pushmpc-linux-<arch>-<version>.tar.gz (user-level install, no root):
#   pushmpc-linux-<arch>-<version>/
#     setup.sh                   integrations/linux/setup.sh (the one command: every player, engine plan, uninstall)
#     install.sh                 integrations/linux/install.sh (install | uninstall | status)
#     mpv-integration.sh         integrations/mpv/mpv-integration.sh (the mpv.conf block)
#     vsplugin/pushmpc_vs.so     the VapourSynth plugin (portable backend)
#     vsplugin/pushmpc.vpy       the script mpv runs (vsplugin/scripts/pushmpc.vpy)
#     vsplugin/pushmpc.lua       adds the filter at run time (integrations/mpv/pushmpc.lua)
#     vsplugin/shaders/          ArtCNN C4F16 / C4F32 fragment shaders (integrations/mpv/make-shaders.py)
#     LICENSE, README.txt
#   make-tarball.sh <pushmpc_vs.so> <out dir> <arch> <version> [<pushmpc-nvvfx-selftest>]   (run from the repository root)
set -eu
so=$1; arch=$3; ver=$4
mkdir -p "$2"; out=$(cd "$2" && pwd)
name=pushmpc-linux-$arch-$ver
tmp=$(mktemp -d)
d=$tmp/$name
mkdir -p "$d/vsplugin"
install -m 755 integrations/linux/setup.sh "$d/setup.sh"
install -m 755 integrations/linux/install.sh "$d/install.sh"
# optional: the VLC module (PUSHMPC_VLC_PLUGIN=<libpushmpc_plugin.so>) and the Kodi add-on folder with its install.sh
# (PUSHMPC_KODI_DIR), which setup.sh installs when VLC / Kodi is found
if [ -n "${PUSHMPC_VLC_PLUGIN:-}" ] && [ -f "$PUSHMPC_VLC_PLUGIN" ]; then mkdir -p "$d/vlc"; install -m 755 "$PUSHMPC_VLC_PLUGIN" "$d/vlc/libpushmpc_plugin.so"; install -m 755 vlcplugin/install/pushmpc-vlc.sh "$d/vlc/pushmpc-vlc.sh"; fi
if [ -n "${PUSHMPC_KODI_DIR:-}" ] && [ -f "$PUSHMPC_KODI_DIR/install.sh" ]; then mkdir -p "$d/kodi"; cp -R "$PUSHMPC_KODI_DIR/." "$d/kodi/"; fi
install -m 755 integrations/mpv/mpv-integration.sh "$d/mpv-integration.sh"
install -m 755 "$so" "$d/vsplugin/pushmpc_vs.so"
install -m 644 vsplugin/scripts/pushmpc.vpy "$d/vsplugin/pushmpc.vpy"
install -m 644 integrations/mpv/pushmpc.lua "$d/vsplugin/pushmpc.lua"
# the ArtCNN upscaling shaders (generated from the pinned ONNX weights; Python 3, stdlib only)
python3 integrations/mpv/make-shaders.py "$d/vsplugin/shaders"
install -m 644 LICENSE "$d/LICENSE"
# the Linux NVIDIA backend's runtime fetcher (NVIDIA's files are downloaded on the user's machine, never shipped) and
# its self-test (optional 5th argument), when this build has them
[ -f scripts/nvidia-vfx-fetch.sh ] && install -m 755 scripts/nvidia-vfx-fetch.sh "$d/nvidia-vfx-fetch.sh"
[ -f scripts/nvidia-vfx-pin.json ] && install -m 644 scripts/nvidia-vfx-pin.json "$d/nvidia-vfx-pin.json"
[ -n "${5:-}" ] && [ -f "$5" ] && install -m 755 "$5" "$d/vsplugin/pushmpc-nvvfx-selftest"
cat > "$d/README.txt" <<EOF
PushMPC $ver for Linux ($arch): frame generation and the scene-cut guard for mpv and mpv-based players.

Install (for your user only, no administrator password):
    sh setup.sh
Remove:
    sh ~/.local/share/pushmpc/setup.sh uninstall

mpv needs VapourSynth. Arch, Manjaro and EndeavourOS have it; Ubuntu, Debian and Mint build mpv without it, so the
installer offers mpv from Flathub (io.mpv.Mpv, user install) instead. Players set up when found: mpv, mpv from
Flathub, Celluloid, Haruna, SMPlayer, MPC-QT, Jellyfin MPV Shim, Plex-MPV-Shim and Kodi (mpv as its external player).
VLC gets the PushMPC module when this download has it (vlc/). setup.sh prints the engine Auto starts with for your
graphics card (sh setup.sh plan). Every change is backed up and undone exactly by the uninstall.

Settings: ~/.config/pushmpc/settings.ini (Smoother=Auto, 2..8 or Off). Logs: ~/.local/state/pushmpc/logs.
https://github.com/CptPakundo/PushMPC
EOF
# reproducible: fixed owner, sorted, the commit time as mtime when available
mt=${SOURCE_DATE_EPOCH:-$(git log -1 --format=%ct 2>/dev/null || date +%s)}
(cd "$tmp" && tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$mt" -czf "$out/$name.tar.gz" "$name")
rm -rf "$tmp"
( cd "$out" && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256" )
echo "$out/$name.tar.gz"
