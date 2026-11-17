# Fatal Frame — native Windows, v1.0.1

Translates your own Fatal Frame (USA, Xbox) disc image into a native
Windows build, on your machine. Nothing here contains any part of the
game.

## Playing it

Open **Fatal Frame.vbs**, press LOAD ISO, pick your disc image, touch the
orb. The first run fetches the build tools (~200 MB) and translates the
game; after that the same window is the launcher and the orb says PLAY.

`Play without launcher.bat` starts a built game with no GUI.
`PlayFatalFrame.bat` still takes a disc image dragged onto it.

## New in this release

**A launcher, not just an installer.** Gold progress while it builds, then
it settles into a play window and steps aside when the game starts. The
bar follows the real build: the translator's output files, then make's own
percentages.

**Controller support in the F6 menu**, with the bindings drawn on screen:
Back opens and closes, d-pad or stick moves, A changes, B closes, LB/RB
page between tabs. The pad is captured while the menu is up, so choosing a
costume no longer presses the button at Miku as well.

**Restart on the COSTUMES tab.** A costume only takes hold when the model
is next loaded, so the thing that applies it sits under the list.

**Costumes.** Twelve entries, and the names match what they load.

**assets\ — files you supply.** Anything dropped in `Game\assets\` stands
in for the disc file of the same name, wherever the game looks for it, and
the size the game expects is corrected automatically. This is how the
Japanese costumes work: they are built from your own discs and never
shipped here.

## Fixes

- The build is a single executable again: the MinGW runtime is linked in,
  so the game no longer depended on a compiler the launcher deletes.
- The finished game installs to `Game\fatalframe.exe` instead of four
  folders deep under `project\build`.
- REINSTALL rebuilds. It used to see the existing exe and exit silently.
- Yukata and Japanese costumes load: an asset-size entry was being shrunk
  as well as grown, which broke every costume whose files are smaller than
  the default.
- A stale costume index now says so instead of quietly showing the
  default.
- Releases are checked for game data, not only for game code, and the zip
  is rebuilt rather than updated in place.

### v1.0.1

- Cutscenes no longer lose their sound partway through. While a movie
  plays the title refuses the points where queued interrupts may run, so
  the APU's interrupts build a backlog and the voice is left without
  samples. The audio device was only given about 43 ms of queued audio,
  which is not enough to ride that out; it now gets about 170 ms.
  `RECOMP_AUDIO_BUF_SAMPLES` and `RECOMP_AUDIO_BUF_COUNT` tune it if the
  added latency is audible.

## Requirements

Your own Fatal Frame (USA) Xbox disc image, ~1.5 GB free, and an internet
connection for the first run only.
