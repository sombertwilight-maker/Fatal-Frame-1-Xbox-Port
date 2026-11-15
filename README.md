# Fatal Frame (Xbox) — native Windows port

A static recompilation of the original Xbox release of Fatal Frame (USA) to a
native Windows executable. The game's x86 code is lifted from the XBE to C
and compiled against a runtime that reimplements the Xbox kernel, the NV2A
GPU (Direct3D 11 backend), the APU/DirectSound audio path, and the USB pad —
no emulator, no original console.

**This repository contains no game content.** You need your own
*Fatal Frame (USA)* disc image. The lifted game code
(`project/src/recomp/gen/`) is generated on your machine from your ISO —
see below.

## Status

Playable start to finish at full speed. Working: rendering (hardware D3D11),
audio and music, WMV cutscenes, photos/ghost battles, saves, keyboard and
pad input.

Extras over the original:

- **F6 in-game settings menu** — changes apply live and persist:
  - internal resolution up to 4x (2560x1920) supersampling
  - widescreen 16:9 (true Hor+ — wider camera FOV, not a stretch)
  - window size presets and borderless fullscreen (F11 / Alt+Enter)
  - FPS counter

## Players — quick start

Download the latest release, unzip it anywhere, and **drag your own
*Fatal Frame (USA)* disc image onto `PlayFatalFrame.bat`**.

The first run translates the game from your disc and builds
`fatalframe.exe` on your machine — a few minutes, once. After that the
same file starts the game immediately. You can also drag the ISO straight
onto `fatalframe.exe` once it exists, or just leave the ISO beside it.

You need, one time only:

- [Python 3.10+](https://www.python.org/downloads/) — tick *Add python.exe to PATH*
- [CMake](https://cmake.org/download/) — tick *Add CMake to PATH*
- [Visual Studio 2022 Community](https://visualstudio.microsoft.com/downloads/)
  with the *Desktop development with C++* workload

`PlayFatalFrame.bat` checks for these and tells you which is missing.

**Why you build it instead of downloading an exe.** The executable contains
the game's own code, lifted from the XBE — roughly 29 MB of translated C.
Shipping that would mean distributing Tecmo's copyrighted program, so no
release here will ever include it. The toolkit is ours to give away; the
game is yours, and the translation happens on your computer, from your disc,
and stays there. Every project of this kind works this way for the same
reason.

## Building

Requirements: Python 3.10+ with `capstone`, CMake, and either Visual Studio
2022 or MinGW-w64.

```
# 1. Generate the game code from YOUR disc image (~1-2 minutes)
py -3 -m pip install capstone
regen.bat "C:\path\to\Fatal Frame (USA).xiso.iso"

# 2. Build
cd project
cmake -S . -B build
cmake --build build --config Release --parallel
```

See `BUILDING.txt` for the MinGW cross-build and the full technical notes.

## Running

Put `fatalframe.exe` anywhere. On first run it finds an `.iso` next to it
(or in the folder above), extracts the game files, verifies the XBE and
starts. Saves go to `saves\` beside the exe; the log is `fatalframe.log`.

## Layout

- `project/` — the game project: launcher, host glue, and (after regen) the
  lifted game code
- `toolkit/` — the xboxrecomp 0.17.0 toolkit with all fixes made for this
  port: recompiler, runtime, GPU/APU/USB models, and analysis tools
- `regen.bat` / `regen.sh` — regenerate `project/src/recomp/gen` from your ISO

## Credits and license

Built on [xboxrecomp](toolkit/README.md) by sp00nz (MIT — see
`toolkit/LICENSE`). Port work, toolkit fixes and the display features were
done with Claude (Anthropic).

Fatal Frame is © Tecmo. This project does not distribute the game; it
requires your own legally obtained copy.
