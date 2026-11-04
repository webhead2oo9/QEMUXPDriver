@echo off
rem Restores the stock d3d9.dll, d3d8.dll and ddraw.dll and removes QD3D from System32.
"%~dp0qd3d_install.exe" uninstall %*
set QD3D_RC=%ERRORLEVEL%
if not defined QD3D_NOPAUSE pause
exit /b %QD3D_RC%
