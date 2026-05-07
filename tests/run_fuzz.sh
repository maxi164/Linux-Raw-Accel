#!/bin/bash
# Fuzz testing for rawaccel — config JSON parsing + acceleration pipeline
# Requires: clang++ with libFuzzer support
#
# Usage:
#   bash tests/run_fuzz.sh              # Run each harness for 60 seconds
#   bash tests/run_fuzz.sh 300          # Run each for 300 seconds
#   bash tests/run_fuzz.sh 0            # Run indefinitely (Ctrl+C to stop)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
BUILD="$ROOT/build-manual"
CORPUS="$SCRIPT_DIR/corpus_config"
DURATION="${1:-60}"

if ! command -v clang++ &>/dev/null; then
    echo "Error: clang++ not found. Install clang to use fuzz testing."
    exit 1
fi

mkdir -p "$BUILD" "$CORPUS"

FUZZ_FLAGS="-std=c++20 -O2 -g -fsanitize=fuzzer,address,undefined -I $ROOT/include"

echo "=== [1/2] Building & running fuzz_config (JSON parser) ==="
clang++ $FUZZ_FLAGS "$SCRIPT_DIR/fuzz_config.cpp" "$ROOT/src/config.cpp" -o "$BUILD/fuzz_config"
if [ "$DURATION" = "0" ]; then
    "$BUILD/fuzz_config" "$CORPUS" -max_len=8192 -timeout=5
else
    "$BUILD/fuzz_config" "$CORPUS" -max_len=8192 -timeout=5 -max_total_time="$DURATION"
fi

echo ""
echo "=== [2/2] Building & running fuzz_accel (acceleration pipeline) ==="
clang++ $FUZZ_FLAGS "$SCRIPT_DIR/fuzz_accel.cpp" "$ROOT/src/config.cpp" -o "$BUILD/fuzz_accel"
if [ "$DURATION" = "0" ]; then
    "$BUILD/fuzz_accel" -max_len=256 -timeout=5
else
    "$BUILD/fuzz_accel" -max_len=256 -timeout=5 -max_total_time="$DURATION"
fi

echo ""
echo "=== Fuzz testing complete — no crashes found ==="
