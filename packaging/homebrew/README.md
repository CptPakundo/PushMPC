# Homebrew formula (our own tap)

`pushmpc.rb` is the formula for a PushMPC tap. The tap is not public yet. It does four things:
- builds `pushmpc_vs` against Homebrew's `vapoursynth`, with the VideoToolbox backend on macOS;
- installs the plugin into VapourSynth's autoload folder;
- installs `pushmpc.vpy` and `pushmpc-setup`;
- depends on Homebrew's `mpv`, which is built with VapourSynth because it depends on the `vapoursynth` formula;
- installs a LuaJIT-FFI helper into Homebrew mpv's global scripts folder (`$(brew --prefix)/etc/mpv/scripts/
  pushmpc-vsscript.lua`) that sets `VSSCRIPT_PATH` inside mpv, because Homebrew's mpv cannot find VapourSynth's VSScript
  library on its own. No launchctl, no LaunchAgent; inert once PushMPC is uninstalled;
- converts the pinned ArtCNN weights (MIT) into the Metal engine's `.pmnn` files and fragment-shader GLSL, installs the
  upstream compute GLSL and an mpv profile (`pushmpc-artcnn`: gpu-next, Vulkan) that `pushmpc-setup install` includes
  (`--no-artcnn` leaves it out);
- on macOS builds `pushmpc-apple` (probe, Apple's one-time super-resolution model download, benchmarks) and the IINA
  bridge `lib/pushmpc/pushmpc-iina.so`.

Once the tap repository `homebrew-pushmpc` exists (a public action, so it waits for the go-ahead), users run:

    brew tap pushmpc/tap <URL of the homebrew-pushmpc tap repository>
    brew install pushmpc/tap/pushmpc
    pushmpc-setup install      # puts the filter into ~/.config/mpv/mpv.conf
    # to remove: pushmpc-setup uninstall && brew uninstall pushmpc

At release time, set `url` and `sha256` to the tag's tarball.

The CI dry run is `.github/workflows/apple.yml`, job `homebrew`, on macos-15:
1. builds a tarball of the checkout and rewrites `url`/`sha256` to point at it;
2. puts the formula into a local tap (`brew tap-new`), then runs `brew install --build-from-source` and `brew test`;
3. runs mpv in encode mode through the installed filter on a short public clip and checks the frame count.
