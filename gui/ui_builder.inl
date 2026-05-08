// ── UI construction: layout helpers, build_ui(), window-close, on_activate() ──

// Forward declarations for KDE helpers defined later in this file (after on_activate)
static void on_kde_fix_clicked(GtkButton*, gpointer);
static void update_kde_warn_bar(AppState*);
static void on_kde_open_settings(GtkWidget*, gpointer);
static void apply_language(AppState*);
static void on_language_changed(GtkDropDown*, GParamSpec*, gpointer);

static void apply_text_binding(AppState* S, const UiTextBinding& b) {
    if (!b.widget || !b.en) return;
    const char* text = ui_text(S, b.en, b.tr);
    switch (b.kind) {
    case UiTextKind::label:   gtk_label_set_text(GTK_LABEL(b.widget), text); break;
    case UiTextKind::markup:  gtk_label_set_markup(GTK_LABEL(b.widget), text); break;
    case UiTextKind::button:  gtk_button_set_label(GTK_BUTTON(b.widget), text); break;
    case UiTextKind::check:   gtk_check_button_set_label(GTK_CHECK_BUTTON(b.widget), text); break;
    case UiTextKind::tooltip: gtk_widget_set_tooltip_text(b.widget, text); break;
    }
}

static void bind_text(AppState* S, GtkWidget* w, UiTextKind kind, const char* en, const char* tr) {
    S->ui_texts.push_back({w, kind, en, tr});
    apply_text_binding(S, S->ui_texts.back());
}

static void bind_tooltip(AppState* S, GtkWidget* w, const char* en, const char* tr) {
    bind_text(S, w, UiTextKind::tooltip, en, tr);
}

static GtkWidget* make_label(AppState* S, const char* en, const char* tr) {
    GtkWidget* lbl = gtk_label_new(nullptr);
    bind_text(S, lbl, UiTextKind::label, en, tr);
    return lbl;
}

static GtkWidget* make_help_button(AppState* S, const char* en, const char* tr) {
    GtkWidget* btn = gtk_button_new_with_label("?");
    gtk_widget_add_css_class(btn, "flat");
    gtk_widget_set_focusable(btn, FALSE);
    bind_tooltip(S, btn, en, tr);
    return btn;
}

static GtkWidget* make_check_help_row(AppState* S, GtkWidget* check, const char* en, const char* tr) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_append(GTK_BOX(box), check);
    gtk_box_append(GTK_BOX(box), make_help_button(S, en, tr));
    return box;
}

static void set_dropdown_strings(GtkWidget* dd, const char* const* names) {
    if (!dd || !names) return;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    GtkStringList* sl = gtk_string_list_new(names);
    gtk_drop_down_set_model(GTK_DROP_DOWN(dd), G_LIST_MODEL(sl));
    g_object_unref(sl);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), sel);
}

static void update_localized_combo_models(AppState* S) {
    static const char* mode_en[] = {
        "None (1:1)", "Classic", "Power", "Natural", "Jump", "Synchronous",
        "Lookup (LUT)", nullptr };
    static const char* mode_tr[] = {
        "Yok (1:1)", "Classic", "Power", "Natural", "Jump", "Synchronous",
        "Lookup (LUT)", nullptr };
    static const char* cap_en[] = {"Output (out)", "Input (in)", "I/O (io)", nullptr};
    static const char* cap_tr[] = {"Çıkış (out)", "Giriş (in)", "G/Ç (io)", nullptr};
    static const char* dist_en[] = {"Euclidean", "Max", "Lp", "Separate", nullptr};
    static const char* dist_tr[] = {"Öklid", "Maks", "Lp", "Ayrı", nullptr};
    bool tr = S->language == UiLanguage::turkish;
    set_dropdown_strings(S->mode_combo, tr ? mode_tr : mode_en);
    set_dropdown_strings(S->mode_combo_y, tr ? mode_tr : mode_en);
    set_dropdown_strings(S->cap_mode_combo, tr ? cap_tr : cap_en);
    set_dropdown_strings(S->dist_mode_combo, tr ? dist_tr : dist_en);
    if (S->device_id_combo) {
        guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(S->device_id_combo));
        GtkStringList* sl = gtk_string_list_new(nullptr);
        gtk_string_list_append(sl, ui_text(S, "All devices (default)", "Tüm cihazlar (varsayılan)"));
        for (auto& m : S->mice_list) {
            std::string label = m.name + "  [" + m.event_node + "]";
            gtk_string_list_append(sl, label.c_str());
        }
        gtk_drop_down_set_model(GTK_DROP_DOWN(S->device_id_combo), G_LIST_MODEL(sl));
        g_object_unref(sl);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(S->device_id_combo), sel);
    }
}

GtkWidget* make_spin(double mn, double mx, double step, double val, int digits = 3) {
    GtkWidget* s = gtk_spin_button_new_with_range(mn, mx, step);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(s), digits);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s), val);
    gtk_widget_set_hexpand(s, TRUE);
    return s;
}

void connect_spin(GtkWidget* s, gpointer user_data) {
    g_signal_connect(s, "value-changed", G_CALLBACK(on_param_changed), user_data);
}

void grid_row(AppState* S, GtkWidget* grid, int row,
              const char* label_en, const char* label_tr, GtkWidget* w,
              const char* help_en = nullptr, const char* help_tr = nullptr) {
    GtkWidget* lbl = make_label(S, label_en, label_tr);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    GtkWidget* label_cell = lbl;
    if (help_en) {
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_widget_set_margin_end(box, 6);
        gtk_box_append(GTK_BOX(box), lbl);
        gtk_box_append(GTK_BOX(box), make_help_button(S, help_en, help_tr));
        label_cell = box;
        bind_tooltip(S, w, help_en, help_tr);
    } else {
        gtk_widget_set_margin_end(lbl, 6);
    }
    gtk_grid_attach(GTK_GRID(grid), label_cell, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), w,   1, row, 1, 1);
}

GtkWidget* make_section(AppState* S, const char* en, const char* tr) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget* lbl = gtk_label_new(nullptr);
    bind_text(S, lbl, UiTextKind::markup, en, tr);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    gtk_widget_set_margin_top(lbl, 10);
    gtk_box_append(GTK_BOX(box), lbl);
    GtkWidget* sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(box), sep);
    return box;
}

GtkWidget* make_grid() {
    GtkWidget* g = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(g), 8);
    gtk_grid_set_row_spacing(GTK_GRID(g), 4);
    gtk_widget_set_margin_start(g, 4);
    return g;
}

static void apply_language(AppState* S) {
    bool prev_updating = S->updating;
    S->updating = true;
    for (const auto& b : S->ui_texts)
        apply_text_binding(S, b);
    update_localized_combo_models(S);
    gtk_window_set_title(GTK_WINDOW(S->window),
                         (std::string("RawAccel Linux  v") + RAWACCEL_VERSION).c_str());
    if (S->daemon_status) update_daemon_status(S);
    if (S->lut_list_box) rebuild_lut_list(S);
    S->updating = prev_updating;
}

static void on_language_changed(GtkDropDown* dd, GParamSpec*, gpointer user_data) {
    auto* S = static_cast<AppState*>(user_data);
    S->language = gtk_drop_down_get_selected(dd) == 1 ? UiLanguage::turkish : UiLanguage::english;
    apply_language(S);
}

// ── Build UI ──────────────────────────────────────────────────────────────────

// Forward declaration — defined below (after on_activate)
gboolean on_window_close_request(GtkWindow*, gpointer);

// inotify hot-plug callback — forward declaration (defined in devices.inl)
gboolean on_inotify_event(GIOChannel*, GIOCondition, gpointer);

void build_ui(AppState* S, GtkApplication* gapp) {
    S->window = gtk_application_window_new(gapp);
    gtk_window_set_title(GTK_WINDOW(S->window),
                         (std::string("RawAccel Linux  v") + RAWACCEL_VERSION).c_str());
    gtk_window_set_default_size(GTK_WINDOW(S->window), 1020, 680);

    // ── Header bar ────────────────────────────────────────────────────────────
    GtkWidget* hbar = gtk_header_bar_new();
    gtk_window_set_titlebar(GTK_WINDOW(S->window), hbar);

    // Profile combo
    {
        GtkStringList* sl = gtk_string_list_new(nullptr);
        for (auto& p : S->config.profiles)
            gtk_string_list_append(sl, p.name.c_str());
        S->profile_combo = gtk_drop_down_new(G_LIST_MODEL(sl), nullptr);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(S->profile_combo), 0);
        g_signal_connect(S->profile_combo, "notify::selected",
                         G_CALLBACK(on_profile_changed), S);
        gtk_header_bar_pack_start(GTK_HEADER_BAR(hbar), S->profile_combo);
    }

    // Profile buttons
    struct { const char* icon; GCallback cb; const char* tip_en; const char* tip_tr; } pbts[] = {
        {"list-add-symbolic",        G_CALLBACK(on_new_profile),       "New profile",          "Yeni profil"},
        {"edit-copy-symbolic",       G_CALLBACK(on_duplicate_profile), "Duplicate profile",    "Profili çoğalt"},
        {"document-edit-symbolic",   G_CALLBACK(on_rename_profile),    "Rename profile",       "Profili yeniden adlandır"},
        {"edit-clear-symbolic",      G_CALLBACK(on_reset_profile),     "Reset to defaults",    "Varsayılana sıfırla"},
        {"edit-delete-symbolic",     G_CALLBACK(on_delete_profile),    "Delete profile",       "Profili sil"},
    };
    for (auto& b : pbts) {
        GtkWidget* btn = gtk_button_new_from_icon_name(b.icon);
        bind_tooltip(S, btn, b.tip_en, b.tip_tr);
        g_signal_connect(btn, "clicked", b.cb, S);
        gtk_header_bar_pack_start(GTK_HEADER_BAR(hbar), btn);
    }

    // Right side: daemon controls + save/apply
    S->daemon_status = gtk_label_new("● Checking...");
    gtk_widget_set_margin_end(S->daemon_status, 8);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hbar), S->daemon_status);

    const char* lang_names[] = {"English", "Türkçe", nullptr};
    S->language_combo = gtk_drop_down_new_from_strings(lang_names);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(S->language_combo), 0);
    g_signal_connect(S->language_combo, "notify::selected", G_CALLBACK(on_language_changed), S);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hbar), S->language_combo);
    GtkWidget* lang_lbl = make_label(S, "Language:", "Dil:");
    gtk_widget_set_margin_end(lang_lbl, 4);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hbar), lang_lbl);

    // Daemon control buttons — store pointers in AppState
    // so update_daemon_status() can set the correct sensitivity
    S->daemon_reload_btn = gtk_button_new_with_label("Reload");
    bind_text(S, S->daemon_reload_btn, UiTextKind::button, "Reload", "Yenile");
    g_signal_connect(S->daemon_reload_btn, "clicked", G_CALLBACK(on_daemon_reload), S);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hbar), S->daemon_reload_btn);

    S->daemon_stop_btn = gtk_button_new_with_label("Stop");
    bind_text(S, S->daemon_stop_btn, UiTextKind::button, "Stop", "Durdur");
    g_signal_connect(S->daemon_stop_btn, "clicked", G_CALLBACK(on_daemon_stop), S);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hbar), S->daemon_stop_btn);

    S->daemon_start_btn = gtk_button_new_with_label("Start");
    bind_text(S, S->daemon_start_btn, UiTextKind::button, "Start", "Başlat");
    gtk_widget_add_css_class(S->daemon_start_btn, "suggested-action");
    g_signal_connect(S->daemon_start_btn, "clicked", G_CALLBACK(on_daemon_start), S);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hbar), S->daemon_start_btn);

    S->apply_btn = gtk_button_new_with_label("Apply & Reload");
    bind_text(S, S->apply_btn, UiTextKind::button, "Apply & Reload", "Uygula ve Yenile");
    gtk_widget_add_css_class(S->apply_btn, "suggested-action");
    g_signal_connect(S->apply_btn, "clicked", G_CALLBACK(on_apply_clicked), S);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hbar), S->apply_btn);

    GtkWidget* save_btn = gtk_button_new_with_label("Save");
    bind_text(S, save_btn, UiTextKind::button, "Save", "Kaydet");
    g_signal_connect(save_btn, "clicked", G_CALLBACK(on_save_clicked), S);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hbar), save_btn);

    // ── Main split ────────────────────────────────────────────────────────────
    GtkWidget* outer_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(S->window), outer_vbox);

    GtkWidget* hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand(hpaned, TRUE);
    gtk_paned_set_position(GTK_PANED(hpaned), 360);
    gtk_box_append(GTK_BOX(outer_vbox), hpaned);

    // ── LEFT PANEL ────────────────────────────────────────────────────────────
    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, 330, -1);
    gtk_paned_set_start_child(GTK_PANED(hpaned), scroll);
    gtk_paned_set_shrink_start_child(GTK_PANED(hpaned), FALSE);

    GtkWidget* lvbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(lvbox, 12);
    gtk_widget_set_margin_end(lvbox, 12);
    gtk_widget_set_margin_top(lvbox, 6);
    gtk_widget_set_margin_bottom(lvbox, 12);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), lvbox);

    auto append_section = [&](const char* en, const char* tr) {
        gtk_box_append(GTK_BOX(lvbox), make_section(S, en, tr));
    };
    auto append_grid = [&]() -> GtkWidget* {
        GtkWidget* g = make_grid();
        gtk_box_append(GTK_BOX(lvbox), g);
        return g;
    };

    // ── Raw Passthrough ───────────────────────────────────────────────────────
    S->raw_check = gtk_check_button_new_with_label("Raw Passthrough (bypass all acceleration)");
    bind_text(S, S->raw_check, UiTextKind::check,
        "Raw Passthrough (bypass all acceleration)",
        "Ham Geçiş (tüm hızlandırmayı atla)");
    bind_tooltip(S, S->raw_check,
        "When enabled, the entire acceleration pipeline is bypassed.\n"
        "No rotation, snap, speed clamp, weights, or sub-pixel accumulation —\n"
        "raw kernel counts are written directly to uinput (1:1 passthrough).",
        "Açıldığında tüm hızlandırma hattı atlanır.\n"
        "Döndürme, snap, hız limiti, ağırlıklar veya sub-pixel birikim uygulanmaz —\n"
        "kernel'dan gelen ham hareketler doğrudan uinput'a 1:1 yazılır.");
    gtk_widget_set_margin_top(S->raw_check, 6);
    g_signal_connect(S->raw_check, "toggled", G_CALLBACK(on_param_changed), S);
    gtk_box_append(GTK_BOX(lvbox), make_check_help_row(S, S->raw_check,
        "When enabled, the entire acceleration pipeline is bypassed.\n"
        "No rotation, snap, speed clamp, weights, or sub-pixel accumulation —\n"
        "raw kernel counts are written directly to uinput (1:1 passthrough).",
        "Açıldığında tüm hızlandırma hattı atlanır.\n"
        "Döndürme, snap, hız limiti, ağırlıklar veya sub-pixel birikim uygulanmaz —\n"
        "kernel'dan gelen ham hareketler doğrudan uinput'a 1:1 yazılır."));

    // ── Acceleration X ────────────────────────────────────────────────────────
    append_section("<b>Acceleration — X Axis</b>", "<b>Hızlandırma — X Ekseni</b>");
    GtkWidget* ag = append_grid();

    const char* mode_names[] = {
        "None (1:1)", "Classic", "Power", "Natural", "Jump", "Synchronous",
        "Lookup (LUT)", nullptr };
    GtkStringList* msl = gtk_string_list_new(mode_names);
    S->mode_combo = gtk_drop_down_new(G_LIST_MODEL(msl), nullptr);
    gtk_widget_set_hexpand(S->mode_combo, TRUE);
    g_signal_connect(S->mode_combo, "notify::selected", G_CALLBACK(on_notify_param_changed), S);
    grid_row(S, ag, 0, "Mode:", "Mod:", S->mode_combo,
        "Acceleration algorithm used for the X axis.",
        "X ekseni için kullanılacak hızlandırma algoritması.");

    S->gain_check = gtk_check_button_new_with_label("Gain mode (recommended)");
    bind_text(S, S->gain_check, UiTextKind::check,
        "Gain mode (recommended)", "Gain modu (önerilir)");
    bind_tooltip(S, S->gain_check,
        "Uses multiplier/gain style output instead of direct velocity output.",
        "Doğrudan hız çıkışı yerine çarpan/gain tabanlı çıkış kullanır.");
    g_signal_connect(S->gain_check, "toggled", G_CALLBACK(on_param_changed), S);
    gtk_grid_attach(GTK_GRID(ag), make_check_help_row(S, S->gain_check,
        "Uses multiplier/gain style output instead of direct velocity output.",
        "Doğrudan hız çıkışı yerine çarpan/gain tabanlı çıkış kullanır."), 0, 1, 2, 1);

    S->accel_spin      = make_spin(0,    20,    0.001, 0.005);
    S->exponent_spin   = make_spin(1,    10,    0.05,  2.0);
    S->power_exp_spin  = make_spin(0.01, 5,     0.01,  0.05);
    S->limit_spin      = make_spin(0.1,  100,   0.05,  1.5);
    S->offset_spin     = make_spin(0,    100,   0.5,   0.0);
    S->decay_spin      = make_spin(0,    10,    0.01,  0.1);
    S->cap_x_spin      = make_spin(0,    500,   1,     15, 0);
    S->cap_y_spin      = make_spin(0,    100,   0.05,  1.5);
    S->sync_speed_spin = make_spin(0.1,  100,   0.5,   5.0);
    S->smooth_spin     = make_spin(0,    1,     0.01,  0.5);
    S->motivity_spin      = make_spin(0.01, 10,    0.01,  1.5);
    S->gamma_spin         = make_spin(0.01, 10,    0.01,  1.0);
    S->output_offset_spin = make_spin(0,    100,   0.001, 0.0);
    S->scale_spin         = make_spin(0.01, 100,   0.01,  1.0);

    for (auto* s : {S->accel_spin, S->exponent_spin, S->power_exp_spin,
                    S->limit_spin, S->offset_spin, S->decay_spin,
                    S->cap_x_spin, S->cap_y_spin,
                    S->sync_speed_spin, S->smooth_spin,
                    S->motivity_spin, S->gamma_spin,
                    S->output_offset_spin, S->scale_spin})
        connect_spin(s, S);
    bind_tooltip(S, S->motivity_spin,
        "Compatibility field stored in the profile; currently not used by the natural algorithm.",
        "Profilde saklanan uyumluluk alanı; şu anda natural algoritması tarafından kullanılmaz.");
    bind_tooltip(S, S->gamma_spin,
        "Compatibility field stored in the profile; currently not used by the classic algorithm.",
        "Profilde saklanan uyumluluk alanı; şu anda classic algoritması tarafından kullanılmaz.");

    const char* cap_names[] = {"Output (out)", "Input (in)", "I/O (io)", nullptr};
    GtkStringList* csl = gtk_string_list_new(cap_names);
    S->cap_mode_combo = gtk_drop_down_new(G_LIST_MODEL(csl), nullptr);
    gtk_widget_set_hexpand(S->cap_mode_combo, TRUE);
    g_signal_connect(S->cap_mode_combo, "notify::selected", G_CALLBACK(on_notify_param_changed), S);

    // Wrap normal parameter widgets in a frame (hidden in LUT mode)
    S->accel_params_frame = gtk_frame_new(nullptr);
    gtk_widget_set_margin_top(S->accel_params_frame, 2);
    {
        GtkWidget* pg = make_grid();
        gtk_widget_set_margin_start(pg, 4); gtk_widget_set_margin_end(pg, 4);
        gtk_widget_set_margin_top(pg, 4);   gtk_widget_set_margin_bottom(pg, 4);
        gtk_frame_set_child(GTK_FRAME(S->accel_params_frame), pg);
        grid_row(S, pg, 0, "Accel:", "İvme:", S->accel_spin,
            "Main acceleration strength. Higher values increase gain faster.",
            "Ana hızlandırma gücü. Yüksek değer, gain değerini daha hızlı artırır.");
        grid_row(S, pg, 1, "Exp (cls):", "Üs (cls):", S->exponent_spin,
            "Classic mode exponent. Higher values make the curve steeper.",
            "Classic mod üssü. Yüksek değer eğriyi daha dik yapar.");
        grid_row(S, pg, 2, "Exp (pwr):", "Üs (pwr):", S->power_exp_spin,
            "Power/synchronous exponent used by those modes.",
            "Power/synchronous modlarının kullandığı üs değeri.");
        grid_row(S, pg, 3, "Limit:", "Limit:", S->limit_spin,
            "Maximum gain/curve limit for modes that support a cap.",
            "Limit destekleyen modlarda en yüksek gain/eğri sınırı.");
        grid_row(S, pg, 4, "Input Offset:", "Giriş Eşiği:", S->offset_spin,
            "Speed offset before acceleration begins.",
            "Hızlandırma başlamadan önceki hız eşiği.");
        grid_row(S, pg, 5, "Decay Rate:", "Sönüm Oranı:", S->decay_spin,
            "Natural mode decay rate; controls how quickly gain approaches its limit.",
            "Natural mod sönüm oranı; gain değerinin limite ne kadar hızlı yaklaşacağını belirler.");
        grid_row(S, pg, 6, "Cap X:", "Cap X:", S->cap_x_spin,
            "Input speed point used by cap modes.",
            "Cap modlarının kullandığı giriş hızı noktası.");
        grid_row(S, pg, 7, "Cap Y:", "Cap Y:", S->cap_y_spin,
            "Output gain at the cap point.",
            "Cap noktasındaki çıkış gain değeri.");
        grid_row(S, pg, 8, "Cap Mode:", "Cap Modu:", S->cap_mode_combo,
            "Controls whether the cap is interpreted on input, output, or both.",
            "Cap değerinin girişte, çıkışta veya ikisinde nasıl yorumlanacağını belirler.");
        grid_row(S, pg, 9, "Sync Speed:", "Senk. Hız:", S->sync_speed_spin,
            "Reference speed used by synchronous mode.",
            "Synchronous modunun kullandığı referans hız.");
        grid_row(S, pg, 10, "Smoothing:", "Yumuşatma:", S->smooth_spin,
            "Smooths jump/synchronous transitions. 0 = none, 1 = strongest.",
            "Jump/synchronous geçişlerini yumuşatır. 0 = kapalı, 1 = en güçlü.");
        grid_row(S, pg, 11, "Motivity (compat):", "Motivity (uyum):", S->motivity_spin,
            "Compatibility field stored in the profile; currently not used by the natural algorithm.",
            "Profilde saklanan uyumluluk alanı; şu anda natural algoritması tarafından kullanılmaz.");
        grid_row(S, pg, 12, "Gamma (compat):", "Gamma (uyum):", S->gamma_spin,
            "Compatibility field stored in the profile; currently not used by the classic algorithm.",
            "Profilde saklanan uyumluluk alanı; şu anda classic algoritması tarafından kullanılmaz.");
        grid_row(S, pg, 13, "Out Offset:", "Çıkış Ofseti:", S->output_offset_spin,
            "Adds an output offset in power mode after acceleration is computed.",
            "Power modunda hızlandırma hesaplandıktan sonra çıkış ofseti ekler.");
        grid_row(S, pg, 14, "Scale:", "Ölçek:", S->scale_spin,
            "Scale multiplier used by power mode.",
            "Power modunun kullandığı ölçek çarpanı.");
    }
    gtk_box_append(GTK_BOX(lvbox), S->accel_params_frame);

    // ── LUT editor frame ──────────────────────────────────────────────────
    S->lut_frame = gtk_frame_new(nullptr);
    gtk_widget_set_margin_top(S->lut_frame, 2);
    gtk_widget_set_visible(S->lut_frame, FALSE); // hidden initially
    {
        GtkWidget* lut_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_margin_start(lut_vbox, 4);
        gtk_widget_set_margin_end(lut_vbox, 4);
        gtk_widget_set_margin_top(lut_vbox, 6);
        gtk_widget_set_margin_bottom(lut_vbox, 6);
        gtk_frame_set_child(GTK_FRAME(S->lut_frame), lut_vbox);

        // Bilgi etiketi
        GtkWidget* info_lbl = gtk_label_new(nullptr);
        bind_text(S, info_lbl, UiTextKind::markup,
            "<small>Left click: add point on graph\n"
            "Right click: remove point on graph\n"
            "Points are speed (ips) → gain pairs.</small>",
            "<small>Sol tık: grafiğe nokta ekle\n"
            "Sağ tık: grafikten nokta sil\n"
            "Noktalar hız (ips) → gain çiftleridir.</small>");
        gtk_label_set_xalign(GTK_LABEL(info_lbl), 0.0);
        gtk_label_set_wrap(GTK_LABEL(info_lbl), TRUE);
        gtk_box_append(GTK_BOX(lut_vbox), info_lbl);

        // Nokta listesi (scrollable)
        GtkWidget* lut_scroll = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(lut_scroll),
                                       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_widget_set_size_request(lut_scroll, -1, 180);
        gtk_box_append(GTK_BOX(lut_vbox), lut_scroll);

        S->lut_list_box = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(S->lut_list_box), GTK_SELECTION_NONE);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(lut_scroll), S->lut_list_box);
        // Attach AppState* to the list_box so LUT row callbacks can retrieve it
        g_object_set_data(G_OBJECT(S->lut_list_box), "app-state", S);

        // "Nokta Ekle" butonu
        GtkWidget* add_btn = gtk_button_new_with_label("+ Add Point");
        bind_text(S, add_btn, UiTextKind::button, "+ Add Point", "+ Nokta Ekle");
        gtk_widget_add_css_class(add_btn, "suggested-action");
        g_signal_connect(add_btn, "clicked", G_CALLBACK(on_lut_add_point), S);
        gtk_box_append(GTK_BOX(lut_vbox), add_btn);

        // "Sort" button — reorder points by speed
        GtkWidget* sort_btn = gtk_button_new_with_label("Sort");
        bind_text(S, sort_btn, UiTextKind::button, "Sort", "Sırala");
        bind_tooltip(S, sort_btn,
            "Sort points by speed value (ascending)",
            "Noktaları hız değerine göre küçükten büyüğe sırala");
        g_signal_connect(sort_btn, "clicked", G_CALLBACK(+[](GtkWidget*, gpointer ud) {
            auto* S2 = static_cast<AppState*>(ud);
            auto& ax = cur_prof(S2).prof.accel_x;
            auto pts = lut_get_points(ax);
            lut_set_points(ax, pts); // lut_set_points already sorts
            if (S2->xy_linked) cur_prof(S2).prof.accel_y = ax;
            rebuild_lut_list(S2);
            gtk_widget_queue_draw(S2->graph_area);
        }), S);
        gtk_box_append(GTK_BOX(lut_vbox), sort_btn);
    }
    gtk_box_append(GTK_BOX(lvbox), S->lut_frame);

    // ── XY Link ───────────────────────────────────────────────────────────────
    append_section("<b>Y Axis</b>", "<b>Y Ekseni</b>");
    S->xy_link_btn = gtk_check_button_new_with_label("Same as X (linked)");
    bind_text(S, S->xy_link_btn, UiTextKind::check,
        "Same as X (linked)", "X ile aynı (bağlı)");
    bind_tooltip(S, S->xy_link_btn,
        "When enabled, Y-axis settings are copied from X-axis settings.",
        "Açıldığında Y ekseni ayarları X ekseni ayarlarından kopyalanır.");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(S->xy_link_btn), TRUE);
    g_signal_connect(S->xy_link_btn, "toggled", G_CALLBACK(on_xy_link_toggled), S);
    gtk_box_append(GTK_BOX(lvbox), make_check_help_row(S, S->xy_link_btn,
        "When enabled, Y-axis settings are copied from X-axis settings.",
        "Açıldığında Y ekseni ayarları X ekseni ayarlarından kopyalanır."));

    S->y_axis_frame = gtk_frame_new(nullptr);
    gtk_widget_set_sensitive(S->y_axis_frame, FALSE);
    gtk_box_append(GTK_BOX(lvbox), S->y_axis_frame);

    GtkWidget* yg = make_grid();
    gtk_widget_set_margin_start(yg, 6); gtk_widget_set_margin_end(yg, 6);
    gtk_widget_set_margin_top(yg, 6);   gtk_widget_set_margin_bottom(yg, 6);
    gtk_frame_set_child(GTK_FRAME(S->y_axis_frame), yg);

    GtkStringList* msl2 = gtk_string_list_new(mode_names);
    S->mode_combo_y = gtk_drop_down_new(G_LIST_MODEL(msl2), nullptr);
    gtk_widget_set_hexpand(S->mode_combo_y, TRUE);
    g_signal_connect(S->mode_combo_y, "notify::selected", G_CALLBACK(on_notify_param_changed), S);

    S->accel_spin_y      = make_spin(0,   20,   0.001, 0.005);
    S->exponent_spin_y   = make_spin(1,   10,   0.05,  2.0);
    S->limit_spin_y      = make_spin(0.1, 100,  0.05,  1.5);
    S->offset_spin_y     = make_spin(0,   100,  0.5,   0.0);
    S->cap_y_spin_y      = make_spin(0,   100,  0.05,  1.5);
    for (auto* s : {S->accel_spin_y, S->exponent_spin_y,
                    S->limit_spin_y, S->offset_spin_y, S->cap_y_spin_y})
        connect_spin(s, S);

    grid_row(S, yg, 0, "Mode:", "Mod:", S->mode_combo_y,
        "Acceleration algorithm used for the Y axis.",
        "Y ekseni için kullanılacak hızlandırma algoritması.");
    grid_row(S, yg, 1, "Accel:", "İvme:", S->accel_spin_y,
        "Y-axis acceleration strength.",
        "Y ekseni hızlandırma gücü.");
    grid_row(S, yg, 2, "Exp:", "Üs:", S->exponent_spin_y,
        "Y-axis classic exponent.",
        "Y ekseni classic üs değeri.");
    grid_row(S, yg, 3, "Limit:", "Limit:", S->limit_spin_y,
        "Y-axis gain/curve limit.",
        "Y ekseni gain/eğri sınırı.");
    grid_row(S, yg, 4, "Input Offset:", "Giriş Eşiği:", S->offset_spin_y,
        "Y-axis speed offset before acceleration begins.",
        "Y ekseni hızlandırması başlamadan önceki hız eşiği.");
    grid_row(S, yg, 5, "Cap Y:", "Cap Y:", S->cap_y_spin_y,
        "Y-axis output gain at the cap point.",
        "Y ekseni cap noktasındaki çıkış gain değeri.");

    // ── Rotasyon & Snap ───────────────────────────────────────────────────────
    append_section("<b>Rotation &amp; Snap</b>", "<b>Döndürme ve Yakalama</b>");
    GtkWidget* rg = append_grid();
    S->rotation_spin  = make_spin(-360, 360, 0.5, 0, 1);
    S->snap_spin      = make_spin(0, 45, 0.5, 0, 1);
    S->lr_ratio_spin  = make_spin(0.01, 100, 0.01, 1.0);
    S->ud_ratio_spin  = make_spin(0.01, 100, 0.01, 1.0);
    connect_spin(S->rotation_spin, S);
    connect_spin(S->snap_spin, S);
    connect_spin(S->lr_ratio_spin, S);
    connect_spin(S->ud_ratio_spin, S);
    grid_row(S, rg, 0, "Rotation (°):", "Döndürme (°):", S->rotation_spin,
        "Rotates input motion by the given angle before acceleration.",
        "Hızlandırmadan önce giriş hareketini belirtilen açı kadar döndürür.");
    grid_row(S, rg, 1, "Snap (°):", "Snap (°):", S->snap_spin,
        "Snaps near-horizontal/vertical movement within this angle.",
        "Bu açı içindeki yatay/dikey harekete hizalama uygular.");
    grid_row(S, rg, 2, "LR Ratio:", "Sol/Sağ Oran:", S->lr_ratio_spin,
        "Left/right output DPI ratio (1.0 = off). Values >1 amplify rightward movement.",
        "Sol/sağ çıkış DPI oranı (1.0 = kapalı). 1'den büyük değer sağa hareketi artırır.");
    grid_row(S, rg, 3, "UD Ratio:", "Yuk/Aşa Oran:", S->ud_ratio_spin,
        "Up/down output DPI ratio (1.0 = off). Values >1 amplify downward movement.",
        "Yukarı/aşağı çıkış DPI oranı (1.0 = kapalı). 1'den büyük değer aşağı hareketi artırır.");

    // ── Speed Limit ───────────────────────────────────────────────────────────
    append_section("<b>Speed Limit</b>", "<b>Hız Limiti</b>");
    GtkWidget* sg = append_grid();
    S->speed_min_spin = make_spin(0, 500, 1, 0, 0);
    S->speed_max_spin = make_spin(0, 500, 1, 0, 0);
    connect_spin(S->speed_min_spin, S);
    connect_spin(S->speed_max_spin, S);
    grid_row(S, sg, 0, "Min (ips):", "Min (ips):", S->speed_min_spin,
        "Minimum speed clamp (ips). 0 = disabled.",
        "Minimum hız sınırı (ips). 0 = kapalı.");
    grid_row(S, sg, 1, "Max (ips):", "Max (ips):", S->speed_max_spin,
        "Maximum speed clamp (ips). Set to 0 to disable clamping.",
        "Maksimum hız sınırı (ips). Sınırlamayı kapatmak için 0 yapın.");
    {
        GtkWidget* speed_hint = gtk_label_new(nullptr);
        bind_text(S, speed_hint, UiTextKind::markup,
            "<small>Set Max to 0 to disable speed clamping.</small>",
            "<small>Hız sınırlamayı kapatmak için Max değerini 0 yapın.</small>");
        gtk_label_set_xalign(GTK_LABEL(speed_hint), 0.0);
        gtk_box_append(GTK_BOX(lvbox), speed_hint);
    }

    // ── Speed Processor ───────────────────────────────────────────────────────
    append_section("<b>Speed Processor</b>", "<b>Hız İşleyici</b>");
    GtkWidget* spg = append_grid();
    {
        const char* dist_names[] = {"Euclidean", "Max", "Lp", "Separate", nullptr};
        GtkStringList* dsl = gtk_string_list_new(dist_names);
        S->dist_mode_combo = gtk_drop_down_new(G_LIST_MODEL(dsl), nullptr);
        gtk_widget_set_hexpand(S->dist_mode_combo, TRUE);
        g_signal_connect(S->dist_mode_combo, "notify::selected", G_CALLBACK(on_notify_param_changed), S);
        grid_row(S, spg, 0, "Distance:", "Mesafe:", S->dist_mode_combo,
            "How speed is calculated from X/Y input:\n"
            "  Euclidean — √(x²+y²)  (default)\n"
            "  Max — max(|x|,|y|)\n"
            "  Lp — generalized norm\n"
            "  Separate — X and Y processed independently",
            "Hızın X/Y girişinden nasıl hesaplanacağını belirler:\n"
            "  Euclidean — √(x²+y²)  (varsayılan)\n"
            "  Max — max(|x|,|y|)\n"
            "  Lp — genelleştirilmiş norm\n"
            "  Separate — X ve Y ayrı ayrı işlenir");

        S->lp_norm_spin = make_spin(1.0, 32.0, 0.5, 2.0);
        connect_spin(S->lp_norm_spin, S);
        bind_tooltip(S, S->lp_norm_spin,
            "Lp-norm exponent (only used when Distance = Lp). 2 = Euclidean, large values → Max.",
            "Lp norm üssü (sadece Distance = Lp iken kullanılır). 2 = Euclidean, büyük değerler → Max.");
        // Build the lp_norm row manually so we can get the label widget for show/hide
        GtkWidget* lp_label_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget* lp_label = make_label(S, "Lp Norm:", "Lp Norm:");
        gtk_label_set_xalign(GTK_LABEL(lp_label), 0.0);
        gtk_box_append(GTK_BOX(lp_label_box), lp_label);
        gtk_box_append(GTK_BOX(lp_label_box), make_help_button(S,
            "Lp-norm exponent (only used when Distance = Lp). 2 = Euclidean, large values → Max.",
            "Lp norm üssü (sadece Distance = Lp iken kullanılır). 2 = Euclidean, büyük değerler → Max."));
        gtk_widget_set_margin_end(lp_label_box, 6);
        S->lp_norm_label = lp_label_box;
        gtk_grid_attach(GTK_GRID(spg), S->lp_norm_label, 0, 1, 1, 1);
        gtk_grid_attach(GTK_GRID(spg), S->lp_norm_spin,  1, 1, 1, 1);
        gtk_widget_set_visible(S->lp_norm_label, FALSE); // hidden until Lp is selected
        gtk_widget_set_visible(S->lp_norm_spin,  FALSE);

        S->input_hl_spin  = make_spin(0, 200, 0.5, 0.0);
        S->scale_hl_spin  = make_spin(0, 200, 0.5, 0.0);
        S->output_hl_spin = make_spin(0, 200, 0.5, 0.0);
        connect_spin(S->input_hl_spin, S);
        connect_spin(S->scale_hl_spin, S);
        connect_spin(S->output_hl_spin, S);
        grid_row(S, spg, 2, "Input HL:", "Giriş HL:", S->input_hl_spin,
            "EMA half-life for input speed smoothing (ms). 0 = off.",
            "Giriş hızı yumuşatması için EMA yarı ömrü (ms). 0 = kapalı.");
        grid_row(S, spg, 3, "Scale HL:", "Ölçek HL:", S->scale_hl_spin,
            "EMA half-life for scale smoothing (ms). 0 = off.",
            "Ölçek yumuşatması için EMA yarı ömrü (ms). 0 = kapalı.");
        grid_row(S, spg, 4, "Output HL:", "Çıkış HL:", S->output_hl_spin,
            "EMA half-life for output speed smoothing (ms). 0 = off.",
            "Çıkış hızı yumuşatması için EMA yarı ömrü (ms). 0 = kapalı.");
    }
    {
        GtkWidget* sp_hint = gtk_label_new(nullptr);
        bind_text(S, sp_hint, UiTextKind::markup,
            "<small>HL = EMA half-life in ms. 0 = smoothing off.\n"
            "Separate: X and Y each processed by their own axis.</small>",
            "<small>HL = EMA yarı ömrü (ms). 0 = yumuşatma kapalı.\n"
            "Separate: X ve Y kendi eksenlerinde ayrı işlenir.</small>");
        gtk_label_set_xalign(GTK_LABEL(sp_hint), 0.0);
        gtk_label_set_wrap(GTK_LABEL(sp_hint), TRUE);
        gtk_box_append(GTK_BOX(lvbox), sp_hint);
    }

    // ── Device ────────────────────────────────────────────────────────────────
    append_section("<b>Device</b>", "<b>Cihaz</b>");
    GtkWidget* dg = append_grid();
    S->dpi_spin        = make_spin(100, 32000, 50, 800, 0);
    S->polling_spin    = make_spin(125, 8000, 125, 1000, 0);
    S->output_dpi_spin = make_spin(100, 32000, 50, 1000, 0);
    connect_spin(S->dpi_spin, S);
    connect_spin(S->polling_spin, S);
    connect_spin(S->output_dpi_spin, S);
    grid_row(S, dg, 0, "DPI:", "DPI:", S->dpi_spin,
        "Physical mouse DPI used to convert counts into real speed.",
        "Hareket sayımlarını gerçek hıza çevirmek için kullanılan fiziksel fare DPI değeri.");
    grid_row(S, dg, 1, "Polling Rate:", "Polling Hızı:", S->polling_spin,
        "Mouse polling rate in Hz. Used for timing fallback/clamping.",
        "Farenin Hz cinsinden polling hızı. Zamanlama fallback/sınırlama için kullanılır.");
    grid_row(S, dg, 2, "Output DPI:", "Çıkış DPI:", S->output_dpi_spin,
        "Output DPI normalization value (default: 1000).\n"
        "Change this to match your monitor's effective DPI scaling.",
        "Çıkış DPI normalizasyon değeri (varsayılan: 1000).\n"
        "Monitörünüzün etkili DPI ölçeklemesine uydurmak için değiştirin.");

    // ── Device assignment ─────────────────────────────────────────────────
    append_section("<b>Device Assignment</b>", "<b>Cihaz Ataması</b>");
    {
        // Fareleri tara
        S->mice_list = list_mice();

        // Dropdown model: "All devices" + discovered mice
        GtkStringList* mlist = gtk_string_list_new(nullptr);
        gtk_string_list_append(mlist, ui_text(S, "All devices (default)", "Tüm cihazlar (varsayılan)"));
        for (auto& m : S->mice_list) {
            // Display: "Name  [/dev/input/eventN]"
            std::string label = m.name + "  [" + m.event_node + "]";
            gtk_string_list_append(mlist, label.c_str());
        }

        S->device_id_combo = gtk_drop_down_new(G_LIST_MODEL(mlist), nullptr);
        gtk_widget_set_hexpand(S->device_id_combo, TRUE);
        g_signal_connect(S->device_id_combo, "notify::selected",
                         G_CALLBACK(on_notify_param_changed), S);

        GtkWidget* da_grid = append_grid();
        grid_row(S, da_grid, 0, "Mouse:", "Fare:", S->device_id_combo,
            "Select which physical mouse this profile applies to. All devices means fallback/default.",
            "Bu profilin uygulanacağı fiziksel fareyi seçer. Tüm cihazlar fallback/varsayılan anlamına gelir.");

        // Yenile butonu — fare listesini yeniden tara
        GtkWidget* refresh_btn = gtk_button_new_from_icon_name("view-refresh-symbolic");
        bind_tooltip(S, refresh_btn, "Rescan connected mice", "Bağlı fareleri yeniden tara");
        g_signal_connect(refresh_btn, "clicked",
            G_CALLBACK(+[](GtkWidget*, gpointer ud) {
                refresh_mice_combo(static_cast<AppState*>(ud), /*is_auto=*/false);
            }), S);
        gtk_grid_attach(GTK_GRID(da_grid), refresh_btn, 2, 0, 1, 1);

        // ── inotify: auto-detect /dev/input hot-plug events ───────────────
        S->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (S->inotify_fd >= 0) {
            S->inotify_wd = inotify_add_watch(S->inotify_fd, "/dev/input",
                                               IN_CREATE | IN_DELETE);
            if (S->inotify_wd >= 0) {
                GIOChannel* chan = g_io_channel_unix_new(S->inotify_fd);
                g_io_channel_set_encoding(chan, nullptr, nullptr); // binary
                g_io_channel_set_buffered(chan, FALSE);
                S->inotify_src = g_io_add_watch(chan, G_IO_IN,
                                                 on_inotify_event, S);
                g_io_channel_unref(chan);
            }
        }

        // Small informational label
        GtkWidget* hint = gtk_label_new(nullptr);
        bind_text(S, hint, UiTextKind::markup,
            "<small>This profile applies only to the selected mouse.\n"
            "The daemon uses the event node as device_id.</small>",
            "<small>Bu profil yalnızca seçilen fareye uygulanır.\n"
            "Daemon event node değerini device_id olarak kullanır.</small>");
        gtk_label_set_xalign(GTK_LABEL(hint), 0.0);
        gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
        gtk_widget_set_margin_top(hint, 2);
        gtk_box_append(GTK_BOX(lvbox), hint);
    }

    // ── KDE double-acceleration warning bar ──────────────────────────────────
    // Only constructed; visibility is set later in update_kde_warn_bar().
    {
        GtkWidget* warn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_start(warn_box, 8);
        gtk_widget_set_margin_end(warn_box, 8);
        gtk_widget_set_margin_top(warn_box, 4);
        gtk_widget_set_margin_bottom(warn_box, 2);

        // Orange warning background via CSS class
        GtkCssProvider* warn_css = gtk_css_provider_new();
        gtk_css_provider_load_from_string(warn_css,
            ".kde-warn-bar { background-color: #7a4000; border-radius: 6px; padding: 4px 8px; }"
            ".kde-warn-bar label { color: #ffcc80; }"
            ".kde-warn-bar button { background: #ff8c00; color: white; border-radius: 4px; }");
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(),
            GTK_STYLE_PROVIDER(warn_css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

        GtkWidget* warn_inner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_add_css_class(warn_inner, "kde-warn-bar");
        gtk_widget_set_hexpand(warn_inner, TRUE);

        GtkWidget* warn_icon = gtk_label_new("⚠");
        gtk_box_append(GTK_BOX(warn_inner), warn_icon);

        GtkWidget* warn_lbl = gtk_label_new(nullptr);
        bind_text(S, warn_lbl, UiTextKind::markup,
            "<b>KDE: Mouse acceleration is NOT disabled!</b>  "
            "KDE will apply its own curve on top of RawAccel → double acceleration.",
            "<b>KDE: Fare hızlandırması kapalı değil!</b>  "
            "KDE, RawAccel üzerine kendi eğrisini de uygular → çift hızlandırma.");
        gtk_label_set_wrap(GTK_LABEL(warn_lbl), TRUE);
        gtk_widget_set_hexpand(warn_lbl, TRUE);
        gtk_box_append(GTK_BOX(warn_inner), warn_lbl);

        GtkWidget* fix_btn = gtk_button_new_with_label("Fix Now");
        bind_text(S, fix_btn, UiTextKind::button, "Fix Now", "Şimdi Düzelt");
        bind_tooltip(S, fix_btn,
            "Sets PointerAccelerationProfile=Flat in ~/.config/kwinrc\n"
            "and reloads KWin input settings immediately (no logout needed).",
            "~/.config/kwinrc içinde PointerAccelerationProfile=Flat ayarlar\n"
            "ve KWin giriş ayarlarını hemen yeniler (oturumu kapatmak gerekmez).");
        g_signal_connect(fix_btn, "clicked", G_CALLBACK(on_kde_fix_clicked), S);
        gtk_box_append(GTK_BOX(warn_inner), fix_btn);

        GtkWidget* manual_btn = gtk_button_new_with_label("Manual");
        bind_text(S, manual_btn, UiTextKind::button, "Manual", "Manuel");
        bind_tooltip(S, manual_btn,
            "Open KDE System Settings → Input Devices → Mouse\n"
            "and set Pointer Acceleration to Flat.",
            "KDE Sistem Ayarları → Giriş Cihazları → Fare bölümünü açın\n"
            "ve Pointer Acceleration değerini Flat yapın.");
        g_signal_connect(manual_btn, "clicked", G_CALLBACK(on_kde_open_settings), nullptr);
        gtk_box_append(GTK_BOX(warn_inner), manual_btn);

        gtk_box_append(GTK_BOX(warn_box), warn_inner);
        S->kde_warn_bar = warn_box;
        gtk_widget_set_visible(warn_box, FALSE); // initially hidden; shown by update_kde_warn_bar
        gtk_box_append(GTK_BOX(lvbox), warn_box);
    }

    // ── RIGHT PANEL (Graph) ───────────────────────────────────────────────────
    GtkWidget* rvbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_hexpand(rvbox, TRUE);
    gtk_widget_set_vexpand(rvbox, TRUE);
    gtk_widget_set_margin_start(rvbox, 8);
    gtk_widget_set_margin_end(rvbox, 12);
    gtk_widget_set_margin_top(rvbox, 8);
    gtk_widget_set_margin_bottom(rvbox, 4);
    gtk_paned_set_end_child(GTK_PANED(hpaned), rvbox);

    GtkWidget* graph_lbl = gtk_label_new(nullptr);
    bind_text(S, graph_lbl, UiTextKind::markup,
        "<b>Gain Curve</b>  <small>(scroll = zoom)</small>",
        "<b>Gain Eğrisi</b>  <small>(kaydırma = zoom)</small>");
    gtk_label_set_xalign(GTK_LABEL(graph_lbl), 0.0);
    gtk_box_append(GTK_BOX(rvbox), graph_lbl);

    S->graph_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(S->graph_area, TRUE);
    gtk_widget_set_vexpand(S->graph_area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(S->graph_area),
                                   on_graph_draw, S, nullptr);

    // Scroll controller for zoom
    GtkEventController* scroll_ctrl =
        gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll_ctrl, "scroll", G_CALLBACK(on_graph_scroll), S);
    gtk_widget_add_controller(S->graph_area, scroll_ctrl);

    // ── LUT graph interaction: left click = add point, right click = remove ─
    // Use GRAPH_ML/MR/MT/MB constants for graph margins (single source of truth)

    // Left click: add a new point in LUT mode
    GtkGesture* lclick = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(lclick), 1);
    g_signal_connect(lclick, "pressed",
        G_CALLBACK(+[](GtkGestureClick*, int /*n*/, double cx, double cy, gpointer ud) {
            auto* S2 = static_cast<AppState*>(ud);
            if (!S2->lut_graph_mode) return;
            int w = gtk_widget_get_width(S2->graph_area);
            int h = gtk_widget_get_height(S2->graph_area);
            double PW = w - GRAPH_ML - GRAPH_MR;
            double PH = h - GRAPH_MT - GRAPH_MB;
            if (PW <= 0 || PH <= 0) return;
            if (cx < GRAPH_ML || cx > GRAPH_ML + PW || cy < GRAPH_MT || cy > GRAPH_MT + PH) return;

            double max_speed = 50.0 / S2->graph_zoom + S2->graph_pan_x;
            max_speed = std::max(max_speed, 5.0);

            double max_gain = compute_max_gain(S2, max_speed);

            double spd  = (cx - GRAPH_ML) / PW * max_speed;
            double gain = (1.0 - (cy - GRAPH_MT) / PH) * max_gain;
            spd  = std::max(0.0, spd);
            gain = std::max(0.01, gain);

            auto& ax = cur_prof(S2).prof.accel_x;
            if (ax.length / 2 >= (int)LUT_POINTS_CAPACITY) {
                set_status(S2, ui_text(S2, "Maximum number of points reached.", "Maksimum nokta sayısına ulaşıldı."));
                return;
            }
            auto pts = lut_get_points(ax);
            pts.push_back({spd, gain});
            lut_set_points(ax, pts);
            if (S2->xy_linked) cur_prof(S2).prof.accel_y = ax;
            rebuild_lut_list(S2);
            gtk_widget_queue_draw(S2->graph_area);
            set_status(S2, std::string(ui_text(S2, "LUT point added: speed=", "LUT noktası eklendi: hız=")) +
                       std::to_string((int)spd) + " gain=" +
                       std::to_string(gain).substr(0, 5));
        }), S);
    gtk_widget_add_controller(S->graph_area, GTK_EVENT_CONTROLLER(lclick));

    // Right click: remove the nearest point in LUT mode
    GtkGesture* rclick = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rclick), 3);
    g_signal_connect(rclick, "pressed",
        G_CALLBACK(+[](GtkGestureClick*, int /*n*/, double cx, double cy, gpointer ud) {
            auto* S2 = static_cast<AppState*>(ud);
            if (!S2->lut_graph_mode) return;
            int w = gtk_widget_get_width(S2->graph_area);
            int h = gtk_widget_get_height(S2->graph_area);
            double PW = w - GRAPH_ML - GRAPH_MR;
            double PH = h - GRAPH_MT - GRAPH_MB;
            if (PW <= 0 || PH <= 0) return;

            double max_speed = 50.0 / S2->graph_zoom + S2->graph_pan_x;
            max_speed = std::max(max_speed, 5.0);
            double max_gain = compute_max_gain(S2, max_speed);

            auto& ax = cur_prof(S2).prof.accel_x;
            auto  pts = lut_get_points(ax);
            if (pts.empty()) return;

            // Find the LUT point nearest to the clicked pixel position
            int best_idx = -1;
            double best_dist2 = 20.0 * 20.0; // 20px threshold
            for (int i = 0; i < (int)pts.size(); i++) {
                double px = GRAPH_ML + (pts[i].first  / max_speed) * PW;
                double py = GRAPH_MT + PH - (pts[i].second / max_gain) * PH;
                double d2 = (cx - px) * (cx - px) + (cy - py) * (cy - py);
                if (d2 < best_dist2) { best_dist2 = d2; best_idx = i; }
            }
            if (best_idx < 0) return;
            pts.erase(pts.begin() + best_idx);
            lut_set_points(ax, pts);
            if (S2->xy_linked) cur_prof(S2).prof.accel_y = ax;
            rebuild_lut_list(S2);
            gtk_widget_queue_draw(S2->graph_area);
            set_status(S2, ui_text(S2, "LUT point removed.", "LUT noktası silindi."));
        }), S);
    gtk_widget_add_controller(S->graph_area, GTK_EVENT_CONTROLLER(rclick));

    // Left-drag = pan (disabled in LUT mode — left click adds points there)
    GtkGesture* drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), 1);
    g_signal_connect(drag, "drag-begin",  G_CALLBACK(on_graph_drag_begin),  S);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_graph_drag_update), S);
    g_signal_connect(drag, "drag-end",    G_CALLBACK(on_graph_drag_end),    S);
    gtk_widget_add_controller(S->graph_area, GTK_EVENT_CONTROLLER(drag));

    // Fare hareketi = LUT nokta hover cursor
    GtkEventController* motion_ctrl = gtk_event_controller_motion_new();
    g_signal_connect(motion_ctrl, "motion", G_CALLBACK(on_graph_motion), S);
    gtk_widget_add_controller(S->graph_area, motion_ctrl);

    gtk_box_append(GTK_BOX(rvbox), S->graph_area);

    // ── Status bar ────────────────────────────────────────────────────────────
    GtkWidget* status_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(status_hbox, 10);
    gtk_widget_set_margin_bottom(status_hbox, 4);
    gtk_widget_set_margin_top(status_hbox, 2);

    S->status_bar = gtk_label_new("Ready.");
    bind_text(S, S->status_bar, UiTextKind::label, "Ready.", "Hazır.");
    gtk_label_set_xalign(GTK_LABEL(S->status_bar), 0.0);
    gtk_widget_set_hexpand(S->status_bar, TRUE);
    gtk_box_append(GTK_BOX(status_hbox), S->status_bar);

    gtk_box_append(GTK_BOX(outer_vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(outer_vbox), status_hbox);

    // ── Init ──────────────────────────────────────────────────────────────────
    profile_to_widgets(S);
    update_daemon_status(S);
    update_kde_warn_bar(S);   // KDE: show warning if libinput acceleration is not disabled

    // Poll daemon status every 3s — pass S as user_data
    S->daemon_poll_id = g_timeout_add_seconds(3, poll_daemon_status, S);

    // Prompt before closing if there are unsaved changes
    g_signal_connect(S->window, "close-request",
                     G_CALLBACK(on_window_close_request), S);

    // Cancel the poll timer when the window is destroyed (prevents stale S->window access)
    g_signal_connect(S->window, "destroy",
        G_CALLBACK(+[](GtkWidget*, gpointer ud) {
            auto* S2 = static_cast<AppState*>(ud);
            if (S2->daemon_poll_id) {
                g_source_remove(S2->daemon_poll_id);
                S2->daemon_poll_id = 0;
            }
            // clean up the inotify source
            if (S2->inotify_src) {
                g_source_remove(S2->inotify_src);
                S2->inotify_src = 0;
            }
            if (S2->inotify_wd >= 0) {
                inotify_rm_watch(S2->inotify_fd, S2->inotify_wd);
                S2->inotify_wd = -1;
            }
            if (S2->inotify_fd >= 0) {
                close(S2->inotify_fd);
                S2->inotify_fd = -1;
            }
        }), S);

    gtk_window_present(GTK_WINDOW(S->window));
}

// ── Window close confirmation ─────────────────────────────────────────────

/// GTK4: returning TRUE from close-request prevents the window from closing.
/// Show a confirmation dialog if there are unsaved changes.
gboolean on_window_close_request(GtkWindow* win, gpointer user_data) {
    auto* S = static_cast<AppState*>(user_data);
    if (!S->unsaved) return FALSE; // no unsaved changes — close immediately

    GtkWidget* dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg), ui_text(S, "Unsaved Changes", "Kaydedilmemiş Değişiklikler"));
    gtk_window_set_transient_for(GTK_WINDOW(dlg), win);
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 320, -1);

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(vbox, 16); gtk_widget_set_margin_end(vbox, 16);
    gtk_widget_set_margin_top(vbox, 16);   gtk_widget_set_margin_bottom(vbox, 16);
    gtk_window_set_child(GTK_WINDOW(dlg), vbox);

    GtkWidget* lbl = gtk_label_new(ui_text(S,
        "You have unsaved changes.\nDo you want to quit?",
        "Kaydedilmemiş değişiklikler var.\nÇıkmak istiyor musunuz?"));
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
    gtk_box_append(GTK_BOX(vbox), lbl);

    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(hbox, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(vbox), hbox);

    GtkWidget* cancel_btn  = gtk_button_new_with_label("Cancel");
    GtkWidget* discard_btn = gtk_button_new_with_label("Quit Without Saving");
    GtkWidget* save_btn    = gtk_button_new_with_label("Save and Quit");
    gtk_button_set_label(GTK_BUTTON(cancel_btn), ui_text(S, "Cancel", "İptal"));
    gtk_button_set_label(GTK_BUTTON(discard_btn), ui_text(S, "Quit Without Saving", "Kaydetmeden Çık"));
    gtk_button_set_label(GTK_BUTTON(save_btn), ui_text(S, "Save and Quit", "Kaydet ve Çık"));
    gtk_widget_add_css_class(save_btn,    "suggested-action");
    gtk_widget_add_css_class(discard_btn, "destructive-action");
    gtk_box_append(GTK_BOX(hbox), cancel_btn);
    gtk_box_append(GTK_BOX(hbox), discard_btn);
    gtk_box_append(GTK_BOX(hbox), save_btn);

    // Cancel: close the dialog, leave the window open
    g_signal_connect(cancel_btn, "clicked",
        G_CALLBACK(+[](GtkWidget*, gpointer d) {
            gtk_window_destroy(GTK_WINDOW(d));
        }), dlg);

    // Quit without saving: close both dialog and main window
    // Store AppState* in dialog for the lambda
    g_object_set_data(G_OBJECT(dlg), "app-state", S);
    g_signal_connect(discard_btn, "clicked",
        G_CALLBACK(+[](GtkWidget*, gpointer d) {
            auto* S2 = static_cast<AppState*>(g_object_get_data(G_OBJECT(d), "app-state"));
            gtk_window_destroy(GTK_WINDOW(d));
            S2->unsaved = false;
            gtk_window_destroy(GTK_WINDOW(S2->window));
        }), dlg);

    // Save and quit
    g_signal_connect(save_btn, "clicked",
        G_CALLBACK(+[](GtkWidget*, gpointer d) {
            auto* S2 = static_cast<AppState*>(g_object_get_data(G_OBJECT(d), "app-state"));
            gtk_window_destroy(GTK_WINDOW(d));
            save_config_now(S2);
            gtk_window_destroy(GTK_WINDOW(S2->window));
        }), dlg);

    gtk_window_present(GTK_WINDOW(dlg));
    return TRUE; // prevent close; window stays open until the dialog is dismissed
}

// ── App activate ─────────────────────────────────────────────────────────────

/// Register application-level keyboard shortcuts via GAction.
/// Ctrl+S = Save, Ctrl+N = New profile, Ctrl+D = Duplicate profile,
/// Ctrl+R = Reload daemon, F5 = Refresh device list.
static void register_shortcuts(AppState* S, GtkApplication* gapp) {
    // Ctrl+S — Save
    {
        GSimpleAction* a = g_simple_action_new("save", nullptr);
        g_signal_connect(a, "activate",
            G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer ud) {
                on_save_clicked(nullptr, ud);
            }), S);
        g_action_map_add_action(G_ACTION_MAP(gapp), G_ACTION(a));
        const char* accels[] = {"<Control>s", nullptr};
        gtk_application_set_accels_for_action(gapp, "app.save", accels);
    }
    // Ctrl+N — New profile
    {
        GSimpleAction* a = g_simple_action_new("new-profile", nullptr);
        g_signal_connect(a, "activate",
            G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer ud) {
                on_new_profile(nullptr, ud);
            }), S);
        g_action_map_add_action(G_ACTION_MAP(gapp), G_ACTION(a));
        const char* accels[] = {"<Control>n", nullptr};
        gtk_application_set_accels_for_action(gapp, "app.new-profile", accels);
    }
    // Ctrl+D — Duplicate profile
    {
        GSimpleAction* a = g_simple_action_new("dup-profile", nullptr);
        g_signal_connect(a, "activate",
            G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer ud) {
                on_duplicate_profile(nullptr, ud);
            }), S);
        g_action_map_add_action(G_ACTION_MAP(gapp), G_ACTION(a));
        const char* accels[] = {"<Control>d", nullptr};
        gtk_application_set_accels_for_action(gapp, "app.dup-profile", accels);
    }
    // Ctrl+R — Reload daemon
    {
        GSimpleAction* a = g_simple_action_new("reload-daemon", nullptr);
        g_signal_connect(a, "activate",
            G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer ud) {
                on_daemon_reload(nullptr, ud);
            }), S);
        g_action_map_add_action(G_ACTION_MAP(gapp), G_ACTION(a));
        const char* accels[] = {"<Control>r", nullptr};
        gtk_application_set_accels_for_action(gapp, "app.reload-daemon", accels);
    }
    // F5 — Refresh device list
    {
        GSimpleAction* a = g_simple_action_new("refresh-devices", nullptr);
        g_signal_connect(a, "activate",
            G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer ud) {
                refresh_mice_combo(static_cast<AppState*>(ud), false);
            }), S);
        g_action_map_add_action(G_ACTION_MAP(gapp), G_ACTION(a));
        const char* accels[] = {"F5", nullptr};
        gtk_application_set_accels_for_action(gapp, "app.refresh-devices", accels);
    }
}

// ── KDE double-acceleration fix helpers ──────────────────────────────────────

/// Enumerate currently-active RawAccel virtual mouse devices from
/// /proc/bus/input/devices.  Returns a vector of (bus, vendor, product, name).
/// Vendor/product/bus are decimal (Plasma stores them in decimal in kwinrc).
struct rawaccel_dev_t {
    int bus, vendor, product;
    std::string name;
};
static std::vector<rawaccel_dev_t> kde_enumerate_rawaccel_devices() {
    std::vector<rawaccel_dev_t> out;
    FILE* f = fopen("/proc/bus/input/devices", "r");
    if (!f) return out;
    char line[1024];
    rawaccel_dev_t cur{};
    bool have_id = false;
    auto flush_block = [&]() {
        if (have_id && cur.name.size() >= 10 &&
            cur.name.compare(cur.name.size() - 10, 10, "(RawAccel)") == 0) {
            out.push_back(cur);
        }
        cur = rawaccel_dev_t{};
        have_id = false;
    };
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        if (n == 0) { flush_block(); continue; }
        // I: Bus=0003 Vendor=046d Product=c542 Version=0111
        if (line[0] == 'I' && line[1] == ':') {
            unsigned bus = 0, ven = 0, prod = 0;
            auto parse_hex_field = [&](const char* key, unsigned& out) {
                const char* p = std::strstr(line, key);
                if (!p) return false;
                p += std::strlen(key);
                char* end = nullptr;
                errno = 0;
                unsigned long v = std::strtoul(p, &end, 16);
                if (end == p || errno == ERANGE ||
                    v > std::numeric_limits<unsigned>::max())
                    return false;
                out = static_cast<unsigned>(v);
                return true;
            };
            if (parse_hex_field("Bus=", bus) &&
                parse_hex_field("Vendor=", ven) &&
                parse_hex_field("Product=", prod)) {
                cur.bus = (int)bus; cur.vendor = (int)ven; cur.product = (int)prod;
                have_id = true;
            }
        } else if (line[0] == 'N' && strncmp(line, "N: Name=\"", 9) == 0) {
            std::string s(line + 9);
            if (!s.empty() && s.back() == '"') s.pop_back();
            cur.name = s;
        }
    }
    flush_block();
    fclose(f);
    return out;
}

/// Replace or append a section [header] with the given key=value lines.
/// `header` includes the brackets, e.g. "[Libinput]" or
/// "[Libinput][3][1133][50498][Logitech ... (RawAccel)]".
/// Section ends at the next line starting with '['.
static void kde_upsert_section(std::vector<std::string>& lines,
                               const std::string& header,
                               const std::vector<std::pair<std::string, std::string>>& kv) {
    // Find existing section
    size_t start = std::string::npos;
    for (size_t i = 0; i < lines.size(); i++) {
        if (lines[i] == header) { start = i; break; }
    }
    std::vector<std::string> body;
    body.reserve(kv.size());
    for (auto& [k, v] : kv) body.push_back(k + "=" + v);

    if (start == std::string::npos) {
        // Append at end (with blank separator if file isn't empty)
        if (!lines.empty() && !lines.back().empty()) lines.emplace_back();
        lines.push_back(header);
        for (auto& l : body) lines.push_back(l);
        return;
    }
    // Find end of section (next '[' line or EOF)
    size_t end = start + 1;
    while (end < lines.size() && (lines[end].empty() || lines[end][0] != '[')) end++;
    // Replace [start+1, end) with body
    lines.erase(lines.begin() + (long)start + 1, lines.begin() + (long)end);
    lines.insert(lines.begin() + (long)start + 1, body.begin(), body.end());
}

/// Atomically write a kwinrc-style INI file (with possibly nested sections)
/// from the in-memory line buffer.
///
/// BUG-8: previously fputs/fputc/fclose return values were ignored — a full
/// disk (ENOSPC) or I/O error could silently truncate the temp file, then
/// rename() would publish the corrupt content as the user's kwinrc.  Now we
/// detect any write/flush failure, drop the temp file, and signal an error
/// to the caller so the original kwinrc is preserved.
static bool kde_atomic_write(const std::string& path,
                             const std::vector<std::string>& lines) {
    std::string tmp = path + ".tmp";
    FILE* fw = fopen(tmp.c_str(), "w");
    if (!fw) return false;
    for (auto& l : lines) {
        if (fputs(l.c_str(), fw) == EOF || fputc('\n', fw) == EOF) {
            fclose(fw); unlink(tmp.c_str());
            return false;
        }
    }
    if (fflush(fw) != 0 || ferror(fw) || fclose(fw) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

/// Read all lines (without trailing CR/LF) from path. Empty vector if missing.
static std::vector<std::string> kde_read_lines(const std::string& path) {
    std::vector<std::string> lines;
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return lines;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        lines.emplace_back(line);
    }
    fclose(f);
    return lines;
}

/// Resolve $XDG_CONFIG_HOME/<name> or ~/.config/<name>.
static std::string kde_xdg_path(const char* name) {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0] != '\0') return std::string(xdg) + "/" + name;
    const char* home = std::getenv("HOME");
    if (!home || home[0] == '\0') return {};
    return std::string(home) + "/.config/" + name;
}

/// Write [Libinput] global + per-device overrides into kwinrc and kcminputrc,
/// with the requested acceleration profile (1 = Flat, 2 = Adaptive).
///
/// KEY INSIGHT — Plasma 6 split-brain storage:
///   • kwinrc    → KWin compositor reads on startup/reconfigure
///                 Section format includes bus: [Libinput][bus][vid][pid][Name]
///   • kcminputrc → System Settings KCM (kcm_mouse) reads + applies via libinput
///                 Section format omits bus: [Libinput][vid][pid][Name]
///                 Only key written is `PointerAccelerationProfile`
/// Manual toggle in System Settings only updates kcminputrc and calls
/// libinput's runtime API directly. Writing kwinrc alone is not enough on
/// Plasma 6 — we MUST also write kcminputrc and trigger KCM init.
static bool kde_write_kwinrc_accel(const char* profile, const char* accel) {
    auto devs = kde_enumerate_rawaccel_devices();

    // ── kwinrc (compositor-level config) ─────────────────────────────────────
    std::string kwinrc_path = kde_xdg_path("kwinrc");
    if (kwinrc_path.empty()) return false;
    auto kwinrc_lines = kde_read_lines(kwinrc_path);

    const std::vector<std::pair<std::string, std::string>> kv_kwin = {
        {"PointerAccelerationProfile", profile},
        {"PointerAcceleration",        accel},
    };
    kde_upsert_section(kwinrc_lines, "[Libinput]", kv_kwin);
    for (auto& d : devs) {
        std::string header = "[Libinput][" + std::to_string(d.bus) + "][" +
                             std::to_string(d.vendor) + "][" +
                             std::to_string(d.product) + "][" + d.name + "]";
        kde_upsert_section(kwinrc_lines, header, kv_kwin);
    }
    if (!kde_atomic_write(kwinrc_path, kwinrc_lines)) return false;

    // ── kcminputrc (KCM-applied per-device config — the one libinput uses) ──
    std::string kcm_path = kde_xdg_path("kcminputrc");
    if (kcm_path.empty()) return true; // kwinrc done — kcminputrc is bonus
    auto kcm_lines = kde_read_lines(kcm_path);

    // KCM only writes the profile key — match its format exactly.
    const std::vector<std::pair<std::string, std::string>> kv_kcm = {
        {"PointerAccelerationProfile", profile},
    };
    for (auto& d : devs) {
        // KCM section header omits the bus index.
        std::string header = "[Libinput][" + std::to_string(d.vendor) + "][" +
                             std::to_string(d.product) + "][" + d.name + "]";
        kde_upsert_section(kcm_lines, header, kv_kcm);
    }
    kde_atomic_write(kcm_path, kcm_lines); // best-effort
    return true;
}

// Forward declaration — defined below
static void kde_reload_input_settings();

/// Apply Flat acceleration (no acceleration) to KDE Plasma libinput config
/// for the global section AND every active (RawAccel) virtual mouse.
///
/// IMPLEMENTATION NOTE — KWin per-device libinput "toggle dance":
/// Empirically on Plasma 6, writing kwinrc + `KWin reconfigure` does NOT
/// always re-apply per-device libinput settings to a device that's already
/// running. The KCM (System Settings → Mouse) bypasses kwinrc entirely and
/// calls libinput's runtime API; toggling the checkbox there fixes it.
/// To replicate that "applied" state without user interaction we:
///   1. Write Adaptive (profile=2) → reconfigure → small sleep
///   2. Write Flat     (profile=1) → reconfigure
/// KWin observes a real change between snapshots and forces re-application.
/// Idempotent: the final on-disk state is always Flat.
///
/// Generic across any mouse / any host (vendor/product/name read from kernel).
static bool kde_write_flat_accel() {
    // Step 1: temporarily set Adaptive so the next reconfigure sees a delta
    if (!kde_write_kwinrc_accel("2", "0")) return false;
    kde_reload_input_settings();
    // Brief pause so KWin processes the first reconfigure before the second.
    // 250 ms is enough on every machine I've tested without making startup
    // noticeably slower.
    struct timespec ts = { 0, 250 * 1000 * 1000 };
    nanosleep(&ts, nullptr);
    // Step 2: settle on Flat — this is the kept state.
    return kde_write_kwinrc_accel("1", "0");
}

/// Run a single command via fork+exec, waiting for completion. Returns true
/// if the child exited with status 0.
static bool kde_run_cmd(const char* const* argv) {
    pid_t pid = fork();
    if (pid == 0) {
        execvp(argv[0], const_cast<char* const*>(argv));
        _exit(127);
    }
    if (pid < 0) return false;
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/// Reload KDE input settings at runtime: tells KWin compositor to re-read
/// kwinrc AND tells KCM to re-apply kcminputrc to libinput devices. Both are
/// needed on Plasma 6 (split-brain config — see kde_write_kwinrc_accel).
static void kde_reload_input_settings() {
    // 1) Compositor: qdbus reconfigure (Plasma 6 first, then 5 fallback)
    {
        const char* a1[] = {"qdbus6", "org.kde.KWin", "/KWin", "reconfigure", nullptr};
        const char* a2[] = {"qdbus",  "org.kde.KWin", "/KWin", "reconfigure", nullptr};
        if (!kde_run_cmd(a1)) kde_run_cmd(a2);
    }
    // 2) KCM init: forces kcm_mouse plugin to read kcminputrc and call
    //    libinput's runtime API on every active device — replicates the
    //    "click → apply → close" the user does manually in System Settings.
    {
        const char* args[] = {"kcminit", "kcm_mouse", nullptr};
        kde_run_cmd(args);
    }
}

/// Callback: "Manual" button — opens KDE System Settings mouse page.
static void on_kde_open_settings(GtkWidget*, gpointer) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        // Try the specific KCM page first (Plasma 5 & 6)
        const char* args[] = {"systemsettings", "kcm_mouse", nullptr};
        execvp(args[0], const_cast<char* const*>(args));
        // Fallback: open plain systemsettings
        const char* fb[] = {"systemsettings", nullptr};
        execvp(fb[0], const_cast<char* const*>(fb));
        _exit(0);
    } else if (pid > 0) {
        // Reap zombie asynchronously
        pid_t* pp = new pid_t(pid);
        g_timeout_add(5000, [](gpointer p) -> gboolean {
            waitpid(*static_cast<pid_t*>(p), nullptr, WNOHANG);
            delete static_cast<pid_t*>(p);
            return G_SOURCE_REMOVE;
        }, pp);
    }
}

/// Callback: "Fix Now" button in the KDE warning bar.
static void on_kde_fix_clicked(GtkButton*, gpointer user_data) {
    auto* S = static_cast<AppState*>(user_data);
    bool ok = kde_write_flat_accel();
    if (ok) {
        kde_reload_input_settings();
        S->kde_accel_ok = true;
        // Hide the warning bar
        if (S->kde_warn_bar) gtk_widget_set_visible(S->kde_warn_bar, FALSE);
        set_status(S, ui_text(S, "KDE: libinput acceleration disabled. Changes applied immediately.",
                              "KDE: libinput hızlandırması kapatıldı. Değişiklikler hemen uygulandı."));
    } else {
        set_status(S, ui_text(S, "KDE: Could not write to kwinrc. Edit manually: System Settings → Input Devices → Mouse → Pointer Acceleration = Flat.",
                              "KDE: kwinrc yazılamadı. Manuel düzenleyin: Sistem Ayarları → Giriş Cihazları → Fare → Pointer Acceleration = Flat."));
    }
}

/// Check KDE state and show/hide the warning bar.
static void update_kde_warn_bar(AppState* S) {
    if (!S->kde_warn_bar) return;
    if (!S->is_kde) { gtk_widget_set_visible(S->kde_warn_bar, FALSE); return; }
    int state = kde_libinput_accel_state();
    // state == 0 → flat (OK); state == 1 → adaptive (bad); -1 → unknown
    bool bad = (state == 1);
    S->kde_accel_ok = !bad;
    gtk_widget_set_visible(S->kde_warn_bar, bad ? TRUE : FALSE);
}

void on_activate(GtkApplication* gapp, gpointer user_data) {
    auto* S = static_cast<AppState*>(user_data);

    // Detect KDE / Wayland upfront
    S->is_kde     = is_kde_session();
    S->is_wayland = is_wayland_session();

    // Dark CSS
    GtkCssProvider* css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        "window, .background { background-color: #1b1b1e; }"
        "label { color: #dcdcdf; }"
        "frame { border-radius: 6px; }"
        "spinbutton { min-width: 96px; color: #dcdcdf; }"
        "entry { color: #dcdcdf; }"
        "button.suggested-action { background: #0078d4; color: white; }"
        "button.destructive-action { background: #c0392b; color: white; }"
        "separator { background-color: #333336; min-height: 1px; }"
        ".sidebar { background-color: #141416; }"
    );
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    register_shortcuts(S, gapp);
    build_ui(S, gapp);

    // Warn on startup if multiple profiles share the same device_id
    std::string dup_warn = check_duplicate_device_ids(S->config);
    if (!dup_warn.empty())
        set_status(S, dup_warn);
}

// ── main ─────────────────────────────────────────────────────────────────────
