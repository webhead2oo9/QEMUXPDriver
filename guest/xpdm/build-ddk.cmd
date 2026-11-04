@echo off
setlocal
rem The 32-bit WDK build.exe cannot see System32\OpenSSH (WOW64 redirects
rem System32 to SysWOW64), so it reports that PATH entry as missing and
rem counts a build warning. Leave it out of this build's PATH.
set "PATH=%PATH:C:\WINDOWS\System32\OpenSSH\;=%"
if "%~1"=="" (
  echo Usage: build-ddk.cmd WDK_ROOT
  exit /b 2
)
py -3 "%~dp0..\..\tools\version.py" --header "%~dp0..\..\build\version\xpdriver_version.h"
if errorlevel 1 exit /b %errorlevel%
for %%I in ("%~1") do set WDKROOT=%%~sI
call %WDKROOT%\bin\setenv.bat %WDKROOT% fre WXP no_oacr
if errorlevel 1 exit /b %errorlevel%
cd /d "%~dp0ddk_probe"
build.exe -cZ
if errorlevel 1 exit /b %errorlevel%

py -3 "%~dp0validate_ddk_pe.py" "%~dp0ddk_probe\objfre_wxp_x86\i386\qxpesc.dll"
if errorlevel 1 exit /b %errorlevel%

cd /d "%~dp0qxpdisp"
build.exe -cZ
if errorlevel 1 exit /b %errorlevel%
py -3 "%~dp0validate_ddk_pe.py" --allow-imports "%~dp0qxpdisp\objfre_wxp_x86\i386\qxpdisp.dll"
if errorlevel 1 exit /b %errorlevel%

cd /d "%~dp0qxpmp"
build.exe -cZ
if errorlevel 1 exit /b %errorlevel%
py -3 "%~dp0validate_ddk_pe.py" --kernel "%~dp0qxpmp\objfre_wxp_x86\i386\qxpmp.sys"
if errorlevel 1 exit /b %errorlevel%

set CHKOUT=%~dp0ddk_probe\objfre_wxp_x86\chkinf
if not exist "%CHKOUT%" mkdir "%CHKOUT%"
rem Delete any previous report first.  chkinf.bat exits 0 even when its Perl
rem aborts, so a surviving report from an earlier run would be silently
rem revalidated and reported as a fresh pass.
if exist "%CHKOUT%\summary.htm" del /q "%CHKOUT%\summary.htm"
rem ChkINF ships its own Perl 5.6.1.  chkinf.bat only appends its directory to
rem PATH, so any modern Perl earlier in PATH wins and fails on the legacy
rem defined(%%hash) syntax in DISPLAY.pm.  Prepend the bundled interpreter.
set PATH=%WDKROOT%\tools\Chkinf;%PATH%
call %WDKROOT%\tools\Chkinf\chkinf.bat "%~dp0qxp.inf" /A /O "%CHKOUT%"
if errorlevel 1 exit /b %errorlevel%
if not exist "%CHKOUT%\summary.htm" (
    echo ChkINF produced no summary report; treating as failure.
    exit /b 1
)
py -3 "%~dp0validate_chkinf.py" "%CHKOUT%\summary.htm"
exit /b %errorlevel%
