#!/usr/bin/env bash
# Kodi for Android in the emulator (kodi-addon.yml, job kodi-android, inside reactivecircus/android-emulator-runner):
# installs Kodi's APK, gives it all-files access, moves Kodi's data folder to the shared storage (xbmc_env.properties
# xbmc.data, Kodi's own setting for it), puts the clips there and plays the cases with run_cases.py through
# android-kodi.sh; the add-on goes into Kodi's home the way a zip install puts it.
#   android-test.sh <kodi apk> <add-on dir> <clips dir> <out dir> <kodi major> <abi: x86|arm64-v8a> [cases]
# arm64-v8a on the x86_64 image runs through the image's ARM translation; without it the run is skipped (exit 0, a
# note in the summary)
set -uo pipefail
apk=$1; addon=$2; clips=$3; out=$4; major=$5; abi=$6; cases=${7:-}
here=$(cd "$(dirname "$0")" && pwd)
# Kodi's data folder on the shared storage, where the adb shell may write: Android 11 keeps the shell out of
# /sdcard/Android/data/<app> ("secure_mkdirs failed", and once Kodi made it, "ls: Permission denied": run 45956597886)
dev=/storage/emulated/0/kodi-ci
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
# Kodi's Splash (21 / 22, Splash.java.in CheckPermissions) wants "All files access" on Android 11+ phones
# (Environment.isExternalStorageManager(), API >= 30 and not Android TV); without it a first start stops on its Info
# dialog and waits for a tap, so Kodi never starts (run 45953088584: Splash shown, no further Kodi line in 180 s).
# Granted here as a user would in Settings; RECORD_AUDIO too (the Splash asks for it on the same path).
adb shell appops set --uid org.xbmc.kodi MANAGE_EXTERNAL_STORAGE allow || adb shell appops set org.xbmc.kodi MANAGE_EXTERNAL_STORAGE allow
adb shell pm grant org.xbmc.kodi android.permission.RECORD_AUDIO 2>/dev/null || true
echo "Kodi's all-files access: $(adb shell appops get org.xbmc.kodi MANAGE_EXTERNAL_STORAGE | tr -d '\r')"
# Kodi reads /sdcard/xbmc_env.properties at start (XBMCProperties); xbmc.data moves its data folder (special://home
# = <xbmc.data>/.kodi). Kodi itself makes .kodi there on its first start (it may write anywhere on the shared storage
# with all-files access): start it once, wait for userdata, stop it.
fail() { echo "::error title=kodi-android::$1"; adb logcat -d > "$out/logcat.txt" 2>&1 || true
         adb exec-out screencap -p > "$out/screen.png" 2>/dev/null || true; exit 1; }
echo "adb shell: $(adb shell id | tr -d '\r')"
adb shell mkdir -p "$dev/clips" || fail "cannot create $dev on the shared storage"
adb shell "echo xbmc.data=$dev > /sdcard/xbmc_env.properties" || fail "cannot write /sdcard/xbmc_env.properties"
adb shell cat /sdcard/xbmc_env.properties | tr -d '\r'
adb shell am start -W -n org.xbmc.kodi/.Splash >/dev/null
i=0; until adb shell ls -d "$dev/.kodi/userdata" >/dev/null 2>&1 || [ "$i" -ge 240 ]; do sleep 3; i=$((i + 3)); done
if ! adb shell ls -d "$dev/.kodi/userdata" >/dev/null 2>&1; then   # what Kodi shows while it waits (before it is stopped)
  adb exec-out screencap -p > "$out/first-start.png" 2>/dev/null || true
  adb shell uiautomator dump /sdcard/ui.xml >/dev/null 2>&1 && adb shell cat /sdcard/ui.xml | grep -o 'text="[^"]*"' | grep -v 'text=""' | head -20
  adb shell ls -la "$dev" 2>&1 | tr -d '\r'
  fail "Kodi did not make its home in $dev/.kodi in 240 s (xbmc.data not taken? see logcat.txt, first-start.png)"
fi
sleep 5; adb shell am force-stop org.xbmc.kodi
echo "Kodi's first start: ${i} s until $dev/.kodi/userdata was there"
adb shell ls -la "$dev/.kodi" 2>&1 | tr -d '\r'
# fatal from here: without the clips on the device no case can play (a failed push still ran every case, about 40 min)
adb push "$clips/." "$dev/clips/" >/dev/null || fail "cannot push the clips to $dev/clips"
adb shell ls -l "$dev/clips"
before="adb shell mkdir -p $dev/.kodi && adb push '$home/.' '$dev/.kodi/' >/dev/null"
after="mkdir -p '$home/userdata' '$home/temp'; adb pull '$dev/.kodi/userdata/Database' '$home/userdata/' >/dev/null 2>&1; adb pull '$dev/.kodi/temp/kodi.log' '$home/temp/kodi.log' >/dev/null 2>&1; adb shell ls '$dev/.kodi/userdata/addon_data/inputstream.pushmpc'; adb shell cat '$dev/.kodi/userdata/addon_data/inputstream.pushmpc/settings.xml' 2>&1 | grep -h enabled; true"
export PM_KODI_MAX=100
# the CPU engine on 2 of the emulator's 4 cores: the emulator draws Kodi's GL on the same CPUs (SwiftShader), which a
# phone does on its GPU. Kodi 22 alone renders 47.6 of 48/s here (native48, run 45976736780, 0 dropped); with the engine
# on all 4 cores the x2 case's render loop fell to 42.9-51.9/s and Kodi dropped 14-38 pictures (40.4 and 40.7 of the
# 40.8/s needed in runs 45965854931 and 45971594219; a pass at 37.2 of 36.5 only because the loop fell). On 2 threads
# the engine still keeps its budget (19.9 of 25 ms, run 45960712431)
# The emulator's speed drifts during a job (kodi-android job 140938256053: the first cases at a 56/s render loop, the last ones at 50/s, Kodi alone 21.1/s with
# no PushMPC in the path): Kodi alone ("disabled") plays first and again last, and the render check of every other case
# is judged against the slower of the two (run_cases.py --baseline-case, analyze.py --baseline). A start without a file
# on the baseline's settings comes first (--settle-baseline): the first baseline of job 140985967927 played through the
# add-on although the add-on logged itself off, the same case last did not
python3 "$here/run_cases.py" --kodi "sh $here/android-kodi.sh" --kill "adb shell am force-stop org.xbmc.kodi" \
  --before "$before" --after "$after" --first 90 --player-clips "$dev/clips" --threads 2 --baseline-case disabled --settle-baseline \
  --home "$home" --addon "$addon" --clips "$clips" --major "$major" --out "$out" --cases "$cases"
rc=$?
adb logcat -d > "$out/logcat.txt" 2>&1 || true
exit $rc
