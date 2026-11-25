# PushMPC for mpv on macOS (and Linux) through Homebrew: the VapourSynth plugin pushmpc.Enhance (backends apple =
# VideoToolbox ML frame interpolation + super resolution on Apple silicon, portable = PushFPS's motion-compensated
# interpolation on any machine), the mpv script and `pushmpc-setup`, which puts the filter into ~/.config/mpv/mpv.conf
# (and takes it out again exactly).
#
# For our own tap (not homebrew-core):
#   brew tap pushmpc/tap <URL of the homebrew-pushmpc tap repository>
#   brew install pushmpc/tap/pushmpc
# The url / sha256 below point at the release tarball; CI (.github/workflows/apple.yml, job homebrew) installs this file from a local tap with
# the checkout's own tarball (`brew install --build-from-source`), then runs `brew test`.
class Pushmpc < Formula
  desc "Frame generation and upscaling for mpv (VapourSynth filter)"
  homepage "https://github.com/CptPakundo/PushMPC"
  url "https://github.com/CptPakundo/PushMPC/archive/refs/tags/v0.6.0.tar.gz"
  sha256 "0000000000000000000000000000000000000000000000000000000000000000"
  license "GPL-3.0-or-later"
  head "https://github.com/CptPakundo/PushMPC.git", branch: "main"

  depends_on "cmake" => :build
  depends_on "pkgconf" => :build
  # Homebrew's mpv is built with VapourSynth (it depends on the vapoursynth formula)
  depends_on "mpv"
  depends_on "vapoursynth"

  # the Python that Homebrew's vapoursynth is built for (its plugin folder lives in that Python's site-packages)
  def vs_python
    Formula["vapoursynth"].deps.map(&:name).grep(/^python@\d/).first || "python@3.15"
  end

  def vs_site_packages
    "lib/python#{vs_python.delete_prefix("python@")}/site-packages"
  end

  def install
    # the SDK headers (VapourSynth4.h) from Homebrew's vapoursynth: pkg-config first, then a search
    inc = begin
      Utils.safe_popen_read("pkgconf", "--variable=includedir", "vapoursynth").chomp
    rescue ErrorDuringExecution
      ""
    end
    inc = File.join(inc, "vapoursynth") if !inc.empty? && !File.exist?(File.join(inc, "VapourSynth4.h"))
    if inc.empty? || !File.exist?(File.join(inc, "VapourSynth4.h"))
      hdr = Dir["#{formula_opt_prefix("vapoursynth").realpath}/**/VapourSynth4.h"].first
      odie "VapourSynth4.h not found in the vapoursynth formula" unless hdr
      inc = File.dirname(hdr)
    end
    ohai "VapourSynth headers: #{inc}"

    apple = OS.mac? ? "ON" : "OFF"
    system "cmake", "-S", "vsplugin", "-B", "build", "-DVAPOURSYNTH_SDK=#{inc}", "-DPUSHMPC_APPLE=#{apple}", *std_cmake_args
    system "cmake", "--build", "build", "--target", "pushmpc_vs", "--parallel"
    system "cmake", "--build", "build", "--target", "portable_test", "apple_test", "--parallel"
    system "build/portable_test"
    system "build/apple_test"

    # VapourSynth autoloads plugins from <site-packages>/vapoursynth/plugins (Homebrew links it into its prefix)
    plugin = Dir["vsplugin/bin/pushmpc_vs.*"].reject { |f| f.end_with?(".pdb") }.first
    (prefix/vs_site_packages/"vapoursynth/plugins").install plugin
    pkgshare.install "vsplugin/scripts/pushmpc.vpy"
    libexec.install "integrations/mpv/mpv-integration.sh"

    # Homebrew's mpv (0.41+) dlopen()s the VSScript library (libvsscript.dylib) by leaf name, but Homebrew's
    # vapoursynth keeps it inside its virtualenv, and dyld's fallback search is only /usr/lib: mpv's vf=vapoursynth
    # fails ("Failed to load VapourSynth VSScript library"). mpv honours VSSCRIPT_PATH, so pushmpc-setup sets it for
    # the login session (launchctl setenv, which apps started from Finder, the Dock and new Terminal windows inherit)
    # and at every login (a LaunchAgent), and takes both out again on uninstall.
    vs_real = formula_opt_prefix("vapoursynth").realpath
    # VapourSynth R7x+ names it libvsscript (mpv tries libvsscript first, then libvapoursynth-script)
    vsscript = Dir["#{vs_real}/**/lib{vsscript,vapoursynth-script}.{dylib,so}"].min_by(&:length)
    opoo "no VSScript library in vapoursynth: #{Dir["#{vs_real}/**/*.{dylib,so}"].join(", ")}" unless vsscript
    vsscript_opt = vsscript ? formula_opt_prefix("vapoursynth")/Pathname(vsscript).relative_path_from(vs_real) : ""
    ohai "VSScript library: #{vsscript_opt}"
    (bin/"pushmpc-setup").write <<~SH
      #!/bin/sh
      # pushmpc-setup [install|uninstall|status] [mpv-integration.sh options]: PushMPC in ~/.config/mpv/mpv.conf, and
      # (macOS) VSSCRIPT_PATH for the login session and every login, so Homebrew's mpv finds VapourSynth.
      action=${1:-install}
      [ $# -gt 0 ] && shift
      vsscript="#{vsscript_opt}"
      agent="$HOME/Library/LaunchAgents/com.pushmpc.vsscript.plist"
      if [ "$(uname -s)" = Darwin ] && [ -n "$vsscript" ]; then
        case $action in
          install)
            mkdir -p "$HOME/Library/LaunchAgents"
            cat > "$agent" <<PLIST
      <?xml version="1.0" encoding="UTF-8"?>
      <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
      <plist version="1.0"><dict>
        <key>Label</key><string>com.pushmpc.vsscript</string>
        <key>ProgramArguments</key><array><string>/bin/launchctl</string><string>setenv</string><string>VSSCRIPT_PATH</string><string>$vsscript</string></array>
        <key>RunAtLoad</key><true/>
      </dict></plist>
      PLIST
            if launchctl setenv VSSCRIPT_PATH "$vsscript" 2>/dev/null; then echo "VSSCRIPT_PATH=$vsscript (this session and every login)"
            else echo "VSSCRIPT_PATH set from the next login (launchctl setenv failed here); now: export VSSCRIPT_PATH=$vsscript"; fi ;;
          uninstall)
            if [ -f "$agent" ]; then
              rm -f "$agent"
              [ "$(launchctl getenv VSSCRIPT_PATH 2>/dev/null)" = "$vsscript" ] && launchctl unsetenv VSSCRIPT_PATH 2>/dev/null
              echo "VSSCRIPT_PATH: LaunchAgent removed"
            fi ;;
          status)
            echo "VSSCRIPT_PATH: session '$(launchctl getenv VSSCRIPT_PATH 2>/dev/null)', LaunchAgent $([ -f "$agent" ] && echo present || echo absent)" ;;
        esac
      fi
      exec /bin/sh "#{opt_libexec}/mpv-integration.sh" "$action" --script "#{opt_pkgshare}/pushmpc.vpy" "$@"
    SH
  end

  def caveats
    <<~EOS
      To use PushMPC in mpv (and in IINA with "Use config directory" and a VapourSynth-enabled libmpv), run:
        pushmpc-setup install
      It also sets VSSCRIPT_PATH (this login session and a LaunchAgent for later logins): Homebrew's mpv
      cannot find VapourSynth's VSScript library on its own. Terminals already open need:
        export VSSCRIPT_PATH="$(launchctl getenv VSSCRIPT_PATH)"
      Before `brew uninstall pushmpc`, run `pushmpc-setup uninstall` (your mpv.conf comes back byte for byte).
      Settings: ~/.config/pushmpc/settings.ini (Smoother=auto|off|2..8, Backend=auto|apple|portable,
      AppleSuperResolution=auto|off|2|4). Logs: ~/Library/Logs/PushMPC.
      For IINA's own player, see the PushMPC IINA plugin (integrations/iina in the repository).
    EOS
  end

  test do
    (testpath/"t.vpy").write <<~PY
      import vapoursynth as vs
      core = vs.core
      clip = core.std.BlankClip(format=vs.YUV420P8, width=320, height=240, length=24, fpsnum=24000, fpsden=1001)
      clip = core.pushmpc.Enhance(clip, backend="portable", smoother="2")
      clip.set_output()
    PY
    info = shell_output("#{formula_opt_bin("vapoursynth")}/vspipe --info #{testpath}/t.vpy")
    assert_match(/Frames:\s+48/, info)
    assert_match(%r{FPS:\s+48000/1001}, info)
    assert_match "share/pushmpc/pushmpc.vpy", shell_output("#{bin}/pushmpc-setup status --conf #{testpath}/mpv.conf")
  end
end
