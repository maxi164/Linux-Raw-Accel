#pragma once
#include "../include/rawaccel.hpp"
#include "../include/config.hpp"
#include "lat_stats.hpp"              // latency histogram — no libevdev dependency
#include <libevdev/libevdev-uinput.h>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>
#include <functional>
#include <set>
#include <unordered_map>
#include <cstdint>

namespace rawaccel {

/// Represents one grabbed input device + its virtual output device.
struct mouse_device {
    std::string      name;
    std::string      path;           // e.g. /dev/input/event3
    std::string      device_id;
    int              fd_in  = -1;    // grabbed evdev fd
    libevdev_uinput* uidev  = nullptr; // uinput virtual device handle
    int              dpi    = 800;
    int              poll_rate = 1000;
    int              detected_dpi          = 0;  // sysfs-detected DPI (0 = unknown)
    int              detected_polling_rate = 0;  // sysfs-detected polling rate in Hz (0 = unknown)

    // Per-device state
    modifier_settings  settings;
    speed_processor    sp;
    modifier           mod;

    // Timing
    double last_time_ms = 0;
    // R13-perf: pre-computed NORMALIZED_DPI / dpi — updated only on profile change,
    // avoids a floating-point division on every mouse event.
    double dpi_factor   = NORMALIZED_DPI / 800.0;

    // Subpixel accumulation: carry fractional remainder between frames
    double remainder_x   = 0.0;
    double remainder_y   = 0.0;
    // Set to true when a fatal I/O error occurs; run_loop removes the device
    bool   disconnected  = false;

    // BUG-18: SYN_DROPPED state must persist across process_device() calls.
    // The Linux input protocol says all events between a SYN_DROPPED and the
    // next SYN_REPORT are unreliable.  A previous bug had this as a function-
    // local flag — if SYN_DROPPED fell at the end of one read batch and the
    // matching SYN_REPORT arrived in the next epoll cycle, the flag was reset
    // to false in between and the daemon would forward unreliable events.
    bool   syn_dropped   = false;

    // ── Latency statistics — see lat_stats.hpp for the full implementation ──
    // Thread safety: flush_motion() (loop thread) writes; dump_latency_stats()
    // (main thread, on SIGUSR1) reads and resets.  lat_stats::mtx serialises access.
    lat_stats lat;
};

class AccelDaemon {
public:
    AccelDaemon();
    ~AccelDaemon();

    /// Load config and start processing. Returns false on error.
    bool start(const std::string& config_path = "");

    /// Stop gracefully (joins thread — do NOT call from a signal handler).
    void stop();

    /// Signal-safe stop: sets running_ = false without joining.
    /// Call from signal handlers; the main loop will complete shutdown.
    void request_stop() { running_.store(false); }

    /// Reload config (hot reload via SIGHUP).
    bool reload();

    bool is_running() const { return running_.load(); }

    /// Set callback for logging.
    void set_log_cb(std::function<void(const std::string&)> cb) { log_cb_ = cb; }

    /// Enable/disable verbose debug output.
    void set_verbose(bool v) { verbose_ = v; }

    /// Print per-device latency statistics to stdout and reset counters.
    /// Call periodically (e.g. on SIGUSR1) to observe real-time performance.
    void dump_latency_stats();

    /// Atomically read-and-clear the IPC latency-dump flag.
    /// The IPC server (input-group accessible) sets this flag when an
    /// unprivileged client requests a stats dump; the main thread polls it
    /// alongside the SIGUSR1 path so latency reporting works without
    /// kill() permission on the (root-owned) daemon process.
    bool consume_latency_dump_request() {
        return latency_dump_flag_.exchange(false);
    }

    /// Build a JSON status string describing the daemon's current state.
    /// Thread-safe: acquires per-device lat_mtx for snapshot reads.
    std::string status_json() const;

    // ── Unix socket IPC ───────────────────────────────────────────────────────
    // start_ipc_server() opens a Unix domain socket at the given path and spawns
    // ipc_thread_ to accept connections.  stop_ipc_server() closes the socket and
    // joins the thread.  The path is typically $XDG_RUNTIME_DIR/rawaccel.sock or
    // /run/rawaccel.sock.  The protocol is line-based:
    //   client → "status\n"  → server → <json>\n
    //   client → "ping\n"    → server → "pong\n"
    //   client → "reload\n"  → server → {"ok":true,...}\n  (schedules config reload)
    //   client → "latency\n" → server → {"ok":true,...}\n  (schedules latency dump)
    //   client → <other>\n   → server → {"error":"unknown command"}\n
    bool start_ipc_server(const std::string& sock_path);
    void stop_ipc_server();
    static std::string ipc_sock_path(); ///< Derive socket path from PID-file convention.

private:
    void run_loop();
    bool setup_devices();
    void teardown_devices();
    bool open_input_device(mouse_device& dev);
    bool create_virtual_device(mouse_device& dev);
    void process_device(mouse_device& dev);
    void apply_profile(mouse_device& dev, const device_profile& prof);
    /// Searches by device_id match first, then active_profile, then the first profile.
    const device_profile* find_profile(const std::string& dev_id) const;
    void handle_hotplug();
    void do_hotplug_scan();
    void log(const std::string& msg, bool verbose_only = false);

    std::atomic<bool>   running_             { false };
    std::atomic<bool>   reload_flag_         { false };
    std::atomic<bool>   latency_dump_flag_   { false };
    std::thread         loop_thread_;
    app_config          config_;
    std::string         config_path_;
    std::vector<mouse_device> devices_;
    // Guards devices_ against concurrent access from ipc_thread_ (status_json)
    // and loop_thread_ (setup/hotplug/cleanup).  Always held briefly (< 1µs typical).
    mutable std::mutex        devices_mutex_;
    std::function<void(const std::string&)> log_cb_;
    bool                verbose_     = false;

    // epoll fd for event loop
    int epoll_fd_   = -1;
    // inotify fd + watch descriptor for /dev/input hot-plug
    int inotify_fd_ = -1;
    int inotify_wd_ = -1;
    // Track which paths are already opened (avoid re-grabbing on spurious events)
    std::set<std::string> opened_paths_;
    // O(1) fd -> device index lookup (avoids linear scan in hot path)
    std::unordered_map<int, size_t> fd_to_dev_;
    // Hot-plug retry counter — accessed only from run_loop() thread; no sync needed.
    // Using atomic here for explicitness and to silence potential sanitizer warnings.
    std::atomic<int> hotplug_retry_ { 0 };
    // Pending hot-plug flag: set by inotify, processed at safe point in loop
    std::atomic<bool> pending_hotplug_ { false };

    // IPC server state
    std::thread         ipc_thread_;
    std::atomic<int>    ipc_sock_fd_  { -1 };  // listening socket (atomic: shared with ipc_thread_)
    std::string         ipc_sock_path_;
    std::atomic<bool>   ipc_running_  { false };

    void ipc_serve_loop(); // runs in ipc_thread_
    void handle_ipc_client(int client_fd);
};

} // namespace rawaccel
