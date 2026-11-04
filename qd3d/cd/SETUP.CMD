@echo off
rem XPDriver setup inside Windows XP, from this CD:
rem  1. QD3D system-wide (INSTALL\qd3d_install.exe install): Direct3D 1-9 and
rem     DirectDraw for every program, plus the qxpmap mapping driver;
rem  2. the signed XPDriver display package (XPDM\qxpsetup.exe --user): the
rem     display driver, the OpenGL driver and the qxpmap service. It restarts
rem     Windows when it is done.
rem Game folders are never changed; INSTALL\scan.cmd lists per-game DLLs that
rem would bypass the system-wide install.
setlocal
set QD3D_NOPAUSE=1
echo Installing QD3D (Direct3D 1-9 and DirectDraw) system-wide ...
"%~dp0INSTALL\qd3d_install.exe" install
set RC=%errorlevel%
if "%RC%"=="3" (
    echo QD3D completes at the next restart.
) else if not "%RC%"=="0" (
    echo QD3D installation failed ^(code %RC%^): see the messages above.
    pause
    exit /b %RC%
)
if not exist "%~dp0XPDM\qxpsetup.exe" (
    echo This CD carries no display driver package; setup is complete.
    pause
    exit /b 0
)
echo.
echo Installing the XPDriver display driver; Windows restarts when it is done ...
"%~dp0XPDM\qxpsetup.exe" --user
set RC=%errorlevel%
echo Display driver installation failed ^(code %RC%^): see C:\QXPSETUP.LOG.
pause
exit /b %RC%
