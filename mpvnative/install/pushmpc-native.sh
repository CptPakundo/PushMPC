#!/bin/sh
# pushmpc-native.sh: installs PushMPC's native route into an existing mpv on Linux (the runtime hook: no VapourSynth,
# no rebuilt mpv), for Setup (integrations/linux/setup.sh) or by hand. POSIX sh.
#
#   pushmpc-native.sh install|uninstall|status [--from DIR] [--target auto|native|flatpak] [--opts 'smoother=4:...']
#                     [--hwdec auto-safe|vulkan|nvdec] [--conf FILE] [--dest DIR] [--allow-both] [--dry-run] [--json]
#
#   --from     folder with pushmpc_hook.so, libpushmpc-avfilter-<ver>.so (one per FFmpeg release) and
#              libpushmpc_engine.so (default: the folder of this script, then ../lib); the sidecar engines
#              libpushmpc_engine_nvl.so (NVIDIA) and libpushmpc_engine_ml.so (PushFlow), pushmpc-nvvfx-selftest and
#              pmml_cli are copied too when they are there
#   --hwdec    vulkan: decode on the GPU into Vulkan frames that go to the Vulkan engine with no copy (hwdec=vulkan,
#              then auto-safe; gpu-api=vulkan), when this mpv takes it (checked); nvdec: NVIDIA's decoder into CUDA
#              frames that NVIDIA's engine takes device to device (hwdec=nvdec, then auto-safe; checked); default auto-safe
#   --target   native   the distro's mpv (and libmpv players that read ~/.config/mpv/mpv.conf)
#              flatpak  io.mpv.Mpv (files inside its sandbox folders)
#              auto     every one of them that is installed (default)
#   --opts     vf_pushmpc options for the hook (script-opts pushmpc-vf=...), default none (the engine's Auto)
#   --conf / --dest  override the mpv.conf and the install folder (tests)
#   --allow-both     install even when mpv.conf holds the VapourSynth route's block (mpv-integration.sh)
#   --json     one JSON object per target on stdout (for Setup), the log on stderr
#
# What install does, per target, with an exact undo (uninstall):
#   - copies the files to <dest> (~/.local/share/pushmpc/mpvnative, or ~/.var/app/io.mpv.Mpv/data/pushmpc/mpvnative);
#   - puts a marked block at the top of mpv.conf (before any [profile]):
#       scripts-append=<dest>/pushmpc_hook.so    (loads the hook; it appends @pushmpc:pushmpc to vf)
#       script-opts-append=pushmpc-vf=<opts>     (only with --opts)
#       hwdec=auto-safe (or vulkan,auto-safe + gpu-api=vulkan, or nvdec,auto-safe) / video-sync=display-resample / interpolation=no /
#       hr-seek-framedrop=no
#     the original file is kept as mpv.conf.pushmpc-native-backup (or .pushmpc-native-absent);
#   - checks that the hook activates in that mpv (a 1-frame probe): "active", or why not (no build for its FFmpeg).
# Exit: 0 done, 1 error, 2 usage, 3 not supported here (macOS, no mpv, no build for this mpv's FFmpeg).
set -u

BEGIN_MARK='# >>> PushMPC native: do not edit between these lines (PushMPC uninstall removes them)'
END_MARK='# <<< PushMPC native'
VS_MARK='# >>> PushMPC managed'

action=${1:-}; [ $# -gt 0 ] && shift
from=''; target=auto; opts=''; conf_override=''; dest_override=''; allow_both=0; dry=0; json=0; hwdec=auto-safe
while [ $# -gt 0 ]; do
    case $1 in
        --from) from=$2; shift 2 ;;
        --target) target=$2; shift 2 ;;
        --opts) opts=$2; shift 2 ;;
        --hwdec) hwdec=$2; shift 2 ;;
        --conf) conf_override=$2; shift 2 ;;
        --dest) dest_override=$2; shift 2 ;;
        --allow-both) allow_both=1; shift ;;
        --dry-run) dry=1; shift ;;
        --json) json=1; shift ;;
        *) echo "pushmpc-native.sh: unknown option $1" >&2; exit 2 ;;
    esac
done
case $action in install|uninstall|status) ;; *) sed -n '2,12p' "$0" >&2; exit 2 ;; esac
case $target in auto|native|flatpak) ;; *) echo "pushmpc-native.sh: --target must be auto, native or flatpak" >&2; exit 2 ;; esac
case $hwdec in auto-safe|vulkan|nvdec) ;; *) echo "pushmpc-native.sh: --hwdec must be auto-safe, vulkan or nvdec" >&2; exit 2 ;; esac
# the optional pieces next to the engine (copied and removed with it)
EXTRAS='libpushmpc_engine_nvl.so libpushmpc_engine_ml.so pushmpc-nvvfx-selftest pmml_cli'

log() { printf '%s\n' "$*" >&2; }
run() { if [ "$dry" = 1 ]; then log "would: $*"; else "$@"; fi; }
jesc() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'; }
emit() {   # emit <target> <state> <detail>
    if [ "$json" = 1 ]; then printf '{"target":"%s","action":"%s","state":"%s","detail":"%s"}\n' "$1" "$action" "$2" "$(jesc "$3")"
    else log "$1: $2${3:+ ($3)}"; fi
}

if [ "$(uname -s 2>/dev/null)" = Darwin ]; then
    emit macos unsupported "the hook is Linux-only (ELF); on macOS PushMPC uses the VapourSynth route"
    exit 3
fi
here=$(cd "$(dirname "$0")" && pwd)
if [ -z "$from" ]; then
    if [ -f "$here/pushmpc_hook.so" ]; then from=$here; else from=$here/../lib; fi
fi

# the block for mpv.conf ($1: dest; $2: crlf or lf)
block() {
    if [ "$2" = crlf ]; then nl=$(printf '\r\n_'); nl=${nl%_}; else nl='
'; fi
    printf '%s%s' "$BEGIN_MARK" "$nl"
    printf 'scripts-append=%s/pushmpc_hook.so%s' "$1" "$nl"
    [ -n "$opts" ] && printf 'script-opts-append=pushmpc-vf=%s%s' "$opts" "$nl"
    case $hw in
        vulkan) printf 'hwdec=vulkan,auto-safe%sgpu-api=vulkan%s' "$nl" "$nl" ;;
        nvdec) printf 'hwdec=nvdec,auto-safe%s' "$nl" ;;
        *) printf 'hwdec=auto-safe%s' "$nl" ;;
    esac
    printf 'video-sync=display-resample%sinterpolation=no%shr-seek-framedrop=no%s' "$nl" "$nl" "$nl"
    printf '%s%s' "$END_MARK" "$nl"
}
line_ending() { if [ -f "$1" ] && [ "$(tr -dc '\r' < "$1" | wc -c | tr -d ' ')" != 0 ]; then echo crlf; else echo lf; fi; }
block_lines() {
    b=$(grep -n -F -x -e "$BEGIN_MARK" -e "$BEGIN_MARK$(printf '\r')" "$1" 2>/dev/null | head -n 1 | cut -d: -f1)
    e=''
    if [ -n "$b" ]; then
        e=$(tail -n +"$b" "$1" | grep -n -F -x -e "$END_MARK" -e "$END_MARK$(printf '\r')" | head -n 1 | cut -d: -f1)
        [ -n "$e" ] && e=$((b + e - 1))
    fi
    [ -n "$e" ] || b=''
}
strip_block() {
    block_lines "$1"
    if [ -z "$b" ]; then cat "$1"; return; fi
    [ "$b" -gt 1 ] && head -n $((b - 1)) "$1"
    tail -n +$((e + 1)) "$1"
}

# a one-frame probe: does the hook activate in this mpv? prints the hook's log line. The clip goes where that mpv can
# read it: $probe_base when set (a Flatpak mpv has its own /tmp, but sees its ~/.var/app/<id> at the same path),
# else a temporary folder
probe() {   # probe <mpv command...>
    if [ -n "$probe_base" ]; then t=$probe_base/pushmpc-probe.$$; else t=$(mktemp -d 2>/dev/null || echo "/tmp/pushmpc-probe.$$"); fi
    mkdir -p "$t" || return 0
    { printf 'YUV4MPEG2 W16 H16 F24:1 Ip A1:1 C420jpeg\nFRAME\n'; head -c 384 /dev/zero; } > "$t/p.y4m"
    "$@" --no-config --script="$hook_path" --script-opts=pushmpc-append=no --vo=null --ao=null --frames=1 "$t/p.y4m" 2>&1 \
        | grep -m1 "pushmpc_hook" | sed 's/^\[pushmpc_hook\] //'
    rm -rf "$t"
}

# does this mpv take `hwdec=<list>` (and gpu-api=vulkan for the Vulkan list; a list needs mpv 0.36)? A one-frame run,
# no window ($hwlist: the list to try)
hwdec_ok() {   # hwdec_ok <mpv command...>
    if [ -n "$probe_base" ]; then t=$probe_base/pushmpc-hwprobe.$$; else t=$(mktemp -d 2>/dev/null || echo "/tmp/pushmpc-hwprobe.$$"); fi
    mkdir -p "$t" || return 1
    { printf 'YUV4MPEG2 W16 H16 F24:1 Ip A1:1 C420jpeg\nFRAME\n'; head -c 384 /dev/zero; } > "$t/p.y4m"
    if [ "$hwlist" = vulkan,auto-safe ]; then set -- "$@" --gpu-api=vulkan; fi
    "$@" --no-config --hwdec="$hwlist" --vo=null --ao=null --frames=1 "$t/p.y4m" >/dev/null 2>&1; r=$?
    rm -rf "$t"
    return $r
}

do_target() {   # do_target <name> <conf> <dest> <mpv command...>
    name=$1; conf=$2; dest=$3; shift 3
    [ -n "$conf_override" ] && conf=$conf_override
    [ -n "$dest_override" ] && dest=$dest_override
    backup=$conf.pushmpc-native-backup; absent=$conf.pushmpc-native-absent
    hook_path=$dest/pushmpc_hook.so
    case $action in
    status)
        st=installed; block_lines "$conf"; [ -n "$b" ] || st=not-installed
        detail=''
        if [ -f "$hook_path" ]; then detail=$(probe "$@"); fi
        grep -q -F "$VS_MARK" "$conf" 2>/dev/null && detail="${detail:+$detail; }the VapourSynth route's block is also in mpv.conf"
        emit "$name" "$st" "$detail"
        ;;
    install)
        if [ ! -f "$from/pushmpc_hook.so" ] || [ ! -f "$from/libpushmpc_engine.so" ] || ! ls "$from"/libpushmpc-avfilter-*.so >/dev/null 2>&1; then
            emit "$name" error "the files are not in $from (pushmpc_hook.so, libpushmpc-avfilter-*.so, libpushmpc_engine.so)"; return 1
        fi
        if [ "$allow_both" = 0 ] && grep -q -F "$VS_MARK" "$conf" 2>/dev/null; then
            emit "$name" refused "mpv.conf holds the VapourSynth route's block: uninstall it first, or pass --allow-both"; return 1
        fi
        run mkdir -p "$dest" "$(dirname "$conf")" || return 1
        run cp "$from/pushmpc_hook.so" "$from/libpushmpc_engine.so" "$from"/libpushmpc-avfilter-*.so "$dest/" || return 1
        [ -f "$from/SOURCES.txt" ] && run cp "$from/SOURCES.txt" "$dest/"
        for x in $EXTRAS; do [ -f "$from/$x" ] && run cp "$from/$x" "$dest/$x"; done
        # Vulkan / NVDEC decoding into the zero-copy engines: only when this mpv accepts the hwdec list (mpv 0.36+)
        hw=auto-safe
        if [ "$hwdec" != auto-safe ]; then
            hwlist=$hwdec,auto-safe
            if [ "$dry" = 1 ] || hwdec_ok "$@"; then hw=$hwdec; else log "$name: this mpv does not take hwdec=$hwlist: auto-safe"; fi
        fi
        if [ "$dry" = 1 ]; then emit "$name" would-install "$conf"; return 0; fi
        if [ ! -f "$backup" ] && [ ! -f "$absent" ]; then
            if [ -f "$conf" ]; then cp "$conf" "$backup"; else : > "$absent"; fi
        fi
        le=$(line_ending "$conf")
        tmp=$conf.pushmpc-native-tmp
        { block "$dest" "$le"; [ ! -f "$conf" ] || strip_block "$conf"; } > "$tmp" && mv "$tmp" "$conf" || return 1
        detail=$(probe "$@")
        case $detail in
            *"not active"*) emit "$name" installed-inactive "$detail"; return 3 ;;
            "") emit "$name" installed "no probe (mpv did not start)" ;;
            *) emit "$name" installed "$detail" ;;
        esac
        ;;
    uninstall)
        if [ "$dry" = 1 ]; then emit "$name" would-uninstall "$conf"; return 0; fi
        if [ -f "$conf" ]; then
            tmp=$conf.pushmpc-native-tmp
            strip_block "$conf" > "$tmp" && mv "$tmp" "$conf"
            if [ -f "$backup" ] && cmp -s "$conf" "$backup"; then rm -f "$backup"
            elif [ -f "$absent" ] && [ ! -s "$conf" ]; then rm -f "$conf" "$absent"
            fi
        fi
        rm -f "$absent" 2>/dev/null
        rm -f "$dest/pushmpc_hook.so" "$dest/libpushmpc_engine.so" "$dest"/libpushmpc-avfilter-*.so "$dest/SOURCES.txt"
        for x in $EXTRAS; do rm -f "$dest/$x"; done
        rmdir "$dest" 2>/dev/null
        emit "$name" uninstalled "$conf"
        ;;
    esac
}

cfg=${XDG_CONFIG_HOME:-}; case $cfg in /*) ;; *) cfg=$HOME/.config ;; esac
data=${XDG_DATA_HOME:-}; case $data in /*) ;; *) data=$HOME/.local/share ;; esac
rc=0; found=0
if [ "$target" != flatpak ] && command -v mpv >/dev/null 2>&1; then
    found=1; probe_base=; do_target native "$cfg/mpv/mpv.conf" "$data/pushmpc/mpvnative" mpv || rc=$?
fi
if [ "$target" != native ] && command -v flatpak >/dev/null 2>&1 && flatpak info io.mpv.Mpv >/dev/null 2>&1; then
    found=1
    fdata=$HOME/.var/app/io.mpv.Mpv
    probe_base=$fdata/cache   # the sandbox cannot see the host's /tmp
    do_target flatpak "$fdata/config/mpv/mpv.conf" "$fdata/data/pushmpc/mpvnative" flatpak run --command=mpv io.mpv.Mpv || rc=$?
fi
if [ "$found" = 0 ]; then emit "$target" unsupported "no mpv found (native or Flatpak io.mpv.Mpv)"; exit 3; fi
exit $rc
