#!/bin/bash
# RawAccel Linux Installer
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR/.."

echo "=== RawAccel Linux Installer ==="
echo ""

# Must run as root for system install
if [[ $EUID -ne 0 ]]; then
    echo "ERROR: Please run as root: sudo $0"
    exit 1
fi

# Detect the real user (in case of sudo)
REAL_USER="${SUDO_USER:-$USER}"

# Check build tools
if ! command -v g++ &>/dev/null && ! command -v c++ &>/dev/null; then
    echo "ERROR: g++ not found. Install it:"
    echo "  sudo pacman -S gcc  (Arch)"
    echo "  sudo apt install g++  (Debian/Ubuntu)"
    exit 1
fi

# Check libevdev
if ! pkg-config --exists libevdev 2>/dev/null; then
    echo "ERROR: libevdev not found. Install it:"
    echo "  sudo pacman -S libevdev  (Arch)"
    echo "  sudo apt install libevdev-dev  (Debian/Ubuntu)"
    exit 1
fi

# Check GTK4
if ! pkg-config --exists gtk4 2>/dev/null; then
    echo "WARNING: GTK4 not found — GUI will not be built."
    echo "  sudo pacman -S gtk4  (Arch)"
    echo "  sudo apt install libgtk-4-dev  (Debian/Ubuntu)"
    BUILD_GUI=0
else
    BUILD_GUI=1
fi

echo "[1/4] Building..."
bash "$SCRIPT_DIR/build.sh"

echo "[2/4] Installing binaries to /usr/local/bin/..."
install -Dm755 "$ROOT/build-manual/rawaccel-daemon" /usr/local/bin/rawaccel-daemon
install -Dm755 "$ROOT/build-manual/rawaccel-cli"    /usr/local/bin/rawaccel-cli
if [[ $BUILD_GUI -eq 1 ]]; then
    install -Dm755 "$ROOT/build-manual/rawaccel-gui" /usr/local/bin/rawaccel-gui
fi

echo "[3/4] Setting up system config..."

# System-wide config dir (used by the systemd service)
mkdir -p /etc/rawaccel
if [[ ! -f /etc/rawaccel/settings.json ]]; then
    cp "$ROOT/config/default.json" /etc/rawaccel/settings.json
    echo "  Installed default config to /etc/rawaccel/settings.json"
fi

# uinput module auto-load
echo "uinput" > /etc/modules-load.d/rawaccel.conf
modprobe uinput 2>/dev/null || true

# udev rules — install from the versioned file in the repo
install -Dm644 "$SCRIPT_DIR/99-rawaccel.rules" /etc/udev/rules.d/99-rawaccel.rules
udevadm control --reload-rules
udevadm trigger

# Add real user to input group
if [[ -n "$REAL_USER" && "$REAL_USER" != "root" ]]; then
    echo "  Adding $REAL_USER to 'input' group..."
    usermod -aG input "$REAL_USER"
fi

# polkit policy + rules — allows input-group users to start daemon without password
install -Dm644 "$SCRIPT_DIR/polkit/org.rawaccel.policy" \
    /usr/share/polkit-1/actions/org.rawaccel.policy
install -Dm644 "$SCRIPT_DIR/polkit/49-rawaccel.rules" \
    /usr/share/polkit-1/rules.d/49-rawaccel.rules
echo "  polkit policy installed (input group members can start daemon without password)"

# Desktop entry
if [[ $BUILD_GUI -eq 1 ]]; then
    install -Dm644 "$SCRIPT_DIR/rawaccel.desktop" \
        /usr/share/applications/rawaccel.desktop 2>/dev/null || true
fi

echo "[4/4] Enabling systemd service..."
install -Dm644 "$SCRIPT_DIR/rawaccel.service" \
    /etc/systemd/system/rawaccel.service
systemctl daemon-reload
systemctl enable rawaccel
systemctl restart rawaccel

echo ""
echo "=== Installation complete! ==="
echo ""

# ── KDE Plasma: disable compositor mouse acceleration ─────────────────────────
# KDE applies its own libinput acceleration on top of raw input.
# When RawAccel is active, this causes double-acceleration.
# We detect KDE and offer to fix it automatically.

KWINRC=""
if [[ -n "$REAL_USER" && "$REAL_USER" != "root" ]]; then
    REAL_HOME=$(getent passwd "$REAL_USER" | cut -d: -f6)
    KWINRC="$REAL_HOME/.config/kwinrc"
fi

if [[ -n "$KWINRC" ]]; then
    # Check if this looks like a KDE session by checking if kwinrc exists or plasma-related packages installed
    IS_KDE=0
    if [[ -f "$KWINRC" ]] || command -v plasmashell &>/dev/null || command -v kwin_wayland &>/dev/null; then
        IS_KDE=1
    fi

    if [[ $IS_KDE -eq 1 ]]; then
        echo "=== KDE Plasma detected ==="
        echo ""
        echo "KDE applies its own mouse acceleration (libinput) which conflicts with RawAccel."
        echo "To prevent double-acceleration, Pointer Acceleration must be set to Flat."
        echo ""

        # Check current state
        ACCEL_PROFILE=$(grep -A20 '^\[Libinput\]' "$KWINRC" 2>/dev/null | grep "^PointerAccelerationProfile=" | cut -d= -f2 | head -1)
        if [[ "$ACCEL_PROFILE" == "1" ]]; then
            echo "  ✓ KDE pointer acceleration is already set to Flat — no action needed."
        else
            echo "  ⚠ KDE pointer acceleration is NOT Flat (current profile: '${ACCEL_PROFILE:-not set}')"
            echo ""
            echo "  Automatically fixing: setting PointerAccelerationProfile=1 (Flat) in kwinrc..."

            # Apply fix as the real user (not root) to avoid permission issues
            sudo -u "$REAL_USER" bash -c "
                KWINRC='$KWINRC'
                # Ensure [Libinput] section exists with the correct keys
                if grep -q '^\[Libinput\]' \"\$KWINRC\" 2>/dev/null; then
                    # Update existing keys in the [Libinput] section
                    python3 -c \"
import configparser, sys
cfg = configparser.ConfigParser(strict=False)
cfg.optionxform = str  # preserve case
cfg.read('$KWINRC')
if 'Libinput' not in cfg:
    cfg['Libinput'] = {}
cfg['Libinput']['PointerAccelerationProfile'] = '1'
cfg['Libinput']['PointerAcceleration'] = '0'
with open('$KWINRC', 'w') as f:
    cfg.write(f, space_around_delimiters=False)
print('kwinrc updated.')
\" 2>/dev/null || {
                        # Fallback: sed
                        sed -i '/^\[Libinput\]/,/^\[/{
                            s/^PointerAccelerationProfile=.*/PointerAccelerationProfile=1/
                            s/^PointerAcceleration=.*/PointerAcceleration=0/
                        }' \"\$KWINRC\"
                    }
                else
                    echo '' >> \"\$KWINRC\"
                    echo '[Libinput]' >> \"\$KWINRC\"
                    echo 'PointerAccelerationProfile=1' >> \"\$KWINRC\"
                    echo 'PointerAcceleration=0' >> \"\$KWINRC\"
                fi
            "

            echo "  ✓ kwinrc updated. Reloading KWin input settings..."
            # Reload kwin without restarting (works while session is active)
            sudo -u "$REAL_USER" bash -c "
                export DBUS_SESSION_BUS_ADDRESS=\$(grep -z DBUS_SESSION_BUS_ADDRESS /proc/\$(pgrep -u $REAL_USER plasmashell | head -1)/environ 2>/dev/null | tr -d '\0' | sed 's/DBUS_SESSION_BUS_ADDRESS=//')
                if command -v qdbus6 &>/dev/null; then
                    qdbus6 org.kde.KWin /KWin reconfigure 2>/dev/null && echo '  ✓ KWin reconfigured (Plasma 6).' || true
                elif command -v qdbus &>/dev/null; then
                    qdbus org.kde.KWin /KWin reconfigure 2>/dev/null && echo '  ✓ KWin reconfigured (Plasma 5).' || true
                fi
            " 2>/dev/null || echo "  ℹ KWin reload attempted. If not applied, log out and back in."
        fi
        echo ""
        echo "  Alternatively, set it manually:"
        echo "  System Settings → Input Devices → Mouse → Pointer Acceleration = Flat"
        echo ""
    fi
fi

echo "Commands:"
echo "  rawaccel-gui          — Open GUI"
echo "  rawaccel-cli list     — List profiles"
echo "  rawaccel-cli --help   — CLI help"
echo ""
echo "Config files:"
echo "  /etc/rawaccel/settings.json      (system service)"
echo "  ~/.config/rawaccel/settings.json (user — GUI/CLI)"
echo ""
if [[ -n "$REAL_USER" && "$REAL_USER" != "root" ]]; then
    echo "NOTE: Log out and back in for group changes to take effect."
    echo "      Or run: newgrp input"
    echo ""
fi
echo "Service status:"
systemctl status rawaccel --no-pager | head -10
