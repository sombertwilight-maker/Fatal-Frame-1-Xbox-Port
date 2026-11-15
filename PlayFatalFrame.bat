@echo off
rem ===================================================================
rem  Fatal Frame (USA) -> native Windows
rem
rem  Drag your own Fatal Frame disc image onto this file, or run:
rem      PlayFatalFrame.bat "D:\path\to\Fatal Frame (USA).iso"
rem
rem  First run fetches the build tools it needs into .\tools and builds
rem  the game from your disc. After that it just starts the game.
rem
rem  Nothing here contains any part of the game. The translated code is
rem  made from your disc image, on your computer, and stays there.
rem ===================================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"
set "HERE=%~dp0"
set "GEN=!HERE!project\src\recomp\gen"
set "OUT=!HERE!project\build\Release\fatalframe.exe"
set "OUT2=!HERE!project\build\fatalframe.exe"
if not exist "!HERE!work" mkdir "!HERE!work"

echo.
echo   Fatal Frame - native Windows
echo   ----------------------------

rem ---- 1. find the disc image --------------------------------------
set "ISO=%~1"
if not defined ISO for %%F in ("!HERE!*.iso" "!HERE!*.xiso") do if not defined ISO set "ISO=%%~fF"
if not defined ISO (
  echo.
  echo   No disc image found.
  echo.
  echo   Drag your Fatal Frame ^(USA^) disc image onto this file, or put
  echo   it in this folder and run it again. You need your own copy of
  echo   the game - nothing here includes it.
  goto :fail
)
if not exist "!ISO!" ( echo   Cannot find: "!ISO!" ^& goto :fail )
echo   Disc image: "!ISO!"

rem ---- 2. already built? -------------------------------------------
if exist "!OUT!" goto :launch
if exist "!OUT2!" ( set "OUT=!OUT2!" & goto :launch )

rem ---- 3. build tools ----------------------------------------------
call :addtools
call :findtools
if defined PY if defined CM goto :havetools

echo.
echo   Fetching the build tools ^(first run only^).
echo   About 200 MB from python.org, PyPI, Kitware and winlibs.
echo   This takes a few minutes; it never happens again.
powershell -NoProfile -ExecutionPolicy Bypass -File "!HERE!tools\bootstrap.ps1" "!HERE!"
if errorlevel 1 (
  echo.
  echo   Could not fetch the build tools. If this machine is offline or
  echo   behind a proxy, see README_PLAYERS.txt for the manual route.
  goto :fail
)
call :addtools
call :findtools
if not defined PY ( echo   Still no working Python. ^& goto :fail )
if not defined CM ( echo   Still no working CMake. ^& goto :fail )
:havetools
echo   Python: !PY!
echo   CMake : !CM!

rem ---- 4. translate the game from YOUR disc ------------------------
if not exist "!GEN!\recomp_funcs.h" (
  echo.
  echo   [1/2] Translating the game from your disc image...
  set "PYTHON=!PY!"
  call "!HERE!regen.bat" "!ISO!"
  if errorlevel 1 (
    echo   Translation failed - see the messages above.
    goto :fail
  )
) else (
  echo   [1/2] Translated code already present - skipping.
)

rem ---- 5. build ----------------------------------------------------
echo.
echo   [2/2] Building fatalframe.exe. First build takes a few minutes.
rem MSVC if the machine has it; otherwise the GCC we fetched.
set "GENFLAG="
where cl >nul 2>&1 || (where gcc >nul 2>&1 && set GENFLAG=-G "MinGW Makefiles")
"!CM!" -S "!HERE!project" -B "!HERE!project\build" !GENFLAG! -DCMAKE_BUILD_TYPE=Release >"!HERE!work\cmake-configure.log" 2>&1
if errorlevel 1 (
  echo   Configure failed. Details: work\cmake-configure.log
  goto :fail
)
"!CM!" --build "!HERE!project\build" --config Release --parallel >"!HERE!work\cmake-build.log" 2>&1
if errorlevel 1 (
  echo   Build failed. Details: work\cmake-build.log
  goto :fail
)
if exist "!OUT2!" set "OUT=!OUT2!"
if not exist "!OUT!" ( echo   Build produced no exe. See work\cmake-build.log ^& goto :fail )
echo   Built: "!OUT!"

:launch
echo.
echo   Starting Fatal Frame...
start "" "!OUT!" "!ISO!"
exit /b 0

rem ---- helpers -----------------------------------------------------
:addtools
if exist "!HERE!tools\python\python.exe" set "PATH=!HERE!tools\python;!PATH!"
for /d %%D in ("!HERE!tools\cmake\*") do if exist "%%D\bin\cmake.exe" set "PATH=%%D\bin;!PATH!"
for /d %%D in ("!HERE!tools\mingw\*") do if exist "%%D\bin\gcc.exe" set "PATH=%%D\bin;!PATH!"
if exist "!HERE!tools\mingw\bin\gcc.exe" set "PATH=!HERE!tools\mingw\bin;!PATH!"
exit /b 0

:findtools
set "PY="
set "CM="
if exist "!HERE!tools\python\python.exe" (
  "!HERE!tools\python\python.exe" -c "import capstone" >nul 2>&1 && set "PY=!HERE!tools\python\python.exe"
)
if not defined PY for %%P in ("py -3" "python" "python3") do (
  if not defined PY ( %%~P -c "import capstone" >nul 2>&1 && set "PY=%%~P" )
)
for /f "delims=" %%C in ('where cmake 2^>nul') do if not defined CM set "CM=%%C"
exit /b 0

:fail
echo.
echo   ---------------------------------------------------------------
echo   Stopped. The reason is above. This window stays open so you can
echo   read it; logs are in the work\ folder next to this file.
echo   ---------------------------------------------------------------
echo.
pause
exit /b 1
