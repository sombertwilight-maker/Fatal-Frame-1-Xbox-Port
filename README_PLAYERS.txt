Fatal Frame (USA, Xbox) -- native Windows port
==============================================

HOW TO PLAY
-----------
  1. Unzip this folder anywhere.
  2. Drag your own Fatal Frame (USA) disc image (.iso / .xiso) onto
     PlayFatalFrame.bat

The first run translates the game from your disc and builds
fatalframe.exe on this computer. That takes a few minutes and happens
once. Every run after it starts the game straight away.

You can also drop the ISO in this folder and just double-click
PlayFatalFrame.bat -- it will find it.


WHAT YOU NEED (one time)
------------------------
  Python 3.10+   https://www.python.org/downloads/
                 tick "Add python.exe to PATH" in the installer
  CMake          https://cmake.org/download/
                 tick "Add CMake to PATH"
  Visual Studio 2022 Community, with the
  "Desktop development with C++" workload
                 https://visualstudio.microsoft.com/downloads/

PlayFatalFrame.bat checks for each of these and tells you which one is
missing, so you can just run it and see.


WHY THERE IS NO READY-MADE EXE
------------------------------
The executable contains the game's own code, translated from the disc --
about 29 MB of it. Handing that out would mean distributing Tecmo's
copyrighted program, which we will not do. What is in this zip is our
own work: the recompiler and the runtime that stands in for the Xbox
kernel, GPU, audio and controller.

You supply the game. The translation runs on your computer, from your
disc, and never leaves it.

You need a real copy of Fatal Frame (USA) for the Xbox. Nothing in this
download includes any part of the game.


IN GAME
-------
  F6          settings menu (resolution, widescreen, effects, costumes)
  F11 / Alt+Enter   fullscreen
  See README.md for the rest.


TROUBLE
-------
Build problems leave logs in work\cmake-configure.log and
work\cmake-build.log -- those are the useful things to attach to a bug
report. Game issues are logged to fatalframe.log beside the exe.
