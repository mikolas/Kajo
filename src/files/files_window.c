#include "files_window_private.h"
#include "files_diskimage.h"
#include "../config.h"
#include <glib/gstdio.h>
#include <errno.h>

G_DEFINE_TYPE(KajoFilesWindow, kajo_files_window, GTK_TYPE_APPLICATION_WINDOW)

static void
mutate_files_prefs(KajoFilesCfg *draft, gpointer user_data)
{
    KajoFilesWindow *self = (KajoFilesWindow *)user_data;
    draft->view_mode          = (KajoFilesViewMode)self->view_mode;
    draft->sort_field         = (KajoFilesSortField)self->sort_field;
    draft->sort_ascending     = self->sort_ascending;
    draft->sort_folders_first = self->sort_folders_first;
    draft->show_hidden        = self->show_hidden;
    draft->show_inspector     = self->inspector_visible;
}

static void
files_window_save_prefs(KajoFilesWindow *self)
{
    if (!self) return;
    kajo_config_store_update_files(NULL, mutate_files_prefs, self, NULL);
}

static void
files_window_load_prefs(KajoFilesWindow *self)
{
    if (!self) return;
    g_autoptr(KajoFilesCfgRef) cfg = kajo_config_store_acquire_files(NULL);
    if (!cfg) return;

    self->view_mode          = (KajoFileViewMode)cfg->view_mode;
    self->sort_field         = (KajoFileSortField)cfg->sort_field;
    self->sort_ascending     = cfg->sort_ascending;
    self->sort_folders_first = cfg->sort_folders_first;
    self->show_hidden        = cfg->show_hidden;
    self->inspector_visible  = cfg->show_inspector;
}

void files_window_set_selected_item_by_index(KajoFilesWindow *self, int idx);
void files_window_activate_file_item(KajoFilesWindow *self, KajoFileItem *item);

/* ─── Status Bar & Capacity Helpers ─── */

void
files_window_set_status_message(KajoFilesWindow *self, const char *msg, gboolean is_error)
{
    if (!self || !self->status_left_label) return;
    gtk_label_set_text(GTK_LABEL(self->status_left_label), msg ? msg : "");
    if (is_error)
        gtk_widget_add_css_class(self->status_left_label, "files-status-error");
    else
        gtk_widget_remove_css_class(self->status_left_label, "files-status-error");
}

static void
update_status_and_capacity_bar(KajoFilesWindow *self)
{
    if (!self) return;

    guint total_vis = self->visible_items ? self->visible_items->len : 0;
    guint dirs = 0, files = 0;

    for (guint i = 0; i < total_vis; i++) {
        KajoFileItem *it = g_ptr_array_index(self->visible_items, i);
        if (kajo_file_item_is_dir(it))
            dirs++;
        else
            files++;
    }

    g_autoptr(GPtrArray) sel_items = files_window_get_selected_items(self);
    if (sel_items && sel_items->len > 1) {
        guint64 sel_bytes = 0;
        for (guint i = 0; i < sel_items->len; i++) {
            KajoFileItem *it = g_ptr_array_index(sel_items, i);
            if (!kajo_file_item_is_dir(it))
                sel_bytes += kajo_file_item_get_size(it);
        }
        g_autofree char *sel_sz = kajo_files_format_size(sel_bytes, FALSE);
        g_autofree char *left = g_strdup_printf(
            "%u ITEMS (%u FOLDERS, %u FILES)  |  SELECTED: %u ITEMS (%s)",
            total_vis, dirs, files,
            sel_items->len,
            sel_sz
        );
        files_window_set_status_message(self, left, FALSE);
    } else if (self->selected_item) {
        g_autofree char *sel_sz = kajo_files_format_size(
            kajo_file_item_get_size(self->selected_item),
            kajo_file_item_is_dir(self->selected_item)
        );
        g_autofree char *left = g_strdup_printf(
            "%u ITEMS (%u FOLDERS, %u FILES)  |  SELECTED: %s (%s)",
            total_vis, dirs, files,
            kajo_file_item_get_display_name(self->selected_item),
            sel_sz
        );
        files_window_set_status_message(self, left, FALSE);
    } else {
        g_autofree char *left = g_strdup_printf(
            "%u ITEMS (%u FOLDERS, %u FILES)",
            total_vis, dirs, files
        );
        files_window_set_status_message(self, left, FALSE);
    }

    if (self->current_dir) {
        g_autoptr(GFileInfo) fs_info = g_file_query_filesystem_info(
            self->current_dir,
            G_FILE_ATTRIBUTE_FILESYSTEM_SIZE "," G_FILE_ATTRIBUTE_FILESYSTEM_FREE,
            NULL,
            NULL
        );
        if (fs_info && g_file_info_has_attribute(fs_info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE)) {
            guint64 total_b = g_file_info_get_attribute_uint64(fs_info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
            guint64 free_b = g_file_info_get_attribute_uint64(fs_info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
            if (total_b > 0) {
                g_autofree char *free_s = kajo_files_format_size(free_b, FALSE);
                g_autofree char *tot_s = kajo_files_format_size(total_b, FALSE);
                g_autofree char *right = g_strdup_printf("FREE: %s / %s", free_s, tot_s);
                gtk_label_set_text(GTK_LABEL(self->status_right_label), right);

                double frac = (double)(total_b - free_b) / (double)total_b;
                gtk_level_bar_set_value(GTK_LEVEL_BAR(self->fs_usage_bar), CLAMP(frac, 0.0, 1.0));
                gtk_widget_set_visible(self->fs_usage_bar, TRUE);
                return;
            }
        }
    }

    gtk_label_set_text(
        GTK_LABEL(self->status_right_label),
        files_window_is_read_only_archive(self) ? "READ-ONLY ARCHIVE" : "GVFS VIRTUAL FS"
    );
    gtk_widget_set_visible(self->fs_usage_bar, FALSE);
}

void
files_window_begin_op(KajoFilesWindow *self, const char *status_msg)
{
    if (!self) return;
    self->active_ops_count++;
    if (self->btn_cancel_op) {
        gtk_widget_set_visible(self->btn_cancel_op, TRUE);
        gtk_widget_set_sensitive(self->btn_cancel_op, TRUE);
    }
    if (status_msg)
        files_window_set_status_message(self, status_msg, FALSE);
}

void
files_window_cancel_active_ops(KajoFilesWindow *self)
{
    if (!self) return;
    if (self->ops_cancel) {
        g_cancellable_cancel(self->ops_cancel);
        g_clear_object(&self->ops_cancel);
    }
    self->ops_cancel = g_cancellable_new();
    /* The counter is NOT reset: every cancelled operation still reports back (with
     * G_IO_ERROR_CANCELLED) and decrements it then, so a newly started operation can
     * never be miscounted by a late completion of a cancelled one. */
    if (self->btn_cancel_op)
        gtk_widget_set_sensitive(self->btn_cancel_op, FALSE);
    files_window_set_status_message(self, "Cancelling…", FALSE);
}

static void
on_btn_cancel_op_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    files_window_cancel_active_ops(KAJO_FILES_WINDOW(user_data));
}

/* File-operation callbacks run after a worker thread finishes, possibly after
 * the window was closed. They receive a heap GWeakRef (never a raw window
 * pointer) so a destroyed window is detected instead of dereferenced. */
gpointer
files_window_op_weak_new(KajoFilesWindow *self)
{
    GWeakRef *ref = g_new0(GWeakRef, 1);
    g_weak_ref_init(ref, self);
    return ref;
}

void
files_window_on_file_op_completed(gboolean success, const char *message, GError *error, gpointer user_data)
{
    GWeakRef *ref = user_data;
    KajoFilesWindow *self = g_weak_ref_get(ref);
    g_weak_ref_clear(ref);
    g_free(ref);
    if (!self) return;

    if (self->active_ops_count > 0)
        self->active_ops_count--;
    if (self->btn_cancel_op && self->active_ops_count == 0)
        gtk_widget_set_visible(self->btn_cancel_op, FALSE);

    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        files_window_set_status_message(
            self, "Operation cancelled - items already processed stay changed", FALSE);
        files_window_reload_directory(self);
        if (self->sidebar)
            kajo_files_sidebar_refresh(self->sidebar);
        g_object_unref(self);
        return;
    }

    if (!success) {
        files_window_set_status_message(self, error ? error->message : "Operation failed", TRUE);
    } else if (message) {
        files_window_set_status_message(self, message, FALSE);
    }

    files_window_reload_directory(self);
    if (self->sidebar)
        kajo_files_sidebar_refresh(self->sidebar);
    g_object_unref(self);
}

/* ─── Contextual Banners (Connect Server / Trash / Inline Prompts) ─── */

static void
on_connect_submit_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    const char *uri = gtk_editable_get_text(GTK_EDITABLE(self->connect_entry));
    gtk_revealer_set_reveal_child(GTK_REVEALER(self->connect_banner_revealer), FALSE);
    if (uri && uri[0] != '\0') {
        kajo_files_window_navigate_uri(self, uri);
    }
}

void
on_connect_cancel_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    gtk_revealer_set_reveal_child(GTK_REVEALER(self->connect_banner_revealer), FALSE);
}

static void
build_connect_banner(KajoFilesWindow *self)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(card, "files-prompt-bar");
    gtk_widget_add_css_class(card, "files-context-banner");

    GtkWidget *lbl = gtk_label_new("🌐 CONNECT TO SERVER (smb://, sftp://, ftp://, davs://, mtp://, onedrive://)");
    gtk_widget_add_css_class(lbl, "heading");
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(card), lbl);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    self->connect_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(self->connect_entry), "smb://server.local/share or sftp://user@host/path");
    gtk_widget_set_hexpand(self->connect_entry, TRUE);
    g_signal_connect(self->connect_entry, "activate", G_CALLBACK(on_connect_submit_clicked), self);

    GtkWidget *btn_ok = gtk_button_new_with_label("CONNECT");
    gtk_widget_add_css_class(btn_ok, "action-btn-primary");
    g_signal_connect(btn_ok, "clicked", G_CALLBACK(on_connect_submit_clicked), self);

    GtkWidget *btn_cancel = gtk_button_new_with_label("CANCEL");
    gtk_widget_add_css_class(btn_cancel, "flat");
    g_signal_connect(btn_cancel, "clicked", G_CALLBACK(on_connect_cancel_clicked), self);

    gtk_box_append(GTK_BOX(hbox), self->connect_entry);
    gtk_box_append(GTK_BOX(hbox), btn_ok);
    gtk_box_append(GTK_BOX(hbox), btn_cancel);
    gtk_box_append(GTK_BOX(card), hbox);

    gtk_revealer_set_child(GTK_REVEALER(self->connect_banner_revealer), card);
}

void
on_trash_restore_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    g_autoptr(GPtrArray) sel_items = files_window_get_selected_items(self);
    if (!sel_items || sel_items->len == 0) {
        files_window_set_status_message(self, "Select an item in Trash to restore", TRUE);
        return;
    }
    files_window_begin_op(self, "Restoring from Trash...");
    kajo_files_op_restore_from_trash_async(sel_items, self->ops_cancel, files_window_on_file_op_completed, files_window_op_weak_new(self));
}

static void
on_trash_empty_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    files_window_begin_op(self, "Emptying Trash...");
    kajo_files_op_empty_trash_async(self->ops_cancel, files_window_on_file_op_completed, files_window_op_weak_new(self));
}

static void
build_trash_banner(KajoFilesWindow *self)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(card, "files-trash-bar");
    gtk_widget_add_css_class(card, "files-context-banner");

    self->trash_summary_lbl = gtk_label_new("🗑 TRASH // RECYCLE BIN");
    gtk_widget_add_css_class(self->trash_summary_lbl, "heading");
    gtk_widget_set_hexpand(self->trash_summary_lbl, TRUE);
    gtk_widget_set_halign(self->trash_summary_lbl, GTK_ALIGN_START);

    GtkWidget *btn_restore = gtk_button_new_with_label("↺ RESTORE SELECTED");
    gtk_widget_add_css_class(btn_restore, "flat");
    g_signal_connect(btn_restore, "clicked", G_CALLBACK(on_trash_restore_clicked), self);

    GtkWidget *btn_empty = gtk_button_new_with_label("🔥 EMPTY TRASH");
    gtk_widget_add_css_class(btn_empty, "flat");
    gtk_widget_add_css_class(btn_empty, "action-btn-danger");
    g_signal_connect(btn_empty, "clicked", G_CALLBACK(on_trash_empty_clicked), self);

    gtk_box_append(GTK_BOX(card), self->trash_summary_lbl);
    gtk_box_append(GTK_BOX(card), btn_restore);
    gtk_box_append(GTK_BOX(card), btn_empty);

    gtk_revealer_set_child(GTK_REVEALER(self->trash_banner_revealer), card);
}

gboolean
files_window_is_read_only_archive(const KajoFilesWindow *self)
{
    if (!self || !self->current_dir)
        return FALSE;
    g_autofree char *uri = g_file_get_uri(self->current_dir);
    return kajo_files_parse_archive_uri(uri, NULL, NULL);
}

/* A nested archive was extracted into the preview cache; walk the recorded origins back to the
 * archive that really lives in the user's file system and return its parent folder. */
static GFile *
files_window_real_host_parent(KajoFilesWindow *self, const char *arch_path)
{
    g_autofree char *cur = g_strdup(arch_path);
    for (int depth = 0; depth < 16 && cur; depth++) {
        const char *origin_uri = self->nested_archive_origin
            ? g_hash_table_lookup(self->nested_archive_origin, cur) : NULL;
        if (!origin_uri)
            break;
        g_autofree char *outer_arch = NULL;
        if (!kajo_files_parse_archive_uri(origin_uri, &outer_arch, NULL))
            break;
        g_free(cur);
        cur = g_steal_pointer(&outer_arch);
    }
    g_autoptr(GFile) f = cur ? g_file_new_for_path(cur) : NULL;
    return f ? g_file_get_parent(f) : NULL;
}

static void
on_archive_extract_all_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (!self || !self->current_dir)
        return;

    g_autofree char *uri = g_file_get_uri(self->current_dir);
    g_autofree char *arch_path = NULL;
    if (!uri || !kajo_files_parse_archive_uri(uri, &arch_path, NULL))
        return;

    g_autoptr(GFile) arch_file = g_file_new_for_path(arch_path);
    g_autoptr(GFile) host_parent = files_window_real_host_parent(self, arch_path);
    if (!arch_file || !host_parent)
        return;

    files_window_begin_op(self, "Extracting archive...");
    kajo_files_op_extract_archive_async(
        arch_file,
        host_parent,
        self->ops_cancel,
        files_window_on_file_op_completed,
        files_window_op_weak_new(self)
    );
}

static void
on_archive_exit_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (!self || !self->current_dir)
        return;

    g_autofree char *uri = g_file_get_uri(self->current_dir);
    g_autofree char *arch_path = NULL;
    if (!uri || !kajo_files_parse_archive_uri(uri, &arch_path, NULL))
        return;

    /* Exit from a nested archive returns to the folder of the outer archive it was opened from. */
    const char *origin_uri = self->nested_archive_origin
        ? g_hash_table_lookup(self->nested_archive_origin, arch_path) : NULL;
    if (origin_uri) {
        g_autofree char *origin_copy = g_strdup(origin_uri);
        kajo_files_window_navigate_uri(self, origin_copy);
        return;
    }

    g_autoptr(GFile) arch_file = g_file_new_for_path(arch_path);
    g_autoptr(GFile) host_parent = arch_file ? g_file_get_parent(arch_file) : NULL;
    if (host_parent) {
        g_autofree char *parent_uri = g_file_get_uri(host_parent);
        if (parent_uri)
            kajo_files_window_navigate_uri(self, parent_uri);
    }
}

static void
build_archive_banner(KajoFilesWindow *self)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(card, "files-prompt-bar");
    gtk_widget_add_css_class(card, "files-context-banner");
    gtk_widget_set_valign(card, GTK_ALIGN_CENTER);

    self->archive_summary_lbl = gtk_label_new("📦 ARCHIVE VIEW (READ-ONLY)");
    gtk_widget_add_css_class(self->archive_summary_lbl, "heading");
    gtk_widget_set_hexpand(self->archive_summary_lbl, TRUE);
    gtk_label_set_xalign(GTK_LABEL(self->archive_summary_lbl), 0.0f);
    gtk_widget_set_valign(self->archive_summary_lbl, GTK_ALIGN_CENTER);
    gtk_label_set_ellipsize(GTK_LABEL(self->archive_summary_lbl), PANGO_ELLIPSIZE_END);

    GtkWidget *btn_extract_all = gtk_button_new_with_label("📦 EXTRACT ALL");
    gtk_widget_add_css_class(btn_extract_all, "action-btn-primary");
    gtk_widget_set_valign(btn_extract_all, GTK_ALIGN_CENTER);
    g_signal_connect(btn_extract_all, "clicked", G_CALLBACK(on_archive_extract_all_clicked), self);

    GtkWidget *btn_exit = gtk_button_new_with_label("↩ EXIT");
    gtk_widget_add_css_class(btn_exit, "flat");
    gtk_widget_set_valign(btn_exit, GTK_ALIGN_CENTER);
    g_signal_connect(btn_exit, "clicked", G_CALLBACK(on_archive_exit_clicked), self);

    gtk_box_append(GTK_BOX(card), self->archive_summary_lbl);
    gtk_box_append(GTK_BOX(card), btn_extract_all);
    gtk_box_append(GTK_BOX(card), btn_exit);

    gtk_revealer_set_child(GTK_REVEALER(self->archive_banner_revealer), card);
}

static void
on_prompt_submit_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    InlinePromptKind kind = self->active_prompt;
    self->active_prompt = PROMPT_NONE;
    gtk_revealer_set_reveal_child(GTK_REVEALER(self->prompt_banner_revealer), FALSE);

    const char *text = gtk_editable_get_text(GTK_EDITABLE(self->prompt_entry));

    if (kind == PROMPT_MKDIR && self->current_dir) {
        files_window_begin_op(self, "Creating folder...");
        kajo_files_op_mkdir_async(self->current_dir, text, self->ops_cancel, files_window_on_file_op_completed, files_window_op_weak_new(self));
    } else if (kind == PROMPT_NEW_FILE && self->current_dir) {
        files_window_begin_op(self, "Creating file...");
        kajo_files_op_create_file_async(self->current_dir, text, self->ops_cancel, files_window_on_file_op_completed, files_window_op_weak_new(self));
    } else if (kind == PROMPT_RENAME && self->selected_item) {
        files_window_begin_op(self, "Renaming...");
        kajo_files_op_rename_async(kajo_file_item_get_file(self->selected_item), text, self->ops_cancel, files_window_on_file_op_completed, files_window_op_weak_new(self));
    } else if (kind == PROMPT_DELETE_PERM) {
        g_autoptr(GPtrArray) sel_items = files_window_get_selected_items(self);
        if (sel_items && sel_items->len > 0) {
            files_window_begin_op(self, "Deleting permanently...");
            kajo_files_op_delete_permanent_async(sel_items, self->ops_cancel, files_window_on_file_op_completed, files_window_op_weak_new(self));
        }
    } else if (kind == PROMPT_COMPRESS && self->current_dir) {
        g_autoptr(GPtrArray) sel_items = files_window_get_selected_items(self);
        if (sel_items && sel_items->len > 0) {
            files_window_begin_op(self, "Compressing to archive...");
            kajo_files_op_compress_async(sel_items, self->current_dir, text, self->ops_cancel, files_window_on_file_op_completed, files_window_op_weak_new(self));
        }
    }
}

void
on_prompt_cancel_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    self->active_prompt = PROMPT_NONE;
    gtk_revealer_set_reveal_child(GTK_REVEALER(self->prompt_banner_revealer), FALSE);
}

static void
build_prompt_banner(KajoFilesWindow *self)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(card, "files-prompt-bar");
    gtk_widget_add_css_class(card, "files-context-banner");

    self->prompt_title_lbl = gtk_label_new("ACTION:");
    gtk_widget_add_css_class(self->prompt_title_lbl, "heading");

    self->prompt_entry = gtk_entry_new();
    gtk_widget_set_hexpand(self->prompt_entry, TRUE);
    g_signal_connect(self->prompt_entry, "activate", G_CALLBACK(on_prompt_submit_clicked), self);

    self->prompt_submit_btn = gtk_button_new_with_label("CONFIRM");
    gtk_widget_add_css_class(self->prompt_submit_btn, "action-btn-primary");
    g_signal_connect(self->prompt_submit_btn, "clicked", G_CALLBACK(on_prompt_submit_clicked), self);

    GtkWidget *btn_cancel = gtk_button_new_with_label("CANCEL");
    gtk_widget_add_css_class(btn_cancel, "flat");
    g_signal_connect(btn_cancel, "clicked", G_CALLBACK(on_prompt_cancel_clicked), self);

    gtk_box_append(GTK_BOX(card), self->prompt_title_lbl);
    gtk_box_append(GTK_BOX(card), self->prompt_entry);
    gtk_box_append(GTK_BOX(card), self->prompt_submit_btn);
    gtk_box_append(GTK_BOX(card), btn_cancel);

    gtk_revealer_set_child(GTK_REVEALER(self->prompt_banner_revealer), card);
}

void
files_window_show_inline_prompt(KajoFilesWindow *self, InlinePromptKind kind)
{
    if (!self) return;
    if (files_window_is_read_only_archive(self) &&
        (kind == PROMPT_MKDIR || kind == PROMPT_NEW_FILE ||
         kind == PROMPT_RENAME || kind == PROMPT_DELETE_PERM ||
         kind == PROMPT_COMPRESS)) {
        files_window_set_status_message(self, "Archive view is read-only", TRUE);
        return;
    }
    self->active_prompt = kind;

    if (kind == PROMPT_MKDIR) {
        gtk_label_set_text(GTK_LABEL(self->prompt_title_lbl), "📁 NEW FOLDER NAME:");
        gtk_widget_set_visible(self->prompt_entry, TRUE);
        gtk_editable_set_text(GTK_EDITABLE(self->prompt_entry), "New Folder");
        gtk_button_set_label(GTK_BUTTON(self->prompt_submit_btn), "CREATE");
        gtk_revealer_set_reveal_child(GTK_REVEALER(self->prompt_banner_revealer), TRUE);
        gtk_widget_grab_focus(self->prompt_entry);
        gtk_editable_select_region(GTK_EDITABLE(self->prompt_entry), 0, -1);
    } else if (kind == PROMPT_NEW_FILE) {
        gtk_label_set_text(GTK_LABEL(self->prompt_title_lbl), "📄 NEW FILE NAME:");
        gtk_widget_set_visible(self->prompt_entry, TRUE);
        gtk_editable_set_text(GTK_EDITABLE(self->prompt_entry), "untitled.txt");
        gtk_button_set_label(GTK_BUTTON(self->prompt_submit_btn), "CREATE");
        gtk_revealer_set_reveal_child(GTK_REVEALER(self->prompt_banner_revealer), TRUE);
        gtk_widget_grab_focus(self->prompt_entry);
        gtk_editable_select_region(GTK_EDITABLE(self->prompt_entry), 0, 8);
    } else if (kind == PROMPT_RENAME && self->selected_item) {
        g_autoptr(GPtrArray) sel_items = files_window_get_selected_items(self);
        if (sel_items && sel_items->len > 1) {
            self->active_prompt = PROMPT_NONE;
            files_window_set_status_message(self, "Select a single item to rename", TRUE);
            return;
        }
        gtk_label_set_text(GTK_LABEL(self->prompt_title_lbl), "✏ RENAME TO:");
        gtk_widget_set_visible(self->prompt_entry, TRUE);
        gtk_editable_set_text(GTK_EDITABLE(self->prompt_entry), kajo_file_item_get_display_name(self->selected_item));
        gtk_button_set_label(GTK_BUTTON(self->prompt_submit_btn), "RENAME");
        gtk_revealer_set_reveal_child(GTK_REVEALER(self->prompt_banner_revealer), TRUE);
        gtk_widget_grab_focus(self->prompt_entry);
        gtk_editable_select_region(GTK_EDITABLE(self->prompt_entry), 0, -1);
    } else if (kind == PROMPT_DELETE_PERM) {
        g_autoptr(GPtrArray) sel_items = files_window_get_selected_items(self);
        if (!sel_items || sel_items->len == 0)
            return;
        g_autofree char *msg = NULL;
        if (sel_items->len > 1) {
            msg = g_strdup_printf(
                "🔥 PERMANENTLY DELETE %u SELECTED ITEMS? (CANNOT BE UNDONE)",
                sel_items->len
            );
        } else {
            KajoFileItem *first = g_ptr_array_index(sel_items, 0);
            msg = g_strdup_printf(
                "🔥 PERMANENTLY DELETE '%s'? (CANNOT BE UNDONE)",
                kajo_file_item_get_display_name(first)
            );
        }
        gtk_label_set_text(GTK_LABEL(self->prompt_title_lbl), msg);
        gtk_widget_set_visible(self->prompt_entry, FALSE);
        gtk_button_set_label(GTK_BUTTON(self->prompt_submit_btn), "DELETE PERMANENTLY");
        gtk_revealer_set_reveal_child(GTK_REVEALER(self->prompt_banner_revealer), TRUE);
    } else if (kind == PROMPT_COMPRESS) {
        g_autoptr(GPtrArray) sel_items = files_window_get_selected_items(self);
        if (!sel_items || sel_items->len == 0) {
            self->active_prompt = PROMPT_NONE;
            files_window_set_status_message(self, "Select one or more items to compress", TRUE);
            return;
        }

        g_autofree char *stem = NULL;
        if (sel_items->len == 1) {
            KajoFileItem *first = g_ptr_array_index(sel_items, 0);
            const char *nm = kajo_file_item_get_display_name(first);
            const char *dot = (!kajo_file_item_is_dir(first) && nm) ? strrchr(nm, '.') : NULL;
            if (dot && dot != nm)
                stem = g_strndup(nm, (gsize)(dot - nm));
            else if (nm && nm[0] != '\0')
                stem = g_strdup(nm);
        } else if (self->current_dir) {
            stem = g_file_get_basename(self->current_dir);
        }
        if (!stem || stem[0] == '\0' || g_strcmp0(stem, "/") == 0) {
            g_free(stem);
            stem = g_strdup("archive");
        }

        g_autofree char *default_arch = g_strdup_printf("%s.zip", stem);
        gtk_label_set_text(GTK_LABEL(self->prompt_title_lbl), "📦 COMPRESS TO ARCHIVE (.zip, .tar.zst, .tar.gz, .tar.xz):");
        gtk_widget_set_visible(self->prompt_entry, TRUE);
        gtk_editable_set_text(GTK_EDITABLE(self->prompt_entry), default_arch);
        gtk_button_set_label(GTK_BUTTON(self->prompt_submit_btn), "COMPRESS");
        gtk_revealer_set_reveal_child(GTK_REVEALER(self->prompt_banner_revealer), TRUE);
        gtk_widget_grab_focus(self->prompt_entry);
        gtk_editable_select_region(GTK_EDITABLE(self->prompt_entry), 0, (int)g_utf8_strlen(stem, -1));
    }
}

/* ─── Interactive Breadcrumb Bar ─── */

static void
sync_breadcrumb_scroll_to_end(KajoFilesWindow *self)
{
    if (!self || !self->breadcrumb_scroll || !self->pending_breadcrumb_scroll_end)
        return;

    GtkWidget *last_child = self->breadcrumb_box ? gtk_widget_get_last_child(self->breadcrumb_box) : NULL;
    if (!last_child || gtk_widget_get_width(last_child) <= 0)
        return;

    GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(self->breadcrumb_scroll);
    if (!hadj)
        return;

    double upper = gtk_adjustment_get_upper(hadj);
    double page = gtk_adjustment_get_page_size(hadj);
    if (page > 0.0) {
        if (upper > page) {
            double target = upper - page;
            if (fabs(gtk_adjustment_get_value(hadj) - target) > 0.5) {
                gtk_adjustment_set_value(hadj, target);
            }
        }
        self->pending_breadcrumb_scroll_end = FALSE;
    }
}

static gboolean
on_breadcrumb_scroll_idle(gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    self->breadcrumb_scroll_idle_id = 0;
    sync_breadcrumb_scroll_to_end(self);
    return G_SOURCE_REMOVE;
}

static void
schedule_breadcrumb_scroll_to_end(KajoFilesWindow *self)
{
    if (!self || !self->pending_breadcrumb_scroll_end)
        return;
    if (self->breadcrumb_scroll_idle_id == 0) {
        self->breadcrumb_scroll_idle_id = g_idle_add(on_breadcrumb_scroll_idle, self);
    }
}

static void
on_breadcrumb_hadj_changed(GtkAdjustment *hadj G_GNUC_UNUSED, gpointer user_data)
{
    schedule_breadcrumb_scroll_to_end(KAJO_FILES_WINDOW(user_data));
}

static void
on_breadcrumb_hadj_notify(GtkAdjustment *hadj G_GNUC_UNUSED,
                          GParamSpec *pspec G_GNUC_UNUSED,
                          gpointer user_data)
{
    schedule_breadcrumb_scroll_to_end(KAJO_FILES_WINDOW(user_data));
}

static gboolean
on_breadcrumb_wheel_scroll(GtkEventControllerScroll *ctrl G_GNUC_UNUSED,
                           double dx,
                           double dy,
                           gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (!self || !self->breadcrumb_scroll)
        return FALSE;

    GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(self->breadcrumb_scroll);
    if (!hadj)
        return FALSE;

    double delta = (fabs(dx) > fabs(dy)) ? dx : dy;
    if (fabs(delta) < 1e-4)
        return FALSE;

    self->pending_breadcrumb_scroll_end = FALSE;
    double val = gtk_adjustment_get_value(hadj);
    double upper = gtk_adjustment_get_upper(hadj);
    double page = gtk_adjustment_get_page_size(hadj);
    double max_val = MAX(0.0, upper - page);

    gtk_adjustment_set_value(hadj, CLAMP(val + delta * 48.0, 0.0, max_val));
    return TRUE;
}

static void
on_bc_pop_subdir_clicked(GtkButton *btn, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    const char *uri = g_object_get_data(G_OBJECT(btn), "target-uri");
    if (self->breadcrumb_popover)
        gtk_popover_popdown(GTK_POPOVER(self->breadcrumb_popover));
    if (uri)
        kajo_files_window_navigate_uri(self, uri);
}

static void
on_bc_pop_newwin_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (self->breadcrumb_popover)
        gtk_popover_popdown(GTK_POPOVER(self->breadcrumb_popover));
    if (!self->bc_pop_target_uri)
        return;

    GtkApplication *app = gtk_window_get_application(GTK_WINDOW(self));
    GtkWidget *win = kajo_files_window_new(app, self->bc_pop_target_uri);
    gtk_window_present(GTK_WINDOW(win));
}

static void
on_bc_pop_copypath_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (self->breadcrumb_popover)
        gtk_popover_popdown(GTK_POPOVER(self->breadcrumb_popover));
    if (!self->bc_pop_target_uri)
        return;

    g_autoptr(GFile) f = g_file_new_for_uri(self->bc_pop_target_uri);
    g_autofree char *local_path = f ? g_file_get_path(f) : NULL;
    const char *text = local_path ? local_path : self->bc_pop_target_uri;

    GdkDisplay *display = gdk_display_get_default();
    if (display) {
        GdkClipboard *clipboard = gdk_display_get_clipboard(display);
        gdk_clipboard_set_text(clipboard, text);
    }
    g_autofree char *msg = g_strdup_printf("Copied path: %s", text);
    files_window_set_status_message(self, msg, FALSE);
}

static gint
compare_bc_dir_items(gconstpointer a, gconstpointer b)
{
    const KajoFileItem *ia = *(const KajoFileItem *const *)a;
    const KajoFileItem *ib = *(const KajoFileItem *const *)b;
    return kajo_file_item_compare_full(ia, ib, KAJO_SORT_NAME, TRUE, TRUE);
}

static void
on_breadcrumb_subdirs_ready(GPtrArray *items, GError *error, gpointer user_data)
{
    GWeakRef *wr = user_data;
    g_autoptr(KajoFilesWindow) self = g_weak_ref_get(wr);
    g_weak_ref_clear(wr);
    g_free(wr);

    if (!self || !self->bc_pop_list_box) {
        if (items) g_ptr_array_unref(items);
        return;
    }

    if (error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        if (items) g_ptr_array_unref(items);
        return;
    }

    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(self->bc_pop_list_box)) != NULL) {
        gtk_box_remove(GTK_BOX(self->bc_pop_list_box), child);
    }

    g_autoptr(GPtrArray) subdirs = g_ptr_array_new_with_free_func(g_object_unref);
    if (items) {
        for (guint i = 0; i < items->len; i++) {
            KajoFileItem *it = g_ptr_array_index(items, i);
            if (!kajo_file_item_is_dir(it))
                continue;
            if (!self->show_hidden && kajo_file_item_is_hidden(it))
                continue;
            g_ptr_array_add(subdirs, g_object_ref(it));
        }
        g_ptr_array_unref(items);
    }

    g_ptr_array_sort(subdirs, compare_bc_dir_items);

    if (subdirs->len == 0) {
        GtkWidget *empty_lbl = gtk_label_new(error ? error->message : "No subdirectories");
        gtk_widget_add_css_class(empty_lbl, "shell-popover-subtitle");
        gtk_widget_set_halign(empty_lbl, GTK_ALIGN_START);
        gtk_widget_set_margin_start(empty_lbl, 8);
        gtk_widget_set_margin_top(empty_lbl, 4);
        gtk_widget_set_margin_bottom(empty_lbl, 4);
        gtk_box_append(GTK_BOX(self->bc_pop_list_box), empty_lbl);
        return;
    }

    guint limit = MIN(subdirs->len, 50U);
    for (guint i = 0; i < limit; i++) {
        KajoFileItem *it = g_ptr_array_index(subdirs, i);
        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_valign(hbox, GTK_ALIGN_CENTER);

        GtkWidget *ic = gtk_image_new_from_icon_name("folder");
        gtk_image_set_pixel_size(GTK_IMAGE(ic), 16);
        gtk_widget_set_valign(ic, GTK_ALIGN_CENTER);

        GtkWidget *lbl = gtk_label_new(kajo_file_item_get_display_name(it));
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_widget_set_valign(lbl, GTK_ALIGN_CENTER);
        gtk_widget_set_hexpand(lbl, TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_MIDDLE);

        GtkWidget *chev = gtk_label_new("›");
        gtk_widget_add_css_class(chev, "sidebar-chevron");
        gtk_widget_set_valign(chev, GTK_ALIGN_CENTER);

        gtk_box_append(GTK_BOX(hbox), ic);
        gtk_box_append(GTK_BOX(hbox), lbl);
        gtk_box_append(GTK_BOX(hbox), chev);

        GtkWidget *row_btn = gtk_button_new();
        gtk_widget_add_css_class(row_btn, "flat");
        gtk_widget_add_css_class(row_btn, "files-menu-row");
        gtk_button_set_child(GTK_BUTTON(row_btn), hbox);
        g_object_set_data_full(G_OBJECT(row_btn), "target-uri", g_strdup(kajo_file_item_get_uri(it)), g_free);
        g_signal_connect(row_btn, "clicked", G_CALLBACK(on_bc_pop_subdir_clicked), self);
        gtk_box_append(GTK_BOX(self->bc_pop_list_box), row_btn);
    }

    if (subdirs->len > limit) {
        g_autofree char *more_txt = g_strdup_printf("… and %u more subfolder(s)", subdirs->len - limit);
        GtkWidget *more_lbl = gtk_label_new(more_txt);
        gtk_widget_add_css_class(more_lbl, "shell-popover-subtitle");
        gtk_widget_set_halign(more_lbl, GTK_ALIGN_START);
        gtk_widget_set_margin_start(more_lbl, 8);
        gtk_widget_set_margin_top(more_lbl, 4);
        gtk_widget_set_margin_bottom(more_lbl, 2);
        gtk_box_append(GTK_BOX(self->bc_pop_list_box), more_lbl);
    }
}

static void
build_breadcrumb_popover(KajoFilesWindow *self)
{
    self->breadcrumb_popover = gtk_popover_new();
    gtk_popover_set_has_arrow(GTK_POPOVER(self->breadcrumb_popover), FALSE);
    gtk_widget_add_css_class(self->breadcrumb_popover, "shell-popover");
    gtk_widget_add_css_class(self->breadcrumb_popover, "files-context-popover");
    gtk_widget_set_parent(self->breadcrumb_popover, GTK_WIDGET(self->location_stack));

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(vbox, 260, -1);

    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(hdr, "shell-popover-header");
    gtk_widget_set_valign(hdr, GTK_ALIGN_CENTER);

    GtkWidget *hdr_icon = gtk_image_new_from_icon_name("folder");
    gtk_image_set_pixel_size(GTK_IMAGE(hdr_icon), 16);
    gtk_widget_set_valign(hdr_icon, GTK_ALIGN_CENTER);

    self->bc_pop_hdr_lbl = gtk_label_new("FOLDER");
    gtk_widget_add_css_class(self->bc_pop_hdr_lbl, "heading");
    gtk_widget_set_hexpand(self->bc_pop_hdr_lbl, TRUE);
    gtk_widget_set_halign(self->bc_pop_hdr_lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(self->bc_pop_hdr_lbl, GTK_ALIGN_CENTER);
    gtk_label_set_ellipsize(GTK_LABEL(self->bc_pop_hdr_lbl), PANGO_ELLIPSIZE_MIDDLE);

    GtkWidget *badge = gtk_label_new("SUBFOLDERS");
    gtk_widget_add_css_class(badge, "shell-popover-badge");
    gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(hdr), hdr_icon);
    gtk_box_append(GTK_BOX(hdr), self->bc_pop_hdr_lbl);
    gtk_box_append(GTK_BOX(hdr), badge);
    gtk_box_append(GTK_BOX(vbox), hdr);

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(body, "shell-popover-body");

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 220);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);

    self->bc_pop_list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), self->bc_pop_list_box);
    gtk_box_append(GTK_BOX(body), scroll);

    gtk_box_append(GTK_BOX(body), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *btn_newwin = gtk_button_new_with_label("🗔  Open in New Window");
    gtk_widget_add_css_class(btn_newwin, "flat");
    gtk_widget_add_css_class(btn_newwin, "files-menu-row");
    g_signal_connect(btn_newwin, "clicked", G_CALLBACK(on_bc_pop_newwin_clicked), self);
    gtk_box_append(GTK_BOX(body), btn_newwin);

    GtkWidget *btn_copypath = gtk_button_new_with_label("🔗  Copy Path / URI");
    gtk_widget_add_css_class(btn_copypath, "flat");
    gtk_widget_add_css_class(btn_copypath, "files-menu-row");
    g_signal_connect(btn_copypath, "clicked", G_CALLBACK(on_bc_pop_copypath_clicked), self);
    gtk_box_append(GTK_BOX(body), btn_copypath);

    gtk_box_append(GTK_BOX(vbox), body);
    gtk_popover_set_child(GTK_POPOVER(self->breadcrumb_popover), vbox);
}

static void
open_breadcrumb_popover_for_segment(KajoFilesWindow *self,
                                    GtkWidget *btn,
                                    const char *uri,
                                    const char *label)
{
    if (!self || !self->breadcrumb_popover || !uri)
        return;

    g_free(self->bc_pop_target_uri);
    self->bc_pop_target_uri = g_strdup(uri);
    gtk_label_set_text(GTK_LABEL(self->bc_pop_hdr_lbl), label ? label : "FOLDER");

    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(self->bc_pop_list_box)) != NULL) {
        gtk_box_remove(GTK_BOX(self->bc_pop_list_box), child);
    }

    GtkWidget *loading = gtk_label_new("Scanning subfolders...");
    gtk_widget_add_css_class(loading, "shell-popover-subtitle");
    gtk_widget_set_halign(loading, GTK_ALIGN_START);
    gtk_widget_set_margin_start(loading, 8);
    gtk_box_append(GTK_BOX(self->bc_pop_list_box), loading);

    if (self->bc_pop_cancel) {
        g_cancellable_cancel(self->bc_pop_cancel);
        g_clear_object(&self->bc_pop_cancel);
    }
    self->bc_pop_cancel = g_cancellable_new();

    g_autoptr(GFile) dir = g_file_new_for_uri(uri);
    kajo_files_enumerate_async(dir, self->bc_pop_cancel, on_breadcrumb_subdirs_ready, files_window_op_weak_new(self));

    graphene_point_t pt = { 0, 0 };
    if (gtk_widget_compute_point(btn, GTK_WIDGET(self->location_stack), &GRAPHENE_POINT_INIT(0, 0), &pt)) {
        GdkRectangle rect = {
            (int)pt.x,
            (int)pt.y,
            MAX(1, gtk_widget_get_width(btn)),
            MAX(1, gtk_widget_get_height(btn))
        };
        gtk_popover_set_pointing_to(GTK_POPOVER(self->breadcrumb_popover), &rect);
    }
    gtk_popover_popup(GTK_POPOVER(self->breadcrumb_popover));
}

static void
on_breadcrumb_btn_pressed(GtkGestureClick *gesture,
                          int n_press,
                          double x G_GNUC_UNUSED,
                          double y G_GNUC_UNUSED,
                          gpointer user_data)
{
    if (n_press != 1)
        return;

    KajoFilesWindow *self = user_data;
    GtkWidget *btn = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    const char *uri = btn ? g_object_get_data(G_OBJECT(btn), "segment-uri") : NULL;
    const char *lbl = btn ? g_object_get_data(G_OBJECT(btn), "segment-label") : NULL;
    if (!uri)
        return;

    guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    GdkModifierType mods = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));

    if (button == GDK_BUTTON_MIDDLE ||
        (button == GDK_BUTTON_PRIMARY && (mods & GDK_CONTROL_MASK) != 0)) {
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
        GtkApplication *app = gtk_window_get_application(GTK_WINDOW(self));
        GtkWidget *win = kajo_files_window_new(app, uri);
        gtk_window_present(GTK_WINDOW(win));
    } else if (button == GDK_BUTTON_SECONDARY) {
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
        open_breadcrumb_popover_for_segment(self, btn, uri, lbl);
    }
}

static void
on_breadcrumb_segment_clicked(GtkButton *btn, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    const char *uri = g_object_get_data(G_OBJECT(btn), "segment-uri");
    if (uri)
        kajo_files_window_navigate_uri(self, uri);
}

static void
rebuild_breadcrumbs(KajoFilesWindow *self)
{
    if (!self || !self->breadcrumb_box || !self->current_dir)
        return;

    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(self->breadcrumb_box)) != NULL) {
        gtk_box_remove(GTK_BOX(self->breadcrumb_box), child);
    }

    g_autofree char *uri = g_file_get_uri(self->current_dir);
    g_autofree char *local_path = g_file_get_path(self->current_dir);
    g_autofree char *arch_path = NULL;
    g_autofree char *inner_path = NULL;

    g_autofree char *badge = NULL;
    if (uri && kajo_files_parse_archive_uri(uri, &arch_path, &inner_path)) {
        g_autofree char *canon_uri = kajo_files_build_archive_uri(arch_path, inner_path);
        gtk_editable_set_text(GTK_EDITABLE(self->uri_entry), canon_uri ? canon_uri : uri);
        g_autofree char *arch_base = g_path_get_basename(arch_path);
        if (inner_path && inner_path[0] != '\0')
            badge = g_strdup_printf("ARCHIVE: %s!/%s", arch_base ? arch_base : arch_path, inner_path);
        else
            badge = g_strdup_printf("ARCHIVE: %s!/", arch_base ? arch_base : arch_path);
    } else {
        gtk_editable_set_text(GTK_EDITABLE(self->uri_entry), local_path ? local_path : (uri ? uri : ""));
        badge = g_strdup_printf("LOCATION: %s", local_path ? local_path : (uri ? uri : ""));
    }
    gtk_label_set_text(GTK_LABEL(self->current_uri_badge), badge);

    /* Build full path chain from root to current_dir so '/' and '/home' remain 1 click away */
    g_autoptr(GPtrArray) chain = kajo_files_build_breadcrumb_chain(self->current_dir);

    for (guint i = 0; i < chain->len; i++) {
        const KajoBreadcrumbSegment *seg = g_ptr_array_index(chain, i);

        if (i > 0) {
            GtkWidget *sep = gtk_label_new("▸");
            gtk_widget_add_css_class(sep, "sidebar-chevron");
            gtk_box_append(GTK_BOX(self->breadcrumb_box), sep);
        }

        GtkWidget *btn = gtk_button_new_with_label(seg->label);
        gtk_widget_add_css_class(btn, "flat");
        gtk_widget_add_css_class(btn, "files-breadcrumb-btn");
        if (i + 1 == chain->len)
            gtk_widget_add_css_class(btn, "active");

        gtk_widget_set_tooltip_text(btn, seg->tooltip);

        g_object_set_data_full(G_OBJECT(btn), "segment-uri", g_strdup(seg->uri), g_free);
        g_object_set_data_full(G_OBJECT(btn), "segment-label", g_strdup(seg->label), g_free);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_breadcrumb_segment_clicked), self);

        GtkGesture *click_gest = gtk_gesture_click_new();
        gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click_gest), GTK_PHASE_CAPTURE);
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click_gest), 0);
        g_signal_connect(click_gest, "pressed", G_CALLBACK(on_breadcrumb_btn_pressed), self);
        gtk_widget_add_controller(btn, GTK_EVENT_CONTROLLER(click_gest));

        gtk_box_append(GTK_BOX(self->breadcrumb_box), btn);
    }

    self->pending_breadcrumb_scroll_end = TRUE;
    schedule_breadcrumb_scroll_to_end(self);
}

/* ─── Virtualized Recycling View Factories: Metro Tile Grid & Details List ─── */

typedef struct {
    GtkWidget *card;
    GtkWidget *icon;
    GtkWidget *name_lbl;
    GtkWidget *sub_lbl;
    GCancellable *thumb_cancel;
} GridTileWidgets;

static void
grid_tile_widgets_free(gpointer data)
{
    GridTileWidgets *w = data;
    if (!w) return;
    if (w->thumb_cancel) {
        g_cancellable_cancel(w->thumb_cancel);
        g_clear_object(&w->thumb_cancel);
    }
    g_free(w);
}

static void
thumb_load_thread_cb(GTask *task,
                     gpointer source_object,
                     gpointer task_data G_GNUC_UNUSED,
                     GCancellable *cancellable)
{
    GFile *file = G_FILE(source_object);
    GError *error = NULL;
    GdkTexture *tex = gdk_texture_new_from_file(file, &error);
    if (!tex) {
        g_task_return_error(task, error);
        return;
    }
    if (g_cancellable_set_error_if_cancelled(cancellable, &error)) {
        g_object_unref(tex);
        g_task_return_error(task, error);
        return;
    }
    g_task_return_pointer(task, tex, g_object_unref);
}

static void
on_thumb_load_ready(GObject *source G_GNUC_UNUSED,
                    GAsyncResult *res,
                    gpointer user_data)
{
    GridTileWidgets *w = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GdkTexture) tex = g_task_propagate_pointer(G_TASK(res), &error);
    if (!tex || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

    if (w && w->icon && GTK_IS_IMAGE(w->icon)) {
        gtk_image_set_from_paintable(GTK_IMAGE(w->icon), GDK_PAINTABLE(tex));
        gtk_image_set_pixel_size(GTK_IMAGE(w->icon), 48);
    }
}

static void
on_grid_item_setup(GtkSignalListItemFactory *factory G_GNUC_UNUSED,
                   GtkListItem *list_item,
                   gpointer user_data G_GNUC_UNUSED)
{
    GridTileWidgets *w = g_new0(GridTileWidgets, 1);

    w->card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(w->card, "files-tile-card");
    gtk_widget_set_size_request(w->card, 136, 114);

    w->icon = gtk_image_new_from_icon_name("text-x-generic");
    gtk_image_set_pixel_size(GTK_IMAGE(w->icon), 42);
    gtk_widget_set_halign(w->icon, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(w->icon, 8);
    gtk_box_append(GTK_BOX(w->card), w->icon);

    w->name_lbl = gtk_label_new("");
    gtk_widget_add_css_class(w->name_lbl, "files-tile-title");
    gtk_label_set_ellipsize(GTK_LABEL(w->name_lbl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(w->name_lbl), 16);
    gtk_widget_set_halign(w->name_lbl, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(w->card), w->name_lbl);

    w->sub_lbl = gtk_label_new("");
    gtk_widget_add_css_class(w->sub_lbl, "files-tile-sub");
    gtk_widget_set_halign(w->sub_lbl, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(w->card), w->sub_lbl);

    g_object_set_data_full(G_OBJECT(list_item), "tile-widgets", w, grid_tile_widgets_free);
    gtk_list_item_set_child(list_item, w->card);
}

static void
on_grid_item_bind(GtkSignalListItemFactory *factory G_GNUC_UNUSED,
                  GtkListItem *list_item,
                  gpointer user_data G_GNUC_UNUSED)
{
    GridTileWidgets *w = g_object_get_data(G_OBJECT(list_item), "tile-widgets");
    KajoFileItem *item = gtk_list_item_get_item(list_item);
    if (!w || !item) return;

    if (w->thumb_cancel) {
        g_cancellable_cancel(w->thumb_cancel);
        g_clear_object(&w->thumb_cancel);
    }

    if (kajo_file_item_is_dir(item))
        gtk_widget_add_css_class(w->card, "files-tile-folder");
    else
        gtk_widget_remove_css_class(w->card, "files-tile-folder");

    gtk_image_set_from_icon_name(GTK_IMAGE(w->icon), kajo_file_item_get_icon_name(item));
    gtk_image_set_pixel_size(GTK_IMAGE(w->icon), 42);

    gtk_label_set_text(GTK_LABEL(w->name_lbl), kajo_file_item_get_display_name(item));

    g_autofree char *sz_str = kajo_files_format_size(
        kajo_file_item_get_size(item),
        kajo_file_item_is_dir(item)
    );
    gtk_label_set_text(GTK_LABEL(w->sub_lbl), sz_str);

    g_object_set_data_full(G_OBJECT(w->card), "file-item", g_object_ref(item), g_object_unref);

    const char *ctype = kajo_file_item_get_content_type(item);
    GFile *file = kajo_file_item_get_file(item);
    guint64 sz = kajo_file_item_get_size(item);
    if (ctype && g_str_has_prefix(ctype, "image/") &&
        file && g_file_is_native(file) &&
        sz > 0 && sz <= (2ULL * 1024ULL * 1024ULL)) {
        w->thumb_cancel = g_cancellable_new();
        g_autoptr(GTask) task = g_task_new(file, w->thumb_cancel, on_thumb_load_ready, w);
        g_task_run_in_thread(task, thumb_load_thread_cb);
    }
}

static void
on_grid_item_unbind(GtkSignalListItemFactory *factory G_GNUC_UNUSED,
                    GtkListItem *list_item,
                    gpointer user_data G_GNUC_UNUSED)
{
    GridTileWidgets *w = g_object_get_data(G_OBJECT(list_item), "tile-widgets");
    if (!w) return;

    if (w->thumb_cancel) {
        g_cancellable_cancel(w->thumb_cancel);
        g_clear_object(&w->thumb_cancel);
    }
    g_object_set_data(G_OBJECT(w->card), "file-item", NULL);
}

typedef struct {
    GtkWidget *hbox;
    GtkWidget *icon;
    GtkWidget *name_lbl;
    GtkWidget *sz_lbl;
    GtkWidget *type_lbl;
    GtkWidget *mtime_lbl;
} ListRowWidgets;

static void
on_list_item_setup(GtkSignalListItemFactory *factory G_GNUC_UNUSED,
                   GtkListItem *list_item,
                   gpointer user_data G_GNUC_UNUSED)
{
    ListRowWidgets *w = g_new0(ListRowWidgets, 1);

    w->hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(w->hbox, "files-list-row");
    gtk_widget_add_css_class(w->hbox, "files-list-row-box");
    gtk_widget_set_valign(w->hbox, GTK_ALIGN_CENTER);

    w->icon = gtk_image_new_from_icon_name("text-x-generic");
    gtk_image_set_pixel_size(GTK_IMAGE(w->icon), 20);
    gtk_widget_set_valign(w->icon, GTK_ALIGN_CENTER);

    w->name_lbl = gtk_label_new("");
    gtk_widget_add_css_class(w->name_lbl, "files-list-name");
    gtk_widget_set_hexpand(w->name_lbl, TRUE);
    gtk_widget_set_halign(w->name_lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(w->name_lbl, GTK_ALIGN_CENTER);
    gtk_label_set_ellipsize(GTK_LABEL(w->name_lbl), PANGO_ELLIPSIZE_END);

    w->sz_lbl = gtk_label_new("");
    gtk_widget_add_css_class(w->sz_lbl, "files-list-meta");
    gtk_widget_set_size_request(w->sz_lbl, 100, -1);
    gtk_widget_set_halign(w->sz_lbl, GTK_ALIGN_END);
    gtk_widget_set_valign(w->sz_lbl, GTK_ALIGN_CENTER);

    w->type_lbl = gtk_label_new("");
    gtk_widget_add_css_class(w->type_lbl, "files-list-meta");
    gtk_widget_set_size_request(w->type_lbl, 160, -1);
    gtk_widget_set_halign(w->type_lbl, GTK_ALIGN_START);
    gtk_widget_set_valign(w->type_lbl, GTK_ALIGN_CENTER);
    gtk_label_set_ellipsize(GTK_LABEL(w->type_lbl), PANGO_ELLIPSIZE_END);

    w->mtime_lbl = gtk_label_new("");
    gtk_widget_add_css_class(w->mtime_lbl, "files-list-meta");
    gtk_widget_set_size_request(w->mtime_lbl, 140, -1);
    gtk_widget_set_halign(w->mtime_lbl, GTK_ALIGN_END);
    gtk_widget_set_valign(w->mtime_lbl, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(w->hbox), w->icon);
    gtk_box_append(GTK_BOX(w->hbox), w->name_lbl);
    gtk_box_append(GTK_BOX(w->hbox), w->sz_lbl);
    gtk_box_append(GTK_BOX(w->hbox), w->type_lbl);
    gtk_box_append(GTK_BOX(w->hbox), w->mtime_lbl);

    g_object_set_data_full(G_OBJECT(list_item), "row-widgets", w, g_free);
    gtk_list_item_set_child(list_item, w->hbox);
}

static void
on_list_item_bind(GtkSignalListItemFactory *factory G_GNUC_UNUSED,
                  GtkListItem *list_item,
                  gpointer user_data G_GNUC_UNUSED)
{
    ListRowWidgets *w = g_object_get_data(G_OBJECT(list_item), "row-widgets");
    KajoFileItem *item = gtk_list_item_get_item(list_item);
    if (!w || !item) return;

    gtk_image_set_from_icon_name(GTK_IMAGE(w->icon), kajo_file_item_get_icon_name(item));
    gtk_label_set_text(GTK_LABEL(w->name_lbl), kajo_file_item_get_display_name(item));

    g_autofree char *sz_str = kajo_files_format_size(
        kajo_file_item_get_size(item),
        kajo_file_item_is_dir(item)
    );
    gtk_label_set_text(GTK_LABEL(w->sz_lbl), sz_str);
    gtk_label_set_text(GTK_LABEL(w->type_lbl), kajo_file_item_get_type_description(item));

    g_autofree char *mtime_str = kajo_files_format_mtime(kajo_file_item_get_mtime(item));
    gtk_label_set_text(GTK_LABEL(w->mtime_lbl), mtime_str);

    g_object_set_data_full(G_OBJECT(w->hbox), "file-item", g_object_ref(item), g_object_unref);
}

static void
on_list_item_unbind(GtkSignalListItemFactory *factory G_GNUC_UNUSED,
                    GtkListItem *list_item,
                    gpointer user_data G_GNUC_UNUSED)
{
    ListRowWidgets *w = g_object_get_data(G_OBJECT(list_item), "row-widgets");
    if (!w) return;
    g_object_set_data(G_OBJECT(w->hbox), "file-item", NULL);
}

typedef struct {
    KajoFileSortField field;
    gboolean ascending;
    gboolean folders_first;
} SortCtx;

static gint
ptr_array_sort_cb(gconstpointer a, gconstpointer b, gpointer user_data)
{
    const KajoFileItem *item_a = *(const KajoFileItem *const *)a;
    const KajoFileItem *item_b = *(const KajoFileItem *const *)b;
    const SortCtx *ctx = user_data;
    return kajo_file_item_compare_full(item_a, item_b, ctx->field, ctx->ascending, ctx->folders_first);
}

void
files_window_rebuild_filtered_view(KajoFilesWindow *self)
{
    if (!self) return;

    g_autoptr(GPtrArray) prev_uris = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) prev_sel_items = files_window_get_selected_items(self);
    if (prev_sel_items) {
        for (guint i = 0; i < prev_sel_items->len; i++) {
            KajoFileItem *it = g_ptr_array_index(prev_sel_items, i);
            const char *u = it ? kajo_file_item_get_uri(it) : NULL;
            if (u)
                g_ptr_array_add(prev_uris, g_strdup(u));
        }
    }

    if (self->visible_items)
        g_ptr_array_set_size(self->visible_items, 0);
    else
        self->visible_items = g_ptr_array_new_with_free_func(g_object_unref);

    const char *query = gtk_editable_get_text(GTK_EDITABLE(self->search_entry));

    if (self->all_items) {
        for (guint i = 0; i < self->all_items->len; i++) {
            KajoFileItem *it = g_ptr_array_index(self->all_items, i);
            if (kajo_file_item_matches_filter(it, self->show_hidden, query)) {
                g_ptr_array_add(self->visible_items, g_object_ref(it));
            }
        }
    }

    SortCtx sctx = { self->sort_field, self->sort_ascending, self->sort_folders_first };
    g_ptr_array_sort_with_data(self->visible_items, ptr_array_sort_cb, &sctx);

    self->suppress_selection_sync = TRUE;
    if (self->visible_store) {
        guint old_n = g_list_model_get_n_items(G_LIST_MODEL(self->visible_store));
        g_list_store_splice(
            self->visible_store,
            0,
            old_n,
            self->visible_items->pdata,
            self->visible_items->len
        );
    }
    self->suppress_selection_sync = FALSE;

    if (self->visible_items->len == 0) {
        gtk_stack_set_visible_child_name(self->view_stack, "empty");
        g_clear_object(&self->selected_item);
        kajo_quicklook_set_item(self->quicklook, NULL, self->current_dir);
        update_status_and_capacity_bar(self);
        return;
    }

    gtk_stack_set_visible_child_name(
        self->view_stack,
        (self->view_mode == KAJO_VIEW_GRID) ? "grid" : "list"
    );

    g_autoptr(GtkBitset) matched_bitset = kajo_files_match_selection_uris(self->visible_items, prev_uris);
    KajoRebuildSelectionAction sel_action = kajo_files_rebuild_selection_action(
        matched_bitset && !gtk_bitset_is_empty(matched_bitset),
        self->autoselect_pending,
        query && query[0] != '\0');
    if (sel_action == KAJO_REBUILD_SEL_RESTORE) {
        guint first_idx = gtk_bitset_get_minimum(matched_bitset);
        KajoFileItem *primary = g_ptr_array_index(self->visible_items, first_idx);
        g_set_object(&self->selected_item, primary);

        if (self->selection_model) {
            g_autoptr(GtkBitset) mask = gtk_bitset_new_range(0, self->visible_items->len);
            self->suppress_selection_sync = TRUE;
            gtk_selection_model_set_selection(GTK_SELECTION_MODEL(self->selection_model), matched_bitset, mask);
            self->suppress_selection_sync = FALSE;
        }

        kajo_quicklook_set_item(self->quicklook, primary, self->current_dir);
        update_status_and_capacity_bar(self);
    } else if (sel_action == KAJO_REBUILD_SEL_FIRST) {
        /* Opening a folder, or filtering with the search box: land on the first entry.
         * Any other refresh (file monitor, sort, hidden toggle, operation reload) must
         * not resurrect a selection the user cleared. */
        self->autoselect_pending = FALSE;
        files_window_set_selected_item_by_index(self, 0);
    } else {
        g_clear_object(&self->selected_item);
        if (self->selection_model) {
            self->suppress_selection_sync = TRUE;
            gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(self->selection_model));
            self->suppress_selection_sync = FALSE;
        }
        kajo_quicklook_set_item(self->quicklook, NULL, self->current_dir);
        update_status_and_capacity_bar(self);
    }
}

/* ─── Selection & Activation ─── */

int
files_window_get_selected_item_index(KajoFilesWindow *self)
{
    if (!self || !self->visible_items || self->visible_items->len == 0)
        return -1;
    if (self->selected_item) {
        for (guint i = 0; i < self->visible_items->len; i++) {
            KajoFileItem *it = g_ptr_array_index(self->visible_items, i);
            if (it == self->selected_item)
                return (int)i;
        }
    }
    if (self->selection_model) {
        g_autoptr(GtkBitset) bitset = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(self->selection_model));
        if (bitset && !gtk_bitset_is_empty(bitset)) {
            guint pos = gtk_bitset_get_minimum(bitset);
            if (pos != GTK_INVALID_LIST_POSITION && pos < self->visible_items->len)
                return (int)pos;
        }
    }
    return -1;
}

GPtrArray *
files_window_get_selected_items(KajoFilesWindow *self)
{
    GPtrArray *arr = g_ptr_array_new_with_free_func(g_object_unref);
    if (!self || !self->visible_items)
        return arr;

    if (self->selection_model) {
        g_autoptr(GtkBitset) bitset = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(self->selection_model));
        if (bitset && !gtk_bitset_is_empty(bitset)) {
            GtkBitsetIter iter;
            guint idx = 0;
            if (gtk_bitset_iter_init_first(&iter, bitset, &idx)) {
                do {
                    if (idx < self->visible_items->len) {
                        KajoFileItem *it = g_ptr_array_index(self->visible_items, idx);
                        if (it)
                            g_ptr_array_add(arr, g_object_ref(it));
                    }
                } while (gtk_bitset_iter_next(&iter, &idx));
            }
        }
    }

    if (arr->len == 0 && self->selected_item) {
        g_ptr_array_add(arr, g_object_ref(self->selected_item));
    }
    return arr;
}

void
files_window_select_all(KajoFilesWindow *self)
{
    if (!self || !self->selection_model || !self->visible_items || self->visible_items->len == 0)
        return;

    gtk_selection_model_select_all(GTK_SELECTION_MODEL(self->selection_model));
}

void
files_window_set_selected_item_by_index(KajoFilesWindow *self, int idx)
{
    if (!self || !self->visible_items || self->visible_items->len == 0)
        return;

    idx = CLAMP(idx, 0, (int)self->visible_items->len - 1);
    KajoFileItem *item = g_ptr_array_index(self->visible_items, (guint)idx);
    g_set_object(&self->selected_item, item);

    if (self->selection_model) {
        self->suppress_selection_sync = TRUE;
        gtk_selection_model_select_item(GTK_SELECTION_MODEL(self->selection_model), (guint)idx, TRUE);
        self->suppress_selection_sync = FALSE;
    }

    kajo_quicklook_set_item(self->quicklook, item, self->current_dir);
    update_status_and_capacity_bar(self);
}

static void
on_selection_model_changed(GtkSelectionModel *sel,
                           guint position G_GNUC_UNUSED,
                           guint n_items G_GNUC_UNUSED,
                           gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (!self || self->suppress_selection_sync || !self->visible_items)
        return;

    g_autoptr(GtkBitset) bitset = gtk_selection_model_get_selection(sel);
    KajoFileItem *item = NULL;

    if (bitset && !gtk_bitset_is_empty(bitset)) {
        guint min_pos = gtk_bitset_get_minimum(bitset);
        if (min_pos != GTK_INVALID_LIST_POSITION && min_pos < self->visible_items->len)
            item = g_ptr_array_index(self->visible_items, min_pos);
    }

    if (item != self->selected_item) {
        g_set_object(&self->selected_item, item);
        kajo_quicklook_set_item(self->quicklook, item, self->current_dir);
    }
    update_status_and_capacity_bar(self);
}

typedef struct {
    GWeakRef ref;
    char    *image_path;
    gboolean fallback_to_archive;
} DiskImageCtx;

static void
on_disk_image_mounted(const char *mount_path, GError *error, gpointer user_data)
{
    DiskImageCtx *dctx = user_data;
    KajoFilesWindow *self = g_weak_ref_get(&dctx->ref);
    g_autofree char *image_path = g_steal_pointer(&dctx->image_path);
    gboolean fallback = dctx->fallback_to_archive;
    g_weak_ref_clear(&dctx->ref);
    g_free(dctx);
    if (!self)
        return;

    if (self->active_ops_count > 0)
        self->active_ops_count--;
    if (self->btn_cancel_op && self->active_ops_count == 0)
        gtk_widget_set_visible(self->btn_cancel_op, FALSE);

    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        files_window_set_status_message(self, "Disk image mount cancelled", FALSE);
    } else if (mount_path) {
        g_autoptr(GFile) mount_dir = g_file_new_for_path(mount_path);
        g_autofree char *uri = g_file_get_uri(mount_dir);
        files_window_set_status_message(self, "Disk image mounted (eject it in the sidebar when done)", FALSE);
        if (self->sidebar)
            kajo_files_sidebar_refresh(self->sidebar);
        if (uri)
            kajo_files_window_navigate_uri(self, uri);
    } else {
        g_autofree char *arch_uri = fallback ? kajo_files_build_archive_uri(image_path, "") : NULL;
        if (arch_uri) {
            g_autofree char *msg = g_strdup_printf("Could not mount (%s) - browsing as archive instead",
                                                   error ? error->message : "unknown error");
            kajo_files_window_navigate_uri(self, arch_uri);
            files_window_set_status_message(self, msg, TRUE);
        } else {
            g_autofree char *msg = g_strdup_printf("Could not mount disk image: %s",
                                                   error ? error->message : "unknown error");
            files_window_set_status_message(self, msg, TRUE);
        }
    }
    g_object_unref(self);
}

void
files_window_open_disk_image(KajoFilesWindow *self, GFile *image, gboolean fallback_to_archive)
{
    g_autofree char *path = image ? g_file_get_path(image) : NULL;
    if (!self || !path)
        return;
    files_window_begin_op(self, "Mounting disk image...");
    DiskImageCtx *dctx = g_new0(DiskImageCtx, 1);
    g_weak_ref_init(&dctx->ref, self);
    dctx->image_path = g_steal_pointer(&path);
    dctx->fallback_to_archive = fallback_to_archive;
    kajo_files_diskimage_mount_async(image, self->ops_cancel, on_disk_image_mounted, dctx);
}

typedef struct {
    GWeakRef ref;
    char    *outer_dir_uri; /* archive:// folder the activated entry was listed in */
} ArchiveOpenCtx;

static void
on_archive_entry_open_ready(GFile *temp_file, GError *error, gpointer user_data)
{
    ArchiveOpenCtx *octx = user_data;
    KajoFilesWindow *self = g_weak_ref_get(&octx->ref);
    g_autofree char *outer_dir_uri = g_steal_pointer(&octx->outer_dir_uri);
    g_weak_ref_clear(&octx->ref);
    g_free(octx);
    if (!self)
        return;

    if (self->active_ops_count > 0)
        self->active_ops_count--;
    if (self->btn_cancel_op && self->active_ops_count == 0)
        gtk_widget_set_visible(self->btn_cancel_op, FALSE);

    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        files_window_set_status_message(self, "Archive entry extraction cancelled", FALSE);
        g_object_unref(self);
        return;
    }

    if (!temp_file) {
        files_window_set_status_message(self, error ? error->message : "Failed to extract archive entry", TRUE);
        g_object_unref(self);
        return;
    }

    g_autofree char *temp_path = g_file_get_path(temp_file);
    g_autofree char *base = g_file_get_basename(temp_file);
    g_autofree char *guessed_ct = base ? g_content_type_guess(base, NULL, 0, NULL) : NULL;
    const char *ctype = (guessed_ct && guessed_ct[0] != '\0') ? guessed_ct : "application/octet-stream";

    if (temp_path && kajo_files_is_archive(ctype, base)) {
        g_autofree char *nested_arch_uri = kajo_files_build_archive_uri(temp_path, "");
        if (nested_arch_uri) {
            if (self->nested_archive_origin && outer_dir_uri)
                g_hash_table_insert(self->nested_archive_origin, g_strdup(temp_path), g_strdup(outer_dir_uri));
            kajo_files_window_navigate_uri(self, nested_arch_uri);
            g_object_unref(self);
            return;
        }
    }

    g_autoptr(KajoFileItem) temp_item = kajo_file_item_new_manual(
        temp_file,
        base ? base : "entry",
        base ? base : "entry",
        ctype,
        0, 0, 0644,
        FALSE, FALSE
    );
    kajo_files_open_item_default(temp_item, GTK_WINDOW(self));
    files_window_set_status_message(self, "Opened extracted archive entry", FALSE);
    g_object_unref(self);
}

void
files_window_activate_file_item(KajoFilesWindow *self, KajoFileItem *item)
{
    if (!self || !item) return;

    /* Check if we are in Trash — if so, Quick Look instead of navigating into trashed folder */
    if (self->current_dir) {
        g_autofree char *scheme = g_file_get_uri_scheme(self->current_dir);
        if (g_strcmp0(scheme, "trash") == 0) {
            kajo_quicklook_show(self->quicklook);
            return;
        }
    }

    const char *target_uri = kajo_file_item_get_target_uri(item);
    const char *item_uri = kajo_file_item_get_uri(item);
    if (kajo_file_item_is_dir(item) || kajo_file_item_is_mountable(item)) {
        const char *dest = target_uri ? target_uri : item_uri;
        kajo_files_window_navigate_uri(self, dest);
        return;
    }

    /* If activating a file entry inside archive://, extract on-demand to temp and open */
    if (item_uri && kajo_files_parse_archive_uri(item_uri, NULL, NULL)) {
        files_window_begin_op(self, "Extracting archive entry...");
        ArchiveOpenCtx *octx = g_new0(ArchiveOpenCtx, 1);
        g_weak_ref_init(&octx->ref, self);
        octx->outer_dir_uri = self->current_dir ? g_file_get_uri(self->current_dir) : NULL;
        kajo_files_op_extract_entry_to_temp_async(
            item_uri,
            self->ops_cancel,
            on_archive_entry_open_ready,
            octx
        );
        return;
    }

    /* If activating a supported archive file on a local/FUSE path, browse inside archive:// */
    GFile *file = kajo_file_item_get_file(item);
    g_autofree char *local_path = file ? g_file_get_path(file) : NULL;
    const char *ctype = kajo_file_item_get_content_type(item);
    const char *name = kajo_file_item_get_name(item);
    /* Open browses archives, ISOs included, as archive:// (no privileges needed). Disk images
     * libarchive cannot read (.img/.raw) are mounted; "Open With" offers mounting an ISO. */
    if (local_path && kajo_files_is_archive(ctype, name)) {
        g_autofree char *arch_uri = kajo_files_build_archive_uri(local_path, "");
        if (arch_uri) {
            kajo_files_window_navigate_uri(self, arch_uri);
            return;
        }
    }
    if (local_path && kajo_files_is_disk_image(ctype, name)) {
        files_window_open_disk_image(self, file, FALSE);
        return;
    }

    kajo_files_open_item_default(item, GTK_WINDOW(self));
}

static void
on_view_item_activated(GtkWidget *view G_GNUC_UNUSED,
                       guint position,
                       gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (!self || !self->visible_items || position >= self->visible_items->len)
        return;

    KajoFileItem *item = g_ptr_array_index(self->visible_items, position);
    if (item)
        files_window_activate_file_item(self, item);
}

/* ─── Directory Enumeration & Live GFileMonitor ─── */

static gboolean
on_dir_monitor_debounce_timeout(gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    self->monitor_debounce_id = 0;
    files_window_reload_directory(self);
    return G_SOURCE_REMOVE;
}

static void
on_dir_monitor_changed(GFileMonitor *monitor G_GNUC_UNUSED,
                       GFile *file G_GNUC_UNUSED,
                       GFile *other_file G_GNUC_UNUSED,
                       GFileMonitorEvent event_type,
                       gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (event_type == G_FILE_MONITOR_EVENT_CREATED ||
        event_type == G_FILE_MONITOR_EVENT_DELETED ||
        event_type == G_FILE_MONITOR_EVENT_RENAMED ||
        event_type == G_FILE_MONITOR_EVENT_MOVED_IN ||
        event_type == G_FILE_MONITOR_EVENT_MOVED_OUT) {
        if (self->monitor_debounce_id == 0) {
            self->monitor_debounce_id = g_timeout_add(250, on_dir_monitor_debounce_timeout, self);
        }
    }
}

static void
on_directory_enumerated(GPtrArray *items, GError *error, gpointer user_data)
{
    KajoFilesWindow *self = user_data;

    if (error) {
        if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            return;

        if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_MOUNTED) && self->current_dir) {
            files_window_set_status_message(self, "Mounting remote / encrypted volume...", FALSE);
            kajo_files_sidebar_mount_uri(self->sidebar, self->current_dir);
            return;
        }

        files_window_set_status_message(self, error->message, TRUE);
        gtk_label_set_text(GTK_LABEL(self->empty_label), error->message);
        gtk_stack_set_visible_child_name(self->view_stack, "empty");
        return;
    }

    gtk_label_set_text(GTK_LABEL(self->empty_label), "DIRECTORY IS EMPTY");

    if (self->all_items)
        g_ptr_array_unref(self->all_items);
    self->all_items = items ? items : g_ptr_array_new_with_free_func(g_object_unref);

    if (self->current_dir) {
        g_autofree char *scheme = g_file_get_uri_scheme(self->current_dir);
        g_autofree char *dir_uri = g_file_get_uri(self->current_dir);
        g_autofree char *arch_path = NULL;
        g_autofree char *inner_path = NULL;
        gboolean is_trash = (g_strcmp0(scheme, "trash") == 0);
        gboolean is_archive = (dir_uri && kajo_files_parse_archive_uri(dir_uri, &arch_path, &inner_path));

        gtk_revealer_set_reveal_child(GTK_REVEALER(self->trash_banner_revealer), is_trash);
        if (is_trash) {
            g_autofree char *t_lbl = g_strdup_printf("🗑 TRASH // %u ITEM(S)", self->all_items->len);
            gtk_label_set_text(GTK_LABEL(self->trash_summary_lbl), t_lbl);
        }

        gtk_revealer_set_reveal_child(GTK_REVEALER(self->archive_banner_revealer), is_archive);
        if (is_archive) {
            g_autofree char *arch_base = g_path_get_basename(arch_path);
            g_autofree char *a_lbl = (inner_path && inner_path[0] != '\0')
                ? g_strdup_printf("📦 ARCHIVE VIEW (READ-ONLY) — %s / %s", arch_base ? arch_base : arch_path, inner_path)
                : g_strdup_printf("📦 ARCHIVE VIEW (READ-ONLY) — %s", arch_base ? arch_base : arch_path);
            gtk_label_set_text(GTK_LABEL(self->archive_summary_lbl), a_lbl);
        }
    }

    files_window_rebuild_filtered_view(self);
}

void
files_window_reload_directory(KajoFilesWindow *self)
{
    if (!self || !self->current_dir)
        return;

    if (self->enum_cancel) {
        g_cancellable_cancel(self->enum_cancel);
        g_clear_object(&self->enum_cancel);
    }
    self->enum_cancel = g_cancellable_new();

    if (self->dir_monitor) {
        g_file_monitor_cancel(self->dir_monitor);
        g_clear_object(&self->dir_monitor);
    }

    gboolean is_archive = files_window_is_read_only_archive(self);
    if (!is_archive) {
        self->dir_monitor = g_file_monitor_directory(
            self->current_dir,
            G_FILE_MONITOR_WATCH_MOVES,
            NULL,
            NULL
        );
        if (self->dir_monitor) {
            g_signal_connect(self->dir_monitor, "changed", G_CALLBACK(on_dir_monitor_changed), self);
        }
    }

    gtk_widget_set_sensitive(self->btn_new_folder, !is_archive);
    gtk_widget_set_sensitive(self->btn_new_file, !is_archive);
    gtk_widget_set_sensitive(self->btn_paste, !is_archive && kajo_files_clipboard_has_files());
    kajo_files_enumerate_async(self->current_dir, self->enum_cancel, on_directory_enumerated, self);
}

void
kajo_files_window_navigate_uri(KajoFilesWindow *self, const char *uri_or_path)
{
    g_return_if_fail(KAJO_IS_FILES_WINDOW(self));

    g_autoptr(GFile) target = NULL;
    if (!uri_or_path || uri_or_path[0] == '\0') {
        target = g_file_new_for_path(g_get_home_dir());
    } else if (strstr(uri_or_path, "://") != NULL) {
        target = g_file_new_for_uri(uri_or_path);
    } else if (uri_or_path[0] == '~') {
        g_autofree char *expanded = g_build_filename(g_get_home_dir(), uri_or_path + 1, NULL);
        target = g_file_new_for_path(expanded);
    } else {
        target = g_file_new_for_commandline_arg(uri_or_path);
    }

    gboolean dir_changed = (!self->current_dir || !g_file_equal(self->current_dir, target));
    if (dir_changed) {
        self->autoselect_pending = TRUE; /* fresh folder: select the first entry once it loads */
        g_clear_object(&self->selected_item);
        if (self->selection_model) {
            self->suppress_selection_sync = TRUE;
            gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(self->selection_model));
            self->suppress_selection_sync = FALSE;
        }
    }

    g_set_object(&self->current_dir, target);
    g_autofree char *canonical_uri = g_file_get_uri(target);

    if (!self->navigating_history && canonical_uri) {
        while ((int)self->history->len > self->history_pos + 1) {
            g_ptr_array_remove_index(self->history, self->history->len - 1);
        }
        g_ptr_array_add(self->history, g_strdup(canonical_uri));
        self->history_pos = (int)self->history->len - 1;
    }

    gtk_widget_set_sensitive(self->btn_back, self->history_pos > 0);
    gtk_widget_set_sensitive(self->btn_forward, self->history_pos + 1 < (int)self->history->len);

    g_autofree char *parent_uri = kajo_files_get_parent_uri(target);
    gtk_widget_set_sensitive(self->btn_up, parent_uri != NULL);

    gtk_stack_set_visible_child_name(self->location_stack, "breadcrumbs");
    rebuild_breadcrumbs(self);

    if (self->sidebar && canonical_uri)
        kajo_files_sidebar_sync_active_uri(self->sidebar, canonical_uri);

    files_window_reload_directory(self);
}

/* ─── Header & Ribbon Button Handlers ─── */

void
on_btn_back_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (self->history_pos > 0) {
        self->history_pos--;
        self->navigating_history = TRUE;
        const char *uri = g_ptr_array_index(self->history, (guint)self->history_pos);
        kajo_files_window_navigate_uri(self, uri);
        self->navigating_history = FALSE;
    }
}

void
on_btn_forward_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (self->history_pos + 1 < (int)self->history->len) {
        self->history_pos++;
        self->navigating_history = TRUE;
        const char *uri = g_ptr_array_index(self->history, (guint)self->history_pos);
        kajo_files_window_navigate_uri(self, uri);
        self->navigating_history = FALSE;
    }
}

void
on_btn_up_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (!self->current_dir) return;
    g_autofree char *parent_uri = kajo_files_get_parent_uri(self->current_dir);
    if (parent_uri) {
        kajo_files_window_navigate_uri(self, parent_uri);
    }
}

static void
on_btn_reload_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    files_window_reload_directory(self);
}

void
on_btn_edit_uri_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    const char *vis = gtk_stack_get_visible_child_name(self->location_stack);
    if (g_strcmp0(vis, "entry") == 0) {
        gtk_stack_set_visible_child_name(self->location_stack, "breadcrumbs");
    } else {
        gtk_stack_set_visible_child_name(self->location_stack, "entry");
        gtk_widget_grab_focus(self->uri_entry);
        gtk_editable_select_region(GTK_EDITABLE(self->uri_entry), 0, -1);
    }
}

static void
on_uri_entry_activated(GtkEntry *entry, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (text && text[0] != '\0')
        kajo_files_window_navigate_uri(self, text);
}

static void
on_search_changed(GtkSearchEntry *entry G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    files_window_rebuild_filtered_view(self);
}

void
files_window_set_view_mode(KajoFilesWindow *self, KajoFileViewMode mode)
{
    gboolean changed = (self->view_mode != mode);
    self->view_mode = mode;
    if (mode == KAJO_VIEW_GRID) {
        gtk_widget_add_css_class(self->btn_view_grid, "active");
        gtk_widget_remove_css_class(self->btn_view_list, "active");
    } else {
        gtk_widget_add_css_class(self->btn_view_list, "active");
        gtk_widget_remove_css_class(self->btn_view_grid, "active");
    }
    if (self->visible_items && self->visible_items->len > 0) {
        gtk_stack_set_visible_child_name(
            self->view_stack,
            (mode == KAJO_VIEW_GRID) ? "grid" : "list"
        );
    }
    if (changed)
        files_window_save_prefs(self);
}

static void
on_btn_view_grid_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    files_window_set_view_mode(KAJO_FILES_WINDOW(user_data), KAJO_VIEW_GRID);
}

static void
on_btn_view_list_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    files_window_set_view_mode(KAJO_FILES_WINDOW(user_data), KAJO_VIEW_LIST);
}

void
on_btn_inspector_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    self->inspector_visible = !self->inspector_visible;
    gtk_widget_set_visible(self->inspector_vbox, self->inspector_visible && !self->is_narrow);
    if (self->inspector_visible)
        gtk_widget_add_css_class(self->btn_inspector, "active");
    else
        gtk_widget_remove_css_class(self->btn_inspector, "active");
    files_window_save_prefs(self);
}

static void
on_btn_float_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    niri_toggle_window_floating();
}

static void
on_btn_close_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    gtk_window_close(GTK_WINDOW(user_data));
}

static void
on_btn_new_folder_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    files_window_show_inline_prompt(KAJO_FILES_WINDOW(user_data), PROMPT_MKDIR);
}

static void
on_btn_new_file_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    files_window_show_inline_prompt(KAJO_FILES_WINDOW(user_data), PROMPT_NEW_FILE);
}

void
on_btn_paste_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (files_window_is_read_only_archive(self)) {
        files_window_set_status_message(self, "Archive view is read-only", TRUE);
        return;
    }
    if (self->current_dir) {
        files_window_begin_op(self, "Pasting clipboard items...");
        kajo_files_clipboard_paste_async(self->current_dir, self->ops_cancel, files_window_on_file_op_completed, files_window_op_weak_new(self));
    }
}

void
on_btn_hidden_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    self->show_hidden = !self->show_hidden;
    if (self->show_hidden)
        gtk_widget_add_css_class(self->btn_hidden, "active");
    else
        gtk_widget_remove_css_class(self->btn_hidden, "active");
    files_window_save_prefs(self);
    files_window_rebuild_filtered_view(self);
}

/* ─── Sorting System (Sort Popover & Clickable Column Headers) ─── */

void
files_window_set_widget_active_class(GtkWidget *w, gboolean active)
{
    if (!w) return;
    if (active)
        gtk_widget_add_css_class(w, "active");
    else
        gtk_widget_remove_css_class(w, "active");
}

static void
sync_sort_ui(KajoFilesWindow *self)
{
    if (!self) return;

    const char *names[] = { "NAME", "SIZE", "MODIFIED", "TYPE" };
    const char *arrow = self->sort_ascending ? "▲" : "▼";

    g_autofree char *btn_lbl = g_strdup_printf(
        "⇅ SORT: %s %s",
        names[self->sort_field],
        arrow
    );
    gtk_menu_button_set_label(GTK_MENU_BUTTON(self->btn_sort), btn_lbl);

    /* Sort Popover Field Rows */
    gtk_button_set_label(
        GTK_BUTTON(self->btn_sort_field_name),
        (self->sort_field == KAJO_SORT_NAME) ? "✓  Name (Natural A–Z)" : "   Name (Natural A–Z)"
    );
    gtk_button_set_label(
        GTK_BUTTON(self->btn_sort_field_modified),
        (self->sort_field == KAJO_SORT_MODIFIED) ? "✓  Date Modified" : "   Date Modified"
    );
    gtk_button_set_label(
        GTK_BUTTON(self->btn_sort_field_size),
        (self->sort_field == KAJO_SORT_SIZE) ? "✓  Size" : "   Size"
    );
    gtk_button_set_label(
        GTK_BUTTON(self->btn_sort_field_type),
        (self->sort_field == KAJO_SORT_TYPE) ? "✓  Type / Extension" : "   Type / Extension"
    );

    files_window_set_widget_active_class(self->btn_sort_field_name, self->sort_field == KAJO_SORT_NAME);
    files_window_set_widget_active_class(self->btn_sort_field_modified, self->sort_field == KAJO_SORT_MODIFIED);
    files_window_set_widget_active_class(self->btn_sort_field_size, self->sort_field == KAJO_SORT_SIZE);
    files_window_set_widget_active_class(self->btn_sort_field_type, self->sort_field == KAJO_SORT_TYPE);

    /* Sort Popover Direction & Folders First */
    files_window_set_widget_active_class(self->btn_sort_asc, self->sort_ascending);
    files_window_set_widget_active_class(self->btn_sort_desc, !self->sort_ascending);

    gtk_button_set_label(
        GTK_BUTTON(self->btn_sort_folders_first),
        self->sort_folders_first ? "☑  Folders First" : "☐  Folders First"
    );
    files_window_set_widget_active_class(self->btn_sort_folders_first, self->sort_folders_first);

    /* Details List Clickable Column Headers */
    g_autofree char *col_name = (self->sort_field == KAJO_SORT_NAME)
        ? g_strdup_printf("NAME %s", arrow) : g_strdup("NAME");
    g_autofree char *col_size = (self->sort_field == KAJO_SORT_SIZE)
        ? g_strdup_printf("SIZE %s", arrow) : g_strdup("SIZE");
    g_autofree char *col_type = (self->sort_field == KAJO_SORT_TYPE)
        ? g_strdup_printf("TYPE %s", arrow) : g_strdup("TYPE");
    g_autofree char *col_mod = (self->sort_field == KAJO_SORT_MODIFIED)
        ? g_strdup_printf("MODIFIED %s", arrow) : g_strdup("MODIFIED");

    gtk_button_set_label(GTK_BUTTON(self->col_btn_name), col_name);
    gtk_button_set_label(GTK_BUTTON(self->col_btn_size), col_size);
    gtk_button_set_label(GTK_BUTTON(self->col_btn_type), col_type);
    gtk_button_set_label(GTK_BUTTON(self->col_btn_modified), col_mod);

    files_window_set_widget_active_class(self->col_btn_name, self->sort_field == KAJO_SORT_NAME);
    files_window_set_widget_active_class(self->col_btn_size, self->sort_field == KAJO_SORT_SIZE);
    files_window_set_widget_active_class(self->col_btn_type, self->sort_field == KAJO_SORT_TYPE);
    files_window_set_widget_active_class(self->col_btn_modified, self->sort_field == KAJO_SORT_MODIFIED);
}

static void
select_or_toggle_sort_field(KajoFilesWindow *self, KajoFileSortField field, gboolean close_popover)
{
    if (self->sort_field == field) {
        self->sort_ascending = !self->sort_ascending;
    } else {
        self->sort_field = field;
        self->sort_ascending = kajo_file_sort_field_default_ascending(field);
    }

    if (close_popover && self->sort_popover)
        gtk_popover_popdown(self->sort_popover);

    sync_sort_ui(self);
    files_window_save_prefs(self);
    files_window_rebuild_filtered_view(self);
}

static void
on_sort_field_name_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    select_or_toggle_sort_field(KAJO_FILES_WINDOW(user_data), KAJO_SORT_NAME, TRUE);
}

static void
on_sort_field_modified_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    select_or_toggle_sort_field(KAJO_FILES_WINDOW(user_data), KAJO_SORT_MODIFIED, TRUE);
}

static void
on_sort_field_size_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    select_or_toggle_sort_field(KAJO_FILES_WINDOW(user_data), KAJO_SORT_SIZE, TRUE);
}

static void
on_sort_field_type_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    select_or_toggle_sort_field(KAJO_FILES_WINDOW(user_data), KAJO_SORT_TYPE, TRUE);
}

static void
on_sort_asc_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    self->sort_ascending = TRUE;
    sync_sort_ui(self);
    files_window_save_prefs(self);
    files_window_rebuild_filtered_view(self);
}

static void
on_sort_desc_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    self->sort_ascending = FALSE;
    sync_sort_ui(self);
    files_window_save_prefs(self);
    files_window_rebuild_filtered_view(self);
}

static void
on_sort_folders_first_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    self->sort_folders_first = !self->sort_folders_first;
    sync_sort_ui(self);
    files_window_save_prefs(self);
    files_window_rebuild_filtered_view(self);
}

static void
on_col_btn_name_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    select_or_toggle_sort_field(KAJO_FILES_WINDOW(user_data), KAJO_SORT_NAME, FALSE);
}

static void
on_col_btn_size_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    select_or_toggle_sort_field(KAJO_FILES_WINDOW(user_data), KAJO_SORT_SIZE, FALSE);
}

static void
on_col_btn_type_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    select_or_toggle_sort_field(KAJO_FILES_WINDOW(user_data), KAJO_SORT_TYPE, FALSE);
}

static void
on_col_btn_modified_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    select_or_toggle_sort_field(KAJO_FILES_WINDOW(user_data), KAJO_SORT_MODIFIED, FALSE);
}

void
on_btn_terminal_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (self->current_dir)
        kajo_files_open_terminal_at(self->current_dir);
}

static void
on_btn_sidebar_toggle_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    gboolean vis = gtk_widget_get_visible(self->sidebar_vbox);
    gtk_widget_set_visible(self->sidebar_vbox, !vis);
}

/* ─── Sidebar & QuickLook Delegate Callbacks ─── */

static void
on_sidebar_navigate(const char *uri, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    if (self->is_narrow)
        gtk_widget_set_visible(self->sidebar_vbox, FALSE);
    kajo_files_window_navigate_uri(self, uri);
}

static void
on_sidebar_connect_server(gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    gboolean rev = gtk_revealer_get_reveal_child(GTK_REVEALER(self->connect_banner_revealer));
    gtk_revealer_set_reveal_child(GTK_REVEALER(self->connect_banner_revealer), !rev);
    if (!rev)
        gtk_widget_grab_focus(self->connect_entry);
}

static void
on_sidebar_status(const char *msg, gboolean is_error, gpointer user_data)
{
    files_window_set_status_message(KAJO_FILES_WINDOW(user_data), msg, is_error);
}

void
files_window_trash_selected_item(KajoFilesWindow *self)
{
    if (!self) return;
    if (files_window_is_read_only_archive(self)) {
        files_window_set_status_message(self, "Archive view is read-only", TRUE);
        return;
    }

    g_autoptr(GPtrArray) sel_items = files_window_get_selected_items(self);
    if (!sel_items || sel_items->len == 0)
        return;

    if (self->current_dir) {
        g_autofree char *scheme = g_file_get_uri_scheme(self->current_dir);
        if (g_strcmp0(scheme, "trash") == 0) {
            files_window_show_inline_prompt(self, PROMPT_DELETE_PERM);
            return;
        }
    }

    files_window_begin_op(self, "Moving to Trash...");
    kajo_files_op_trash_async(sel_items, self->ops_cancel, files_window_on_file_op_completed, files_window_op_weak_new(self));
}

static void
on_quicklook_action(KajoQuickLookAction action, KajoFileItem *item, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    switch (action) {
    case KAJO_QL_ACTION_OPEN_DEFAULT:
        if (item) files_window_activate_file_item(self, item);
        break;
    case KAJO_QL_ACTION_OPEN_TERMINAL:
        if (self->current_dir) kajo_files_open_terminal_at(self->current_dir);
        break;
    case KAJO_QL_ACTION_RENAME:
        files_window_show_inline_prompt(self, PROMPT_RENAME);
        break;
    case KAJO_QL_ACTION_TRASH:
        files_window_trash_selected_item(self);
        break;
    case KAJO_QL_ACTION_TOGGLE_EXEC: {
        if (!item || kajo_file_item_is_dir(item))
            break;
        GFile *f = kajo_file_item_get_file(item);
        g_autofree char *local_path = f ? g_file_get_path(f) : NULL;
        guint32 new_mode = 0;
        g_autoptr(GError) err = NULL;
        if (kajo_files_toggle_executable(local_path, &new_mode, &err)) {
            g_autofree char *msg = g_strdup_printf(
                "Updated permissions on '%s' (%04o)",
                kajo_file_item_get_display_name(item),
                new_mode
            );
            files_window_set_status_message(self, msg, FALSE);
            files_window_reload_directory(self);
        } else {
            files_window_set_status_message(self, err ? err->message : "Failed to toggle executable bit", TRUE);
        }
        break;
    }
    case KAJO_QL_ACTION_PREV_ITEM: {
        int idx = files_window_get_selected_item_index(self);
        if (idx > 0) files_window_set_selected_item_by_index(self, idx - 1);
        break;
    }
    case KAJO_QL_ACTION_NEXT_ITEM: {
        int idx = files_window_get_selected_item_index(self);
        if (idx >= 0 && idx + 1 < (int)self->visible_items->len)
            files_window_set_selected_item_by_index(self, idx + 1);
        break;
    }
    }
}

/* ─── Responsive Width Adaptation (< 768px) ─── */

static void
update_responsive_width(KajoFilesWindow *self, int width)
{
    if (!self || width <= 0) return;
    gboolean narrow = (width < 768);
    if (narrow == self->is_narrow) return;

    self->is_narrow = narrow;
    gtk_widget_set_visible(self->btn_sidebar_toggle, narrow);
    if (self->title_label)
        gtk_widget_set_visible(self->title_label, !narrow);
    gtk_widget_set_visible(self->sidebar_vbox, !narrow);
    gtk_widget_set_visible(self->inspector_vbox, self->inspector_visible && !narrow);
}

static void
on_surface_width_changed(GdkSurface *surface, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    KajoFilesWindow *self = user_data;
    update_responsive_width(self, gdk_surface_get_width(surface));
}

static void
on_window_realize(GtkWidget *widget, gpointer user_data G_GNUC_UNUSED)
{
    KajoFilesWindow *self = KAJO_FILES_WINDOW(widget);
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(widget));
    if (surface) {
        g_signal_connect_object(surface, "notify::width", G_CALLBACK(on_surface_width_changed), self, 0);
        update_responsive_width(self, gdk_surface_get_width(surface));
    }
}

/* ─── GObject Lifecycle ─── */

static void
kajo_files_window_dispose(GObject *object)
{
    KajoFilesWindow *self = KAJO_FILES_WINDOW(object);

    if (self->monitor_debounce_id > 0) {
        g_source_remove(self->monitor_debounce_id);
        self->monitor_debounce_id = 0;
    }
    if (self->breadcrumb_scroll_idle_id > 0) {
        g_source_remove(self->breadcrumb_scroll_idle_id);
        self->breadcrumb_scroll_idle_id = 0;
    }
    if (self->enum_cancel) {
        g_cancellable_cancel(self->enum_cancel);
        g_clear_object(&self->enum_cancel);
    }
    if (self->ops_cancel) {
        g_cancellable_cancel(self->ops_cancel);
        g_clear_object(&self->ops_cancel);
    }
    if (self->bc_pop_cancel) {
        g_cancellable_cancel(self->bc_pop_cancel);
        g_clear_object(&self->bc_pop_cancel);
    }
    if (self->dir_monitor) {
        g_file_monitor_cancel(self->dir_monitor);
        g_clear_object(&self->dir_monitor);
    }

    g_clear_pointer(&self->bc_pop_target_uri, g_free);
    g_clear_pointer(&self->nested_archive_origin, g_hash_table_destroy);
    g_clear_pointer(&self->breadcrumb_popover, gtk_widget_unparent);
    g_clear_pointer(&self->context_popover, gtk_widget_unparent);
    g_clear_pointer(&self->sidebar, kajo_files_sidebar_free);
    g_clear_pointer(&self->quicklook, kajo_quicklook_free);
    g_clear_object(&self->current_dir);
    g_clear_object(&self->selected_item);
    g_clear_object(&self->selection_model);
    g_clear_object(&self->visible_store);

    if (self->all_items) {
        g_ptr_array_unref(self->all_items);
        self->all_items = NULL;
    }
    if (self->visible_items) {
        g_ptr_array_unref(self->visible_items);
        self->visible_items = NULL;
    }
    if (self->history) {
        g_ptr_array_unref(self->history);
        self->history = NULL;
    }

    G_OBJECT_CLASS(kajo_files_window_parent_class)->dispose(object);
}

static void
kajo_files_window_class_init(KajoFilesWindowClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->dispose = kajo_files_window_dispose;

    gtk_widget_class_set_template_from_resource(widget_class, "/org/kajo/shell/ui/files_window.ui");

    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, root_overlay);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_sidebar_toggle);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, title_label);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_back);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_forward);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_up);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_reload);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, location_stack);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, breadcrumb_scroll);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, breadcrumb_box);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_edit_uri);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, uri_entry);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, search_entry);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_view_grid);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_view_list);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_inspector);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_float);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_close);

    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, current_uri_badge);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_new_folder);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_new_file);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_paste);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_hidden);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_sort);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, sort_popover);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_sort_field_name);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_sort_field_modified);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_sort_field_size);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_sort_field_type);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_sort_asc);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_sort_desc);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_sort_folders_first);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_terminal);

    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, sidebar_vbox);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, sidebar_list);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, auth_banner_revealer);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, connect_banner_revealer);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, trash_banner_revealer);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, archive_banner_revealer);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, prompt_banner_revealer);

    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, view_stack);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, grid_view);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, col_btn_name);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, col_btn_size);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, col_btn_type);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, col_btn_modified);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, details_listview);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, empty_label);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, inspector_vbox);

    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, status_left_label);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, btn_cancel_op);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, status_right_label);
    gtk_widget_class_bind_template_child(widget_class, KajoFilesWindow, fs_usage_bar);
}

static void
kajo_files_window_init(KajoFilesWindow *self)
{
    gtk_widget_init_template(GTK_WIDGET(self));

    self->history = g_ptr_array_new_with_free_func(g_free);
    self->nested_archive_origin = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    self->history_pos = -1;
    self->ops_cancel = g_cancellable_new();
    self->view_mode = KAJO_VIEW_GRID;
    self->sort_field = KAJO_SORT_NAME;
    self->sort_ascending = TRUE;
    self->sort_folders_first = TRUE;
    self->show_hidden = FALSE;
    self->inspector_visible = FALSE;
    files_window_load_prefs(self);

    /* Set up virtualized GListStore + GtkMultiSelection + Factories */
    self->visible_store = g_list_store_new(KAJO_TYPE_FILE_ITEM);
    self->selection_model = gtk_multi_selection_new(G_LIST_MODEL(g_object_ref(self->visible_store)));
    g_signal_connect(self->selection_model, "selection-changed", G_CALLBACK(on_selection_model_changed), self);

    GtkListItemFactory *grid_factory = gtk_signal_list_item_factory_new();
    g_signal_connect(grid_factory, "setup", G_CALLBACK(on_grid_item_setup), self);
    g_signal_connect(grid_factory, "bind", G_CALLBACK(on_grid_item_bind), self);
    g_signal_connect(grid_factory, "unbind", G_CALLBACK(on_grid_item_unbind), self);
    gtk_grid_view_set_model(self->grid_view, GTK_SELECTION_MODEL(self->selection_model));
    gtk_grid_view_set_factory(self->grid_view, grid_factory);
    g_object_unref(grid_factory);

    GtkListItemFactory *list_factory = gtk_signal_list_item_factory_new();
    g_signal_connect(list_factory, "setup", G_CALLBACK(on_list_item_setup), self);
    g_signal_connect(list_factory, "bind", G_CALLBACK(on_list_item_bind), self);
    g_signal_connect(list_factory, "unbind", G_CALLBACK(on_list_item_unbind), self);
    gtk_list_view_set_model(self->details_listview, GTK_SELECTION_MODEL(self->selection_model));
    gtk_list_view_set_factory(self->details_listview, list_factory);
    g_object_unref(list_factory);

    files_window_set_widget_active_class(self->btn_view_grid, self->view_mode == KAJO_VIEW_GRID);
    files_window_set_widget_active_class(self->btn_view_list, self->view_mode == KAJO_VIEW_LIST);
    files_window_set_widget_active_class(self->btn_hidden, self->show_hidden);
    files_window_set_widget_active_class(self->btn_inspector, self->inspector_visible);
    gtk_widget_set_visible(self->inspector_vbox, self->inspector_visible && !self->is_narrow);
    gtk_stack_set_visible_child_name(
        self->view_stack,
        (self->view_mode == KAJO_VIEW_GRID) ? "grid" : "list"
    );

    build_connect_banner(self);
    build_trash_banner(self);
    build_archive_banner(self);
    build_prompt_banner(self);
    build_breadcrumb_popover(self);
    files_window_build_context_menu_popover(self);
    sync_sort_ui(self);

    if (self->breadcrumb_scroll) {
        GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(self->breadcrumb_scroll);
        if (hadj) {
            g_signal_connect(hadj, "changed", G_CALLBACK(on_breadcrumb_hadj_changed), self);
            g_signal_connect(hadj, "notify::upper", G_CALLBACK(on_breadcrumb_hadj_notify), self);
            g_signal_connect(hadj, "notify::page-size", G_CALLBACK(on_breadcrumb_hadj_notify), self);
        }
        GtkEventController *bc_scroll_ctrl = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
        g_signal_connect(bc_scroll_ctrl, "scroll", G_CALLBACK(on_breadcrumb_wheel_scroll), self);
        gtk_widget_add_controller(GTK_WIDGET(self->breadcrumb_scroll), bc_scroll_ctrl);
    }

    self->quicklook = kajo_quicklook_new(
        self->root_overlay,
        self->inspector_vbox,
        on_quicklook_action,
        self
    );

    self->sidebar = kajo_files_sidebar_new(
        self->sidebar_list,
        self->auth_banner_revealer,
        on_sidebar_navigate,
        on_sidebar_connect_server,
        on_sidebar_status,
        self
    );

    /* Connect Header & Ribbon Signals */
    g_signal_connect(self->btn_sidebar_toggle, "clicked", G_CALLBACK(on_btn_sidebar_toggle_clicked), self);
    g_signal_connect(self->btn_back, "clicked", G_CALLBACK(on_btn_back_clicked), self);
    g_signal_connect(self->btn_forward, "clicked", G_CALLBACK(on_btn_forward_clicked), self);
    g_signal_connect(self->btn_up, "clicked", G_CALLBACK(on_btn_up_clicked), self);
    g_signal_connect(self->btn_reload, "clicked", G_CALLBACK(on_btn_reload_clicked), self);
    g_signal_connect(self->btn_edit_uri, "clicked", G_CALLBACK(on_btn_edit_uri_clicked), self);
    g_signal_connect(self->uri_entry, "activate", G_CALLBACK(on_uri_entry_activated), self);
    g_signal_connect(self->search_entry, "search-changed", G_CALLBACK(on_search_changed), self);
    g_signal_connect(self->btn_view_grid, "clicked", G_CALLBACK(on_btn_view_grid_clicked), self);
    g_signal_connect(self->btn_view_list, "clicked", G_CALLBACK(on_btn_view_list_clicked), self);
    g_signal_connect(self->btn_inspector, "clicked", G_CALLBACK(on_btn_inspector_clicked), self);
    g_signal_connect(self->btn_float, "clicked", G_CALLBACK(on_btn_float_clicked), self);
    g_signal_connect(self->btn_close, "clicked", G_CALLBACK(on_btn_close_clicked), self);

    g_signal_connect(self->btn_new_folder, "clicked", G_CALLBACK(on_btn_new_folder_clicked), self);
    g_signal_connect(self->btn_new_file, "clicked", G_CALLBACK(on_btn_new_file_clicked), self);
    g_signal_connect(self->btn_paste, "clicked", G_CALLBACK(on_btn_paste_clicked), self);
    g_signal_connect(self->btn_hidden, "clicked", G_CALLBACK(on_btn_hidden_clicked), self);
    g_signal_connect(self->btn_terminal, "clicked", G_CALLBACK(on_btn_terminal_clicked), self);
    g_signal_connect(self->btn_cancel_op, "clicked", G_CALLBACK(on_btn_cancel_op_clicked), self);

    /* Sort Popover & Column Header Signals */
    g_signal_connect(self->btn_sort_field_name, "clicked", G_CALLBACK(on_sort_field_name_clicked), self);
    g_signal_connect(self->btn_sort_field_modified, "clicked", G_CALLBACK(on_sort_field_modified_clicked), self);
    g_signal_connect(self->btn_sort_field_size, "clicked", G_CALLBACK(on_sort_field_size_clicked), self);
    g_signal_connect(self->btn_sort_field_type, "clicked", G_CALLBACK(on_sort_field_type_clicked), self);
    g_signal_connect(self->btn_sort_asc, "clicked", G_CALLBACK(on_sort_asc_clicked), self);
    g_signal_connect(self->btn_sort_desc, "clicked", G_CALLBACK(on_sort_desc_clicked), self);
    g_signal_connect(self->btn_sort_folders_first, "clicked", G_CALLBACK(on_sort_folders_first_clicked), self);

    g_signal_connect(self->col_btn_name, "clicked", G_CALLBACK(on_col_btn_name_clicked), self);
    g_signal_connect(self->col_btn_size, "clicked", G_CALLBACK(on_col_btn_size_clicked), self);
    g_signal_connect(self->col_btn_type, "clicked", G_CALLBACK(on_col_btn_type_clicked), self);
    g_signal_connect(self->col_btn_modified, "clicked", G_CALLBACK(on_col_btn_modified_clicked), self);

    g_signal_connect(self->grid_view, "activate", G_CALLBACK(on_view_item_activated), self);
    g_signal_connect(self->details_listview, "activate", G_CALLBACK(on_view_item_activated), self);

    /* Secondary Click (Right-Click) Controller for Context Menu */
    GtkGesture *right_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click), GDK_BUTTON_SECONDARY);
    g_signal_connect(right_click, "pressed", G_CALLBACK(on_view_stack_right_click), self);
    gtk_widget_add_controller(GTK_WIDGET(self->view_stack), GTK_EVENT_CONTROLLER(right_click));

    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key_ctrl, GTK_PHASE_CAPTURE);
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(files_window_on_key_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(self), key_ctrl);

    g_signal_connect(self, "realize", G_CALLBACK(on_window_realize), self);
}

GtkWidget *
kajo_files_window_new(GtkApplication *app, const char *initial_uri_or_path)
{
    KajoFilesWindow *win = g_object_new(
        KAJO_TYPE_FILES_WINDOW,
        "application", app,
        NULL
    );
    kajo_files_window_navigate_uri(win, initial_uri_or_path);
    return GTK_WIDGET(win);
}
