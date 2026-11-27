#!/usr/bin/env bash
# Inside a distro container (root; the host's render node and /dev/udmabuf passed in): that distro's VLC 3 and libva,
# the null VA-API driver built against them, then run-hw.sh as an ordinary user (VLC refuses to run as root), and the
# installer's hardware self-test. Ubuntu 24.04's VLC 3.0.20 ships no VA-API decoder module, so the VA-API cases run
# here, in distros whose VLC has one.
# args: <label> [subset|full|desktop|interop]
#   subset   a few VA-API cases (DRM decoder)
#   full     every VA-API case (DRM decoder), the HEVC Main 10 clip, the installer's headless self-test both ways
#   desktop  (Debian 13, its own Mesa 25.0) zero copy into the GPU engine on lavapipe; the installer under Xvfb: Mesa 25.0
#            hides EGL dma-buf import on an X server without DRI3, so the interop tier fails cleanly and vaapi_drm applies
#   interop  (Debian 13 + Mesa from trixie-backports, >= 25.1, which imports dma-bufs on any X server) the VA-API
#            decoder with VLC's OpenGL output and GL interop (glconv_vaapi_x11) under Xvfb, required; the installer's
#            interop tier passes
set -eu
label="$1"; mode="${2:-subset}"
export DEBIAN_FRONTEND=noninteractive
extra=""
case "$mode" in desktop|interop) extra="xvfb xauth mesa-vulkan-drivers libegl-mesa0 libgl1-mesa-dri libegl1 mesa-utils" ;; esac
if [ "$mode" = interop ]; then
  echo "deb http://deb.debian.org/debian trixie-backports main" > /etc/apt/sources.list.d/backports.list
fi
apt-get -qq update >/dev/null
# shellcheck disable=SC2086
apt-get -qq install -y --no-install-recommends vlc-bin vlc-plugin-base vlc-plugin-video-output libva-dev gcc libc6-dev \
  python3 perl $extra >/dev/null
if [ "$mode" = interop ]; then   # the whole Mesa set together (mesa-libgallium is versioned per release)
  apt-get -qq install -y --no-install-recommends -t trixie-backports libegl-mesa0 libgl1-mesa-dri libglx-mesa0 libgbm1 \
    mesa-libgallium mesa-vulkan-drivers mesa-utils >/dev/null
  dpkg-query -W -f='${Package} ${Version}\n' libegl-mesa0 mesa-libgallium
fi
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
  desktop) cases='va_base va_x4 va_zc_x2 va_zc_x2_sync va10_zc_x2_sync va_zc_noexport_x2 va_zc_novk_x2 va_zc_x2_tap va_zc_x2_notap va_zc_x2_lag va_zc_x2_switch'
           envx='ZC=1' ;;
  interop) cases='va_base vagl_base vagl_x2 vagl_broken_x2 vagl_zc_x2'
           envx='ZC=1 GL=1' ;;
esac
run_as() { su tester -c "$1"; }
wrap=""
case "$mode" in desktop|interop) wrap="xvfb-run -a -s '-screen 0 1280x720x24'" ;; esac
interop=0
if [ "$mode" = interop ]; then
  # VLC's GL interop needs EGL_EXT_image_dma_buf_import on the X11 platform (Mesa >= 25.1 offers it without DRI3); here
  # it is required. eglinfo's X11 section is printed for the record; VLC's own EGL line (hwcheck --need) decides.
  run_as "xvfb-run -a sh -c 'eglinfo 2>/dev/null'" > /tmp/eglinfo.txt 2>&1 || true
  cp /tmp/eglinfo.txt res/eglinfo-$label.txt
  awk '/^X11 platform/ {p = 1; next} /^[A-Za-z].* platform/ {p = 0} p' /tmp/eglinfo.txt | grep -E "EGL driver name|EGL version|OpenGL core profile version|dma_buf_import" | head -5
  interop=1
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
  if [ $interop = 1 ]; then
    echo "--- self-test with a display (Mesa >= 25.1): VLC's OpenGL interop passes -> the user's own avcodec-hw (VLC's default)"
    selftest '^ok interop vaapi' default '' 'xvfb-run -a' || rc=1
  else
    echo "--- self-test with a display, Mesa 25.0 on an X server without DRI3 (no EGL dma-buf import): the interop tier fails cleanly, the DRM decoder tier passes"
    selftest '^ok direct vaapi_drm' vaapi_drm '' 'xvfb-run -a' || rc=1
  fi
  echo "--- no surface export (no GL interop) -> the DRM decoder tier: avcodec-hw=vaapi_drm"
  selftest '^ok direct vaapi_drm' vaapi_drm 'PMNULL_EXPORT=0' 'xvfb-run -a' || rc=1
  echo "--- a driver that loses uploads -> software decoding"
  selftest '^failed' none 'PMNULL_BROKEN=1' 'xvfb-run -a' || rc=1
fi
run_as "sh vlcplugin/install/pushmpc-vlc.sh uninstall --target native; [ ! -e \$HOME/.local/share/pushmpc/vlc/hw-selftest.txt ] && echo 'self-test state removed'" || rc=1
exit $rc
