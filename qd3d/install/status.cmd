@echo off
rem Shows what is installed: System32 DLLs, saved originals, WFP caches.
"%~dp0qd3d_install.exe" status %*
set QD3D_RC=%ERRORLEVEL%
if not defined QD3D_NOPAUSE pause
exit /b %QD3D_RC%
