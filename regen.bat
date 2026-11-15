@echo off
rem Regenerate project\src\recomp\gen from YOUR Fatal Frame (USA) disc image.
rem   regen.bat "D:\Games\Fatal Frame (USA).iso"
rem Needs Python 3.10+ (py launcher) with capstone:  py -3 -m pip install capstone
setlocal
rem PYTHON lets a caller pass the interpreter it found; py -3 by default
if "%PYTHON%"=="" set PYTHON=py -3
set HERE=%~dp0
set ISO=%~1
if "%ISO%"=="" ( echo usage: regen.bat ^<Fatal Frame (USA) iso^> & exit /b 1 )
set WORK=%HERE%work
if not exist "%WORK%" mkdir "%WORK%"
pushd "%HERE%toolkit"
%PYTHON% -m tools.xiso get "%ISO%" default.xbe -o "%WORK%\\" || goto :err
%PYTHON% -m tools.xbe_parser "%WORK%\default.xbe" --json "%WORK%\default_analysis.json" > "%WORK%\xbe_report.txt" || goto :err
%PYTHON% -m tools.disasm "%WORK%\default.xbe" -o "%WORK%\disasm" || goto :err
%PYTHON% -m tools.func_id "%WORK%\default.xbe" --functions "%WORK%\disasm\functions.json" --strings "%WORK%\disasm\strings.json" --xrefs "%WORK%\disasm\xrefs.json" -o "%WORK%\func_id" || goto :err
%PYTHON% -m tools.abi_analysis "%WORK%\default.xbe" --functions "%WORK%\disasm\functions.json" --identified "%WORK%\func_id\identified_functions.json" --output-dir "%WORK%\abi" || goto :err
%PYTHON% -m tools.recomp "%WORK%\default.xbe" --all --split 250 --game-name "Fatal Frame" --disasm-dir "%WORK%\disasm" --func-id-dir "%WORK%\func_id" --abi-dir "%WORK%\abi" --gen-dir "%HERE%project\src\recomp\gen" -o "%WORK%\recomp" || goto :err
popd
echo Done. Now build: see BUILDING.txt
exit /b 0
:err
popd
echo Regeneration failed.
exit /b 1
