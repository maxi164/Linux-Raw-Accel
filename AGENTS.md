# RawAccel Linux — Developer Notes

This file documents build, test, and verification commands.

## Build

```bash
# Standard build (-march=native, fastest)
bash scripts/build.sh

# Portable build (works on other CPU architectures)
RAWACCEL_PORTABLE=1 bash scripts/build.sh

# Custom compiler
CXX=clang++ bash scripts/build.sh
```

Output binaries: `build-manual/rawaccel-daemon`, `build-manual/rawaccel-cli`, `build-manual/rawaccel-gui`

## Test

```bash
# All unit tests (compile + run)
bash tests/run_tests.sh

# Same tests under AddressSanitizer + UBSan (slower, catches memory/UB bugs)
bash tests/run_tests_asan.sh

# Expected output: "=== Sonuç: N/N geçti ===" (N/N passed)
# Exits with code 1 if any FAIL line appears.
```

Test file: `tests/test_accel.cpp`
- No external dependencies (standard C++20 + project headers)
- Each `SECTION()` is an independent test group
- Assertions use `EXPECT` / `EXPECT_NEAR` macros
- 99 test functions, 783 runtime assertions covering: algorithms, JSON round-trips,
  file I/O, input validation, multi-profile round-trip, atomic write, IPC JSON,
  config error paths, LUT sort, int overflow guard, NaN/Inf remainder guard,
  accel_args sanitize, fuzz tests, extreme speeds, EMA stability, subpixel
  accumulation, modifier flags, stress tests, DPI ratio div-by-zero guard,
  lp_distance zero-vector guard, EMA extreme halflife, NaN pipeline injection,
  classic io degenerate cap, power output offset, directional weight boundary,
  1M-iteration subpixel drift, classic monotonicity, natural gain formula,
  EMA smoother half-life/convergence, linear EMA smoother, NaN propagation all
  modes, pathological params, event batching accumulation/split, speed processor
  distance modes + smoothing, SYN_DROPPED reset, config empty profiles / missing
  active profile / extreme values / duplicate device IDs,
  sanitize NaN/Inf in all fields, subnormal time guard,
  classic sign flip (io cap.y < 1), classic linear path (exp<=1) cap,
  power cap branch (all 3 cap modes), modifier rotation + snap combined,
  speed_processor Lp/max/separate modes, lookup LUT max capacity,
  EMA coefficient=0 path, directional weight cos/sin blend,
  speed clamp min/max, dir mul negative direction, synchronous power<1 guard,
  natural legacy (non-gain) mode, output_dpi NaN sanitize,
  lat_stats move semantics, dpi_factor pre-compute consistency,
  magnitude hypot overflow safety

## Fuzz Testing

```bash
# Requires clang++ with libFuzzer support
bash tests/run_fuzz.sh          # 60 seconds per harness (default)
bash tests/run_fuzz.sh 300      # 300 seconds per harness
```

Two harnesses:
- `tests/fuzz_config.cpp` — JSON parser + sanitize (config round-trip)
- `tests/fuzz_accel.cpp` — acceleration pipeline (modifier + motion_math)

Seed corpus: `tests/corpus_config/`

## Continuous Integration

GitHub Actions workflow: `.github/workflows/ci.yml`

Three jobs run on every push/PR (Ubuntu 24.04):
- **build-and-test** — portable build (`RAWACCEL_PORTABLE=1`), warning-as-failure gate
  via `grep -E "warning:|error:"`, then `tests/run_tests.sh`.
- **sanitizers** — rebuilds tests with `-fsanitize=address,undefined` and runs them
  with `halt_on_error=1` so any leak/UB fails CI.
- **fuzz-smoke** — 60 s per harness via `tests/run_fuzz.sh 60`. Skipped on PRs to
  keep them fast; runs on push to main and on `workflow_dispatch`. Crash inputs
  are uploaded as artifacts on failure.

Concurrency group cancels superseded runs on the same ref.

## Lint / Warning Check

```bash
# Build with -Wall -Wextra (should produce 0 warnings)
bash scripts/build.sh 2>&1 | grep -E "warning:|error:"
```

## Version Update

Version number lives in a single place: `include/rawaccel-base.hpp` → `RAWACCEL_VERSION`
Changing it automatically propagates to the daemon, CLI, and GUI.

## File Responsibilities

| File/Directory | Contents |
|----------------|----------|
| `include/accel-*.hpp` | Acceleration algorithms (header-only) |
| `include/rawaccel.hpp` | Modifier + EMA smoother engine |
| `include/rawaccel-base.hpp` | Core types, structs, RAWACCEL_VERSION |
| `include/config.hpp` | Config structs |
| `src/config.cpp` | JSON serialization (nlohmann/json) |
| `daemon/daemon.cpp` | evdev/uinput implementation, hot-plug |
| `daemon/main.cpp` | Daemon entry point, PID file, signal handling |
| `cli/main.cpp` | rawaccel-cli commands |
| `gui/main.cpp` | rawaccel-gui entry point + helpers (~107 lines) |
| `gui/app_state.hpp` | AppState struct, shared includes, forward declarations |
| `gui/devices.inl` | Mouse discovery (stable by-id paths), inotify hot-plug |
| `gui/daemon_comm.inl` | Daemon PID lookup, status display, signal sending |
| `gui/graph.inl` | Cairo curve rendering, LUT editor |
| `gui/widgets_sync.inl` | Widget ↔ profile sync, GTK callbacks |
| `gui/profile_mgr.inl` | Profile CRUD dialogs |
| `gui/ui_builder.inl` | Layout helpers, build_ui(), window-close, on_activate() |
| `tests/test_accel.cpp` | Unit + integration tests (783 assertions, 99 functions) |
| `tests/fuzz_config.cpp` | libFuzzer harness — config JSON parsing |
| `tests/fuzz_accel.cpp` | libFuzzer harness — acceleration pipeline |
| `tests/run_fuzz.sh` | Fuzz test runner (both harnesses) |
| `tests/run_tests.sh` | Unit test runner (compile + run) |
| `tests/run_tests_asan.sh` | Unit test runner under ASan + UBSan |
| `scripts/build.sh` | Quick build script |
| `.github/workflows/ci.yml` | GitHub Actions CI (build + tests + sanitizers + fuzz smoke) |

## Key Design Decisions

- **PID file priority**: `$XDG_RUNTIME_DIR/rawaccel.pid` → `/run/rawaccel.pid` → `/tmp/rawaccel.pid`
- **Signal safety**: signal handler only sets an atomic flag (`request_stop()`), never joins threads
- **Sub-pixel accumulation**: `remainder_x/y` carries fractional movement so no micro-moves are lost
- **Float comparisons**: use epsilon (`1e-9`) instead of `!= 0` / `!= 1`
- **Atomic config write**: tmp file → `rename()` so the daemon never reads a half-written JSON
- **Live reload (R5 fix)**: config reload updates settings in-place without releasing the mouse grab — no dropout window
- **Stable device IDs**: GUI and daemon both resolve `eventN` → `/dev/input/by-id/...` for reboot-stable profile assignment
- **Input validation**: `sanitize_device_profile()` clamps DPI (1–32 000), polling rate (125–8 000 Hz), rotation (0–360°), snap (0–45°), output DPI, speed_max ≥ speed_min, accel_args fields (acceleration, scale, decay_rate, exponent_power ≥ 1e-4, offsets ≥ 0, limit ≥ 0, sync_speed ≥ 1e-4, smooth ≥ 0, motivity/gamma ≥ 0, cap ≥ 0), domain/range weights ≥ 0, smooth halflifes ≥ 0 — called on every JSON load
- **Systemd hardening**: `NoNewPrivileges`, `MemoryDenyWriteExecute`, `RestrictNamespaces`, `RestrictAddressFamilies=AF_UNIX AF_NETLINK`, `ProtectKernelModules/Tunables/ControlGroups`, `LockPersonality`, `RestrictRealtime`
- **Verbose log**: `daemon -v` shows device open/uinput creation details
- **uinput_write error handling**: all `libevdev_uinput_write_event()` calls are wrapped by `uinput_write()` which checks the return value and marks the device as disconnected on failure
- **SYN_DROPPED handling**: when kernel reports `SYN_DROPPED` (event buffer overflow), a `syn_dropped` flag is set; ALL subsequent events (motion, buttons, etc.) are discarded until the next `SYN_REPORT` clears the flag — per the Linux input protocol, events between SYN_DROPPED and SYN_REPORT are unreliable
- **IPC reload command**: `"reload\n"` via Unix socket schedules config reload (alternative to SIGHUP); GUI prefers IPC then falls back to SIGHUP
- **NaN sanitization**: `sanitize_accel_args()` and `sanitize_profile()` replace all NaN/Inf double fields (including `output_dpi`) with safe defaults before range-clamping (NaN silently passes `<`/`>` comparisons)
- **Subnormal time guard**: `modifier::modify()` clamps `ips_factor` to 0 when `dpi_factor/time` overflows to Inf (subnormal time values like 1e-309)
- **Modify output guard**: defense-in-depth `isfinite()` check at the end of `modifier::modify()` ensures no NaN/Inf escapes to motion_math
- **Duplicate device_id warning**: GUI warns on startup and on save if multiple profiles share the same device_id (first-match-wins in daemon)
- **lat_stats move safety**: move constructor/assignment lock the source mutex before copying data (defense-in-depth against concurrent record() during vector reallocation)
- **Pre-computed dpi_factor**: `mouse_device::dpi_factor` is computed once in `apply_profile()` instead of dividing on every mouse event — eliminates a floating-point division from the hot path
- **Overflow-safe magnitude**: `magnitude()` uses `std::hypot(x,y)` instead of `sqrt(x*x+y*y)` — prevents intermediate overflow/underflow for extreme delta values
- **Unified clock source**: both `now_ms()` and `now_ns()` use `CLOCK_MONOTONIC_RAW` — eliminates drift between timing sources and reduces syscalls per event from 3 to 2
- **Y-axis unlinked field sync**: when X/Y axes are unlinked, fields without dedicated Y widgets (cap_mode, exponent_power, decay_rate, scale, output_offset, motivity, gamma, smooth, sync_speed) are copied from X to prevent stale values
- **Widget sensitivity refactor**: raw passthrough grey-out logic extracted to `update_raw_sensitivity()` — single source of truth for 18 widget enable/disable calls

## Known Limitations

- GUI uses `.inl` file compilation (single translation unit) — GTK4 C callback ABI makes true class-based split impractical without a full rewrite
- Test infrastructure is simple (no external framework) — no parallel test support
- No end-to-end daemon test with real evdev/uinput (requires root — not run in CI); config/validation/multi-profile covered by integration tests
