@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-xp-acceptance.ps1" %*
exit /b %errorlevel%
