// ── Graph rendering (Cairo) and LUT editor ────────────────────────────────────

// Forward declaration — lut_get_points() is defined later in this file.
static std::vector<std::pair<double,double>> lut_get_points(const accel_args& ax);

/// Compute the maximum gain for the graph Y-axis given the current profile and zoom/pan.
/// All graph draw and gesture handlers use this function — no duplication.
static double compute_max_gain(AppState* S, double max_speed) {
    double max_gain = 2.0;
    auto& dp = cur_prof(S);
    auto check = [&](const accel_args& args) {
        accel_union au; au.init(args);
        for (int i = 1; i <= 200; i++) {
            double s = max_speed * i / 200.0;
            double g = au.apply(s, args);
            if (std::isfinite(g) && g > max_gain) max_gain = g;
        }
        // In LUT mode: sampling may miss the actual points —
        // check all LUT gain values directly.
        if (args.mode == accel_mode::lookup) {
            int n = args.length / 2;
            for (int i = 0; i < n; i++) {
                double g = static_cast<double>(args.data[i * 2 + 1]);
                if (std::isfinite(g) && g > max_gain) max_gain = g;
            }
        }
    };
    check(dp.prof.accel_x);
    if (!S->xy_linked) check(dp.prof.accel_y);
    max_gain = std::ceil(max_gain * 1.2 * 10.0) / 10.0;
    return std::max(max_gain, 2.0);
}

/// Compute common constants for graph coordinate conversion.
/// ML/MR/MT/MB are the graph margins (same values as in on_graph_draw).

// ── Curve computation ─────────────────────────────────────────────────────────

struct CurvePt { double speed, gain; };

static std::vector<CurvePt> compute_curve(const accel_args& args,
                                           double max_speed, int N = 600) {
    accel_union au;
    au.init(args);
    std::vector<CurvePt> pts;
    pts.reserve(N + 1);
    for (int i = 0; i <= N; i++) {
        double s = max_speed * i / N;
        double g = au.apply(s, args);
        // Defensive: NaN/Inf gain feeds straight into cairo_line_to() coords
        // below, where it can produce undefined rendering on some Cairo
        // backends.  Replace with 0 so the curve dips to the x-axis and the
        // problem is visually obvious instead of crashing the GUI.
        if (!std::isfinite(g)) g = 0.0;
        pts.push_back({s, g});
    }
    return pts;
}

// ── Graph drawing ─────────────────────────────────────────────────────────────

void on_graph_draw(GtkDrawingArea*, cairo_t* cr,
                          int width, int height, gpointer user_data) {
    auto* S = static_cast<AppState*>(user_data);
    // Margins — GRAPH_ML/MR/MT/MB sabitlerini kullan (tek kaynak)
    double PW = width  - GRAPH_ML - GRAPH_MR;
    double PH = height - GRAPH_MT - GRAPH_MB;
    if (PW < 10 || PH < 10) return;

    double max_speed = 50.0 / S->graph_zoom + S->graph_pan_x;
    max_speed = std::max(max_speed, 5.0);

    // Dynamic max_gain: use the shared helper (also used by gesture handlers)
    double max_gain = compute_max_gain(S, max_speed);

    auto to_cx = [&](double s)  { return GRAPH_ML + (s / max_speed) * PW; };
    auto to_cy = [&](double g)  { return GRAPH_MT + PH - std::clamp(g / max_gain, 0.0, 1.0) * PH; };

    // ── Background ────────────────────────────────────────────────────────────
    cairo_set_source_rgb(cr, C_BG[0], C_BG[1], C_BG[2]);
    cairo_paint(cr);

    // Clip to graph area
    cairo_rectangle(cr, GRAPH_ML, GRAPH_MT, PW, PH);
    cairo_clip(cr);

    // ── Grid ──────────────────────────────────────────────────────────────────
    cairo_set_source_rgba(cr, C_GRID[0], C_GRID[1], C_GRID[2], C_GRID[3]);
    cairo_set_line_width(cr, 0.5);
    for (int i = 0; i <= 5; i++) {
        double cx = GRAPH_ML + PW * i / 5;
        double cy = GRAPH_MT + PH * i / 5;
        cairo_move_to(cr, cx, GRAPH_MT); cairo_line_to(cr, cx, GRAPH_MT + PH);
        cairo_move_to(cr, GRAPH_ML, cy); cairo_line_to(cr, GRAPH_ML + PW, cy);
    }
    cairo_stroke(cr);

    // ── gain = 1 reference line ────────────────────────────────────────────────
    {
        double y1 = to_cy(1.0);
        double dash[] = {6.0, 3.0};
        cairo_set_dash(cr, dash, 2, 0);
        cairo_set_source_rgba(cr, C_REF[0], C_REF[1], C_REF[2], C_REF[3]);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, GRAPH_ML, y1); cairo_line_to(cr, GRAPH_ML + PW, y1);
        cairo_stroke(cr);
        cairo_set_dash(cr, nullptr, 0, 0);
    }

    // ── Draw curves ───────────────────────────────────────────────────────────
    auto draw_curve = [&](const accel_args& args, const double col[3]) {
        auto pts = compute_curve(args, max_speed);
        if (pts.empty()) return;
        cairo_set_source_rgb(cr, col[0], col[1], col[2]);
        cairo_set_line_width(cr, 2.2);
        bool first = true;
        for (auto& p : pts) {
            double cx = to_cx(p.speed);
            double cy = to_cy(p.gain);
            if (first) { cairo_move_to(cr, cx, cy); first = false; }
            else        cairo_line_to(cr, cx, cy);
        }
        cairo_stroke(cr);
    };

    auto& dp = cur_prof(S);
    draw_curve(dp.prof.accel_x, C_CURVE);
    if (!S->xy_linked) draw_curve(dp.prof.accel_y, C_CURVE2);

    cairo_reset_clip(cr);

    // ── Axis labels ───────────────────────────────────────────────────────────
    cairo_select_font_face(cr, "monospace",
                           CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10);
    cairo_set_source_rgb(cr, C_TEXT[0], C_TEXT[1], C_TEXT[2]);

    for (int i = 0; i <= 5; i++) {
        char buf[16];
        // X labels
        double sx = max_speed * i / 5;
        snprintf(buf, sizeof(buf), "%.0f", sx);
        cairo_move_to(cr, GRAPH_ML + PW * i / 5 - 8, GRAPH_MT + PH + 14);
        cairo_show_text(cr, buf);
        // Y labels — scaled to dynamic max_gain
        double sy = max_gain * (5 - i) / 5;
        // use 2 decimals for values <2, 1 decimal otherwise
        snprintf(buf, sizeof(buf), max_gain <= 5.0 ? "%.2f" : "%.1f", sy);
        cairo_move_to(cr, 2, GRAPH_MT + PH * i / 5 + 4);
        cairo_show_text(cr, buf);
    }

    // Axis titles
    cairo_set_font_size(cr, 11);
    cairo_move_to(cr, GRAPH_ML + PW / 2 - 35, GRAPH_MT + PH + 30);
    cairo_show_text(cr, "Speed (ips)");

    cairo_save(cr);
    cairo_translate(cr, 11, GRAPH_MT + PH / 2 + 20);
    cairo_rotate(cr, -M_PI / 2);
    cairo_show_text(cr, "Gain");
    cairo_restore(cr);

    // Legend
    cairo_set_font_size(cr, 10);
    auto draw_legend = [&](double lx, double ly,
                           const double col[3], const char* label) {
        cairo_set_source_rgb(cr, col[0], col[1], col[2]);
        cairo_rectangle(cr, lx, ly, 12, 3);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, C_TEXT[0], C_TEXT[1], C_TEXT[2]);
        cairo_move_to(cr, lx + 16, ly + 4);
        cairo_show_text(cr, label);
    };
    draw_legend(GRAPH_ML + PW - 100, GRAPH_MT + 10, C_CURVE, "X Axis");
    if (!S->xy_linked)
        draw_legend(GRAPH_ML + PW - 100, GRAPH_MT + 24, C_CURVE2, "Y Axis");

    // Zoom hint
    {
        char buf[48];
        if (S->lut_graph_mode)
            snprintf(buf, sizeof(buf), "%.0f ips  (left click=add, right click=remove)", max_speed);
        else
            snprintf(buf, sizeof(buf), "%.0f ips  (scroll=zoom)", max_speed);
        cairo_set_source_rgba(cr, C_TEXT[0], C_TEXT[1], C_TEXT[2], 0.5);
        cairo_set_font_size(cr, 9);
        cairo_move_to(cr, GRAPH_ML + 4, GRAPH_MT + 12);
        cairo_show_text(cr, buf);
    }

    // Draw LUT points visually on the graph in LUT mode
    if (S->lut_graph_mode) {
        auto& dp2 = cur_prof(S);
        auto  pts = lut_get_points(dp2.prof.accel_x);
        for (auto& p : pts) {
            double px = to_cx(p.first);
            double py = to_cy(p.second);
            // Nokta dairesi
            cairo_set_source_rgb(cr, C_DOT[0], C_DOT[1], C_DOT[2]);
            cairo_arc(cr, px, py, 5.0, 0, 2 * M_PI);
            cairo_fill(cr);
            // White inner dot
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_arc(cr, px, py, 2.0, 0, 2 * M_PI);
            cairo_fill(cr);
        }
    }
}

// Graph scroll = zoom
gboolean on_graph_scroll(GtkEventControllerScroll*, double, double dy, gpointer user_data) {
    auto* S = static_cast<AppState*>(user_data);
    S->graph_zoom *= (dy > 0) ? 0.85 : 1.0 / 0.85;
    S->graph_zoom = std::clamp(S->graph_zoom, 0.2, 10.0);
    gtk_widget_queue_draw(S->graph_area);
    return TRUE;
}

// ── Graph pan (drag to scroll) ────────────────────────────────────────────

void on_graph_drag_begin(GtkGestureDrag* drag, double x, double /*y*/, gpointer user_data) {
    auto* S = static_cast<AppState*>(user_data);
    if (S->lut_graph_mode) {
        // Reject left-drag in LUT mode — let GtkGestureClick handle the click
        gtk_gesture_set_state(GTK_GESTURE(drag), GTK_EVENT_SEQUENCE_DENIED);
        return;
    }
    S->graph_drag      = true;
    S->drag_pan_start  = S->graph_pan_x;
    (void)x; // drag_start_x is no longer used (fixed in K3)
}

void on_graph_drag_update(GtkGestureDrag*, double dx, double /*dy*/, gpointer user_data) {
    auto* S = static_cast<AppState*>(user_data);
    if (!S->graph_drag) return;
    int w = gtk_widget_get_width(S->graph_area);
    double PW = w - GRAPH_ML - GRAPH_MR;
    if (PW <= 0) return;

    // max_speed must remain fixed at the pan value recorded at drag start —
    // otherwise the pixel↔ips ratio drifts during the drag (non-linear pan).
    double max_speed_at_start = 50.0 / S->graph_zoom + S->drag_pan_start;
    max_speed_at_start = std::max(max_speed_at_start, 5.0);

    // On-screen pixel movement → pan delta in ips
    double delta_ips = -(dx / PW) * max_speed_at_start;
    S->graph_pan_x = std::max(0.0, S->drag_pan_start + delta_ips);
    gtk_widget_queue_draw(S->graph_area);
}

void on_graph_drag_end(GtkGestureDrag*, double /*dx*/, double /*dy*/, gpointer user_data) {
    auto* S = static_cast<AppState*>(user_data);
    S->graph_drag = false;
}

// ── LUT hover cursor ─────────────────────────────────────────────────────────
// In LUT mode: show a crosshair cursor when hovering over a point.

void on_graph_motion(GtkEventControllerMotion*, double cx, double cy, gpointer user_data) {
    auto* S = static_cast<AppState*>(user_data);
    if (!S->lut_graph_mode) {
        gtk_widget_set_cursor_from_name(S->graph_area, "default");
        return;
    }
    int w = gtk_widget_get_width(S->graph_area);
    int h = gtk_widget_get_height(S->graph_area);
    double PW = w - GRAPH_ML - GRAPH_MR;
    double PH = h - GRAPH_MT - GRAPH_MB;
    if (PW <= 0 || PH <= 0) return;

    double max_speed = 50.0 / S->graph_zoom + S->graph_pan_x;
    max_speed = std::max(max_speed, 5.0);
    double max_gain = compute_max_gain(S, max_speed);

    const auto& ax = cur_prof(S).prof.accel_x;
    auto pts = lut_get_points(ax);

    bool near = false;
    for (auto& [spd, gain] : pts) {
        double px = GRAPH_ML + (spd / max_speed) * PW;
        double py = GRAPH_MT + (1.0 - gain / max_gain) * PH;
        double d2 = (cx - px) * (cx - px) + (cy - py) * (cy - py);
        if (d2 < 10.0 * 10.0) { near = true; break; }
    }
    gtk_widget_set_cursor_from_name(S->graph_area, near ? "crosshair" : "default");
}

// ── LUT helpers ──────────────────────────────────────────────────────────────

/// Convert LUT points from accel_args.data/length to std::vector<pair<double,double>>.
static std::vector<std::pair<double,double>> lut_get_points(const accel_args& ax) {
    std::vector<std::pair<double,double>> pts;
    int n = ax.length / 2;
    for (int i = 0; i < n; i++)
        pts.push_back({ static_cast<double>(ax.data[i*2]),
                        static_cast<double>(ax.data[i*2+1]) });
    return pts;
}

/// Sort a std::vector<pair> by speed and write it back to accel_args.data/length.
/// Returns true if the point list was truncated to fit LUT_POINTS_CAPACITY.
bool lut_set_points(accel_args& ax,
                    std::vector<std::pair<double,double>> pts) {
    // Sort by speed value
    std::sort(pts.begin(), pts.end(),
              [](const auto& a, const auto& b){ return a.first < b.first; });
    bool truncated = false;
    if ((int)pts.size() > (int)LUT_POINTS_CAPACITY) {
        pts.resize(LUT_POINTS_CAPACITY);
        truncated = true;
    }
    ax.length = static_cast<int>(pts.size()) * 2;
    // BUG-7-related defence: a double > FLT_MAX casts to +Inf, NaN casts to
    // a NaN float — both corrupt the LUT binary search.  GtkSpinButton ranges
    // are bounded so this should never fire, but a future widget change or
    // out-of-band edit (drag-to-create with a buggy max_speed) could regress.
    constexpr double FLT_HI = static_cast<double>(std::numeric_limits<float>::max());
    auto safe_f = [](double v) -> float {
        if (!std::isfinite(v)) return 0.0f;
        if (v >  FLT_HI) v =  FLT_HI;
        if (v < -FLT_HI) v = -FLT_HI;
        return static_cast<float>(v);
    };
    for (int i = 0; i < (int)pts.size(); i++) {
        ax.data[i*2]   = safe_f(pts[i].first);
        ax.data[i*2+1] = safe_f(pts[i].second);
    }
    return truncated;
}

// Forward declarations
accel_mode idx_to_mode(int i); // defined in widgets_sync.inl
void lut_list_changed(AppState* S); // defined later in this file

/// Called when the delete button for a LUT list row is pressed.
void on_lut_row_delete(GtkButton*, gpointer row_ptr) {
    // row_ptr is a GtkListBoxRow* — AppState is in widget data
    auto* row = GTK_LIST_BOX_ROW(row_ptr);
    // Retrieve AppState from the list box's parent chain via qdata
    GtkWidget* list_box = gtk_widget_get_parent(GTK_WIDGET(row));
    auto* S = static_cast<AppState*>(g_object_get_data(G_OBJECT(list_box), "app-state"));
    if (!S) return;
    int idx   = gtk_list_box_row_get_index(row);
    auto& ax  = cur_prof(S).prof.accel_x;
    auto pts  = lut_get_points(ax);
    if (idx >= 0 && idx < (int)pts.size()) {
        pts.erase(pts.begin() + idx);
        lut_set_points(ax, pts);
        if (S->xy_linked) cur_prof(S).prof.accel_y = ax;
        rebuild_lut_list(S);
        gtk_widget_queue_draw(S->graph_area);
    }
}

/// Called when a LUT spin button value changes.
void on_lut_spin_changed(GtkSpinButton* spin, gpointer) {
    // Walk up the widget tree to find the list_box that has the AppState attached
    GtkWidget* w = gtk_widget_get_parent(GTK_WIDGET(spin)); // hbox
    w = gtk_widget_get_parent(w); // GtkListBoxRow
    w = gtk_widget_get_parent(w); // GtkListBox
    auto* S = static_cast<AppState*>(g_object_get_data(G_OBJECT(w), "app-state"));
    if (!S || S->updating) return;
    lut_list_changed(S);
}

/// Fully rebuild the LUT list widget (after adding or removing a point).
void rebuild_lut_list(AppState* S) {
    if (!S->lut_list_box) return;
    S->updating = true;

    // Remove all existing rows
    GtkWidget* child;
    while ((child = gtk_widget_get_first_child(S->lut_list_box)) != nullptr)
        gtk_list_box_remove(GTK_LIST_BOX(S->lut_list_box), child);

    auto& ax  = cur_prof(S).prof.accel_x;
    auto  pts = lut_get_points(ax);

    for (int i = 0; i < (int)pts.size(); i++) {
        // Row: [  Speed: [spin]   Gain: [spin]  [Delete] ]
        GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_margin_start(hbox, 4);
        gtk_widget_set_margin_end(hbox, 4);
        gtk_widget_set_margin_top(hbox, 2);
        gtk_widget_set_margin_bottom(hbox, 2);

        GtkWidget* lbl_s = gtk_label_new(ui_text(S, "Spd:", "Hız:"));
        GtkWidget* spin_s = gtk_spin_button_new_with_range(0.0, 500.0, 0.5);
        gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin_s), 2);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_s), pts[i].first);
        gtk_widget_set_hexpand(spin_s, TRUE);

        GtkWidget* lbl_g = gtk_label_new(ui_text(S, "Gain:", "Gain:"));
        GtkWidget* spin_g = gtk_spin_button_new_with_range(0.01, 50.0, 0.01);
        gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin_g), 3);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_g), pts[i].second);
        gtk_widget_set_hexpand(spin_g, TRUE);

        GtkWidget* del_btn = gtk_button_new_from_icon_name("edit-delete-symbolic");
        gtk_widget_set_tooltip_text(del_btn, ui_text(S, "Remove this point", "Bu noktayı sil"));

        gtk_box_append(GTK_BOX(hbox), lbl_s);
        gtk_box_append(GTK_BOX(hbox), spin_s);
        gtk_box_append(GTK_BOX(hbox), lbl_g);
        gtk_box_append(GTK_BOX(hbox), spin_g);
        gtk_box_append(GTK_BOX(hbox), del_btn);

        GtkWidget* row_widget = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row_widget), hbox);
        gtk_list_box_append(GTK_LIST_BOX(S->lut_list_box), row_widget);

        // Spin callbacks: walk up the tree to find AppState (attached to list_box)
        g_signal_connect(spin_s, "value-changed", G_CALLBACK(on_lut_spin_changed), nullptr);
        g_signal_connect(spin_g, "value-changed", G_CALLBACK(on_lut_spin_changed), nullptr);
        g_signal_connect(del_btn, "clicked", G_CALLBACK(on_lut_row_delete), row_widget);
    }

    S->updating = false;
}

/// Re-read LUT data whenever a spin value changes.
/// The user may still be typing — we don't reorder the UI rows here, but
/// the data written to the daemon must be sorted (required for binary search).
/// lut_set_points handles sorting; if the order changed, also rebuild the UI.
void lut_list_changed(AppState* S) {
    if (!S->lut_list_box) return;
    S->unsaved = true; // LUT spin changes also count as unsaved
    auto& ax = cur_prof(S).prof.accel_x;
    std::vector<std::pair<double,double>> pts;

    GtkWidget* row_widget = gtk_widget_get_first_child(S->lut_list_box);
    while (row_widget) {
        GtkWidget* hbox  = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row_widget));
        // hbox children: lbl_s, spin_s, lbl_g, spin_g, del_btn
        GtkWidget* c = gtk_widget_get_first_child(hbox);
        c = gtk_widget_get_next_sibling(c); // spin_s
        double spd = gtk_spin_button_get_value(GTK_SPIN_BUTTON(c));
        c = gtk_widget_get_next_sibling(c); // lbl_g
        c = gtk_widget_get_next_sibling(c); // spin_g
        double gain = gtk_spin_button_get_value(GTK_SPIN_BUTTON(c));
        pts.push_back({spd, gain});
        row_widget = gtk_widget_get_next_sibling(row_widget);
    }

    // Save the current order before sorting; rebuild the UI if it changed.
    // This auto-sorts rows when the user edits a speed spin button.
    auto pts_before = pts;
    // lut_set_points sorts — the daemon uses binary search; unsorted data produces wrong acceleration
    lut_set_points(ax, pts);
    if (S->xy_linked) cur_prof(S).prof.accel_y = ax;
    gtk_widget_queue_draw(S->graph_area);

    // If the *order* of points changed, rebuild the list so rows move to their
    // correct positions.  BUG-3: `lut_set_points` writes float-truncated values
    // back to ax.data, so reading them with `lut_get_points` returns sub-ULP-
    // different doubles even when no reorder happened.  Exact `!=` therefore
    // fired on every spin tick (e.g. typing "5.6" → 5.5999...), causing a full
    // list rebuild on each keystroke that ate the user's caret.  Use a generous
    // epsilon (LUT speeds are at the integer scale; 1e-3 is far below user
    // intent) to detect *real* reorders only.
    bool order_changed = false;
    auto sorted_pts = lut_get_points(ax);
    if (sorted_pts.size() == pts_before.size()) {
        for (size_t i = 0; i < pts_before.size(); i++) {
            if (std::fabs(pts_before[i].first - sorted_pts[i].first) > 1e-3) {
                order_changed = true; break;
            }
        }
    } else {
        order_changed = true; // size mismatch (shouldn't normally happen)
    }
    if (order_changed) rebuild_lut_list(S);
}

/// Called when the "Add Point" button is pressed.
void on_lut_add_point(GtkButton*, gpointer user_data) {
    auto* S = static_cast<AppState*>(user_data);
    auto& ax = cur_prof(S).prof.accel_x;
    auto  pts = lut_get_points(ax);
    if ((int)pts.size() >= (int)LUT_POINTS_CAPACITY) {
        set_status(S, ui_text(S, "Maximum number of points reached.", "Maksimum nokta sayısına ulaşıldı."));
        return;
    }
    // Insert a new point just beyond the current last point
    double new_speed = pts.empty() ? 10.0 : pts.back().first + 10.0;
    double new_gain  = pts.empty() ? 1.5  : pts.back().second;
    pts.push_back({new_speed, new_gain});
    lut_set_points(ax, pts);
    if (S->xy_linked) cur_prof(S).prof.accel_y = ax;
    rebuild_lut_list(S);
    gtk_widget_queue_draw(S->graph_area);
}

/// Show/hide the LUT frame or normal parameters when the mode changes.
void update_lut_visibility(AppState* S) {
    if (!S->lut_frame || !S->accel_params_frame) return;
    bool is_lut = (idx_to_mode((int)gtk_drop_down_get_selected(
                        GTK_DROP_DOWN(S->mode_combo))) == accel_mode::lookup);
    S->lut_graph_mode = is_lut;
    gtk_widget_set_visible(S->lut_frame,         is_lut);
    gtk_widget_set_visible(S->accel_params_frame, !is_lut);
    if (is_lut) rebuild_lut_list(S);
}
