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
   README_PLAYERS.txt LICENSE "dist/$OUT/" 2>/dev/null || true
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
BAD=$(grep -rlE '^void sub_[0-9a-f]{8}' --include='*.c' --include='*.h' "dist/$OUT" 2>/dev/null || true)
LEFT=$(find "dist/$OUT/project/src/recomp/gen" -type f ! -name '.gitkeep' 2>/dev/null || true)
if [ -n "$BAD" ] || [ -n "$LEFT" ]; then
  echo "ABORT: the package contains lifted game code -- not shipping." >&2
  printf '%s\n' $BAD $LEFT | head >&2
  exit 1
fi

( cd dist && zip -qr "$OUT.zip" "$OUT" )
echo "dist/$OUT.zip  ($(du -h "dist/$OUT.zip" | cut -f1))"
echo "Contains no game code or data. Players supply their own disc image."
