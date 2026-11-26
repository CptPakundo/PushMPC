#!/bin/sh
# Builds Tencent ncnn from its pinned full-source release (scripts/ml-runtime-pin.json, ncnn.platforms.source) as a
# static, position-independent library with Vulkan, for linking into pmml's ncnn backend inside pushmpc_vs.so on hosts
# where no prebuilt ncnn package fits (manylinux_2_28 = glibc 2.28; Tencent's oldest Linux package needs 2.35).
#   simplevk:   no Vulkan SDK at build time; ncnn dlopens libvulkan.so.1 at run time
#   simpleomp:  ncnn's own OpenMP runtime, so nothing links libgomp statically
#
#   sh scripts/build-ncnn-static.sh <install prefix> [<cache dir>]     (run from the repository root)
# Needs cmake, a C++ compiler, curl and python3 (PY overrides the python). Exit 0 = <prefix>/lib/cmake/ncnn exists.
set -eu
prefix=$1; cache=${2:-/tmp/ncnn-dl}
PY=${PY:-python3}
pin=scripts/ml-runtime-pin.json
url=$("$PY" -c "import json; print(json.load(open('$pin'))['ncnn']['platforms']['source']['url'])")
sha=$("$PY" -c "import json; print(json.load(open('$pin'))['ncnn']['platforms']['source']['sha256'])")
mkdir -p "$cache"
zip=$cache/$(basename "$url")
if ! { [ -f "$zip" ] && echo "$sha  $zip" | sha256sum -c - >/dev/null 2>&1; }; then
    curl -fsSL --retry 3 -o "$zip" "$url"
    echo "$sha  $zip" | sha256sum -c -
fi
src=$cache/src; rm -rf "$src"; mkdir -p "$src"
"$PY" -c "import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$zip" "$src"
top=$(find "$src" -maxdepth 2 -name CMakeLists.txt | awk '{ print length, $0 }' | sort -n | head -n 1 | cut -d' ' -f2-); top=$(dirname "$top")
cmake -S "$top" -B "$cache/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DNCNN_VULKAN=ON -DNCNN_SIMPLEVK=ON -DNCNN_SIMPLEOMP=ON -DNCNN_SHARED_LIB=OFF \
    -DNCNN_BUILD_TOOLS=OFF -DNCNN_BUILD_EXAMPLES=OFF -DNCNN_BUILD_TESTS=OFF -DNCNN_BUILD_BENCHMARK=OFF -DNCNN_PYTHON=OFF >/dev/null
# a bare --parallel means make -j without a limit: ncnn's hundreds of objects then starve the runner of memory
cmake --build "$cache/build" --parallel "$(nproc 2>/dev/null || echo 2)" >/dev/null
cmake --install "$cache/build" --prefix "$prefix" >/dev/null
[ -d "$prefix/lib/cmake/ncnn" ] || [ -d "$prefix/lib64/cmake/ncnn" ]
echo "ncnn $(basename "$url" .zip): static, PIC, Vulkan (simplevk) in $prefix"
