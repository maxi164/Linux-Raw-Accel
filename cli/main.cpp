#include "../include/config.hpp"
#include "../include/rawaccel.hpp"
#include "../include/nlohmann/json.hpp"
#include <iostream>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <iomanip>
#include <sstream>
#include <cmath>
#include <cctype>
#include <csignal>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>

// Version number comes from RAWACCEL_VERSION in rawaccel-base.hpp (single source of truth).
static constexpr const char* VERSION = rawaccel::RAWACCEL_VERSION;

using namespace rawaccel;

// ── Daemon communication ──────────────────────────────────────────────────────

/// Check whether a PID is alive by probing /proc/<pid>.
/// Unlike kill(pid, 0), this works even when the daemon runs as root
/// and the caller is an unprivileged user (kill -0 returns EPERM in that case).
static bool pid_alive(pid_t pid) {
    if (pid <= 0) return false;
    // /proc/<pid> exists as long as the process is alive — readable by any user.
    std::string proc_path = "/proc/" + std::to_string(pid);
    struct stat st{};
    return stat(proc_path.c_str(), &st) == 0;
}

/// Result codes from a daemon-signal attempt — lets the caller distinguish
/// "daemon not running" from "running but I lack permission" (the daemon
/// runs as root via systemd, and `kill()` from an unprivileged user returns
/// EPERM).  This was previously squashed into a single bool, producing the
/// misleading "is it running?" message even when the daemon was up.
enum class signal_result { sent, not_running, permission_denied, other };

static signal_result send_signal_to_daemon(int sig) {
    // N6: daemon prefers $XDG_RUNTIME_DIR/rawaccel.pid — check it first.
    std::vector<std::string> paths;
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg && xdg[0] != '\0') paths.push_back(std::string(xdg) + "/rawaccel.pid");
    paths.push_back("/run/rawaccel.pid");
    paths.push_back("/tmp/rawaccel.pid");

    for (auto& path : paths) {
        std::ifstream f(path);
        if (!f.is_open()) continue;
        pid_t pid = 0;
        f >> pid;
        if (!pid_alive(pid)) continue;
        if (kill(pid, sig) == 0) return signal_result::sent;
        if (errno == EPERM)      return signal_result::permission_denied;
        return signal_result::other;
    }
    return signal_result::not_running;
}

// ── Daemon IPC (Unix socket) ──────────────────────────────────────────────────

/// Candidate IPC socket paths, in the order the daemon tries them.
/// The daemon writes the *first* one it can bind: $XDG_RUNTIME_DIR (only set
/// for user services), then /run (system service — what systemd uses).
/// CLI is invoked from a user shell, where XDG_RUNTIME_DIR *is* set, so a
/// naive `daemon_sock_path()` would point at /run/user/1000/rawaccel.sock
/// while the system daemon listens on /run/rawaccel.sock.  Walk the whole
/// list so both deployments work.
static std::vector<std::string> daemon_sock_candidates() {
    std::vector<std::string> v;
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg && xdg[0] != '\0') v.push_back(std::string(xdg) + "/rawaccel.sock");
    v.push_back("/run/rawaccel.sock");
    return v;
}

/// Send a one-line command to the daemon socket and return the response.
/// Empty string on failure.  Used so unprivileged users (in the input group)
/// can ask the root-owned daemon to reload without needing kill() permission.
static std::string daemon_ipc_query(const std::string& cmd) {
    for (const auto& sock : daemon_sock_candidates()) {
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) continue;

        struct timeval tv { .tv_sec = 0, .tv_usec = 200000 }; // 200 ms
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (sock.size() >= sizeof(addr.sun_path)) { close(fd); continue; }
        std::strncpy(addr.sun_path, sock.c_str(), sizeof(addr.sun_path) - 1);
        if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close(fd); continue; // try the next candidate
        }

        std::string req = cmd + "\n";
        (void)!send(fd, req.c_str(), req.size(), MSG_NOSIGNAL);

        std::string resp;
        char buf[4096];
        while (true) {
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            resp.append(buf, (size_t)n);
            if (resp.size() > 65536) break;
        }
        close(fd);
        return resp;
    }
    return {};
}

/// Return the config path currently used by the running daemon, if reachable.
/// This prevents the CLI from editing ~/.config/rawaccel while the systemd
/// daemon is actually reading /etc/rawaccel/settings.json.
static std::string daemon_config_path_from_ipc() {
    std::string resp = daemon_ipc_query("status");
    if (resp.empty()) return {};
    auto j = nlohmann::json::parse(resp, nullptr, false);
    if (!j.is_discarded() && j.contains("config") && j["config"].is_string())
        return j["config"].get<std::string>();
    return {};
}

/// Ask the daemon to reload its config.  Tries the IPC socket first (works
/// for any user in the input group regardless of who the daemon runs as),
/// then falls back to SIGHUP for older daemons that don't speak IPC.
/// @return  true if either path succeeded.
static bool daemon_reload_via_any_path() {
    std::string resp = daemon_ipc_query("reload");
    if (resp.find("\"ok\":true") != std::string::npos) return true;
    return send_signal_to_daemon(SIGHUP) == signal_result::sent;
}

/// Print a uniform "couldn't reach daemon" diagnostic.  Suggests the right
/// remediation based on whether kill() failed with EPERM (sudo / systemctl)
/// or because the PID file simply isn't there.
static void print_signal_failure(signal_result r, const char* action, const char* kill_signal) {
    switch (r) {
    case signal_result::permission_denied:
        std::cerr << "Permission denied while trying to " << action << " the daemon.\n"
                  << "The daemon is running as root; signal it with:\n";
        if (std::strcmp(action, "reload") == 0 || std::strcmp(action, "stop") == 0)
            std::cerr << "  sudo systemctl " << action << " rawaccel    (preferred)\n";
        std::cerr << "  sudo kill -" << kill_signal << " $(cat /run/rawaccel.pid)\n";
        break;
    case signal_result::not_running:
        std::cerr << "Daemon is not running.  Start it with: sudo systemctl start rawaccel\n";
        break;
    default:
        std::cerr << "Could not " << action << " the daemon (errno=" << errno << ").\n";
        break;
    }
}

static int finite_double_to_int(double v) {
    if (v < static_cast<double>(INT_MIN)) return INT_MIN;
    if (v > static_cast<double>(INT_MAX)) return INT_MAX;
    return static_cast<int>(v);
}

static app_config make_default_config() {
    app_config cfg;
    device_profile dp;
    dp.name = "default";
    dp.dev_cfg.dpi = 800;
    dp.dev_cfg.polling_rate = 1000;
    cfg.profiles.push_back(dp);
    return cfg;
}

/// BUG-24: Validate a user-supplied profile name.
/// Previously cmd_create / cmd_import accepted *any* string — including
/// "../../../etc/passwd", strings containing newlines, control characters
/// or NUL bytes — which is harmless from a security standpoint (the name
/// is never used as a filesystem path; cf. grep for fopen/fs::path) but
/// breaks `rawaccel-cli list` / GUI rendering and the JSON export round-trip
/// (a name containing "\n" prints across two profile entries in `list`).
///
/// Rules:
///   • non-empty
///   • length ≤ 128 chars (room for the JSON struct's `char name[256]`)
///   • no control characters (anything < 0x20 except space) or NUL or DEL
///   • no path separators "/" or "\" (defensive — names should never be paths)
///   • no leading/trailing whitespace
///   • not exactly "." or ".."
///
/// Returns an empty string on success, or a human-readable reason on failure.
static std::string validate_profile_name(const std::string& name) {
    if (name.empty())                  return "must not be empty";
    if (name.size() > 128)             return "must be 128 characters or fewer";
    if (std::isspace(static_cast<unsigned char>(name.front())) ||
        std::isspace(static_cast<unsigned char>(name.back())))
                                       return "must not start or end with whitespace";
    if (name == "." || name == "..")   return "reserved name";
    for (unsigned char c : name) {
        if (c == '\0')                 return "contains a NUL byte";
        if (c == 0x7f)                 return "contains the DEL character";
        if (c < 0x20)                  return "contains a control character "
                                              "(newline, tab, etc.)";
        if (c == '/' || c == '\\')     return "contains a path separator (/ or \\)";
    }
    return {};
}

/// Returns true for "no file at this path that holds any user data".
///
/// Plain ENOENT obviously qualifies, but a 0-byte file (e.g. created by
/// `mktemp` or `touch`) and a file containing only whitespace also do —
/// they hold nothing worth preserving, and the previous strict-existence
/// check made the CLI refuse to write into such paths.  That broke
/// scripts, CI pipelines and any "mktemp + rawaccel-cli -c …" workflow
/// with a confusing "Refusing to overwrite existing config" error.
static bool path_is_missing(const std::string& path) {
    errno = 0;
    if (access(path.c_str(), F_OK) != 0) return errno == ENOENT;
    // File exists — peek at its size and contents.
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return false;
    if (st.st_size == 0) return true; // 0-byte file is morally empty
    // Up to 4 KB scan for non-whitespace.  If the entire file is just
    // whitespace, treat it as missing.  Bound the read so a buggy huge
    // file doesn't make `list` slow.
    std::ifstream f(path);
    if (!f.is_open()) return false;
    char buf[4096];
    f.read(buf, sizeof(buf));
    std::streamsize n = f.gcount();
    for (std::streamsize i = 0; i < n; ++i)
        if (!std::isspace(static_cast<unsigned char>(buf[i]))) return false;
    return true;
}

// ── Profile display ────────────────────────────────────────────────────────────

static void print_accel_args(const accel_args& a, const std::string& prefix = "  ") {
    auto mode_str = [](accel_mode m) -> std::string {
        switch (m) {
        case accel_mode::classic:     return "classic";
        case accel_mode::power:       return "power";
        case accel_mode::natural:     return "natural";
        case accel_mode::jump:        return "jump";
        case accel_mode::synchronous: return "synchronous";
        case accel_mode::lookup:      return "lookup";
        default:                      return "noaccel";
        }
    };

    std::cout << prefix << "mode:             " << mode_str(a.mode) << "\n";
    std::cout << std::fixed << std::setprecision(4);
    std::cout << prefix << "gain:             " << (a.gain ? "true" : "false") << "\n";
    std::cout << prefix << "acceleration:     " << a.acceleration     << "\n";
    std::cout << prefix << "input_offset:     " << a.input_offset     << "\n";
    std::cout << prefix << "output_offset:    " << a.output_offset    << "\n";
    std::cout << prefix << "scale:            " << a.scale            << "\n";
    std::cout << prefix << "exponent_classic: " << a.exponent_classic << "\n";
    std::cout << prefix << "exponent_power:   " << a.exponent_power   << "\n";
    std::cout << prefix << "limit:            " << a.limit            << "\n";
    std::cout << prefix << "motivity:         " << a.motivity         << "\n";
    std::cout << prefix << "gamma:            " << a.gamma            << "\n";
    std::cout << prefix << "decay_rate:       " << a.decay_rate       << "\n";
    std::cout << prefix << "sync_speed:       " << a.sync_speed       << "\n";
    std::cout << prefix << "smooth:           " << a.smooth           << "\n";
    std::cout << prefix << "cap:              [" << a.cap.x << ", " << a.cap.y << "]\n";
    auto cap_mode_str = [](cap_mode c) -> std::string {
        switch (c) {
        case cap_mode::in:  return "in";
        case cap_mode::io:  return "io";
        default:            return "out";
        }
    };
    std::cout << prefix << "cap_mode:         " << cap_mode_str(a.cap_mode_val) << "\n";
}

static void print_profile(const device_profile& dp) {
    auto& p = dp.prof;
    std::cout << "Profile: " << dp.name << "\n";
    std::cout << "  device_id:    " << (dp.device_id.empty() ? "(all)" : dp.device_id) << "\n";
    std::cout << "  disabled:     " << (dp.dev_cfg.disable ? "true" : "false") << "\n";
    if (p.raw_passthrough) {
        std::cout << "  raw:          true  (all processing bypassed)\n";
        if (p.accel_x.mode != accel_mode::noaccel || p.accel_y.mode != accel_mode::noaccel)
            std::cout << "  note:         acceleration parameters are ignored while raw=true\n";
        std::cout << "  dpi:          " << dp.dev_cfg.dpi         << "\n";
        std::cout << "  polling_rate: " << dp.dev_cfg.polling_rate << "\n";
        return;
    }
    std::cout << "  dpi:          " << dp.dev_cfg.dpi         << "\n";
    std::cout << "  polling_rate: " << dp.dev_cfg.polling_rate << "\n";
    std::cout << "  rotation:     " << p.degrees_rotation      << "°\n";
    std::cout << "  snap:         " << p.degrees_snap          << "°\n";
    // Use epsilon comparison: sub-ULP residue from JSON round-trip can leave
    // speed_min as e.g. 1e-17 even when the user typed "0", which would print
    // without the "(disabled)" hint and confuse status output.
    std::cout << "  speed_min:    " << p.speed_min << (std::fabs(p.speed_min) < 1e-9 ? "  (disabled)" : "") << "\n";
    std::cout << "  speed_max:    " << p.speed_max << (std::fabs(p.speed_max) < 1e-9 ? "  (disabled)" : "") << "\n";
    std::cout << "  output_dpi:   " << p.output_dpi << (std::fabs(p.output_dpi - NORMALIZED_DPI) < 1e-9 ? "  (default 1000)" : "") << "\n";
    std::cout << "  lr_ratio:     " << p.lr_output_dpi_ratio << (std::fabs(p.lr_output_dpi_ratio - 1.0) < 1e-9 ? "  (off)" : "") << "\n";
    std::cout << "  ud_ratio:     " << p.ud_output_dpi_ratio << (std::fabs(p.ud_output_dpi_ratio - 1.0) < 1e-9 ? "  (off)" : "") << "\n";
    {
        auto& sp = p.speed_processor_args;
        std::string dist = sp.whole ? (sp.lp_norm >= 16 || sp.lp_norm <= 0 ? "max" :
                           (std::fabs(sp.lp_norm - 2.0) > 1e-9 ? "lp" : "euclidean")) : "separate";
        std::cout << "  distance_mode:  " << dist << "\n";
        if (dist == "lp")
            std::cout << "  lp_norm:        " << sp.lp_norm << "\n";
        if (sp.input_speed_smooth_halflife > 0)
            std::cout << "  input_smooth_halflife:  " << sp.input_speed_smooth_halflife << "\n";
        if (sp.scale_smooth_halflife > 0)
            std::cout << "  scale_smooth_halflife:  " << sp.scale_smooth_halflife << "\n";
        if (sp.output_speed_smooth_halflife > 0)
            std::cout << "  output_smooth_halflife: " << sp.output_speed_smooth_halflife << "\n";
    }
    std::cout << "  Acceleration (X axis):\n";
    print_accel_args(p.accel_x, "    ");
    if (p.accel_x != p.accel_y) {
        std::cout << "  Acceleration (Y axis):\n";
        print_accel_args(p.accel_y, "    ");
    } else {
        std::cout << "  Acceleration (Y axis): same as X\n";
    }
}

// ── Commands ──────────────────────────────────────────────────────────────────

static int cmd_list(const app_config& cfg) {
    std::cout << "Active profile: " << cfg.active_profile << "\n\n";
    for (auto& dp : cfg.profiles) {
        print_profile(dp);
        std::cout << "\n";
    }
    return 0;
}

static int cmd_show(const app_config& cfg, const std::string& name) {
    for (auto& dp : cfg.profiles) {
        if (dp.name == name) {
            print_profile(dp);
            return 0;
        }
    }
    std::cerr << "Profile not found: " << name << "\n";
    return 1;
}

static int cmd_set(app_config& cfg, const std::string& config_path, const std::string& name) {
    for (auto& dp : cfg.profiles) {
        if (dp.name == name) {
            cfg.active_profile = name;
            save_config(cfg, config_path);
            std::cout << "Active profile set to: " << name << "\n";
            // Signal daemon to reload
            if (daemon_reload_via_any_path()) {
                std::cout << "Daemon reloaded.\n";
            } else {
                std::cout << "Note: daemon not running or not signaled.\n";
            }
            return 0;
        }
    }
    std::cerr << "Profile not found: " << name << "\n";
    return 1;
}

static int cmd_create(app_config& cfg, const std::string& config_path, const std::string& name) {
    // BUG-12 / BUG-24: profile name must pass the shared validator.
    // Beyond rejecting "" (BUG-12), this also catches names with newlines,
    // path separators or control characters — see validate_profile_name().
    if (auto reason = validate_profile_name(name); !reason.empty()) {
        std::cerr << "Invalid profile name: " << reason << ".\n";
        return 1;
    }
    // Check duplicate
    for (auto& dp : cfg.profiles) {
        if (dp.name == name) {
            std::cerr << "Profile already exists: " << name << "\n";
            return 1;
        }
    }
    device_profile dp;
    dp.name = name;
    dp.dev_cfg.dpi = 800;
    dp.dev_cfg.polling_rate = 1000;
    dp.prof.accel_x.mode = accel_mode::noaccel;
    dp.prof.accel_y.mode = accel_mode::noaccel;
    cfg.profiles.push_back(dp);
    save_config(cfg, config_path);
    std::cout << "Created profile: " << name << "\n";
    if (daemon_reload_via_any_path())
        std::cout << "Daemon reloaded.\n";
    return 0;
}

static int cmd_delete(app_config& cfg, const std::string& config_path, const std::string& name) {
    auto it = std::remove_if(cfg.profiles.begin(), cfg.profiles.end(),
                             [&](const device_profile& dp) { return dp.name == name; });
    if (it == cfg.profiles.end()) {
        std::cerr << "Profile not found: " << name << "\n";
        return 1;
    }
    cfg.profiles.erase(it, cfg.profiles.end());
    // If we just deleted the active profile, fall back to the first remaining one.
    if (cfg.active_profile == name && !cfg.profiles.empty())
        cfg.active_profile = cfg.profiles[0].name;
    save_config(cfg, config_path);
    std::cout << "Deleted profile: " << name << "\n";
    if (cfg.active_profile != name)
        std::cout << "Active profile is now: " << cfg.active_profile << "\n";
    if (daemon_reload_via_any_path())
        std::cout << "Daemon reloaded.\n";
    return 0;
}

/// Quick parameter setter: rawaccel set-param <profile> <key> <value>
static int cmd_set_param(app_config& cfg, const std::string& config_path,
                         const std::string& profile_name,
                         const std::string& key, const std::string& val)
{
    device_profile* dp = nullptr;
    for (auto& p : cfg.profiles) {
        if (p.name == profile_name) { dp = &p; break; }
    }
    if (!dp) { std::cerr << "Profile not found: " << profile_name << "\n"; return 1; }

    auto& a = dp->prof.accel_x;
    auto& ay = dp->prof.accel_y;
    // BUG-19: previously an unknown mode silently fell through to noaccel,
    // so a typo like "classicc" would silently disable accel without
    // feedback.  Returns false for unknown input so the caller can error.
    auto set_mode = [](accel_args& a, const std::string& m) -> bool {
        if      (m == "classic")     a.mode = accel_mode::classic;
        else if (m == "power")       a.mode = accel_mode::power;
        else if (m == "natural")     a.mode = accel_mode::natural;
        else if (m == "jump")        a.mode = accel_mode::jump;
        else if (m == "synchronous") a.mode = accel_mode::synchronous;
        else if (m == "lookup")      a.mode = accel_mode::lookup;
        else if (m == "noaccel" ||
                 m == "off" ||
                 m == "none")        a.mode = accel_mode::noaccel;
        else return false;
        return true;
    };
    // BUG-19: strict bool — only accept canonical truthy/falsey strings.
    auto parse_strict_bool = [](const std::string& s, bool& out) -> bool {
        if (s == "1" || s == "true"  || s == "yes" || s == "on")  { out = true;  return true; }
        if (s == "0" || s == "false" || s == "no"  || s == "off") { out = false; return true; }
        return false;
    };

    // Parse numeric value only for numeric params (not for string/bool keys)
    static const std::vector<std::string> non_numeric_keys = {
        "mode", "gain", "cap_mode", "distance_mode", "raw", "disable"
    };
    double v = 0;
    bool need_numeric = true;
    for (auto& nk : non_numeric_keys) if (nk == key) { need_numeric = false; break; }
    if (need_numeric) {
        // BUG-11: std::stod("1.5junk") happily returns 1.5 and silently drops
        // the trailing garbage — a typo would slip through unnoticed.  Use the
        // optional `pos` out-param and require it to consume the entire string
        // (allowing only trailing whitespace).
        try {
            size_t pos = 0;
            v = std::stod(val, &pos);
            // Skip trailing whitespace
            while (pos < val.size() &&
                   std::isspace(static_cast<unsigned char>(val[pos]))) ++pos;
            if (pos != val.size()) {
                std::cerr << "Invalid numeric value: " << val
                          << " (trailing garbage)\n";
                return 1;
            }
        } catch (...) { std::cerr << "Invalid numeric value: " << val << "\n"; return 1; }
        // BUG-10: NaN/Inf passed straight through to JSON storage as "NaN"/
        // "Infinity" — non-portable per the JSON spec.  Daemon would sanitize
        // on load but the file itself is malformed.  Reject before persisting.
        if (!std::isfinite(v)) {
            std::cerr << "Invalid numeric value: " << val
                      << " (NaN/Inf not allowed)\n";
            return 1;
        }
    }

    // BUG-23: capture a pointer (or two) to the field(s) we're about to write
    // so that — after sanitize_device_profile() runs — we can compare the
    // stored value back against the raw user input and print a clamp note.
    // Previously the CLI would happily report "Set dpi = -100" while the
    // sanitize layer silently clamped it to 1, leaving the user with a value
    // that doesn't match what they asked for.
    double* dbl_target = nullptr;
    int*    int_target = nullptr;

    if      (key == "mode")             {
        if (!set_mode(a, val) || !set_mode(ay, val)) {
            std::cerr << "Invalid mode: '" << val << "'.  Valid: classic, power, "
                         "natural, jump, synchronous, lookup, noaccel\n";
            return 1;
        }
    }
    else if (key == "gain")             {
        bool b;
        if (!parse_strict_bool(val, b)) {
            std::cerr << "Invalid bool for 'gain': '" << val
                      << "'.  Valid: true/false/1/0/yes/no/on/off\n";
            return 1;
        }
        a.gain = ay.gain = b;
    }
    else if (key == "acceleration")     { a.acceleration = ay.acceleration = v;             dbl_target = &a.acceleration; }
    else if (key == "exponent_classic") { a.exponent_classic = ay.exponent_classic = v;     dbl_target = &a.exponent_classic; }
    else if (key == "exponent_power")   { a.exponent_power = ay.exponent_power = v;         dbl_target = &a.exponent_power; }
    else if (key == "limit")            { a.limit = ay.limit = v;                           dbl_target = &a.limit; }
    else if (key == "decay_rate")       { a.decay_rate = ay.decay_rate = v;                 dbl_target = &a.decay_rate; }
    else if (key == "input_offset")     { a.input_offset = ay.input_offset = v;             dbl_target = &a.input_offset; }
    else if (key == "output_offset")    { a.output_offset = ay.output_offset = v;           dbl_target = &a.output_offset; }
    else if (key == "scale")            { a.scale = ay.scale = v;                           dbl_target = &a.scale; }
    else if (key == "sync_speed")       { a.sync_speed = ay.sync_speed = v;                 dbl_target = &a.sync_speed; }
    else if (key == "smooth")           { a.smooth = ay.smooth = v;                         dbl_target = &a.smooth; }
    else if (key == "motivity")         { a.motivity = ay.motivity = v;                     dbl_target = &a.motivity; }
    else if (key == "gamma")            { a.gamma = ay.gamma = v;                           dbl_target = &a.gamma; }
    else if (key == "cap_y")            { a.cap.y = ay.cap.y = v;                           dbl_target = &a.cap.y; }
    else if (key == "cap_x")            { a.cap.x = ay.cap.x = v;                           dbl_target = &a.cap.x; }
    else if (key == "cap_mode")         {
        cap_mode m;
        if      (val == "in")  m = cap_mode::in;
        else if (val == "out") m = cap_mode::out;
        else if (val == "io" || val == "in_out" || val == "both") m = cap_mode::io;
        else {
            std::cerr << "Invalid cap_mode: '" << val
                      << "'.  Valid: in, out, io\n";
            return 1;
        }
        a.cap_mode_val = ay.cap_mode_val = m;
    }
    else if (key == "raw")              {
        bool b;
        if (!parse_strict_bool(val, b)) {
            std::cerr << "Invalid bool for 'raw': '" << val << "'\n";
            return 1;
        }
        dp->prof.raw_passthrough = b;
    }
    else if (key == "disable")          {
        bool b;
        if (!parse_strict_bool(val, b)) {
            std::cerr << "Invalid bool for 'disable': '" << val << "'\n";
            return 1;
        }
        dp->dev_cfg.disable = b;
    }
    else if (key == "rotation")         { dp->prof.degrees_rotation = v;                    dbl_target = &dp->prof.degrees_rotation; }
    else if (key == "snap")             { dp->prof.degrees_snap = v;                        dbl_target = &dp->prof.degrees_snap; }
    else if (key == "dpi")              { dp->dev_cfg.dpi = finite_double_to_int(v);        int_target = &dp->dev_cfg.dpi; }
    else if (key == "polling_rate")     { dp->dev_cfg.polling_rate = finite_double_to_int(v); int_target = &dp->dev_cfg.polling_rate; }
    else if (key == "speed_min")        { dp->prof.speed_min = v;                           dbl_target = &dp->prof.speed_min; }
    else if (key == "speed_max")        { dp->prof.speed_max = v;                           dbl_target = &dp->prof.speed_max; }
    else if (key == "output_dpi")       { dp->prof.output_dpi = v;                          dbl_target = &dp->prof.output_dpi; }
    else if (key == "lr_ratio")         { dp->prof.lr_output_dpi_ratio = v;                 dbl_target = &dp->prof.lr_output_dpi_ratio; }
    else if (key == "ud_ratio")         { dp->prof.ud_output_dpi_ratio = v;                 dbl_target = &dp->prof.ud_output_dpi_ratio; }
    else if (key == "distance_mode")    {
        if      (val == "separate" || val == "manhattan") {
            dp->prof.speed_processor_args.whole = false;
        }
        else if (val == "max" || val == "chebyshev") {
            dp->prof.speed_processor_args.whole = true;
            dp->prof.speed_processor_args.lp_norm = 9999;
        }
        else if (val == "lp") {
            dp->prof.speed_processor_args.whole = true;
            /* lp_norm set separately */
        }
        else if (val == "euclidean") {
            dp->prof.speed_processor_args.whole = true;
            dp->prof.speed_processor_args.lp_norm = 2;
        }
        else {
            std::cerr << "Invalid distance_mode: '" << val
                      << "'.  Valid: euclidean, max, lp, separate\n";
            return 1;
        }
    }
    else if (key == "lp_norm")          { dp->prof.speed_processor_args.lp_norm = v;                                dbl_target = &dp->prof.speed_processor_args.lp_norm; }
    else if (key == "input_smooth_halflife")  { dp->prof.speed_processor_args.input_speed_smooth_halflife = v;      dbl_target = &dp->prof.speed_processor_args.input_speed_smooth_halflife; }
    else if (key == "scale_smooth_halflife")  { dp->prof.speed_processor_args.scale_smooth_halflife = v;            dbl_target = &dp->prof.speed_processor_args.scale_smooth_halflife; }
    else if (key == "output_smooth_halflife") { dp->prof.speed_processor_args.output_speed_smooth_halflife = v;     dbl_target = &dp->prof.speed_processor_args.output_speed_smooth_halflife; }
    else                                { std::cerr << "Unknown key: " << key << "\n"; return 1; }

    // Sanitize after setting — clamps DPI, polling rate, rotation, etc. to safe ranges
    sanitize_device_profile(*dp);

    // BUG-23 cont.: check whether sanitize clamped what we just wrote and
    // surface that to the user.  Without this, "set-param … dpi -100" would
    // print "Set dpi = -100" while the disk file actually holds dpi=1.
    bool clamped = false;
    std::string clamped_to;
    if (int_target) {
        int requested = finite_double_to_int(v);
        if (*int_target != requested) {
            clamped = true;
            clamped_to = std::to_string(*int_target);
        }
    } else if (dbl_target) {
        if (std::fabs(*dbl_target - v) > 1e-9) {
            clamped = true;
            std::ostringstream oss;
            oss << *dbl_target;
            clamped_to = oss.str();
        }
    }

    save_config(cfg, config_path);
    if (clamped) {
        std::cout << "Set " << key << " = " << val
                  << " (clamped to " << clamped_to << ") in profile '"
                  << profile_name << "'\n";
    } else {
        std::cout << "Set " << key << " = " << val
                  << " in profile '" << profile_name << "'\n";
    }
    if (daemon_reload_via_any_path())
        std::cout << "Daemon reloaded.\n";
    return 0;
}

static int cmd_export(const app_config& cfg, const std::string& name) {
    if (name.empty()) {
        for (auto& dp : cfg.profiles)
            std::cout << profile_to_json(dp) << "\n";
        return 0;
    }
    for (auto& dp : cfg.profiles) {
        if (dp.name == name) {
            std::cout << profile_to_json(dp) << "\n";
            return 0;
        }
    }
    std::cerr << "Profile not found: " << name << "\n";
    return 1;
}

static int cmd_import(app_config& cfg, const std::string& config_path, const std::string& json_file) {
    std::ifstream f(json_file);
    if (!f.is_open()) { std::cerr << "Cannot open: " << json_file << "\n"; return 1; }
    std::string content((std::istreambuf_iterator<char>(f)), {});
    device_profile dp;
    try {
        dp = profile_from_json(content);
    } catch (std::exception& e) {
        std::cerr << "Invalid profile JSON: " << e.what() << "\n";
        return 1;
    }

    // BUG-15-fix-followup: the LUT truncate warning was previously placed
    // AFTER profile_from_json() which calls sanitize_profile() →
    // sort_lut_data() — by then a.length is already clamped to
    // LUT_POINTS_CAPACITY*2, so the warning was dead code.  Re-parse the raw
    // JSON to count the original lut_data array size and warn at import time.
    try {
        auto raw = nlohmann::json::parse(content);
        auto check_lut_raw = [&](const char* axis_key, const char* axis) {
            if (!raw.contains(axis_key)) return;
            auto& ax = raw[axis_key];
            if (!ax.contains("lut_data") || !ax["lut_data"].is_array()) return;
            size_t n = ax["lut_data"].size();
            if (n / 2 > LUT_POINTS_CAPACITY) {
                std::cerr << "Warning: LUT (" << axis << " axis) in imported "
                          << "profile has " << (n/2) << " points; truncated to "
                          << LUT_POINTS_CAPACITY << ".\n";
            }
        };
        check_lut_raw("accel_x", "X");
        check_lut_raw("accel_y", "Y");
    } catch (const std::exception& e) {
        std::cerr << "Warning: could not inspect raw LUT size: " << e.what() << "\n";
    }

    // BUG-20: previously cmd_import did NOT validate the profile name.  An
    // empty name or a duplicate of an existing profile would be silently
    // appended, leaving the user with multiple ambiguous profiles that
    // commands like delete/show/set-param target by first match.
    // BUG-24: extend the check to the same character-class rules used by
    // cmd_create — otherwise a malicious or hand-edited JSON with a name
    // containing newlines / control chars / path separators would slip in.
    if (auto reason = validate_profile_name(dp.name); !reason.empty()) {
        std::cerr << "Imported profile has invalid 'name': " << reason
                  << ".\n";
        return 1;
    }
    for (auto& existing : cfg.profiles) {
        if (existing.name == dp.name) {
            std::cerr << "Profile '" << dp.name << "' already exists. "
                         "Delete it first or rename the JSON before importing.\n";
            return 1;
        }
    }
    cfg.profiles.push_back(dp);
    try {
        save_config(cfg, config_path);
    } catch (std::exception& e) {
        std::cerr << "Failed to save config: " << e.what() << "\n";
        return 1;
    }
    std::cout << "Imported profile: " << dp.name << "\n";
    if (daemon_reload_via_any_path())
        std::cout << "Daemon reloaded.\n";
    return 0;
}

static int cmd_reload() {
    if (daemon_reload_via_any_path()) {
        std::cout << "Daemon reloaded.\n";
        return 0;
    }
    print_signal_failure(send_signal_to_daemon(SIGHUP), "reload", "HUP");
    return 1;
}

static int cmd_stop() {
    auto r = send_signal_to_daemon(SIGTERM);
    if (r == signal_result::sent) {
        std::cout << "Daemon stopped.\n";
        return 0;
    }
    print_signal_failure(r, "stop", "TERM");
    return 1;
}

static bool daemon_running() {
    std::vector<std::string> paths;
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg && xdg[0] != '\0') paths.push_back(std::string(xdg) + "/rawaccel.pid");
    paths.push_back("/run/rawaccel.pid");
    paths.push_back("/tmp/rawaccel.pid");
    for (auto& path : paths) {
        std::ifstream f(path);
        if (!f.is_open()) continue;
        pid_t pid = 0;
        f >> pid;
        if (pid_alive(pid)) return true;
    }
    return false;
}

static int cmd_status(const std::string& config_path) {
    bool running = daemon_running();
    std::cout << "Daemon:  " << (running ? "running" : "stopped") << "\n";
    try {
        auto cfg = load_config(config_path);
        std::cout << "Config:  " << config_path << "\n";
        std::cout << "Active:  " << cfg.active_profile << "\n";
        std::cout << "Profiles (" << cfg.profiles.size() << "):\n";
        for (auto& p : cfg.profiles) {
            bool is_active = (p.name == cfg.active_profile);
            std::cout << "  " << (is_active ? "* " : "  ") << p.name;
            // Show device assignment if set
            if (!p.device_id.empty())
                std::cout << "  [device: " << p.device_id << "]";
            // Show mode summary
            const char* mode_s = "noaccel";
            if (p.prof.raw_passthrough) {
                mode_s = "raw";
            } else {
                switch (p.prof.accel_x.mode) {
                case accel_mode::classic:     mode_s = "classic";     break;
                case accel_mode::power:       mode_s = "power";       break;
                case accel_mode::natural:     mode_s = "natural";     break;
                case accel_mode::jump:        mode_s = "jump";        break;
                case accel_mode::synchronous: mode_s = "synchronous"; break;
                case accel_mode::lookup:      mode_s = "lookup";      break;
                default: break;
                }
            }
            std::cout << "  (mode: " << mode_s
                      << ", DPI: " << p.dev_cfg.dpi
                      << ", poll: " << p.dev_cfg.polling_rate << "Hz";
            if (p.dev_cfg.disable) std::cout << ", disabled";
            std::cout << ")\n";
        }
        // Latency hint: tell user how to get stats
        if (running)
            std::cout << "Tip: kill -USR1 $(cat /run/rawaccel.pid)  → dump latency stats\n";
    } catch (...) {
        std::cout << "Config:  " << config_path << " (unreadable or missing)\n";
    }
    return running ? 0 : 1;
}

// ── Help ──────────────────────────────────────────────────────────────────────

static void print_help() {
    std::cout <<
R"(rawaccel-cli v)" << VERSION << R"( — Raw Accel Linux

Usage: rawaccel-cli [OPTIONS] <COMMAND> [ARGS...]

Commands:
  list                          List all profiles
  show <profile>                Show profile details
  set <profile>                 Set active profile
  create <profile>              Create new profile with defaults
  delete <profile>              Delete a profile
  set-param <profile> <key> <value>
                                Set a parameter in a profile
  export [profile]              Export profile as JSON to stdout
  import <file.json>            Import profile from JSON file
  status                        Show daemon status, profiles, and device assignments
  reload                        Reload daemon config (SIGHUP)
  stop                          Stop daemon (SIGTERM)
  latency                       Dump per-device processing latency stats (SIGUSR1)

Options:
  -c, --config PATH             Config file path
  -h, --help                    Show this help
  -V, --version                 Show version

Parameters (for set-param):
  raw               true|false|1|0  (raw passthrough — bypass all processing)
  disable           true|false|1|0  (leave matched device ungrabbed)
  mode              classic|power|natural|jump|synchronous|lookup|noaccel
  gain              true|false|1|0  (gain mode on/off)
  acceleration      Acceleration multiplier (e.g. 0.005)
  exponent_classic  Classic exponent (e.g. 2.0)
  exponent_power    Power/synchronous exponent (e.g. 0.05)
  limit             Upper multiplier asymptote, jump/natural (e.g. 1.5)
  decay_rate        Natural decay rate (e.g. 0.1)
  motivity          Compatibility field (stored; currently not used)
  gamma             Compatibility field (stored; currently not used)
  input_offset      Speed offset before acceleration starts
  output_offset     Output offset (power mode)
  scale             Scale factor (power mode)
  sync_speed        Synchronous sync speed (e.g. 5.0)
  smooth            Jump smoothness (e.g. 0.5)
  cap_x             Input speed cap
  cap_y             Output gain cap
  cap_mode          out|in|io  (cap mode)
  rotation          Rotation in degrees
  snap              Snap angle in degrees
  dpi               Mouse DPI
  polling_rate      Mouse polling rate (Hz)
  speed_min         Minimum speed clamp (ips)
  speed_max         Maximum speed clamp (ips)
  output_dpi        Output DPI normalization value (default: 1000)
  lr_ratio          Left/right output DPI ratio
  ud_ratio          Up/down output DPI ratio
  distance_mode     euclidean|max|lp|separate  (speed calculation method)
  lp_norm           Lp-norm value (when distance_mode=lp, e.g. 3.0)
  input_smooth_halflife   Input speed EMA halflife (ms, 0=off)
  scale_smooth_halflife   Scale EMA halflife (ms, 0=off)
  output_smooth_halflife  Output speed EMA halflife (ms, 0=off)

Examples:
  rawaccel-cli list
  rawaccel-cli create gaming
  rawaccel-cli set-param gaming mode classic
  rawaccel-cli set-param gaming acceleration 0.005
  rawaccel-cli set-param gaming exponent_classic 2
  rawaccel-cli set-param gaming limit 1.8
  rawaccel-cli set gaming
  rawaccel-cli export gaming > backup.json
)";
}

// ── main ──────────────────────────────────────────────────────────────────────

static int run_cli(int argc, char* argv[]) {
    std::string config_path;
    std::vector<std::string> args;

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) && i + 1 < argc) {
            config_path = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_help();
            return 0;
        } else if (strcmp(argv[i], "-V") == 0 || strcmp(argv[i], "--version") == 0) {
            std::cout << "rawaccel-cli " << VERSION << "\n";
            return 0;
        } else {
            args.push_back(argv[i]);
        }
    }

    if (args.empty()) { print_help(); return 0; }

    if (config_path.empty()) {
        std::string daemon_cfg = daemon_config_path_from_ipc();
        config_path = daemon_cfg.empty() ? find_config_path() : daemon_cfg;
    }

    // Commands that don't need config loaded
    if (args[0] == "reload") return cmd_reload();
    if (args[0] == "stop")   return cmd_stop();
    if (args[0] == "status") return cmd_status(config_path);
    if (args[0] == "latency") {
        // Try the IPC "latency" command first — works for any user in the
        // input group regardless of who owns the daemon.  Falls back to
        // SIGUSR1 if the running daemon doesn't speak that command yet.
        std::string ipc_resp = daemon_ipc_query("latency");
        if (ipc_resp.find("\"ok\":true") != std::string::npos) {
            std::cout << "Latency dump scheduled. View it with: journalctl -u rawaccel -n 30\n";
            return 0;
        }
        auto r = send_signal_to_daemon(SIGUSR1);
        if (r == signal_result::sent) {
            std::cout << "SIGUSR1 sent. View stats with: journalctl -u rawaccel -n 30\n";
            return 0;
        }
        print_signal_failure(r, "request latency stats from", "USR1");
        return 1;
    }

    // Load config (create default if missing)
    app_config cfg;
    try {
        cfg = load_config(config_path);
    } catch (const std::exception& e) {
        // Only auto-create when the file is genuinely missing.  A malformed or
        // unreadable existing config must be preserved, not overwritten by `list`.
        if (!path_is_missing(config_path)) {
            std::cerr << "Config load failed: " << e.what() << "\n"
                      << "Refusing to overwrite existing config: " << config_path << "\n";
            return 1;
        }
        cfg = make_default_config();
        try { save_config(cfg, config_path); }
        catch (const std::exception& e) {
            std::cerr << "Warning: could not save default config: " << e.what() << "\n";
        } catch (...) {
            std::cerr << "Warning: could not save default config.\n";
        }
    } catch (...) {
        if (!path_is_missing(config_path)) {
            std::cerr << "Config load failed; refusing to overwrite existing config: "
                      << config_path << "\n";
            return 1;
        }
        cfg = make_default_config();
        try { save_config(cfg, config_path); }
        catch (...) { std::cerr << "Warning: could not save default config.\n"; }
    }

    const std::string& cmd = args[0];

    if (cmd == "list")   return cmd_list(cfg);
    if (cmd == "show"  && args.size() >= 2) return cmd_show(cfg, args[1]);
    if (cmd == "set"   && args.size() >= 2) return cmd_set(cfg, config_path, args[1]);
    if (cmd == "create" && args.size() >= 2) return cmd_create(cfg, config_path, args[1]);
    if (cmd == "delete" && args.size() >= 2) return cmd_delete(cfg, config_path, args[1]);
    if (cmd == "set-param" && args.size() >= 4) return cmd_set_param(cfg, config_path, args[1], args[2], args[3]);
    if (cmd == "export") return cmd_export(cfg, args.size() >= 2 ? args[1] : "");
    if (cmd == "import" && args.size() >= 2) return cmd_import(cfg, config_path, args[1]);

    std::cerr << "Unknown command: " << cmd << "\n";
    print_help();
    return 1;
}

int main(int argc, char* argv[]) {
    try {
        return run_cli(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "Unexpected error: " << e.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "Unexpected error.\n";
        return 1;
    }
}
