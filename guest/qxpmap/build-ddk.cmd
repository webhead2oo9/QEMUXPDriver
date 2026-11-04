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
if not exist "%~1\bin\setenv.bat" (
  echo WDK setenv.bat is missing: "%~1\bin\setenv.bat"
  exit /b 2
)
py -3 "%~dp0..\..\tools\version.py" --header "%~dp0..\..\build\version\xpdriver_version.h"
if errorlevel 1 exit /b %errorlevel%
for %%I in ("%~1") do set WDKROOT=%%~sI
call %WDKROOT%\bin\setenv.bat %WDKROOT% fre WXP no_oacr
if errorlevel 1 exit /b %errorlevel%
cd /d "%~dp0"
rem Delete only this build's prior diagnostics, never accept stale logs.
for %%F in (buildfre_wxp_x86.err buildfre_wxp_x86.wrn) do if exist "%%F" del /q "%%F"
build.exe -cZ
set BUILDRESULT=%errorlevel%
for %%F in (buildfre_wxp_x86.err buildfre_wxp_x86.wrn) do if exist "%%F" if not "%%~zF"=="0" (
  type "%%F"
  echo qxpmap: nonempty error or warning log is a build failure.
  exit /b 1
)
if not "%BUILDRESULT%"=="0" exit /b %BUILDRESULT%
py -3 "%~dp0..\xpdm\validate_ddk_pe.py" --native-driver "%~dp0objfre_wxp_x86\i386\qxpmap.sys"
exit /b %errorlevel%
