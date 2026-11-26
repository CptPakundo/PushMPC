# Homebrew formula

This repository is its own Homebrew tap: the formula is [`Formula/pushmpc.rb`](../../Formula/pushmpc.rb) (Homebrew
looks in `Formula/`). This folder keeps its helper, `pmml-models.py`.

    brew tap CptPakundo/pushmpc https://github.com/CptPakundo/PushMPC
    brew install CptPakundo/pushmpc/pushmpc
    pushmpc-setup install      # mpv's mpv.conf, and Plex for Mac / Plex HTPC when they are in /Applications
    # to remove: pushmpc-setup uninstall && brew uninstall pushmpc

What the formula does:
- builds `pushmpc_vs` against Homebrew's `vapoursynth`, with the VideoToolbox backend on macOS, and installs it into
  VapourSynth's autoload folder;
- installs `pushmpc.vpy` and `pushmpc-setup`;
- depends on Homebrew's `mpv`, which is built with VapourSynth because it depends on the `vapoursynth` formula;
- installs a LuaJIT-FFI helper into Homebrew mpv's global scripts folder (`$(brew --prefix)/etc/mpv/scripts/
  pushmpc-vsscript.lua`) that sets `VSSCRIPT_PATH` inside mpv, because Homebrew's mpv cannot find VapourSynth's VSScript
  library on its own. No launchctl, no LaunchAgent; inert once PushMPC is uninstalled;
- converts the pinned ArtCNN weights (MIT) into the Metal engine's `.pmnn` files and fragment-shader GLSL, installs the
  upstream compute GLSL and an mpv profile (`pushmpc-artcnn`: gpu-next, Vulkan) that `pushmpc-setup install` includes
  (`--no-artcnn` leaves it out);
- on macOS builds `pushmpc-apple` (probe, Apple's one-time super-resolution model download, benchmarks) and the IINA
  bridge `lib/pushmpc/pushmpc-iina.so`;
- on macOS keeps the Mac bundle (resource `plex-mac`): `pushmpc-setup install` puts it into Plex for Mac and Plex HTPC
  (`mpvnative/install/pushmpc-plex-mac.sh`, with a LaunchAgent that re-applies it after Plex updates), IINA (the PushMPC
  plugin, which loads the bridge) and VLC (the frame generation module and the FSR output;
  `mpvnative/install/pushmpc-mac-players.sh`), each with a self-test.

Its `url` / `version` / `sha256` and the `pmml-graphs` and `plex-mac` resources point at a release's assets. The
release fills them in: `release.yml` builds the source tarball (`pushmpc-<version>-src.tar.gz`), and after a stable
release its publish job runs `scripts/update-formula.py` on main's formula and commits the result. A release without the
Plex for Mac asset leaves that resource out.

CI: `.github/workflows/apple.yml`, job `homebrew`, on macos-15: makes the three archives from the checkout, points a
clone of this repository at them with `scripts/update-formula.py`, runs `brew tap CptPakundo/pushmpc <clone>` and
`brew install CptPakundo/pushmpc/pushmpc` (the full name, which Homebrew 6 trusts), `brew test`, mpv in encode mode through the installed filter, and Plex for Mac through
`pushmpc-setup` (install, the re-apply agent, uninstall bit for bit).
