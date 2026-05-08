// ── Daemon communication: PID lookup, status display, signal sending ──────────
#include <sys/socket.h>
#include <sys/un.h>
#include <cerrno>
#include <climits>
#include "../include/nlohmann/json.hpp"

// ── KDE / libinput acceleration detection ────────────────────────────────────

/// Returns true if the current desktop session is KDE Plasma.
static bool is_kde_session() {
    const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");
    if (desktop && (strstr(desktop, "KDE") || strstr(desktop, "plasma")))
        return true;
    const char* session = std::getenv("DESKTOP_SESSION");
    if (session && (strstr(session, "plasma") || strstr(session, "kde")))
        return true;
    return false;
}

/// Returns true if the current session is Wayland.
static bool is_wayland_session() {
    const char* wt = std::getenv("WAYLAND_DISPLAY");
    if (wt && wt[0] != '\0') return true;
    const char* xdg_st = std::getenv("XDG_SESSION_TYPE");
    if (xdg_st && strcmp(xdg_st, "wayland") == 0) return true;
    return false;
}

/// Check KDE's kwinrc to see if pointer acceleration is set to flat/none.
/// Returns:
///   0 = acceleration is disabled (flat) — OK
///   1 = acceleration is enabled / not flat — WARNING
///  -1 = cannot determine (kwinrc not found or key missing)
static int kde_libinput_accel_state() {
    // kwinrc location: $XDG_CONFIG_HOME/kwinrc  or  ~/.config/kwinrc
    std::string kwinrc_path;
    const char* xdg_cfg = std::getenv("XDG_CONFIG_HOME");
    if (xdg_cfg && xdg_cfg[0] != '\0') {
        kwinrc_path = std::string(xdg_cfg) + "/kwinrc";
    } else {
        const char* home = std::getenv("HOME");
        if (!home || home[0] == '\0') return -1;
        kwinrc_path = std::string(home) + "/.config/kwinrc";
    }

    FILE* f = fopen(kwinrc_path.c_str(), "r");
    if (!f) return -1;

    // Scan for [Libinput] section and PointerAcceleration key
    bool in_libinput = false;
    char line[512];
    int result = -1;
    while (fgets(line, sizeof(line), f)) {
        // Strip newline
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        if (line[0] == '[') {
            in_libinput = (strncmp(line, "[Libinput]", 10) == 0);
            continue;
        }
        if (!in_libinput) continue;

        // PointerAccelerationProfile=1  (1=flat/none, 2=adaptive)
        if (strncmp(line, "PointerAccelerationProfile=", 27) == 0) {
            // BUG-6: atoi() invokes UB on out-of-range input (a malicious
            // kwinrc with "1e26" would propagate UB through the GUI).
            // strtol with explicit range/errno handling is safe.
            errno = 0;
            char* end = nullptr;
            long val = strtol(line + 27, &end, 10);
            int v = (end != line + 27 && errno == 0 &&
                     val >= INT_MIN && val <= INT_MAX) ? (int)val : 0;
            // 1 = flat (disabled) — good; 2 = adaptive (enabled) — bad
            result = (v == 1) ? 0 : 1;
        }
        // PointerAcceleration=0  (0 = no extra gain on top of flat)
        if (strncmp(line, "PointerAcceleration=", 20) == 0) {
            // BUG-16: atof() invokes UB on out-of-range input (C99 7.20.1.1).
            // A malicious kwinrc with "PointerAcceleration=1e1000" would
            // propagate UB through the GUI.  Use strtod + errno check; treat
            // unparseable / non-finite values as "non-zero" (i.e. accel on).
            errno = 0;
            char* end = nullptr;
            double val = std::strtod(line + 20, &end);
            bool parsed = (end != line + 20 && errno == 0 && std::isfinite(val));
            // A near-zero PointerAcceleration with flat profile is fine
            if (result == 0 && (!parsed || std::fabs(val) > 0.05)) result = 1;
        }
    }
    fclose(f);
    return result;
}

/// Candidate IPC socket paths.  Same rationale as in cli/main.cpp:
/// when the daemon is a *system* service it doesn't have XDG_RUNTIME_DIR
/// set so it lands on /run/rawaccel.sock, while the GUI is started from a
/// user session that *does* have XDG_RUNTIME_DIR — without walking both,
/// the GUI's IPC reload silently failed and fell back to SIGHUP (which
/// itself fails with EPERM against a root daemon).
static std::vector<std::string> daemon_sock_candidates() {
    std::vector<std::string> v;
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg && xdg[0] != '\0') v.push_back(std::string(xdg) + "/rawaccel.sock");
    v.push_back("/run/rawaccel.sock");
    return v;
}

/// Send a one-line command to the daemon socket and return the response.
/// Returns an empty string on failure.
static std::string daemon_ipc_query(const std::string& cmd) {
    for (const auto& sock : daemon_sock_candidates()) {
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) continue;

        struct timeval tv { .tv_sec = 0, .tv_usec = 100000 }; // 100 ms
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (sock.size() >= sizeof(addr.sun_path)) { close(fd); continue; }
        strncpy(addr.sun_path, sock.c_str(), sizeof(addr.sun_path) - 1);
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close(fd); continue; // try the next candidate
        }

        std::string req = cmd + "\n";
        send(fd, req.c_str(), req.size(), MSG_NOSIGNAL);

        // Read response (up to 64KB)
        std::string resp;
        char buf[4096];
        while (true) {
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            resp.append(buf, (size_t)n);
            if (resp.size() > 65536) break; // safety cap
        }
        close(fd);
        return resp;
    }
    return {};
}

std::string daemon_config_path_from_ipc() {
    std::string resp = daemon_ipc_query("status");
    if (resp.empty()) return {};
    auto j = nlohmann::json::parse(resp, nullptr, false);
    if (!j.is_discarded() && j.contains("config") && j["config"].is_string())
        return j["config"].get<std::string>();
    return {};
}

bool daemon_reload_via_any_path(std::string* err_out) {
    std::string ipc_resp = daemon_ipc_query("reload");
    if (!ipc_resp.empty() && ipc_resp.find("\"ok\":true") != std::string::npos) {
        if (err_out) err_out->clear();
        return true;
    }
    return daemon_send_signal(SIGHUP, err_out);
}

pid_t read_daemon_pid() {
    // 1. Try PID files first (fastest)
    // N1: daemon prefers $XDG_RUNTIME_DIR/rawaccel.pid — must check it here too.
    std::vector<std::string> pid_paths;
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg && xdg[0] != '\0') pid_paths.push_back(std::string(xdg) + "/rawaccel.pid");
    pid_paths.push_back("/run/rawaccel.pid");
    pid_paths.push_back("/tmp/rawaccel.pid");

    for (auto& path : pid_paths) {
        FILE* fp = fopen(path.c_str(), "r");
        if (!fp) continue;
        pid_t pid = 0;
        char line[64] = {};
        if (fgets(line, sizeof(line), fp)) {
            char* end = nullptr;
            errno = 0;
            long parsed = std::strtol(line, &end, 10);
            if (end != line && errno != ERANGE && parsed > 0 &&
                parsed <= static_cast<long>(std::numeric_limits<pid_t>::max()))
                pid = static_cast<pid_t>(parsed);
        }
        fclose(fp);
        if (pid > 0 && kill(pid, 0) == 0) return pid;
    }

    // 2. Fallback: scan /proc for rawaccel-daemon process name
    //    Handles the case where daemon runs but PID file write failed (e.g. /run not writable).
    DIR* proc = opendir("/proc");
    if (proc) {
        struct dirent* ent;
        while ((ent = readdir(proc)) != nullptr) {
            // Only numeric entries are PIDs
            bool is_num = true;
            for (const char* p = ent->d_name; *p; p++)
                if (*p < '0' || *p > '9') { is_num = false; break; }
            if (!is_num) continue;

            std::string comm_path = std::string("/proc/") + ent->d_name + "/comm";
            FILE* cf = fopen(comm_path.c_str(), "r");
            if (!cf) continue;
            char comm[64] = {};
            // Zero-init guarantees a NUL terminator even if fgets yields nothing.
            (void)!fgets(comm, sizeof(comm), cf);
            fclose(cf);
            // strip newline
            size_t len = strlen(comm);
            if (len > 0 && comm[len-1] == '\n') comm[len-1] = '\0';

            if (strcmp(comm, "rawaccel-daemon") == 0) {
                // BUG-6: atoi(d_name) is UB if the directory name doesn't
                // fit in `int`.  /proc only exposes numeric PIDs (pid_t,
                // typically 4194304 max) but be defensive — strtol +
                // range check before the pid_t cast.
                errno = 0;
                char* end = nullptr;
                long v = strtol(ent->d_name, &end, 10);
                if (end != ent->d_name && errno == 0 && v > 0 &&
                    v <= INT_MAX) {
                    pid_t pid = static_cast<pid_t>(v);
                    closedir(proc);
                    return pid;
                }
            }
        }
        closedir(proc);
    }
    return 0;
}

bool daemon_running() {
    // Fast path: try IPC ping first (socket exists only while daemon is running)
    std::string pong = daemon_ipc_query("ping");
    if (!pong.empty() && pong.find("pong") != std::string::npos) return true;
    // Fallback: check PID file / /proc scan
    return read_daemon_pid() > 0;
}

void update_daemon_status(AppState* S) {
    bool running = daemon_running();
    if (running) {
        gtk_label_set_markup(GTK_LABEL(S->daemon_status), ui_text(S,
            "<span foreground='#40c040'>● Daemon running</span>",
            "<span foreground='#40c040'>● Daemon çalışıyor</span>"));
    } else {
        gtk_label_set_markup(GTK_LABEL(S->daemon_status), ui_text(S,
            "<span foreground='#c04040'>● Daemon stopped</span>",
            "<span foreground='#c04040'>● Daemon durdu</span>"));
    }
    // Update button sensitivity based on whether the daemon is running
    if (S->apply_btn)        gtk_widget_set_sensitive(S->apply_btn,        running);
    if (S->daemon_start_btn) gtk_widget_set_sensitive(S->daemon_start_btn, !running);
    if (S->daemon_stop_btn)  gtk_widget_set_sensitive(S->daemon_stop_btn,  running);
    if (S->daemon_reload_btn)gtk_widget_set_sensitive(S->daemon_reload_btn,running);
}

gboolean poll_daemon_status(gpointer user_data) {
    update_daemon_status(static_cast<AppState*>(user_data));
    return G_SOURCE_CONTINUE;
}

/// Send a signal to the daemon.  Returns true on success.
/// On EPERM (user not in input group / daemon owned by different user) returns
/// false and sets a human-readable error in *err_out if non-null.
bool daemon_send_signal(int sig, std::string* err_out) {
    pid_t pid = read_daemon_pid();
    if (pid <= 0) {
        if (err_out) *err_out = "Daemon is not running.";
        return false;
    }
    if (kill(pid, sig) == 0) return true;

    // kill() failed — build a useful message
    if (err_out) {
        if (errno == EPERM) {
            *err_out =
                "Permission denied (EPERM) — cannot signal the daemon.\n"
                "Fix: ensure you are in the 'input' group:\n"
                "  sudo usermod -aG input $USER  (then log out and back in)\n"
                "Or restart the daemon from the GUI using pkexec.";
        } else {
            *err_out = std::string("kill() failed: ") + strerror(errno);
        }
    }
    return false;
}
