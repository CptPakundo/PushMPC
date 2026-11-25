#!/bin/sh
# PushMPC for Kodi: installs inputstream.pushmpc into every Kodi found for this user (Linux: native, Flatpak, Snap;
# macOS), enables it, and leaves a marker other PushMPC installers read (integrations/linux/install.sh then skips its
# Kodi external-player rule, since Kodi now enhances video itself).
#
#   install.sh install | uninstall | status
#
# Environment:
#   PUSHMPC_KODI_DIR   the package folder holding kodi21/inputstream.pushmpc and kodi22/inputstream.pushmpc
#                      (default: the folder of this script)
#   KODI_HOME          one Kodi user folder to use instead of searching (the folder with addons/ and userdata/)
#   KODI_MAJOR         21 or 22, when the Kodi version cannot be detected
# Marker: ${XDG_CONFIG_HOME:-$HOME/.config}/pushmpc/kodi-addon.installed, one line per Kodi: <kodi home><TAB><major>
# Exit codes: 0 done, 1 error, 2 no Kodi found.
set -eu
ID=inputstream.pushmpc
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
pkg=${PUSHMPC_KODI_DIR:-$here}
marker_dir=${XDG_CONFIG_HOME:-$HOME/.config}/pushmpc
marker=$marker_dir/kodi-addon.installed

say() { printf '%s\n' "PushMPC Kodi: $*"; }

homes() {
    if [ -n "${KODI_HOME:-}" ]; then printf '%s\n' "$KODI_HOME"; return; fi
    for h in "$HOME/.kodi" "$HOME/.var/app/tv.kodi.Kodi/data" "$HOME/snap/kodi/common/.kodi" \
             "$HOME/Library/Application Support/Kodi"; do
        [ -d "$h" ] && printf '%s\n' "$h"
    done
    # Kodi installed but never started: its default folder
    if [ "$(uname -s)" = Darwin ]; then
        [ -d /Applications/Kodi.app ] && [ ! -d "$HOME/Library/Application Support/Kodi" ] && printf '%s\n' "$HOME/Library/Application Support/Kodi"
    elif command -v kodi >/dev/null 2>&1 && [ ! -d "$HOME/.kodi" ]; then
        printf '%s\n' "$HOME/.kodi"
    fi
    return 0
}

# the Kodi major for a user folder: KODI_MAJOR, Kodi's own log, the Flatpak or app bundle version, the kodi binary
major_of() {
    h=$1
    if [ -n "${KODI_MAJOR:-}" ]; then printf '%s\n' "$KODI_MAJOR"; return; fi
    for log in "$h/temp/kodi.log" "$h/temp/kodi.old.log" "$h/kodi.log"; do
        if [ -f "$log" ]; then
            m=$(sed -n 's/.*Starting Kodi (\([0-9][0-9]*\)\..*/\1/p' "$log" | head -n 1)
            [ -n "$m" ] && { printf '%s\n' "$m"; return; }
        fi
    done
    case "$h" in
        *"/.var/app/tv.kodi.Kodi/"*)
            m=$(flatpak info tv.kodi.Kodi 2>/dev/null | sed -n 's/^ *Version: *\([0-9][0-9]*\)\..*/\1/p' | head -n 1)
            [ -n "$m" ] && { printf '%s\n' "$m"; return; } ;;
        *"/Library/Application Support/Kodi"*)
            m=$(defaults read /Applications/Kodi.app/Contents/Info CFBundleShortVersionString 2>/dev/null | sed -n 's/^\([0-9][0-9]*\)\..*/\1/p')
            [ -n "$m" ] && { printf '%s\n' "$m"; return; } ;;
    esac
    if command -v kodi >/dev/null 2>&1; then
        m=$(kodi --version 2>/dev/null | sed -n 's/^\([0-9][0-9]*\)\..*/\1/p' | head -n 1)
        [ -n "$m" ] && { printf '%s\n' "$m"; return; }
    fi
    printf '%s\n' ""
}

# Kodi registers add-ons copied in by hand as disabled: switch it on in Kodi's add-on database (when it exists)
enable_in_db() {
    h=$1
    db=$(ls "$h"/userdata/Database/Addons*.db 2>/dev/null | sort | tail -n 1 || true)
    if [ -z "$db" ]; then say "$h: Kodi has not run yet; it asks to enable PushMPC on its first start (Add-ons > My add-ons)"; return; fi
    if command -v python3 >/dev/null 2>&1; then
        python3 - "$db" "$ID" <<'PY'
import sqlite3, sys
con = sqlite3.connect(sys.argv[1]); cur = con.cursor()
cur.execute("SELECT 1 FROM installed WHERE addonID=?", (sys.argv[2],))
if cur.fetchone():
    cur.execute("UPDATE installed SET enabled=1, disabledReason=0 WHERE addonID=?", (sys.argv[2],))
else:
    cur.execute("INSERT INTO installed (addonID, enabled, installDate, origin, disabledReason) VALUES (?, 1, datetime('now'), '', 0)", (sys.argv[2],))
con.commit(); con.close()
PY
    elif command -v sqlite3 >/dev/null 2>&1; then
        sqlite3 "$db" "INSERT OR IGNORE INTO installed (addonID, enabled, installDate, origin, disabledReason) VALUES ('$ID', 1, datetime('now'), '', 0); UPDATE installed SET enabled=1, disabledReason=0 WHERE addonID='$ID';"
    else
        say "$h: no python3 or sqlite3 to enable the add-on; enable it in Kodi (Add-ons > My add-ons > Video player InputStream > PushMPC)"
        return
    fi
    say "$h: enabled"
}

install_one() {
    h=$1
    major=$(major_of "$h")
    if [ -z "$major" ]; then
        major=22
        say "$h: Kodi version not found (set KODI_MAJOR=21 or 22); assuming 22"
    fi
    src=$pkg/kodi$major/$ID
    if [ ! -f "$src/addon.xml" ]; then say "$h: this package has no build for Kodi $major ($src)"; return 1; fi
    mkdir -p "$h/addons"
    rm -rf "$h/addons/$ID.new"
    cp -R "$src" "$h/addons/$ID.new"
    rm -rf "$h/addons/$ID"
    mv "$h/addons/$ID.new" "$h/addons/$ID"
    say "$h: installed for Kodi $major"
    enable_in_db "$h"
    mkdir -p "$marker_dir"
    tmp=$marker.tmp
    { [ -f "$marker" ] && grep -v "^$h	" "$marker" || true; printf '%s\t%s\n' "$h" "$major"; } > "$tmp"
    mv "$tmp" "$marker"
}

uninstall_one() {
    h=$1
    if [ -d "$h/addons/$ID" ]; then rm -rf "$h/addons/$ID"; say "$h: removed"; fi
    if [ -f "$marker" ]; then
        grep -v "^$h	" "$marker" > "$marker.tmp" || true
        if [ -s "$marker.tmp" ]; then mv "$marker.tmp" "$marker"; else rm -f "$marker.tmp" "$marker"; fi
    fi
}

cmd=${1:-}
list=$(homes)
case "$cmd" in
    install)
        [ -n "$list" ] || { say "no Kodi found for this user"; exit 2; }
        rc=0
        printf '%s\n' "$list" | { while IFS= read -r h; do install_one "$h" || rc=1; done; exit $rc; }
        ;;
    uninstall)
        # the Kodi folders it was installed into, plus any found now
        all=$( { [ -f "$marker" ] && cut -f1 "$marker"; printf '%s\n' "$list"; } | awk 'NF && !seen[$0]++')
        [ -n "$all" ] && printf '%s\n' "$all" | while IFS= read -r h; do uninstall_one "$h"; done
        [ -f "$marker" ] && [ ! -s "$marker" ] && rm -f "$marker"
        say "uninstalled"
        ;;
    status)
        if [ -f "$marker" ]; then say "installed:"; cat "$marker"; else say "not installed"; fi
        printf '%s\n' "$list" | while IFS= read -r h; do [ -n "$h" ] && say "found Kodi: $h (Kodi $(major_of "$h"))"; done
        ;;
    *)
        echo "usage: install.sh install|uninstall|status" >&2; exit 1 ;;
esac
