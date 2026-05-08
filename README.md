# RawAccel Linux

A Linux port of [Windows Raw Accel](https://github.com/a1xd/rawaccel), using the same acceleration algorithms.

## Features

- **Same algorithms as Raw Accel**: Classic, Power, Natural, Jump, Synchronous, Lookup Table
- **Userspace daemon**: kernel-level feel via `libevdev` + `uinput`
- **GTK4 GUI**: similar interface to Raw Accel, with a real-time gain curve graph
- **CLI**: full control via `rawaccel-cli`
- **JSON config**: `~/.config/rawaccel/settings.json`
- **Multi-profile + per-device assignment**: different settings per mouse
- **systemd service**: automatic startup support
- **Hot-plug**: automatically picks up mice connected/disconnected at runtime
- **Live reload**: config changes apply instantly without releasing the mouse grab

## Dependencies

```bash
# Arch Linux
sudo pacman -S libevdev gtk4 base-devel cmake

# Debian/Ubuntu
sudo apt install libevdev-dev libgtk-4-dev build-essential cmake
```

## Build

```bash
# Quick build (no cmake required)
bash scripts/build.sh

# Portable binary (no -march=native, runs on other CPUs)
RAWACCEL_PORTABLE=1 bash scripts/build.sh

# With CMake
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

Compiled binaries are placed in `build-manual/`.

## Installation

```bash
sudo bash scripts/install.sh
```

Or manually:
```bash
sudo cp build-manual/rawaccel-daemon /usr/local/bin/
sudo cp build-manual/rawaccel-cli    /usr/local/bin/
sudo cp build-manual/rawaccel-gui    /usr/local/bin/
sudo systemctl enable --now rawaccel
```

## Usage

### GUI

```bash
rawaccel-gui
```

### CLI

```bash
# List all profiles
rawaccel-cli list

# Create a new profile
rawaccel-cli create gaming

# Set parameters
rawaccel-cli set-param gaming mode classic
rawaccel-cli set-param gaming acceleration 0.005
rawaccel-cli set-param gaming exponent_classic 2
rawaccel-cli set-param gaming limit 1.8
rawaccel-cli set-param gaming dpi 800

# Switch active profile (signals daemon to reload)
rawaccel-cli set gaming

# Export/import as JSON
rawaccel-cli export gaming > backup.json
rawaccel-cli import backup.json
```

### Daemon

```bash
# Start manually
sudo rawaccel-daemon

# Verbose mode: shows device open/uinput creation details
sudo rawaccel-daemon -v

# With systemd
sudo systemctl start rawaccel
sudo systemctl enable rawaccel   # start on boot

# Reload config without restarting
rawaccel-cli reload
# or
kill -HUP $(cat /run/rawaccel.pid)
```

## Parameters

### Acceleration

| Parameter | Description | Default |
|-----------|-------------|---------|
| `mode` | `classic`, `power`, `natural`, `jump`, `synchronous`, `lookup`, `noaccel` | `noaccel` |
| `gain` | Enable gain mode (recommended) | `true` |
| `acceleration` | Acceleration multiplier | `0.005` |
| `exponent_classic` | Classic mode exponent | `2.0` |
| `exponent_power` | Power/synchronous mode exponent | `0.05` |
| `limit` | Maximum gain asymptote (jump/natural) | `1.5` |
| `decay_rate` | Natural mode decay rate | `0.1` |
| `motivity` | Compatibility field; stored but currently not used by Natural mode | `1.5` |
| `gamma` | Compatibility field; stored but currently not used by Classic mode | `1.0` |
| `input_offset` | Speed threshold before acceleration starts (ips) | `0` |
| `output_offset` | Output offset (power mode) | `0` |
| `scale` | Scale factor (power mode) | `1.0` |
| `sync_speed` | Synchronous mode reference speed (ips) | `5.0` |
| `smooth` | Jump sigmoid smoothing | `0.5` |
| `cap_x` | Input speed cap (ips) | `15` |
| `cap_y` | Output gain cap | `1.5` |
| `cap_mode` | Cap mode: `out`, `in`, `io` | `out` |

### Motion

| Parameter | Description | Default |
|-----------|-------------|---------|
| `rotation` | Axis rotation in degrees | `0` |
| `snap` | Angle snap in degrees | `0` |
| `speed_min` | Minimum speed clamp (ips), `0` = disabled | `0` |
| `speed_max` | Maximum speed clamp (ips), `0` = disabled | `0` |

### Speed Processor

| Parameter | Description | Default |
|-----------|-------------|---------|
| `distance_mode` | How speed is computed: `euclidean`, `max`, `lp`, `separate` | `euclidean` |
| `lp_norm` | Lp-norm exponent (only when `distance_mode=lp`) | `2.0` |
| `input_smooth_halflife` | EMA half-life for input speed smoothing (ms, `0` = off) | `0` |
| `scale_smooth_halflife` | EMA half-life for scale smoothing (ms, `0` = off) | `0` |
| `output_smooth_halflife` | EMA half-life for output speed smoothing (ms, `0` = off) | `0` |

### Device

| Parameter | Description | Default |
|-----------|-------------|---------|
| `dpi` | Mouse DPI | `800` |
| `polling_rate` | Mouse polling rate (Hz) | `1000` |
| `output_dpi` | Output count normalization (`1000` = no extra output scale) | `1000` |
| `lr_ratio` | Left/right output DPI ratio (`1.0` = off) | `1.0` |
| `ud_ratio` | Up/down output DPI ratio (`1.0` = off) | `1.0` |

## Multi-Mouse / Per-Device Profile Assignment

RawAccel Linux supports assigning different profiles to different mice.

### How it works

The daemon identifies each physical mouse using two methods (in order of preference):

1. **USB serial number** (`EVIOCGUNIQ` ioctl) — stable across reboots, unique per device
2. **Event node path** (`/dev/input/eventN`) — fallback when serial is empty; may change across reboots

When a mouse connects, the daemon selects its profile using this priority:

1. **Device-specific profile**: a profile whose `device_id` matches the mouse's identifier
2. **Active profile**: the currently selected profile (applies to all unmatched mice)
3. **First profile**: the first profile in the list (last resort fallback)

### Setting up per-device profiles

**Via GUI:**
1. Open RawAccel GUI
2. Create or select a profile
3. In the **Device Assignment** section, choose the target mouse from the dropdown
4. Save — the daemon will apply this profile only to that mouse

**Via CLI:**
```bash
# List currently connected mice (shown with event nodes)
rawaccel-cli status

# The daemon stores the event node as device_id in the profile JSON
# Example: assign "office-mouse" profile to /dev/input/event4
rawaccel-cli set-param office-mouse device_id /dev/input/event4
# Note: device_id is a string field — set it directly in the JSON for reliability

# Or edit the JSON directly:
# ~/.config/rawaccel/settings.json
# Set "device_id": "/dev/input/event4" in the profile object
```

**Via JSON:**
```json
{
  "active_profile": "gaming",
  "profiles": [
    {
      "name": "gaming",
      "device_id": "",
      "dpi": 1600,
      "polling_rate": 1000,
      "profile": { ... }
    },
    {
      "name": "office",
      "device_id": "/dev/input/event4",
      "dpi": 800,
      "polling_rate": 125,
      "profile": { ... }
    }
  ]
}
```

### Notes

- An **empty `device_id`** means the profile applies to all mice not matched by another profile
- If multiple profiles have the same `device_id`, the first match wins
- The active profile (selected via GUI or `rawaccel-cli set`) applies to all mice with no specific assignment
- Event node paths (`/dev/input/eventN`) can change across reboots if you plug/unplug devices; USB serial numbers are more reliable when available

## Setup

### Add user to `input` group (run daemon without root)

```bash
sudo usermod -aG input $USER
# Log out and back in for the change to take effect
```

### Load the `uinput` module

```bash
sudo modprobe uinput
# Make it persistent:
echo "uinput" | sudo tee /etc/modules-load.d/rawaccel.conf
```

## How it works

1. `rawaccel-daemon` scans `/dev/input/event*` for physical mice
2. Each mouse is grabbed with `EVIOCGRAB` (raw events go only to the daemon)
3. A virtual mouse is created via `libevdev-uinput`
4. For each movement event, the daemon computes speed in ips (using DPI + event timing)
5. The Raw Accel algorithm is applied (gain multiplier)
6. The result is written to the virtual device → seen by XOrg/Wayland
7. Config reloads are **live** (no grab release): settings update in-place without any mouse dropout

## Architecture

```
/dev/input/eventX  (physical mouse)
        │
        ▼ EVIOCGRAB
   rawaccel-daemon
        │ acceleration algorithm
        ▼
/dev/input/uinput  (virtual mouse)
        │
        ▼
   XOrg / Wayland
```

## File structure

```
rawaccel-linux/
├── include/
│   ├── math-vec2.hpp          # Vector math
│   ├── rawaccel-base.hpp      # Core types, structs, RAWACCEL_VERSION
│   ├── accel-classic.hpp      # Classic acceleration
│   ├── accel-power.hpp        # Power acceleration
│   ├── accel-natural.hpp      # Natural acceleration
│   ├── accel-jump.hpp         # Jump (sigmoid) acceleration
│   ├── accel-synchronous.hpp  # Synchronous acceleration
│   ├── accel-lookup.hpp       # Lookup table acceleration
│   ├── accel-union.hpp        # Union of all accel modes
│   ├── rawaccel.hpp           # Main modifier + EMA smoother engine
│   └── config.hpp             # Config structs
├── src/
│   └── config.cpp             # JSON serialization (nlohmann/json)
├── daemon/
│   ├── daemon.hpp             # AccelDaemon class declaration
│   ├── daemon.cpp             # evdev/uinput implementation, hot-plug
│   └── main.cpp               # Daemon entry point, PID file, signal handling
├── cli/
│   └── main.cpp               # rawaccel-cli
├── gui/
│   └── main.cpp               # rawaccel-gui (GTK4)
├── scripts/
│   ├── build.sh               # Quick build script
│   ├── install.sh             # Installation script
│   ├── rawaccel.service       # systemd service
│   └── rawaccel.desktop       # .desktop file
└── CMakeLists.txt
```

## GUI Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| `Ctrl+S` | Save profile |
| `Ctrl+N` | New profile |
| `Ctrl+D` | Duplicate current profile |
| `Ctrl+R` | Reload daemon config |
| `F5`     | Refresh device list |

## Performance

RawAccel Linux processes mouse events with sub-microsecond latency in the daemon's hot path:

- **Processing latency** (modifier math + uinput write): typically **< 5 µs** per event
- **Event loop**: epoll-based, 10 ms timeout — no busy-wait, minimal CPU usage
- **Algorithm overhead**: all algorithms are header-only, compiler-inlined
- **Subpixel accumulation**: no micro-movements are silently lost

To measure live latency statistics, send `SIGUSR1` to the daemon:

```bash
kill -USR1 $(cat /run/rawaccel.pid)
# Output appears in journald: journalctl -u rawaccel -f
```

## KDE Plasma Setup

KDE applies its own **libinput acceleration curve** on top of raw mouse input.
When RawAccel is active, this causes **double-acceleration** — KDE's curve runs
before RawAccel, compounding both effects.  You must set KDE's pointer
acceleration to **Flat** (disabled) for RawAccel to be the sole accelerator.

### Automatic fix (recommended)

```bash
# One-time fix — no logout required
bash scripts/kde-fix-accel.sh

# Check current state
bash scripts/kde-fix-accel.sh --check

# Undo (restore KDE adaptive acceleration)
bash scripts/kde-fix-accel.sh --undo
```

The fix script:
1. Sets `PointerAccelerationProfile=1` (Flat) in `~/.config/kwinrc`
2. Reloads KWin input settings immediately via D-Bus (`qdbus org.kde.KWin /KWin reconfigure`)

### Manual fix

**KDE System Settings:**
1. Open **System Settings → Input Devices → Mouse**
2. Set **Pointer Acceleration** to **Flat** (the leftmost preset)
3. Click **Apply**

**Or via CLI (kwriteconfig5 / kwriteconfig6):**
```bash
# Plasma 5
kwriteconfig5 --file kwinrc --group Libinput --key PointerAccelerationProfile 1
kwriteconfig5 --file kwinrc --group Libinput --key PointerAcceleration 0
qdbus org.kde.KWin /KWin reconfigure

# Plasma 6
kwriteconfig6 --file kwinrc --group Libinput --key PointerAccelerationProfile 1
kwriteconfig6 --file kwinrc --group Libinput --key PointerAcceleration 0
qdbus6 org.kde.KWin /KWin reconfigure
```

### GUI warning

The **RawAccel GUI** automatically detects KDE sessions and checks whether
libinput acceleration is disabled.  If it detects double-acceleration is likely,
an orange warning banner appears with a **Fix Now** button that applies the fix
without requiring any manual steps.

### KDE + Wayland

On **KDE Wayland** sessions the systemd service starts after `plasma-kwin_wayland.service`
(soft dependency via `Wants=`/`After=`) to ensure the virtual uinput device created
by the daemon is visible to the compositor on first boot.

## Wayland / X11 Compatibility

RawAccel Linux works at the kernel input layer (evdev + uinput), **below** the display server:

- ✅ **X11**: fully compatible — works with any X11 compositor/WM
- ✅ **Wayland**: fully compatible — Wayland compositors receive already-accelerated events from uinput
- ✅ **Display-server agnostic**: no compositor or display server patches required
- ⚠️ **libinput note**: some Wayland compositors apply their own acceleration on top of rawaccel output. Disable compositor acceleration in your DE settings.
  - **KDE**: see [KDE Plasma Setup](#kde-plasma-setup) above
  - **GNOME**: System Settings → Mouse & Touchpad → disable "Mouse Acceleration"

## Known Issues

- **Device ID instability**: on kernels without by-id udev rules, `eventN` numbers can change across reboots. RawAccel GUI resolves `/dev/input/by-id/...` stable paths automatically — but only if the device has a unique USB ID.
- **Multi-DPI sensors**: some mice report different DPI values than configured. Always verify DPI with a measurement tool.
- **Large LUT tables**: LUT mode is capped at 257 points (514 floats). Larger tables are silently truncated.

## Troubleshooting

**Daemon won't start / no mice found:**
- Add yourself to the `input` group: `sudo usermod -aG input $USER`, then re-login
- Load uinput: `sudo modprobe uinput`
- Stop conflicting software: `sudo systemctl stop abrek`

**Mouse is grabbed but no output:**
- Check `rawaccel-daemon -v` for virtual device creation errors
- Make sure `uinput` module is loaded

**Settings not applying:**
- Run `rawaccel-cli status` to check if daemon is running and see profile details
- Run `rawaccel-cli reload` to trigger a config reload
- Check `~/.config/rawaccel/settings.json` exists and is valid JSON

**Wayland compositor applies double acceleration (KDE):**
- Run `bash scripts/kde-fix-accel.sh` (automatic one-step fix)
- Or: System Settings → Input Devices → Mouse → Pointer Acceleration = **Flat**
- The GUI will show an orange warning banner with a **Fix Now** button

**Wayland compositor applies double acceleration (GNOME):**
- System Settings → Mouse & Touchpad → disable "Mouse Acceleration"

**Config file location:**
- Running systemd daemon: `/etc/rawaccel/settings.json`
- GUI/CLI ask the running daemon for its config path over IPC and edit the same file
- Daemon stopped / standalone tools: `~/.config/rawaccel/settings.json`
- Override: `rawaccel-daemon -c /path/to/settings.json` or `rawaccel-cli -c /path/to/settings.json`
