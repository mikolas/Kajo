#include "files_window_private.h"

/* ─── Keyboard Shortcuts Controller (Space Quick Look, Navigation, Clipboard) ─── */

static gboolean
is_text_input_focused(GtkWindow *win)
{
    GtkWidget *focus = gtk_window_get_focus(win);
    if (!focus) return FALSE;
    return (GTK_IS_EDITABLE(focus) ||
            GTK_IS_ENTRY(focus) ||
            GTK_IS_SEARCH_ENTRY(focus) ||
            GTK_IS_PASSWORD_ENTRY(focus) ||
            gtk_widget_get_ancestor(focus, GTK_TYPE_ENTRY) != NULL ||
            gtk_widget_get_ancestor(focus, GTK_TYPE_SEARCH_ENTRY) != NULL ||
            gtk_widget_get_ancestor(focus, GTK_TYPE_PASSWORD_ENTRY) != NULL);
}

gboolean
files_window_on_key_pressed(GtkEventControllerKey *controller G_GNUC_UNUSED,
                      guint keyval,
                      guint keycode G_GNUC_UNUSED,
                      GdkModifierType state,
                      gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    gboolean ctrl = (state & GDK_CONTROL_MASK) != 0;
    gboolean shift = (state & GDK_SHIFT_MASK) != 0;
    gboolean alt = (state & GDK_ALT_MASK) != 0;
    gboolean ql_open = kajo_quicklook_is_visible(self->quicklook);

    /* 1. Escape closes Quick Look, inline banners, URI entry, or cancels active file ops */
    if (keyval == GDK_KEY_Escape) {
        if (ql_open) {
            kajo_quicklook_hide(self->quicklook);
            return TRUE;
        }
        if (gtk_revealer_get_reveal_child(GTK_REVEALER(self->prompt_banner_revealer))) {
            on_prompt_cancel_clicked(NULL, self);
            return TRUE;
        }
        if (gtk_revealer_get_reveal_child(GTK_REVEALER(self->connect_banner_revealer))) {
            on_connect_cancel_clicked(NULL, self);
            return TRUE;
        }
        if (g_strcmp0(gtk_stack_get_visible_child_name(self->location_stack), "entry") == 0) {
            gtk_stack_set_visible_child_name(self->location_stack, "breadcrumbs");
            return TRUE;
        }
        if (self->active_ops_count > 0) {
            files_window_cancel_active_ops(self);
            return TRUE;
        }
    }

    /* 2. When Quick Look is open, Space / Enter / Arrow keys control Quick Look directly */
    if (ql_open) {
        if (keyval == GDK_KEY_space) {
            kajo_quicklook_hide(self->quicklook);
            return TRUE;
        }
        if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
            KajoFileItem *it = self->selected_item;
            kajo_quicklook_hide(self->quicklook);
            if (it) files_window_activate_file_item(self, it);
            return TRUE;
        }
        if (keyval == GDK_KEY_Left || keyval == GDK_KEY_Up) {
            int idx = files_window_get_selected_item_index(self);
            if (idx > 0) files_window_set_selected_item_by_index(self, idx - 1);
            return TRUE;
        }
        if (keyval == GDK_KEY_Right || keyval == GDK_KEY_Down) {
            int idx = files_window_get_selected_item_index(self);
            if (idx >= 0 && self->visible_items && idx + 1 < (int)self->visible_items->len)
                files_window_set_selected_item_by_index(self, idx + 1);
            return TRUE;
        }
        return FALSE;
    }

    /* 3. Global Ctrl / Alt / Function Shortcuts */
    if (ctrl && !shift && !alt && (keyval == GDK_KEY_l || keyval == GDK_KEY_L)) {
        on_btn_edit_uri_clicked(NULL, self);
        return TRUE;
    }
    if (ctrl && !shift && !alt && (keyval == GDK_KEY_f || keyval == GDK_KEY_F)) {
        gtk_widget_grab_focus(self->search_entry);
        return TRUE;
    }
    if (ctrl && !shift && !alt && (keyval == GDK_KEY_h || keyval == GDK_KEY_H)) {
        on_btn_hidden_clicked(NULL, self);
        return TRUE;
    }
    if (ctrl && !shift && !alt && (keyval == GDK_KEY_i || keyval == GDK_KEY_I)) {
        on_btn_inspector_clicked(NULL, self);
        return TRUE;
    }
    if (ctrl && !shift && !alt && keyval == GDK_KEY_1) {
        files_window_set_view_mode(self, KAJO_VIEW_GRID);
        return TRUE;
    }
    if (ctrl && !shift && !alt && keyval == GDK_KEY_2) {
        files_window_set_view_mode(self, KAJO_VIEW_LIST);
        return TRUE;
    }
    if (ctrl && !shift && !alt && (keyval == GDK_KEY_r || keyval == GDK_KEY_R)) {
        files_window_reload_directory(self);
        return TRUE;
    }
    if (ctrl && alt && !shift && (keyval == GDK_KEY_n || keyval == GDK_KEY_N)) {
        files_window_show_inline_prompt(self, PROMPT_NEW_FILE);
        return TRUE;
    }
    if (ctrl && !shift && !alt && (keyval == GDK_KEY_n || keyval == GDK_KEY_N)) {
        GtkApplication *app = gtk_window_get_application(GTK_WINDOW(self));
        g_autofree char *uri = self->current_dir ? g_file_get_uri(self->current_dir) : NULL;
        GtkWidget *new_win = kajo_files_window_new(app, uri);
        gtk_window_present(GTK_WINDOW(new_win));
        return TRUE;
    }
    if (ctrl && shift && !alt && (keyval == GDK_KEY_n || keyval == GDK_KEY_N)) {
        files_window_show_inline_prompt(self, PROMPT_MKDIR);
        return TRUE;
    }
    if (ctrl && shift && !alt && (keyval == GDK_KEY_a || keyval == GDK_KEY_A)) {
        files_window_show_inline_prompt(self, PROMPT_COMPRESS);
        return TRUE;
    }
    if (ctrl && shift && (keyval == GDK_KEY_t || keyval == GDK_KEY_T)) {
        if (self->current_dir) kajo_files_open_terminal_at(self->current_dir);
        return TRUE;
    }
    if (ctrl && shift && (keyval == GDK_KEY_c || keyval == GDK_KEY_C)) {
        files_window_copy_selected_item_path(self);
        return TRUE;
    }
    if (keyval == GDK_KEY_Menu || (shift && keyval == GDK_KEY_F10)) {
        files_window_open_context_menu_for_selection(self);
        return TRUE;
    }
    if (alt && keyval == GDK_KEY_Left) {
        on_btn_back_clicked(NULL, self);
        return TRUE;
    }
    if (alt && keyval == GDK_KEY_Right) {
        on_btn_forward_clicked(NULL, self);
        return TRUE;
    }
    if (alt && keyval == GDK_KEY_Up) {
        on_btn_up_clicked(NULL, self);
        return TRUE;
    }
    if (keyval == GDK_KEY_F5) {
        files_window_reload_directory(self);
        return TRUE;
    }
    if (keyval == GDK_KEY_F2) {
        files_window_show_inline_prompt(self, PROMPT_RENAME);
        return TRUE;
    }

    /* 4. Shortcuts that only fire when a text entry is NOT focused */
    if (!is_text_input_focused(GTK_WINDOW(self))) {
        if (keyval == GDK_KEY_period && !ctrl && !alt) {
            files_window_open_context_menu_for_selection(self);
            return TRUE;
        }
        if (keyval == GDK_KEY_space && !ctrl && !alt) {
            if (self->selected_item) {
                kajo_quicklook_toggle(self->quicklook);
                return TRUE;
            }
        }
        if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) && !ctrl && !alt) {
            if (self->selected_item) {
                files_window_activate_file_item(self, self->selected_item);
                return TRUE;
            }
        }
        if (keyval == GDK_KEY_BackSpace && !ctrl && !alt) {
            on_btn_up_clicked(NULL, self);
            return TRUE;
        }
        if (keyval == GDK_KEY_Delete) {
            if (shift)
                files_window_show_inline_prompt(self, PROMPT_DELETE_PERM);
            else
                files_window_trash_selected_item(self);
            return TRUE;
        }
        if (ctrl && !shift && !alt && (keyval == GDK_KEY_a || keyval == GDK_KEY_A)) {
            files_window_select_all(self);
            return TRUE;
        }
        if (ctrl && !shift && !alt && (keyval == GDK_KEY_c || keyval == GDK_KEY_C)) {
            g_autoptr(GPtrArray) sel_items = files_window_get_selected_items(self);
            if (sel_items && sel_items->len > 0) {
                kajo_files_clipboard_set(sel_items, FALSE);
                gtk_widget_set_sensitive(self->btn_paste, !files_window_is_read_only_archive(self));
                g_autofree char *msg = g_strdup_printf("Copied %u item(s) to clipboard", sel_items->len);
                files_window_set_status_message(self, msg, FALSE);
                return TRUE;
            }
        }
        if (ctrl && !shift && !alt && (keyval == GDK_KEY_x || keyval == GDK_KEY_X)) {
            if (files_window_is_read_only_archive(self)) {
                files_window_set_status_message(self, "Archive view is read-only", TRUE);
                return TRUE;
            }
            g_autoptr(GPtrArray) sel_items = files_window_get_selected_items(self);
            if (sel_items && sel_items->len > 0) {
                kajo_files_clipboard_set(sel_items, TRUE);
                gtk_widget_set_sensitive(self->btn_paste, TRUE);
                g_autofree char *msg = g_strdup_printf("Cut %u item(s) to clipboard", sel_items->len);
                files_window_set_status_message(self, msg, FALSE);
                return TRUE;
            }
        }
        if (ctrl && !shift && !alt && (keyval == GDK_KEY_v || keyval == GDK_KEY_V)) {
            if (self->current_dir) {
                on_btn_paste_clicked(NULL, self);
                return TRUE;
            }
        }
    }

    return FALSE;
}
