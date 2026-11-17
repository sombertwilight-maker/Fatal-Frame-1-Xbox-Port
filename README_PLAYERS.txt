Fatal Frame (USA, Xbox) -- native Windows port
==============================================

HOW TO PLAY
-----------
  1. Unzip this folder anywhere.
  2. Open "Fatal Frame.vbs".
  3. Press LOAD ISO and choose your own Fatal Frame (USA) disc image
     (.iso / .xiso), then touch the orb to install.

The first run translates the game from your disc and builds it on this
computer. That takes a few minutes and happens once.

After that, "Fatal Frame.vbs" is your launcher: open it and the orb says
PLAY. It remembers the disc image, so there is nothing to choose again
and nothing to rebuild. The finished game sits in Game\fatalframe.exe if
you would rather make a shortcut to it.

REINSTALL builds the game again from your disc. You only need it after
replacing the release with a newer one -- the game you have keeps working
until the new build finishes.

If the launcher window will not open, these do the same thing without it:
  "Play without launcher.bat"  - starts a game you have already built
  PlayFatalFrame.bat           - drag your disc image onto it to build


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
