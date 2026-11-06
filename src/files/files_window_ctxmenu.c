#include "files_window_private.h"

/* ─── Context Menu Popover (Item Context + Directory Background + Open With) ─── */

static GtkWidget *
create_ctx_action_button(const char *label_text,
                         const char *shortcut_text,
                         gboolean is_danger,
                         GCallback callback,
                         gpointer user_data)
{
    GtkWidget *btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "flat");
    gtk_widget_add_css_class(btn, "files-menu-row");
    if (is_danger)
        gtk_widget_add_css_class(btn, "action-btn-danger");

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_valign(hbox, GTK_ALIGN_CENTER);

    GtkWidget *lbl = gtk_label_new(label_text);
    gtk_widget_set_hexpand(lbl, TRUE);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(hbox), lbl);

    if (shortcut_text && shortcut_text[0] != '\0') {
        GtkWidget *sc = gtk_label_new(shortcut_text);
        gtk_widget_add_css_class(sc, "files-ctx-shortcut");
        gtk_widget_set_halign(sc, GTK_ALIGN_END);
        gtk_widget_set_valign(sc, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(hbox), sc);
    }

    gtk_button_set_child(GTK_BUTTON(btn), hbox);
    if (callback)
        g_signal_connect(btn, "clicked", callback, user_data);
    return btn;
}

static void
close_context_menu(KajoFilesWindow *self)
{
    if (self && self->context_popover)
        gtk_popover_popdown(GTK_POPOVER(self->context_popover));
}

static void
on_ctx_open_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    if (self->selected_item)
        files_window_activate_file_item(self, self->selected_item);
}

static void
on_ctx_quicklook_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    if (self->selected_item)
        kajo_quicklook_show(self->quicklook);
}

static void
on_ctx_extract_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    if (!self->selected_item || !self->current_dir)
        return;

    GFile *archive_file = kajo_file_item_get_file(self->selected_item);
    files_window_set_status_message(self, "Extracting archive...", FALSE);
    kajo_files_op_extract_archive_async(archive_file, self->current_dir, files_window_on_file_op_completed, files_window_op_weak_new(self));
}

static void
on_ctx_item_terminal_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    if (self->selected_item && kajo_file_item_is_dir(self->selected_item))
        kajo_files_open_terminal_at(kajo_file_item_get_file(self->selected_item));
    else if (self->current_dir)
        kajo_files_open_terminal_at(self->current_dir);
}

static void
on_ctx_item_newwin_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    GtkApplication *app = gtk_window_get_application(GTK_WINDOW(self));
    const char *uri = NULL;
    g_autofree char *dir_uri = NULL;
    if (self->selected_item && kajo_file_item_is_dir(self->selected_item)) {
        uri = kajo_file_item_get_target_uri(self->selected_item);
        if (!uri) uri = kajo_file_item_get_uri(self->selected_item);
    } else if (self->current_dir) {
        dir_uri = g_file_get_uri(self->current_dir);
        uri = dir_uri;
    }
    GtkWidget *new_win = kajo_files_window_new(app, uri);
    gtk_window_present(GTK_WINDOW(new_win));
}

static void
on_ctx_restore_trash_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    on_trash_restore_clicked(NULL, self);
}

static void
on_ctx_copy_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    if (!self->selected_item) return;
    g_autoptr(GPtrArray) arr = g_ptr_array_new_with_free_func(g_object_unref);
    g_ptr_array_add(arr, g_object_ref(self->selected_item));
    kajo_files_clipboard_set(arr, FALSE);
    gtk_widget_set_sensitive(self->btn_paste, TRUE);
    files_window_set_status_message(self, "Copied 1 item to clipboard", FALSE);
}

static void
on_ctx_cut_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    if (!self->selected_item) return;
    g_autoptr(GPtrArray) arr = g_ptr_array_new_with_free_func(g_object_unref);
    g_ptr_array_add(arr, g_object_ref(self->selected_item));
    kajo_files_clipboard_set(arr, TRUE);
    gtk_widget_set_sensitive(self->btn_paste, TRUE);
    files_window_set_status_message(self, "Cut 1 item to clipboard", FALSE);
}

void
files_window_copy_selected_item_path(KajoFilesWindow *self)
{
    if (!self || !self->selected_item) return;
    g_autofree char *copied = kajo_files_copy_path_to_clipboard(self->selected_item);
    if (copied) {
        g_autofree char *msg = g_strdup_printf("Copied path: %s", copied);
        files_window_set_status_message(self, msg, FALSE);
    }
}

static void
on_ctx_copy_path_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    files_window_copy_selected_item_path(self);
}

static void
on_ctx_rename_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    files_window_show_inline_prompt(self, PROMPT_RENAME);
}

static void
on_ctx_move_trash_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    files_window_trash_selected_item(self);
}

static void
on_ctx_delete_perm_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    files_window_show_inline_prompt(self, PROMPT_DELETE_PERM);
}

static void
on_ctx_inspector_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    on_btn_inspector_clicked(NULL, self);
}

static void
on_ctx_bg_mkdir_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    files_window_show_inline_prompt(self, PROMPT_MKDIR);
}

static void
on_ctx_bg_paste_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    on_btn_paste_clicked(NULL, self);
}

static void
on_ctx_bg_terminal_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    on_btn_terminal_clicked(NULL, self);
}

static void
on_ctx_bg_newwin_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    GtkApplication *app = gtk_window_get_application(GTK_WINDOW(self));
    g_autofree char *uri = self->current_dir ? g_file_get_uri(self->current_dir) : NULL;
    GtkWidget *new_win = kajo_files_window_new(app, uri);
    gtk_window_present(GTK_WINDOW(new_win));
}

static void
on_ctx_bg_hidden_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    on_btn_hidden_clicked(NULL, self);
}

static void
on_ctx_bg_reload_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    close_context_menu(self);
    files_window_reload_directory(self);
}

/* ─── Open With Sub-View Handlers ─── */

static void
on_ctx_open_with_app_clicked(GtkButton *btn, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    GAppInfo *app_info = g_object_get_data(G_OBJECT(btn), "app-info");
    close_context_menu(self);

    if (!self->selected_item || !app_info)
        return;

    g_autoptr(GError) err = NULL;
    if (kajo_files_open_item_with_app(self->selected_item, app_info, self->open_with_set_default, &err)) {
        if (self->open_with_set_default) {
            g_autofree char *msg = g_strdup_printf(
                "Opened with %s (saved as default for %s)",
                g_app_info_get_display_name(app_info),
                kajo_file_item_get_content_type(self->selected_item)
            );
            files_window_set_status_message(self, msg, FALSE);
        }
    } else if (err) {
        files_window_set_status_message(self, err->message, TRUE);
    }
}

static void
on_ctx_ow_default_toggle_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    self->open_with_set_default = !self->open_with_set_default;
    gtk_button_set_label(
        GTK_BUTTON(self->ctx_ow_default_btn),
        self->open_with_set_default ? "☑  Set as Default for this type" : "☐  Set as Default for this type"
    );
    files_window_set_widget_active_class(self->ctx_ow_default_btn, self->open_with_set_default);
}

static void
on_ctx_ow_back_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    gtk_stack_set_visible_child_name(GTK_STACK(self->ctx_stack), "main");
}

static void
on_ctx_open_with_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (!self->selected_item) return;

    const char *ctype = kajo_file_item_get_content_type(self->selected_item);
    gtk_label_set_text(GTK_LABEL(self->ctx_ow_mime_lbl), ctype ? ctype : "application/octet-stream");

    self->open_with_set_default = FALSE;
    gtk_button_set_label(GTK_BUTTON(self->ctx_ow_default_btn), "☐  Set as Default for this type");
    files_window_set_widget_active_class(self->ctx_ow_default_btn, FALSE);

    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(self->ctx_ow_list_box)) != NULL) {
        gtk_box_remove(GTK_BOX(self->ctx_ow_list_box), child);
    }

    g_autoptr(GAppInfo) default_app = ctype ? g_app_info_get_default_for_type(ctype, FALSE) : NULL;
    GList *apps = ctype ? g_app_info_get_all_for_type(ctype) : NULL;

    /* Always offer Kajo Player at the top of Open With for any video or audio media */
    if (ctype && (g_str_has_prefix(ctype, "video/") ||
                  g_str_has_prefix(ctype, "audio/") ||
                  g_strcmp0(ctype, "application/ogg") == 0)) {
        GDesktopAppInfo *kajo_player_dt = g_desktop_app_info_new("org.kajo.Player.desktop");
        if (!kajo_player_dt && g_file_test("/usr/local/share/applications/org.kajo.Player.desktop", G_FILE_TEST_EXISTS)) {
            kajo_player_dt = g_desktop_app_info_new_from_filename("/usr/local/share/applications/org.kajo.Player.desktop");
        }
        if (kajo_player_dt) {
            GAppInfo *kp_info = G_APP_INFO(kajo_player_dt);
            gboolean already_listed = FALSE;
            for (GList *l = apps; l != NULL; l = l->next) {
                GAppInfo *cand = G_APP_INFO(l->data);
                if (g_app_info_equal(cand, kp_info) ||
                    g_strcmp0(g_app_info_get_id(cand), "org.kajo.Player.desktop") == 0) {
                    already_listed = TRUE;
                    break;
                }
            }
            if (!already_listed)
                apps = g_list_prepend(apps, kp_info);
            else
                g_object_unref(kp_info);
        }
    }

    if (!apps) {
        GtkWidget *empty = gtk_label_new("No registered applications found for this MIME type");
        gtk_widget_add_css_class(empty, "shell-popover-subtitle");
        gtk_widget_set_margin_top(empty, 8);
        gtk_widget_set_margin_bottom(empty, 8);
        gtk_box_append(GTK_BOX(self->ctx_ow_list_box), empty);
    } else {
        for (GList *l = apps; l != NULL; l = l->next) {
            GAppInfo *app = G_APP_INFO(l->data);
            if (!g_app_info_should_show(app) && !(default_app && g_app_info_equal(app, default_app)))
                continue;

            GtkWidget *row_btn = gtk_button_new();
            gtk_widget_add_css_class(row_btn, "flat");
            gtk_widget_add_css_class(row_btn, "files-menu-row");
            if (default_app && g_app_info_equal(app, default_app))
                gtk_widget_add_css_class(row_btn, "active");

            GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            gtk_widget_set_valign(hbox, GTK_ALIGN_CENTER);

            GIcon *gicon = g_app_info_get_icon(app);
            GtkWidget *ic = gicon
                ? gtk_image_new_from_gicon(gicon)
                : gtk_image_new_from_icon_name("application-x-executable");
            gtk_image_set_pixel_size(GTK_IMAGE(ic), 16);
            gtk_widget_set_valign(ic, GTK_ALIGN_CENTER);

            GtkWidget *nm = gtk_label_new(g_app_info_get_display_name(app));
            gtk_widget_set_hexpand(nm, TRUE);
            gtk_widget_set_halign(nm, GTK_ALIGN_START);
            gtk_widget_set_valign(nm, GTK_ALIGN_CENTER);
            gtk_label_set_ellipsize(GTK_LABEL(nm), PANGO_ELLIPSIZE_END);

            gtk_box_append(GTK_BOX(hbox), ic);
            gtk_box_append(GTK_BOX(hbox), nm);

            if (default_app && g_app_info_equal(app, default_app)) {
                GtkWidget *def_badge = gtk_label_new("DEFAULT");
                gtk_widget_add_css_class(def_badge, "files-ctx-shortcut");
                gtk_widget_set_valign(def_badge, GTK_ALIGN_CENTER);
                gtk_box_append(GTK_BOX(hbox), def_badge);
            }

            gtk_button_set_child(GTK_BUTTON(row_btn), hbox);
            g_object_set_data_full(G_OBJECT(row_btn), "app-info", g_object_ref(app), g_object_unref);
            g_signal_connect(row_btn, "clicked", G_CALLBACK(on_ctx_open_with_app_clicked), self);
            gtk_box_append(GTK_BOX(self->ctx_ow_list_box), row_btn);
        }
        g_list_free_full(apps, g_object_unref);
    }

    gtk_stack_set_visible_child_name(GTK_STACK(self->ctx_stack), "open_with");
}

void
files_window_build_context_menu_popover(KajoFilesWindow *self)
{
    self->context_popover = gtk_popover_new();
    gtk_popover_set_has_arrow(GTK_POPOVER(self->context_popover), FALSE);
    gtk_popover_set_autohide(GTK_POPOVER(self->context_popover), TRUE);
    gtk_widget_add_css_class(self->context_popover, "shell-popover");
    gtk_widget_add_css_class(self->context_popover, "files-context-popover");
    gtk_widget_set_parent(self->context_popover, GTK_WIDGET(self->view_stack));

    self->ctx_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(self->ctx_stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
    gtk_widget_set_size_request(self->ctx_stack, 286, -1);

    /* ── Page 1: Main Context Menu (Item or Directory Background) ── */
    GtkWidget *main_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(hdr, "shell-popover-header");
    gtk_widget_set_valign(hdr, GTK_ALIGN_CENTER);

    self->ctx_hdr_icon = gtk_image_new_from_icon_name("folder");
    gtk_image_set_pixel_size(GTK_IMAGE(self->ctx_hdr_icon), 16);
    gtk_widget_set_valign(self->ctx_hdr_icon, GTK_ALIGN_CENTER);

    self->ctx_hdr_title_lbl = gtk_label_new("ITEM");
    gtk_widget_add_css_class(self->ctx_hdr_title_lbl, "heading");
    gtk_widget_set_hexpand(self->ctx_hdr_title_lbl, TRUE);
    gtk_widget_set_halign(self->ctx_hdr_title_lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(self->ctx_hdr_title_lbl, GTK_ALIGN_CENTER);
    gtk_label_set_ellipsize(GTK_LABEL(self->ctx_hdr_title_lbl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(self->ctx_hdr_title_lbl), 22);

    self->ctx_hdr_badge_lbl = gtk_label_new("");
    gtk_widget_add_css_class(self->ctx_hdr_badge_lbl, "shell-popover-badge");
    gtk_widget_set_halign(self->ctx_hdr_badge_lbl, GTK_ALIGN_END);
    gtk_widget_set_valign(self->ctx_hdr_badge_lbl, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(hdr), self->ctx_hdr_icon);
    gtk_box_append(GTK_BOX(hdr), self->ctx_hdr_title_lbl);
    gtk_box_append(GTK_BOX(hdr), self->ctx_hdr_badge_lbl);
    gtk_box_append(GTK_BOX(main_page), hdr);

    /* Item Actions Box */
    self->ctx_item_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_add_css_class(self->ctx_item_box, "shell-popover-body");

    gtk_box_append(GTK_BOX(self->ctx_item_box),
        create_ctx_action_button("▶  Open", "Enter", FALSE, G_CALLBACK(on_ctx_open_clicked), self));

    self->ctx_btn_open_with = create_ctx_action_button(
        "⚙  Open With...", "▸", FALSE, G_CALLBACK(on_ctx_open_with_clicked), self);
    gtk_box_append(GTK_BOX(self->ctx_item_box), self->ctx_btn_open_with);

    gtk_box_append(GTK_BOX(self->ctx_item_box),
        create_ctx_action_button("👁  Quick Look", "Space", FALSE, G_CALLBACK(on_ctx_quicklook_clicked), self));

    self->ctx_btn_extract = create_ctx_action_button(
        "📦  Extract Here", "", FALSE, G_CALLBACK(on_ctx_extract_clicked), self);
    gtk_box_append(GTK_BOX(self->ctx_item_box), self->ctx_btn_extract);

    self->ctx_btn_item_terminal = create_ctx_action_button(
        ">_  Open Terminal Here", "", FALSE, G_CALLBACK(on_ctx_item_terminal_clicked), self);
    gtk_box_append(GTK_BOX(self->ctx_item_box), self->ctx_btn_item_terminal);

    self->ctx_btn_item_newwin = create_ctx_action_button(
        "🗔  Open in New Window", "Ctrl+N", FALSE, G_CALLBACK(on_ctx_item_newwin_clicked), self);
    gtk_box_append(GTK_BOX(self->ctx_item_box), self->ctx_btn_item_newwin);

    self->ctx_btn_restore_trash = create_ctx_action_button(
        "↺  Restore from Trash", "", FALSE, G_CALLBACK(on_ctx_restore_trash_clicked), self);
    gtk_box_append(GTK_BOX(self->ctx_item_box), self->ctx_btn_restore_trash);

    gtk_box_append(GTK_BOX(self->ctx_item_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    gtk_box_append(GTK_BOX(self->ctx_item_box),
        create_ctx_action_button("📋  Copy", "Ctrl+C", FALSE, G_CALLBACK(on_ctx_copy_clicked), self));
    gtk_box_append(GTK_BOX(self->ctx_item_box),
        create_ctx_action_button("✂  Cut", "Ctrl+X", FALSE, G_CALLBACK(on_ctx_cut_clicked), self));
    gtk_box_append(GTK_BOX(self->ctx_item_box),
        create_ctx_action_button("🔗  Copy Path / URI", "Ctrl+Shift+C", FALSE, G_CALLBACK(on_ctx_copy_path_clicked), self));

    gtk_box_append(GTK_BOX(self->ctx_item_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    gtk_box_append(GTK_BOX(self->ctx_item_box),
        create_ctx_action_button("✏  Rename...", "F2", FALSE, G_CALLBACK(on_ctx_rename_clicked), self));

    self->ctx_btn_move_trash = create_ctx_action_button(
        "🗑  Move to Trash", "Del", FALSE, G_CALLBACK(on_ctx_move_trash_clicked), self);
    gtk_box_append(GTK_BOX(self->ctx_item_box), self->ctx_btn_move_trash);

    gtk_box_append(GTK_BOX(self->ctx_item_box),
        create_ctx_action_button("⚠  Delete Permanently", "Shift+Del", TRUE, G_CALLBACK(on_ctx_delete_perm_clicked), self));

    gtk_box_append(GTK_BOX(self->ctx_item_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    gtk_box_append(GTK_BOX(self->ctx_item_box),
        create_ctx_action_button("ⓘ  Toggle Inspector", "Ctrl+I", FALSE, G_CALLBACK(on_ctx_inspector_clicked), self));

    gtk_box_append(GTK_BOX(main_page), self->ctx_item_box);

    /* Directory Background Actions Box */
    self->ctx_bg_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_add_css_class(self->ctx_bg_box, "shell-popover-body");

    gtk_box_append(GTK_BOX(self->ctx_bg_box),
        create_ctx_action_button("+  New Folder...", "Ctrl+Shift+N", FALSE, G_CALLBACK(on_ctx_bg_mkdir_clicked), self));

    self->ctx_btn_bg_paste = create_ctx_action_button(
        "📋  Paste Here", "Ctrl+V", FALSE, G_CALLBACK(on_ctx_bg_paste_clicked), self);
    gtk_box_append(GTK_BOX(self->ctx_bg_box), self->ctx_btn_bg_paste);

    gtk_box_append(GTK_BOX(self->ctx_bg_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    gtk_box_append(GTK_BOX(self->ctx_bg_box),
        create_ctx_action_button(">_  Open Terminal Here", "Ctrl+Shift+T", FALSE, G_CALLBACK(on_ctx_bg_terminal_clicked), self));
    gtk_box_append(GTK_BOX(self->ctx_bg_box),
        create_ctx_action_button("🗔  Open in New Window", "Ctrl+N", FALSE, G_CALLBACK(on_ctx_bg_newwin_clicked), self));

    gtk_box_append(GTK_BOX(self->ctx_bg_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    gtk_box_append(GTK_BOX(self->ctx_bg_box),
        create_ctx_action_button("👁  Toggle Hidden Files", "Ctrl+H", FALSE, G_CALLBACK(on_ctx_bg_hidden_clicked), self));
    gtk_box_append(GTK_BOX(self->ctx_bg_box),
        create_ctx_action_button("↻  Reload Directory", "Ctrl+R", FALSE, G_CALLBACK(on_ctx_bg_reload_clicked), self));
    gtk_box_append(GTK_BOX(self->ctx_bg_box),
        create_ctx_action_button("ⓘ  Toggle Inspector", "Ctrl+I", FALSE, G_CALLBACK(on_ctx_inspector_clicked), self));

    gtk_box_append(GTK_BOX(main_page), self->ctx_bg_box);
    gtk_stack_add_named(GTK_STACK(self->ctx_stack), main_page, "main");

    /* ── Page 2: Open With / Set Default Sub-View ── */
    GtkWidget *ow_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *ow_hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(ow_hdr, "shell-popover-header");
    gtk_widget_set_valign(ow_hdr, GTK_ALIGN_CENTER);

    GtkWidget *btn_back = gtk_button_new_with_label("← BACK");
    gtk_widget_add_css_class(btn_back, "flat");
    gtk_widget_add_css_class(btn_back, "files-nav-btn");
    gtk_widget_set_valign(btn_back, GTK_ALIGN_CENTER);
    g_signal_connect(btn_back, "clicked", G_CALLBACK(on_ctx_ow_back_clicked), self);

    self->ctx_ow_mime_lbl = gtk_label_new("MIME");
    gtk_widget_add_css_class(self->ctx_ow_mime_lbl, "shell-popover-badge");
    gtk_widget_set_hexpand(self->ctx_ow_mime_lbl, TRUE);
    gtk_widget_set_halign(self->ctx_ow_mime_lbl, GTK_ALIGN_END);
    gtk_widget_set_valign(self->ctx_ow_mime_lbl, GTK_ALIGN_CENTER);
    gtk_label_set_ellipsize(GTK_LABEL(self->ctx_ow_mime_lbl), PANGO_ELLIPSIZE_MIDDLE);

    gtk_box_append(GTK_BOX(ow_hdr), btn_back);
    gtk_box_append(GTK_BOX(ow_hdr), self->ctx_ow_mime_lbl);
    gtk_box_append(GTK_BOX(ow_page), ow_hdr);

    GtkWidget *ow_body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(ow_body, "shell-popover-body");

    GtkWidget *ow_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ow_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(ow_scroll), 220);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(ow_scroll), TRUE);

    self->ctx_ow_list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(ow_scroll), self->ctx_ow_list_box);
    gtk_box_append(GTK_BOX(ow_body), ow_scroll);

    gtk_box_append(GTK_BOX(ow_body), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    self->ctx_ow_default_btn = gtk_button_new_with_label("☐  Set as Default for this type");
    gtk_widget_add_css_class(self->ctx_ow_default_btn, "flat");
    gtk_widget_add_css_class(self->ctx_ow_default_btn, "files-menu-row");
    g_signal_connect(self->ctx_ow_default_btn, "clicked", G_CALLBACK(on_ctx_ow_default_toggle_clicked), self);
    gtk_box_append(GTK_BOX(ow_body), self->ctx_ow_default_btn);

    gtk_box_append(GTK_BOX(ow_page), ow_body);
    gtk_stack_add_named(GTK_STACK(self->ctx_stack), ow_page, "open_with");

    gtk_popover_set_child(GTK_POPOVER(self->context_popover), self->ctx_stack);
}

static void
open_context_menu_at(KajoFilesWindow *self, KajoFileItem *item, int x, int y)
{
    if (!self || !self->context_popover)
        return;

    gtk_stack_set_visible_child_name(GTK_STACK(self->ctx_stack), "main");

    gboolean is_trash = FALSE;
    if (self->current_dir) {
        g_autofree char *scheme = g_file_get_uri_scheme(self->current_dir);
        is_trash = (g_strcmp0(scheme, "trash") == 0);
    }

    if (item) {
        gboolean is_dir = kajo_file_item_is_dir(item);
        const char *ctype = kajo_file_item_get_content_type(item);
        const char *name = kajo_file_item_get_name(item);
        g_autofree char *sz_str = kajo_files_format_size(kajo_file_item_get_size(item), is_dir);

        gtk_image_set_from_icon_name(GTK_IMAGE(self->ctx_hdr_icon), kajo_file_item_get_icon_name(item));
        gtk_label_set_text(GTK_LABEL(self->ctx_hdr_title_lbl), kajo_file_item_get_display_name(item));
        gtk_label_set_text(GTK_LABEL(self->ctx_hdr_badge_lbl), sz_str);

        gtk_widget_set_visible(self->ctx_item_box, TRUE);
        gtk_widget_set_visible(self->ctx_bg_box, FALSE);

        gtk_widget_set_visible(self->ctx_btn_open_with, !is_dir);
        gtk_widget_set_visible(self->ctx_btn_extract, !is_dir && kajo_files_is_archive(ctype, name));
        gtk_widget_set_visible(self->ctx_btn_item_terminal, is_dir);
        gtk_widget_set_visible(self->ctx_btn_item_newwin, is_dir);
        gtk_widget_set_visible(self->ctx_btn_restore_trash, is_trash);
        gtk_widget_set_visible(self->ctx_btn_move_trash, !is_trash);
    } else {
        g_autofree char *base = self->current_dir ? g_file_get_basename(self->current_dir) : NULL;
        gtk_image_set_from_icon_name(GTK_IMAGE(self->ctx_hdr_icon), "folder");
        gtk_label_set_text(GTK_LABEL(self->ctx_hdr_title_lbl), base ? base : "CURRENT FOLDER");
        gtk_label_set_text(GTK_LABEL(self->ctx_hdr_badge_lbl), "DIR");

        gtk_widget_set_visible(self->ctx_item_box, FALSE);
        gtk_widget_set_visible(self->ctx_bg_box, TRUE);
        gtk_widget_set_sensitive(self->ctx_btn_bg_paste, kajo_files_clipboard_has_files());
    }

    GdkRectangle rect = { MAX(1, x), MAX(1, y), 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(self->context_popover), &rect);
    gtk_popover_popup(GTK_POPOVER(self->context_popover));
}

void
files_window_open_context_menu_for_selection(KajoFilesWindow *self)
{
    if (!self) return;

    int x = gtk_widget_get_width(GTK_WIDGET(self->view_stack)) / 2;
    int y = gtk_widget_get_height(GTK_WIDGET(self->view_stack)) / 3;

    open_context_menu_at(self, self->selected_item, x, y);
}

void
on_view_stack_right_click(GtkGestureClick *gesture G_GNUC_UNUSED,
                          int n_press,
                          double x,
                          double y,
                          gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (n_press != 1) return;

    GtkWidget *picked = gtk_widget_pick(GTK_WIDGET(self->view_stack), x, y, GTK_PICK_DEFAULT);
    KajoFileItem *hit_item = NULL;

    if (picked) {
        /* Ignore right-clicks on the Details List column header buttons */
        if (picked == self->col_btn_name || picked == self->col_btn_size ||
            picked == self->col_btn_type || picked == self->col_btn_modified ||
            gtk_widget_get_ancestor(picked, GTK_TYPE_BUTTON) != NULL) {
            return;
        }

        for (GtkWidget *w = picked; w && w != GTK_WIDGET(self->view_stack); w = gtk_widget_get_parent(w)) {
            KajoFileItem *cand = g_object_get_data(G_OBJECT(w), "file-item");
            if (cand) {
                hit_item = cand;
                break;
            }
        }

        if (hit_item && self->visible_items) {
            for (guint i = 0; i < self->visible_items->len; i++) {
                if (g_ptr_array_index(self->visible_items, i) == hit_item) {
                    files_window_set_selected_item_by_index(self, (int)i);
                    break;
                }
            }
        }
    }

    open_context_menu_at(self, hit_item, (int)x, (int)y);
}
