@echo off
rem Installs or updates QD3D (d3d9.dll, d3d8.dll, ddraw.dll beside this file) in System32.
"%~dp0qd3d_install.exe" install %*
set QD3D_RC=%ERRORLEVEL%
if not defined QD3D_NOPAUSE pause
exit /b %QD3D_RC%
