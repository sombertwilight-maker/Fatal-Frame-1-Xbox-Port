#!/usr/bin/env bash
# Regenerate project/src/recomp/gen from YOUR Fatal Frame (USA) disc image.
#   ./regen.sh "/path/to/Fatal Frame (USA).iso"
# Needs Python 3.10+ with capstone (pip install capstone).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ISO="${1:?usage: regen.sh <Fatal Frame (USA) iso>}"
PY="${PYTHON:-python3}"
WORK="$HERE/work"; mkdir -p "$WORK"
cd "$HERE/toolkit"
"$PY" -m tools.xiso get "$ISO" default.xbe -o "$WORK/"
"$PY" -m tools.xbe_parser "$WORK/default.xbe" --json "$WORK/default_analysis.json" > "$WORK/xbe_report.txt"
"$PY" -m tools.disasm "$WORK/default.xbe" -o "$WORK/disasm" | tail -3          # every code section
"$PY" -m tools.func_id "$WORK/default.xbe" --functions "$WORK/disasm/functions.json" \
      --strings "$WORK/disasm/strings.json" --xrefs "$WORK/disasm/xrefs.json" -o "$WORK/func_id" | tail -1
"$PY" -m tools.abi_analysis "$WORK/default.xbe" --functions "$WORK/disasm/functions.json" \
      --identified "$WORK/func_id/identified_functions.json" --output-dir "$WORK/abi" | tail -1
"$PY" -m tools.recomp "$WORK/default.xbe" --all --split 250 --game-name "Fatal Frame" \
      --disasm-dir "$WORK/disasm" --func-id-dir "$WORK/func_id" --abi-dir "$WORK/abi" \
      --gen-dir "$HERE/project/src/recomp/gen" -o "$WORK/recomp" | tail -4
echo "Done. Now build: see BUILDING.txt"
