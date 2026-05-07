#!/bin/bash
# Quick build script (no cmake required)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."
BUILD="$ROOT/build-manual"

EVDEV_CFLAGS="$(pkg-config --cflags libevdev)"
EVDEV_LIBS="$(pkg-config --libs libevdev)"
HAVE_GTK4=0
GTK4_CFLAGS=""
GTK4_LIBS=""
if pkg-config --exists gtk4 2>/dev/null; then
    HAVE_GTK4=1
    GTK4_CFLAGS="$(pkg-config --cflags gtk4)"
    GTK4_LIBS="$(pkg-config --libs gtk4)"
fi

CXX="${CXX:-g++}"
# -march=native optimises for this machine's CPU but breaks portability.
# Set RAWACCEL_PORTABLE=1 to build a portable binary (e.g. for packaging).
if [ "${RAWACCEL_PORTABLE:-0}" = "1" ]; then
    MARCH=""
else
    MARCH="-march=native"
fi
# Security hardening for a daemon that runs with root + uinput access:
#   -fstack-protector-strong   : stack canaries on functions with arrays/refs
#   -fstack-clash-protection   : probe pages on stack growth → defeats stack
#                                clash attacks (large alloca / VLA into adj. region)
#   -fcf-protection=full       : Intel CET — indirect-call/branch + return target
#                                validation (no-op on CPUs without CET hardware)
#   -D_FORTIFY_SOURCE=2        : compile-time bounds checking on libc string ops
#   -D_GLIBCXX_ASSERTIONS      : runtime bounds checking on libstdc++ containers
#                                (vector::operator[], std::string ops, ...)
#   -fPIE -pie                 : full ASLR for the binary
#   -Wformat -Wformat-security : catch printf-style format-string mistakes
HARDENING="-fstack-protector-strong -fstack-clash-protection -fcf-protection=full \
-D_FORTIFY_SOURCE=2 -D_GLIBCXX_ASSERTIONS -fPIE -Wformat -Wformat-security"
# -z,noexecstack : kernel refuses to execute the stack page (defence in depth
#                  beyond the GNU_STACK PT_LOAD permissions).
# -z,separate-code: keep .text and .rodata in separate PT_LOAD segments so
#                  read-only data isn't mappable as executable.
LDFLAGS_HARDEN="-pie -Wl,-z,relro,-z,now,-z,noexecstack,-z,separate-code"
CXXFLAGS="-std=c++20 -O3 $MARCH -Wall -Wextra -Wno-unused-parameter $HARDENING -I$ROOT/include"

mkdir -p "$BUILD"

echo "[1/3] Building rawaccel-daemon..."
$CXX $CXXFLAGS $EVDEV_CFLAGS \
    "$ROOT/src/config.cpp" \
    "$ROOT/daemon/daemon.cpp" \
    "$ROOT/daemon/main.cpp" \
    $EVDEV_LIBS -lpthread $LDFLAGS_HARDEN \
    -o "$BUILD/rawaccel-daemon"

echo "[2/3] Building rawaccel-cli..."
$CXX $CXXFLAGS \
    "$ROOT/src/config.cpp" \
    "$ROOT/cli/main.cpp" \
    $LDFLAGS_HARDEN \
    -o "$BUILD/rawaccel-cli"

if [ "$HAVE_GTK4" = "1" ]; then
    echo "[3/3] Building rawaccel-gui..."
    $CXX $CXXFLAGS $GTK4_CFLAGS \
        "$ROOT/src/config.cpp" \
        "$ROOT/gui/main.cpp" \
        $GTK4_LIBS $LDFLAGS_HARDEN \
        -o "$BUILD/rawaccel-gui"
else
    echo "[3/3] Skipping rawaccel-gui (GTK4 development files not found)."
    rm -f "$BUILD/rawaccel-gui"
fi

echo ""
echo "Build complete! Binaries in $BUILD/"
echo ""
echo "  $BUILD/rawaccel-daemon   — run as root or with input group"
echo "  $BUILD/rawaccel-cli      — CLI config tool"
if [ "$HAVE_GTK4" = "1" ]; then
    echo "  $BUILD/rawaccel-gui      — GUI"
fi
