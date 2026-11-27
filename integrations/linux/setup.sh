#!/bin/sh
# PushMPC Setup for Linux and macOS: the one command that sets PushMPC up for every player it finds, for your user
# only (no root, nothing outside your home folder). It finds your graphics card and your players, logs the engine
# Auto starts with (the same rules as the plugin's Auto: vsplugin backend.h), and runs the per-player installers:
#   install.sh        mpv, Flathub mpv, Celluloid, Haruna, SMPlayer, MPC-QT, the Jellyfin / Plex MPV shims,
#                     Jellyfin Desktop, Kodi (mpv as its external player), NVIDIA's Video Effects (Linux, licence step)
#   mpvnative/        PushMPC's native hook for the distro mpv (no VapourSynth; the same engines, Vulkan frames with no
#                     copy), when it activates in that mpv and carries this GPU's engine; else the VapourSynth route
#                     (never both)
#   pushmpc-vlc.sh    VLC 3 (native, Flathub, Snap), when this download has the VLC module (vlc/)
#   kodi/install.sh   the Kodi add-on, when this download has it (kodi/)
#   plex/             Plex desktop / HTPC from Flathub: PushMPC's filter in Plex's own player (LD_PRELOAD), when it
#                     passes its self-test there; plex/pushmpc-apps-linux.sh: Snap Plex / Plex HTPC, Flathub Stremio and
#                     Jellyfin Desktop, Stremio 4, Emby Theater (each self-tested through its own libmpv)
#   pmml              PushFlow (RIFE) on the GPU for the mpv route: ONNX Runtime and the EP for this GPU (TensorRT-RTX on an
#                     NVIDIA RTX card, OpenVINO on Intel, MIGraphX on AMD with ROCm installed) fetched by pmml's own
#                     fetch_runtime.py and ml-runtime-pin.json (official sources, SHA-256 checked), PushFlow's models from
#                     PushFPS's release (pinned), pmml's graphs from this download, into .../pushmpc/pmml (default on;
#                     --no-pmml declines it; needs python3)
#   macOS             Homebrew's pushmpc-setup (mpv, IINA) and the VLC module; the content model for IINA's bridge with
#                     its ONNX Runtime (Apple silicon or Intel, pinned), into ~/Library/Application Support/PushMPC/pmml
#   capture/          capture mode (pushmpc-capture) for players with no other route here: Flathub Celluloid, Stremio,
#                     Totem, Clapper, Dragon, Snap Plex (menu entries "<Player> with PushMPC"); browsers on request
#                     (sh ~/.local/share/pushmpc/capture/pushmpc-capture.sh enable firefox)
#
#   sh setup.sh [install]  [--yes] [--dry-run] [--flatpak ask|yes|no] [--nvidia-vfx ask|yes|no] [--no-pmml]
#   sh setup.sh uninstall  [--keep-flatpak-mpv]
#   sh setup.sh plan       the engine Auto starts with, per player route (no change)
#   sh setup.sh status
#   sh setup.sh pmml       only the PushFlow runtime (the same step as in install; uninstall removes it); on macOS the
#                          content model with its ONNX Runtime
# Nothing is asked unless a download needs your yes: the Flathub mpv (Ubuntu / Debian mpv cannot run VapourSynth) and
# NVIDIA's Video Effects (NVIDIA's licence). Both are offered once in a terminal; --yes accepts both ahead, --flatpak /
# --nvidia-vfx answer each one. Without a terminal neither is downloaded.
# What it changes is recorded in ${XDG_DATA_HOME:-~/.local/share}/pushmpc (setup-state, auto-plan.txt) and undone by
# uninstall. Tests: PUSHMPC_TEST_GPU="<vendor hex>:<device hex>:<name>" stands in for the graphics card, PUSHMPC_TEST_VULKAN
# for the Vulkan device name (empty: none), PUSHMPC_TEST_NVIDIA_DRIVER=1 for the proprietary driver;
# PUSHMPC_TEST_UNAME=Darwin|Linux for the OS, PUSHMPC_TEST_MAC_ARCH=arm64|x86_64 for the Mac's architecture.
set -u

action=install
case ${1:-} in install|uninstall|status|plan|pmml) action=$1; shift ;; esac
pass=''; yes=0; dry=0; nopmml=0
while [ $# -gt 0 ]; do
    case $1 in
        --flatpak|--nvidia-vfx) pass="$pass $1 $2"; shift 2 ;;
        --yes|-y) yes=1; pass="$pass --yes"; shift ;;
        --dry-run) dry=1; pass="$pass --dry-run"; shift ;;
        --keep-flatpak-mpv) pass="$pass $1"; shift ;;
        --no-pmml) nopmml=1; shift ;;
        -h|--help) sed -n '2,/^set -u/p' "$0" | sed '$d'; exit 0 ;;
        *) echo "setup.sh: unknown option $1" >&2; exit 2 ;;
    esac
done

here=$(cd "$(dirname "$0")" && pwd)
os=${PUSHMPC_TEST_UNAME:-$(uname -s)}
data=${XDG_DATA_HOME:-}; case $data in /*) ;; *) data=$HOME/.local/share ;; esac
[ "$os" = Darwin ] && [ -z "${XDG_DATA_HOME:-}" ] && data=$HOME/.local/share
pm=$data/pushmpc
sstate=$pm/setup-state
plan=$pm/auto-plan.txt

say() { printf '%s\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }
record() { [ "$dry" = 1 ] && return 0; mkdir -p "$pm"; grep -q -F -x "$*" "$sstate" 2>/dev/null || printf '%s\n' "$*" >> "$sstate"; }
recorded() { [ -f "$sstate" ] && grep -q -F -x "$*" "$sstate"; }

# ------------------------------------------------------------------------------------------------ the graphics card
# gpu_vendor (hex, lower case), gpu_device, gpu_name; the one pushcore / the GPU engine would take: NVIDIA, AMD, Intel
detect_gpu() {
    gpu_vendor=''; gpu_device=''; gpu_name=''; gpu_src=''
    [ "${PUSHMPC_TEST_GPU:-}" = none ] && { gpu_src=PUSHMPC_TEST_GPU; return; }
    if [ -n "${PUSHMPC_TEST_GPU:-}" ]; then
        gpu_vendor=$(printf '%s' "$PUSHMPC_TEST_GPU" | cut -d: -f1 | tr 'A-F' 'a-f')
        gpu_device=$(printf '%s' "$PUSHMPC_TEST_GPU" | cut -d: -f2 | tr 'A-F' 'a-f')
        gpu_name=$(printf '%s' "$PUSHMPC_TEST_GPU" | cut -d: -f3-); gpu_src=PUSHMPC_TEST_GPU; return
    fi
    if [ "$os" = Darwin ]; then
        gpu_name=$(sysctl -n machdep.cpu.brand_string 2>/dev/null)
        case $gpu_name in *Apple*) gpu_vendor=106b ;; *) gpu_name=$(system_profiler SPDisplaysDataType 2>/dev/null | sed -n 's/^ *Chipset Model: //p' | head -n 1) ;; esac
        gpu_src=sysctl; return
    fi
    best=9
    for d in /sys/class/drm/card[0-9]*/device; do
        [ -r "$d/vendor" ] || continue
        v=$(sed 's/^0x//' "$d/vendor" | tr 'A-F' 'a-f'); dv=$(sed 's/^0x//' "$d/device" 2>/dev/null | tr 'A-F' 'a-f')
        # virtual display adapters (Hyper-V, QEMU, VMware, virtio, VirtualBox) are no GPU
        case $v in 10de) r=0 ;; 1002) r=1 ;; 8086) r=2 ;; 1414|1234|15ad|1af4|80ee) continue ;; *) r=8 ;; esac
        [ "$r" -lt "$best" ] || continue
        best=$r; gpu_vendor=$v; gpu_device=$dv; gpu_src=sysfs
    done
    [ -n "$gpu_vendor" ] || return 0
    if [ "$gpu_vendor" = 10de ] && have nvidia-smi; then gpu_name=$(nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | head -n 1); fi
    [ -n "$gpu_name" ] || { have lspci && gpu_name=$(lspci -mm -d "$gpu_vendor:$gpu_device" 2>/dev/null | head -n 1 | awk -F'"' '{ print $6 }'); }
    [ -n "$gpu_name" ] || gpu_name="PCI $gpu_vendor:$gpu_device"
}
# pushcore's familyOf rules (PushCoreGpu.cpp), so the names match the Windows plan
gpu_family() {
    n=$gpu_name
    case $gpu_vendor in
        10de) case $n in *"RTX 50"*|*Blackwell*|*"RTX PRO"*) echo 'RTX 50' ;; *"RTX 40"*|*Ada*) echo 'RTX 40' ;; *"RTX 30"*|*"RTX A"*) echo 'RTX 30' ;;
                         *"RTX 20"*|*"TITAN RTX"*|*"Quadro RTX"*) echo 'RTX 20' ;; *RTX*) echo 'RTX 40' ;; *) echo GTX ;; esac ;;
        1002) d=$(printf '%s' "$n" | tr 'a-z' 'A-Z' | sed -n 's/.*RX *\([0-9]\).*/\1/p' | head -c 1)
              case $d in 9) echo 'RX 9000' ;; 7) echo 'RX 7000' ;; 6) echo 'RX 6000' ;; 5) echo 'RX 5000' ;; *) echo Radeon ;; esac ;;
        8086) echo Intel ;;
        106b) echo Apple ;;
        '') echo none ;;
        *) echo other ;;
    esac
}
vulkan_device() {
    if [ -n "${PUSHMPC_TEST_VULKAN+x}" ]; then echo "$PUSHMPC_TEST_VULKAN"; return; fi
    have vulkaninfo || { echo ''; return; }
    vulkaninfo --summary 2>/dev/null | sed -n 's/^[[:space:]]*deviceName[[:space:]]*= //p' | grep -v -i 'llvmpipe\|lavapipe\|swiftshader' | head -n 1
}

# ------------------------------------------------------------------------------------------------ the plan
# the mpv route's engine by backend.h SelectBackend: nvidia (Linux: the Video Effects runtime through CUDA) on RTX with
# the proprietary driver and NVIDIA's files, apple on a Mac that offers VideoToolbox interpolation, ml when the pmml
# runtime is installed, else portable (the GPU engine on a Vulkan device, else the CPU)
make_plan() {
    detect_gpu
    fam=$(gpu_family)
    vk=$(vulkan_device)
    nvprop=0; [ -e /proc/driver/nvidia/version ] && nvprop=1
    [ -n "${PUSHMPC_TEST_NVIDIA_DRIVER:-}" ] && nvprop=$PUSHMPC_TEST_NVIDIA_DRIVER
    vfx=0; [ -d "$pm/nvidia-vfx" ] && vfx=1
    pmml=''; [ -f "$pm/pmml/graphs/synth-yuv.onnx" ] && pmml=$pm/pmml
    if [ -n "$pmml" ]; then interp="ml (PushFlow through pmml on ${vk:-the GPU})"
    elif [ -n "$vk" ]; then interp="portable GPU engine on $vk"
    else interp="portable CPU engine"; fi
    if [ "$os" = Darwin ]; then
        if [ "$fam" = Apple ]; then mpv="apple: VideoToolbox frame interpolation and super resolution (macOS 26 and newer; else the portable engine on Metal); ArtCNN upscaling in mpv"
        else mpv="portable: the GPU engine on Metal (MoltenVK), else the CPU engine"; fi
        nv='not on macOS'
    else
        case $fam in
            'RTX 50'|'RTX 40') fg=vfg ;; 'RTX 30') fg=ampere ;; 'RTX 20') fg=none ;; *) fg=x ;;
        esac
        if [ "$fg" != x ] && [ "$nvprop" = 1 ]; then
            nv="offered: NVIDIA's Video Effects (RTX VSR, RTX Video HDR, frame generation) after you accept NVIDIA's licence"
            [ "$vfx" = 1 ] && nv='installed'
            case $fg in
                vfg) mpv="nvidia: RTX Video Super Resolution + NVIDIA VFG frame generation (NVIDIA's Video Effects runtime; until it is installed: $interp)" ;;
                ampere) mpv="nvidia: RTX Video Super Resolution; NVIDIA VFG through the RTX 30 port once its self-test passes (until then frame generation by $interp)" ;;
                none) mpv="nvidia: RTX Video Super Resolution; frame generation by $interp" ;;
            esac
        else
            if [ "$fg" != x ]; then nv="needs NVIDIA's proprietary driver (nouveau / NVK: the Vulkan path)"; else nv='not for this GPU'; fi
            if [ -n "$pmml" ]; then mpv="ml: PushFlow (RIFE) through pmml on ${vk:-the GPU}"
            elif [ -n "$vk" ]; then mpv="portable: the GPU engine on $vk (motion-compensated frame generation, NIS upscaling, scene-cut guard)"
            else mpv="portable: the CPU engine (about 2x at 1080p; no Vulkan device found)"; fi
        fi
    fi
    if [ "$os" = Darwin ]; then
        if have pushmpc-capture; then capture="available: pushmpc-capture for QuickTime Player, Infuse, browsers and other apps (pushmpc-capture --player <app>; macOS asks for the Screen Recording permission once)"
        else capture="comes with Homebrew's pushmpc (brew install pushmpc), for QuickTime Player, Infuse, browsers and other apps"; fi
    elif [ -x "$here/capture/pushmpc-capture" ] || [ -x "$pm/capture/pushmpc-capture" ]; then
        capture="available for players with no other route (menu entries \"<Player> with PushMPC\"; X11, or your desktop's screen-capture portal on Wayland)"
    else
        capture="not in this download (capture/pushmpc-capture is missing)"
    fi
}
show_plan() {
    say "Graphics card: ${gpu_name:-none found} [${gpu_vendor:-?}:${gpu_device:-?}] (family $fam; from ${gpu_src:-nothing})"
    say "Vulkan device: ${vk:-none (or vulkaninfo not installed)}"
    say "Engine for mpv and mpv-based players: $mpv"
    say "Engine for VLC: the VLC module on the same engine (GPU engine, else CPU)"
    say "NVIDIA Video Effects: $nv"
    say "Capture mode: $capture"
}
write_plan() {
    [ "$dry" = 1 ] && return 0
    mkdir -p "$pm"
    { printf 'gpu=%s\ngpuVendor=%s\ngpuDevice=%s\nfamily=%s\nvulkan=%s\nmpv=%s\nnvidiaVfx=%s\npmml=%s\ncapture=%s\nos=%s\n' \
        "${gpu_name:-none}" "$gpu_vendor" "$gpu_device" "$fam" "$vk" "$mpv" "$nv" "$pmml" "$capture" "$os"; } > "$plan"
}

# ------------------------------------------------------------------------------------------------ players
list_players() {
    found=''
    add() { found="$found${found:+, }$1"; }
    have mpv && add mpv
    have flatpak && flatpak info io.mpv.Mpv >/dev/null 2>&1 && add 'mpv (Flathub)'
    for p in celluloid haruna smplayer mpc-qt kodi vlc stremio; do have "$p" && add "$p"; done
    [ -d /Applications/IINA.app ] && add IINA
    [ -d /Applications/VLC.app ] && add 'VLC (macOS)'
    if have flatpak; then
        for a in org.videolan.VLC tv.kodi.Kodi com.stremio.Stremio tv.plex.PlexDesktop tv.plex.PlexHTPC org.jellyfin.JellyfinDesktop; do flatpak info "$a" >/dev/null 2>&1 && add "$a (Flathub)"; done
    fi
    pt=$(plex_tarball_dir) && add "Plex ($pt)"
    have snap && snap list vlc >/dev/null 2>&1 && add 'vlc (Snap)'
    if have snap; then for s in plex-desktop plex-htpc emby-theater; do snap list "$s" >/dev/null 2>&1 && add "$s (Snap)"; done; fi
    if [ -d /opt/emby-theater ] || have emby-theater; then add 'Emby Theater'; fi
    say "Players found: ${found:-none}"
    # players with no PushMPC route here
    none=''
    for p in totem clapper dragon; do have "$p" && none="$none${none:+, }$p"; done
    [ -n "$none" ] && say "No plug-in route for: $none (capture mode covers them; or play through mpv)"
    return 0
}

# ------------------------------------------------------------------------------------------------ the per-player installers
vlc_plugin() { for f in "$here/vlc/libpushmpc_plugin.so" "$here/vlc/libpushmpc_plugin.dylib"; do [ -f "$f" ] && { echo "$f"; return; }; done; echo ''; }
vlc_found() { have vlc || [ -d /Applications/VLC.app ] || { have flatpak && flatpak info org.videolan.VLC >/dev/null 2>&1; } || { have snap && snap list vlc >/dev/null 2>&1; }; }
do_vlc() {
    vlc_found || return 0
    p=$(vlc_plugin)
    if [ -z "$p" ] || [ ! -f "$here/vlc/pushmpc-vlc.sh" ]; then say "VLC: this download has no VLC module (vlc/): not changed"; return 0; fi
    say "VLC:"
    if [ "$dry" = 1 ]; then say "  would: sh pushmpc-vlc.sh install --plugin $p"; return 0; fi
    mkdir -p "$pm"; cp "$here/vlc/pushmpc-vlc.sh" "$pm/pushmpc-vlc.sh"
    # the installer's hardware-decoding self-test (on by default) plays pushmpc-selftest.mkv from its own folder;
    # without the clip it skips the test and VLC keeps software decoding
    if [ -f "$here/vlc/pushmpc-selftest.mkv" ]; then cp "$here/vlc/pushmpc-selftest.mkv" "$pm/pushmpc-selftest.mkv"
    else say "  (no pushmpc-selftest.mkv in this download: the hardware-decoding self-test is skipped)"; fi
    out=$(sh "$pm/pushmpc-vlc.sh" install --plugin "$p" 2>&1); rc=$?
    printf '%s\n' "$out" | sed 's/^/  /'
    if [ "$rc" = 0 ]; then record vlc; else say "  VLC's installer failed (exit $rc): VLC not changed"; fi
}
# The distro mpv: PushMPC's native hook (mpvnative/: no VapourSynth needed) when it activates in this mpv, else the
# VapourSynth route (install.sh); never both in one mpv. The hook carries the same engines as the VapourSynth route
# (NVIDIA's engine and PushFlow as sidecar modules, the Vulkan GPU engine, the CPU engine; Auto picks at play time);
# when the engine this GPU should get is not in this download's hook, the VapourSynth route is taken instead.
# Vulkan decoding (hwdec=vulkan: frames straight into the Vulkan engine, no copy) when that engine is the one that
# runs: a Vulkan GPU without NVIDIA's engine and without PushFlow (PushFlow is measured against it on copied frames);
# NVDEC (hwdec=nvdec: CUDA frames into NVIDIA's engine, device to device) when NVIDIA's engine runs.
# The hook's own installer decides "installed" (active) or "installed-inactive" (exit 3: no build for this mpv's
# FFmpeg), which is undone again.
hookon=0
native_engine() {   # the engine Auto will run in the hook: nvidia, ml, vulkan, cpu
    case $fam in 'RTX 50'|'RTX 40'|'RTX 30'|'RTX 20') [ "$nvprop" = 1 ] && [ "$(uname -m)" = x86_64 ] && { echo nvidia; return; } ;; esac
    if [ -f "$pm/pmml/graphs/synth-yuv.onnx" ] || { [ "$nopmml" = 0 ] && [ -n "$(pmml_eps)" ] && have python3 && [ -f "$here/pmml/fetch_runtime.py" ]; }; then echo ml; return; fi
    if [ -n "$vk" ]; then echo vulkan; else echo cpu; fi
}
do_native() {
    hookon=0
    [ "$os" = Darwin ] && return 0
    ni=$here/mpvnative/pushmpc-native.sh
    [ -f "$ni" ] && have mpv || return 0
    eng=$(native_engine); hw=auto-safe; need=''
    case $eng in
        nvidia) need=libpushmpc_engine_nvl.so; hw=nvdec ;;   # NVDEC's CUDA frames into NVIDIA's engine, device to device
        ml) need=libpushmpc_engine_ml.so ;;
        vulkan) hw=vulkan ;;
    esac
    if [ -n "$need" ] && [ ! -f "$here/mpvnative/$need" ]; then
        say "mpv (native hook): this download's hook has no $need (the $eng engine): the VapourSynth route instead"; return 0
    fi
    if [ "$dry" = 1 ]; then say "mpv (native hook): would try sh mpvnative/pushmpc-native.sh install --target native --hwdec $hw (engine: $eng)"; return 0; fi
    say "mpv (native hook):"
    mkdir -p "$pm/mpvnative-installer"; cp "$ni" "$pm/mpvnative-installer/pushmpc-native.sh"
    out=$(sh "$ni" install --from "$here/mpvnative" --target native --hwdec "$hw" --json 2>"$pm/mpvnative-installer/install.log"); rc=$?
    sed 's/^/  /' "$pm/mpvnative-installer/install.log" | tail -n 5
    state=$(printf '%s\n' "$out" | sed -n 's/.*"target":"native".*"state":"\([^"]*\)".*/\1/p' | head -n 1)
    detail=$(printf '%s\n' "$out" | sed -n 's/.*"target":"native".*"detail":"\([^"]*\)".*/\1/p' | head -n 1)
    if [ "$rc" = 0 ] && [ "$state" = installed ]; then
        hookon=1; record native
        say "  the native hook is active in this mpv${detail:+ ($detail)}, engine: $eng${hw:+, hwdec $hw}: the VapourSynth route is not added to it"
    else
        say "  the native hook does not run in this mpv (${state:-exit $rc}${detail:+: $detail}): the VapourSynth route instead"
        sh "$ni" uninstall --target native >/dev/null 2>&1
    fi
}
# PushFlow's models: from PushFPS's release zip (the same files and pins as the Windows Setup takes)
PF_ZIP_URL=https://github.com/DosukaSOL/PushFPS/releases/download/v4.5.5/PushFPS-4.5.5-win64.zip
PF_ZIP_SHA=ee58e9cee35b09e1b2a728011737604df20d6549226f96c8db74cd8ed250468c
# the runtime candidates for this GPU, "<ONNX Runtime flavors> <EPs> <ncnn: 1/0>" ('-' = none), or nothing; Auto
# (pmml_cli auto, every candidate measured in its own process) then picks the one that does the most here
pmml_eps() {
    case $fam in
        'RTX 50'|'RTX 40'|'RTX 30'|'RTX 20') if [ "$nvprop" = 1 ]; then echo "core trt-rtx 0"; else echo "core webgpu 1"; fi ;;
        Intel) echo "core openvino,webgpu 1" ;;
        'RX 9000'|'RX 7000'|'RX 6000'|'RX 5000'|Radeon) if [ -d /opt/rocm ]; then echo "core,migraphx webgpu 1"; else echo "core webgpu 1"; fi ;;
        # any other GPU with Vulkan (GeForce GTX, nouveau / NVK, others): ncnn and ONNX Runtime's WebGPU EP (Dawn on Vulkan)
        none) ;;
        *) echo "core webgpu 1" ;;
    esac
}
# the anime upscalers (ArtCNN MIT, CuNNy LGPL-3.0, AnimeSR Apache-2.0): their files from the authors' own release places,
# each checked against its pinned size and SHA-256 (pmml/anime-models-pin.json; a mismatch is refused), and the model
# graphs from this download, into .../pushmpc/pmml/graphs/anime-sr; the mpv route's anime shaders come from there too
do_anime() {
    [ "$os" = Darwin ] && return 0
    [ -f "$here/pmml/anime-models-pin.json" ] || return 0
    py=$(command -v python3 || true)
    if [ -z "$py" ]; then say "anime upscalers: need python3 for the checked fetch (not found): not installed"; return 0; fi
    dest=$pm/pmml/graphs/anime-sr
    if [ "$dry" = 1 ]; then say "  would: fetch the pinned anime upscaler files into $dest"; return 0; fi
    mkdir -p "$dest"
    [ -d "$here/pmml/graphs/anime-sr" ] && cp "$here/pmml/graphs/anime-sr/"*.onnx "$dest/" 2>/dev/null
    for g in "$here/pmml/graphs/"sr_*.onnx; do [ -f "$g" ] && cp "$g" "$pm/pmml/graphs/"; done
    "$py" -I - "$here/pmml/anime-models-pin.json" "$dest" <<'PY' | sed 's/^/  /'
import hashlib, json, os, sys, urllib.request
pin, dest = sys.argv[1], sys.argv[2]
bad = 0
for f in json.load(open(pin, encoding="utf-8"))["files"]:
    out = os.path.join(dest, f["name"])
    ok = lambda b: len(b) == f["size"] and hashlib.sha256(b).hexdigest() == f["sha256"]
    if os.path.exists(out) and ok(open(out, "rb").read()):
        continue
    try:
        data = urllib.request.urlopen(urllib.request.Request(f["url"], headers={"User-Agent": "PushMPC-Setup"}), timeout=120).read()
    except Exception as e:
        print(f"{f['name']}: download failed ({e})"); bad += 1; continue
    if not ok(data):
        print(f"{f['name']}: refused (size / SHA-256 differ from the pin)"); bad += 1; continue
    open(out, "wb").write(data)
    print(f"ok {f['name']} ({f['licence']})")
sys.exit(1 if bad else 0)
PY
    say "anime upscalers: files in $dest"
}
# ContentDetect's image model (CLIP ViT-B/32 image encoder, MIT, pinned): reliable animation detection; without it the
# hand-built cues decide alone
do_content_model() {
    [ "$os" = Darwin ] && return 0
    [ -f "$here/pmml/content-model-pin.json" ] || return 0
    py=$(command -v python3 || true)
    if [ -z "$py" ]; then say "content model: need python3 for the checked fetch (not found): not installed"; return 0; fi
    dest=$pm/pmml/content-model
    if [ "$dry" = 1 ]; then say "  would: fetch the pinned content model (CLIP image encoder, MIT) into $dest"; return 0; fi
    mkdir -p "$dest"
    fetch_content_model "$py" "$dest"
    say "content model (animation detection): files in $dest"
}
# the content model's files by content-model-pin.json into $2 (python3 $1), each checked by size and SHA-256; a file
# already there with the right hash is kept
fetch_content_model() {
    "$1" -I - "$here/pmml/content-model-pin.json" "$2" <<'PY' | sed 's/^/  /'
import hashlib, json, os, sys, urllib.request
pin, dest = sys.argv[1], sys.argv[2]
bad = 0
for f in json.load(open(pin, encoding="utf-8"))["files"]:
    out = os.path.join(dest, f["name"])
    ok = lambda b: len(b) == f["size"] and hashlib.sha256(b).hexdigest() == f["sha256"]
    if os.path.exists(out) and ok(open(out, "rb").read()):
        continue
    try:
        data = urllib.request.urlopen(urllib.request.Request(f["url"], headers={"User-Agent": "PushMPC-Setup"}), timeout=120).read()
    except Exception as e:
        print(f"{f['name']}: download failed ({e})"); bad += 1; continue
    if not ok(data):
        print(f"{f['name']}: refused (size / SHA-256 differ from the pin)"); bad += 1; continue
    open(out, "wb").write(data)
    print(f"ok {f['name']} ({f['licence']})")
sys.exit(1 if bad else 0)
PY
}
# macOS: the content model for IINA and the other in-process routes (pushmpc/content_model.h: CoreML, then the CPU),
# with the ONNX Runtime pmml loads it through (ml-runtime-pin.json: osx-arm64 on Apple silicon, osx-x64 on Intel), into
# ~/Library/Application Support/PushMPC/pmml: ort/core/libonnxruntime.dylib and content-model/. Without them the
# hand-built cues decide alone. The download carries fetch_runtime.py and both pins in pmml/.
macrt="$HOME/Library/Application Support/PushMPC/pmml"
mac_python() {   # a python3 that runs without asking: /usr/bin/python3 is only a stub until the Command Line Tools are in
    for p in /opt/homebrew/bin/python3 /usr/local/bin/python3 python3; do
        c=$(command -v "$p" 2>/dev/null) || continue
        if [ "$c" = /usr/bin/python3 ] && ! xcode-select -p >/dev/null 2>&1; then continue; fi
        echo "$c"; return 0
    done
    return 1
}
do_mac_ml() {
    [ "$os" = Darwin ] || return 0
    for need in pmml/fetch_runtime.py pmml/ml-runtime-pin.json pmml/content-model-pin.json; do
        [ -f "$here/$need" ] || { say "image model (animation detection): this download has no $need: not installed (the hand-built cues decide)"; return 0; }
    done
    # the Mac's own architecture, also when this shell runs under Rosetta (IINA runs natively)
    march=${PUSHMPC_TEST_MAC_ARCH:-$(uname -m)}
    [ -z "${PUSHMPC_TEST_MAC_ARCH:-}" ] && [ "$(sysctl -n sysctl.proc_translated 2>/dev/null)" = 1 ] && march=arm64
    case $march in
        arm64) plat=osx-arm64 ;;
        x86_64) plat=osx-x64 ;;
        *) say "image model (animation detection): no ONNX Runtime for $march: not installed"; return 0 ;;
    esac
    say "image model (animation detection) for IINA: ONNX Runtime ($plat) and the content model (downloads of about 300 MB, checked by their pinned hashes)"
    if [ "$dry" = 1 ]; then say "  would: python3 fetch_runtime.py --platform $plat --flavors core --out $macrt, and the content model into $macrt/content-model"; return 0; fi
    py=$(mac_python) || { say "  needs python3 for the checked fetch (Homebrew's, or Apple's after xcode-select --install): not installed"; return 0; }
    mkdir -p "$macrt/content-model" || return 0
    fetch_content_model "$py" "$macrt/content-model"
    if [ ! -f "$macrt/content-model/vision_model_fp16.onnx" ]; then say "  the content model was not installed: the hand-built cues decide"; return 0; fi
    record mac-ml
    # ONNX Runtime: kept when the installed one is the pinned version for this Mac
    have_rt=$("$py" -I -c 'import json, sys; f = json.load(open(sys.argv[1])); print(f["platform"], f["ort"]["core"]["version"])' "$macrt/fetched.json" 2>/dev/null)
    want_rt=$("$py" -I -c 'import json, sys; c = json.load(open(sys.argv[1]))["onnxruntime"]["core"]; s = c["platforms"].get(sys.argv[2]); print(sys.argv[2], s.get("version", c["version"]) if isinstance(s, dict) else c["version"])' "$here/pmml/ml-runtime-pin.json" "$plat" 2>/dev/null)
    if [ -n "$have_rt" ] && [ "$have_rt" = "$want_rt" ] && [ -f "$macrt/ort/core/libonnxruntime.dylib" ]; then
        say "  ONNX Runtime $want_rt is already installed; files in $macrt"; return 0
    fi
    dl="$macrt.download"; rm -rf "$macrt.part"
    out=$("$py" -I "$here/pmml/fetch_runtime.py" --pin "$here/pmml/ml-runtime-pin.json" --platform "$plat" --out "$macrt.part" --cache "$dl" --flavors core 2>&1); rc=$?
    printf '%s\n' "$out" | grep -E '^(ok|skip|::error)' | sed 's/^/  /'
    if [ "$rc" != 0 ] || [ ! -f "$macrt.part/ort/core/libonnxruntime.dylib" ]; then
        say "  ONNX Runtime was not installed: the hand-built cues decide"; rm -rf "$macrt.part" "$dl"; return 0
    fi
    rm -rf "$macrt/ort"; mv "$macrt.part/ort" "$macrt/ort"; mv "$macrt.part/fetched.json" "$macrt/fetched.json"
    rm -rf "$macrt.part" "$dl"
    say "  installed in $macrt (IINA's bridge finds it there)"
}
do_pmml() {
    [ "$os" = Darwin ] && return 0
    sel=$(pmml_eps)
    if [ -z "$sel" ]; then say "PushFlow runtime: none for this GPU ($fam): the GPU engine interpolates"; return 0; fi
    if [ "$nopmml" = 1 ]; then say "PushFlow runtime: declined (--no-pmml)"; return 0; fi
    py=$(command -v python3 || true)
    if [ -z "$py" ]; then say "PushFlow runtime: needs python3 for its fetch (not found): not installed"; return 0; fi
    for need in pmml/fetch_runtime.py pmml/ml-runtime-pin.json pmml/graphs/synth-yuv.onnx; do
        [ -f "$here/$need" ] || { say "PushFlow runtime: this download has no $need: not installed"; return 0; }
    done
    arch=x64; case $(uname -m) in aarch64|arm64) arch=arm64 ;; esac
    # shellcheck disable=SC2086
    set -- $sel
    flv=$1; ep=$2; [ "$ep" = - ] && ep=''; extra=''
    [ "$3" = 1 ] && extra=--ncnn-models
    say "PushFlow runtime for $fam: ONNX Runtime ($flv)${ep:+ + $ep}${extra:+, the RIFE models for ncnn on Vulkan} and PushFlow's models (downloads of about 300 MB, checked by SHA-256)"
    if [ "$dry" = 1 ]; then say "  would: python3 fetch_runtime.py --platform linux-$arch --flavors '$flv'${ep:+ --eps $ep} $extra --out $pm/pmml"; return 0; fi
    mkdir -p "$pm"; dl=$pm/pmml-download; rm -rf "$pm/pmml.part"
    out=$("$py" -I "$here/pmml/fetch_runtime.py" --pin "$here/pmml/ml-runtime-pin.json" --platform "linux-$arch" --out "$pm/pmml.part" --cache "$dl" \
            --flavors "$flv" ${ep:+--eps "$ep"} $extra 2>&1); rc=$?
    printf '%s\n' "$out" | grep -E '^(ok|skip|fetch|cached|::error)' | sed 's/^/  /'
    if [ "$rc" != 0 ]; then
        say "  the fetch failed: PushFlow runtime not installed (the GPU engine interpolates)"; rm -rf "$pm/pmml.part" "$dl"; return 0
    fi
    # PushFlow's models (pushflow-fast / -quality with the grid and mask outputs) out of PushFPS's release zip; the ncnn
    # route needs them too: its network size and padding come from their settings (.onnx.txt, models.json)
    if ! "$py" -I - "$PF_ZIP_URL" "$PF_ZIP_SHA" "$dl" "$pm/pmml.part/models" "$here/pmml/ml-runtime-pin.json" <<'PY'
import hashlib, json, sys, urllib.request, zipfile, pathlib
url, want, cache, dest, pin = sys.argv[1:6]
cache = pathlib.Path(cache); cache.mkdir(parents=True, exist_ok=True); dest = pathlib.Path(dest); dest.mkdir(parents=True, exist_ok=True)
z = cache / url.rsplit("/", 1)[1]
def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""): h.update(b)
    return h.hexdigest()
if not (z.exists() and sha(z) == want):
    req = urllib.request.Request(url, headers={"User-Agent": "pushmpc-setup/1"})
    with urllib.request.urlopen(req, timeout=600) as r, open(z, "wb") as f:
        while True:
            b = r.read(1 << 20)
            if not b: break
            f.write(b)
if sha(z) != want: sys.exit("PushFPS's release zip does not match its pinned SHA-256")
models = json.load(open(pin))["models"]["pushflow"]
need = {models[r]["file"]: models[r]["sha256"] for r in ("fast", "quality")}
with zipfile.ZipFile(z) as a:
    for n in a.namelist():
        base = n.rsplit("/", 1)[-1]
        if "/runtime/experimental/pushflow/" not in n.replace("\\", "/"): continue
        if base in need or base in ("models.json", "RIFE_LICENSE.txt") or base in [f + ".txt" for f in need]:
            out = dest / base
            out.write_bytes(a.read(n))
for f, s in need.items():
    if not (dest / f).exists() or sha(dest / f) != s: sys.exit(f"{f}: missing or not the pinned model")
print("ok      models: " + ", ".join(sorted(need)))
PY
    then say "  PushFlow's models could not be taken from PushFPS's release: PushFlow runtime not installed"; rm -rf "$pm/pmml.part" "$dl"; return 0; fi
    cp -R "$here/pmml/graphs" "$pm/pmml.part/graphs"
    [ -f "$here/pmml/pmml_cli" ] && install -m 755 "$here/pmml/pmml_cli" "$pm/pmml.part/pmml_cli"
    # keep the anime upscalers' files (do_anime) across a runtime refresh
    [ -d "$pm/pmml/graphs/anime-sr" ] && { mkdir -p "$pm/pmml.part/graphs/anime-sr"; cp -R "$pm/pmml/graphs/anime-sr/." "$pm/pmml.part/graphs/anime-sr/"; }
    # and the content model (do_content_model, which runs before this step)
    [ -d "$pm/pmml/content-model" ] && cp -R "$pm/pmml/content-model" "$pm/pmml.part/content-model"
    rm -rf "$pm/pmml" "$dl"; mv "$pm/pmml.part" "$pm/pmml"
    record pmml
    say "  PushFlow runtime installed in $pm/pmml (the mpv route's ml backend finds it there)"
    # Auto's measurement of every candidate at 1080p, in the background and at the lowest priority (other sizes are
    # measured the first time they play); PUSHMPC_NO_AUTOBENCH=1 skips it
    if [ -x "$pm/pmml/pmml_cli" ] && [ "${PUSHMPC_NO_AUTOBENCH:-0}" != 1 ]; then
        nc=''; [ -d "$pm/pmml/ncnn-models" ] && nc="--ncnn-models $pm/pmml/ncnn-models"
        # shellcheck disable=SC2086
        (nohup nice -n 19 "$pm/pmml/pmml_cli" auto --runtime "$pm/pmml" --cache "$pm/pmml" --size 1920x1080 --fps 23.976 --mult 5 $nc > "$pm/pmml/pmml-autobench-1920x1080.log" 2>&1 &)
        : > "$pm/pmml/pmml-autobench-1920x1080.started"
        say "  measuring PushFlow's options on this GPU in the background (a few minutes; log: $pm/pmml/pmml-autobench-1920x1080.log)"
    fi
}
# Plex desktop / Plex HTPC from Flathub, or as plain files (Plex's Snap unpacked by a distro package into /opt, or a
# folder given in PUSHMPC_PLEX_TARBALL_DIR; it needs Plex.sh there): PushMPC's filter inside Plex's own player through
# LD_PRELOAD (plex/, the installer self-tests it through Plex's libmpv). A target that does not pass is undone again.
# Snap Plex itself cannot take LD_PRELOAD: do_apps' C-plugin route, else capture mode's menu entry (do_capture).
plex_tarball_dir() {
    for d in "${PUSHMPC_PLEX_TARBALL_DIR:-}" /opt/plex-htpc /opt/plex-desktop /opt/plexhtpc /opt/plex /usr/lib/plex-htpc /usr/lib/plex-desktop; do
        [ -n "$d" ] && [ -f "$d/Plex.sh" ] && { printf '%s\n' "$d"; return 0; }
    done
    return 1
}
# Plex desktop / Plex HTPC from Flathub: PushMPC's filter inside Plex's own player through LD_PRELOAD (plex/, the
# installer self-tests it through Plex's libmpv in its sandbox). A target that does not pass is undone again. Snap Plex
# cannot take LD_PRELOAD: do_apps puts the filter into it as an mpv C plugin from its own mpv.conf instead (capture mode
# covers what neither route reaches: do_capture).
do_plex() {
    pi=$here/plex/pushmpc-plex-linux.sh
    [ -f "$pi" ] || return 0
    tdir=$(plex_tarball_dir) || tdir=''
    fp=0
    if have flatpak && { flatpak info tv.plex.PlexDesktop >/dev/null 2>&1 || flatpak info tv.plex.PlexHTPC >/dev/null 2>&1; }; then fp=1; fi
    [ "$fp" = 1 ] || [ -n "$tdir" ] || return 0
    say "Plex${tdir:+ ($tdir)}:"
    if [ "$dry" = 1 ]; then say "  would: sh plex/pushmpc-plex-linux.sh install${tdir:+ --tarball-dir $tdir}"; return 0; fi
    # set up before (an earlier download): out first, so this download's files go in (its installer keeps what is there)
    if recorded plex && [ -f "$pm/plex-installer/pushmpc-plex-linux.sh" ]; then plex_uninstall >/dev/null 2>&1; fi
    mkdir -p "$pm/plex-installer"; cp "$pi" "$pm/plex-installer/pushmpc-plex-linux.sh"
    if [ -n "$tdir" ]; then printf '%s\n' "$tdir" > "$pm/plex-installer/tarball-dir"; else rm -f "$pm/plex-installer/tarball-dir"; fi
    out=$(sh "$pi" install --from "$here/plex" ${tdir:+--tarball-dir "$tdir"} --json 2>"$pm/plex-installer/install.log"); rc=$?
    sed 's/^/  /' "$pm/plex-installer/install.log" | tail -n 5
    on=0
    for t in desktop htpc tarball; do
        st=$(printf '%s\n' "$out" | sed -n "s/.*\"target\":\"$t\".*\"state\":\"\([^\"]*\)\".*/\1/p" | head -n 1)
        [ -n "$st" ] || continue
        case $st in
            installed) on=1; say "  Plex ($t): smoother motion inside Plex's own player${tdir:+$( [ "$t" = tarball ] && echo "; start it with plex-pushmpc")}" ;;
            refused|unsupported) say "  Plex ($t): not changed ($st)" ;;
            *) say "  Plex ($t): the self-test did not pass ($st): undone"; sh "$pi" uninstall --target "$t" ${tdir:+--tarball-dir "$tdir"} >/dev/null 2>&1 ;;
        esac
    done
    if [ "$on" = 1 ]; then record plex; else [ "$rc" = 0 ] || say "  Plex is not changed (exit $rc)"; rm -rf "$pm/plex-installer"; fi
}
plex_uninstall() {   # every target, the folder Plex recorded at install
    td=''; [ -f "$pm/plex-installer/tarball-dir" ] && td=$(cat "$pm/plex-installer/tarball-dir")
    sh "$pm/plex-installer/pushmpc-plex-linux.sh" uninstall ${td:+--tarball-dir "$td"}
}
# Players with their own libmpv (Snap Plex / Plex HTPC, Flathub Stremio and Jellyfin Desktop, Emby Theater as .deb /
# .rpm / Snap / Flatpak): PushMPC's filter inside each player's own libmpv (plex/pushmpc-apps-linux.sh: a C plugin from
# the player's mpv.conf in Snaps and for Emby Theater, LD_PRELOAD in Flatpaks). Each one is self-tested through that
# player's libmpv; one that does not pass is left unchanged, with the reason.
do_apps() {
    ai=$here/plex/pushmpc-apps-linux.sh
    [ -f "$ai" ] && [ -f "$here/mpvnative/pushmpc_hook.so" ] && [ -f "$here/plex/pushmpc_preload.so" ] || return 0
    sh "$ai" status --json >/dev/null 2>&1; [ $? = 3 ] && return 0   # none of those players here
    say "Players with their own mpv (Snap Plex, Stremio, Jellyfin Desktop, Emby Theater):"
    if [ "$dry" = 1 ]; then say "  would: sh plex/pushmpc-apps-linux.sh install"; return 0; fi
    if recorded apps && [ -f "$pm/apps-installer/pushmpc-apps-linux.sh" ]; then sh "$pm/apps-installer/pushmpc-apps-linux.sh" uninstall >/dev/null 2>&1; fi
    mkdir -p "$pm/apps-installer"; cp "$ai" "$pm/apps-installer/pushmpc-apps-linux.sh"
    out=$(sh "$ai" install --from "$here/mpvnative" --kit "$here/plex" --json 2>"$pm/apps-installer/install.log")
    on=0
    for t in plex-snap plex-htpc-snap stremio-flatpak jellyfin-desktop-flatpak stremio emby emby-snap emby-flatpak; do
        st=$(printf '%s\n' "$out" | sed -n "s/.*\"target\":\"$t\".*\"state\":\"\([^\"]*\)\".*/\1/p" | head -n 1)
        dt=$(printf '%s\n' "$out" | sed -n "s/.*\"target\":\"$t\".*\"detail\":\"\([^\"]*\)\".*/\1/p" | head -n 1)
        [ -n "$st" ] || continue
        case $st in
            installed) on=1; say "  $t: smoother motion inside its own player" ;;
            *) say "  $t: not changed ($st: $(printf '%s' "$dt" | cut -c1-160))" ;;
        esac
    done
    if [ "$on" = 1 ]; then record apps; else rm -rf "$pm/apps-installer"; fi
}
do_kodi_addon() {
    [ -f "$here/kodi/install.sh" ] || return 0
    have kodi || [ -d "$HOME/.kodi" ] || { have flatpak && flatpak info tv.kodi.Kodi >/dev/null 2>&1; } || return 0
    say "Kodi add-on:"
    if [ "$dry" = 1 ]; then say "  would: sh kodi/install.sh install"; return 0; fi
    mkdir -p "$pm/kodi-installer"; cp -R "$here/kodi/." "$pm/kodi-installer/"
    out=$(PUSHMPC_KODI_DIR="$pm/kodi-installer" sh "$pm/kodi-installer/install.sh" install 2>&1); rc=$?
    printf '%s\n' "$out" | sed 's/^/  /'
    if [ "$rc" = 0 ]; then record kodi-addon; else say "  the Kodi add-on's installer did not install it (exit $rc): Kodi keeps mpv as its external player"; fi
}
# capture mode (capture/: pushmpc-capture and its launcher) for the players with no other route here: a menu entry
# "<Player> with PushMPC" each; browsers only on request (the launcher's enable). Undone by uninstall.
capture_players() {   # the launcher's keys of the players found here that PushMPC cannot plug into
    out=''
    if have flatpak; then
        flatpak info io.github.celluloid_player.Celluloid >/dev/null 2>&1 && out="$out celluloid"
        flatpak info com.stremio.Stremio >/dev/null 2>&1 && out="$out stremio"
    fi
    have stremio && case $out in *stremio*) ;; *) out="$out stremio" ;; esac
    for p in totem clapper dragon; do
        if have "$p" || { have flatpak && flatpak list --app --columns=application 2>/dev/null | grep -qi "$p"; }; then out="$out $p"; fi
    done
    # Plex: the Flathub one has its own route (do_plex); the Snap one gets do_apps' C-plugin route when its self-test
    # passes, and keeps this menu entry as the fallback
    if have snap && snap list plex-desktop >/dev/null 2>&1 && ! recorded plex; then out="$out plex"; fi
    printf '%s\n' "$out"
}
do_capture() {
    [ "$os" = Darwin ] && return 0
    [ -x "$here/capture/pushmpc-capture" ] && [ -f "$here/capture/pushmpc-capture.sh" ] || return 0
    list=$(capture_players)
    say "Capture mode:"
    if [ "$dry" = 1 ]; then say "  would: install capture/ into $pm/capture${list:+ and add menu entries for:$list}"; return 0; fi
    mkdir -p "$pm/capture"
    cp "$here/capture/pushmpc-capture" "$pm/capture/pushmpc-capture" && chmod 755 "$pm/capture/pushmpc-capture"
    cp "$here/capture/pushmpc-capture.sh" "$pm/capture/pushmpc-capture.sh"
    record capture
    if ! "$pm/capture/pushmpc-capture" --help >/dev/null 2>&1; then
        say "  pushmpc-capture does not start on this system (missing libraries?): no menu entries added"; return 0
    fi
    for p in $list; do sh "$pm/capture/pushmpc-capture.sh" enable "$p" | sed 's/^/  /'; done
    [ -n "$list" ] || say "  installed (no player without a route found; for a browser: sh $pm/capture/pushmpc-capture.sh enable firefox)"
}
do_mac() {
    if have pushmpc-setup; then say "mpv / IINA (Homebrew's pushmpc-setup):"; [ "$dry" = 1 ] && say "  would: pushmpc-setup install" || { pushmpc-setup install | sed 's/^/  /'; record brew-setup; }
    else say "mpv / IINA: install PushMPC with Homebrew first (brew install pushmpc), then run this again"; fi
}

# ------------------------------------------------------------------------------------------------ actions
case $action in
    plan) make_plan; show_plan; list_players ;;
    pmml) make_plan; do_pmml; do_mac_ml; [ "$dry" = 1 ] || { mkdir -p "$pm"; [ "$here/setup.sh" = "$pm/setup.sh" ] || cp "$here/setup.sh" "$pm/setup.sh"; } ;;
    status)
        make_plan; show_plan; list_players
        [ -f "$sstate" ] && { say "Setup's changes ($sstate):"; sed 's/^/  /' "$sstate"; }
        [ -f "$pm/install.sh" ] && sh "$pm/install.sh" status | sed 's/^/  /'
        ;;
    install)
        say "PushMPC Setup ($os)"
        make_plan; show_plan; list_players; say ''
        if [ "$os" = Darwin ]; then do_mac; do_mac_ml
        else
            [ -f "$here/install.sh" ] || { echo "setup.sh: install.sh is missing (run setup.sh from the unpacked PushMPC folder)" >&2; exit 1; }
            do_native
            # the Kodi add-on first: install.sh leaves a Kodi that has it alone
            do_kodi_addon
            do_plex
            do_anime
            do_content_model
            # shellcheck disable=SC2086
            PUSHMPC_NATIVE_HOOK=$hookon sh "$here/install.sh" install $pass; rc=$?
            [ "$rc" = 0 ] || { echo "setup.sh: install.sh failed ($rc)" >&2; exit "$rc"; }
            # after install.sh: Emby Theater shares the native hook block in ~/.config/mpv/mpv.conf when there is one
            do_apps
        fi
        do_vlc
        [ "$os" = Darwin ] && do_kodi_addon
        do_pmml
        do_capture
        # the plan again: NVIDIA's files may have arrived in the meantime
        make_plan; write_plan
        if [ "$dry" = 0 ]; then mkdir -p "$pm"; [ "$here/setup.sh" = "$pm/setup.sh" ] || cp "$here/setup.sh" "$pm/setup.sh"; fi
        say ''
        say "Auto starts with: $mpv"
        say "Plan saved in $plan. Remove everything with: sh $pm/setup.sh uninstall"
        ;;
    uninstall)
        if recorded native && [ -f "$pm/mpvnative-installer/pushmpc-native.sh" ]; then say "mpv (native hook):"; sh "$pm/mpvnative-installer/pushmpc-native.sh" uninstall --target native 2>&1 | sed 's/^/  /'; fi
        [ "$dry" = 1 ] || rm -rf "$pm/mpvnative-installer"
        if recorded vlc && [ -f "$pm/pushmpc-vlc.sh" ]; then say "VLC:"; sh "$pm/pushmpc-vlc.sh" uninstall | sed 's/^/  /'; fi
        if recorded plex && [ -f "$pm/plex-installer/pushmpc-plex-linux.sh" ]; then say "Plex:"; plex_uninstall 2>&1 | sed 's/^/  /'; fi
        if recorded plex && [ -f "$pm/plex-installer/pushmpc-plex-linux.sh" ]; then say "Plex (Flathub):"; sh "$pm/plex-installer/pushmpc-plex-linux.sh" uninstall 2>&1 | sed 's/^/  /'; fi
        if recorded apps && [ -f "$pm/apps-installer/pushmpc-apps-linux.sh" ]; then say "Players with their own mpv:"; sh "$pm/apps-installer/pushmpc-apps-linux.sh" uninstall 2>&1 | sed 's/^/  /'; fi
        if recorded kodi-addon && [ -f "$pm/kodi-installer/install.sh" ]; then say "Kodi add-on:"; PUSHMPC_KODI_DIR="$pm/kodi-installer" sh "$pm/kodi-installer/install.sh" uninstall 2>&1 | sed 's/^/  /'; fi
        if recorded brew-setup && have pushmpc-setup; then pushmpc-setup uninstall | sed 's/^/  /'; fi
        if [ "$dry" = 0 ] && recorded pmml; then rm -rf "$pm/pmml"; say "PushFlow runtime removed"; fi
        if [ "$dry" = 0 ] && recorded mac-ml; then
            rm -rf "$macrt" "$macrt.part" "$macrt.download"; rmdir "$HOME/Library/Application Support/PushMPC" 2>/dev/null
            say "Content model and its ONNX Runtime removed"
        fi
        if [ "$dry" = 0 ] && recorded capture; then
            [ -f "$pm/capture/pushmpc-capture.sh" ] && sh "$pm/capture/pushmpc-capture.sh" stop >/dev/null 2>&1
            [ -f "$pm/capture/pushmpc-capture.sh" ] && sh "$pm/capture/pushmpc-capture.sh" disable --all >/dev/null 2>&1
            rm -rf "$pm/capture"; say "Capture mode removed"
        fi
        if [ "$dry" = 0 ]; then rm -rf "$pm/kodi-installer" "$pm/plex-installer" "$pm/apps-installer"; rm -f "$pm/pushmpc-vlc.sh" "$pm/pushmpc-selftest.mkv" "$sstate" "$plan" "$pm/setup.sh"; fi
        if [ -f "$pm/install.sh" ]; then
            # shellcheck disable=SC2086
            sh "$pm/install.sh" uninstall $pass
        elif [ -f "$here/install.sh" ] && [ "$os" != Darwin ]; then
            # shellcheck disable=SC2086
            sh "$here/install.sh" uninstall $pass
        fi
        [ "$dry" = 1 ] || rmdir "$pm" 2>/dev/null
        say "PushMPC Setup's changes are undone."
        ;;
esac
exit 0
