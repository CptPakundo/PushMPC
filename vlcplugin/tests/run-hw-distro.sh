#!/usr/bin/env bash
# Inside a distro container (root; the host's render node and /dev/udmabuf passed in): that distro's VLC 3 and libva,
# the null VA-API driver built against them, then run-hw.sh as an ordinary user (VLC refuses to run as root), and the
# installer's hardware self-test. Ubuntu 24.04's VLC 3.0.20 ships no VA-API decoder module, so the VA-API cases run
# here, in distros whose VLC has one.
# args: <label> [subset|full|desktop]
#   subset   a few VA-API cases (DRM decoder)
#   full     every VA-API case (DRM decoder), the HEVC Main 10 clip, the installer's headless self-test both ways
#   desktop  (Mesa >= 24.1: Debian 13) the VA-API decoder with VLC's OpenGL output and GL interop under Xvfb, zero copy
#            into the GPU engine on lavapipe, and the installer's interop tier with its fallback to vaapi_drm
set -eu
label="$1"; mode="${2:-subset}"
export DEBIAN_FRONTEND=noninteractive
extra=""
[ "$mode" = desktop ] && extra="xvfb xauth mesa-vulkan-drivers libegl-mesa0 libgl1-mesa-dri libegl1 mesa-utils"
apt-get -qq update >/dev/null
# shellcheck disable=SC2086
apt-get -qq install -y --no-install-recommends vlc-bin vlc-plugin-base vlc-plugin-video-output libva-dev gcc libc6-dev \
  python3 perl $extra >/dev/null
vlc --version 2>/dev/null | head -1 || true
find /usr/lib -path '*vlc/plugins*' \( -name '*vaapi*' -o -name '*vdpau*' -o -name '*glconv*' \) | sort
ls -l /dev/dri /dev/udmabuf 2>&1 || true
mkdir -p /opt/fakehw
gcc -std=gnu11 -O2 -Wall -fPIC -shared -o /opt/fakehw/pmnull_drv_video.so vlcplugin/tests/fakehw/pmnull_drv_video.c -lpthread
id tester >/dev/null 2>&1 || useradd -m tester
mkdir -p res && chmod -R a+rwX res
cases='va_base va_x4 va_x8_noout va_noderive_x4 va_broken_x4'
envx=""
case "$mode" in
  full) cases='' ;;                                   # every VA-API case, the HEVC Main 10 clip too
  desktop) cases='va_base va_x4 va_zc_x2 va_zc_x2_sync va10_zc_x2 va_zc_noexport_x2 va_zc_novk_x2 vagl_base vagl_x2 vagl_broken_x2 vagl_zc_x2'
           envx='ZC=1 GL=1' ;;
esac
run_as() { su tester -c "$1"; }
wrap=""
[ "$mode" = desktop ] && wrap="xvfb-run -a -s '-screen 0 1280x720x24'"
if [ "$mode" = desktop ]; then
  run_as "xvfb-run -a sh -c 'eglinfo -B 2>/dev/null | head -20; eglinfo 2>/dev/null | grep -o EGL_EXT_image_dma_buf_import | head -1'" || true
fi
rc=0
run_as "set -e; d=\$HOME/.local/share/pushmpc/vlc; mkdir -p \$d; cp art/plugin-linux/*.so \$d/; \
  export VLC=/usr/bin/vlc PLUGDIR=\$d FAKEHW=/opt/fakehw CLIP=art/clips/hwclip.mkv CLIP10=art/clips/hwclip10.mkv \
  SRC_FRAMES=\$(sed -n 's/nb_read_frames=//p' art/clips/hwclip.txt) SRC_FRAMES10=\$(sed -n 's/nb_read_frames=//p' art/clips/hwclip10.txt) \
  OUT=res/hw-$label LABEL='$label distro VLC, null VA-API driver ($mode)' $envx \
  CASES='$cases' GITHUB_STEP_SUMMARY=res/hw-$label/summary.md; $wrap bash vlcplugin/tests/run-hw.sh" || rc=1
[ "$mode" = subset ] && exit $rc

# the installer's hardware self-test: the state file and the vlcrc tell what it decided
selftest() { # <regexp for the state file's first line> <avcodec-hw value, or "default"> <env> <wrapper>
  run_as "set -eu; export LIBVA_DRIVERS_PATH=/opt/fakehw LIBVA_DRIVER_NAME=pmnull $3
    d=\$HOME/.local/share/pushmpc/vlc; st=\$d/hw-selftest.txt; rc=\$HOME/.config/vlc/vlcrc
    $4 sh vlcplugin/install/pushmpc-vlc.sh selftest-hw --target native --clip art/clips/hwclip.mkv
    cat \$st; head -1 \$st | grep -qE '$1' || { tail -30 \$d/pushmpc-selftest.log; echo 'SELFTEST-FAIL: state'; exit 1; }
    v=\$(sed -n 's/^avcodec-hw=//p' \$rc); echo \"avcodec-hw=\${v:-(default)}\"
    if [ '$2' = default ]; then [ -z \"\$v\" ] || { echo 'SELFTEST-FAIL: avcodec-hw'; exit 1; }
    else [ \"\$v\" = '$2' ] || { echo 'SELFTEST-FAIL: avcodec-hw'; exit 1; }; fi"
}
run_as "set -eu; rm -rf \$HOME/.local/share/pushmpc/vlc; mkdir -p \$HOME/.config/vlc
  printf '[core]\nvideo-filter=adjust\n[avcodec]\n#avcodec-hw=any\n' > \$HOME/.config/vlc/vlcrc
  sh vlcplugin/install/pushmpc-vlc.sh install --no-hw-selftest --plugin art/plugin-linux/libpushmpc_plugin.so --target native"
if [ "$mode" = full ]; then
  echo "--- self-test, headless: the DRM decoder passes -> avcodec-hw=vaapi_drm"
  selftest '^ok direct vaapi_drm' vaapi_drm '' '' || rc=1
  echo "--- a driver that loses uploads -> software decoding"
  selftest '^failed' none 'PMNULL_BROKEN=1' '' || rc=1
else
  echo "--- self-test with a display: VLC's OpenGL interop passes -> the user's own avcodec-hw (VLC's default)"
  selftest '^ok interop vaapi' default '' 'xvfb-run -a' || rc=1
  echo "--- no surface export (no GL interop) -> the DRM decoder tier: avcodec-hw=vaapi_drm"
  selftest '^ok direct vaapi_drm' vaapi_drm 'PMNULL_EXPORT=0' 'xvfb-run -a' || rc=1
  echo "--- a driver that loses uploads -> software decoding"
  selftest '^failed' none 'PMNULL_BROKEN=1' 'xvfb-run -a' || rc=1
fi
run_as "sh vlcplugin/install/pushmpc-vlc.sh uninstall --target native; [ ! -e \$HOME/.local/share/pushmpc/vlc/hw-selftest.txt ] && echo 'self-test state removed'" || rc=1
exit $rc
