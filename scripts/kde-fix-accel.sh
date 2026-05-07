#!/bin/bash
# kde-fix-accel.sh — Disable KDE Plasma mouse acceleration for RawAccel
#
# KDE applies its own libinput acceleration curve on top of your mouse input.
# When RawAccel is running, this causes double-acceleration (two curves stacked).
# This script sets Pointer Acceleration to "Flat" (profile=1) in kwinrc
# and reloads KWin input settings immediately — no logout required.
#
# Usage:
#   bash scripts/kde-fix-accel.sh          # fix + reload
#   bash scripts/kde-fix-accel.sh --check  # check current state only
#   bash scripts/kde-fix-accel.sh --undo   # restore adaptive acceleration

set -euo pipefail

# ── Config file location ──────────────────────────────────────────────────────
KWINRC="${XDG_CONFIG_HOME:-$HOME/.config}/kwinrc"

# ── Helpers ───────────────────────────────────────────────────────────────────

die() { echo "ERROR: $*" >&2; exit 1; }

check_kde() {
    local desktop="${XDG_CURRENT_DESKTOP:-}"
    local session="${DESKTOP_SESSION:-}"
    if [[ "$desktop" == *KDE* || "$desktop" == *plasma* ||
          "$session" == *plasma* || "$session" == *kde* ||
          -f "$KWINRC" ]]; then
        return 0
    fi
    return 1
}

get_current_profile() {
    # Returns the PointerAccelerationProfile value from [Libinput] section, or "" if not set.
    awk '/^\[Libinput\]/{found=1; next} /^\[/{if(found) exit} found && /^PointerAccelerationProfile=/{print $0; exit}' \
        "$KWINRC" 2>/dev/null | cut -d= -f2
}

write_kwinrc() {
    local profile="$1"  # 1 = flat, 2 = adaptive
    local accel="$2"    # 0 for flat, -0.5 or 0 for adaptive

    # Create backup
    [[ -f "$KWINRC" ]] && cp "$KWINRC" "$KWINRC.rawaccel-backup.$(date +%Y%m%d-%H%M%S)"

    # Use python3 for clean INI manipulation (preserves comments/sections).
    # We write BOTH the global [Libinput] section AND per-device overrides
    # for any "(RawAccel)" virtual device found in /proc/bus/input/devices —
    # KDE Plasma 6 stores per-device libinput config as nested sections like
    # [Libinput][bustype][vendor_decimal][product_decimal][Device Name],
    # which override the global setting.
    if command -v python3 &>/dev/null; then
        python3 - "$KWINRC" "$profile" "$accel" << 'PYEOF'
import sys, os, re

kwinrc, profile, accel = sys.argv[1], sys.argv[2], sys.argv[3]

# Read existing file (if any) as raw text so nested [Libinput][a][b]...
# sections (which configparser cannot represent) are preserved.
text = ""
if os.path.exists(kwinrc):
    with open(kwinrc) as f:
        text = f.read()

# 1) Enumerate live RawAccel virtual devices from /proc/bus/input/devices.
devices = []
try:
    with open("/proc/bus/input/devices") as f:
        block = {}
        for line in f:
            line = line.rstrip("\n")
            if not line:
                if block.get("name", "").endswith("(RawAccel)"):
                    devices.append(block)
                block = {}
                continue
            m = re.match(r"I: Bus=([0-9a-fA-F]+) Vendor=([0-9a-fA-F]+) Product=([0-9a-fA-F]+)", line)
            if m:
                block["bus"]     = int(m.group(1), 16)
                block["vendor"]  = int(m.group(2), 16)
                block["product"] = int(m.group(3), 16)
            elif line.startswith('N: Name="'):
                block["name"] = line[len('N: Name="'):-1]
        if block.get("name", "").endswith("(RawAccel)"):
            devices.append(block)
except FileNotFoundError:
    pass

def upsert_section(text, section_header, kv):
    """Replace or append [section_header] block with the given key=value pairs.
    section_header includes the brackets, e.g. '[Libinput]' or '[Libinput][3][1133][...]'.
    kv is a list of (key, value) tuples preserving order.
    """
    # Match this section up to the next top-level (non-nested) section header.
    # Sections in kwinrc are line-anchored.
    esc = re.escape(section_header)
    pattern = rf"(?ms)^{esc}\s*\n(.*?)(?=^\[|\Z)"
    body_lines = "".join(f"{k}={v}\n" for k, v in kv)
    new_block = f"{section_header}\n{body_lines}"
    if re.search(pattern, text):
        return re.sub(pattern, new_block, text, count=1)
    sep = "" if text.endswith("\n") or not text else "\n"
    return text + sep + "\n" + new_block

kv = [("PointerAccelerationProfile", profile), ("PointerAcceleration", accel)]

# 2) Global section (fallback for any device without a specific override)
text = upsert_section(text, "[Libinput]", kv)

# 3) Per-device override for every RawAccel virtual device.
#    Plasma stores these as nested headers like [Libinput][3][1133][50498][Name]
for d in devices:
    header = f"[Libinput][{d['bus']}][{d['vendor']}][{d['product']}][{d['name']}]"
    text = upsert_section(text, header, kv)

# Atomic rename
tmp = kwinrc + ".tmp"
with open(tmp, "w") as f:
    f.write(text)
os.rename(tmp, kwinrc)

print(f"  Updated {kwinrc}")
print(f"  + global [Libinput] → profile={profile}")
if devices:
    for d in devices:
        print(f"  + per-device: {d['name']} (bus={d['bus']}, vid={d['vendor']}, pid={d['product']})")
else:
    print("  (No (RawAccel) virtual devices found — start the daemon first, then re-run.)")
PYEOF
    else
        # Fallback: manual INI editing with sed + append
        if grep -q '^\[Libinput\]' "$KWINRC" 2>/dev/null; then
            sed -i "/^\[Libinput\]/,/^\[/{
                s/^PointerAccelerationProfile=.*/PointerAccelerationProfile=$profile/
                s/^PointerAcceleration=.*/PointerAcceleration=$accel/
            }" "$KWINRC"
            # If keys not present in section, append them
            if ! grep -A20 '^\[Libinput\]' "$KWINRC" | grep -q '^PointerAccelerationProfile='; then
                sed -i "/^\[Libinput\]/a PointerAccelerationProfile=$profile" "$KWINRC"
            fi
            if ! grep -A20 '^\[Libinput\]' "$KWINRC" | grep -q '^PointerAcceleration='; then
                sed -i "/^\[Libinput\]/a PointerAcceleration=$accel" "$KWINRC"
            fi
        else
            printf '\n[Libinput]\nPointerAccelerationProfile=%s\nPointerAcceleration=%s\n' \
                "$profile" "$accel" >> "$KWINRC"
        fi
    fi
}

reload_kwin() {
    echo "  Reloading KWin input settings..."
    local reloaded=0
    for cmd in qdbus6 qdbus; do
        if command -v "$cmd" &>/dev/null; then
            if "$cmd" org.kde.KWin /KWin reconfigure 2>/dev/null; then
                echo "  ✓ KWin reconfigured via $cmd."
                reloaded=1
                break
            fi
        fi
    done
    if [[ $reloaded -eq 0 ]]; then
        echo "  ℹ Could not reach KWin D-Bus. Changes will apply on next KWin start."
        echo "    (If running under Wayland, log out and back in.)"
    fi
}

# ── Main ──────────────────────────────────────────────────────────────────────

MODE="${1:---fix}"

case "$MODE" in
--check)
    echo "=== KDE Acceleration State Check ==="
    if ! check_kde; then
        echo "  Not a KDE session (kwinrc not found). Nothing to do."
        exit 0
    fi
    PROFILE=$(get_current_profile)
    echo "  kwinrc: $KWINRC"
    echo "  PointerAccelerationProfile: '${PROFILE:-not set}'"
    if [[ "$PROFILE" == "1" ]]; then
        echo "  ✓ Flat (disabled) — correct for RawAccel."
        exit 0
    else
        echo "  ⚠ NOT flat — double-acceleration will occur with RawAccel!"
        echo "  Run: bash scripts/kde-fix-accel.sh"
        exit 1
    fi
    ;;

--undo)
    echo "=== Restoring KDE adaptive acceleration ==="
    if ! check_kde; then
        echo "  Not a KDE session. Nothing to do."; exit 0
    fi
    write_kwinrc "2" "-0.5"
    reload_kwin
    echo "  ✓ Restored adaptive acceleration (profile=2, accel=-0.5)."
    echo "  Note: adjust the exact value in System Settings → Input Devices → Mouse."
    ;;

--fix|*)
    echo "=== KDE RawAccel Fix: Disable Pointer Acceleration ==="
    if ! check_kde; then
        echo "  Not a KDE session (XDG_CURRENT_DESKTOP=$XDG_CURRENT_DESKTOP)."
        echo "  This script is only needed on KDE Plasma. Exiting."
        exit 0
    fi

    PROFILE=$(get_current_profile)
    if [[ "$PROFILE" == "1" ]]; then
        echo "  ✓ Pointer acceleration is already Flat. No change needed."
        exit 0
    fi

    echo "  Current profile: '${PROFILE:-not set}'"
    echo "  Setting PointerAccelerationProfile=1 (Flat) in: $KWINRC"
    write_kwinrc "1" "0"
    reload_kwin
    echo ""
    echo "  ✓ Done. KDE will no longer apply an extra acceleration curve."
    echo "  RawAccel is now the sole acceleration provider."
    echo ""
    echo "  To verify: bash scripts/kde-fix-accel.sh --check"
    echo "  To undo:   bash scripts/kde-fix-accel.sh --undo"
    ;;
esac
