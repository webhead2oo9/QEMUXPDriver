@echo off
setlocal
if "%~1"=="" (
  echo Usage: stage-package.cmd WDK_ROOT
  exit /b 2
)
for %%I in ("%~1") do set WDKROOT=%%~sI

call "%~dp0build-ddk.cmd" "%WDKROOT%"
if errorlevel 1 exit /b %errorlevel%

call "%~dp0..\qxpmap\build-ddk.cmd" "%WDKROOT%"
if errorlevel 1 exit /b %errorlevel%

pushd "%~dp0..\.."
wsl make -C guest/xpdm all
if errorlevel 1 (
  popd
  exit /b %errorlevel%
)
wsl python3 guest/xpdm/validate_coinstaller.py guest/xpdm/build/qxpcoin2.dll
if errorlevel 1 (
  popd
  exit /b %errorlevel%
)
wsl make -C guest/qxpicd all coverage
if errorlevel 1 (
  popd
  exit /b %errorlevel%
)
popd

set PACKAGE=%~dp0build\package
if not exist "%PACKAGE%" mkdir "%PACKAGE%"
if errorlevel 1 exit /b %errorlevel%
copy /y "%~dp0qxp.inf" "%PACKAGE%\qxp.inf" >nul
if errorlevel 1 exit /b %errorlevel%
py -3 "%~dp0stamp_inf.py" "%PACKAGE%\qxp.inf"
if errorlevel 1 exit /b %errorlevel%
copy /y "%~dp0qxpmp\objfre_wxp_x86\i386\qxpmp.sys" "%PACKAGE%\qxpmp.sys" >nul
if errorlevel 1 exit /b %errorlevel%
copy /y "%~dp0..\qxpmap\objfre_wxp_x86\i386\qxpmap.sys" "%PACKAGE%\qxpmap.sys" >nul
if errorlevel 1 exit /b %errorlevel%
copy /y "%~dp0qxpdisp\objfre_wxp_x86\i386\qxpdisp.dll" "%PACKAGE%\qxpdisp.dll" >nul
if errorlevel 1 exit /b %errorlevel%
copy /y "%~dp0..\qxpicd\build\qxpicd.dll" "%PACKAGE%\qxpicd.dll" >nul
if errorlevel 1 exit /b %errorlevel%
copy /y "%~dp0build\qxpcoin2.dll" "%PACKAGE%\qxpcoin2.dll" >nul
if errorlevel 1 exit /b %errorlevel%
rem Drop the superseded co-installer from this mutable staging directory only.
if exist "%PACKAGE%\qxpcoinst.dll" del /q "%PACKAGE%\qxpcoinst.dll"
if errorlevel 1 exit /b %errorlevel%
if exist "%PACKAGE%\qxp.cat" del /q "%PACKAGE%\qxp.cat"
if errorlevel 1 exit /b %errorlevel%

"%WDKROOT%\bin\selfsign\Inf2Cat.exe" /driver:"%PACKAGE%" /os:XP_X86 /verbose
if errorlevel 1 exit /b %errorlevel%
if not exist "%PACKAGE%\qxp.cat" exit /b 1

set SIGNING=%~dp0build\signing
if not exist "%SIGNING%" mkdir "%SIGNING%"
rem No labels: cmd.exe can miss GOTO targets in LF-only batch files. Inside
rem the blocks %errorlevel% would expand before the commands run, so a
rem failure exits with 1.
if defined XPD_SIGN_PFX (
  rem Releases: the persistent project certificate, see sign-package.ps1 and
  rem tools\new-signing-cert.ps1.
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0sign-package.ps1" -SignTool "%WDKROOT%\bin\x86\SignTool.exe" -Catalog "%PACKAGE%\qxp.cat" -CertificateOut "%SIGNING%\xpdriver-test.cer"
  if errorlevel 1 exit /b 1
) else (
  rem Development: a throwaway self-signed certificate for every staging run.
  certutil.exe -user -delstore XPDriverDev "XPDriver Development Test" >nul 2>&1
  "%WDKROOT%\bin\x86\MakeCert.exe" -r -pe -a sha1 -n "CN=XPDriver Development Test" -ss XPDriverDev "%SIGNING%\xpdriver-test.cer"
  if errorlevel 1 exit /b 1
  "%WDKROOT%\bin\x86\SignTool.exe" sign /s XPDriverDev /n "XPDriver Development Test" /v "%PACKAGE%\qxp.cat"
  if errorlevel 1 exit /b 1
)
copy /y "%SIGNING%\xpdriver-test.cer" "%PACKAGE%\xpdriver-test.cer" >nul
if errorlevel 1 exit /b %errorlevel%
echo Staged XP driver package: %PACKAGE%
exit /b 0
