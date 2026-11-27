# PushMPC for mpv on macOS (and Linux) through Homebrew: the VapourSynth plugin pushmpc.Enhance (backends apple =
# VideoToolbox ML frame interpolation + super resolution on Apple silicon, portable = PushFPS's motion-compensated
# interpolation on any machine), the mpv script and `pushmpc-setup`, which puts the filter into ~/.config/mpv/mpv.conf
# (and takes it out again exactly).
#
# This repository is the tap (Homebrew looks in Formula/):
#   brew tap CptPakundo/pushmpc https://github.com/CptPakundo/PushMPC
#   brew install CptPakundo/pushmpc/pushmpc   (the full name: Homebrew then trusts this formula from the tap)
#   pushmpc-setup install
# url / version / sha256 and the release resources below are filled in by the release (scripts/update-formula.py, run by
# release.yml's publish job on main once the assets exist). CI (.github/workflows/apple.yml, job homebrew) taps a clone
# of this repository with the same script pointing at the checkout's own archives, then installs and tests.
class Pushmpc < Formula
  desc "Frame generation and upscaling for mpv (VapourSynth filter)"
  homepage "https://github.com/CptPakundo/PushMPC"
  url "https://github.com/CptPakundo/PushMPC/releases/download/v0.6.0/pushmpc-0.6.0-src.tar.gz"
  version "0.6.0"
  sha256 "0000000000000000000000000000000000000000000000000000000000000000"
  license "GPL-3.0-or-later"
  head "https://github.com/CptPakundo/PushMPC.git", branch: "main"

  depends_on "cmake" => :build
  depends_on "pkgconf" => :build
  # Homebrew's mpv is built with VapourSynth (it depends on the vapoursynth formula)
  depends_on "ffmpeg" # headers for the IINA bridge (mpv depends on it anyway)
  depends_on "mpv"
  # PushFlow (RIFE) through pmml, the plugin's ml backend: Homebrew's ONNX Runtime (built with CoreML on macOS)
  depends_on "onnxruntime"
  depends_on "vapoursynth"
  # capture mode on macOS (pushmpc-capture: QuickTime Player, Infuse, browsers): its shaders, and Vulkan on Metal; the
  # portable GPU engine's Vulkan driver (Apple silicon and Intel Macs): the engine finds Homebrew's libMoltenVK.dylib by itself
  on_macos do
    # the IINA bridge for apps that bundle an older FFmpeg (Stremio 5.1: FFmpeg 7): headers only, at build time
    depends_on "ffmpeg@7" => :build
    depends_on "ffmpeg@8" => :build
    depends_on "glslang" => :build
    depends_on "molten-vk"
  end

  # pmml's graphs (pmml/scripts/make_graphs.py output, GPL-3.0), a release asset: release.yml builds it; at release time
  # set url / sha256 as for the tarball above (CI replaces both with a freshly generated archive)
  resource "pmml-graphs" do
    url "https://github.com/CptPakundo/PushMPC/releases/download/v0.6.0/pmml-graphs-0.6.0.tar.gz"
    sha256 "0000000000000000000000000000000000000000000000000000000000000000"
  end

  # PushMPC for Plex for Mac / Plex HTPC (mpv-native.yml builds it, release.yml attaches it): the universal proxy
  # libavfilter.8.dylib, the vf_pushmpc builds, the engines (VideoToolbox, Vulkan on MoltenVK, CPU), the self-test and
  # pushmpc-plex-mac.sh. Kept as the archive (Homebrew does not relocate or re-sign what goes into Plex); pushmpc-setup
  # unpacks it and puts it into the Plex apps. A release without that asset leaves this block out
  on_macos do
    resource "plex-mac" do
      url "https://github.com/CptPakundo/PushMPC/releases/download/v0.6.0/pushmpc-plex-mac-0.6.0.tar.gz"
      sha256 "0000000000000000000000000000000000000000000000000000000000000000"
    end
  end

  # the portable GPU engine (vsplugin/engines/gpu): Vulkan-Headers and volk, the commits vsplugin/engines/gpu/gpu-pins.json pins
  resource "vulkan-headers" do
    url "https://github.com/KhronosGroup/Vulkan-Headers/archive/b5c8f996196ba4aa6d8f97e52b5d3b6e70f7e4e2.tar.gz"
    sha256 "7937d8ec21785736436d9ff52564db7d55992070b5ac98324472cce6e425a7ba"
  end
  resource "volk" do
    url "https://github.com/zeux/volk/archive/d979819fd03de3a7606d6e23d6ff4968942599da.tar.gz"
    sha256 "ff6facfca90161325a1f817c08c7edb6f892bd3bf4745449b735d7e13585ec4c"
  end

  # PushMPC for Kodi on the Mac (kodi-addon.yml builds it, release.yml attaches it): the add-on inputstream.pushmpc
  # (universal, for Kodi 21 and 22) and its installer. Kept as the archive; pushmpc-setup unpacks it and puts the add-on
  # into Kodi. A release without that asset leaves this block out
  on_macos do
    resource "kodi-mac" do
      url "https://github.com/CptPakundo/PushMPC/releases/download/v0.6.0/pushmpc-kodi-macos-0.6.0.tar.gz"
      sha256 "0000000000000000000000000000000000000000000000000000000000000000"
    end
  end

  # ArtCNN (MIT, github.com/Artoriuz/ArtCNN), pinned to one commit (vsplugin/models/artcnn.lock.json)
  resource "artcnn-c4f16-onnx" do
    url "https://raw.githubusercontent.com/Artoriuz/ArtCNN/0837e5326e710f49e2ab0a8d995da0708047442e/ONNX/ArtCNN_C4F16.onnx"
    sha256 "7a6436299b8c757dbb7084ac39a25c310db1941133e73d1ee3e74c8b3bddc1ca"
  end
  resource "artcnn-c4f32-onnx" do
    url "https://raw.githubusercontent.com/Artoriuz/ArtCNN/0837e5326e710f49e2ab0a8d995da0708047442e/ONNX/ArtCNN_C4F32.onnx"
    sha256 "b1a1130469a0538d4484fc3d6ca39854ef3906a3a01df8799997b6ed0408f6ea"
  end
  resource "artcnn-c4f16-glsl" do
    url "https://raw.githubusercontent.com/Artoriuz/ArtCNN/0837e5326e710f49e2ab0a8d995da0708047442e/GLSL/ArtCNN_C4F16.glsl"
    sha256 "03d0b3d31cb82c898a94a46663021a3e8f02c5a21d69c5cfdf0208de4bfd453e"
  end
  resource "artcnn-c4f32-glsl" do
    url "https://raw.githubusercontent.com/Artoriuz/ArtCNN/0837e5326e710f49e2ab0a8d995da0708047442e/GLSL/ArtCNN_C4F32.glsl"
    sha256 "f773bce6cf5fe7e5e5d599a695edd40df5cd7a20c3d08c4d164d07591d5bead3"
  end

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
    # macOS: also the IINA bridge (pushmpc-iina.so, an mpv C plugin), built against Homebrew's FFmpeg and mpv headers,
    # and once more for each older FFmpeg major (ffmpeg@7, ffmpeg@8: pushmpc-iina-av<libavutil major>.so); a bridge
    # refuses to patch anything when the app's FFmpeg majors differ from its own, so pushmpc-apps-mac.sh picks per app
    bridge = if OS.mac?
      ["-DPUSHMPC_IINA_BRIDGE=ON", "-DFFMPEG_INCLUDE=#{Formula["ffmpeg"].opt_include}", "-DMPV_INCLUDE=#{Formula["mpv"].opt_include}",
       "-DPUSHMPC_IINA_FFMPEG_INCLUDES=#{Formula["ffmpeg@7"].opt_include};#{Formula["ffmpeg@8"].opt_include}"]
    else
      []
    end
    # the ml backend: ONNX Runtime's C API headers from Homebrew's onnxruntime (loaded at run time, nothing linked)
    ort = Formula["onnxruntime"].opt_include
    ort /= "onnxruntime" if (ort/"onnxruntime/onnxruntime_c_api.h").exist?
    # the portable GPU engine (Vulkan: MoltenVK on macOS): the pinned headers and volk, the checked-in SPIR-V
    gpu = buildpath/"gpu-deps"
    resource("vulkan-headers").stage(gpu/"vulkan-headers")
    resource("volk").stage(gpu/"volk")
    gpu_args = ["-DPMVS_GPU=ON", "-DPMVS_VULKAN_HEADERS_DIR=#{gpu}/vulkan-headers", "-DPMVS_VOLK_DIR=#{gpu}/volk"]
    system "cmake", "-S", "vsplugin", "-B", "build", "-DVAPOURSYNTH_SDK=#{inc}", "-DPUSHMPC_APPLE=#{apple}", "-DPMML_ORT_INCLUDE=#{ort}",
           *bridge, *gpu_args, *std_cmake_args
    system "cmake", "--build", "build", "--target", "pushmpc_vs", "--parallel"
    system "cmake", "--build", "build", "--target", "pushmpc_apple", "pushmpc_iina", "--parallel" if OS.mac?
    system "cmake", "--build", "build", "--target", "portable_test", "apple_test", "--parallel"
    system "build/portable_test"
    system "build/apple_test"
    # the image model's helper for plain mpv (pushmpc.lua starts it when no PushMPC engine runs), with its unit test
    system "cmake", "-S", "integrations/mpv/content-helper", "-B", "build-content", "-DPMML_ORT_INCLUDE=#{ort}", *std_cmake_args
    system "cmake", "--build", "build-content", "--target", "pushmpc-content", "content_helper_test", "--parallel"
    system "build-content/content_helper_test"

    # capture mode (macOS): pushmpc-capture inside an app bundle, because macOS gives the Screen Recording permission to
    # an app ("PushMPC Capture" in System Settings > Privacy & Security). Its own build folder with the GPU engine.
    if OS.mac?
      # the same pinned Vulkan-Headers and volk as the plugin's GPU engine (staged above)
      system "cmake", "-S", "vsplugin", "-B", "build-capture", "-DPUSHMPC_APPLE=ON", "-DPMML_ORT_INCLUDE=#{ort}",
             "-DPMVS_VULKAN_HEADERS_DIR=#{gpu}/vulkan-headers", "-DPMVS_VOLK_DIR=#{gpu}/volk",
             "-DPMVS_GLSLANG=#{Formula["glslang"].opt_bin}/glslangValidator", *std_cmake_args
      system "cmake", "--build", "build-capture", "--target", "pushmpc_capture", "capture_test", "--parallel"
      system "build-capture/capture_test"
      app = libexec/"PushMPC Capture.app/Contents"
      (app/"MacOS").install "build-capture/pushmpc-capture"
      # MoltenVK's first instance in a process of its own (mac_vk_probe.h): looked up next to pushmpc-capture
      (app/"MacOS").install "build-capture/pushmpc-vulkan-probe"
      (app/"Info.plist").write <<~XML
        <?xml version="1.0" encoding="UTF-8"?>
        <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
        <plist version="1.0">
        <dict>
          <key>CFBundleIdentifier</key><string>org.pushmpc.capture</string>
          <key>CFBundleName</key><string>PushMPC Capture</string>
          <key>CFBundleDisplayName</key><string>PushMPC Capture</string>
          <key>CFBundleExecutable</key><string>pushmpc-capture</string>
          <key>CFBundlePackageType</key><string>APPL</string>
          <key>CFBundleShortVersionString</key><string>#{version}</string>
          <key>CFBundleVersion</key><string>#{version}</string>
          <key>LSMinimumSystemVersion</key><string>13.3</string>
          <key>LSUIElement</key><true/>
        </dict>
        </plist>
      XML
      system "codesign", "--force", "--sign", "-", libexec/"PushMPC Capture.app"
      libexec.install "scripts/pushmpc-capture.sh"
      # the app itself must start the capture (macOS asks the starting app for the permission, not the terminal): open
      # starts it as the app; --list / --help / --selftest need no permission and run here
      (bin/"pushmpc-capture").write <<~SH
        #!/bin/sh
        # pushmpc-capture [--player <app>] [options]: PushMPC capture mode (pushmpc-capture --help)
        exe="#{opt_libexec}/PushMPC Capture.app/Contents/MacOS/pushmpc-capture"
        for a in "$@"; do case $a in --list|--help|-h|--selftest|--no-present) exec "$exe" "$@" ;; esac; done
        exec open -W -n -a "#{opt_libexec}/PushMPC Capture.app" --args "$@"
      SH
    end

    # VapourSynth autoloads plugins from <site-packages>/vapoursynth/plugins (Homebrew links it into its prefix)
    plugin = Dir["vsplugin/bin/pushmpc_vs.*"].reject { |f| f.end_with?(".pdb") }.first
    (prefix/vs_site_packages/"vapoursynth/plugins").install plugin
    # macOS: MoltenVK's first instance runs in this helper before the plugin uses Vulkan (Metal can abort a process
    # inside vkCreateInstance on some GPUs; mac_vk_probe.h); it must sit next to the plugin
    probe = "vsplugin/bin/pushmpc-vulkan-probe"
    (prefix/vs_site_packages/"vapoursynth/plugins").install probe if OS.mac? && File.exist?(probe)
    pkgshare.install "vsplugin/scripts/pushmpc.vpy"
    # pushmpc.lua adds the filter at run time (the mpv.conf block names it, next to pushmpc.vpy)
    pkgshare.install "integrations/mpv/pushmpc.lua"
    pkgshare.install "build-content/pushmpc-content"   # next to pushmpc.lua (it looks there)
    libexec.install "build/pushmpc-apple" if OS.mac? && File.exist?("build/pushmpc-apple")
    (lib/"pushmpc").install Dir["build/pushmpc-iina*.so"] if OS.mac?   # pushmpc-iina.so and pushmpc-iina-av<N>.so
    libexec.install "integrations/mpv/mpv-integration.sh"
    if OS.mac? && resources.any? { |r| r.name == "plex-mac" }
      (pkgshare/"plex-mac").mkpath
      # packed again as it came (an archive in the keg is neither relocated nor re-signed by Homebrew)
      resource("plex-mac").stage { system "tar", "-czf", pkgshare/"plex-mac/pushmpc-plex-mac.tar.gz", "." }
    end
    if OS.mac? && resources.any? { |r| r.name == "kodi-mac" }
      (pkgshare/"kodi-mac").mkpath
      resource("kodi-mac").stage { system "tar", "-czf", pkgshare/"kodi-mac/pushmpc-kodi-mac.tar.gz", "." }
    end
    resource("pmml-graphs").stage { (pkgshare/"pmml/graphs").install Dir["*.onnx", "**/*.onnx"].uniq }
    (pkgshare/"pmml").install "scripts/ml-runtime-pin.json"
    libexec.install "packaging/homebrew/pmml-models.py"

    # ArtCNN (MIT): the pinned ONNX files become the Metal engine's .pmnn weights (next to the plugin, where the
    # apple backend looks first) and fragment-shader GLSL (OpenGL-only renderers); the upstream compute GLSL is used
    # by the gpu-next / Vulkan profile below. vsplugin/models/artcnn.lock.json has the same pins.
    py = Formula[vs_python].opt_bin/"python#{vs_python.delete_prefix("python@")}"
    models = prefix/vs_site_packages/"vapoursynth/plugins/pushmpc-models"
    models.mkpath
    (pkgshare/"shaders").mkpath
    %w[C4F16 C4F32].each do |m|
      resource("artcnn-#{m.downcase}-onnx").stage do
        system py, "-I", buildpath/"vsplugin/tools/artcnn_tool.py", "pmnn", "ArtCNN_#{m}.onnx", models/"ArtCNN_#{m}.pmnn"
        system py, "-I", buildpath/"vsplugin/tools/artcnn_tool.py", "glsl", "ArtCNN_#{m}.onnx",
               pkgshare/"shaders/ArtCNN_#{m}_fragment.glsl", "--name", "ArtCNN #{m}"
      end
      resource("artcnn-#{m.downcase}-glsl").stage { (pkgshare/"shaders").install "ArtCNN_#{m}.glsl" }
    end
    # the mpv profile: ArtCNN in mpv's own renderer (gpu-next, mpv's default vo since 0.41; Vulkan for compute shaders:
    # MoltenVK on macOS). It only runs when mpv upscales luma by more than 1.3x, so it never doubles our own super
    # resolution. It sets no vo, so encode mode (--o) keeps working.
    api = OS.mac? ? "gpu-api=vulkan\ngpu-context=macvk\n" : ""
    (pkgshare/"mpv").mkpath
    (pkgshare/"mpv/pushmpc-artcnn.conf").write <<~CONF
      # PushMPC: ArtCNN luma doubler in mpv's gpu-next renderer (included from the PushMPC block in mpv.conf)
      [pushmpc-artcnn]
      profile-desc=PushMPC: ArtCNN C4F16 luma doubler (gpu-next, Vulkan compute)
      #{api}glsl-shaders-append=#{opt_pkgshare}/shaders/ArtCNN_C4F16.glsl
      [pushmpc-artcnn32]
      profile-desc=PushMPC: ArtCNN C4F32 luma doubler (stronger GPUs)
      #{api}glsl-shaders-append=#{opt_pkgshare}/shaders/ArtCNN_C4F32.glsl
      [default]
      profile=pushmpc-artcnn
    CONF

    # Homebrew's mpv (0.41+) dlopen()s the VSScript library (libvsscript.dylib) by leaf name, but Homebrew's
    # vapoursynth keeps it inside its virtualenv, and dyld's fallback search is only /usr/lib: mpv's vf=vapoursynth
    # fails ("Failed to load VapourSynth VSScript library"). mpv honours VSSCRIPT_PATH, so a LuaJIT-FFI script in
    # Homebrew mpv's global scripts folder (<prefix>/etc/mpv/scripts: mpv is built with --sysconfdir=<prefix>/etc and
    # LuaJIT) sets it inside mpv itself. mpv runs every script's top level before it opens the first file (loadfile.c
    # waits until all clients are initialised), and vf_vapoursynth reads the variable later, at filter init. No
    # launchctl, no LaunchAgent, nothing for other apps. With --no-config / --load-scripts=no, set VSSCRIPT_PATH
    # yourself.
    vs_real = formula_opt_prefix("vapoursynth").realpath
    # VapourSynth R7x+ names it libvsscript (mpv tries libvsscript first, then libvapoursynth-script)
    vsscript = Dir["#{vs_real}/**/lib{vsscript,vapoursynth-script}.{dylib,so}"].min_by(&:length)
    opoo "no VSScript library in vapoursynth: #{Dir["#{vs_real}/**/*.{dylib,so}"].join(", ")}" unless vsscript
    vsscript_opt = vsscript ? formula_opt_prefix("vapoursynth")/Pathname(vsscript).relative_path_from(vs_real) : ""
    ohai "VSScript library: #{vsscript_opt}"
    (pkgshare/"mpv/pushmpc-vsscript.lua").write <<~LUA
      -- PushMPC (Homebrew): tells mpv's vf=vapoursynth where Homebrew's VSScript library is. Inert when PushMPC or
      -- VapourSynth is uninstalled, or when VSSCRIPT_PATH is already set.
      local lib = "#{vsscript_opt}"
      local marker = "#{opt_pkgshare}/pushmpc.vpy"
      local function exists(p) local f = io.open(p, "rb"); if f then f:close(); return true end; return false end
      if lib ~= "" and not os.getenv("VSSCRIPT_PATH") and exists(lib) and exists(marker) then
        local ok, ffi = pcall(require, "ffi")
        if ok then
          ffi.cdef("int setenv(const char *name, const char *value, int overwrite);")
          ffi.C.setenv("VSSCRIPT_PATH", lib, 0)
          mp.msg.verbose("PushMPC: VSSCRIPT_PATH=" .. lib)
        else
          mp.msg.warn("PushMPC: mpv's Lua has no FFI (not LuaJIT); set VSSCRIPT_PATH=" .. lib .. " yourself")
        end
      end
    LUA
    (bin/"pushmpc-setup").write <<~SH
      #!/bin/sh
      # pushmpc-setup [install|uninstall|status] [--no-artcnn] [mpv-integration.sh options]
      #   install    PushMPC in ~/.config/mpv/mpv.conf (a managed block: the filter, plus the ArtCNN gpu-next profile
      #              unless --no-artcnn), makes sure Homebrew mpv's global VSScript helper script is in place, and on
      #              macOS 26 offers Apple's one-time super-resolution model download (pushmpc-apple model-download)
      #   uninstall  takes the block out (mpv.conf comes back byte for byte) and removes the LaunchAgent of earlier
      #              PushMPC test builds, if any
      #   Kodi       on macOS, install also puts the PushMPC add-on into Kodi (in /Applications) for this user;
      #              uninstall takes it out again
      #   Plex       on macOS, install also puts PushMPC into Plex for Mac / Plex HTPC when they are in /Applications
      #              (pushmpc-plex-mac.sh: Plex's original library backed up, a self-test with Plex's own player, a
      #              LaunchAgent that re-applies it after Plex updates); uninstall gives Plex back exactly
      #   IINA, VLC  on macOS, install also sets up IINA (the PushMPC plugin and the bridge into IINA's own player) and VLC
      #              (frame generation and FSR upscaling) when they are installed, each with a self-test; neither app
      #              is modified, and uninstall takes both out again
      #   PushFlow   install also sets up PushFlow (RIFE) for the ml backend in ~/.local/share/pushmpc/pmml: Homebrew's ONNX
      #              Runtime (CoreML), the graphs, and PushFlow's models from PushFPS's release (pinned, about 130 MB the
      #              first time); --no-pmml skips it, uninstall removes it
      action=${1:-install}
      [ $# -gt 0 ] && shift
      artcnn=1; pmml=1
      n=$#
      while [ "$n" -gt 0 ]; do
        a=$1; shift; n=$((n - 1))
        if [ "$a" = --no-artcnn ]; then artcnn=0; elif [ "$a" = --no-pmml ]; then pmml=0; else set -- "$@" "$a"; fi
      done
      data=${XDG_DATA_HOME:-$HOME/.local/share}; pm="$data/pushmpc/pmml"
      if [ "$action" = install ] && [ "$pmml" = 1 ]; then
        mkdir -p "$pm/ort/core" "$pm/models"
        ln -sf "#{Formula["onnxruntime"].opt_lib}/libonnxruntime.dylib" "$pm/ort/core/libonnxruntime.dylib"
        rm -rf "$pm/graphs"; ln -s "#{opt_pkgshare}/pmml/graphs" "$pm/graphs"
        if "#{Formula[vs_python].opt_bin}/python#{vs_python.delete_prefix("python@")}" -I "#{opt_libexec}/pmml-models.py" "$pm/models" "#{opt_pkgshare}/pmml/ml-runtime-pin.json" "$data/pushmpc/pmml-download"; then
          echo "PushFlow for the ml backend: $pm"; : > "$pm/.pushmpc-setup"
        else
          echo "PushFlow's models could not be fetched: the ml backend stays off (run pushmpc-setup install again later)"; rm -rf "$pm"
        fi
        rm -rf "$data/pushmpc/pmml-download"
      fi
      # Stremio, Jellyfin Desktop and the Jellyfin / Plex MPV shims on macOS (pushmpc-apps-mac.sh in the Mac bundle): the
      # bridge inside the apps' own player (a small stand-in libmpv in the app, self-tested; a LaunchAgent puts it back
      # after an app update), the shims switched to Homebrew's mpv with the PushMPC block beside their conf.json
      appsdir="$data/pushmpc/plex-mac"; appstgz="#{opt_pkgshare}/plex-mac/pushmpc-plex-mac.tar.gz"
      if [ "$(uname -s)" = Darwin ]; then
        if [ "$action" = install ] && [ -f "$appstgz" ]; then
          rm -rf "$appsdir.new" && mkdir -p "$appsdir.new" && tar -xzf "$appstgz" -C "$appsdir.new" && rm -rf "$appsdir" && mv "$appsdir.new" "$appsdir"
        fi
        if [ -f "$appsdir/pushmpc-apps-mac.sh" ]; then
          ax=''; [ "$artcnn" = 0 ] || ax="include=\\"#{opt_pkgshare}/mpv/pushmpc-artcnn.conf\\""
          ag=''; [ "$action" != install ] || ag=--agent
          aout=$(/bin/sh "$appsdir/pushmpc-apps-mac.sh" "$action" --from "$appsdir" --bridge "#{opt_lib}/pushmpc/pushmpc-iina.so" \\
            --mpv "#{Formula["mpv"].opt_bin}/mpv" --integration "#{opt_libexec}/mpv-integration.sh" --vpy "#{opt_pkgshare}/pushmpc.vpy" \\
            --vsscript "#{HOMEBREW_PREFIX}/etc/mpv/scripts/pushmpc-vsscript.lua" ${ax:+--extra "$ax"} $ag 2>&1); arc=$?
          if [ "$arc" != 3 ] || ! printf '%s' "$aout" | grep -q 'none of these players'; then
            echo "Stremio, Jellyfin Desktop, MPV shims:"; printf '%s\\n' "$aout" | sed 's/^/  /'
          fi
        fi
      fi
      if [ "$action" = uninstall ] && [ -f "$pm/.pushmpc-setup" ]; then
        rm -rf "$pm"; rmdir "$data/pushmpc" "$data" 2>/dev/null || true; echo "PushFlow for the ml backend removed"
      fi
      # The Mac bundle (resource plex-mac, unpacked into $data/pushmpc/plex-mac): Plex for Mac / Plex HTPC (the proxy
      # libavfilter in the app, pushmpc-plex-mac.sh, with a LaunchAgent that puts it back after a Plex update), and IINA and
      # VLC (pushmpc-mac-players.sh: the IINA plugin and bridge, the VLC module with FSR; neither app is modified)
      plextgz="#{opt_pkgshare}/plex-mac/pushmpc-plex-mac.tar.gz"; plexdir="$data/pushmpc/plex-mac"
      bridge="#{opt_lib}/pushmpc/pushmpc-iina.so"
      has_plex() { [ -d /Applications/Plex.app ] || [ -d "/Applications/Plex HTPC.app" ]; }
      has_players() { [ -d /Applications/IINA.app ] || [ -d "$HOME/Applications/IINA.app" ] || [ -d /Applications/VLC.app ] || [ -d "$HOME/Applications/VLC.app" ]; }
      if [ "$(uname -s)" = Darwin ]; then
        case $action in
          install)
            if [ -f "$plextgz" ] && { has_plex || has_players; }; then
              if rm -rf "$plexdir.new" && mkdir -p "$plexdir.new" && tar -xzf "$plextgz" -C "$plexdir.new" && rm -rf "$plexdir" && mv "$plexdir.new" "$plexdir"; then
                if has_plex; then
                  echo "Plex for Mac:"
                  /bin/sh "$plexdir/pushmpc-plex-mac.sh" install --from "$plexdir" --agent || echo "  Plex: not set up (see above); the rest of PushMPC is"
                fi
                if has_players && [ -f "$plexdir/pushmpc-mac-players.sh" ]; then
                  echo "IINA and VLC:"
                  /bin/sh "$plexdir/pushmpc-mac-players.sh" install --from "$plexdir" --bridge "$bridge" | sed 's/^/  /'
                fi
                # the image model's runtime for IINA / VLC (pinned ONNX Runtime + the content model into
                # ~/Library/Application Support/PushMPC/pmml; without it the hand-built cues decide); --no-pmml skips it
                if [ "$pmml" = 1 ] && [ -f "$plexdir/setup.sh" ]; then
                  echo "Image model (content detection):"
                  { /bin/sh "$plexdir/setup.sh" pmml 2>&1 || echo "not installed (see above): the hand-built cues decide"; } | sed 's/^/  /'
                fi
              fi
            fi ;;
          uninstall)
            if [ -f "$plexdir/pushmpc-plex-mac.sh" ]; then
              if has_plex || [ -d "$HOME/Library/Application Support/PushMPC/plex-native" ]; then echo "Plex for Mac:"; /bin/sh "$plexdir/pushmpc-plex-mac.sh" uninstall; fi
              if [ -f "$plexdir/pushmpc-mac-players.sh" ]; then echo "IINA and VLC:"; /bin/sh "$plexdir/pushmpc-mac-players.sh" uninstall --from "$plexdir" | sed 's/^/  /'; fi
              rm -rf "$plexdir"; rmdir "$data/pushmpc" 2>/dev/null
            fi
            rm -rf "$HOME/Library/Application Support/PushMPC/pmml"; rmdir "$HOME/Library/Application Support/PushMPC" 2>/dev/null ;;
          status)
            [ ! -f "$plexdir/pushmpc-plex-mac.sh" ] || /bin/sh "$plexdir/pushmpc-plex-mac.sh" status --from "$plexdir"
            [ ! -f "$plexdir/pushmpc-mac-players.sh" ] || /bin/sh "$plexdir/pushmpc-mac-players.sh" status --from "$plexdir" ;;
        esac
      fi
      # Kodi: the add-on inputstream.pushmpc in Kodi's own player (install.sh from the package, unpacked into
      # $data/pushmpc/kodi-mac; it picks the Kodi 21 or 22 build and enables the add-on)
      koditgz="#{opt_pkgshare}/kodi-mac/pushmpc-kodi-mac.tar.gz"; kodidir="$data/pushmpc/kodi-mac"
      if [ "$(uname -s)" = Darwin ]; then
        case $action in
          install)
            if [ -f "$koditgz" ] && { [ -d /Applications/Kodi.app ] || [ -d "$HOME/Library/Application Support/Kodi" ]; }; then
              echo "Kodi:"
              if rm -rf "$kodidir.new" && mkdir -p "$kodidir.new" && tar -xzf "$koditgz" -C "$kodidir.new" && rm -rf "$kodidir" && mv "$kodidir.new" "$kodidir"; then
                PUSHMPC_KODI_DIR="$kodidir" /bin/sh "$kodidir/install.sh" install || echo "  Kodi: not set up (see above); the rest of PushMPC is"
              fi
            fi ;;
          uninstall)
            if [ -f "$kodidir/install.sh" ]; then echo "Kodi:"; PUSHMPC_KODI_DIR="$kodidir" /bin/sh "$kodidir/install.sh" uninstall; rm -rf "$kodidir"; rmdir "$data/pushmpc" 2>/dev/null; fi ;;
          status)
            [ ! -f "$kodidir/install.sh" ] || PUSHMPC_KODI_DIR="$kodidir" /bin/sh "$kodidir/install.sh" status ;;
        esac
      fi
      script_dir="#{HOMEBREW_PREFIX}/etc/mpv/scripts"
      helper="#{opt_pkgshare}/mpv/pushmpc-vsscript.lua"
      tool="#{opt_libexec}/pushmpc-apple"
      agent="$HOME/Library/LaunchAgents/com.pushmpc.vsscript.plist"
      case $action in
        install)
          if ! cmp -s "$helper" "$script_dir/pushmpc-vsscript.lua"; then
            mkdir -p "$script_dir" && cp "$helper" "$script_dir/pushmpc-vsscript.lua" && echo "VSScript helper: $script_dir/pushmpc-vsscript.lua"
          fi
          if [ "$(uname -s)" = Darwin ] && [ -x "$tool" ]; then
            case $("$tool" model-status 2>/dev/null) in
              "download required"*)
                if [ -t 0 ] && [ -z "${PUSHMPC_NO_PROMPT:-}" ]; then
                  printf "VideoToolbox's editing-grade super resolution (1080p to 4K) needs Apple's one-time model download. Download now? [Y/n] "
                  read -r ans
                  case $ans in
                    n*|N*) echo "skipped (Auto starts it in the background later; AppleSrModelDownload=never turns that off)" ;;
                    *) "$tool" model-download 900 || true ;;
                  esac
                fi ;;
            esac
          fi
          if [ "$artcnn" = 1 ]; then
            exec /bin/sh "#{opt_libexec}/mpv-integration.sh" install --script "#{opt_pkgshare}/pushmpc.vpy" --extra "include=\\"#{opt_pkgshare}/mpv/pushmpc-artcnn.conf\\"" "$@"
          fi ;;
        uninstall)
          if [ -f "$agent" ]; then
            rm -f "$agent"
            launchctl unsetenv VSSCRIPT_PATH 2>/dev/null
            echo "removed the LaunchAgent of an earlier PushMPC test build"
          fi ;;
        status)
          echo "VSScript helper: $([ -f "$script_dir/pushmpc-vsscript.lua" ] && echo "$script_dir/pushmpc-vsscript.lua" || echo absent)"
          [ -x "$tool" ] && echo "editing-grade super-resolution model: $("$tool" model-status 2>/dev/null)" ;;
      esac
      exec /bin/sh "#{opt_libexec}/mpv-integration.sh" "$action" --script "#{opt_pkgshare}/pushmpc.vpy" "$@"
    SH
  end

  def post_install
    # the global helper (see install). A file in etc survives upgrades; it is inert once PushMPC is uninstalled.
    (etc/"mpv/scripts").mkpath
    cp pkgshare/"mpv/pushmpc-vsscript.lua", etc/"mpv/scripts/pushmpc-vsscript.lua"
  end

  def caveats
    <<~EOS
      To use PushMPC in mpv, IINA and VLC, run:
        pushmpc-setup install
      Homebrew's mpv finds VapourSynth through a small global mpv script (#{HOMEBREW_PREFIX}/etc/mpv/scripts/
      pushmpc-vsscript.lua; it does nothing once PushMPC is uninstalled). mpv started with --no-config or
      --load-scripts=no needs VSSCRIPT_PATH set by hand.
      pushmpc-setup also adds the ArtCNN gpu-next profile (Vulkan; --no-artcnn leaves it out) and, on macOS 26,
      offers Apple's one-time download of VideoToolbox's editing-grade super-resolution model.
      Before `brew uninstall pushmpc`, run `pushmpc-setup uninstall` (your mpv.conf comes back byte for byte).
      Settings: ~/.config/pushmpc/settings.ini (Smoother=auto|off|2..8, Backend=auto|apple|portable,
      AppleSuperResolution=auto|off|2|4|editing|artcnn|lowlatency, Content=live|anime, AppleTenBit=auto|off,
      AppleSrModelDownload=auto|never). Logs: ~/Library/Logs/PushMPC.
      On macOS, pushmpc-setup install also puts the PushMPC add-on into Kodi (in /Applications) when it is there.
      On macOS, pushmpc-setup install also sets up Plex for Mac and Plex HTPC (in /Applications) when they are there, and
      re-applies itself after Plex updates. If macOS refuses, allow your terminal in System Settings > Privacy & Security >
      App Management, then run pushmpc-setup install again.
      It also sets up IINA (the PushMPC plugin) and VLC when they are installed; neither app is changed.
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
    assert_path_exists prefix/vs_site_packages/"vapoursynth/plugins/pushmpc-models/ArtCNN_C4F16.pmnn"
    refute_match "//!COMPUTE", (pkgshare/"shaders/ArtCNN_C4F16_fragment.glsl").read
    assert_match "ffi.C.setenv", (pkgshare/"mpv/pushmpc-vsscript.lua").read
  end
end
