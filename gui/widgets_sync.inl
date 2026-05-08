// ── Widget ↔ profile synchronisation and GTK signal callbacks ────────────────

// R13-refactor: extracted from widgets_to_profile and profile_to_widgets
// to eliminate 15-line duplication.  Greys out pipeline widgets when raw
// passthrough is active.
static void update_raw_sensitivity(AppState* S, bool raw) {
    if (S->accel_params_frame) gtk_widget_set_sensitive(S->accel_params_frame, !raw);
    if (S->lut_frame)          gtk_widget_set_sensitive(S->lut_frame, !raw);
    if (S->mode_combo)         gtk_widget_set_sensitive(S->mode_combo, !raw);
    if (S->gain_check)         gtk_widget_set_sensitive(S->gain_check, !raw);
    if (S->xy_link_btn)        gtk_widget_set_sensitive(S->xy_link_btn, !raw);
    if (S->y_axis_frame)       gtk_widget_set_sensitive(S->y_axis_frame, !raw && !S->xy_linked);
    if (S->rotation_spin)      gtk_widget_set_sensitive(S->rotation_spin, !raw);
    if (S->snap_spin)          gtk_widget_set_sensitive(S->snap_spin, !raw);
    if (S->speed_min_spin)     gtk_widget_set_sensitive(S->speed_min_spin, !raw);
    if (S->speed_max_spin)     gtk_widget_set_sensitive(S->speed_max_spin, !raw);
    if (S->lr_ratio_spin)      gtk_widget_set_sensitive(S->lr_ratio_spin, !raw);
    if (S->ud_ratio_spin)      gtk_widget_set_sensitive(S->ud_ratio_spin, !raw);
    if (S->dist_mode_combo)    gtk_widget_set_sensitive(S->dist_mode_combo, !raw);
    if (S->lp_norm_spin)       gtk_widget_set_sensitive(S->lp_norm_spin, !raw);
    if (S->input_hl_spin)      gtk_widget_set_sensitive(S->input_hl_spin, !raw);
    if (S->scale_hl_spin)      gtk_widget_set_sensitive(S->scale_hl_spin, !raw);
    if (S->output_hl_spin)     gtk_widget_set_sensitive(S->output_hl_spin, !raw);
}

accel_mode idx_to_mode(int i) {
    static const accel_mode M[] = {
        accel_mode::noaccel, accel_mode::classic, accel_mode::power,
        accel_mode::natural, accel_mode::jump,    accel_mode::synchronous,
        accel_mode::lookup,
    };
    return (i >= 0 && i < 7) ? M[i] : accel_mode::noaccel;
}

int mode_to_idx(accel_mode m) {
    switch (m) {
    case accel_mode::classic:     return 1;
    case accel_mode::power:       return 2;
    case accel_mode::natural:     return 3;
    case accel_mode::jump:        return 4;
    case accel_mode::synchronous: return 5;
    case accel_mode::lookup:      return 6;
    default:                      return 0;
    }
}

cap_mode idx_to_cap(int i) {
    if (i == 1) return cap_mode::in;
    if (i == 2) return cap_mode::io;
    return cap_mode::out;
}
int cap_to_idx(cap_mode c) {
    if (c == cap_mode::in)  return 1;
    if (c == cap_mode::io)  return 2;
    return 0;
}

void widgets_to_profile(AppState* S) {
    if (S->updating || S->config.profiles.empty()) return;
    S->unsaved = true;
    auto& dp = cur_prof(S);
    auto& ax = dp.prof.accel_x;
    auto& ay = dp.prof.accel_y;

#define SPIN(w)  gtk_spin_button_get_value(GTK_SPIN_BUTTON(S->w))
#define CHECK(w) gtk_check_button_get_active(GTK_CHECK_BUTTON(S->w))
#define DD(w)    (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(S->w))

    ax.mode           = idx_to_mode(DD(mode_combo));
    ax.gain           = CHECK(gain_check);
    ax.acceleration   = SPIN(accel_spin);
    ax.exponent_classic = SPIN(exponent_spin);
    ax.exponent_power = SPIN(power_exp_spin);
    ax.limit          = SPIN(limit_spin);
    ax.input_offset   = SPIN(offset_spin);
    ax.decay_rate     = SPIN(decay_spin);
    ax.cap.x          = SPIN(cap_x_spin);
    ax.cap.y          = SPIN(cap_y_spin);
    ax.cap_mode_val   = idx_to_cap(DD(cap_mode_combo));
    ax.sync_speed     = SPIN(sync_speed_spin);
    ax.smooth         = SPIN(smooth_spin);
    ax.motivity       = SPIN(motivity_spin);
    ax.gamma          = SPIN(gamma_spin);
    ax.output_offset  = SPIN(output_offset_spin);
    ax.scale          = SPIN(scale_spin);

    if (S->xy_linked) {
        ay = ax;
    } else {
        // R13: copy fields that have no dedicated Y-axis widget from X,
        // so they stay in sync rather than going stale when Y is unlinked.
        ay.gain             = ax.gain;
        ay.cap_mode_val     = ax.cap_mode_val;
        ay.cap.x            = ax.cap.x;
        ay.exponent_power   = ax.exponent_power;
        ay.decay_rate       = ax.decay_rate;
        ay.scale            = ax.scale;
        ay.output_offset    = ax.output_offset;
        ay.motivity         = ax.motivity;
        ay.gamma            = ax.gamma;
        ay.smooth           = ax.smooth;
        ay.sync_speed       = ax.sync_speed;
        // Y-axis-specific widgets:
        ay.mode             = idx_to_mode(DD(mode_combo_y));
        ay.acceleration     = SPIN(accel_spin_y);
        ay.exponent_classic = SPIN(exponent_spin_y);
        ay.limit            = SPIN(limit_spin_y);
        ay.input_offset     = SPIN(offset_spin_y);
        ay.cap.y            = SPIN(cap_y_spin_y);
    }

    dp.prof.raw_passthrough     = CHECK(raw_check);
    dp.prof.degrees_rotation    = SPIN(rotation_spin);
    dp.prof.degrees_snap        = SPIN(snap_spin);
    dp.prof.speed_min           = SPIN(speed_min_spin);
    dp.prof.speed_max           = SPIN(speed_max_spin);
    dp.prof.output_dpi          = SPIN(output_dpi_spin);
    dp.prof.lr_output_dpi_ratio = SPIN(lr_ratio_spin);
    dp.prof.ud_output_dpi_ratio = SPIN(ud_ratio_spin);
    dp.dev_cfg.dpi              = (int)SPIN(dpi_spin);
    dp.dev_cfg.polling_rate     = (int)SPIN(polling_spin);

    // Speed processor args
    {
        auto& sp = dp.prof.speed_processor_args;
        int dist_sel = DD(dist_mode_combo);
        // 0=Euclidean, 1=Max, 2=Lp, 3=Separate
        if      (dist_sel == 3) { sp.whole = false; }
        else if (dist_sel == 1) { sp.whole = true;  sp.lp_norm = 9999; }
        else if (dist_sel == 2) { sp.whole = true;  sp.lp_norm = SPIN(lp_norm_spin); }
        else                    { sp.whole = true;  sp.lp_norm = 2; } // euclidean
        sp.input_speed_smooth_halflife  = SPIN(input_hl_spin);
        sp.scale_smooth_halflife        = SPIN(scale_hl_spin);
        sp.output_speed_smooth_halflife = SPIN(output_hl_spin);
        // Show/hide lp_norm row
        bool show_lp = (dist_sel == 2);
        if (S->lp_norm_label) gtk_widget_set_visible(S->lp_norm_label, show_lp);
        if (S->lp_norm_spin)  gtk_widget_set_visible(S->lp_norm_spin,  show_lp);
    }

    // device_id: index 0 = "All devices" (empty string), 1+ = mice_list[idx-1].stable_id
    // Prefer stable_id ("usb:VVVV:PPPP:serial") over event_node for reboot stability.
    if (S->device_id_combo) {
        int sel = (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(S->device_id_combo));
        if (sel <= 0 || sel - 1 >= (int)S->mice_list.size()) {
            dp.device_id = "";
        } else {
            auto& m = S->mice_list[sel - 1];
            dp.device_id = m.stable_id.empty() ? m.event_node : m.stable_id;
        }
    }

#undef SPIN
#undef CHECK
#undef DD

    update_raw_sensitivity(S, dp.prof.raw_passthrough);

    gtk_widget_queue_draw(S->graph_area);
}

void profile_to_widgets(AppState* S) {
    if (S->config.profiles.empty()) return;
    S->updating = true;
    auto& dp = cur_prof(S);
    auto& ax = dp.prof.accel_x;
    auto& ay = dp.prof.accel_y;

#define SET_SPIN(w, v)  gtk_spin_button_set_value(GTK_SPIN_BUTTON(S->w), v)
#define SET_DD(w, v)    gtk_drop_down_set_selected(GTK_DROP_DOWN(S->w), v)
#define SET_CHK(w, v)   gtk_check_button_set_active(GTK_CHECK_BUTTON(S->w), v)

    SET_DD(mode_combo,      mode_to_idx(ax.mode));
    SET_CHK(gain_check,     ax.gain);
    SET_SPIN(accel_spin,    ax.acceleration);
    SET_SPIN(exponent_spin, ax.exponent_classic);
    SET_SPIN(power_exp_spin,ax.exponent_power);
    SET_SPIN(limit_spin,    ax.limit);
    SET_SPIN(offset_spin,   ax.input_offset);
    SET_SPIN(decay_spin,    ax.decay_rate);
    SET_SPIN(cap_x_spin,    ax.cap.x);
    SET_SPIN(cap_y_spin,    ax.cap.y);
    SET_DD(cap_mode_combo,  cap_to_idx(ax.cap_mode_val));
    SET_SPIN(sync_speed_spin, ax.sync_speed);
    SET_SPIN(smooth_spin,   ax.smooth);
    SET_SPIN(motivity_spin,      ax.motivity);
    SET_SPIN(gamma_spin,         ax.gamma);
    SET_SPIN(output_offset_spin, ax.output_offset);
    SET_SPIN(scale_spin,         ax.scale);

    // Y axis — update both the checkbox and the frame together
    bool same = (ax == ay);
    S->xy_linked = same;
    gtk_check_button_set_active(GTK_CHECK_BUTTON(S->xy_link_btn), same);
    gtk_widget_set_sensitive(S->y_axis_frame, !same);

    SET_DD(mode_combo_y,      mode_to_idx(ay.mode));
    SET_SPIN(accel_spin_y,    ay.acceleration);
    SET_SPIN(exponent_spin_y, ay.exponent_classic);
    SET_SPIN(limit_spin_y,    ay.limit);
    SET_SPIN(offset_spin_y,   ay.input_offset);
    SET_SPIN(cap_y_spin_y,    ay.cap.y);

    SET_CHK(raw_check,        dp.prof.raw_passthrough);
    SET_SPIN(rotation_spin,   dp.prof.degrees_rotation);
    SET_SPIN(snap_spin,       dp.prof.degrees_snap);
    SET_SPIN(speed_min_spin,  dp.prof.speed_min);
    SET_SPIN(speed_max_spin,  dp.prof.speed_max);
    SET_SPIN(output_dpi_spin, dp.prof.output_dpi);
    SET_SPIN(lr_ratio_spin,   dp.prof.lr_output_dpi_ratio);
    SET_SPIN(ud_ratio_spin,   dp.prof.ud_output_dpi_ratio);
    SET_SPIN(dpi_spin,        dp.dev_cfg.dpi);
    SET_SPIN(polling_spin,    dp.dev_cfg.polling_rate);

    // Speed processor
    {
        auto& sp = dp.prof.speed_processor_args;
        int dist_idx;
        if (!sp.whole)                              dist_idx = 3; // separate
        else if (sp.lp_norm >= 16 || sp.lp_norm <= 0) dist_idx = 1; // max
        else if (std::fabs(sp.lp_norm - 2.0) > 1e-9)  dist_idx = 2; // lp
        else                                        dist_idx = 0; // euclidean
        SET_DD(dist_mode_combo, (guint)dist_idx);
        SET_SPIN(lp_norm_spin,  sp.lp_norm > 0 ? sp.lp_norm : 2.0);
        SET_SPIN(input_hl_spin,  sp.input_speed_smooth_halflife);
        SET_SPIN(scale_hl_spin,  sp.scale_smooth_halflife);
        SET_SPIN(output_hl_spin, sp.output_speed_smooth_halflife);
        bool show_lp2 = (dist_idx == 2);
        if (S->lp_norm_label) gtk_widget_set_visible(S->lp_norm_label, show_lp2);
        if (S->lp_norm_spin)  gtk_widget_set_visible(S->lp_norm_spin,  show_lp2);
    }

    // device_id dropdown: empty = index 0, match on stable_id or event_node
    if (S->device_id_combo) {
        int sel = 0; // "All devices"
        for (int i = 0; i < (int)S->mice_list.size(); i++) {
            auto& m = S->mice_list[i];
            if ((!m.stable_id.empty() && m.stable_id == dp.device_id) ||
                m.event_node == dp.device_id) {
                sel = i + 1; break;
            }
        }
        gtk_drop_down_set_selected(GTK_DROP_DOWN(S->device_id_combo), (guint)sel);
    }

#undef SET_SPIN
#undef SET_DD
#undef SET_CHK

    S->updating = false;
    update_lut_visibility(S); // show the correct panel when a profile is loaded

    update_raw_sensitivity(S, dp.prof.raw_passthrough);

    gtk_widget_queue_draw(S->graph_area);

    // Warn if the loaded LUT had more points than the buffer capacity.
    // This can happen when importing a config created with a future version that
    // raised LUT_POINTS_CAPACITY, or when a hand-edited JSON has excess data.
    {
        auto& ax = cur_prof(S).prof.accel_x;
        auto& ay = cur_prof(S).prof.accel_y;
        bool over_x = ax.mode == accel_mode::lookup &&
                      ax.length / 2 > (int)LUT_POINTS_CAPACITY;
        bool over_y = !S->xy_linked && ay.mode == accel_mode::lookup &&
                      ay.length / 2 > (int)LUT_POINTS_CAPACITY;
        if (over_x || over_y) {
            std::string axis = (over_x && over_y) ? "X and Y" : (over_x ? "X" : "Y");
            set_status(S, "Warning: LUT (" + axis + " axis) was truncated to " +
                          std::to_string(LUT_POINTS_CAPACITY) + " points (max capacity).");
        }
    }
}

// ── Callbacks ─────────────────────────────────────────────────────────────────

static bool has_systemd_rawaccel_unit() {
    return fs::exists("/etc/systemd/system/rawaccel.service") ||
           fs::exists("/usr/lib/systemd/system/rawaccel.service");
}

static bool systemctl_async(const char* action, std::string* err_out = nullptr) {
    pid_t pid = fork();
    if (pid < 0) {
        if (err_out) *err_out = "fork() failed.";
        return false;
    }
    if (pid == 0) {
        setsid();
        execlp("systemctl", "systemctl", action, "rawaccel", (char*)nullptr);
        _exit(127);
    }
    // Reap the child asynchronously so it doesn't become a zombie.
    pid_t* pid_ptr = new pid_t(pid);
    g_timeout_add(3000, [](gpointer p) -> gboolean {
        pid_t* pp = static_cast<pid_t*>(p);
        int status;
        waitpid(*pp, &status, WNOHANG);
        delete pp;
        return G_SOURCE_REMOVE;
    }, pid_ptr);
    if (err_out) err_out->clear();
    // We can't easily wait on systemctl without freezing the GUI, so we
    // optimistically return true:
    // the timeout above reaps the child, and the caller's status-poll picks up
    // the actual daemon state a second later.
    return true;
}

void on_param_changed(GtkWidget*, gpointer user_data) {
    auto* S = static_cast<AppState*>(user_data);
    widgets_to_profile(S);
    update_lut_visibility(S); // show/hide the LUT panel when the mode changes
}

// "notify::<prop>" signals use a 3-argument callback signature
// (GObject*, GParamSpec*, gpointer). Calling the 2-arg on_param_changed
// directly via G_CALLBACK corrupts user_data (it receives the GParamSpec*
// instead of AppState*), causing SIGSEGV on dropdown changes (Mode/Mouse).
void on_notify_param_changed(GObject*, GParamSpec*, gpointer user_data) {
    on_param_changed(nullptr, user_data);
}

void on_profile_changed(GtkDropDown* dd, GParamSpec*, gpointer user_data) {
    auto* S = static_cast<AppState*>(user_data);
    if (S->updating) return; // blocked during rebuild_profile_combo
    int idx = (int)gtk_drop_down_get_selected(dd);
    if (idx < 0 || idx >= (int)S->config.profiles.size()) return;

    // Warn in the status bar if switching away from a profile with unsaved changes.
    // A modal dialog here would be too disruptive for a dropdown switch.
    if (S->unsaved) {
        set_status(S, std::string(ui_text(S, "Warning: unsaved changes to \"", "Uyarı: \"")) +
                   cur_prof(S).name +
                   ui_text(S, "\" are kept in memory; Save/Apply before quitting.",
                           "\" profilindeki kaydedilmemiş değişiklikler bellekte tutuluyor; çıkmadan önce Kaydet/Uygula."));
    }

    S->current_profile_idx = idx;
    S->config.active_profile = cur_prof(S).name;
    // Reset zoom/pan on profile change — different profiles may have different speed ranges
    S->graph_zoom  = 1.0;
    S->graph_pan_x = 0.0;
    profile_to_widgets(S);
}

void on_xy_link_toggled(GtkCheckButton* btn, gpointer user_data) {
    auto* S = static_cast<AppState*>(user_data);
    if (S->updating) return; // block spurious triggers from profile_to_widgets programmatic sets
    S->xy_linked = gtk_check_button_get_active(btn);
    gtk_widget_set_sensitive(S->y_axis_frame, !S->xy_linked);
    widgets_to_profile(S);
}

void on_save_clicked(GtkButton*, gpointer user_data) {
    auto* S = static_cast<AppState*>(user_data);
    // Save As: always ask for a name so the default profile is never overwritten.
    std::string cur_name = S->config.profiles.empty() ? "" : cur_prof(S).name;
    show_input_dialog(S, ui_text(S, "Save Profile As", "Profili Farklı Kaydet"),
        ui_text(S, "Profile name", "Profil adı"), cur_name.c_str(),
        [S](const std::string& name) {
            if (name.empty()) return;
            // If a profile with this name already exists, overwrite it;
            // otherwise create a new one (copy of current settings).
            widgets_to_profile(S);
            device_profile dp = cur_prof(S);
            dp.name = name;

            bool found = false;
            for (int i = 0; i < (int)S->config.profiles.size(); ++i) {
                if (S->config.profiles[i].name == name) {
                    S->config.profiles[i] = dp;
                    S->current_profile_idx = i;
                    found = true;
                    break;
                }
            }
            if (!found) {
                S->config.profiles.push_back(dp);
                S->current_profile_idx = (int)S->config.profiles.size() - 1;
            }
            S->config.active_profile = name;
            rebuild_profile_combo(S);
            save_config_now(S);
        });
}

void on_apply_clicked(GtkButton*, gpointer user_data) {
    auto* S = static_cast<AppState*>(user_data);
    // Apply & Reload: same Save As logic, then reload the daemon.
    std::string cur_name = S->config.profiles.empty() ? "" : cur_prof(S).name;
    show_input_dialog(S, ui_text(S, "Save Profile As", "Profili Farklı Kaydet"),
        ui_text(S, "Profile name", "Profil adı"), cur_name.c_str(),
        [S](const std::string& name) {
            if (name.empty()) return;
            widgets_to_profile(S);
            device_profile dp = cur_prof(S);
            dp.name = name;

            bool found = false;
            for (int i = 0; i < (int)S->config.profiles.size(); ++i) {
                if (S->config.profiles[i].name == name) {
                    S->config.profiles[i] = dp;
                    S->current_profile_idx = i;
                    found = true;
                    break;
                }
            }
            if (!found) {
                S->config.profiles.push_back(dp);
                S->current_profile_idx = (int)S->config.profiles.size() - 1;
            }
            S->config.active_profile = name;
            rebuild_profile_combo(S);
            // save_config_now() auto-sends SIGHUP when daemon is running.
            save_config_now(S);
        });
}

void on_daemon_start(GtkButton*, gpointer user_data) {
    auto* S = static_cast<AppState*>(user_data);
    if (has_systemd_rawaccel_unit()) {
        std::string err;
        if (systemctl_async("start", &err)) {
            g_timeout_add(1000, [](gpointer p) -> gboolean {
                update_daemon_status(static_cast<AppState*>(p));
                return G_SOURCE_REMOVE;
            }, S);
            set_status(S, ui_text(S, "Starting daemon via systemd...", "Daemon systemd ile başlatılıyor..."));
            return;
        }
        set_status(S, err + ui_text(S, " Falling back to direct daemon start...", " Doğrudan daemon başlatmaya geçiliyor..."));
    }

    // Candidate paths in preference order
    static const char* candidates[] = {
        "/usr/local/bin/rawaccel-daemon",
        "/usr/bin/rawaccel-daemon",
        nullptr
    };
    // Also check next to the GUI binary (same dir)
    std::string self_dir;
    {
        char buf[1024] = {};
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) {
            self_dir = fs::path(std::string(buf, n)).parent_path().string();
        }
    }

    std::string daemon_path;
    for (int i = 0; candidates[i]; i++) {
        if (fs::exists(candidates[i])) { daemon_path = candidates[i]; break; }
    }
    if (daemon_path.empty() && !self_dir.empty()) {
        std::string p = self_dir + "/rawaccel-daemon";
        if (fs::exists(p)) daemon_path = p;
    }
    if (daemon_path.empty()) {
        set_status(S, ui_text(S, "rawaccel-daemon not found. Install with: sudo scripts/install.sh",
                              "rawaccel-daemon bulunamadı. Kurulum: sudo scripts/install.sh"));
        return;
    }

    // Use fork+execv to avoid shell injection — never pass paths through sh -c
    pid_t pid = fork();
    if (pid == 0) {
        // Child: exec pkexec rawaccel-daemon -v -c <config>
        // setsid so child doesn't die with GUI
        setsid();
        const char* args[] = {
            "pkexec",
            daemon_path.c_str(),
            "-v",
            "-c", S->config_path.c_str(),
            nullptr
        };
        execvp("pkexec", const_cast<char* const*>(args));
        _exit(127); // execvp failed
    } else if (pid > 0) {
        // Parent: don't wait — daemon runs in background.
        // Reap zombie asynchronously via a one-shot timeout.
        // Pass pid on heap so the lambda can reap exactly this child (not all children).
        pid_t* pid_ptr = new pid_t(pid);
        g_timeout_add(3000, [](gpointer p) -> gboolean {
            pid_t* pp = static_cast<pid_t*>(p);
            int status;
            waitpid(*pp, &status, WNOHANG);
            delete pp;
            return G_SOURCE_REMOVE;
        }, pid_ptr);
    } else {
        set_status(S, ui_text(S, "fork() failed.", "fork() başarısız."));
        return;
    }
    // Update daemon status after a short delay (daemon needs time to start)
    g_timeout_add(1500, [](gpointer p) -> gboolean {
        update_daemon_status(static_cast<AppState*>(p));
        return G_SOURCE_REMOVE;
    }, S);
    set_status(S, ui_text(S, "Starting daemon...", "Daemon başlatılıyor..."));
}

void on_daemon_stop(GtkButton*, gpointer user_data) {
    auto* S = static_cast<AppState*>(user_data);
    if (has_systemd_rawaccel_unit()) {
        std::string err;
        if (systemctl_async("stop", &err)) {
            g_timeout_add(800, [](gpointer p) -> gboolean {
                update_daemon_status(static_cast<AppState*>(p));
                return G_SOURCE_REMOVE;
            }, S);
            set_status(S, ui_text(S, "Stopping daemon via systemd...", "Daemon systemd ile durduruluyor..."));
            return;
        }
    }
    std::string err;
    if (!daemon_send_signal(SIGTERM, &err)) {
        set_status(S, err);
        return;
    }
    g_timeout_add(800, [](gpointer p) -> gboolean {
        update_daemon_status(static_cast<AppState*>(p));
        return G_SOURCE_REMOVE;
    }, S);
    set_status(S, ui_text(S, "Stopping daemon...", "Daemon durduruluyor..."));
}

void on_daemon_reload(GtkButton*, gpointer user_data) {
    auto* S = static_cast<AppState*>(user_data);
    // Prefer IPC reload (works without signal permissions); fall back to SIGHUP.
    std::string ipc_resp = daemon_ipc_query("reload");
    if (!ipc_resp.empty() && ipc_resp.find("ok") != std::string::npos) {
        set_status(S, ui_text(S, "Daemon reloaded (IPC).", "Daemon yenilendi (IPC)."));
        update_daemon_status(S);
        return;
    }
    std::string err;
    if (!daemon_send_signal(SIGHUP, &err)) {
        if (has_systemd_rawaccel_unit()) {
            std::string serr;
            if (systemctl_async("reload", &serr)) {
                set_status(S, ui_text(S, "Daemon reloaded (systemd).", "Daemon yenilendi (systemd)."));
                update_daemon_status(S);
                return;
            }
            set_status(S, err + " | " + serr);
            return;
        } else {
            set_status(S, err);
            return;
        }
    }
    set_status(S, ui_text(S, "Daemon reloaded (SIGHUP).", "Daemon yenilendi (SIGHUP)."));
    update_daemon_status(S);
}
