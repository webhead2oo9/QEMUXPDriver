@echo off
rem System-wide QD3D install test inside XP, logged to C:\QD3D\syswide.txt.
rem   SYSWIDE            status, leftover scan (read-only, Desktop included),
rem                      install, then status again after 90 s, past the
rem                      one-minute Windows File Protection exception
rem   SYSWIDE uninstall  restore the stock DLLs, then status
rem   SYSWIDE status     status only
setlocal
set QD3D_NOPAUSE=1
set INST="%~dp0INSTALL\qd3d_install.exe"
if not exist C:\QD3D mkdir C:\QD3D
set OUT=C:\QD3D\syswide.txt
set RUN=%TEMP%\qd3d-syswide.txt
set MODE=%~1
if "%MODE%"=="" set MODE=install
if /i not "%MODE%"=="install" if /i not "%MODE%"=="uninstall" if /i not "%MODE%"=="status" (
    echo usage: SYSWIDE [install^|uninstall^|status]
    exit /b 1
)
echo ==== SYSWIDE %MODE% %DATE% %TIME% ==== > "%RUN%"
goto %MODE%

:install
call :run status
call :run scan --scan-dir "%USERPROFILE%\Desktop"
call :run install
echo Waiting 90 seconds, past Windows File Protection's exception window ...
ping -n 91 127.0.0.1 >nul
call :run status
goto done

:uninstall
call :run uninstall
call :run status
goto done

:status
call :run status
goto done

:run
echo --- qd3d_install %* --- >> "%RUN%"
%INST% %* >> "%RUN%" 2>&1
echo exit %errorlevel% >> "%RUN%"
exit /b 0

:done
type "%RUN%" >> "%OUT%"
type "%RUN%"
del "%RUN%"
pause
