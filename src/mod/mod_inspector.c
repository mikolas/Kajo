#include "mod_window_private.h"

void
mod_inspector_populate_rows(KajoModWindow          *self,
                            const KajoModSampleRow *rows,
                            int                     n_rows)
{
    GtkWidget *child = NULL;
    while ((child = gtk_widget_get_first_child(self->box_sample_rows)) != NULL) {
        gtk_box_remove(GTK_BOX(self->box_sample_rows), child);
    }

    for (int i = 0; i < n_rows; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_add_css_class(row, "mod-sample-row");
        mod_window_append_row_label(row, rows[i].idx_hex,    NULL, 24, 0.0f, FALSE);
        mod_window_append_row_label(row, rows[i].ascii_name, NULL, -1, 0.0f, TRUE);
        mod_window_append_row_label(row, rows[i].length,     NULL, 52, 1.0f, FALSE);
        mod_window_append_row_label(row, rows[i].loop,       NULL, 74, 1.0f, FALSE);
        mod_window_append_row_label(row, rows[i].vol,        NULL, 64, 1.0f, FALSE);
        gtk_box_append(GTK_BOX(self->box_sample_rows), row);
    }
}

void
mod_inspector_update_live_rows(KajoModWindow *self, const KajoModTelemetry *tel)
{
    if (!tel || !tel->has_live_rows || tel->n_live_rows <= 0) return;
    GtkWidget *row = gtk_widget_get_first_child(self->box_sample_rows);
    for (int i = 0; i < tel->n_live_rows && row != NULL; i++) {
        const KajoModSampleRow *r = &tel->live_rows[i];
        const char *vals[5] = { r->idx_hex, r->ascii_name, r->length, r->loop, r->vol };
        GtkWidget *cell = gtk_widget_get_first_child(row);
        for (int c = 0; c < 5 && cell != NULL; c++) {
            if (GTK_IS_LABEL(cell)) gtk_label_set_text(GTK_LABEL(cell), vals[c]);
            cell = gtk_widget_get_next_sibling(cell);
        }
        row = gtk_widget_get_next_sibling(row);
    }
}

static void
reflow_grid_items(GtkWidget *grid, GtkWidget **items, int count, int cols)
{
    if (!grid || !items[0]) return;
    for (int i = 0; i < count; i++) {
        g_object_ref(items[i]);
        gtk_grid_remove(GTK_GRID(grid), items[i]);
    }
    for (int i = 0; i < count; i++) {
        gtk_grid_attach(GTK_GRID(grid), items[i], i % cols, i / cols, 1, 1);
        g_object_unref(items[i]);
    }
}

void
mod_inspector_apply_responsive_reflow(KajoModWindow *self, int width)
{
    if (!self || !self->col_samples || !self->col_stil) return;

    int effective_w = (self->sim_col_width >= 480) ? self->sim_col_width : width;
    gboolean narrow = (effective_w > 0 && effective_w < 760);
    self->is_narrow_column = narrow;

    reflow_grid_items(self->cnt_grid, self->cnt_cells, 6, narrow ? 3 : 6);
    reflow_grid_items(self->ch_grid, self->ch_pills, 4, narrow ? 2 : 4);

    gtk_widget_set_visible(self->bdg_col_mode, !narrow);
    gtk_widget_set_visible(self->box_narrow_tabs, narrow);
    gtk_widget_set_visible(self->col_samples, !narrow || !self->narrow_show_stil);
    gtk_widget_set_visible(self->col_stil, !narrow || self->narrow_show_stil);
    if (!narrow)
        gtk_widget_add_css_class(self->col_samples, "mod-inspector-left");
    else
        gtk_widget_remove_css_class(self->col_samples, "mod-inspector-left");
}

static void
on_narrow_tab_samples_clicked(GtkButton *b G_GNUC_UNUSED, gpointer d)
{
    KajoModWindow *self = KAJO_MOD_WINDOW(d);
    self->narrow_show_stil = FALSE;
    gtk_widget_add_css_class(self->btn_tab_samples, "active");
    gtk_widget_remove_css_class(self->btn_tab_stil, "active");
    mod_inspector_apply_responsive_reflow(self, gtk_widget_get_width(GTK_WIDGET(self)));
}

static void
on_narrow_tab_stil_clicked(GtkButton *b G_GNUC_UNUSED, gpointer d)
{
    KajoModWindow *self = KAJO_MOD_WINDOW(d);
    self->narrow_show_stil = TRUE;
    gtk_widget_remove_css_class(self->btn_tab_samples, "active");
    gtk_widget_add_css_class(self->btn_tab_stil, "active");
    mod_inspector_apply_responsive_reflow(self, gtk_widget_get_width(GTK_WIDGET(self)));
}

void
mod_inspector_rebuild_playlist_rows(KajoModWindow *self)
{
    GtkWidget *child = NULL;
    while ((child = gtk_widget_get_first_child(self->pl_listbox)) != NULL) {
        gtk_list_box_remove(GTK_LIST_BOX(self->pl_listbox), child);
    }

    guint count = self->playlist ? self->playlist->items->len : 0;
    g_autofree gchar *hdr = g_strdup_printf("📂 PLAYLIST & ARCHIVE QUEUE (%u TRACK%s)",
                                            count, count == 1 ? "" : "S");
    gtk_label_set_text(GTK_LABEL(self->lbl_pl_title), hdr);

    const char *q = gtk_editable_get_text(GTK_EDITABLE(self->entry_filter));
    g_autofree gchar *q_low = g_ascii_strdown(q ? q : "", -1);

    for (guint i = 0; i < count; i++) {
        KajoModPlaylistItem *it = g_ptr_array_index(self->playlist->items, i);
        GtkWidget *rbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        g_autofree gchar *num_str = g_strdup_printf("%02u", i + 1);

        mod_window_append_row_label(rbox, num_str, "mod-pl-text", 22, 0.0f, FALSE);
        mod_window_append_row_label(rbox, it->display_title ? it->display_title : "Track",
                                    "mod-pl-text", -1, 0.0f, TRUE);

        GtkWidget *l_b = gtk_label_new(it->badge ? it->badge : "MOD");
        gtk_widget_add_css_class(l_b, "shell-popover-badge");
        if (it->badge_css && it->badge_css[0] != '\0')
            gtk_widget_add_css_class(l_b, it->badge_css);
        gtk_widget_set_valign(l_b, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(rbox), l_b);

        mod_window_append_row_label(rbox, it->sub_text ? it->sub_text : "1 SUB",
                                    "mod-pl-text", 40, 0.0f, FALSE);
        mod_window_append_row_label(rbox, it->dur_text ? it->dur_text : "--:--",
                                    "mod-pl-text", 40, 1.0f, FALSE);

        GtkWidget *row = gtk_list_box_row_new();
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), rbox);

        g_autofree gchar *t_low = g_ascii_strdown(it->display_title ? it->display_title : "", -1);
        g_autofree gchar *b_low = g_ascii_strdown(it->badge ? it->badge : "", -1);
        gboolean match = (!q_low[0] || strstr(t_low, q_low) != NULL || strstr(b_low, q_low) != NULL);
        gtk_widget_set_visible(row, match);

        gtk_list_box_append(GTK_LIST_BOX(self->pl_listbox), row);
    }
}

static void
on_pl_row_selected(GtkListBox *box G_GNUC_UNUSED, GtkListBoxRow *row, gpointer d)
{
    if (!row) return;
    KajoModWindow *self = KAJO_MOD_WINDOW(d);
    int idx = gtk_list_box_row_get_index(row);
    if (idx >= 0 && idx != self->active_pl_index)
        mod_window_activate_playlist_index(self, idx);
}

static void
on_filter_changed(GtkEditable *editable G_GNUC_UNUSED, gpointer d)
{
    mod_inspector_rebuild_playlist_rows(KAJO_MOD_WINDOW(d));
}

static void
on_add_files_finish(GObject *source, GAsyncResult *res, gpointer user_data)
{
    GWeakRef *wr = (GWeakRef *)user_data;
    g_autoptr(GError) err = NULL;
    g_autoptr(GListModel) files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(source), res, &err);
    g_autoptr(KajoModWindow) self = g_weak_ref_get(wr);
    mod_window_weak_ref_free(wr);
    if (!self || !files || err) return;

    guint n = g_list_model_get_n_items(files);
    g_autoptr(GPtrArray) paths = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < n; i++) {
        g_autoptr(GFile) f = g_list_model_get_item(files, i);
        gchar *p = g_file_get_path(f);
        if (p) g_ptr_array_add(paths, p);
    }
    if (paths->len > 0)
        kajo_mod_window_open_paths(self, (const char * const *)paths->pdata, (int)paths->len);
}

static void
on_add_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer d)
{
    KajoModWindow *self = KAJO_MOD_WINDOW(d);
    g_autoptr(GtkFileDialog) dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Add Modules or Archives (.lha, .zip, .sid, .mod, ...)");
    gtk_file_dialog_open_multiple(dlg, GTK_WINDOW(self), NULL, on_add_files_finish,
                                  mod_window_weak_ref_new(self));
}

static void
on_add_dir_finish(GObject *source, GAsyncResult *res, gpointer user_data)
{
    GWeakRef *wr = (GWeakRef *)user_data;
    g_autoptr(GError) err = NULL;
    g_autoptr(GFile) folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), res, &err);
    g_autoptr(KajoModWindow) self = g_weak_ref_get(wr);
    mod_window_weak_ref_free(wr);
    if (!self || !folder || err) return;

    g_autofree gchar *p = g_file_get_path(folder);
    if (p) {
        const char *paths[1] = { p };
        kajo_mod_window_open_paths(self, paths, 1);
    }
}

static void
on_dir_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer d)
{
    KajoModWindow *self = KAJO_MOD_WINDOW(d);
    g_autoptr(GtkFileDialog) dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Add Folder of Modules / HVSC / Archives");
    gtk_file_dialog_select_folder(dlg, GTK_WINDOW(self), NULL, on_add_dir_finish,
                                  mod_window_weak_ref_new(self));
}

static void
on_rem_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer d)
{
    KajoModWindow *self = KAJO_MOD_WINDOW(d);
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(self->pl_listbox));
    if (!row) return;
    int idx = gtk_list_box_row_get_index(row);
    if (idx >= 0) {
        kajo_mod_playlist_remove_at(self->playlist, (guint)idx);
        if (self->playlist->items->len == 0) {
            kajo_mod_audio_set_engine(self->audio, NULL, FALSE);
            self->active_pl_index = -1;
            mod_window_reset_ui_to_idle(self);
        }
        mod_inspector_rebuild_playlist_rows(self);
    }
}

static void
on_clr_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer d)
{
    KajoModWindow *self = KAJO_MOD_WINDOW(d);
    kajo_mod_audio_set_engine(self->audio, NULL, FALSE);
    kajo_mod_playlist_clear(self->playlist);
    self->active_pl_index = -1;
    mod_window_reset_ui_to_idle(self);
    mod_inspector_rebuild_playlist_rows(self);
}

static GtkWidget *
build_upper_inspector_card(KajoModWindow *self)
{
    GtkWidget *upper_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(upper_card, "mod-pane-card");
    gtk_widget_set_vexpand(upper_card, TRUE);

    GtkWidget *up_tb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(up_tb, "mod-pane-toolbar");
    mod_window_append_row_label(up_tb, "📋 MODULE, HARDWARE & ARCHIVE INSPECTOR",
                                "mod-pane-title", -1, 0.0f, TRUE);
    self->bdg_col_mode = mod_window_create_badge_label("2-COL SPLIT (≥760px)", NULL);

    self->box_narrow_tabs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(self->box_narrow_tabs, "segmented-group");
    self->btn_tab_samples = mod_window_create_nav_button(
        "SAMPLES / VOICES", G_CALLBACK(on_narrow_tab_samples_clicked), self);
    gtk_widget_add_css_class(self->btn_tab_samples, "active");
    self->btn_tab_stil = mod_window_create_nav_button(
        "STIL / .NFO", G_CALLBACK(on_narrow_tab_stil_clicked), self);
    gtk_box_append(GTK_BOX(self->box_narrow_tabs), self->btn_tab_samples);
    gtk_box_append(GTK_BOX(self->box_narrow_tabs), self->btn_tab_stil);
    gtk_widget_set_visible(self->box_narrow_tabs, FALSE);

    gtk_box_append(GTK_BOX(up_tb), self->bdg_col_mode);
    gtk_box_append(GTK_BOX(up_tb), self->box_narrow_tabs);

    GtkWidget *insp_body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_vexpand(insp_body, TRUE);
    gtk_widget_set_hexpand(insp_body, TRUE);

    self->col_samples = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(self->col_samples, "mod-inspector-left");
    gtk_widget_set_hexpand(self->col_samples, TRUE);
    gtk_widget_set_vexpand(self->col_samples, TRUE);

    GtkWidget *s_hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(s_hdr, "mod-subcol-hdr");
    self->lbl_sample_hdr_left = gtk_label_new("SAMPLES / INSTRUMENTS / REGISTERS");
    gtk_label_set_xalign(GTK_LABEL(self->lbl_sample_hdr_left), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(self->lbl_sample_hdr_left), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(self->lbl_sample_hdr_left, TRUE);
    self->lbl_sample_hdr_right = gtk_label_new("IDLE");
    gtk_widget_add_css_class(self->lbl_sample_hdr_right, "mod-counter-val");
    gtk_widget_add_css_class(self->lbl_sample_hdr_right, "accent");
    gtk_box_append(GTK_BOX(s_hdr), self->lbl_sample_hdr_left);
    gtk_box_append(GTK_BOX(s_hdr), self->lbl_sample_hdr_right);

    GtkWidget *s_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(s_scroll, TRUE);
    self->box_sample_rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(s_scroll), self->box_sample_rows);
    gtk_box_append(GTK_BOX(self->col_samples), s_hdr);
    gtk_box_append(GTK_BOX(self->col_samples), s_scroll);

    self->col_stil = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(self->col_stil, TRUE);
    gtk_widget_set_vexpand(self->col_stil, TRUE);

    GtkWidget *m_hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(m_hdr, "mod-subcol-hdr");
    mod_window_append_row_label(m_hdr, "ARCHIVE .NFO / HVSC STIL / SONG MESSAGE", NULL, -1, 0.0f, TRUE);
    self->lbl_stil_hdr_right = mod_window_create_badge_label("NONE", "warning");
    gtk_box_append(GTK_BOX(m_hdr), self->lbl_stil_hdr_right);

    GtkWidget *m_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(m_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(m_scroll, TRUE);

    GtkWidget *stil_tv = gtk_text_view_new();
    gtk_widget_add_css_class(stil_tv, "mod-stil-view");
    gtk_text_view_set_editable(GTK_TEXT_VIEW(stil_tv), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(stil_tv), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(stil_tv), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(stil_tv), 8);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(stil_tv), 8);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(stil_tv), 6);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(stil_tv), 6);
    self->stil_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(stil_tv));
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(m_scroll), stil_tv);

    gtk_box_append(GTK_BOX(self->col_stil), m_hdr);
    gtk_box_append(GTK_BOX(self->col_stil), m_scroll);

    gtk_box_append(GTK_BOX(insp_body), self->col_samples);
    gtk_box_append(GTK_BOX(insp_body), self->col_stil);
    gtk_box_append(GTK_BOX(upper_card), up_tb);
    gtk_box_append(GTK_BOX(upper_card), insp_body);
    return upper_card;
}

GtkWidget *
mod_inspector_build_paned(KajoModWindow *self)
{
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_widget_set_vexpand(paned, TRUE);
    gtk_widget_set_hexpand(paned, TRUE);
    gtk_widget_set_margin_start(paned, 8);
    gtk_widget_set_margin_end(paned, 8);
    gtk_widget_set_margin_top(paned, 5);
    gtk_widget_set_margin_bottom(paned, 6);
    gtk_paned_set_resize_start_child(GTK_PANED(paned), TRUE);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), TRUE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_position(GTK_PANED(paned), 195);

    GtkWidget *lower_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(lower_card, "mod-pane-card");
    gtk_widget_set_vexpand(lower_card, TRUE);

    GtkWidget *pl_tb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(pl_tb, "mod-pane-toolbar");
    self->lbl_pl_title = gtk_label_new("📂 PLAYLIST & ARCHIVE QUEUE (0 TRACKS)");
    gtk_widget_add_css_class(self->lbl_pl_title, "mod-pane-title");
    gtk_label_set_xalign(GTK_LABEL(self->lbl_pl_title), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(self->lbl_pl_title), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(self->lbl_pl_title, TRUE);

    self->entry_filter = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(self->entry_filter), "🔍 Filter...");
    gtk_editable_set_width_chars(GTK_EDITABLE(self->entry_filter), 5);
    gtk_editable_set_max_width_chars(GTK_EDITABLE(self->entry_filter), 12);
    g_signal_connect(self->entry_filter, "changed", G_CALLBACK(on_filter_changed), self);

    gtk_box_append(GTK_BOX(pl_tb), self->lbl_pl_title);
    gtk_box_append(GTK_BOX(pl_tb), mod_window_create_nav_button("+ ADD", G_CALLBACK(on_add_clicked), self));
    gtk_box_append(GTK_BOX(pl_tb), mod_window_create_nav_button("+ DIR", G_CALLBACK(on_dir_clicked), self));
    gtk_box_append(GTK_BOX(pl_tb), mod_window_create_nav_button("− REM", G_CALLBACK(on_rem_clicked), self));
    gtk_box_append(GTK_BOX(pl_tb), mod_window_create_nav_button("CLR",   G_CALLBACK(on_clr_clicked), self));
    gtk_box_append(GTK_BOX(pl_tb), self->entry_filter);

    GtkWidget *pl_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(pl_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(pl_scroll, TRUE);

    self->pl_listbox = gtk_list_box_new();
    gtk_widget_add_css_class(self->pl_listbox, "mod-pl-listbox");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->pl_listbox), GTK_SELECTION_SINGLE);
    g_signal_connect(self->pl_listbox, "row-selected", G_CALLBACK(on_pl_row_selected), self);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(pl_scroll), self->pl_listbox);
    gtk_box_append(GTK_BOX(lower_card), pl_tb);
    gtk_box_append(GTK_BOX(lower_card), pl_scroll);

    gtk_paned_set_start_child(GTK_PANED(paned), build_upper_inspector_card(self));
    gtk_paned_set_end_child(GTK_PANED(paned), lower_card);
    return paned;
}
