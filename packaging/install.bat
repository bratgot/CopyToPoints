@echo off
REM Double-click installer for CopyToPoints.
REM
REM It only calls install.ps1. -ExecutionPolicy Bypass is here because the
REM default policy on a workstation refuses to run a downloaded .ps1 at all,
REM and the alternative is telling everyone to unblock the file by hand.
setlocal
set "HERE=%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%install.ps1" %*
if errorlevel 1 (
  echo.
  echo Install FAILED - see the message above.
  pause
  exit /b 1
)
echo.
pause
