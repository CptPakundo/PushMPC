# install.ps1 round trip on Windows with a fake %APPDATA%: a Kodi 22 user folder (log + add-on database made with
# winsqlite3); install enables it and writes the marker, uninstall removes it.
param([Parameter(Mandatory)][string]$Package)
$ErrorActionPreference = 'Stop'
$fail = 0
function Check($name, $ok) { if ($ok) { Write-Host "ok   $name" } else { Write-Host "FAIL $name"; $script:fail = 1 } }
$root = Join-Path ([IO.Path]::GetTempPath()) ("pmk" + [guid]::NewGuid().ToString('N'))
$env:APPDATA = $root
$h = Join-Path $root 'Kodi'
New-Item -ItemType Directory -Force "$h\userdata\Database" | Out-Null
Set-Content "$h\kodi.log" '2026-11-25 10:00:00.000 T:1 info <general>: Starting Kodi (22.0-RC2 (21.95.802) Git:20261101-def). Platform: Windows NT x86 64-bit'
Add-Type -Namespace PmTest -Name Sql -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("winsqlite3.dll", EntryPoint = "sqlite3_open16", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern int Open(string path, out System.IntPtr db);
[System.Runtime.InteropServices.DllImport("winsqlite3.dll", EntryPoint = "sqlite3_exec")]
public static extern int Exec(System.IntPtr db, byte[] sql, System.IntPtr cb, System.IntPtr arg, out System.IntPtr err);
[System.Runtime.InteropServices.DllImport("winsqlite3.dll", EntryPoint = "sqlite3_prepare16_v2", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern int Prepare(System.IntPtr db, string sql, int n, out System.IntPtr st, System.IntPtr tail);
[System.Runtime.InteropServices.DllImport("winsqlite3.dll", EntryPoint = "sqlite3_step")]
public static extern int Step(System.IntPtr st);
[System.Runtime.InteropServices.DllImport("winsqlite3.dll", EntryPoint = "sqlite3_column_int")]
public static extern int ColumnInt(System.IntPtr st, int i);
[System.Runtime.InteropServices.DllImport("winsqlite3.dll", EntryPoint = "sqlite3_finalize")]
public static extern int Finalize(System.IntPtr st);
[System.Runtime.InteropServices.DllImport("winsqlite3.dll", EntryPoint = "sqlite3_close")]
public static extern int Close(System.IntPtr db);
'@
$db = "$h\userdata\Database\Addons33.db"
$p = [IntPtr]::Zero; $e = [IntPtr]::Zero
[void][PmTest.Sql]::Open($db, [ref]$p)
$sql = "CREATE TABLE installed (id INTEGER PRIMARY KEY, addonID TEXT UNIQUE, enabled BOOLEAN, installDate TEXT, lastUpdated TEXT, lastUsed TEXT, origin TEXT NOT NULL DEFAULT '', disabledReason INTEGER NOT NULL DEFAULT 0);"
[void][PmTest.Sql]::Exec($p, [Text.Encoding]::UTF8.GetBytes($sql + [char]0), [IntPtr]::Zero, [IntPtr]::Zero, [ref]$e)
[void][PmTest.Sql]::Close($p)
$env:PUSHMPC_KODI_DIR = $Package
& powershell -NoProfile -ExecutionPolicy Bypass -File "$Package\install.ps1" install
Check 'Kodi 22 build installed' (Select-String -Path "$h\addons\inputstream.pushmpc\addon.xml" -Pattern 'kodi.binary.instance.inputstream" minversion="3.4' -Quiet)
[void][PmTest.Sql]::Open($db, [ref]$p)
$st = [IntPtr]::Zero
[void][PmTest.Sql]::Prepare($p, "SELECT enabled FROM installed WHERE addonID='inputstream.pushmpc'", -1, [ref]$st, [IntPtr]::Zero)
$en = if ([PmTest.Sql]::Step($st) -eq 100) { [PmTest.Sql]::ColumnInt($st, 0) } else { -1 }
[void][PmTest.Sql]::Finalize($st); [void][PmTest.Sql]::Close($p)
Check 'enabled in the database' ($en -eq 1)
Check 'marker written' ((Get-Content "$root\PushMPC\kodi-addon.installed") -match 'Kodi\t22')
& powershell -NoProfile -ExecutionPolicy Bypass -File "$Package\install.ps1" uninstall
Check 'removed' (-not (Test-Path "$h\addons\inputstream.pushmpc"))
Check 'marker removed' (-not (Test-Path "$root\PushMPC\kodi-addon.installed"))
exit $fail
