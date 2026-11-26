#!/usr/bin/env bash
# The Linux add-on in a manylinux_2_28 container (glibc 2.28, static libstdc++): one build loads in every Kodi 21/22
# on Linux, from Flathub and distro packages to LibreELEC, CoreELEC and OSMC (aarch64). Run by kodi-addon.yml with the
# checkout and $RUNNER_TEMP mounted at the same paths as on the host, so the host runs the unit tests afterwards.
#   build-linux.sh <kodi dev kit include dir> <ffmpeg prefix> <build dir> [nvl]
# nvl: also the NVIDIA engine (x86_64; nvcc from NVIDIA's RHEL 8 repository, build time only)
set -euo pipefail
devkit="$1"; ff="$2"; build="$3"; nvl="${4:-}"
here=$(cd "$(dirname "$0")" && pwd); src=$(cd "$here/.." && pwd)
dnf install -y -q nasm >/dev/null 2>&1 || yum install -y -q nasm >/dev/null 2>&1 || echo "no nasm package: FFmpeg without x86 assembly"
if [ ! -f "$ff/lib/libavcodec.a" ]; then
  extra=""
  if [ "$(uname -m)" = x86_64 ] && ! command -v nasm >/dev/null 2>&1; then extra="--disable-x86asm"; fi
  bash "$here/build-ffmpeg.sh" "$FFMPEG_VERSION" "$ff" $extra
fi
args=()
if [ "$nvl" = nvl ] && [ "$(uname -m)" = x86_64 ]; then
  ver="${CUDA_PKG_VERSION:-12-8}"
  dnf config-manager --add-repo https://developer.download.nvidia.com/compute/cuda/repos/rhel8/x86_64/cuda-rhel8.repo >/dev/null 2>&1 || true
  dnf install -y -q "cuda-nvcc-$ver" "cuda-cudart-devel-$ver" >/dev/null
  export CUDA_PATH=/usr/local/cuda-${ver/-/.}
  "$CUDA_PATH/bin/nvcc" --version | tail -1
  args+=(-DPM_NVL=ON -DPM_NVCC="$CUDA_PATH/bin/nvcc")
  if [ -n "${SLANG_DIR:-}" ] && [ -x "$SLANG_DIR/bin/slangc" ]; then args+=(-DPM_SLANGC="$SLANG_DIR/bin/slangc"); fi
else
  args+=(-DPM_NVL=OFF)
fi
cmake -S "$src" -B "$build" -DCMAKE_BUILD_TYPE=Release -DKODI_DEVKIT="$devkit" -DFFMPEG_ROOT="$ff" "${args[@]}"
cmake --build "$build" --parallel
f=$(ls "$build"/addon/inputstream.pushmpc/inputstream.pushmpc.so.*)
echo "dynamic dependencies:"; readelf -d "$f" | grep NEEDED
echo "glibc symbol versions needed (must be <= 2.28):"; objdump -T "$f" | grep -o 'GLIBC_[0-9.]*' | sort -u -V | tail -3
echo "exported av* symbols (must be 0): $(nm -D --defined-only "$f" | grep -c ' av' || true)"
