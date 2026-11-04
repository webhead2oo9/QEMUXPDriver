@echo off
rem Puts this CD's QD3D d3d8.dll and d3d9.dll beside every installed test game
rem and refreshes every other folder on C: that already holds a QD3D copy
rem (per-game test install; INSTALL\ on this CD is the system-wide installer,
rem so C:\WINDOWS DLLs are left alone). Installs or updates qxpmap.sys first
rem (a no-op when the installed driver is current). The DirectDraw/Direct3D 7 front end
rem ddraw.dll goes only beside the DirectDraw test games listed below and
rem wherever a QD3D ddraw.dll already is, so Direct3D 8/9 games keep their
rem ddraw.dll. A d3d8.dll/d3d9.dll/ddraw.dll there that is not from QD3D is
rem kept as *.qd3d-bak first.
rem
rem Prince of Persia: The Sands of Time keys a fog workaround for 2003 NVIDIA
rem drivers on the vendor alone (ForceVSFog=0, InvertFogRange=1); modern
rem NVIDIA drivers then draw a grey screen. Beside every POP.EXE whose
rem Hardware.ini carries those keys, this sets the values the game's launcher
rem writes for ATI cards (ForceVSFog=1, InvertFogRange=0, ZEnableFix=0) and
rem makes the file read-only, because the launcher rewrites it at every start.
setlocal enabledelayedexpansion
set SRC=%~dp0QD3D
set DRIVER_REBOOT=
rem No sc.exe test: XP's sc.exe exits 0 even for a missing service. The
rem installer asks the service manager itself and leaves a current driver alone.
echo Checking the QD3D mapping driver ...
"%~dp0INSTALL\qd3d_install.exe" driver
set "DRIVER_RC=!errorlevel!"
if "!DRIVER_RC!"=="3" (
    set DRIVER_REBOOT=1
) else if not "!DRIVER_RC!"=="0" (
    echo QD3D mapping driver installation failed ^(code !DRIVER_RC!^): see the installer's message above.
    echo No game copies were changed.
    pause
    exit /b !DRIVER_RC!
)
set N=0
set LIST=%TEMP%\qd3d-update.txt
set SORTED=%TEMP%\qd3d-update-sorted.txt
set POPLIST=%TEMP%\qd3d-update-pop.txt
type nul > "%LIST%"
type nul > "%POPLIST%"
rem Lines are "N <folder>" (Direct3D 8/9) or "D <folder>" (also DirectDraw).
for %%d in ("C:\Program Files\Warcraft III Demo" "C:\Program Files\Warcraft III" "C:\Program Files\Max Payne Demo" "C:\Program Files\Sierra\FEAR SP Demo" "C:\Program Files\EA GAMES\Need for Speed Underground 2" "C:\UT2003Demo\System") do (
    if exist "%%~d\" (>>"%LIST%" echo N %%~d)
)
for %%d in ("C:\Program Files\Microsoft Games\Age of Empires II" "C:\QUAKE2" "%USERPROFILE%\Desktop\QUAKE2") do (
    if exist "%%~d\" (>>"%LIST%" echo D %%~d)
)
echo Looking for QD3D copies on C: ...
for /f "delims=" %%f in ('dir /s /b /a-d "C:\d3d8.dll" "C:\d3d9.dll" "C:\ddraw.dll" "C:\POP.EXE" 2^>nul') do (
    set "D=%%~dpf"
    set "D=!D:~0,-1!"
    if /i "%%~nxf"=="POP.EXE" (
        >>"%POPLIST%" echo !D!
    ) else if /i not "!D:~0,10!"=="C:\WINDOWS" (
        findstr /m /c:"QD3D" "%%f" >nul 2>nul && (
            if /i "%%~nxf"=="ddraw.dll" (>>"%LIST%" echo D !D!) else (>>"%LIST%" echo N !D!)
        )
    )
)
sort /+3 "%LIST%" > "%SORTED%"
set PREV=
set PF=
for /f "usebackq tokens=1,*" %%a in ("%SORTED%") do (
    if /i "%%b"=="!PREV!" (
        if "%%a"=="D" set PF=D
    ) else (
        if defined PREV call :put "!PREV!" !PF!
        set "PREV=%%b"
        set "PF=%%a"
    )
)
if defined PREV call :put "!PREV!" !PF!
for /f "usebackq delims=" %%d in ("%POPLIST%") do call :popfog "%%d"
del "%LIST%" "%SORTED%" "%POPLIST%" 2>nul
echo.
echo Updated %N% game folder(s). Start the games normally.
if defined DRIVER_REBOOT echo Restart Windows before starting the games: the QD3D mapping driver is pending.
pause
exit /b 0

:put
set "DIR=%~1"
set FILES=d3d8.dll d3d9.dll
set WHAT=
if "%~2"=="D" (
    set FILES=d3d8.dll d3d9.dll ddraw.dll
    set "WHAT= (with DirectDraw)"
)
for %%f in (%FILES%) do (
    if exist "%~1\%%f" (
        findstr /m /c:"QD3D" "%~1\%%f" >nul || (
            if not exist "%~1\%%f.qd3d-bak" ren "%~1\%%f" "%%f.qd3d-bak"
        )
    )
)
set FAIL=
for %%f in (%FILES%) do (
    copy /Y "%SRC%\%%f" "%~1\" >nul || set FAIL=1
)
if defined FAIL (
    echo   NOT UPDATED: !DIR! - close the game, then run UPDATE again
    exit /b 0
)
echo   %~1!WHAT!
set /a N+=1
exit /b 0

:popfog
set "INI=%~1\Hardware.ini"
if not exist "%INI%" (
    echo   Prince of Persia: start it once, then run UPDATE again for its fog fix
    exit /b 0
)
findstr /b /i /c:"InvertFogRange=" "%INI%" >nul || exit /b 0
set "NEW=%TEMP%\qd3d-hardware.ini"
type nul > "%NEW%"
set F1=
set F2=
set F3=
for /f "usebackq delims=" %%l in ("%INI%") do (
    set "L=%%l"
    if /i "!L:~0,11!"=="ForceVSFog=" (set "L=ForceVSFog=1" & set F1=1)
    if /i "!L:~0,15!"=="InvertFogRange=" (set "L=InvertFogRange=0" & set F2=1)
    if /i "!L:~0,11!"=="ZEnableFix=" (set "L=ZEnableFix=0" & set F3=1)
    >>"%NEW%" echo(!L!
)
if not defined F1 (>>"%NEW%" echo ForceVSFog=1)
if not defined F2 (>>"%NEW%" echo InvertFogRange=0)
if not defined F3 (>>"%NEW%" echo ZEnableFix=0)
attrib -r "%INI%"
copy /Y "%NEW%" "%INI%" >nul || (
    del "%NEW%" 2>nul
    echo   NOT FIXED: !INI! - close the game, then run UPDATE again
    exit /b 0
)
attrib +r "%INI%"
del "%NEW%" 2>nul
echo   Prince of Persia fog fix: %INI%
exit /b 0
