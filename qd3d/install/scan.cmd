@echo off
rem Lists per-game wrapper DLLs that games load instead of the system-wide ones.
rem Add --move-leftovers to rename them to NAME.qd3d-bak, --restore-leftovers
rem to rename them back.
"%~dp0qd3d_install.exe" scan %*
set QD3D_RC=%ERRORLEVEL%
if not defined QD3D_NOPAUSE pause
exit /b %QD3D_RC%
