<#
  PushMPC for Kodi (Windows): installs inputstream.pushmpc into Kodi for this user (installed Kodi: %APPDATA%\Kodi;
  portable Kodi: pass its portable_data folder as KODI_HOME), enables it, and leaves a marker other PushMPC installers
  read. The Windows Setup calls it.

    install.ps1 install | uninstall | status

  Environment:
    PUSHMPC_KODI_DIR   the package folder holding kodi21\inputstream.pushmpc and kodi22\inputstream.pushmpc
                       (default: the folder of this script)
    KODI_HOME          one Kodi user folder (holding addons\ and userdata\) instead of %APPDATA%\Kodi
    KODI_MAJOR         21 or 22, when the Kodi version cannot be detected
  Marker: %APPDATA%\PushMPC\kodi-addon.installed, one line per Kodi: <kodi home><TAB><major>
  Exit codes: 0 done, 1 error, 2 no Kodi found.
#>
param([Parameter(Position = 0)][string]$Command = '')
$ErrorActionPreference = 'Stop'
$Id = 'inputstream.pushmpc'
$pkg = if ($env:PUSHMPC_KODI_DIR) { $env:PUSHMPC_KODI_DIR } else { $PSScriptRoot }
$markerDir = Join-Path $env:APPDATA 'PushMPC'
$marker = Join-Path $markerDir 'kodi-addon.installed'
function Say($m) { Write-Host "PushMPC Kodi: $m" }

function Get-KodiHomes {
    if ($env:KODI_HOME) { return @($env:KODI_HOME) }
    $h = Join-Path $env:APPDATA 'Kodi'
    $exe = @("$env:ProgramFiles\Kodi\kodi.exe", "${env:ProgramFiles(x86)}\Kodi\kodi.exe") | Where-Object { $_ -and (Test-Path $_) }
    if ((Test-Path $h) -or $exe) { return @($h) }
    return @()
}

function Get-KodiMajor($h) {
    if ($env:KODI_MAJOR) { return [int]$env:KODI_MAJOR }
    foreach ($log in @("$h\kodi.log", "$h\kodi.old.log", "$h\temp\kodi.log")) {
        if (Test-Path $log) {
            $m = Select-String -Path $log -Pattern 'Starting Kodi \((\d+)\.' -List | Select-Object -First 1
            if ($m) { return [int]$m.Matches[0].Groups[1].Value }
        }
    }
    foreach ($exe in @("$env:ProgramFiles\Kodi\kodi.exe", "${env:ProgramFiles(x86)}\Kodi\kodi.exe", (Join-Path (Split-Path -Parent $h) 'kodi.exe'))) {
        if ($exe -and (Test-Path $exe)) { $v = (Get-Item $exe).VersionInfo.ProductMajorPart; if ($v -ge 19) { return [int]$v } }
    }
    return 0
}

# Kodi registers add-ons copied in by hand as disabled: switch it on in Kodi's add-on database, through Windows' own
# SQLite (winsqlite3.dll, Windows 10 and later)
Add-Type -Namespace PushMpc -Name Sqlite -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("winsqlite3.dll", EntryPoint = "sqlite3_open16", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern int Open(string path, out System.IntPtr db);
[System.Runtime.InteropServices.DllImport("winsqlite3.dll", EntryPoint = "sqlite3_exec")]
public static extern int Exec(System.IntPtr db, byte[] sql, System.IntPtr cb, System.IntPtr arg, out System.IntPtr err);
[System.Runtime.InteropServices.DllImport("winsqlite3.dll", EntryPoint = "sqlite3_close")]
public static extern int Close(System.IntPtr db);
'@

function Enable-InDb($h) {
    $db = Get-ChildItem -Path "$h\userdata\Database" -Filter 'Addons*.db' -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1
    if (-not $db) { Say "${h}: Kodi has not run yet; it asks to enable PushMPC on its first start (Add-ons > My add-ons)"; return }
    $p = [IntPtr]::Zero; $e = [IntPtr]::Zero
    if ([PushMpc.Sqlite]::Open($db.FullName, [ref]$p) -ne 0) { Say "${h}: cannot open $($db.Name); enable PushMPC in Kodi"; return }
    $sql = "INSERT OR IGNORE INTO installed (addonID, enabled, installDate, origin, disabledReason) VALUES ('$Id', 1, datetime('now'), '', 0); " +
           "UPDATE installed SET enabled=1, disabledReason=0 WHERE addonID='$Id';"
    $rc = [PushMpc.Sqlite]::Exec($p, [Text.Encoding]::UTF8.GetBytes($sql + [char]0), [IntPtr]::Zero, [IntPtr]::Zero, [ref]$e)
    [void][PushMpc.Sqlite]::Close($p)
    if ($rc -eq 0) { Say "${h}: enabled" } else { Say "${h}: could not enable (sqlite $rc); enable PushMPC in Kodi" }
}

function Read-Marker { if (Test-Path $marker) { return @(Get-Content $marker | Where-Object { $_ }) } return @() }
function Write-Marker($lines) {
    if ($lines.Count -eq 0) { Remove-Item $marker -ErrorAction SilentlyContinue; return }
    New-Item -ItemType Directory -Force $markerDir | Out-Null
    Set-Content -Path $marker -Value $lines -Encoding UTF8
}

function Install-One($h) {
    $major = Get-KodiMajor $h
    if ($major -eq 0) { $major = 22; Say "${h}: Kodi version not found (set KODI_MAJOR=21 or 22); assuming 22" }
    $src = Join-Path $pkg "kodi$major\$Id"
    if (-not (Test-Path "$src\addon.xml")) { Say "${h}: this package has no build for Kodi $major ($src)"; return $false }
    $dst = Join-Path $h "addons\$Id"
    New-Item -ItemType Directory -Force (Join-Path $h 'addons') | Out-Null
    if (Test-Path "$dst.new") { Remove-Item -Recurse -Force "$dst.new" }
    Copy-Item -Recurse $src "$dst.new"
    if (Test-Path $dst) { Remove-Item -Recurse -Force $dst }
    Rename-Item "$dst.new" $Id
    Say "${h}: installed for Kodi $major"
    Enable-InDb $h
    $lines = @(Read-Marker | Where-Object { -not $_.StartsWith("$h`t") }) + "$h`t$major"
    Write-Marker $lines
    return $true
}

$homes = @(Get-KodiHomes)
switch ($Command) {
    'install' {
        if ($homes.Count -eq 0) { Say 'no Kodi found for this user'; exit 2 }
        $ok = $true
        foreach ($h in $homes) { if (-not (Install-One $h)) { $ok = $false } }
        exit ($(if ($ok) { 0 } else { 1 }))
    }
    'uninstall' {
        $all = @(@(Read-Marker | ForEach-Object { $_.Split("`t")[0] }) + $homes | Select-Object -Unique)
        foreach ($h in $all) {
            $dst = Join-Path $h "addons\$Id"
            if (Test-Path $dst) { Remove-Item -Recurse -Force $dst; Say "${h}: removed" }
        }
        Write-Marker @()
        Say 'uninstalled'
    }
    'status' {
        $m = Read-Marker
        if ($m.Count) { Say 'installed:'; $m | ForEach-Object { Write-Host $_ } } else { Say 'not installed' }
        foreach ($h in $homes) { Say "found Kodi: $h (Kodi $(Get-KodiMajor $h))" }
    }
    default { Write-Host 'usage: install.ps1 install|uninstall|status'; exit 1 }
}
