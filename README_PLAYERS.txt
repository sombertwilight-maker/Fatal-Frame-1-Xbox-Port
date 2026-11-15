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


WHAT YOU NEED
-------------
A copy of Fatal Frame (USA) for the Xbox, as a disc image. That is all.

The first run downloads the build tools it needs (Python, CMake and a C
compiler, about 200 MB) into a tools\ folder beside this file, then
translates and builds the game. That happens once; after it, starting the
game is instant. You need an internet connection for that first run only.

Nothing is installed on your system and nothing touches your registry --
everything lands in this folder, and deleting it removes every trace.

If the machine is offline, or a firewall blocks the downloads, you can
instead install Python 3.10+, CMake and Visual Studio 2022 (with the
"Desktop development with C++" workload) yourself; the script uses them
if it finds them and skips the download.


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
