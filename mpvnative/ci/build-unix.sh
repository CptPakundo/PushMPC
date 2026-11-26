#!/bin/bash
# build-unix.sh <prefix> <src dir> [linux|macos|msys2]: Vulkan-Headers, libplacebo (when the system one is too old),
# FFmpeg with vf_pushmpc, mpv, all from the tags pinned in mpvnative/pins.json (commits checked), into <prefix>.
# ccache is used when present (CI caches its folder). Idempotent: finished steps leave a stamp in <prefix>/stamps.
set -euo pipefail
P=$1; S=$2; OS=${3:-linux}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
J=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
mkdir -p "$P/stamps" "$S"
export PKG_CONFIG_PATH="$P/lib/pkgconfig:$P/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export CFLAGS="-I$P/include ${CFLAGS:-}" CPPFLAGS="-I$P/include ${CPPFLAGS:-}" LDFLAGS="-L$P/lib ${LDFLAGS:-}"
BASECC=cc; [ "$OS" = msys2 ] && BASECC=gcc
CC="$BASECC"; command -v ccache >/dev/null && CC="ccache $BASECC"
pin() { python3 -c "import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]][sys.argv[3]])" "$ROOT/mpvnative/pins.json" "$1" "$2"; }
. "$ROOT/mpvnative/ci/fetch-commit.sh"
fetch() {   # fetch <pin> <dir> [--recurse]: the pinned commit itself (fetch-commit.sh: by SHA, mirrors, retries)
    local d=$2
    # a cached tree already at the pin stays as it is (it may carry our patches)
    if [ -d "$d/.git" ] && [ "$(git -C "$d" rev-parse HEAD 2>/dev/null)" = "$(pin "$1" commit)" ]; then return 0; fi
    fetch_commit "$d" "$(pin "$1" commit)" "$(pin "$1" repo)" "$(pin "$1" tag)" || exit 1
    [ -z "${3:-}" ] || git -C "$d" submodule update -q --init --recursive --depth 1 || exit 1
}
stamp() { echo "$1:$(pin "$2" commit)"; }
done_() { [ -f "$P/stamps/$1" ] && [ "$(cat "$P/stamps/$1")" = "$(stamp "$1" "$2")" ]; }
mark() { stamp "$1" "$2" > "$P/stamps/$1"; }

# Vulkan-Headers (FFmpeg 9.1's fruc_vulkan needs >= 1.4.317)
if ! done_ vulkanHeaders vulkanHeaders; then
    fetch vulkanHeaders "$S/Vulkan-Headers"
    cmake -S "$S/Vulkan-Headers" -B "$S/Vulkan-Headers/b" -DCMAKE_INSTALL_PREFIX="$P" -DVULKAN_HEADERS_ENABLE_TESTS=OFF -DVULKAN_HEADERS_ENABLE_MODULE=OFF >/dev/null
    cmake --install "$S/Vulkan-Headers/b" >/dev/null
    mark vulkanHeaders vulkanHeaders
fi

# libplacebo >= 7.387.0 (mpv 0.42); the system's when new enough
if pkg-config --atleast-version=7.387.0 libplacebo 2>/dev/null; then
    echo "libplacebo $(pkg-config --modversion libplacebo) from the system"
elif ! done_ libplacebo libplacebo; then
    fetch libplacebo "$S/libplacebo" --recurse
    rm -rf "$S/libplacebo/b"
    CC="$CC" meson setup "$S/libplacebo/b" "$S/libplacebo" --prefix="$P" --libdir=lib --buildtype=release \
        -Dvulkan-registry="$P/share/vulkan/registry/vk.xml" -Ddemos=false -Dtests=false -Dbench=false -Dfuzz=false \
        $( [ "$OS" = macos ] && echo "-Dd3d11=disabled" )
    ninja -C "$S/libplacebo/b" install
    mark libplacebo libplacebo
fi

# FFmpeg 9.1 + vf_pushmpc (rebuilt on every run: ccache makes it a relink unless the filter changed)
fetch ffmpeg "$S/ffmpeg"
sh "$ROOT/mpvnative/ffmpeg/add-to-ffmpeg.sh" "$S/ffmpeg"
cfg=(--prefix="$P" --libdir="$P/lib" --enable-gpl --enable-version3 --enable-shared --disable-static --disable-doc
     --disable-debug --enable-vulkan --cc="$CC" --extra-cflags="-I$P/include" --extra-ldflags="-L$P/lib")
case $OS in
    msys2) cfg+=(--enable-d3d11va --enable-dxva2 --disable-libplacebo) ;;
    macos) cfg+=(--enable-videotoolbox) ;;
    linux) cfg+=(--disable-libplacebo) ;;
esac
want="${cfg[*]}"
cd "$S/ffmpeg"
if [ ! -f config.h ] || [ "$(cat .pm-config 2>/dev/null)" != "$want" ]; then
    ./configure "${cfg[@]}" > configure.out 2>&1 || { tail -40 configure.out; cat ffbuild/config.log | tail -60; exit 1; }
    echo "$want" > .pm-config
fi
grep -q 'CONFIG_PUSHMPC_FILTER 1' config_components.h || { echo "::error::FFmpeg configure did not enable the pushmpc filter"; exit 1; }
grep -E 'CONFIG_(PUSHMPC|FRUC_VULKAN|D3D11VA|VULKAN)(_FILTER)? ' config.h config_components.h || true
make -j"$J" >/dev/null
make install >/dev/null
cd "$ROOT"

# mpv 0.42.0 against it
if ! done_ mpv mpv || { [ ! -x "$P/bin/mpv" ] && [ ! -f "$P/bin/mpv.exe" ]; }; then
    fetch mpv "$S/mpv"
    rm -rf "$S/mpv/b"
    extra=()
    case $OS in
        linux) extra+=(-Dc_link_args="-Wl,-rpath,\$ORIGIN/../lib") ;;
        macos) ;;   # mpv 0.42 builds its macOS player with Swift (Xcode's): cplayer needs it
    esac
    CC="$CC" meson setup "$S/mpv/b" "$S/mpv" --prefix="$P" --libdir=lib --buildtype=release -Dlibmpv=true -Dcplayer=true \
        -Dmanpage-build=disabled -Dhtml-build=disabled -Dpdf-build=disabled "${extra[@]}" > "$S/mpv-setup.log" 2>&1 \
        || { tail -60 "$S/mpv-setup.log"; exit 1; }
    grep -iE "ffmpeg|libplacebo|vulkan|d3d11|lua|enabled features" "$S/mpv-setup.log" | head -20 || true
    ninja -C "$S/mpv/b" install >/dev/null
    mark mpv mpv
fi
"$P/bin/mpv" --version | head -3 || true
