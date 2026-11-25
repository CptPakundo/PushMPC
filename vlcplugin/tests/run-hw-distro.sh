#!/usr/bin/env bash
# Inside a distro container (root, the host's render node passed in): that distro's VLC 3 and libva, the null VA-API
# driver built against them, then run-hw.sh as an ordinary user (VLC refuses to run as root), optionally followed by
# the installer's hardware self-test (passes on the working driver, keeps software decoding on a broken one).
# Ubuntu 24.04's VLC 3.0.20 ships no VA-API decoder module, so the VA-API cases run here, in distros whose VLC has one.
# args: <label> [full]; the workspace is the current directory (art/ and res/ as in the workflow)
set -eu
label="$1"; mode="${2:-subset}"
export DEBIAN_FRONTEND=noninteractive
apt-get -qq update >/dev/null
apt-get -qq install -y --no-install-recommends vlc-bin vlc-plugin-base vlc-plugin-video-output libva-dev gcc libc6-dev \
  python3 perl >/dev/null
vlc --version 2>/dev/null | head -1 || true
find /usr/lib -path '*vlc/plugins*' \( -name '*vaapi*' -o -name '*vdpau*' \) | sort
mkdir -p /opt/fakehw
gcc -std=gnu11 -O2 -Wall -fPIC -shared -o /opt/fakehw/pmnull_drv_video.so vlcplugin/tests/fakehw/pmnull_drv_video.c -lpthread
id tester >/dev/null 2>&1 || useradd -m tester
mkdir -p res && chmod -R a+rwX res
cases='va_base va_x4 va_x8_noout va_noderive_x4 va_broken_x4'
[ "$mode" = full ] && cases=''     # every VA-API case, the HEVC Main 10 clip too
rc=0
su tester -c "set -e; d=\$HOME/.local/share/pushmpc/vlc; mkdir -p \$d; cp art/plugin-linux/*.so \$d/; \
  VLC=/usr/bin/vlc PLUGDIR=\$d FAKEHW=/opt/fakehw CLIP=art/clips/hwclip.mkv CLIP10=art/clips/hwclip10.mkv \
  SRC_FRAMES=\$(sed -n 's/nb_read_frames=//p' art/clips/hwclip.txt) SRC_FRAMES10=\$(sed -n 's/nb_read_frames=//p' art/clips/hwclip10.txt) \
  OUT=res/hw-$label LABEL='$label distro VLC, null VA-API driver' \
  CASES='$cases' GITHUB_STEP_SUMMARY=res/hw-$label/summary.md bash vlcplugin/tests/run-hw.sh" || rc=1
[ "$mode" = full ] || exit $rc

echo "--- the installer's hardware self-test ($label)"
su tester -c "set -eu; export LIBVA_DRIVERS_PATH=/opt/fakehw LIBVA_DRIVER_NAME=pmnull
  d=\$HOME/.local/share/pushmpc/vlc; st=\$d/hw-selftest.txt; rc=\$HOME/.config/vlc/vlcrc
  rm -rf \$d; mkdir -p \$HOME/.config/vlc; printf '[core]\nvideo-filter=adjust\n[avcodec]\n#avcodec-hw=any\n' > \$rc
  sh vlcplugin/install/pushmpc-vlc.sh install --plugin art/plugin-linux/libpushmpc_plugin.so --target native --clip art/clips/hwclip.mkv
  cat \$st; grep -q '^ok vaapi_drm' \$st || { tail -30 \$d/pushmpc-selftest.log; echo 'SELFTEST-FAIL: VA-API self-test did not pass'; exit 1; }
  ! grep -q '^avcodec-hw=none' \$rc || { echo 'SELFTEST-FAIL: avcodec-hw=none left after a pass'; exit 1; }
  PMNULL_BROKEN=1 sh vlcplugin/install/pushmpc-vlc.sh selftest-hw --target native --clip art/clips/hwclip.mkv
  cat \$st; grep -q '^failed' \$st && grep -q '^avcodec-hw=none' \$rc || { echo 'SELFTEST-FAIL: broken driver not caught'; exit 1; }
  sh vlcplugin/install/pushmpc-vlc.sh uninstall --target native
  [ ! -e \$st ] && echo 'self-test: both ways as expected, state removed'" || rc=1
exit $rc
