//  rawaccel-gui — GTK4 frontend for rawaccel-linux
//  Version: RAWACCEL_VERSION (rawaccel-base.hpp)
//
//  File layout (each .inl compiled as part of this single translation unit):
//    app_state.hpp    — AppState struct, shared includes, forward declarations
//    devices.inl      — Mouse device discovery + stable by-id paths, inotify hot-plug
//    daemon_comm.inl  — Daemon PID lookup, status display, signal sending
//    graph.inl        — Cairo curve rendering, LUT editor
//    widgets_sync.inl — Widget ↔ profile sync, GTK signal callbacks
//    profile_mgr.inl  — Profile CRUD dialogs (new/rename/delete/duplicate/reset/save)
//    ui_builder.inl   — Layout helpers, build_ui(), window-close dialog, on_activate()
//    main.cpp         — AppState instantiation, config load, main()
//
//  Architecture note: AppState is NOT accessed via a global pointer. Instead:
//    - All helper functions receive AppState* as a parameter.
//    - GTK callbacks receive AppState* through the gpointer user_data mechanism.
//    - on_activate() is the only entry point that receives the live AppState*.

#include "app_state.hpp"

// ── Helpers ───────────────────────────────────────────────────────────────────

// Returns reference to current profile. Caller must ensure profiles is non-empty.
device_profile& cur_prof(AppState* S) {
    auto& profs = S->config.profiles;
    if (profs.empty()) {
        device_profile dp; dp.name = "default";
        profs.push_back(dp);
        S->current_profile_idx = 0;
    }
    int idx = std::clamp(S->current_profile_idx, 0, (int)profs.size() - 1);
    S->current_profile_idx = idx;
    return profs[idx];
}

void set_status(AppState* S, const std::string& msg) {
    gtk_label_set_text(GTK_LABEL(S->status_bar), msg.c_str());
}

void save_config_now(AppState* S) {
    try {
        save_config(S->config, S->config_path);
        S->unsaved = false;
        // Auto-notify daemon so the running instance picks up the new config immediately.
        // This mirrors the "Apply" button path but is silent (no error if daemon is stopped).
        std::string sig_err;
        bool notified = daemon_send_signal(SIGHUP, &sig_err);
        // Check for duplicate device IDs and prepend warning to status
        std::string dup_warn = check_duplicate_device_ids(S->config);
        std::string status_msg;
        if (notified)
            status_msg = "Saved & reloaded: " + S->config_path;
        else
            status_msg = "Saved: " + S->config_path;
        if (!dup_warn.empty())
            status_msg = dup_warn + " | " + status_msg;
        set_status(S, status_msg);
    } catch (std::exception& e) {
        set_status(S, std::string("Save error: ") + e.what());
    }
}

std::string check_duplicate_device_ids(const app_config& cfg) {
    // O(n^2) but profiles are few; avoids extra includes.
    std::string warn;
    for (size_t i = 0; i < cfg.profiles.size(); ++i) {
        const auto& a = cfg.profiles[i];
        if (a.device_id.empty()) continue;
        for (size_t j = i + 1; j < cfg.profiles.size(); ++j) {
            const auto& b = cfg.profiles[j];
            if (a.device_id == b.device_id) {
                if (!warn.empty()) warn += "; ";
                warn += "\"" + a.name + "\" & \"" + b.name +
                        "\" share device " + a.device_id;
            }
        }
    }
    if (!warn.empty())
        warn = "Warning: duplicate device IDs — " + warn +
               ". Only the first matching profile is used by the daemon.";
    return warn;
}

// ── Translation units (included in dependency order) ─────────────────────────
#include "devices.inl"
#include "daemon_comm.inl"
#include "graph.inl"
#include "widgets_sync.inl"
#include "profile_mgr.inl"
#include "ui_builder.inl"

// ── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    // Suppress harmless GTK4 GtkGizmo size warnings (known GTK4 cosmetic bug
    // where sliders briefly report negative min-size during layout).
    // GTK4 uses structured logging, so g_log_set_handler() is bypassed —
    // we must install a writer func instead.
    g_log_set_writer_func(
        [](GLogLevelFlags level, const GLogField* fields, gsize n_fields, gpointer) -> GLogWriterOutput {
            for (gsize i = 0; i < n_fields; ++i) {
                if (g_strcmp0(fields[i].key, "MESSAGE") == 0 && fields[i].value) {
                    const char* msg = static_cast<const char*>(fields[i].value);
                    if (std::strstr(msg, "GtkGizmo")) return G_LOG_WRITER_HANDLED;
                }
            }
            return g_log_writer_default(level, fields, n_fields, nullptr);
        }, nullptr, nullptr);

    AppState state;

    // Find config — prefer real user's config even if launched with sudo.
    // Use getpwnam_r (reentrant, thread-safe) instead of getpwnam.
    const char* sudo_user = getenv("SUDO_USER");
    if (sudo_user && sudo_user[0] != '\0') {
        struct passwd  pwd_buf;
        struct passwd* result = nullptr;
        std::vector<char> pw_buf(16384);
        int ret = getpwnam_r(sudo_user, &pwd_buf, pw_buf.data(), pw_buf.size(), &result);
        if (ret == 0 && result && result->pw_dir && result->pw_dir[0] != '\0') {
            state.config_path = std::string(result->pw_dir) + "/.config/rawaccel/settings.json";
        } else {
            state.config_path = std::string("/home/") + sudo_user + "/.config/rawaccel/settings.json";
        }
    } else {
        state.config_path = find_config_path();
    }

    try {
        state.config = load_config(state.config_path);
    } catch (...) {
        device_profile dp;
        dp.name = "default";
        dp.dev_cfg.dpi = 800;
        dp.dev_cfg.polling_rate = 1000;
        state.config.profiles.push_back(dp);
    }
    if (state.config.profiles.empty()) {
        device_profile dp; dp.name = "default";
        state.config.profiles.push_back(dp);
    }

    GtkApplication* app = gtk_application_new(
        "io.github.rawaccel", G_APPLICATION_DEFAULT_FLAGS);
    // Pass &state as user_data — on_activate receives it via gpointer.
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), &state);
    int ret = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return ret;
}
