#!/bin/sh
# PushMPC Setup for Linux and macOS: the one command that sets PushMPC up for every player it finds, for your user
# only (no root, nothing outside your home folder). It finds your graphics card and your players, logs the engine
# Auto starts with (the same rules as the plugin's Auto: vsplugin backend.h), and runs the per-player installers:
#   install.sh        mpv, Flathub mpv, Celluloid, Haruna, SMPlayer, MPC-QT, the Jellyfin / Plex MPV shims,
#                     Jellyfin Desktop, Kodi (mpv as its external player), NVIDIA's Video Effects (Linux, licence step)
#   pushmpc-vlc.sh    VLC 3 (native, Flathub, Snap), when this download has the VLC module (vlc/)
#   kodi/install.sh   the Kodi add-on, when this download has it (kodi/)
#   macOS             Homebrew's pushmpc-setup (mpv, IINA) and the VLC module
#
#   sh setup.sh [install]  [--yes] [--dry-run] [--flatpak ask|yes|no] [--nvidia-vfx ask|yes|no]
#   sh setup.sh uninstall  [--keep-flatpak-mpv]
#   sh setup.sh plan       the engine Auto starts with, per player route (no change)
#   sh setup.sh status
# Nothing is asked unless a download needs your yes: the Flathub mpv (Ubuntu / Debian mpv cannot run VapourSynth) and
# NVIDIA's Video Effects (NVIDIA's licence). Both are offered once in a terminal; --yes accepts both ahead, --flatpak /
# --nvidia-vfx answer each one. Without a terminal neither is downloaded.
# What it changes is recorded in ${XDG_DATA_HOME:-~/.local/share}/pushmpc (setup-state, auto-plan.txt) and undone by
# uninstall. Tests: PUSHMPC_TEST_GPU="<vendor hex>:<device hex>:<name>" stands in for the graphics card, PUSHMPC_TEST_VULKAN
# for the Vulkan device name (empty: none), PUSHMPC_TEST_NVIDIA_DRIVER=1 for the proprietary driver;
# PUSHMPC_TEST_UNAME=Darwin|Linux for the OS.
set -u

action=install
case ${1:-} in install|uninstall|status|plan) action=$1; shift ;; esac
pass=''; yes=0; dry=0
while [ $# -gt 0 ]; do
    case $1 in
        --flatpak|--nvidia-vfx) pass="$pass $1 $2"; shift 2 ;;
        --yes|-y) yes=1; pass="$pass --yes"; shift ;;
        --dry-run) dry=1; pass="$pass --dry-run"; shift ;;
        --keep-flatpak-mpv) pass="$pass $1"; shift ;;
        -h|--help) sed -n '2,24p' "$0"; exit 0 ;;
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
    osname=Linux; [ "$os" = Darwin ] && osname=macOS
    capture="not available on $osname: PushFPS's $osname build reads capture cards only, it has no window capture (capture mode is Windows only)"
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
        for a in org.videolan.VLC tv.kodi.Kodi com.stremio.Stremio tv.plex.PlexDesktop org.jellyfin.JellyfinDesktop; do flatpak info "$a" >/dev/null 2>&1 && add "$a (Flathub)"; done
    fi
    have snap && snap list vlc >/dev/null 2>&1 && add 'vlc (Snap)'
    say "Players found: ${found:-none}"
    # players with no PushMPC route here (Stremio and Plex desktop ship their own mpv without a filter slot on Linux)
    none=''
    for p in stremio totem clapper dragon; do have "$p" && none="$none${none:+, }$p"; done
    if have flatpak; then for a in com.stremio.Stremio tv.plex.PlexDesktop; do flatpak info "$a" >/dev/null 2>&1 && none="$none${none:+, }$a"; done; fi
    [ -n "$none" ] && say "No PushMPC route yet for: $none (play through mpv instead; capture mode is Windows only)"
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
do_kodi_addon() {
    [ -f "$here/kodi/install.sh" ] || return 0
    have kodi || [ -d "$HOME/.kodi" ] || { have flatpak && flatpak info tv.kodi.Kodi >/dev/null 2>&1; } || return 0
    say "Kodi add-on:"
    if [ "$dry" = 1 ]; then say "  would: sh kodi/install.sh install"; return 0; fi
    mkdir -p "$pm/kodi-installer"; cp -R "$here/kodi/." "$pm/kodi-installer/"
    if sh "$pm/kodi-installer/install.sh" install | sed 's/^/  /'; then record kodi-addon; else say "  the Kodi add-on's installer failed: Kodi keeps mpv as its external player"; fi
}
do_mac() {
    if have pushmpc-setup; then say "mpv / IINA (Homebrew's pushmpc-setup):"; [ "$dry" = 1 ] && say "  would: pushmpc-setup install" || { pushmpc-setup install | sed 's/^/  /'; record brew-setup; }
    else say "mpv / IINA: install PushMPC with Homebrew first (brew install pushmpc), then run this again"; fi
}

# ------------------------------------------------------------------------------------------------ actions
case $action in
    plan) make_plan; show_plan; list_players ;;
    status)
        make_plan; show_plan; list_players
        [ -f "$sstate" ] && { say "Setup's changes ($sstate):"; sed 's/^/  /' "$sstate"; }
        [ -f "$pm/install.sh" ] && sh "$pm/install.sh" status | sed 's/^/  /'
        ;;
    install)
        say "PushMPC Setup ($os)"
        make_plan; show_plan; list_players; say ''
        if [ "$os" = Darwin ]; then do_mac
        else
            [ -f "$here/install.sh" ] || { echo "setup.sh: install.sh is missing (run setup.sh from the unpacked PushMPC folder)" >&2; exit 1; }
            # shellcheck disable=SC2086
            sh "$here/install.sh" install $pass; rc=$?
            [ "$rc" = 0 ] || { echo "setup.sh: install.sh failed ($rc)" >&2; exit "$rc"; }
        fi
        do_vlc
        do_kodi_addon
        # the plan again: NVIDIA's files may have arrived in the meantime
        make_plan; write_plan
        if [ "$dry" = 0 ]; then mkdir -p "$pm"; [ "$here/setup.sh" = "$pm/setup.sh" ] || cp "$here/setup.sh" "$pm/setup.sh"; fi
        say ''
        say "Auto starts with: $mpv"
        say "Plan saved in $plan. Remove everything with: sh $pm/setup.sh uninstall"
        ;;
    uninstall)
        if recorded vlc && [ -f "$pm/pushmpc-vlc.sh" ]; then say "VLC:"; sh "$pm/pushmpc-vlc.sh" uninstall | sed 's/^/  /'; fi
        if recorded kodi-addon && [ -f "$pm/kodi-installer/install.sh" ]; then say "Kodi add-on:"; sh "$pm/kodi-installer/install.sh" uninstall | sed 's/^/  /'; fi
        if recorded brew-setup && have pushmpc-setup; then pushmpc-setup uninstall | sed 's/^/  /'; fi
        if [ "$dry" = 0 ]; then rm -rf "$pm/kodi-installer"; rm -f "$pm/pushmpc-vlc.sh" "$pm/pushmpc-selftest.mkv" "$sstate" "$plan" "$pm/setup.sh"; fi
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
