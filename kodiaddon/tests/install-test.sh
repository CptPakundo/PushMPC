#!/bin/sh
# install.sh round trip in a fake HOME: a native Kodi 21 and a Flatpak Kodi 22 (each with Kodi's log and add-on
# database), install picks the right build per Kodi, enables it, writes the marker; uninstall removes all of it.
# usage: install-test.sh <package dir with kodi21/ kodi22/ install.sh>
set -eu
pkg=$1
fail=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }
mkdb() {
    mkdir -p "$1/userdata/Database"
    python3 - "$1/userdata/Database/Addons33.db" <<'PY'
import sqlite3, sys
con = sqlite3.connect(sys.argv[1])
con.execute("CREATE TABLE installed (id INTEGER PRIMARY KEY, addonID TEXT UNIQUE, enabled BOOLEAN, installDate TEXT, lastUpdated TEXT, lastUsed TEXT, origin TEXT NOT NULL DEFAULT '', disabledReason INTEGER NOT NULL DEFAULT 0)")
con.execute("INSERT INTO installed (addonID, enabled, origin) VALUES ('inputstream.pushmpc', 0, '')")
con.commit()
PY
}
enabled() { python3 -c "import sqlite3,sys; print(sqlite3.connect(sys.argv[1]).execute(\"SELECT enabled FROM installed WHERE addonID='inputstream.pushmpc'\").fetchone()[0])" "$1/userdata/Database/Addons33.db"; }
H=$(mktemp -d)
native=$H/.kodi; flat=$H/.var/app/tv.kodi.Kodi/data
mkdir -p "$native/temp" "$flat/temp"
echo "2026-11-25 10:00:00.000 T:1 info <general>: Starting Kodi (21.3 (21.3.0) Git:20251019-abc). Platform: Linux x86 64-bit" > "$native/temp/kodi.log"
echo "2026-11-25 10:00:00.000 T:1 info <general>: Starting Kodi (22.0-RC2 (21.95.802) Git:20261101-def). Platform: Linux x86 64-bit" > "$flat/temp/kodi.log"
mkdb "$native"; mkdb "$flat"
HOME=$H XDG_CONFIG_HOME=$H/.config PUSHMPC_KODI_DIR=$pkg sh "$pkg/install.sh" install
check "native Kodi 21 got the Kodi 21 build" "grep -q 'kodi.binary.instance.inputstream\" minversion=\"3.3' '$native/addons/inputstream.pushmpc/addon.xml'"
check "Flatpak Kodi 22 got the Kodi 22 build" "grep -q 'kodi.binary.instance.inputstream\" minversion=\"3.4' '$flat/addons/inputstream.pushmpc/addon.xml'"
check "enabled in Kodi 21's database" "[ \"\$(enabled '$native')\" = 1 ]"
check "enabled in Kodi 22's database" "[ \"\$(enabled '$flat')\" = 1 ]"
check "marker lists both" "[ \"\$(wc -l < '$H/.config/pushmpc/kodi-addon.installed')\" -eq 2 ]"
HOME=$H XDG_CONFIG_HOME=$H/.config PUSHMPC_KODI_DIR=$pkg sh "$pkg/install.sh" install >/dev/null
check "reinstall keeps one marker line per Kodi" "[ \"\$(wc -l < '$H/.config/pushmpc/kodi-addon.installed')\" -eq 2 ]"
HOME=$H XDG_CONFIG_HOME=$H/.config PUSHMPC_KODI_DIR=$pkg sh "$pkg/install.sh" status
HOME=$H XDG_CONFIG_HOME=$H/.config PUSHMPC_KODI_DIR=$pkg sh "$pkg/install.sh" uninstall
check "removed from both" "[ ! -e '$native/addons/inputstream.pushmpc' ] && [ ! -e '$flat/addons/inputstream.pushmpc' ]"
check "marker removed" "[ ! -e '$H/.config/pushmpc/kodi-addon.installed' ]"
E=$(mktemp -d)
set +e; HOME=$E XDG_CONFIG_HOME=$E/.config PATH=/usr/bin:/bin sh "$pkg/install.sh" install >/dev/null 2>&1; rc=$?; set -e
check "no Kodi: exit 2" "[ $rc -eq 2 ]"
exit $fail
