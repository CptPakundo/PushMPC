#!/usr/bin/env bash
# Kodi for Android in the emulator (kodi-addon.yml, job kodi-android, inside reactivecircus/android-emulator-runner):
# installs Kodi's APK, puts the clips into Kodi's own app folder (no storage permission needed there) and plays the
# cases with run_cases.py through android-kodi.sh; the add-on goes into Kodi's home the way a zip install puts it.
#   android-test.sh <kodi apk> <add-on dir> <clips dir> <out dir> <kodi major> [cases]
set -uo pipefail
apk=$1; addon=$2; clips=$3; out=$4; major=$5; cases=${6:-}
here=$(cd "$(dirname "$0")" && pwd)
dev=/sdcard/Android/data/org.xbmc.kodi/files
home=$RUNNER_TEMP/kodi-android-home; rm -rf "$home"; mkdir -p "$home/userdata" "$home/temp" "$out"
adb wait-for-device
adb shell getprop ro.product.cpu.abilist
# Kodi publishes x86 (32-bit) Android builds; the x86_64 system image runs them
adb install -r --abi x86 "$apk" || adb install -r "$apk"
adb shell pm grant org.xbmc.kodi android.permission.READ_EXTERNAL_STORAGE 2>/dev/null || true
adb shell mkdir -p "$dev/.kodi" "$dev/clips"
adb push "$clips/." "$dev/clips/" >/dev/null
adb shell ls -l "$dev/clips"
before="adb shell mkdir -p $dev/.kodi && adb push '$home/.' '$dev/.kodi/' >/dev/null"
after="mkdir -p '$home/userdata' '$home/temp'; adb pull '$dev/.kodi/userdata/Database' '$home/userdata/' >/dev/null 2>&1; adb pull '$dev/.kodi/temp/kodi.log' '$home/temp/kodi.log' >/dev/null 2>&1; true"
export PM_KODI_MAX=100
python3 "$here/run_cases.py" --kodi "sh $here/android-kodi.sh" --kill "adb shell am force-stop org.xbmc.kodi" \
  --before "$before" --after "$after" --first 90 --player-clips "$dev/clips" \
  --home "$home" --addon "$addon" --clips "$clips" --major "$major" --out "$out" --cases "$cases"
rc=$?
adb logcat -d > "$out/logcat.txt" 2>&1 || true
exit $rc
