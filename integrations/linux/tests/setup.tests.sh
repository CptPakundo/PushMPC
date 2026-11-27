#!/bin/sh
# setup.sh's plan per graphics card (no GPU, no player, a scratch HOME): the family by pushcore's rules and the mpv
# route's engine by backend.h's order. Run from the repository root: sh integrations/linux/tests/setup.tests.sh
SH=sh
set -u
tool=integrations/linux/setup.sh
home=$(mktemp -d)
pass=0; fail=0
check() {   # check <gpu> <vulkan> <nvidia driver 0|1> <os> <expected text>...
    g=$1; vk=$2; nv=$3; os=$4; shift 4
    out=$(HOME=$home PUSHMPC_TEST_GPU="$g" PUSHMPC_TEST_VULKAN="$vk" PUSHMPC_TEST_NVIDIA_DRIVER=$nv PUSHMPC_TEST_UNAME=$os PATH=/usr/bin:/bin $SH $tool plan 2>&1)
    for want in "$@"; do
        if printf '%s\n' "$out" | grep -q -F -- "$want"; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL [$g / $os]: no '$want' in:"; printf '%s\n' "$out" | sed 's/^/    /'; fi
    done
}
check '10de:2684:NVIDIA GeForce RTX 4090' 'NVIDIA GeForce RTX 4090' 1 Linux 'family RTX 40' 'nvidia: RTX Video Super Resolution + NVIDIA VFG' "NVIDIA Video Effects: offered"
check '10de:2204:NVIDIA GeForce RTX 3090' 'NVIDIA GeForce RTX 3090' 1 Linux 'family RTX 30' 'RTX 30 port once its self-test passes'
check '10de:1e84:NVIDIA GeForce RTX 2070 SUPER' 'NVIDIA GeForce RTX 2070 SUPER' 1 Linux 'family RTX 20' 'frame generation by portable GPU engine on NVIDIA GeForce RTX 2070 SUPER'
check '10de:2684:NVIDIA GeForce RTX 4090' 'NVK' 0 Linux 'portable: the GPU engine on NVK' "needs NVIDIA's proprietary driver"
check '10de:1b80:NVIDIA GeForce GTX 1080' 'NVIDIA GeForce GTX 1080' 1 Linux 'family GTX' 'portable: the GPU engine on NVIDIA GeForce GTX 1080' 'NVIDIA Video Effects: not for this GPU'
check '1002:744c:AMD Radeon RX 7900 XTX' 'AMD Radeon RX 7900 XTX (RADV NAVI31)' 0 Linux 'family RX 7000' 'portable: the GPU engine on AMD Radeon RX 7900 XTX'
check '1002:73bf:AMD Radeon RX 6800 XT' '' 0 Linux 'family RX 6000' 'portable: the CPU engine'
check '8086:56a0:Intel Arc A770' 'Intel(R) Arc(tm) A770 Graphics (DG2)' 0 Linux 'family Intel' 'portable: the GPU engine on Intel'
check '106b:0:Apple M4' '' 0 Darwin 'family Apple' 'apple: VideoToolbox frame interpolation' "Capture mode: comes with Homebrew's pushmpc"
check 'none' '' 0 Linux 'family none' 'portable: the CPU engine' 'Capture mode: not in this download'
# the pmml runtime installed: ml before portable
mkdir -p "$home/.local/share/pushmpc/pmml/graphs"; : > "$home/.local/share/pushmpc/pmml/graphs/synth-yuv.onnx"
check '1002:744c:AMD Radeon RX 7900 XTX' 'AMD Radeon RX 7900 XTX (RADV NAVI31)' 0 Linux 'ml: PushFlow (RIFE) through pmml'
check '10de:1e84:NVIDIA GeForce RTX 2070 SUPER' 'NVIDIA GeForce RTX 2070 SUPER' 1 Linux 'frame generation by ml (PushFlow through pmml'
# capture mode ships on Linux (the tarball's capture/, release.yml requires it): installed, it is offered
mkdir -p "$home/.local/share/pushmpc/capture"; printf '#!/bin/sh
' > "$home/.local/share/pushmpc/capture/pushmpc-capture"; chmod 755 "$home/.local/share/pushmpc/capture/pushmpc-capture"
check 'none' '' 0 Linux 'Capture mode: available for players with no other route'
rm -rf "$home"
echo "setup.sh plan: $pass passed, $fail failed"
[ "$fail" = 0 ]
