#!/usr/bin/env bash
# Inside a distro container (root, the host's render node passed in): that distro's VLC 3 and libva, the null VA-API
# driver built against them, then a subset of run-hw.sh as an ordinary user (VLC refuses to run as root).
# args: <label>; the workspace is the current directory (art/ and res/ as in the workflow)
set -eu
label="$1"
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
su tester -c "set -e; d=\$HOME/.local/share/pushmpc/vlc; mkdir -p \$d; cp art/plugin-linux/*.so \$d/; \
  VLC=/usr/bin/vlc PLUGDIR=\$d FAKEHW=/opt/fakehw CLIP=art/clips/hwclip.mkv \
  SRC_FRAMES=\$(sed -n 's/nb_read_frames=//p' art/clips/hwclip.txt) OUT=res/hw-$label LABEL='$label distro VLC, null VA-API driver' \
  CASES='va_base va_x4 va_x8_noout va_noderive_x4 va_broken_x4' GITHUB_STEP_SUMMARY=res/hw-$label/summary.md \
  bash vlcplugin/tests/run-hw.sh"
