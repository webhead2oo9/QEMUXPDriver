@echo off
if "%1"=="" goto usage
if %1 LSS 1 goto usage
if %1 GTR 6 goto usage
reg add "HKLM\SOFTWARE\XPDriver\StubICD" /v FailMode /t REG_DWORD /d %1 /f
if errorlevel 1 echo Failed. Run this command from an Administrator account.
goto end
:usage
echo Usage: setmode 1^|2^|3^|4^|5^|6
echo   1 ValidateVersion fails
echo   2 DescribePixelFormat reports zero ICD formats
echo   3 ICD format advertised, SetPixelFormat fails
echo   4 SetPixelFormat succeeds, context creation fails
echo   5 Context creation succeeds, SetContext fails
echo   6 Inert context succeeds with a 336-entry no-op table
:end

