#!/usr/bin/env bash
# Kodi for Android in the emulator (kodi-addon.yml, job kodi-android, inside reactivecircus/android-emulator-runner):
# installs Kodi's APK, starts it once (it creates its own app folder), puts the clips into that folder and plays the
# cases with run_cases.py through android-kodi.sh; the add-on goes into Kodi's home the way a zip install puts it.
#   android-test.sh <kodi apk> <add-on dir> <clips dir> <out dir> <kodi major> <abi: x86|arm64-v8a> [cases]
# arm64-v8a on the x86_64 image runs through the image's ARM translation; without it the run is skipped (exit 0, a
# note in the summary)
set -uo pipefail
apk=$1; addon=$2; clips=$3; out=$4; major=$5; abi=$6; cases=${7:-}
here=$(cd "$(dirname "$0")" && pwd)
dev=/sdcard/Android/data/org.xbmc.kodi/files
home=$RUNNER_TEMP/kodi-android-home; rm -rf "$home"; mkdir -p "$home/userdata" "$home/temp" "$out"
adb wait-for-device
abilist=$(adb shell getprop ro.product.cpu.abilist | tr -d '\r')
echo "device ABIs: $abilist"
case ",$abilist," in
  *",$abi,"*) ;;
  *) printf '**skipped:** this emulator image cannot run %s apps (ABIs: %s; no ARM translation)\n\n' "$abi" "$abilist" > "$out/summary.md"
     echo "::notice title=kodi-android::$abi skipped: the image's ABIs are $abilist"; exit 0 ;;
esac
# Kodi publishes x86 (32-bit) and ARM Android builds, no x86_64; the x86_64 system image runs x86 natively, ARM through
# its translation layer
adb install -r --abi "$abi" "$apk" || adb install -r "$apk"
adb shell pm grant org.xbmc.kodi android.permission.READ_EXTERNAL_STORAGE 2>/dev/null || true
# Android 11+ (scoped storage): only the app itself can create its folder under /sdcard/Android/data (adb's push and
# mkdir fail with "secure_mkdirs failed: Permission denied" otherwise); the shell may write into it once it exists.
# Kodi creates it (files/.kodi) on its first start: start Kodi once, wait for the folder, stop it.
echo "adb shell: $(adb shell id | tr -d '\r')"
adb shell ls -ld /sdcard/Android/data "$dev" 2>&1 | tr -d '\r'
if ! adb shell ls -d "$dev/.kodi" >/dev/null 2>&1; then
  adb shell am start -W -n org.xbmc.kodi/.Splash >/dev/null
  i=0; until adb shell ls -d "$dev/.kodi" >/dev/null 2>&1 || [ "$i" -ge 180 ]; do sleep 3; i=$((i + 3)); done
  sleep 5; adb shell am force-stop org.xbmc.kodi
  echo "Kodi's first start: ${i} s until its folder was there"
fi
adb shell ls -ld "$dev" "$dev/.kodi" 2>&1 | tr -d '\r'
# fatal from here: without the clips on the device no case can play (a failed push still ran every case, about 40 min)
adb shell mkdir -p "$dev/.kodi" "$dev/clips" || { echo "::error title=kodi-android::cannot create folders in $dev (Kodi did not make its folder)"; exit 1; }
adb push "$clips/." "$dev/clips/" >/dev/null || { echo "::error title=kodi-android::cannot push the clips to $dev/clips"; exit 1; }
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
