#!/bin/bash
# RawAccel Linux — Test çalıştırıcı
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
BIN="$ROOT/build-manual/test_accel"

CXX="${CXX:-g++}"
CXXFLAGS="-std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter -I$ROOT/include -I$ROOT/src"

echo "=== RawAccel Linux Birim Testleri ==="
echo "Derleniyor..."

# config.cpp ayrı derleme birimi olarak derlenir (M2: ODR sorununu önler)
$CXX $CXXFLAGS \
    "$ROOT/tests/test_accel.cpp" \
    "$ROOT/src/config.cpp" \
    -o "$BIN"

echo "Çalıştırılıyor..."
echo ""
# Forward any CLI args (e.g. --filter, --list, --quiet) to the test binary
"$BIN" "$@"
