@echo off
rem Runs a program with QD3D's DirectDraw trace on: drag the game's .exe onto
rem this file (or pass its path). The trace goes to C:\QD3D\qddraw-trace.log,
rem one block of call counts and timings per second; play the slow part for
rem half a minute, then quit the game normally.
setlocal
if "%~1"=="" (
    echo Drag a game's .exe onto TRACE.CMD, or run: TRACE.CMD "C:\path\game.exe"
    pause
    exit /b 1
)
if not exist C:\QD3D mkdir C:\QD3D
set QDDRAW_TRACE=1
set QDDRAW_LOG=C:\QD3D\qddraw-trace.log
if exist "%QDDRAW_LOG%" del "%QDDRAW_LOG%"
echo Tracing %~nx1 into %QDDRAW_LOG% ...
cd /d "%~dp1"
start "" /wait "%~1" %2 %3 %4 %5 %6 %7 %8 %9
echo Done. Trace: %QDDRAW_LOG%
pause
