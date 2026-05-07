#!/bin/bash
# RawAccel Linux Uninstaller
set -e

echo "=== RawAccel Linux Uninstaller ==="
echo ""

if [[ $EUID -ne 0 ]]; then
    echo "ERROR: Please run as root: sudo $0"
    exit 1
fi

# Stop and disable service
if systemctl is-active --quiet rawaccel 2>/dev/null; then
    echo "[1/4] Stopping service..."
    systemctl stop rawaccel
fi
if systemctl is-enabled --quiet rawaccel 2>/dev/null; then
    echo "      Disabling service..."
    systemctl disable rawaccel
fi
# Kill any rawaccel-daemon started outside systemd (e.g. directly via pkexec).
# Without this, removing the binary leaves the running process holding /dev/uinput.
pkill -TERM -x rawaccel-daemon 2>/dev/null || true
sleep 0.3
pkill -KILL -x rawaccel-daemon 2>/dev/null || true
# Clean up PID/socket artefacts
rm -f /run/rawaccel.pid /run/rawaccel.sock /tmp/rawaccel.pid /tmp/rawaccel.sock 2>/dev/null || true

# Remove service file
echo "[2/4] Removing service file..."
rm -f /etc/systemd/system/rawaccel.service
systemctl daemon-reload

# Remove binaries
echo "[3/4] Removing binaries..."
rm -f /usr/local/bin/rawaccel-daemon
rm -f /usr/local/bin/rawaccel-cli
rm -f /usr/local/bin/rawaccel-gui

# Remove system files
echo "[4/4] Removing system files..."
rm -f /etc/udev/rules.d/99-rawaccel.rules
rm -f /etc/modules-load.d/rawaccel.conf
rm -f /usr/share/applications/rawaccel.desktop
rm -f /usr/share/polkit-1/actions/org.rawaccel.policy
rm -f /usr/share/polkit-1/rules.d/49-rawaccel.rules
# libinput "trackball" quirk for the (RawAccel) virtual mouse — installed
# by setup.sh.  Removing it restores libinput's default acceleration profile
# behaviour for any future virtual device with the same name.
rm -f /etc/libinput/local-overrides.quirks
rm -f /etc/modprobe.d/rawaccel.conf
udevadm control --reload-rules 2>/dev/null || true

# Keep /etc/rawaccel/settings.json so config is not lost
echo ""
echo "=== Uninstall complete! ==="
echo ""
echo "NOTE: /etc/rawaccel/settings.json was kept (your config)."
echo "      To remove it too: sudo rm -rf /etc/rawaccel"
echo ""
echo "NOTE: User config ~/.config/rawaccel/ was not touched."
