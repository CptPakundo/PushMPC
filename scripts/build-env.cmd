@echo off
rem Shared set-up for the build scripts (filter, updater, installer, vsplugin, build-pushmpc, build-pushfps). CALL it:
rem it sets the caller's environment.
rem   - Visual Studio's x64 build environment. vcvars64.bat takes 1-2 s, so its result is kept in
rem     %TEMP%\pushmpc-vsenv-x64.txt and reused while that Visual Studio's default toolset is unchanged. Skipped when
rem     the shell already is an x64 Visual Studio prompt.
rem   - CMake and Ninja from Visual Studio on PATH.
rem   - A compiler cache when one is installed (ccache, or sccache). PUSHMPC_LAUNCHER then holds the CMake options for it
rem     (an empty launcher without one, so a tree configured with a cache before still builds when it is gone).
rem   - PUSHMPC_PARALLEL: "--parallel N -- [ninja options]" for cmake --build (N = PUSHMPC_JOBS when set, else the
rem     number of logical processors; ninja's load limit -l PUSHMPC_LOAD when set). It ends with "--": a script may
rem     add more ninja options after it.
rem Options (environment):
rem   PUSHMPC_VS              Visual Studio folder (default: the newest with C++, found by vswhere)
rem   PUSHMPC_COMPILER_CACHE  path of ccache.exe or sccache.exe, or "off" (default: ccache, then sccache, from PATH,
rem                           then PUSHMPC_DEPS\ccache-*\ccache.exe)
rem   PUSHMPC_JOBS            parallel compile jobs
rem   PUSHMPC_LOAD            ninja starts no new job while the system load is above this (two builds at once
rem                           throttle each other instead of overloading the machine)
rem   PUSHMPC_BUILD_ENV       a .cmd with your own settings, called first (default: pushmpc-build-env.cmd next to the
rem                           repository folder or inside it, when there is one; it is never committed)
rem   PUSHMPC_CCACHE_BASEDIR  ccache's base_dir for every build (default: the repository for the filter, updater,
rem                           installer and plugin; the folder holding the build tree for build-pushmpc.cmd), so
rem                           checkouts with the same layout share cache hits
rem   CCACHE_DIR, CCACHE_MAXSIZE  ccache's own settings (CCACHE_NOHASHDIR defaults to true here: the debug info's
rem                           build folder does not split the cache between checkouts)
for %%i in ("%~dp0..") do set "PUSHMPC_REPO=%%~fi"
for %%i in ("%PUSHMPC_REPO%\..") do set "PUSHMPC_REPO_PARENT=%%~fi"
if defined PUSHMPC_BUILD_ENV (call "%PUSHMPC_BUILD_ENV%" || exit /b 1) else (
  if exist "%PUSHMPC_REPO_PARENT%\pushmpc-build-env.cmd" call "%PUSHMPC_REPO_PARENT%\pushmpc-build-env.cmd"
  if exist "%PUSHMPC_REPO%\pushmpc-build-env.cmd" call "%PUSHMPC_REPO%\pushmpc-build-env.cmd"
)
if not defined PUSHMPC_DEPS set "PUSHMPC_DEPS=%PUSHMPC_REPO%\deps"

rem ---- Visual Studio
if /i "%VSCMD_ARG_TGT_ARCH%"=="x64" if exist "%VCToolsInstallDir%bin\Hostx64\x64\cl.exe" goto :vs_ready
set "PUSHMPC_VSENV=%TEMP%\pushmpc-vsenv-x64.txt"
set PUSHMPC_VSENV_OK=
if exist "%PUSHMPC_VSENV%" call :vsenv_check
if defined PUSHMPC_VSENV_OK goto :vsenv_load
if not defined PUSHMPC_VS for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "PUSHMPC_VS=%%i"
if not exist "%PUSHMPC_VS%\VC\Auxiliary\Build\vcvars64.bat" (echo build-env: no Visual Studio with C++ found ^(set PUSHMPC_VS^) & exit /b 1)
rem what vcvars64 changes, written once: PATH, INCLUDE, LIB and LIBPATH as prefixes (PFX_<name>), the rest as values
set VSCMD_SKIP_SENDTELEMETRY=1
set "_pm_tmp=%PUSHMPC_VSENV%.%RANDOM%"
set > "%_pm_tmp%.before"
cmd /d /c "call "%PUSHMPC_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul && set" > "%_pm_tmp%.after" || exit /b 1
powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command ^
  "$b = @{}; foreach ($l in (Get-Content -LiteralPath ($env:_pm_tmp + '.before'))) { $i = $l.IndexOf('='); if ($i -gt 0) { $b[$l.Substring(0, $i)] = $l.Substring($i + 1) } };" ^
  "$o = foreach ($l in (Get-Content -LiteralPath ($env:_pm_tmp + '.after'))) { $i = $l.IndexOf('='); if ($i -lt 1) { continue }; $n = $l.Substring(0, $i); $v = $l.Substring($i + 1);" ^
  "  if ($b.ContainsKey($n) -and $b[$n] -ceq $v) { continue };" ^
  "  $k = -1; if (@('PATH','INCLUDE','LIB','LIBPATH') -contains $n.ToUpper() -and $b[$n]) { $k = $v.IndexOf($b[$n]) };" ^
  "  if ($k -ge 0) { $p = $v.Substring(0, $k); $s = $v.Substring($k + $b[$n].Length).Trim(';'); if ($s) { $p = $p.TrimEnd(';') + ';' + $s + ';' };" ^
  "    if ($p.Trim(';')) { 'PFX_' + $n.ToUpper() + '=' + $p.TrimStart(';') } }" ^
  "  elseif ($n -notmatch '^(PROMPT|PUSHMPC_.*|_pm_.*)$') { $n + '=' + $v } };" ^
  "[IO.File]::WriteAllLines($env:_pm_tmp, [string[]]$o); Move-Item -Force $env:_pm_tmp $env:PUSHMPC_VSENV" || exit /b 1
del "%_pm_tmp%.before" "%_pm_tmp%.after" 2>nul
call :vsenv_check
if not defined PUSHMPC_VSENV_OK (echo build-env: vcvars64.bat did not set up the compiler & exit /b 1)
:vsenv_load
for /f "usebackq tokens=1* delims==" %%a in ("%PUSHMPC_VSENV%") do (
  if /i "%%a"=="PFX_PATH" (set "PATH=%%b%PATH%") else if /i "%%a"=="PFX_INCLUDE" (set "INCLUDE=%%b%INCLUDE%") else if /i "%%a"=="PFX_LIB" (set "LIB=%%b%LIB%") else if /i "%%a"=="PFX_LIBPATH" (set "LIBPATH=%%b%LIBPATH%") else set "%%a=%%b"
)
:vs_ready
if not defined PUSHMPC_VS set "PUSHMPC_VS=%VSINSTALLDIR:~0,-1%"
set "PATH=%PUSHMPC_VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PUSHMPC_VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"

rem ---- compiler cache
set "PUSHMPC_LAUNCHER=-DCMAKE_C_COMPILER_LAUNCHER= -DCMAKE_CXX_COMPILER_LAUNCHER="
if /i "%PUSHMPC_COMPILER_CACHE%"=="off" goto :jobs
if not defined PUSHMPC_COMPILER_CACHE for %%i in (ccache.exe sccache.exe) do if not defined PUSHMPC_COMPILER_CACHE if not "%%~$PATH:i"=="" set "PUSHMPC_COMPILER_CACHE=%%~$PATH:i"
if not defined PUSHMPC_COMPILER_CACHE for /d %%d in ("%PUSHMPC_DEPS%\ccache-*") do if exist "%%~fd\ccache.exe" set "PUSHMPC_COMPILER_CACHE=%%~fd\ccache.exe"
if not defined PUSHMPC_COMPILER_CACHE goto :jobs
if not exist "%PUSHMPC_COMPILER_CACHE%" (echo build-env: PUSHMPC_COMPILER_CACHE=%PUSHMPC_COMPILER_CACHE% does not exist: building without a compiler cache & set PUSHMPC_COMPILER_CACHE=& goto :jobs)
set "PUSHMPC_LAUNCHER=-DCMAKE_C_COMPILER_LAUNCHER=%PUSHMPC_COMPILER_CACHE:\=/% -DCMAKE_CXX_COMPILER_LAUNCHER=%PUSHMPC_COMPILER_CACHE:\=/%"
if defined PUSHMPC_CCACHE_BASEDIR (set "CCACHE_BASEDIR=%PUSHMPC_CCACHE_BASEDIR%") else set "CCACHE_BASEDIR=%PUSHMPC_REPO%"
if not defined CCACHE_HASHDIR if not defined CCACHE_NOHASHDIR set CCACHE_NOHASHDIR=true
:jobs
set "PUSHMPC_PARALLEL=--parallel %NUMBER_OF_PROCESSORS% --"
if defined PUSHMPC_JOBS set "PUSHMPC_PARALLEL=--parallel %PUSHMPC_JOBS% --"
if defined PUSHMPC_LOAD set "PUSHMPC_PARALLEL=%PUSHMPC_PARALLEL% -l %PUSHMPC_LOAD%"
exit /b 0

rem the saved environment is usable when it names this Visual Studio and its default toolset is still installed
:vsenv_check
set PUSHMPC_VSENV_OK=
set _pm_vs=
set _pm_tv=
set _pm_dv=
set _pm_vc=
for /f "usebackq tokens=1* delims==" %%a in (`findstr /b /i "VSINSTALLDIR= VCToolsVersion= VCINSTALLDIR=" "%PUSHMPC_VSENV%"`) do (
  if /i "%%a"=="VSINSTALLDIR" set "_pm_vs=%%b"
  if /i "%%a"=="VCToolsVersion" set "_pm_tv=%%b"
  if /i "%%a"=="VCINSTALLDIR" set "_pm_vc=%%b"
)
if not defined _pm_vs exit /b 0
if defined PUSHMPC_VS if /i not "%PUSHMPC_VS%\"=="%_pm_vs%" exit /b 0
if not exist "%_pm_vc%Auxiliary\Build\Microsoft.VCToolsVersion.default.txt" exit /b 0
set /p _pm_dv=<"%_pm_vc%Auxiliary\Build\Microsoft.VCToolsVersion.default.txt"
for /f "tokens=* delims= " %%v in ("%_pm_dv%") do set "_pm_dv=%%v"
if "%_pm_dv%"=="%_pm_tv%" if exist "%_pm_vc%Tools\MSVC\%_pm_tv%\bin\Hostx64\x64\cl.exe" set PUSHMPC_VSENV_OK=1
exit /b 0
