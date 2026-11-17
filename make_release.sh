#!/usr/bin/env bash
# Package a source release: everything a player needs, and nothing of the
# game's. The translated code (project/src/recomp/gen) is deliberately left
# out -- it is made on the player's machine from the player's own disc.
set -euo pipefail
VER="${1:?usage: make_release.sh <version>, e.g. v0.9.0}"
OUT="FatalFrame-PC-$VER"
rm -rf "dist/$OUT" && mkdir -p "dist/$OUT"

cp -r project toolkit "dist/$OUT/"
cp regen.bat regen.sh PlayFatalFrame.bat BUILDING.txt README.md \
   README_PLAYERS.txt LICENSE VERSION "dist/$OUT/" 2>/dev/null || true
cp "Fatal Frame.vbs" "Fatal Frame.bat" "Play without launcher.bat" "dist/$OUT/" 2>/dev/null || true
cp -r installer "dist/$OUT/"
# only the fetch script, never a toolchain someone downloaded locally
mkdir -p "dist/$OUT/tools"
cp tools/bootstrap.ps1 "dist/$OUT/tools/"

# strip anything generated, built, or belonging to the game
rm -rf "dist/$OUT"/project/build* "dist/$OUT"/project/src/recomp/gen/*
touch "dist/$OUT/project/src/recomp/gen/.gitkeep"
find "dist/$OUT" -name '__pycache__' -prune -exec rm -rf {} + 2>/dev/null || true
find "dist/$OUT" \( -name '*.xbe' -o -name '*.iso' -o -name '*.xiso' \
     -o -name '*.exe' -o -name '*.log' -o -name '*.pyc' \) -delete 2>/dev/null || true

# Refuse to ship if any lifted game code slipped in. A translated function
# is "void sub_<addr>" at the start of a line in a C file; the toolkit's own
# sources only ever mention that shape inside Python tests, so this catches
# the real thing and nothing else.
# Game data, as opposed to game code. The guard below catches lifted
# functions; it said nothing about assets, and a costume texture pack
# derived from the game's own art shipped in every release from v0.9.6 to
# v0.9.14 because of that gap. These are the disc's own formats, plus the
# disc itself. Nothing the project legitimately ships uses them: the
# player's own files live beside their build, never in the package.
ASSETS=$(find "dist/$OUT" -type f \( \
      -iname '*.xpr' -o -iname '*.mpx' -o -iname '*.mpk' -o -iname '*.acs' \
   -o -iname '*.sgd' -o -iname '*.pkx' -o -iname '*.xp2' -o -iname '*.mwa' \
   -o -iname '*.slt' -o -iname '*.scn' -o -iname '*.lit' -o -iname '*.anm' \
   -o -iname '*.wav' -o -iname '*.wmv' -o -iname '*.xbe' \
   -o -iname '*.iso' -o -iname '*.xiso' \) 2>/dev/null || true)
if [ -n "$ASSETS" ]; then
  echo "ABORT: the package contains game data -- not shipping." >&2
  printf '%s\n' $ASSETS | head >&2
  exit 1
fi

BAD=$(grep -rlE '^void sub_[0-9a-f]{8}' --include='*.c' --include='*.h' "dist/$OUT" 2>/dev/null || true)
LEFT=$(find "dist/$OUT/project/src/recomp/gen" -type f ! -name '.gitkeep' 2>/dev/null || true)
if [ -n "$BAD" ] || [ -n "$LEFT" ]; then
  echo "ABORT: the package contains lifted game code -- not shipping." >&2
  printf '%s\n' $BAD $LEFT | head >&2
  exit 1
fi

# Configure the PACKAGED tree, exactly as a player's machine sees it. A
# release that cannot even configure is the one bug players cannot work
# around, and it has shipped once already: the project looked for the
# toolkit under a name the package does not use, so every player hit
# "../xboxrecomp is not an existing directory" on the first build.
#
# Configure only. The translated code is deliberately absent, so there is
# nothing to compile yet, and host libraries here are not the player's.
# What this proves is the layout: that the package's own pieces find each
# other. A failure that names the toolkit lookup is fatal; anything else
# is this machine, not the package.
if command -v cmake >/dev/null 2>&1; then
  SMOKE="$(mktemp -d)"
  cmake -S "dist/$OUT/project" -B "$SMOKE" -DCMAKE_BUILD_TYPE=Release \
        >"$SMOKE/configure.log" 2>&1 && SMOKE_RC=0 || SMOKE_RC=$?
  if grep -qE 'Cannot find the xboxrecomp|add_subdirectory given source|is not an existing directory' \
       "$SMOKE/configure.log"; then
    echo "ABORT: the packaged tree cannot find its own toolkit -- not shipping." >&2
    grep -nE 'Cannot find the xboxrecomp|add_subdirectory given source|is not an existing directory' \
      "$SMOKE/configure.log" >&2
    rm -rf "$SMOKE"; exit 1
  fi
  if ! grep -q 'xboxrecomp toolkit:' "$SMOKE/configure.log"; then
    echo "ABORT: the packaged tree never resolved a toolkit path -- not shipping." >&2
    tail -25 "$SMOKE/configure.log" >&2
    rm -rf "$SMOKE"; exit 1
  fi
  if [ "$SMOKE_RC" = 0 ]; then
    echo "smoke test: the packaged tree configures cleanly."
  else
    echo "smoke test: layout resolves; configure stopped on this host's own"
    echo "            dependencies, which the player's bootstrap supplies."
  fi
  rm -rf "$SMOKE"
else
  echo "warning: cmake not present, skipping the packaged-tree smoke test." >&2
fi

# zip UPDATES an archive that already exists rather than replacing it, so
# a file removed from the tree stays in the zip from the previous build of
# the same version. That is how a deleted asset kept shipping even after
# the guard above passed -- the guard checks the staged folder, the zip was
# older than both.
rm -f "dist/$OUT.zip"
( cd dist && zip -qr "$OUT.zip" "$OUT" )
echo "dist/$OUT.zip  ($(du -h "dist/$OUT.zip" | cut -f1))"
echo "Contains no game code or data. Players supply their own disc image."
