@echo off
rem XPDriver tests inside XP: copies the proxies and tests to C:\QD3D, runs the
rem qxpmap driver probe (it claims every QD3D slot, so it runs first, on an
rem idle VM), the OpenGL ICD's direct test (C:\QXPICD, the ICD beside it), then
rem the QD3D smoke tests, and shows C:\QD3D\results.txt. The last two runs use
rem exclusive fullscreen (the display mode changes briefly).
setlocal
if not exist C:\QD3D mkdir C:\QD3D
copy /Y "%~dp0QD3D\*.*" C:\QD3D\ >nul
cd /d C:\QD3D
echo === qxpmap driver probe === > results.txt
qxpmap_probe.exe >> results.txt 2>&1
echo exit %errorlevel% >> results.txt
echo. >> results.txt
echo === OpenGL ICD direct test (qxpicd.dll beside qxptest.exe) === >> results.txt
if not exist C:\QXPICD mkdir C:\QXPICD
copy /Y "%~dp0QXPICD\*.*" C:\QXPICD\ >nul
if exist C:\QXPICD-TEST.LOG del C:\QXPICD-TEST.LOG
pushd C:\QXPICD
qxptest.exe >> C:\QD3D\results.txt 2>&1
echo exit %errorlevel% >> C:\QD3D\results.txt
popd
if exist C:\QXPICD-TEST.LOG type C:\QXPICD-TEST.LOG >> results.txt
echo. >> results.txt
echo === Direct3D 9 smoke (QD3D proxy) === >> results.txt
qd3d9_smoke.exe >> results.txt 2>&1
echo exit %errorlevel% >> results.txt
echo. >> results.txt
echo === Direct3D 8 smoke (front end + QD3D proxy) === >> results.txt
qd3d8_smoke.exe >> results.txt 2>&1
echo exit %errorlevel% >> results.txt
echo. >> results.txt
echo === DirectDraw smoke, windowed (front end + QD3D proxy) === >> results.txt
qdd_smoke.exe --expect-local >> results.txt 2>&1
echo exit %errorlevel% >> results.txt
echo. >> results.txt
echo === Direct3D 7 smoke, windowed (front end + QD3D proxy) === >> results.txt
qdd_d3d7_smoke.exe --expect-local >> results.txt 2>&1
echo exit %errorlevel% >> results.txt
echo. >> results.txt
echo === Direct3D 1-6 smoke, windowed (front end + QD3D proxy) === >> results.txt
qdd_d3d7_legacy_smoke.exe --expect-local >> results.txt 2>&1
echo exit %errorlevel% >> results.txt
echo. >> results.txt
echo === DirectDraw smoke, fullscreen === >> results.txt
qdd_smoke.exe --expect-local --fullscreen >> results.txt 2>&1
echo exit %errorlevel% >> results.txt
echo. >> results.txt
echo === Direct3D 7 smoke, fullscreen === >> results.txt
qdd_d3d7_smoke.exe --expect-local --fullscreen >> results.txt 2>&1
echo exit %errorlevel% >> results.txt
type results.txt
pause
