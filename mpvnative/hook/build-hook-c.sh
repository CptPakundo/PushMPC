#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# build-hook-c.sh <out-dir>: the native hook's shipped C parts, built on Debian 12 (glibc 2.36) like its engine, never on
# the build machine's own glibc: pushmpc_hook.so (mpv's C plugin), pushmpc_preload.so (LD_PRELOAD for Flatpak / Snap apps)
# and pushmpc-plex-selftest. A build on Ubuntu 24.04 (glibc 2.39) bound sscanf / strtol to __isoc23_* (GLIBC_2.38): the
# hook did not load in Snap's Plex (core22, glibc 2.35) or Debian 12's mpv (players run 45951788669). Then
# glibc-ceiling.sh: nothing above GLIBC_2.35.
# Run from the repository root; needs docker. mpv's client header: the build machine's (pkg-config mpv) when it has one,
# else Debian 12's libmpv-dev.
set -eu
out=$1
mkdir -p "$out"
rm -rf .hookc-inc .hookc-out
mkdir -p .hookc-inc
if pkg-config --exists mpv 2>/dev/null; then cp -r "$(pkg-config --variable=includedir mpv)/mpv" .hookc-inc/; fi
docker run --rm -e HOST_ID="$(id -u):$(id -g)" -v "$PWD:/w" -w /w debian:bookworm sh -euc '
  apt-get update -qq >/dev/null
  pk="gcc libc6-dev"; [ -d .hookc-inc/mpv ] || pk="$pk libmpv-dev"
  apt-get install -y -qq --no-install-recommends $pk >/dev/null
  mkdir -p .hookc-out
  cc -shared -fPIC -O2 -Wall -Werror=implicit-function-declaration -I.hookc-inc mpvnative/hook/pushmpc_hook.c -o .hookc-out/pushmpc_hook.so -ldl
  cc -shared -fPIC -O2 -Wall -Werror=implicit-function-declaration mpvnative/hook/pushmpc_preload.c -o .hookc-out/pushmpc_preload.so -ldl -lpthread
  cc -O2 -Wall mpvnative/hook/libmpv_host.c -o .hookc-out/pushmpc-plex-selftest -ldl
  chown -R "$HOST_ID" .hookc-out .hookc-inc'
cp .hookc-out/* "$out/"
rm -rf .hookc-out .hookc-inc
sh mpvnative/hook/glibc-ceiling.sh "$out/pushmpc_hook.so" "$out/pushmpc_preload.so" "$out/pushmpc-plex-selftest"
