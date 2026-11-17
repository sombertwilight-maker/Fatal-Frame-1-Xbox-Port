@echo off
rem ===================================================================
rem  Fatal Frame - native Windows : start the game you already built.
rem
rem  This does not build anything. If the game has not been built yet,
rem  run "Install Fatal Frame.vbs" (or PlayFatalFrame.bat) first.
rem
rem  Drag your disc image onto this file to play from a different one.
rem ===================================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"
set "HERE=%~dp0"
set "EXE=!HERE!Game\fatalframe.exe"

if not exist "!EXE!" (
  rem an older layout, or a build that was never installed into Game\
  if exist "!HERE!project\build\fatalframe.exe"         set "EXE=!HERE!project\build\fatalframe.exe"
  if exist "!HERE!project\build\Release\fatalframe.exe" set "EXE=!HERE!project\build\Release\fatalframe.exe"
)
if not exist "!EXE!" (
  echo.
  echo   The game has not been built yet.
  echo.
  echo   Open "Fatal Frame.vbs" and point it at your Fatal Frame
  echo   disc image. This file only starts a game that already exists.
  echo.
  pause
  exit /b 1
)

rem ---- which disc image ---------------------------------------------
rem The game reads its data from the disc image every time it runs, so it
rem needs one even though it is already built.
set "ISO=%~1"
if not defined ISO if exist "!HERE!work\iso.txt" (
  set /p ISO=<"!HERE!work\iso.txt"
)
if defined ISO if not exist "!ISO!" set "ISO="
if not defined ISO for %%F in ("!HERE!*.iso" "!HERE!*.xiso") do if not defined ISO set "ISO=%%~fF"

if not defined ISO (
  echo.
  echo   Cannot find your disc image.
  echo.
  echo   Drag it onto this file, or put it in this folder, and try again.
  echo.
  pause
  exit /b 1
)

rem remember it for next time
>"!HERE!work\iso.txt" echo !ISO!

echo   Starting Fatal Frame...
start "" "!EXE!" "!ISO!"
exit /b 0
