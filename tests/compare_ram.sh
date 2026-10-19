#!/usr/bin/env bash
# End-to-end equivalence test: recompiled build vs. Supermodel's own interpreter.
#
#   tests/compare_ram.sh [build_dir] [seconds]
#
# Runs the game for <seconds> of emulated time three times: twice with the stock
# interpreter (ECA_PPC_MODE=stock) and once with recompiled code, then compares the
# 8 MB RAM dumps. Supermodel itself is not bit-for-bit repeatable between runs (the
# RTC reads the host clock), so a byte only counts as a mismatch if the native run
# differs from BOTH stock runs. Exit status 0 means no mismatches.
#
# Set EXE to test another binary, e.g. EXE="wine eca.exe" with build_dir=build-win.
set -euo pipefail
BUILD="${1:-build}"
SECS="${2:-15}"
RUN="$(cd "$BUILD/run" && pwd)"
EXE="${EXE:-./eca}"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

run() {  # run <mode> <dump name>
  ( cd "$RUN" && rm -f ram.bin &&
    ECA_PPC_MODE="$1" ECA_EXIT_AFTER="$SECS" ECA_PPC_STATS=1 WINEDEBUG=-all \
      $EXE eca.zip -no-threads -no-throttle -no-sound -no-vsync -dump-memory -log-output=none \
      >"$OUT/$2.log" 2>&1 && mv ram.bin "$OUT/$2.bin" )
  grep '^\[ppc\]' "$OUT/$2.log" | tail -2 | sed "s/^/  $2: /" || true
}

echo "Running $SECS s of emulated time in $RUN"
run stock stock1
run stock stock2
run native native

python3 -I - "$OUT/stock1.bin" "$OUT/stock2.bin" "$OUT/native.bin" <<'PY'
import sys
s1, s2, n = (open(p, 'rb').read() for p in sys.argv[1:4])
noise = sum(1 for a, b in zip(s1, s2) if a != b)
bad = [i for i in range(len(n)) if n[i] != s1[i] and n[i] != s2[i]]
print(f"stock-vs-stock noise: {noise} bytes; native mismatches: {len(bad)} bytes")
for i in bad[:16]:
    print(f"  RAM {i:06X}: native={n[i]:02X} stock={s1[i]:02X}/{s2[i]:02X}")
sys.exit(1 if bad else 0)
PY
