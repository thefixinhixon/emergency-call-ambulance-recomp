#!/usr/bin/env bash
# Headless Ghidra analysis of the fixed CROM, exporting ppcrecomp seeds.
#
#   tools/ghidra/analyze.sh <ghidra_install_dir> <crom.bin> <config> <out_seeds> [extra seed files...]
#
# The Ghidra project is kept in analysis/ghidra so REA (or the Ghidra GUI) can open it
# afterwards for interactive work: names, comments, data types.
#
# Ghidra 12 needs a JDK 21+; set JAVA_HOME if the default java is older or is a JRE.
set -euo pipefail
GHIDRA="$1"; CROM="$2"; CFG="$3"; OUT="$4"; shift 4
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
PROJ="$ROOT/analysis/ghidra"
mkdir -p "$PROJ"

# Seeds are passed as absolute paths since Ghidra runs scripts from its own directory
SEEDS=()
for s in "$@"; do SEEDS+=("$(realpath "$s")"); done

"$GHIDRA/support/analyzeHeadless" "$PROJ" eca \
  -import "$CROM" -overwrite \
  -processor PowerPC:BE:32:default \
  -loader BinaryLoader -loader-baseAddr 0xFF800000 \
  -scriptPath "$HERE" \
  -preScript SetupModel3.java "$(realpath "$CFG")" "${SEEDS[@]}" \
  -postScript ExportSeeds.java "$(realpath -m "$OUT")" \
  -analysisTimeoutPerFile 3600
