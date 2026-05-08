#include "daemon.hpp"
#include "motion_math.hpp"
#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>

#include <linux/input.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <poll.h>
#include <fcntl.h>
#include <grp.h>
#include <unistd.h>
#include <dirent.h>
#include <glob.h>
#include <cstring>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <time.h>

namespace rawaccel {

// ── Timing helper ─────────────────────────────────────────────────────────────

// R13-perf: unified CLOCK_MONOTONIC_RAW for both ms and ns timestamps.
// Previously flush_motion() called steady_clock (vDSO) + CLOCK_MONOTONIC_RAW
// separately — 3 syscalls per event.  Now only 2 (start + end), both from the
// same clock source, eliminating drift between the two clocks.
static double now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<double>(ts.tv_sec) * 1000.0 +
           static_cast<double>(ts.tv_nsec) / 1'000'000.0;
}

// ── sysfs device property helpers ────────────────────────────────────────────

/// Extract the event number from a /dev/input/eventN or /dev/input/by-id/... path.
/// Returns -1 on failure.
static int event_num_from_path(const std::string& path) {
    // Resolve symlinks to get the real /dev/input/eventN path
    char real[PATH_MAX] = {};
    const char* p = realpath(path.c_str(), real) ? real : path.c_str();

    // Find "event" and parse the number after it
    const char* ev = strstr(p, "event");
    if (!ev) return -1;
    ev += 5; // skip "event"
    errno = 0;
    char* end = nullptr;
    long n = strtol(ev, &end, 10);
    // BUG-7: long → int cast UB if n out of int range. Linux event numbers
    // are <1000 in practice, but be defensive.
    if (end == ev || errno != 0 || n < 0 || n > INT_MAX) return -1;
    return static_cast<int>(n);
}

/// Read a single integer from a sysfs file. Returns -1 on failure.
static int sysfs_read_int(const std::string& sysfs_path) {
    FILE* f = fopen(sysfs_path.c_str(), "r");
    if (!f) return -1;
    char buf[32] = {};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return -1;
    errno = 0;
    char* end = nullptr;
    long val = std::strtol(buf, &end, 10);
    // BUG-7: long → int cast UB if val out of int range.  Sysfs may
    // legitimately expose values like 0xffffffff for some properties.
    if (end == buf || errno != 0 || val < INT_MIN || val > INT_MAX)
        return -1;
    return static_cast<int>(val);
}

/// Detect mouse polling rate (Hz) from sysfs.
/// Returns detected rate clamped to [POLL_RATE_MIN, POLL_RATE_MAX], or 0 if unknown.
static int detect_polling_rate(const std::string& event_path) {
    int n = event_num_from_path(event_path);
    if (n < 0) return 0;

    char buf[256];
    // 1. Direct polling_rate sysfs node (some HID drivers expose this)
    snprintf(buf, sizeof(buf), "/sys/class/input/event%d/device/polling_rate", n);
    int rate = sysfs_read_int(buf);
    if (rate > 0)
        return std::clamp(rate, (int)POLL_RATE_MIN, (int)POLL_RATE_MAX);

    // 2. USB bInterval (polling interval in ms for USB HID)
    //    bInterval is in frames (1 ms for full-speed USB, 0.125 ms for high-speed).
    //    /sys/class/input/eventN/device/device/bInterval is the USB endpoint interval.
    snprintf(buf, sizeof(buf), "/sys/class/input/event%d/device/device/bInterval", n);
    int binterval = sysfs_read_int(buf);
    if (binterval > 0) {
        // bInterval for high-speed USB is in 125µs units; for full-speed in 1ms.
        // Most gaming mice are high-speed USB.
        // High-speed: bInterval=1 → 8000 Hz, bInterval=4 → 2000 Hz, bInterval=8 → 1000 Hz.
        // Full-speed: bInterval=1 → 1000 Hz, bInterval=10 → 100 Hz.
        // Heuristic: if bInterval <= 8, treat as high-speed (125µs per frame).
        int rate_hz;
        if (binterval <= 8)
            rate_hz = 8000 / binterval;       // high-speed: 125µs × bInterval per poll
        else
            rate_hz = 1000 / binterval;       // full-speed: 1ms × bInterval per poll
        if (rate_hz > 0)
            return std::clamp(rate_hz, (int)POLL_RATE_MIN, (int)POLL_RATE_MAX);
    }

    return 0; // unknown
}

/// Detect mouse DPI from sysfs resolution node.
/// Returns detected DPI clamped to [100, 32000], or 0 if unknown.
static int detect_dpi_sysfs(int event_n) {
    char buf[256];
    // Some drivers expose resolution in counts/mm under:
    // /sys/class/input/eventN/device/resolution
    snprintf(buf, sizeof(buf), "/sys/class/input/event%d/device/resolution", event_n);
    int res_counts_per_mm = sysfs_read_int(buf);
    if (res_counts_per_mm > 0) {
        // BUG-7-2: float → int cast UB if res * 25.4 exceeds INT_MAX.
        // sysfs_read_int already clamps to [INT_MIN, INT_MAX] so the
        // multiplication here can overflow into ~5.4e10 for INT_MAX
        // input.  Compute in double then clamp before the cast.
        double dpi_f = static_cast<double>(res_counts_per_mm) * 25.4;
        dpi_f = std::clamp(dpi_f, 100.0, 32000.0);
        return static_cast<int>(dpi_f);
    }
    return 0;
}

// ── Device discovery ──────────────────────────────────────────────────────────

/// Check if an evdev device is a physical mouse (REL_X + REL_Y, not virtual).
static bool is_physical_mouse(int fd) {
    libevdev* dev = nullptr;
    if (libevdev_new_from_fd(fd, &dev) < 0) return false;

    bool has_motion = libevdev_has_event_code(dev, EV_REL, REL_X) &&
                      libevdev_has_event_code(dev, EV_REL, REL_Y);

    bool is_virtual = false;
    const char* name = libevdev_get_name(dev);
    if (name) {
        std::string sname(name);
        if (sname.size() >= 10 &&
            sname.compare(sname.size() - 10, 10, "(RawAccel)") == 0)
            is_virtual = true;
        const char* phys = libevdev_get_phys(dev);
        if (phys && std::string(phys).find("uinput") != std::string::npos)
            is_virtual = true;
    }

    libevdev_free(dev);
    return has_motion && !is_virtual;
}

/// Resolve a /dev/input/eventN path to its stable /dev/input/by-id/... symlink.
/// Returns the by-id path if found, otherwise returns the original path.
/// Stable IDs match what the GUI stores in device_profile.device_id.
static std::string resolve_stable_id(const std::string& event_node) {
    const char* by_id = "/dev/input/by-id";
    DIR* dir = opendir(by_id);
    if (!dir) return event_node;

    char real_event[PATH_MAX] = {};
    if (!realpath(event_node.c_str(), real_event)) { closedir(dir); return event_node; }

    struct dirent* ent;
    std::string best;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        std::string link = std::string(by_id) + "/" + ent->d_name;
        char target[PATH_MAX] = {};
        if (!realpath(link.c_str(), target)) continue;
        if (std::string(target) == std::string(real_event)) {
            if (best.empty() || link.find("-event-mouse") != std::string::npos)
                best = link;
        }
    }
    closedir(dir);
    return best.empty() ? event_node : best;
}

/// List all physical mouse event paths in /dev/input/, using stable by-id paths.
static std::vector<std::string> find_mice() {
    std::vector<std::string> result;
    glob_t g{};
    if (glob("/dev/input/event*", 0, nullptr, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            int fd = open(g.gl_pathv[i], O_RDONLY | O_NONBLOCK);
            if (fd < 0) {
                // D3: access error — permission issue or device already grabbed (informational)
                continue;
            }
            if (is_physical_mouse(fd))
                result.push_back(resolve_stable_id(g.gl_pathv[i]));
            close(fd);
        }
    }
    globfree(&g);
    return result;
}

// ── AccelDaemon ───────────────────────────────────────────────────────────────

AccelDaemon::AccelDaemon() = default;

AccelDaemon::~AccelDaemon() {
    // Stop IPC thread first — otherwise if start() failed and the user
    // forgot to call stop_ipc_server(), the std::thread destructor would
    // call std::terminate() (cannot join a still-running thread).
    stop_ipc_server();
    stop();
}

void AccelDaemon::log(const std::string& msg, bool verbose_only) {
    if (verbose_only && !verbose_) return;
    if (log_cb_) log_cb_(msg);
    else         std::cout << "[rawaccel] " << msg << "\n";
}

bool AccelDaemon::start(const std::string& config_path) {
    if (running_.load()) return true;

    config_path_ = config_path.empty() ? find_config_path() : config_path;

    try {
        config_ = load_config(config_path_);
    } catch (std::exception& e) {
        log("Config load failed: " + std::string(e.what()) + " — using defaults.");
        device_profile dp;
        dp.name = "default";
        dp.dev_cfg.dpi = 800;
        dp.dev_cfg.polling_rate = 1000;
        config_.profiles.push_back(dp);
    }

    // ── epoll setup ───────────────────────────────────────────────────────────
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        log("epoll_create1 failed: " + std::string(strerror(errno)));
        return false;
    }

    // ── inotify setup for hot-plug ────────────────────────────────────────────
    inotify_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd_ >= 0) {
        inotify_wd_ = inotify_add_watch(inotify_fd_, "/dev/input",
                                        IN_CREATE | IN_DELETE | IN_ATTRIB);
        if (inotify_wd_ >= 0) {
            epoll_event ev{};
            ev.events   = EPOLLIN;
            ev.data.fd  = inotify_fd_;
            if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, inotify_fd_, &ev) < 0) {
                log("epoll_ctl(inotify) failed: " + std::string(strerror(errno)) +
                    " — hot-plug disabled.");
                close(inotify_fd_); inotify_fd_ = -1; inotify_wd_ = -1;
            } else {
                log("Hot-plug: watching /dev/input via inotify.", true);
            }
        } else {
            log("inotify_add_watch failed — hot-plug disabled.");
        }
    } else {
        log("inotify_init1 failed — hot-plug disabled.");
    }

    if (!setup_devices()) {
        close(epoll_fd_); epoll_fd_ = -1;
        if (inotify_fd_ >= 0) { close(inotify_fd_); inotify_fd_ = -1; }
        return false;
    }

    running_.store(true);
    loop_thread_ = std::thread([this] { run_loop(); });
    log("Daemon started.");
    return true;
}

void AccelDaemon::stop() {
    bool was_running = running_.exchange(false);
    if (loop_thread_.joinable()) loop_thread_.join();
    bool had_runtime_fds = (inotify_fd_ >= 0 || epoll_fd_ >= 0);
    teardown_devices();

    if (inotify_fd_ >= 0) { close(inotify_fd_); inotify_fd_ = -1; inotify_wd_ = -1; }
    if (epoll_fd_   >= 0) { close(epoll_fd_);   epoll_fd_   = -1; }

    if (was_running || had_runtime_fds)
        log("Daemon stopped.");
}

bool AccelDaemon::reload() {
    reload_flag_.store(true);
    return true;
}

// ── Device setup ──────────────────────────────────────────────────────────────

bool AccelDaemon::open_input_device(mouse_device& dev) {
    dev.fd_in = open(dev.path.c_str(), O_RDONLY | O_NONBLOCK);
    if (dev.fd_in < 0) {
        log("Cannot open " + dev.path + ": " + strerror(errno));
        return false;
    }

    char name[256] = {};
    // O4: check return value; fall back to path if the name cannot be read.
    // Guarantee null terminator even if the buffer is completely filled.
    if (ioctl(dev.fd_in, EVIOCGNAME(sizeof(name) - 1), name) < 0)
        snprintf(name, sizeof(name), "unknown(%s)", dev.path.c_str());
    name[sizeof(name) - 1] = '\0'; // overflow guard
    dev.name = std::string(name);

    // Build a stable composite device ID: "usb:VVVV:PPPP:serial"
    // Priority:
    //   1. vendor+product+serial → most stable, survives reboots and re-plugs
    //   2. vendor+product (no serial) → stable for single-device setups
    //   3. /dev/input/by-id/... symlink → stable if udev provides it
    //   4. /dev/input/eventN → last resort; changes on reboot with multiple USB devices

    // Read USB serial (EVIOCGUNIQ)
    char uniq[256] = {};
    ioctl(dev.fd_in, EVIOCGUNIQ(sizeof(uniq)), uniq); // ignore error — may be empty

    // Read vendor/product IDs (EVIOCGID)
    struct input_id iid = {};
    bool has_vid_pid = (ioctl(dev.fd_in, EVIOCGID, &iid) >= 0 &&
                        (iid.vendor != 0 || iid.product != 0));

    if (has_vid_pid) {
        // "usb:045e:082a:SERIAL" — unique per physical device even without serial
        char id_buf[512];
        snprintf(id_buf, sizeof(id_buf), "usb:%04x:%04x:%s",
                 iid.vendor, iid.product, uniq);
        dev.device_id = std::string(id_buf);
    } else if (uniq[0] != '\0') {
        // No vid/pid but has serial — use serial alone
        dev.device_id = std::string(uniq);
    } else {
        // Fallback: /dev/input/eventN — may change across reboots
        dev.device_id = dev.path;
    }

    if (ioctl(dev.fd_in, EVIOCGRAB, 1) < 0) {
        log("Skipping " + dev.name + " (" + dev.path + "): already grabbed.");
        close(dev.fd_in);
        dev.fd_in = -1;
        return false;
    }

    // Detect polling rate and DPI from sysfs (best-effort, non-blocking).
    // Results stored for status_json() and GUI auto-fill; never overwrite user config.
    dev.detected_polling_rate = detect_polling_rate(dev.path);
    int ev_n = event_num_from_path(dev.path);
    dev.detected_dpi = (ev_n >= 0) ? detect_dpi_sysfs(ev_n) : 0;

    if (dev.detected_polling_rate > 0)
        log("Detected polling rate: " + std::to_string(dev.detected_polling_rate) +
            " Hz for " + dev.name, true);
    if (dev.detected_dpi > 0)
        log("Detected DPI: " + std::to_string(dev.detected_dpi) +
            " for " + dev.name, true);

    log("Opened mouse: " + dev.name + " [id=" + dev.device_id + "] (" + dev.path + ")", false);
    return true;
}

bool AccelDaemon::create_virtual_device(mouse_device& dev) {
    int tmp_fd = open(dev.path.c_str(), O_RDONLY | O_NONBLOCK);
    libevdev* src = nullptr;
    int src_fd = (tmp_fd >= 0) ? tmp_fd : dev.fd_in;

    if (libevdev_new_from_fd(src_fd, &src) < 0) {
        if (tmp_fd >= 0) close(tmp_fd);
        log("Cannot read capabilities of " + dev.path);
        return false;
    }

    std::string vname = dev.name + " (RawAccel)";
    libevdev_set_name(src, vname.c_str());
    libevdev_set_uniq(src, nullptr);

    libevdev_uinput* uidev = nullptr;
    int ret = libevdev_uinput_create_from_device(src, LIBEVDEV_UINPUT_OPEN_MANAGED, &uidev);
    libevdev_free(src);
    if (tmp_fd >= 0) close(tmp_fd);

    if (ret < 0) {
        log("Cannot create uinput device for " + dev.path + ": " + strerror(-ret));
        return false;
    }

    dev.uidev = uidev;
    log("Created virtual device: " + vname, true); // verbose: not needed in normal operation
    return true;
}

bool AccelDaemon::setup_devices() {
    auto mice = find_mice();
    if (mice.empty()) {
        // D3: Possible cause: no access to /dev/input/event* or all devices are already grabbed.
        log("No physical mice found. Check 'input' group membership or udev rules.");
        return !devices_.empty(); // ok if we already have some from hot-plug
    }
    log("Found " + std::to_string(mice.size()) + " physical mouse device(s).");

    for (auto& path : mice) {
        if (opened_paths_.count(path)) continue; // already grabbed

        mouse_device dev;
        dev.path = path;

        if (!open_input_device(dev)) continue;

        const device_profile* prof = find_profile(dev.device_id);
        if (prof && prof->dev_cfg.disable) {
            log("Skipping disabled mouse profile: " + dev.name +
                " [id=" + dev.device_id + "]");
            ioctl(dev.fd_in, EVIOCGRAB, 0);
            close(dev.fd_in);
            continue;
        }

        if (!create_virtual_device(dev)) {
            ioctl(dev.fd_in, EVIOCGRAB, 0);
            close(dev.fd_in);
            continue;
        }

        if (prof) apply_profile(dev, *prof);

        // Register in epoll
        epoll_event ev{};
        ev.events  = EPOLLIN;
        ev.data.fd = dev.fd_in;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, dev.fd_in, &ev) < 0) {
            log("epoll_ctl(add) failed for " + dev.path + ": " +
                strerror(errno) + " — skipping device.");
            if (dev.uidev) libevdev_uinput_destroy(dev.uidev);
            ioctl(dev.fd_in, EVIOCGRAB, 0);
            close(dev.fd_in);
            continue;
        }

        opened_paths_.insert(path);
        {
            std::lock_guard<std::mutex> lk(devices_mutex_);
            fd_to_dev_[dev.fd_in] = devices_.size(); // index before push
            devices_.push_back(std::move(dev));
        }
    }

    if (devices_.empty()) {
        log("No mice could be grabbed. If abrek is running, stop it first:");
        log("  sudo systemctl stop abrek");
        // K3: clean up any partially opened devices to prevent fd leaks
        teardown_devices();
        return false;
    }
    return true;
}

void AccelDaemon::teardown_devices() {
    // Collect handles to destroy outside the lock to keep the critical section short
    std::vector<mouse_device> to_destroy;
    {
        std::lock_guard<std::mutex> lk(devices_mutex_);
        to_destroy = std::move(devices_);
        devices_.clear();
        opened_paths_.clear();
        fd_to_dev_.clear();
    }
    for (auto& dev : to_destroy) {
        if (epoll_fd_ >= 0 && dev.fd_in >= 0)
            epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, dev.fd_in, nullptr);
        if (dev.uidev) {
            libevdev_uinput_destroy(dev.uidev);
            dev.uidev = nullptr;
        }
        if (dev.fd_in >= 0) {
            ioctl(dev.fd_in, EVIOCGRAB, 0);
            close(dev.fd_in);
            dev.fd_in = -1;
        }
    }
}

const device_profile* AccelDaemon::find_profile(const std::string& dev_id) const {
    // 1. Device-specific assignment takes priority
    for (auto& p : config_.profiles)
        if (!p.device_id.empty() && p.device_id == dev_id) return &p;
    // 2. Active profile
    for (auto& p : config_.profiles)
        if (p.name == config_.active_profile) return &p;
    // 3. First profile (last resort fallback)
    if (!config_.profiles.empty()) return &config_.profiles[0];
    return nullptr;
}

void AccelDaemon::apply_profile(mouse_device& dev, const device_profile& prof) {
    // Defense-in-depth clamp: values are already sanitized in config.cpp,
    // but guard here too in case of programmatic / future IPC paths.
    dev.dpi       = std::clamp(prof.dev_cfg.dpi,          1, 32000);
    dev.poll_rate = std::clamp(prof.dev_cfg.polling_rate,
                               (int)POLL_RATE_MIN, (int)POLL_RATE_MAX);
    // Convert raw counts/ms to physical inches/s:
    //   counts / dpi / (ms / 1000) = counts * NORMALIZED_DPI / (dpi * ms).
    // `output_dpi` is applied later in modifier::modify() as output scaling.
    dev.dpi_factor = NORMALIZED_DPI / dev.dpi; // R13-perf: pre-compute
    dev.settings.prof = prof.prof;
    init_settings(dev.settings);
    dev.sp.init(prof.prof.speed_processor_args);
    // Reset subpixel remainders on profile change
    dev.remainder_x = 0.0;
    dev.remainder_y = 0.0;
}

// ── Hot-plug handler ──────────────────────────────────────────────────────────

void AccelDaemon::handle_hotplug() {
    // Drain inotify events
    char buf[4096] __attribute__((aligned(__alignof__(inotify_event))));
    bool changed = false;
    while (true) {
        ssize_t n = read(inotify_fd_, buf, sizeof(buf));
        if (n <= 0) break;
        size_t i = 0;
        size_t total = static_cast<size_t>(n);
        while (i < total) {
            auto* ev = reinterpret_cast<inotify_event*>(buf + i);
            if (ev->len > 0) {
                std::string fname(ev->name);
                // Only care about event* nodes
                if (fname.rfind("event", 0) == 0) changed = true;
            }
            i += sizeof(inotify_event) + static_cast<size_t>(ev->len);
        }
    }

    if (changed)
        pending_hotplug_.store(true); // defer actual scan to run_loop (no usleep)
}

// ── Hot-plug device scan (called from run_loop after kernel settle delay) ─────

void AccelDaemon::do_hotplug_scan() {
    // Check for newly added mice
    auto mice = find_mice();
    for (auto& path : mice) {
        if (opened_paths_.count(path)) continue;
        log("Hot-plug: new mouse detected at " + path);

        mouse_device dev;
        dev.path = path;
        if (!open_input_device(dev)) continue;

        // Apply per-device profile assignment before creating uinput so a
        // disabled profile really leaves the physical mouse untouched.
        const device_profile* prof = find_profile(dev.device_id);
        if (prof && prof->dev_cfg.disable) {
            log("Hot-plug: disabled profile matched, skipping " + dev.name +
                " [id=" + dev.device_id + "]");
            ioctl(dev.fd_in, EVIOCGRAB, 0);
            close(dev.fd_in);
            continue;
        }

        if (!create_virtual_device(dev)) {
            ioctl(dev.fd_in, EVIOCGRAB, 0);
            close(dev.fd_in);
            continue;
        }

        if (prof) apply_profile(dev, *prof);

        epoll_event eev{};
        eev.events  = EPOLLIN;
        eev.data.fd = dev.fd_in;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, dev.fd_in, &eev) < 0) {
            log("Hot-plug: epoll_ctl(add) failed for " + dev.path + ": " +
                strerror(errno) + " — skipping device.");
            if (dev.uidev) libevdev_uinput_destroy(dev.uidev);
            ioctl(dev.fd_in, EVIOCGRAB, 0);
            close(dev.fd_in);
            continue;
        }

        opened_paths_.insert(path);
        {
            std::lock_guard<std::mutex> lk(devices_mutex_);
            fd_to_dev_[dev.fd_in] = devices_.size();
            devices_.push_back(std::move(dev));
        }
    }

    // Remove devices whose path is no longer a physical mouse (disconnected)
    // Collect handles to destroy outside the lock
    std::vector<mouse_device> to_destroy;
    {
        std::lock_guard<std::mutex> lk(devices_mutex_);
        auto it = devices_.begin();
        while (it != devices_.end()) {
            bool still_physical = false;
            for (auto& p : mice)
                if (p == it->path) { still_physical = true; break; }

            if (!still_physical) {
                log("Hot-plug: mouse disconnected: " + it->name + " (" + it->path + ")");
                epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, it->fd_in, nullptr);
                opened_paths_.erase(it->path);
                to_destroy.push_back(std::move(*it));
                it = devices_.erase(it);
            } else {
                ++it;
            }
        }
        // Rebuild fd_to_dev_ index since vector indices shifted after erase
        fd_to_dev_.clear();
        for (size_t i = 0; i < devices_.size(); i++)
            fd_to_dev_[devices_[i].fd_in] = i;
    }
    for (auto& dev : to_destroy) {
        if (dev.uidev) libevdev_uinput_destroy(dev.uidev);
        if (dev.fd_in >= 0) {
            ioctl(dev.fd_in, EVIOCGRAB, 0);
            close(dev.fd_in);
        }
    }
}

// ── Main loop (epoll-based) ───────────────────────────────────────────────────

void AccelDaemon::run_loop() {
    constexpr int MAX_EVENTS = 32;
    epoll_event events[MAX_EVENTS];

    while (running_.load()) {
        // Handle config reload request
        if (reload_flag_.exchange(false)) {
            log("Reloading config...");
            try {
                auto new_cfg = load_config(config_path_);

                // Live profile update: re-apply settings to all open devices WITHOUT
                // releasing the grab or destroying the virtual device.  This avoids
                // the ~150 ms mouse-loss window that teardown+setup caused (R5).
                // Only fall back to teardown+setup when a new physical device appears
                // that wasn't grabbed before (handled by the hot-plug path).
                //
                // config_ is written under devices_mutex_ so that status_json()
                // (which runs on the IPC thread) never reads a half-updated config_.
                bool any_live = false;
                bool needs_full_reopen = false;
                {
                    std::lock_guard<std::mutex> lk(devices_mutex_);
                    config_ = new_cfg;
                    for (auto& dev : devices_) {
                        const device_profile* prof = find_profile(dev.device_id);
                        if (prof && prof->dev_cfg.disable) {
                            needs_full_reopen = true;
                            break;
                        }
                        if (prof) {
                            apply_profile(dev, *prof);
                            any_live = true;
                            log("Live-updated profile for: " + dev.name, true);
                        }
                    }
                }

                if (needs_full_reopen) {
                    // A live device now maps to a disabled profile.  Reopen the
                    // device set so that disabled devices are ungrabbed and skipped.
                    teardown_devices();
                    if (!setup_devices())
                        log("Reload: no devices available after applying disabled profiles.");
                } else if (!any_live) {
                    // No grabbed devices yet — do a full setup so new devices are opened.
                    teardown_devices();
                    if (!setup_devices())
                        log("Reload: no devices available after reload.");
                }
                log("Config reloaded.");
            } catch (std::exception& e) {
                log("Config reload failed (keeping current): " + std::string(e.what()));
            }
        }

        // Deferred hot-plug: wait for the kernel to finish creating the device node.
        // We retry once per epoll_wait cycle (every ~10ms); after 8 retries (~80ms)
        // we give up if the device still isn't ready. This keeps the event loop non-blocking.
        if (pending_hotplug_.load()) {
            hotplug_retry_++;
            // 8 × 10ms epoll_wait = ~80ms wait — USB hubs may take 50-100ms for
            // the kernel to make the device node available.
            if (hotplug_retry_ >= 8) {
                pending_hotplug_.store(false);
                hotplug_retry_ = 0;
                do_hotplug_scan();
            }
        }

        // epoll_wait with 10ms timeout (allows flag checks above)
        int n = epoll_wait(epoll_fd_, events, MAX_EVENTS, 10);
        if (n < 0) {
            if (errno == EINTR) continue;
            log("epoll_wait error: " + std::string(strerror(errno)));
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            // inotify hot-plug event: drain events and set pending flag
            if (fd == inotify_fd_) {
                handle_hotplug();
                continue;
            }

            // O(1) fd -> device lookup
            auto it = fd_to_dev_.find(fd);
            if (it != fd_to_dev_.end() && it->second < devices_.size())
                process_device(devices_[it->second]);
        }

        // Clean up any devices that got a fatal I/O error during processing
        std::vector<mouse_device> disc_devs;
        {
            std::lock_guard<std::mutex> lk(devices_mutex_);
            auto dit = devices_.begin();
            while (dit != devices_.end()) {
                if (dit->disconnected) {
                    log("Removing disconnected device: " + dit->name);
                    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, dit->fd_in, nullptr);
                    fd_to_dev_.erase(dit->fd_in);
                    opened_paths_.erase(dit->path);
                    disc_devs.push_back(std::move(*dit));
                    dit = devices_.erase(dit);
                } else {
                    ++dit;
                }
            }
            if (!disc_devs.empty()) {
                // Rebuild fd_to_dev_ index after removals
                fd_to_dev_.clear();
                for (size_t i = 0; i < devices_.size(); i++)
                    fd_to_dev_[devices_[i].fd_in] = i;
            }
        }
        for (auto& dev : disc_devs) {
            if (dev.uidev) libevdev_uinput_destroy(dev.uidev);
            if (dev.fd_in >= 0) {
                ioctl(dev.fd_in, EVIOCGRAB, 0);
                close(dev.fd_in);
            }
        }
    }
}

// ── Per-device event processing ───────────────────────────────────────────────

/// Nanosecond wall-clock timestamp using CLOCK_MONOTONIC_RAW.
/// More stable than steady_clock on Linux (no NTP slew adjustments).
static inline uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

/// Write a single event to uinput; returns false if the write fails
/// (indicating the virtual device is dead — caller should mark dev as disconnected).
static inline bool uinput_write(libevdev_uinput* uidev, unsigned int type,
                                unsigned int code, int value) {
    return libevdev_uinput_write_event(uidev, type, code, value) == 0;
}

/// Apply acceleration to accumulated (dx,dy) and write REL events to uidev.
/// Updates dev timing and subpixel remainder. Does NOT write SYN.
/// Measures processing latency (time from call entry to last uinput write) in µs.
/// Returns false if a uinput write fails (caller should mark dev as disconnected).
static bool flush_motion(mouse_device& dev, libevdev_uinput* uidev,
                         double dx, double dy) {
    const uint64_t t_start = now_ns(); // latency measurement start

    // Raw passthrough: bypass the entire acceleration pipeline.
    // No rotation, no snap, no speed clamp, no weights, no subpixel accumulation.
    // dx/dy are already integer counts from the kernel — write them directly.
    if (dev.settings.prof.raw_passthrough) {
        // Defense-in-depth: clamp to INT range before casting (mirrors the
        // guard in motion_math.hpp). A pathological event batch with millions
        // of REL_X events in one SYN frame would otherwise be UB on cast.
        constexpr double INT_LO = static_cast<double>(INT_MIN);
        constexpr double INT_HI = static_cast<double>(INT_MAX);
        if (!std::isfinite(dx)) dx = 0;
        if (!std::isfinite(dy)) dy = 0;
        int ix = static_cast<int>(std::clamp(dx, INT_LO, INT_HI));
        int iy = static_cast<int>(std::clamp(dy, INT_LO, INT_HI));
        if (ix != 0 && !uinput_write(uidev, EV_REL, REL_X, ix)) return false;
        if (iy != 0 && !uinput_write(uidev, EV_REL, REL_Y, iy)) return false;
        double lat_us = static_cast<double>(now_ns() - t_start) / 1000.0;
        dev.lat.record(lat_us);
        return true;
    }

    double now = now_ms();
    milliseconds time_ms = now - dev.last_time_ms;
    // D6: modify() returns early when time<=0 (no motion applied).
    // flush_motion follows the same strategy: don't send motion for zero/negative intervals.
    // On the first call last_time_ms==0, so time_ms will be very large and gets clamped to DEFAULT_TIME_MAX — correct.
    double min_time_ms = 1000.0 / std::clamp(dev.poll_rate, (int)POLL_RATE_MIN, (int)POLL_RATE_MAX) / 2.0;
    if (time_ms <= 0) time_ms = min_time_ms; // clamp to configured half-poll window instead of zero
    if (time_ms > DEFAULT_TIME_MAX) time_ms = DEFAULT_TIME_MAX;
    dev.last_time_ms = now;

    int out_x = 0, out_y = 0;
    apply_motion_math(dev.mod, dev.sp, dev.settings, dev.dpi_factor, time_ms,
                      dx, dy, dev.remainder_x, dev.remainder_y, out_x, out_y);

    if (out_x != 0 && !uinput_write(uidev, EV_REL, REL_X, out_x)) return false;
    if (out_y != 0 && !uinput_write(uidev, EV_REL, REL_Y, out_y)) return false;

    // Record processing latency (µs): time from flush_motion entry to last write.
    // This covers: modifier math + uinput write (does NOT include kernel→user round-trip).
    double lat_us = static_cast<double>(now_ns() - t_start) / 1000.0;
    dev.lat.record(lat_us);
    return true;
}

void AccelDaemon::process_device(mouse_device& dev) {
    auto* uidev = dev.uidev;
    if (!uidev) return;

    input_event ev;

    // Accumulate relative motion in this batch
    double dx = 0, dy = 0;
    bool has_motion  = false;
    bool has_syn     = false;
    bool wrote_unsynced_event = false;
    // BUG-18: syn_dropped is now a device-state field (mouse_device::syn_dropped)
    // so a SYN_DROPPED event in one read batch is correctly remembered until
    // the matching SYN_REPORT arrives in the next process_device() invocation.
    bool& syn_dropped = dev.syn_dropped;
    auto flush_pending_motion = [&]() -> bool {
        if (!has_motion) return true;
        if (!flush_motion(dev, uidev, dx, dy)) {
            dev.disconnected = true;
            return false;
        }
        wrote_unsynced_event = true;
        dx = dy = 0;
        has_motion = false;
        return true;
    };

    // Read all pending events
    while (true) {
        ssize_t n = read(dev.fd_in, &ev, sizeof(ev));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break; // normal: no more events
            if (errno == EINTR) continue;
            // Fatal errors: device physically disconnected or kernel error
            if (errno == EIO || errno == ENODEV || errno == EBADF) {
                log("Device error (" + std::string(strerror(errno)) +
                    "), marking for removal: " + dev.name);
                dev.disconnected = true;
            } else {
                // Unexpected errno (EINTR/EFAULT/EINVAL/etc.) — log so we
                // don't silently drop events.  Default policy: keep the
                // device, the next loop iteration will retry.
                log("read() unexpected errno=" + std::to_string(errno) +
                    " (" + strerror(errno) + ") on " + dev.name, true);
            }
            break;
        }
        if (n == 0) { dev.disconnected = true; break; } // EOF — treat as disconnect
        if (n < (ssize_t)sizeof(ev)) break; // short read, skip

        if (ev.type == EV_SYN) {
            if (ev.code == SYN_DROPPED) {
                // Kernel dropped events due to buffer overflow.
                // Per the Linux input protocol, ALL events between SYN_DROPPED
                // and the next SYN_REPORT are unreliable and must be discarded.
                // Set a flag so subsequent events in this batch are ignored.
                dx = dy = 0;
                has_motion = false;
                syn_dropped = true;
                continue;
            }
            // SYN_REPORT: if we were in syn_dropped state, this marks the
            // boundary — clear the flag and resume normal processing.
            // Do NOT flush any motion or forward this SYN (the dropped window
            // ends here; fresh data starts from the next event batch).
            if (syn_dropped) {
                syn_dropped = false;
                continue;
            }
            has_syn = true;
            if (!flush_pending_motion()) return;
            // Forward SYN
            if (!uinput_write(uidev, EV_SYN, SYN_REPORT, 0))
                { dev.disconnected = true; return; }
            wrote_unsynced_event = false;
        } else if (syn_dropped) {
            // R12: discard all non-SYN events while in SYN_DROPPED state.
            // Motion counts, button events, etc. are unreliable until the
            // next SYN_REPORT clears the dropped flag.
            continue;
        } else if (ev.type == EV_REL) {
            if (ev.code == REL_X || ev.code == REL_Y) {
                // Raw passthrough fast path: forward each REL_X/REL_Y event
                // INDIVIDUALLY (no batching) so the byte-stream written to
                // uinput is bit-identical to a "dumb" 1:1 forwarder like
                // abrek. libinput's adaptive accel treats batched vs split
                // events differently (single +15 vs 5×+3), and we noticed
                // this caused subtly different cursor feel even in raw mode.
                // Skip accumulation entirely in this mode.
                if (dev.settings.prof.raw_passthrough) {
                    if (!uinput_write(uidev, ev.type, ev.code, ev.value))
                        { dev.disconnected = true; return; }
                    wrote_unsynced_event = true;
                } else if (ev.code == REL_X) {
                    dx += ev.value; has_motion = true;
                } else {
                    dy += ev.value; has_motion = true;
                }
            } else {
                // Pass through other REL events (wheel, tilt, etc.)
                if (!flush_pending_motion()) return;
                if (!uinput_write(uidev, ev.type, ev.code, ev.value))
                    { dev.disconnected = true; return; }
                wrote_unsynced_event = true;
            }
        } else {
            // Forward all other events (buttons, misc, etc.)
            if (!flush_pending_motion()) return;
            if (!uinput_write(uidev, ev.type, ev.code, ev.value))
                { dev.disconnected = true; return; }
            wrote_unsynced_event = true;
        }
    }

    // If events were forwarded but no SYN arrived (rare edge case), flush with synthetic SYN
    if (!has_syn) {
        if (!flush_pending_motion()) return;
        if (wrote_unsynced_event && !uinput_write(uidev, EV_SYN, SYN_REPORT, 0))
            { dev.disconnected = true; return; }
    }
}

// ── Latency statistics dump ───────────────────────────────────────────────────

void AccelDaemon::dump_latency_stats() {
    // Called on SIGUSR1 from the main thread — prints to stdout (journald captures it).
    // Stats cover flush_motion processing time (modifier math + uinput write),
    // NOT the full kernel→userspace round-trip latency.
    // snapshot_and_reset() atomically reads and clears counters under the mutex.
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "=== RawAccel Processing Latency ===\n";
    std::lock_guard<std::mutex> lk(devices_mutex_);
    if (devices_.empty()) {
        std::cout << "  No devices currently grabbed.\n";
        std::cout << "===================================\n";
        std::cout.flush();
        return;
    }
    for (auto& dev : devices_) {
        std::cout << "  Device: " << dev.name;
        // BUG-21 (GPT-10): in raw_passthrough mode REL_X/REL_Y events bypass
        // flush_motion() entirely (forwarded one-by-one straight from
        // process_device), so dev.lat is never updated.  Make this explicit
        // in the dump so users don't see a misleading "No motion events".
        if (dev.settings.prof.raw_passthrough) {
            std::cout << "  [raw passthrough — no per-event measurement]\n";
            std::cout << "    (events forwarded 1:1 to uinput; the clock_gettime\n"
                         "     pair would add ~50 ns per event vs. 0 in raw mode.)\n";
            continue;
        }
        std::cout << "\n";
        auto s = dev.lat.snapshot_and_reset();
        if (s.count == 0) {
            std::cout << "    No motion events recorded yet.\n";
        } else {
            std::cout << std::fixed << std::setprecision(2)
                      << "    Samples  : " << s.count              << "\n"
                      << "    Min      : " << s.min_us             << " µs\n"
                      << "    Avg      : " << s.avg_us()           << " µs\n"
                      << "    p50      : " << s.percentile(50)     << " µs\n"
                      << "    p95      : " << s.percentile(95)     << " µs\n"
                      << "    p99      : " << s.percentile(99)     << " µs\n"
                      << "    Max      : " << s.max_us             << " µs\n";
            if (s.over > 0)
                std::cout << "    Overflow : " << s.over
                          << " samples > " << lat_stats::RANGE_US << " µs\n";
        }
        std::cout << "    (counters reset)\n";
    }
    std::cout << "===================================\n";
    std::cout.flush();
}

// ── IPC: Unix domain socket server ───────────────────────────────────────────

/// Helper: escape a string for JSON (only handles ASCII printable + common escapes).
static std::string json_str(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (unsigned char c : s) {
        if      (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20)  out += ' '; // replace other controls with space
        else                out += static_cast<char>(c);
    }
    out += '"';
    return out;
}

std::string AccelDaemon::ipc_sock_path() {
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg && xdg[0] != '\0') return std::string(xdg) + "/rawaccel.sock";
    return "/run/rawaccel.sock";
}

std::string AccelDaemon::status_json() const {
    std::ostringstream o;
    o << "{";
    o << "\"running\":" << (running_.load() ? "true" : "false") << ",";

    // Collect device snapshots, config path, and active_profile under a single
    // devices_mutex_ lock.  This prevents a data race with run_loop()'s
    // reload path which writes config_ under the same mutex.
    // lat.mtx is acquired nested inside devices_mutex_; dump_latency_stats() uses
    // the same lock order (devices_mutex_ → lat.mtx) so no deadlock is possible.

    struct DevSnap {
        std::string name, path;
        int dpi, poll_rate, detected_dpi, detected_polling_rate;
        uint64_t lat_count = 0;
        double lat_avg = 0, lat_p50 = 0, lat_p95 = 0, lat_p99 = 0, lat_max = 0;
    };

    std::string cfg_path_snap;
    std::string active_prof_snap;
    std::vector<DevSnap> snaps;
    {
        std::lock_guard<std::mutex> lk(devices_mutex_);
        cfg_path_snap    = config_path_;
        active_prof_snap = config_.active_profile;
        for (const auto& dev : devices_) {
            DevSnap s;
            s.name = dev.name; s.path = dev.path;
            s.dpi = dev.dpi; s.poll_rate = dev.poll_rate;
            s.detected_dpi = dev.detected_dpi;
            s.detected_polling_rate = dev.detected_polling_rate;

            std::lock_guard<std::mutex> llk(dev.lat.mtx);
            if (dev.lat.count > 0) {
                s.lat_count = dev.lat.count;
                s.lat_avg   = dev.lat.avg_us();
                s.lat_p50   = dev.lat.percentile(50);
                s.lat_p95   = dev.lat.percentile(95);
                s.lat_p99   = dev.lat.percentile(99);
                s.lat_max   = dev.lat.max_us;
            }
            snaps.push_back(s);
        }
    }

    o << "\"config\":" << json_str(cfg_path_snap) << ",";
    o << "\"active_profile\":" << json_str(active_prof_snap) << ",";
    o << "\"devices\":[";

    bool first = true;
    for (const auto& s : snaps) {
        if (!first) o << ",";
        first = false;
        o << "{";
        o << "\"name\":"                  << json_str(s.name) << ",";
        o << "\"path\":"                  << json_str(s.path) << ",";
        o << "\"dpi\":"                   << s.dpi << ",";
        o << "\"poll_rate\":"             << s.poll_rate << ",";
        o << "\"detected_dpi\":"          << s.detected_dpi << ",";
        o << "\"detected_polling_rate\":" << s.detected_polling_rate;
        if (s.lat_count > 0) {
            o << std::fixed << std::setprecision(2);
            o << ",\"lat_samples\":" << s.lat_count;
            o << ",\"lat_avg_us\":"  << s.lat_avg;
            o << ",\"lat_p50_us\":"  << s.lat_p50;
            o << ",\"lat_p95_us\":"  << s.lat_p95;
            o << ",\"lat_p99_us\":"  << s.lat_p99;
            o << ",\"lat_max_us\":"  << s.lat_max;
        }
        o << "}";
    }
    o << "]}";
    return o.str();
}

bool AccelDaemon::start_ipc_server(const std::string& sock_path) {
    ipc_sock_path_ = sock_path;

    // Remove stale socket file
    unlink(sock_path.c_str());

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        log("IPC: socket() failed: " + std::string(strerror(errno)));
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (sock_path.size() >= sizeof(addr.sun_path)) {
        log("IPC: socket path too long: " + sock_path);
        close(fd);
        return false;
    }
    strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        log("IPC: bind() failed: " + std::string(strerror(errno)));
        close(fd);
        return false;
    }
    // Allow the 'input' group to connect (normal users added by the installer).
    // chown root:input + chmod 0660 → only root and input-group members can connect.
    // If chown fails (e.g. no input group), fall back to owner-only (0600).
    {
        struct group* grp = getgrnam("input");
        if (grp) {
            // Best-effort group ownership; if chown fails (e.g. unprivileged
            // start) we fall through to chmod which still tightens permissions.
            if (chown(sock_path.c_str(), 0, grp->gr_gid) != 0)
                log("IPC: chown(input group) failed: " + std::string(strerror(errno)) +
                    " — proceeding with default ownership.", true);
            chmod(sock_path.c_str(), 0660);
        } else {
            chmod(sock_path.c_str(), 0600); // fallback: owner only
        }
    }

    if (listen(fd, 8) < 0) {
        log("IPC: listen() failed: " + std::string(strerror(errno)));
        close(fd);
        return false;
    }
    ipc_sock_fd_.store(fd);

    ipc_running_.store(true);
    ipc_thread_ = std::thread([this] { ipc_serve_loop(); });
    log("IPC socket: " + sock_path, true);
    return true;
}

void AccelDaemon::stop_ipc_server() {
    ipc_running_.store(false);
    int fd = ipc_sock_fd_.exchange(-1);
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
    if (ipc_thread_.joinable()) ipc_thread_.join();
    if (!ipc_sock_path_.empty()) {
        unlink(ipc_sock_path_.c_str());
        ipc_sock_path_.clear();
    }
}

void AccelDaemon::ipc_serve_loop() {
    while (ipc_running_.load()) {
        // Guard: stop_ipc_server() may close the fd before we exit.
        int fd = ipc_sock_fd_.load();
        if (fd < 0) break;

        // Use poll() instead of select() to avoid FD_SETSIZE overflow when fd >= 1024.
        // select()'s FD_SET macro has undefined behaviour for fd >= FD_SETSIZE (typically 1024).
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        int r = poll(&pfd, 1, 1000 /* ms */);
        if (r <= 0) continue; // timeout or error — check ipc_running_ again

        // R6: use the local 'fd' copy instead of ipc_sock_fd_ to avoid TOCTOU race
        // with stop_ipc_server() which may close ipc_sock_fd_ between poll() and accept4().
        int client = accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) continue;
        handle_ipc_client(client);
        close(client);
    }
}

void AccelDaemon::handle_ipc_client(int client_fd) {
    // Set a short read timeout to prevent slow-client DoS
    struct timeval tv { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[64] = {};
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return;

    // Strip trailing whitespace/newlines
    for (int i = (int)n - 1; i >= 0 && (buf[i] == '\n' || buf[i] == '\r' || buf[i] == ' '); i--)
        buf[i] = '\0';

    std::string response;
    if (strcmp(buf, "status") == 0) {
        response = status_json() + "\n";
    } else if (strcmp(buf, "ping") == 0) {
        response = "pong\n";
    } else if (strcmp(buf, "reload") == 0) {
        reload_flag_.store(true);
        response = "{\"ok\":true,\"message\":\"config reload scheduled\"}\n";
    } else if (strcmp(buf, "latency") == 0) {
        // Same effect as SIGUSR1, but accessible to any input-group user
        // even when the daemon runs as root via systemd (kill() returns
        // EPERM in that case).  Set a flag the main thread polls.
        latency_dump_flag_.store(true);
        response = "{\"ok\":true,\"message\":\"latency dump scheduled\"}\n";
    } else {
        response = "{\"error\":\"unknown command\"}\n";
    }

    // Write full response (handle partial writes)
    const char* p = response.c_str();
    size_t left = response.size();
    while (left > 0) {
        ssize_t w = send(client_fd, p, left, MSG_NOSIGNAL);
        if (w <= 0) break;
        p += w; left -= (size_t)w;
    }
}

} // namespace rawaccel
