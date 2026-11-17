@echo off
rem Fatal Frame - native Windows : opens the installer window.
rem Use this if Windows policy blocks the .vbs launcher next to it.
cd /d "%~dp0"
if not exist "installer\FatalFrameInstaller.ps1" (
  echo   The installer folder is missing. Unzip the whole release and
  echo   keep these files together.
  pause
  exit /b 1
)
start "" powershell -NoProfile -ExecutionPolicy Bypass -STA -WindowStyle Hidden ^
  -File "%~dp0installer\FatalFrameInstaller.ps1"
exit /b 0
