#include "daemon.hpp"
#include <csignal>
#include <iostream>
#include <fstream>
#include <string>
#include <cstring>
#include <atomic>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <limits.h>
#include <cerrno>
#include <pwd.h>
#include <vector>

// Version number comes from RAWACCEL_VERSION in rawaccel-base.hpp (single source of truth).
static constexpr const char* VERSION    = rawaccel::RAWACCEL_VERSION;
static constexpr const char* PID_FILE   = "/run/rawaccel.pid";
static constexpr const char* PID_FILE2  = "/tmp/rawaccel.pid"; // root-fallback

/// D5: Returns $XDG_RUNTIME_DIR/rawaccel.pid for a per-user PID file.
/// Returns an empty string if XDG_RUNTIME_DIR is not set.
static std::string xdg_pid_path() {
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg && xdg[0] != '\0') return std::string(xdg) + "/rawaccel.pid";
    return {};
}

// K2: atomic pointer — safe to load() from a signal handler
static std::atomic<rawaccel::AccelDaemon*> g_daemon { nullptr };
static std::string g_pid_file;

/// K1: Atomically write PID file using O_CREAT|O_EXCL.
/// Returns false if the file already exists (another daemon instance is running).
static bool write_pid(const std::string& path) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return false; // EEXIST → daemon already running
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
    // Write the full PID; if anything goes wrong (ENOSPC, EIO) remove the
    // half-written file so the next startup doesn't see a stale empty PID.
    ssize_t written = 0;
    while (written < n) {
        ssize_t w = write(fd, buf + written, (size_t)(n - written));
        if (w < 0) {
            if (errno == EINTR) continue;
            close(fd);
            unlink(path.c_str());
            return false;
        }
        if (w == 0) break; // shouldn't happen for a regular file
        written += w;
    }
    if (written != n) {
        close(fd);
        unlink(path.c_str());
        return false;
    }
    fsync(fd); // ensure PID hits disk before another instance probes it
    // BUG-17: open()'s mode argument is masked by the process umask.  When
    // the systemd unit sets UMask=0077 (default-private files), our 0644
    // becomes 0600 — root-only — and rawaccel-cli running as a normal
    // user can no longer read the PID to display "running" status.
    // The PID number is not sensitive (visible in /proc anyway), so force
    // 0644 explicitly via fchmod() which ignores the umask.
    fchmod(fd, 0644);
    close(fd);
    g_pid_file = path;
    return true;
}

static void remove_pid() {
    if (!g_pid_file.empty()) {
        unlink(g_pid_file.c_str());
        g_pid_file.clear();
    }
}

// K3: atomic flag for SIGUSR1 — dump_latency_stats() is not async-signal-safe
// (it uses cout), so we set a flag in the handler and call it from the main loop.
static std::atomic<bool> g_dump_latency { false };

static void handle_signal(int sig) {
    // Signal handlers must only call async-signal-safe functions.
    // AccelDaemon::reload() sets an atomic flag — safe.
    // AccelDaemon::stop() calls thread::join() — NOT safe from a signal handler.
    // Solution: set running_ = false here; the main loop detects it and calls stop().
    // K2: g_daemon is atomic — load() from signal handler is safe.
    rawaccel::AccelDaemon* d = g_daemon.load();
    if (!d) return;
    if (sig == SIGHUP) {
        d->reload();
    } else if (sig == SIGUSR1) {
        // K3: set flag; actual dump happens in the main loop (safe to use cout there)
        g_dump_latency.store(true);
    } else {
        d->request_stop(); // sets running_ = false, does NOT join
    }
}

// Resolve real user's config when running under sudo.
// Uses getpwnam_r (reentrant, no shell, no injection risk) instead of popen.
static std::string resolve_config_path() {
    const char* sudo_user = getenv("SUDO_USER");
    if (sudo_user && sudo_user[0] != '\0') {
        struct passwd  pwd_buf;
        struct passwd* result = nullptr;
        // Allocate a reasonably large buffer for getpwnam_r
        std::vector<char> buf(16384);
        int ret = getpwnam_r(sudo_user, &pwd_buf, buf.data(), buf.size(), &result);
        if (ret == 0 && result && result->pw_dir && result->pw_dir[0] != '\0') {
            std::string path = std::string(result->pw_dir) +
                               "/.config/rawaccel/settings.json";
            return path; // return even if not yet existing — daemon will create it
        }
    }
    return rawaccel::find_config_path();
}

/// Validate a user-supplied config path when the daemon runs as root.
/// Returns true if the path is acceptable; prints an error and returns false otherwise.
/// Checks:
///   1. Path must not be empty.
///   2. Resolved (realpath) path must have a ".json" extension.
///   3. If the file exists, it must be a regular file (not /dev/*, /proc/*, special nodes).
///   4. If the file exists, it must be ≤ 4 MB (sanity guard against reading huge files).
static bool validate_config_path(const std::string& path) {
    if (path.empty()) {
        std::cerr << "[rawaccel] Config path is empty.\n";
        return false;
    }

    // Resolve to canonical path (removes ../ traversal, symlinks, etc.)
    char resolved[PATH_MAX] = {};
    if (realpath(path.c_str(), resolved) != nullptr) {
        // File exists — validate it
        struct stat st {};
        if (stat(resolved, &st) == 0) {
            if (!S_ISREG(st.st_mode)) {
                std::cerr << "[rawaccel] Config path '" << resolved
                          << "' is not a regular file.\n";
                return false;
            }
            constexpr off_t MAX_CONFIG_BYTES = 4L * 1024L * 1024L; // 4 MB
            if (st.st_size > MAX_CONFIG_BYTES) {
                std::cerr << "[rawaccel] Config file is too large ("
                          << st.st_size << " bytes, max " << MAX_CONFIG_BYTES << ").\n";
                return false;
            }
        }
        // Require .json extension on the resolved path
        std::string rp(resolved);
        if (rp.size() < 5 || rp.substr(rp.size() - 5) != ".json") {
            std::cerr << "[rawaccel] Config path '" << rp
                      << "' does not have a .json extension.\n";
            return false;
        }
    } else {
        // File does not yet exist — validate the path string itself
        std::string p(path);
        if (p.size() < 5 || p.substr(p.size() - 5) != ".json") {
            std::cerr << "[rawaccel] Config path '" << p
                      << "' does not have a .json extension.\n";
            return false;
        }
        // Disallow obviously dangerous prefixes even before the file exists
        for (const char* bad : { "/proc/", "/sys/", "/dev/" }) {
            if (p.rfind(bad, 0) == 0) {
                std::cerr << "[rawaccel] Config path '" << p
                          << "' is in a disallowed directory.\n";
                return false;
            }
        }
    }
    return true;
}

static void print_usage(const char* argv0) {
    std::cout
        << "Usage: " << argv0 << " [OPTIONS]\n\n"
        << "Options:\n"
        << "  -c, --config PATH   Config file (default: ~/.config/rawaccel/settings.json)\n"
        << "  -v, --verbose       Verbose logging to stdout\n"
        << "  -V, --version       Print version\n"
        << "  -h, --help          Show this help\n\n"
        << "Signals:\n"
        << "  SIGHUP   Hot-reload config\n"
        << "  SIGTERM  Stop daemon\n"
        << "  SIGINT   Stop daemon\n"
        << "  SIGUSR1  Dump per-device processing latency stats to stdout\n"
        << "           (use: kill -USR1 $(cat /run/rawaccel.pid))\n";
}

int main(int argc, char* argv[]) {
    std::string config_path;
    bool verbose = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-V") == 0 || strcmp(argv[i], "--version") == 0) {
            std::cout << "rawaccel-daemon " << VERSION << "\n";
            return 0;
        } else if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) && i + 1 < argc) {
            config_path = argv[++i];
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        }
    }

    // K1: Atomic PID write via O_CREAT|O_EXCL — fail if already running.
    // D5: Priority: $XDG_RUNTIME_DIR (user runtime), /run/ (root), /tmp/ (fallback).
    std::string xdg_pid = xdg_pid_path();
    bool pid_written = (!xdg_pid.empty() && write_pid(xdg_pid))
                    || write_pid(PID_FILE)
                    || write_pid(PID_FILE2);
    if (pid_written)
        std::cout << "PID file: " << g_pid_file << "\n";
    if (!pid_written) {
        // Both attempts failed — daemon is likely already running.
        // Check for a stale PID file (left behind by a crashed previous instance):
        // if the recorded PID no longer exists, delete the file and retry.
        auto try_clear_stale = [](const char* path) -> bool {
            int fd = open(path, O_RDONLY);
            if (fd < 0) return false;
            char buf[32] = {};
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n <= 0) { unlink(path); return false; }
            errno = 0;
            char* end = nullptr;
            long val = std::strtol(buf, &end, 10);
            // BUG-7: long → pid_t (int) cast UB if val out of int range.
            // A corrupted PID file should never let us cast garbage to int.
            pid_t pid = (end > buf && errno == 0 && val > 0 &&
                         val <= INT_MAX) ? static_cast<pid_t>(val) : 0;
            if (pid > 0 && kill(pid, 0) != 0 && errno == ESRCH) {
                // Process does not exist — remove the stale PID file
                unlink(path);
                return true;
            }
            return false; // process is still running
        };

        // D5: also include the XDG path in the stale-check list
        bool cleared = (!xdg_pid.empty() && try_clear_stale(xdg_pid.c_str()))
                    || try_clear_stale(PID_FILE)
                    || try_clear_stale(PID_FILE2);
        bool retry_ok = cleared &&
                        ((!xdg_pid.empty() && write_pid(xdg_pid))
                         || write_pid(PID_FILE)
                         || write_pid(PID_FILE2));
        if (!retry_ok) {
            std::cerr << "[rawaccel] Another instance may already be running "
                         "(PID file exists). Use 'rawaccel-cli stop' to stop it.\n";
            return 1;
        }
    }

    rawaccel::AccelDaemon daemon;
    // K2: atomic store — allows safe load() from the signal handler
    g_daemon.store(&daemon);

    // Always log to stdout (systemd journal captures it), verbose = also show debug
    daemon.set_log_cb([verbose](const std::string& msg) {
        std::cout << "[rawaccel] " << msg << std::endl;
    });
    daemon.set_verbose(verbose);

    // Use sigaction (POSIX) instead of std::signal (implementation-defined).
    // SA_RESTART makes blocking syscalls (read/recv/poll) restart automatically
    // instead of failing with EINTR, simplifying the loop's error handling.
    struct sigaction sa{};
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP,  &sa, nullptr);
    sigaction(SIGUSR1, &sa, nullptr); // latency stats dump
    // Ignore SIGPIPE so a broken IPC client connection just returns EPIPE
    // from send() instead of killing the daemon.  (MSG_NOSIGNAL also covers
    // this on the send-site, but defending in depth is cheap.)
    struct sigaction sa_ign{};
    sa_ign.sa_handler = SIG_IGN;
    sigemptyset(&sa_ign.sa_mask);
    sigaction(SIGPIPE, &sa_ign, nullptr);

    std::cout << "RawAccel Linux Daemon v" << VERSION << "\n";

    if (config_path.empty())
        config_path = resolve_config_path();

    // V1: Validate user-supplied config path before passing to daemon (runs as root).
    // Auto-resolved paths from resolve_config_path() are always safe (home dir / .json).
    // Only validate explicitly-provided paths (argv -c / --config).
    {
        // We can detect explicit supply: if config_path is from argv it was set before
        // resolve_config_path() was called; we re-scan argv to check.
        bool explicit_path = false;
        for (int i = 1; i < argc; i++) {
            if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0)
                && i + 1 < argc) {
                explicit_path = true;
                break;
            }
        }
        if (explicit_path && !validate_config_path(config_path)) {
            remove_pid();
            return 1;
        }
    }

    std::cout << "Config: " << config_path << "\n";

    // Start IPC server (non-fatal if it fails — daemon still works without it)
    std::string sock_path = rawaccel::AccelDaemon::ipc_sock_path();
    if (!daemon.start_ipc_server(sock_path))
        std::cerr << "[rawaccel] IPC socket unavailable (non-fatal)\n";
    else
        std::cout << "IPC socket: " << sock_path << "\n";

    if (!daemon.start(config_path)) {
        std::cerr << "[rawaccel] Failed to start. Tips:\n"
                  << "  - Add yourself to 'input' group: sudo usermod -aG input $USER\n"
                  << "  - Load uinput: sudo modprobe uinput\n"
                  << "  - Stop abrek if running: sudo systemctl stop abrek\n";
        // IPC server was started before start() — must be stopped to join its thread.
        // Otherwise the daemon destructor sees a still-running thread and SIGABRTs.
        daemon.stop_ipc_server();
        g_daemon.store(nullptr);
        remove_pid();
        return 1;
    }

    // Main thread: spin until signal sets running_ = false, then do clean shutdown.
    // Also handles SIGUSR1 (latency dump) which cannot safely use cout from a signal handler.
    // BUG-4: input-group users can't kill(root_daemon, SIGUSR1) — they get EPERM.
    // The IPC server (input-group writable socket) accepts a "latency" command
    // that sets daemon.latency_dump_flag_; we drain it here on the same path.
    while (daemon.is_running()) {
        sleep(1);
        if (g_dump_latency.exchange(false) ||
            daemon.consume_latency_dump_request())
            daemon.dump_latency_stats();
    }
    // request_stop() was called from signal handler; now safe to join the loop thread.
    daemon.stop();
    daemon.stop_ipc_server();

    // K2: prevent the signal handler from accessing the daemon after this point — null first, then clean up
    g_daemon.store(nullptr);
    remove_pid();
    return 0;
}
