#!/usr/bin/env bash
# The 32-bit ARM Linux add-on (armv7 hard-float: OSMC on Vero and Raspberry Pi, CoreELEC "ng" on Amlogic boxes,
# LibreELEC for Raspberry Pi 2/3), cross-built in a debian:bullseye container (gcc 10, glibc 2.31, static libstdc++), then
# cadence_test under qemu-arm. Run by kodi-addon.yml (job build-linux-armv7) with the checkout and $RUNNER_TEMP mounted
# at the same paths as on the host.
#   build-linux-armv7.sh <kodi dev kit include dir> <ffmpeg prefix> <build dir>
set -euo pipefail
devkit="$1"; ff="$2"; build="$3"
here=$(cd "$(dirname "$0")" && pwd); src=$(cd "$here/.." && pwd)
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq crossbuild-essential-armhf git curl ca-certificates xz-utils make pkg-config python3-pip qemu-user > /dev/null
pip3 install -q 'cmake>=3.24,<4'
hash -r
cmake --version | head -1
tc="$build-toolchain.cmake"
cat > "$tc" <<'TC'
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armv7l)
set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)
set(CMAKE_C_FLAGS_INIT "-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard")
set(CMAKE_CXX_FLAGS_INIT "-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard")
set(CMAKE_FIND_ROOT_PATH /usr/arm-linux-gnueabihf)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_CROSSCOMPILING_EMULATOR "qemu-arm;-L;/usr/arm-linux-gnueabihf")
TC
if [ ! -f "$ff/lib/libavcodec.a" ]; then
  bash "$here/build-ffmpeg.sh" "$FFMPEG_VERSION" "$ff" --enable-cross-compile --target-os=linux --arch=arm --cpu=armv7-a \
    --enable-neon --cross-prefix=arm-linux-gnueabihf- --extra-cflags="-mfpu=neon-vfpv4 -mfloat-abi=hard"
fi
cmake -S "$src" -B "$build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$tc" -DKODI_DEVKIT="$devkit" -DFFMPEG_ROOT="$ff" -DPM_NVL=OFF
cmake --build "$build" --parallel
f=$(ls "$build"/addon/inputstream.pushmpc/inputstream.pushmpc.so.*)
echo "dynamic dependencies:"; arm-linux-gnueabihf-readelf -d "$f" | grep NEEDED
echo "glibc symbol versions needed (must be <= 2.31):"; arm-linux-gnueabihf-objdump -T "$f" | grep -o 'GLIBC_[0-9.]*' | sort -u -V | tail -3
echo "exported av* symbols (must be 0): $(arm-linux-gnueabihf-nm -D --defined-only "$f" | grep -c ' av' || true)"
# the CPU-only unit test under qemu (the GPU engine's tests need a Vulkan device)
ctest --test-dir "$build" --output-on-failure -R cadence_test
