#pragma once
// ── Common includes for all GUI translation units ─────────────────────────────
#include <gtk/gtk.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <cairo.h>
#include <cmath>
#include <limits>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <string>
#include <unistd.h>
#include <dirent.h>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <vector>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <pwd.h>

#include "../include/rawaccel.hpp"
#include "../include/config.hpp"

using namespace rawaccel;
namespace fs = std::filesystem;

// ── Constants (dark theme colours) ───────────────────────────────────────────
static const double C_BG[3]    = {0.11, 0.11, 0.13};
static const double C_GRID[4]  = {0.25, 0.25, 0.28, 0.5};
static const double C_CURVE[3] = {0.18, 0.72, 1.00};
static const double C_CURVE2[3]= {1.00, 0.55, 0.18};
static const double C_REF[4]   = {0.45, 0.45, 0.48, 0.6};
static const double C_DOT[3]   = {1.00, 0.30, 0.30};
static const double C_TEXT[3]  = {0.80, 0.80, 0.85};

// Graph margins (shared between graph.cpp and ui_builder.cpp)
static constexpr double GRAPH_ML = 58, GRAPH_MR = 16, GRAPH_MT = 16, GRAPH_MB = 44;

// ── Mouse device info ─────────────────────────────────────────────────────────
struct InputDeviceInfo {
    std::string name;
    std::string event_node;
    std::string uniq;
    std::string stable_id;   // "usb:VVVV:PPPP:serial" if vendor+product available, else event_node
    uint16_t    vendor  = 0;
    uint16_t    product = 0;
    bool        has_rel_xy  = false;
    bool        is_rawaccel = false;

    bool operator==(const InputDeviceInfo& o) const {
        return event_node == o.event_node && name == o.name;
    }
};

// ── Application state ─────────────────────────────────────────────────────────
struct AppState {
    app_config  config;
    std::string config_path;
    int         current_profile_idx = 0;
    bool        xy_linked  = true;
    bool        updating   = false;
    bool        unsaved    = false;

    // Daemon state
    guint       daemon_poll_id = 0;

    // inotify — /dev/input hot-plug monitoring
    int         inotify_fd  = -1;
    int         inotify_wd  = -1;
    guint       inotify_src = 0;

    // Graph interaction
    double graph_zoom     = 1.0;
    double graph_pan_x    = 0.0;
    bool   graph_drag     = false;
    double drag_pan_start = 0.0;

    // KDE / libinput double-acceleration warning
    bool   is_kde          = false;   // true if running under KDE Plasma
    bool   is_wayland      = false;   // true if Wayland session
    bool   kde_accel_ok    = true;    // false = libinput accel NOT disabled → double-accel!
    GtkWidget* kde_warn_bar = nullptr; // infobar shown when kde_accel_ok == false

    // Auto-detected device properties (from daemon status_json)
    int    detected_dpi          = 0;  // 0 = unknown
    int    detected_polling_rate = 0;  // 0 = unknown
    // Auto-fill hint labels and buttons (Device section)
    GtkWidget* dpi_detected_lbl      = nullptr;
    GtkWidget* polling_detected_lbl  = nullptr;
    GtkWidget* dpi_autofill_btn      = nullptr;
    GtkWidget* polling_autofill_btn  = nullptr;

    // ── Widgets ──────────────────────────────────────────────────────────────
    GtkWidget* window            = nullptr;
    GtkWidget* profile_combo     = nullptr;
    GtkWidget* status_bar        = nullptr;
    GtkWidget* daemon_status     = nullptr;
    GtkWidget* graph_area        = nullptr;
    GtkWidget* apply_btn         = nullptr;
    GtkWidget* daemon_start_btn  = nullptr;
    GtkWidget* daemon_stop_btn   = nullptr;
    GtkWidget* daemon_reload_btn = nullptr;

    // Accel X
    GtkWidget* mode_combo         = nullptr;
    GtkWidget* gain_check         = nullptr;
    GtkWidget* accel_spin         = nullptr;
    GtkWidget* exponent_spin      = nullptr;
    GtkWidget* power_exp_spin     = nullptr;
    GtkWidget* limit_spin         = nullptr;
    GtkWidget* offset_spin        = nullptr;
    GtkWidget* decay_spin         = nullptr;
    GtkWidget* cap_x_spin         = nullptr;
    GtkWidget* cap_y_spin         = nullptr;
    GtkWidget* cap_mode_combo     = nullptr;
    GtkWidget* sync_speed_spin    = nullptr;
    GtkWidget* smooth_spin        = nullptr;
    GtkWidget* motivity_spin      = nullptr;
    GtkWidget* gamma_spin         = nullptr;
    GtkWidget* output_offset_spin = nullptr;
    GtkWidget* scale_spin         = nullptr;

    // Accel Y
    GtkWidget* xy_link_btn     = nullptr;
    GtkWidget* mode_combo_y    = nullptr;
    GtkWidget* accel_spin_y    = nullptr;
    GtkWidget* exponent_spin_y = nullptr;
    GtkWidget* limit_spin_y    = nullptr;
    GtkWidget* offset_spin_y   = nullptr;
    GtkWidget* cap_y_spin_y    = nullptr;
    GtkWidget* y_axis_frame    = nullptr;

    // Device
    GtkWidget* dpi_spin        = nullptr;
    GtkWidget* polling_spin    = nullptr;
    GtkWidget* output_dpi_spin = nullptr;
    GtkWidget* lr_ratio_spin   = nullptr;
    GtkWidget* ud_ratio_spin   = nullptr;

    // Speed processor
    GtkWidget* dist_mode_combo = nullptr;
    GtkWidget* lp_norm_spin    = nullptr;
    GtkWidget* lp_norm_label   = nullptr;
    GtkWidget* input_hl_spin   = nullptr;
    GtkWidget* scale_hl_spin   = nullptr;
    GtkWidget* output_hl_spin  = nullptr;

    // Raw passthrough
    GtkWidget* raw_check      = nullptr;

    // Motion
    GtkWidget* rotation_spin  = nullptr;
    GtkWidget* snap_spin      = nullptr;
    GtkWidget* speed_min_spin = nullptr;
    GtkWidget* speed_max_spin = nullptr;

    // LUT editor
    GtkWidget* lut_frame          = nullptr;
    GtkWidget* accel_params_frame = nullptr;
    GtkWidget* lut_list_box       = nullptr;
    bool       lut_graph_mode     = false;

    // Device assignment
    GtkWidget* device_id_combo = nullptr;
    std::vector<InputDeviceInfo> mice_list;
};

// ── Global app state pointer (defined in main.cpp) ───────────────────────────
// NOTE: G is intentionally only accessible to main.cpp. All other code receives
// AppState* through function parameters or GTK callback gpointer user_data.
// Do NOT add 'extern AppState* G' here — use the AppState* parameter instead.

// ── Shared function declarations ─────────────────────────────────────────────
// All functions take AppState* explicitly; no implicit global access.

// helpers
device_profile& cur_prof(AppState* S);
void set_status(AppState* S, const std::string& msg);
void save_config_now(AppState* S);
/// Returns a warning string if two or more profiles share the same non-empty
/// device_id (first-match-wins in the daemon, so this is likely unintentional).
/// Returns empty string if no collision detected.
std::string check_duplicate_device_ids(const app_config& cfg);

// daemon
pid_t read_daemon_pid();
bool  daemon_running();
bool  daemon_send_signal(int sig, std::string* err_out = nullptr);
void  update_daemon_status(AppState* S);

// profile
void rebuild_profile_combo(AppState* S);
void show_input_dialog(AppState* S,
                       const char* title, const char* placeholder,
                       const char* initial,
                       std::function<void(const std::string&)> cb);

// widgets <-> profile sync
void widgets_to_profile(AppState* S);
void profile_to_widgets(AppState* S);

// device combo
void refresh_mice_combo(AppState* S, bool is_auto = false);

// graph / LUT
void rebuild_lut_list(AppState* S);
void update_lut_visibility(AppState* S);

// UI entry point
void build_ui(AppState* S, GtkApplication* gapp);

// GTK callbacks — forward declarations (implementations in .inl files)
void on_activate(GtkApplication* gapp, gpointer user_data); // user_data = AppState*
