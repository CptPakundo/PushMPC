#!/bin/sh
# Kodi on an Android emulator for run_cases.py (--kodi "sh android-kodi.sh"): starts Kodi (with a file: Android's
# "open with" intent, as a file manager does), then stays until Kodi's process is gone (drive.py quits Kodi over
# JSON-RPC) or $PM_KODI_MAX seconds passed; JSON-RPC reaches the device through adb forward tcp:9090.
#   android-kodi.sh [file on the device]
pkg=org.xbmc.kodi
max=${PM_KODI_MAX:-120}
adb forward tcp:9090 tcp:9090 >/dev/null
if [ -n "${1:-}" ]; then
    case $1 in *.mkv) t=video/x-matroska ;; *.mp4) t=video/mp4 ;; *.ts) t=video/mp2t ;; *) t='video/*' ;; esac
    adb shell am start -W -a android.intent.action.VIEW -d "file://$1" -t "$t" -n "$pkg/.Splash" >/dev/null
else
    adb shell am start -W -n "$pkg/.Splash" >/dev/null
fi
sleep 5
i=0
while [ "$i" -lt "$max" ]; do
    [ -n "$(adb shell pidof "$pkg" 2>/dev/null | tr -d '\r')" ] || break
    sleep 2; i=$((i + 2))
done
adb shell am force-stop "$pkg"
