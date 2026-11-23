#include "files_window_private.h"
#include "files_diskimage.h"
#include "files_network.h"
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

void
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

void
files_window_update_status_and_capacity_bar(KajoFilesWindow *self)
{
    if (!self) return;

    guint total_vis = self->visible_items ? self->visible_items->len : 0;
    guint dirs = 0, files = 0;
    gboolean is_network = files_window_is_virtual_network(self);
    gboolean is_archive = files_window_is_read_only_archive(self);

    for (guint i = 0; i < total_vis; i++) {
        KajoFileItem *it = g_ptr_array_index(self->visible_items, i);
        if (kajo_file_item_is_dir(it) || kajo_file_item_is_mountable(it))
            dirs++;
        else
            files++;
    }

    g_autoptr(GPtrArray) sel_items = files_window_get_selected_items(self);
    if (sel_items && sel_items->len > 1) {
        guint64 sel_bytes = 0;
        for (guint i = 0; i < sel_items->len; i++) {
            KajoFileItem *it = g_ptr_array_index(sel_items, i);
            if (!kajo_file_item_is_dir(it) && !kajo_file_item_is_mountable(it))
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
        g_autofree char *sel_sz = kajo_file_item_format_subtitle(self->selected_item);
        g_autofree char *left = is_network
            ? g_strdup_printf(
                "%u NETWORK LOCATION(S)  |  SELECTED: %s (%s)",
                total_vis,
                kajo_file_item_get_display_name(self->selected_item),
                sel_sz)
            : g_strdup_printf(
                "%u ITEMS (%u FOLDERS, %u FILES)  |  SELECTED: %s (%s)",
                total_vis, dirs, files,
                kajo_file_item_get_display_name(self->selected_item),
                sel_sz);
        files_window_set_status_message(self, left, FALSE);
    } else {
        g_autofree char *left = is_network
            ? g_strdup_printf("%u NETWORK LOCATION(S)", total_vis)
            : g_strdup_printf("%u ITEMS (%u FOLDERS, %u FILES)", total_vis, dirs, files);
        files_window_set_status_message(self, left, FALSE);
    }

    if (self->current_dir && !is_network && !is_archive) {
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
        is_archive ? "READ-ONLY ARCHIVE" : (is_network ? "NETWORK DISCOVERY" : "GVFS VIRTUAL FS")
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
    g_autofree char *uri = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->connect_entry)));
    gtk_revealer_set_reveal_child(GTK_REVEALER(self->connect_banner_revealer), FALSE);
    if (uri) {
        g_strstrip(uri);
        if (uri[0] != '\0')
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

gboolean
files_window_is_virtual_network(const KajoFilesWindow *self)
{
    if (!self || !self->current_dir)
        return FALSE;
    return g_file_has_uri_scheme(self->current_dir, "network");
}

void
files_window_set_image_for_item(GtkImage *img, const KajoFileItem *item)
{
    if (!img || !item)
        return;
    GIcon *gicon = kajo_file_item_get_gicon(item);
    if (gicon)
        gtk_image_set_from_gicon(img, gicon);
    else
        gtk_image_set_from_icon_name(img, kajo_file_item_get_icon_name(item));
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

/* ─── Item Activation, Disk Images & Archive Entry Open ─── */

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
            g_autofree char *cur_uri = g_file_get_uri(self->current_dir);
            if (cur_uri && g_strcmp0(self->last_mount_attempt_uri, cur_uri) != 0) {
                g_free(self->last_mount_attempt_uri);
                self->last_mount_attempt_uri = g_strdup(cur_uri);
                files_window_set_status_message(self, "Mounting remote / encrypted volume...", FALSE);
                kajo_files_sidebar_mount_uri(self->sidebar, self->current_dir);
                return;
            }
        }

        files_window_set_status_message(self, error->message, TRUE);
        gtk_label_set_text(GTK_LABEL(self->empty_label), error->message);
        gtk_stack_set_visible_child_name(self->view_stack, "empty");
        return;
    }

    g_clear_pointer(&self->last_mount_attempt_uri, g_free);

    gboolean is_network = files_window_is_virtual_network(self);
    gtk_label_set_text(
        GTK_LABEL(self->empty_label),
        is_network
            ? "NO NETWORK SERVERS FOUND — USE '+ CONNECT TO SERVER' TO ADD SMB / SFTP / WEBDAV"
            : "DIRECTORY IS EMPTY"
    );

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

        if (dir_uri && kajo_files_network_is_remote_uri(dir_uri))
            kajo_files_network_record_server(dir_uri, NULL);

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
    gboolean is_network = files_window_is_virtual_network(self);
    gboolean read_only = is_archive || is_network;
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

    gtk_widget_set_sensitive(self->btn_new_folder, !read_only);
    gtk_widget_set_sensitive(self->btn_new_file, !read_only);
    gtk_widget_set_sensitive(self->btn_paste, !read_only && kajo_files_clipboard_has_files());
    if (is_network)
        files_window_set_status_message(self, "Scanning local network...", FALSE);
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
        g_clear_pointer(&self->last_mount_attempt_uri, g_free);
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
    files_window_rebuild_breadcrumbs(self);

    if (self->sidebar && canonical_uri)
        kajo_files_sidebar_sync_active_uri(self->sidebar, canonical_uri);

    files_window_reload_directory(self);
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
    g_clear_pointer(&self->last_mount_attempt_uri, g_free);
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

    files_window_views_setup_models(self);

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
    files_window_views_init_breadcrumbs(self);
    files_window_build_context_menu_popover(self);

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

    files_window_views_connect_toolbar_signals(self);
    g_signal_connect(self->btn_cancel_op, "clicked", G_CALLBACK(on_btn_cancel_op_clicked), self);

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
