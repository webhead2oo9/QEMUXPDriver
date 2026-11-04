@echo off
rem Same Direct3D 9 draw benchmark twice: first through the stock XP runtime
rem (old XPDM driver path), then through the QD3D proxy. Results are appended
rem to C:\QD3D\bench.txt.
setlocal
if not exist C:\QD3D mkdir C:\QD3D
if not exist C:\QD3D\stock mkdir C:\QD3D\stock
copy /Y "%~dp0QD3D\*.*" C:\QD3D\ >nul
copy /Y "%~dp0QD3D\qd3d9_bench.exe" C:\QD3D\stock\ >nul
echo === stock runtime, 2000 draws/frame === >> C:\QD3D\bench.txt
C:\QD3D\stock\qd3d9_bench.exe 2000 >> C:\QD3D\bench.txt 2>&1
echo === QD3D proxy, 2000 draws/frame === >> C:\QD3D\bench.txt
C:\QD3D\qd3d9_bench.exe 2000 >> C:\QD3D\bench.txt 2>&1
type C:\QD3D\bench.txt
pause
