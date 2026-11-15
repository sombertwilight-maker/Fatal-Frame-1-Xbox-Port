@echo off
rem ===================================================================
rem  Fatal Frame (USA) -> native Windows
rem
rem  Drag your own Fatal Frame disc image onto this file, or run:
rem      PlayFatalFrame.bat "D:\path\to\Fatal Frame (USA).iso"
rem
rem  The first run translates the game on THIS machine and builds
rem  fatalframe.exe. That takes a few minutes. Every run after it
rem  starts the game straight away.
rem
rem  Nothing here contains any of the game. The translated code is
rem  made from your disc image, on your computer, and stays there.
rem ===================================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"
set "HERE=%~dp0"
set "GEN=%HERE%project\src\recomp\gen"
set "OUT=%HERE%project\build\Release\fatalframe.exe"
if not exist "%HERE%work" mkdir "%HERE%work"

echo.
echo   Fatal Frame - native Windows build
echo   ----------------------------------

rem ---- 1. find the disc image --------------------------------------
set "ISO=%~1"
if not defined ISO for %%F in ("%HERE%*.iso" "%HERE%*.xiso") do if not defined ISO set "ISO=%%~fF"
if not defined ISO (
  echo.
  echo   No disc image found.
  echo.
  echo   Drag your Fatal Frame ^(USA^) ISO onto this file, or put it in
  echo   this folder and run it again. You need your own copy of the
  echo   game - nothing here includes it.
  echo.
  pause
  exit /b 1
)
if not exist "%ISO%" ( echo   Cannot find: %ISO% & pause & exit /b 1 )
echo   Disc image: %ISO%

rem ---- 2. already built? -------------------------------------------
rem (delete project\build to force a rebuild)
if exist "%OUT%" goto :launch

rem ---- 3. prerequisites --------------------------------------------
set "PY="
for %%P in ("py -3" "python" "python3") do (
  if not defined PY ( %%~P -c "import sys" >nul 2>&1 && set "PY=%%~P" )
)
if not defined PY (
  echo.
  echo   Python 3.10 or newer is required to translate the game.
  echo   Install it from https://www.python.org/downloads/
  echo   ^(tick "Add python.exe to PATH" in the installer^)
  echo.
  pause & exit /b 1
)
%PY% -c "import capstone" >nul 2>&1
if errorlevel 1 (
  echo   Installing the disassembler library ^(capstone^)...
  %PY% -m pip install --quiet capstone || (
    echo   Could not install capstone. Try:  %PY% -m pip install capstone
    pause & exit /b 1
  )
)
where cmake >nul 2>&1
if errorlevel 1 (
  echo.
  echo   CMake is required to build. Install it from
  echo   https://cmake.org/download/  ^(tick "Add CMake to PATH"^)
  echo.
  pause & exit /b 1
)

rem ---- 4. translate the game from YOUR disc ------------------------
if not exist "%GEN%\recomp_funcs.h" (
  echo.
  echo   [1/2] Translating the game from your disc image...
  echo         This runs once and takes a minute or two.
  set "PYTHON=%PY%"
  call "%HERE%regen.bat" "%ISO%" || (
    echo.
    echo   Translation failed. See the messages above.
    pause & exit /b 1
  )
) else (
  echo   [1/2] Translated code already present - skipping.
)

rem ---- 5. build ----------------------------------------------------
echo.
echo   [2/2] Building fatalframe.exe...
echo         First build takes several minutes. Later builds are quick.
cmake -S "%HERE%project" -B "%HERE%project\build" >"%HERE%work\cmake-configure.log" 2>&1
if errorlevel 1 (
  echo.
  echo   Could not configure the build. A C compiler is needed:
  echo   install Visual Studio 2022 Community with the
  echo   "Desktop development with C++" workload, then run this again.
  echo   https://visualstudio.microsoft.com/downloads/
  echo.
  echo   Details: work\cmake-configure.log
  pause & exit /b 1
)
cmake --build "%HERE%project\build" --config Release --parallel >"%HERE%work\cmake-build.log" 2>&1
if errorlevel 1 (
  echo   Build failed. Details: work\cmake-build.log
  pause & exit /b 1
)
if not exist "%OUT%" ( echo   Build produced no exe. See work\cmake-build.log & pause & exit /b 1 )
echo   Built: %OUT%

rem ---- 6. play -----------------------------------------------------
:launch
echo.
echo   Starting Fatal Frame...
echo.
start "" "%OUT%" "%ISO%"
exit /b 0
