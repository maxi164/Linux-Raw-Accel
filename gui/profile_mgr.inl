// ── Profile management: CRUD dialogs (new / rename / delete / duplicate / reset) ──

void rebuild_profile_combo(AppState* S) {
    // Block on_profile_changed while we rebuild the model to avoid double-update
    S->updating = true;

    GtkStringList* sl = gtk_string_list_new(nullptr);
    for (auto& p : S->config.profiles) {
        // Mark the active profile with ★ so it's visible at a glance
        std::string label = p.name;
        if (p.name == S->config.active_profile)
            label = "\xe2\x98\x85 " + p.name;  // UTF-8 ★
        gtk_string_list_append(sl, label.c_str());
    }
    gtk_drop_down_set_model(GTK_DROP_DOWN(S->profile_combo), G_LIST_MODEL(sl));

    int idx = S->current_profile_idx;
    if (idx >= (int)S->config.profiles.size())
        idx = (int)S->config.profiles.size() - 1;
    S->current_profile_idx = std::max(0, idx);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(S->profile_combo),
                               S->current_profile_idx);

    S->updating = false;
    profile_to_widgets(S);
}

void show_input_dialog(AppState* S,
                              const char* title, const char* placeholder,
                              const char* initial,
                              std::function<void(const std::string&)> cb) {
    GtkWidget* dlg  = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg), title);
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(S->window));
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 320, -1);

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(vbox, 16);
    gtk_widget_set_margin_end(vbox, 16);
    gtk_widget_set_margin_top(vbox, 16);
    gtk_widget_set_margin_bottom(vbox, 16);
    gtk_window_set_child(GTK_WINDOW(dlg), vbox);

    GtkWidget* entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder);
    if (initial && strlen(initial) > 0)
        gtk_editable_set_text(GTK_EDITABLE(entry), initial);
    gtk_box_append(GTK_BOX(vbox), entry);

    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(hbox, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(vbox), hbox);

    GtkWidget* cancel_btn = gtk_button_new_with_label("Cancel");
    GtkWidget* ok_btn     = gtk_button_new_with_label("OK");
    gtk_widget_add_css_class(ok_btn, "suggested-action");
    gtk_box_append(GTK_BOX(hbox), cancel_btn);
    gtk_box_append(GTK_BOX(hbox), ok_btn);

    // Store callback in a heap-allocated wrapper
    auto* cb_ptr = new std::function<void(const std::string&)>(cb);
    g_object_set_data_full(G_OBJECT(dlg), "cb", cb_ptr,
                           [](gpointer p) { delete (std::function<void(const std::string&)>*)p; });
    g_object_set_data(G_OBJECT(dlg), "entry", entry);

    auto do_ok = +[](GtkWidget*, gpointer dlg_ptr) {
        GtkWidget* e = GTK_WIDGET(g_object_get_data(G_OBJECT(dlg_ptr), "entry"));
        auto* cb_p   = (std::function<void(const std::string&)>*)
                        g_object_get_data(G_OBJECT(dlg_ptr), "cb");
        std::string val = gtk_editable_get_text(GTK_EDITABLE(e));
        if (!val.empty() && cb_p) (*cb_p)(val);
        gtk_window_destroy(GTK_WINDOW(dlg_ptr));
    };

    g_signal_connect(ok_btn,     "clicked", G_CALLBACK(do_ok),   dlg);
    g_signal_connect(cancel_btn, "clicked",
        G_CALLBACK(+[](GtkWidget*, gpointer d){ gtk_window_destroy(GTK_WINDOW(d)); }), dlg);

    // Enter key in entry = ok
    g_signal_connect(entry, "activate", G_CALLBACK(do_ok), dlg);

    gtk_window_present(GTK_WINDOW(dlg));
}

void on_new_profile(GtkButton*, gpointer user_data) {
    auto* S = static_cast<AppState*>(user_data);
    show_input_dialog(S, "New Profile", "Profile name (e.g. gaming)", "",
        [S](const std::string& name) {
            // Check duplicate
            for (auto& p : S->config.profiles)
                if (p.name == name) { set_status(S, "A profile with that name already exists."); return; }
            device_profile dp;
            dp.name = name;
            dp.dev_cfg.dpi = 800;
            dp.dev_cfg.polling_rate = 1000;
            S->config.profiles.push_back(dp);
            S->current_profile_idx = (int)S->config.profiles.size() - 1;
            S->config.active_profile = name;
            rebuild_profile_combo(S);
            save_config_now(S);
        });
}

void on_rename_profile(GtkButton*, gpointer user_data) {
    auto* S = static_cast<AppState*>(user_data);
    if (S->config.profiles.empty()) return;
    std::string old_name = cur_prof(S).name;
    show_input_dialog(S, "Rename Profile", "New name", old_name.c_str(),
        [S, old_name](const std::string& name) {
            if (name.empty()) return;
            // Duplicate check
            for (auto& p : S->config.profiles)
                if (p.name == name && p.name != old_name) {
                    set_status(S, "A profile with that name already exists.");
                    return;
                }
            cur_prof(S).name = name;
            // Update active_profile if we renamed the active one
            if (S->config.active_profile == old_name)
                S->config.active_profile = name;
            rebuild_profile_combo(S);
            save_config_now(S);
        });
}

void on_delete_profile(GtkButton*, gpointer user_data) {
    auto* S = static_cast<AppState*>(user_data);
    if (S->config.profiles.size() <= 1) {
        set_status(S, "At least one profile is required.");
        return;
    }

    // Confirm dialog
    GtkWidget* dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg), "Delete Profile");
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(S->window));
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 300, -1);

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(vbox, 16); gtk_widget_set_margin_end(vbox, 16);
    gtk_widget_set_margin_top(vbox, 16);   gtk_widget_set_margin_bottom(vbox, 16);
    gtk_window_set_child(GTK_WINDOW(dlg), vbox);

    std::string msg = "Delete profile \"" + cur_prof(S).name + "\"?";
    GtkWidget* lbl = gtk_label_new(msg.c_str());
    gtk_box_append(GTK_BOX(vbox), lbl);

    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(hbox, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(vbox), hbox);

    GtkWidget* cancel_btn = gtk_button_new_with_label("Cancel");
    GtkWidget* del_btn    = gtk_button_new_with_label("Delete");
    gtk_widget_add_css_class(del_btn, "destructive-action");
    gtk_box_append(GTK_BOX(hbox), cancel_btn);
    gtk_box_append(GTK_BOX(hbox), del_btn);

    g_signal_connect(cancel_btn, "clicked",
        G_CALLBACK(+[](GtkWidget*, gpointer d){ gtk_window_destroy(GTK_WINDOW(d)); }), dlg);

    // Store AppState* in the dialog so the lambda can access it
    g_object_set_data(G_OBJECT(dlg), "app-state", S);
    g_signal_connect(del_btn, "clicked", G_CALLBACK(+[](GtkWidget*, gpointer d) {
        auto* dlg_w = GTK_WIDGET(d);
        auto* S2 = static_cast<AppState*>(g_object_get_data(G_OBJECT(dlg_w), "app-state"));
        int idx = S2->current_profile_idx;
        S2->config.profiles.erase(S2->config.profiles.begin() + idx);
        S2->current_profile_idx = std::max(0, idx - 1);
        S2->config.active_profile = S2->config.profiles[S2->current_profile_idx].name;
        rebuild_profile_combo(S2);
        save_config_now(S2);
        gtk_window_destroy(GTK_WINDOW(d));
    }), dlg);

    gtk_window_present(GTK_WINDOW(dlg));
}

void on_duplicate_profile(GtkButton*, gpointer user_data) {
    auto* S = static_cast<AppState*>(user_data);
    if (S->config.profiles.empty()) return;
    device_profile copy = cur_prof(S);
    copy.name += " (copy)";
    S->config.profiles.push_back(copy);
    S->current_profile_idx = (int)S->config.profiles.size() - 1;
    rebuild_profile_combo(S);
    save_config_now(S);
}

void on_reset_profile(GtkButton*, gpointer user_data) {
    auto* S = static_cast<AppState*>(user_data);
    if (S->config.profiles.empty()) return;

    // Confirm dialog
    GtkWidget* dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg), "Reset to Defaults");
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(S->window));
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 320, -1);

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(vbox, 16); gtk_widget_set_margin_end(vbox, 16);
    gtk_widget_set_margin_top(vbox, 16);   gtk_widget_set_margin_bottom(vbox, 16);
    gtk_window_set_child(GTK_WINDOW(dlg), vbox);

    std::string msg = "Reset \"" + cur_prof(S).name + "\" to default values?\n"
                      "This cannot be undone.";
    GtkWidget* lbl = gtk_label_new(msg.c_str());
    gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
    gtk_box_append(GTK_BOX(vbox), lbl);

    GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(hbox, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(vbox), hbox);

    GtkWidget* cancel_btn = gtk_button_new_with_label("Cancel");
    GtkWidget* reset_btn  = gtk_button_new_with_label("Reset");
    gtk_widget_add_css_class(reset_btn, "destructive-action");
    gtk_box_append(GTK_BOX(hbox), cancel_btn);
    gtk_box_append(GTK_BOX(hbox), reset_btn);

    g_signal_connect(cancel_btn, "clicked",
        G_CALLBACK(+[](GtkWidget*, gpointer d){ gtk_window_destroy(GTK_WINDOW(d)); }), dlg);

    // Store AppState* in the dialog for the reset lambda
    g_object_set_data(G_OBJECT(dlg), "app-state", S);
    g_signal_connect(reset_btn, "clicked", G_CALLBACK(+[](GtkWidget*, gpointer d) {
        auto* dlg_w = GTK_WIDGET(d);
        auto* S2 = static_cast<AppState*>(g_object_get_data(G_OBJECT(dlg_w), "app-state"));
        // Keep name and device_id, reset everything else to defaults
        std::string saved_name = cur_prof(S2).name;
        std::string saved_id   = cur_prof(S2).device_id;
        device_profile fresh;
        fresh.name      = saved_name;
        fresh.device_id = saved_id;
        S2->config.profiles[S2->current_profile_idx] = fresh;
        profile_to_widgets(S2);
        S2->unsaved = true;
        set_status(S2, "Profile reset to defaults — press Save to keep.");
        gtk_window_destroy(GTK_WINDOW(d));
    }), dlg);

    gtk_window_present(GTK_WINDOW(dlg));
}
