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
set "ROOT=!HERE!"
if "!ROOT:~-1!"=="\" set "ROOT=!ROOT:~0,-1!"

echo.
echo   Fatal Frame - native Windows   (build v1.0.1)
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
rem FF_REBUILD means "build it again even though an exe is sitting there":
rem the sources changed. Without this the launcher's REINSTALL did nothing
rem at all -- this file saw the old exe, jumped to :launch, and exited.
rem CMake still only recompiles what actually changed, so asking for a
rem rebuild when nothing has is cheap.
if defined FF_REBUILD goto :mustbuild
if exist "!OUT!" goto :launch
if exist "!OUT2!" ( set "OUT=!OUT2!" & goto :launch )
:mustbuild

rem ---- 3. build tools ----------------------------------------------
call :addtools
call :findtools
if defined PY if defined CM goto :havetools

echo.
echo   Fetching the build tools ^(first run only^).
echo   About 200 MB from python.org, PyPI, Kitware and winlibs.
echo   This takes a few minutes; it never happens again.
powershell -NoProfile -ExecutionPolicy Bypass -File "!HERE!tools\bootstrap.ps1" -Root "!ROOT!"
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
rem GCC cannot live under a path with a space. It works out where its own
rem libraries are from where it was invoked and then splits that path on
rem spaces, inside the compiler driver, where no quoting of ours reaches --
rem and it resolves short names and drive mappings back to the real path
rem first, so neither of those helps either. Rather than making the player
rem move the folder, stage the compiler somewhere without spaces for the one
rem build that needs it, and take it away again afterwards.
set "MINGWSRC="
for /d %%D in ("!HERE!tools\mingw\*") do if exist "%%D\bin\gcc.exe" set "MINGWSRC=%%D"
if exist "!HERE!tools\mingw\bin\gcc.exe" set "MINGWSRC=!HERE!tools\mingw"
set "STAGE="
if defined MINGWSRC echo !MINGWSRC!| find " " >nul && (
  for %%T in ("!TEMP!" "!LOCALAPPDATA!" "!SystemDrive!") do if not defined STAGE (
    set "CAND=%%~T\ffport-build"
    echo !CAND!| find " " >nul || set "STAGE=!CAND!"
  )
  if defined STAGE (
    echo   This folder's path contains a space, which the C compiler cannot
    echo   work under. Staging it in "!STAGE!" for this build.
    if not exist "!STAGE!\mingw\bin\gcc.exe" (
      robocopy "!MINGWSRC!" "!STAGE!\mingw" /E /NFL /NDL /NJH /NJS /NP >nul
    )
    ver >nul
    if exist "!STAGE!\mingw\bin\gcc.exe" (
      set "PATH=!STAGE!\mingw\bin;!PATH!"
      set "CM=!STAGE!\mingw\bin\cmake.exe"
      if not exist "!CM!" for /f "delims=" %%C in ('where cmake 2^>nul') do set "CM=%%C"
    ) else (
      set "STAGE="
    )
  )
  if not defined STAGE (
    echo.
    echo   This folder's path contains a space and the compiler could not be
    echo   staged anywhere without one. Move this folder somewhere without
    echo   spaces - for example C:\FatalFramePC - and run it again.
    goto :fail
  )
)
echo   [2/2] Building fatalframe.exe. First build takes a few minutes.
rem MSVC if the machine has it; otherwise the GCC we fetched.
set "GENFLAG="
where cl >nul 2>&1 || (where gcc >nul 2>&1 && set GENFLAG=-G "MinGW Makefiles")
"!CM!" -S "!HERE!project" -B "!HERE!project\build" !GENFLAG! -DCMAKE_BUILD_TYPE=Release >"!HERE!work\cmake-configure.log" 2>&1
if errorlevel 1 (
  rem CMake caches the compiler it found, so a run that failed here would
  rem keep failing with the cached one. Clear it so the next run is clean.
  rmdir /s /q "!HERE!project\build" 2>nul
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

rem ---- install it somewhere a person can find -----------------------
rem The build leaves the exe several folders deep under project\build, which
rem nobody should have to go digging for. Put the finished game in Game\
rem next to this file, and launch it from there.
set "GAMEDIR=!HERE!Game"
if not exist "!GAMEDIR!" mkdir "!GAMEDIR!"
copy /y "!OUT!" "!GAMEDIR!\fatalframe.exe" >nul
if exist "!GAMEDIR!\fatalframe.exe" set "OUT=!GAMEDIR!\fatalframe.exe"

rem Belt and braces. The exe links the compiler runtime statically, so this
rem should copy nothing -- but if a future change makes it import a DLL from
rem the toolchain, that DLL has to travel with the game, because the staged
rem toolchain is deleted below and is gone for good.
set "MGBIN="
if defined STAGE if exist "!STAGE!\mingw\bin\objdump.exe" set "MGBIN=!STAGE!\mingw\bin"
if not defined MGBIN if defined MINGWSRC if exist "!MINGWSRC!\bin\objdump.exe" set "MGBIN=!MINGWSRC!\bin"
if defined MGBIN (
  for /f "tokens=3" %%D in ('""!MGBIN!\objdump.exe" -p "!OUT!" ^| findstr /i /c:"DLL Name:""') do (
    if exist "!MGBIN!\%%D" copy /y "!MGBIN!\%%D" "!GAMEDIR!\" >nul
  )
  rem one more pass, for anything those DLLs themselves pull in
  for %%F in ("!GAMEDIR!\*.dll") do (
    for /f "tokens=3" %%D in ('""!MGBIN!\objdump.exe" -p "%%F" ^| findstr /i /c:"DLL Name:""') do (
      if exist "!MGBIN!\%%D" if not exist "!GAMEDIR!\%%D" copy /y "!MGBIN!\%%D" "!GAMEDIR!\" >nul
    )
  )
)

if defined STAGE rmdir /s /q "!STAGE!" 2>nul

rem Remember the disc image, so the game can be started again later without
rem being told a second time where it is.
>"!HERE!work\iso.txt" echo !ISO!

echo   Built: "!OUT!"
echo.
echo   The game is installed in:  Game\fatalframe.exe
echo   To play it again later, open "Fatal Frame.vbs".

:launch
rem When this run only exists to build through the drive mapping, stop here
rem and let the parent launch from the real path.
if defined FF_SUBST exit /b 0
rem The GUI front end builds through this file and starts the game
rem itself, so it asks for the build alone.
if defined FF_BUILD_ONLY exit /b 0
echo.
echo   Starting Fatal Frame...
start "" "!OUT!" "!ISO!"
exit /b 0

rem ---- helpers -----------------------------------------------------
:addtools
rem Short (8.3) paths, because GCC works out where its own libraries live
rem from where it was invoked, and splits that on spaces when it hands the
rem result to ld -- so a folder like "Fatal Frame (USA)" breaks linking with
rem "cannot find C:/Users/.../Fatal". Quoting cannot reach inside ld; giving
rem it a path with no spaces in the first place can.
if exist "!HERE!tools\python\python.exe" for %%D in ("!HERE!tools\python") do set "PATH=%%~sD;!PATH!"
for /d %%D in ("!HERE!tools\cmake\*") do if exist "%%D\bin\cmake.exe" set "PATH=%%~sD\bin;!PATH!"
for /d %%D in ("!HERE!tools\mingw\*") do if exist "%%D\bin\gcc.exe" set "PATH=%%~sD\bin;!PATH!"
if exist "!HERE!tools\mingw\bin\gcc.exe" for %%D in ("!HERE!tools\mingw") do set "PATH=%%~sD\bin;!PATH!"
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
if defined STAGE rmdir /s /q "!STAGE!" 2>nul
echo.
echo   ---------------------------------------------------------------
echo   Stopped. The reason is above. This window stays open so you can
echo   read it; logs are in the work\ folder next to this file.
echo   ---------------------------------------------------------------
echo.
pause
exit /b 1
