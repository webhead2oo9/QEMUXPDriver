@echo off
"%~dp0qxptest.exe" --crash-owner
ping -n 9 127.0.0.1 >nul
"%~dp0qxptest.exe"
