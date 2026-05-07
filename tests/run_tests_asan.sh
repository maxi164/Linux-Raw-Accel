#!/bin/bash
# RawAccel Linux — ASan + UBSan ile birim testler
# Kullanım: bash tests/run_tests_asan.sh
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
BIN="$ROOT/build-manual/test_accel_asan"

CXX="${CXX:-g++}"
CXXFLAGS="-std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I$ROOT/include -I$ROOT/src"

export ASAN_OPTIONS="${ASAN_OPTIONS:-halt_on_error=1:abort_on_error=1:strict_string_checks=1:detect_leaks=1}"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:abort_on_error=1:print_stacktrace=1}"

mkdir -p "$ROOT/build-manual"

echo "=== RawAccel Linux Birim Testleri (ASan + UBSan) ==="
echo "Derleniyor..."
$CXX $CXXFLAGS \
    "$ROOT/tests/test_accel.cpp" \
    "$ROOT/src/config.cpp" \
    -o "$BIN"

echo "Çalıştırılıyor..."
echo ""
"$BIN" "$@"
