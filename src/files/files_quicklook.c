#include "files_quicklook.h"
#include "files_ops.h"
#include "../player/player_mpv.h"
#include <archive.h>
#include <archive_entry.h>
#include <cairo.h>
#include <string.h>

#define QUICKLOOK_MAX_TEXT_BYTES (64 * 1024)

struct _KajoQuickLook {
    GtkOverlay *overlay;
    GtkWidget *inspector_box;

    KajoQuickLookActionCallback action_cb;
    gpointer user_data;

    KajoFileItem *current_item;
    GFile *current_dir;
    GCancellable *preview_cancel;

    /* Right Inspector Widgets */
    GtkWidget *insp_preview_stack;
    GtkWidget *insp_picture;
    GtkWidget *insp_icon;
    GtkWidget *insp_title_lbl;
    GtkWidget *insp_type_badge;
    GtkWidget *insp_size_lbl;
    GtkWidget *insp_mtime_lbl;
    GtkWidget *insp_perms_lbl;
    GtkWidget *insp_mime_lbl;
    GtkWidget *insp_extra_lbl;
    GtkWidget *insp_btn_quicklook;
    GtkWidget *insp_btn_open;
    GtkWidget *insp_btn_exec;
    GtkWidget *insp_btn_rename;
    GtkWidget *insp_btn_trash;

    /* Quick Look Modal Overlay Widgets */
    GtkWidget *ql_backdrop;
    GtkWidget *ql_title_lbl;
    GtkWidget *ql_badge_lbl;
    GtkWidget *ql_stack;
    GtkWidget *ql_picture;
    GtkWidget *ql_img_info_lbl;
    KajoPlayerMpv *ql_mpv;
    GtkWidget *ql_gl_area;
    GtkWidget *ql_media_play_btn;
    GtkWidget *ql_media_seek_scale;
    GtkWidget *ql_media_time_lbl;
    gboolean ql_updating_seek;
    GtkWidget *ql_text_view;
    GtkTextBuffer *ql_text_buf;
    GtkWidget *ql_folder_summary_lbl;
    GtkWidget *ql_folder_list;
    GtkWidget *ql_info_icon;
    GtkWidget *ql_info_details_lbl;
    GtkWidget *ql_footer_mtime_lbl;
};

static void
stop_media_playback(KajoQuickLook *self)
{
    if (self && self->ql_mpv)
        kajo_player_mpv_stop(self->ql_mpv);
}

static char *
format_ql_media_time(gint64 usec)
{
    if (usec < 0) usec = 0;
    gint64 total_sec = usec / 1000000;
    gint64 mins = total_sec / 60;
    gint64 secs = total_sec % 60;
    return g_strdup_printf("%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, mins, secs);
}

static void
on_ql_mpv_state_changed(const KajoMpvState *st, gpointer user_data)
{
    KajoQuickLook *self = user_data;
    if (!self || !st) return;

    if (self->ql_media_play_btn)
        gtk_button_set_label(GTK_BUTTON(self->ql_media_play_btn), st->playing ? "⏸" : "▶");

    if (self->ql_media_seek_scale) {
        self->ql_updating_seek = TRUE;
        double frac = (st->duration_usec > 0)
            ? CLAMP((double)st->position_usec / (double)st->duration_usec, 0.0, 1.0)
            : 0.0;
        gtk_range_set_value(GTK_RANGE(self->ql_media_seek_scale), frac);
        self->ql_updating_seek = FALSE;
    }

    if (self->ql_media_time_lbl) {
        g_autofree char *pos_s = format_ql_media_time(st->position_usec);
        g_autofree char *dur_s = format_ql_media_time(st->duration_usec);
        g_autofree char *lbl = g_strdup_printf("%s / %s", pos_s, dur_s);
        gtk_label_set_text(GTK_LABEL(self->ql_media_time_lbl), lbl);
    }
}

static void
on_ql_media_play_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoQuickLook *self = user_data;
    if (!self || !self->ql_mpv) return;
    kajo_player_mpv_set_playing(self->ql_mpv, !kajo_player_mpv_get_playing(self->ql_mpv));
}

static void
on_ql_media_seek_value_changed(GtkRange *range, gpointer user_data)
{
    KajoQuickLook *self = user_data;
    if (!self || !self->ql_mpv || self->ql_updating_seek)
        return;

    gint64 dur = kajo_player_mpv_get_duration_usec(self->ql_mpv);
    if (dur > 0) {
        double frac = CLAMP(gtk_range_get_value(range), 0.0, 1.0);
        kajo_player_mpv_seek_absolute_usec(self->ql_mpv, (gint64)(frac * (double)dur));
    }
}

/* ─── Archive Inspection (libarchive, Async GTask) ─── */

typedef struct {
    char *pathname;
    guint64 size;
    gboolean is_dir;
} ArchivePreviewEntry;

static void
archive_preview_entry_free(gpointer data)
{
    ArchivePreviewEntry *e = data;
    if (!e) return;
    g_free(e->pathname);
    g_free(e);
}

typedef struct {
    GPtrArray *entries; /* ArchivePreviewEntry* (up to 60) */
    guint total_entries;
    guint64 total_uncompressed;
} ArchivePreviewData;

static void
archive_preview_data_free(gpointer data)
{
    ArchivePreviewData *d = data;
    if (!d) return;
    if (d->entries)
        g_ptr_array_unref(d->entries);
    g_free(d);
}

static void
archive_preview_thread_cb(GTask *task,
                          gpointer source_object,
                          gpointer task_data G_GNUC_UNUSED,
                          GCancellable *cancellable)
{
    GFile *file = G_FILE(source_object);
    g_autofree char *local_path = g_file_get_path(file);
    if (!local_path) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Archive requires local path");
        return;
    }

    struct archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    if (archive_read_open_filename(a, local_path, 16384) != ARCHIVE_OK) {
        const char *err_s = archive_error_string(a);
        GError *err = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, err_s ? err_s : "Unable to open archive");
        archive_read_free(a);
        g_task_return_error(task, err);
        return;
    }

    ArchivePreviewData *res = g_new0(ArchivePreviewData, 1);
    res->entries = g_ptr_array_new_with_free_func(archive_preview_entry_free);

    struct archive_entry *entry = NULL;
    while (!g_cancellable_is_cancelled(cancellable) &&
           archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const char *pathname = archive_entry_pathname(entry);
        la_int64_t sz = archive_entry_size(entry);
        mode_t ftype = archive_entry_filetype(entry);
        gboolean is_dir = (ftype == AE_IFDIR) || (pathname && g_str_has_suffix(pathname, "/"));

        res->total_entries++;
        if (sz > 0)
            res->total_uncompressed += (guint64)sz;

        if (res->total_entries <= 60 && pathname) {
            ArchivePreviewEntry *ae = g_new0(ArchivePreviewEntry, 1);
            ae->pathname = g_strdup(pathname);
            ae->size = (guint64)MAX(0, sz);
            ae->is_dir = is_dir;
            g_ptr_array_add(res->entries, ae);
        }

        archive_read_data_skip(a);
    }

    archive_read_free(a);

    GError *cancel_err = NULL;
    if (g_cancellable_set_error_if_cancelled(cancellable, &cancel_err)) {
        archive_preview_data_free(res);
        g_task_return_error(task, cancel_err);
        return;
    }

    g_task_return_pointer(task, res, archive_preview_data_free);
}

static void
on_archive_preview_ready(GObject *source G_GNUC_UNUSED,
                         GAsyncResult *res,
                         gpointer user_data)
{
    KajoQuickLook *self = user_data;
    g_autoptr(GError) error = NULL;
    ArchivePreviewData *data = g_task_propagate_pointer(G_TASK(res), &error);
    if (!data) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) && self && self->ql_folder_summary_lbl) {
            gtk_label_set_text(GTK_LABEL(self->ql_folder_summary_lbl), error ? error->message : "Failed to inspect archive");
        }
        return;
    }

    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(self->ql_folder_list)) != NULL) {
        gtk_box_remove(GTK_BOX(self->ql_folder_list), child);
    }

    for (guint i = 0; i < data->entries->len; i++) {
        ArchivePreviewEntry *ae = g_ptr_array_index(data->entries, i);
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_add_css_class(row, "widget-row-card");
        gtk_widget_set_valign(row, GTK_ALIGN_CENTER);

        GtkWidget *ic = gtk_image_new_from_icon_name(ae->is_dir ? "folder" : "text-x-generic");
        gtk_image_set_pixel_size(GTK_IMAGE(ic), 16);
        gtk_widget_set_valign(ic, GTK_ALIGN_CENTER);

        GtkWidget *nm = gtk_label_new(ae->pathname);
        gtk_widget_set_halign(nm, GTK_ALIGN_START);
        gtk_widget_set_valign(nm, GTK_ALIGN_CENTER);
        gtk_widget_set_hexpand(nm, TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(nm), PANGO_ELLIPSIZE_MIDDLE);

        g_autofree char *sz_str = kajo_files_format_size(ae->size, ae->is_dir);
        GtkWidget *sz_lbl = gtk_label_new(sz_str);
        gtk_widget_add_css_class(sz_lbl, "shell-popover-subtitle");
        gtk_widget_set_valign(sz_lbl, GTK_ALIGN_CENTER);

        gtk_box_append(GTK_BOX(row), ic);
        gtk_box_append(GTK_BOX(row), nm);
        gtk_box_append(GTK_BOX(row), sz_lbl);
        gtk_box_append(GTK_BOX(self->ql_folder_list), row);
    }

    g_autofree char *uncomp_str = kajo_files_format_size(data->total_uncompressed, FALSE);
    g_autofree char *sum = g_strdup_printf(
        "ARCHIVE CONTENTS: %u ENTRIES (UNCOMPRESSED: %s)",
        data->total_entries, uncomp_str
    );
    gtk_label_set_text(GTK_LABEL(self->ql_folder_summary_lbl), sum);
    archive_preview_data_free(data);
}

static gboolean
populate_archive_preview(KajoQuickLook *self, GFile *file)
{
    g_autofree char *local_path = g_file_get_path(file);
    if (!local_path) return FALSE;

    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(self->ql_folder_list)) != NULL) {
        gtk_box_remove(GTK_BOX(self->ql_folder_list), child);
    }
    gtk_label_set_text(GTK_LABEL(self->ql_folder_summary_lbl), "Scanning archive...");
    gtk_stack_set_visible_child_name(GTK_STACK(self->ql_stack), "folder");

    g_autoptr(GTask) task = g_task_new(file, self->preview_cancel, on_archive_preview_ready, self);
    g_task_run_in_thread(task, archive_preview_thread_cb);
    return TRUE;
}

/* ─── Action Callbacks ─── */

static void
on_btn_quicklook_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoQuickLook *self = user_data;
    kajo_quicklook_toggle(self);
}

static void
on_btn_open_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoQuickLook *self = user_data;
    if (self->current_item && self->action_cb) {
        kajo_quicklook_hide(self);
        self->action_cb(KAJO_QL_ACTION_OPEN_DEFAULT, self->current_item, self->user_data);
    }
}

static void
on_btn_terminal_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoQuickLook *self = user_data;
    if (self->action_cb)
        self->action_cb(KAJO_QL_ACTION_OPEN_TERMINAL, self->current_item, self->user_data);
}

static void
on_btn_rename_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoQuickLook *self = user_data;
    if (self->current_item && self->action_cb)
        self->action_cb(KAJO_QL_ACTION_RENAME, self->current_item, self->user_data);
}

static void
on_btn_trash_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoQuickLook *self = user_data;
    if (self->current_item && self->action_cb)
        self->action_cb(KAJO_QL_ACTION_TRASH, self->current_item, self->user_data);
}

static void
on_btn_exec_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoQuickLook *self = user_data;
    if (self->current_item && self->action_cb)
        self->action_cb(KAJO_QL_ACTION_TOGGLE_EXEC, self->current_item, self->user_data);
}

static void
on_ql_close_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoQuickLook *self = user_data;
    kajo_quicklook_hide(self);
}

static void
on_ql_prev_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoQuickLook *self = user_data;
    if (self->action_cb)
        self->action_cb(KAJO_QL_ACTION_PREV_ITEM, self->current_item, self->user_data);
}

static void
on_ql_next_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    KajoQuickLook *self = user_data;
    if (self->action_cb)
        self->action_cb(KAJO_QL_ACTION_NEXT_ITEM, self->current_item, self->user_data);
}

/* ─── Compact Inspector UI Construction ─── */

static GtkWidget *
create_meta_row(const char *key, GtkWidget **out_val_lbl)
{
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *k_lbl = gtk_label_new(key);
    gtk_widget_add_css_class(k_lbl, "files-meta-key");
    gtk_widget_set_halign(k_lbl, GTK_ALIGN_START);
    gtk_widget_set_size_request(k_lbl, 64, -1);

    GtkWidget *v_lbl = gtk_label_new("--");
    gtk_widget_add_css_class(v_lbl, "files-meta-val");
    gtk_widget_set_halign(v_lbl, GTK_ALIGN_START);
    gtk_widget_set_hexpand(v_lbl, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(v_lbl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_selectable(GTK_LABEL(v_lbl), TRUE);

    gtk_box_append(GTK_BOX(hbox), k_lbl);
    gtk_box_append(GTK_BOX(hbox), v_lbl);

    if (out_val_lbl)
        *out_val_lbl = v_lbl;
    return hbox;
}

static void
build_inspector_ui(KajoQuickLook *self)
{
    if (!self->inspector_box) return;

    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(card, "shell-popover-card");
    gtk_widget_set_vexpand(card, TRUE);

    /* Header */
    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(hdr, "shell-popover-header");
    GtkWidget *hdr_lbl = gtk_label_new("INSPECTOR");
    gtk_widget_add_css_class(hdr_lbl, "heading");
    self->insp_type_badge = gtk_label_new("ITEM");
    gtk_widget_add_css_class(self->insp_type_badge, "shell-popover-badge");
    gtk_widget_set_hexpand(self->insp_type_badge, TRUE);
    gtk_widget_set_halign(self->insp_type_badge, GTK_ALIGN_END);
    gtk_label_set_ellipsize(GTK_LABEL(self->insp_type_badge), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(hdr), hdr_lbl);
    gtk_box_append(GTK_BOX(hdr), self->insp_type_badge);
    gtk_box_append(GTK_BOX(card), hdr);

    /* Body */
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(body, "shell-popover-body");
    gtk_widget_set_vexpand(body, TRUE);

    /* Compact Thumbnail Box */
    GtkWidget *thumb_frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(thumb_frame, "files-inspector-thumb");
    gtk_widget_set_size_request(thumb_frame, -1, 96);

    self->insp_preview_stack = gtk_stack_new();
    gtk_widget_set_vexpand(self->insp_preview_stack, TRUE);

    self->insp_icon = gtk_image_new_from_icon_name("folder");
    gtk_image_set_pixel_size(GTK_IMAGE(self->insp_icon), 48);
    gtk_widget_set_halign(self->insp_icon, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(self->insp_icon, GTK_ALIGN_CENTER);

    self->insp_picture = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(self->insp_picture), GTK_CONTENT_FIT_CONTAIN);

    gtk_stack_add_named(GTK_STACK(self->insp_preview_stack), self->insp_icon, "icon");
    gtk_stack_add_named(GTK_STACK(self->insp_preview_stack), self->insp_picture, "image");
    gtk_box_append(GTK_BOX(thumb_frame), self->insp_preview_stack);
    gtk_box_append(GTK_BOX(body), thumb_frame);

    /* Item Title */
    self->insp_title_lbl = gtk_label_new("No Item Selected");
    gtk_widget_add_css_class(self->insp_title_lbl, "row-title");
    gtk_widget_set_halign(self->insp_title_lbl, GTK_ALIGN_START);
    gtk_label_set_wrap(GTK_LABEL(self->insp_title_lbl), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(self->insp_title_lbl), PANGO_WRAP_WORD_CHAR);
    gtk_box_append(GTK_BOX(body), self->insp_title_lbl);

    /* Metadata Grid */
    GtkWidget *meta_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_add_css_class(meta_box, "section-card");
    gtk_box_append(GTK_BOX(meta_box), create_meta_row("SIZE:", &self->insp_size_lbl));
    gtk_box_append(GTK_BOX(meta_box), create_meta_row("MOD:", &self->insp_mtime_lbl));
    gtk_box_append(GTK_BOX(meta_box), create_meta_row("PERMS:", &self->insp_perms_lbl));
    gtk_box_append(GTK_BOX(meta_box), create_meta_row("MIME:", &self->insp_mime_lbl));
    gtk_box_append(GTK_BOX(meta_box), create_meta_row("PATH:", &self->insp_extra_lbl));
    gtk_box_append(GTK_BOX(body), meta_box);

    /* Action Buttons */
    GtkWidget *actions_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_valign(actions_vbox, GTK_ALIGN_END);
    gtk_widget_set_vexpand(actions_vbox, TRUE);

    self->insp_btn_quicklook = gtk_button_new_with_label("👁 QUICK LOOK (SPACE)");
    gtk_widget_add_css_class(self->insp_btn_quicklook, "files-accent-btn");
    g_signal_connect(self->insp_btn_quicklook, "clicked", G_CALLBACK(on_btn_quicklook_clicked), self);

    self->insp_btn_open = gtk_button_new_with_label("↗ OPEN DEFAULT APP");
    gtk_widget_add_css_class(self->insp_btn_open, "flat");
    g_signal_connect(self->insp_btn_open, "clicked", G_CALLBACK(on_btn_open_clicked), self);

    self->insp_btn_exec = gtk_button_new_with_label("☐ EXECUTABLE (+X)");
    gtk_widget_add_css_class(self->insp_btn_exec, "flat");
    gtk_widget_set_visible(self->insp_btn_exec, FALSE);
    g_signal_connect(self->insp_btn_exec, "clicked", G_CALLBACK(on_btn_exec_clicked), self);

    GtkWidget *btn_term = gtk_button_new_with_label(">_ OPEN TERMINAL");
    gtk_widget_add_css_class(btn_term, "flat");
    g_signal_connect(btn_term, "clicked", G_CALLBACK(on_btn_terminal_clicked), self);

    self->insp_btn_rename = gtk_button_new_with_label("✏ RENAME (F2)");
    gtk_widget_add_css_class(self->insp_btn_rename, "flat");
    g_signal_connect(self->insp_btn_rename, "clicked", G_CALLBACK(on_btn_rename_clicked), self);

    self->insp_btn_trash = gtk_button_new_with_label("🗑 MOVE TO TRASH");
    gtk_widget_add_css_class(self->insp_btn_trash, "flat");
    gtk_widget_add_css_class(self->insp_btn_trash, "action-btn-danger");
    g_signal_connect(self->insp_btn_trash, "clicked", G_CALLBACK(on_btn_trash_clicked), self);

    gtk_box_append(GTK_BOX(actions_vbox), self->insp_btn_quicklook);
    gtk_box_append(GTK_BOX(actions_vbox), self->insp_btn_open);
    gtk_box_append(GTK_BOX(actions_vbox), self->insp_btn_exec);
    gtk_box_append(GTK_BOX(actions_vbox), btn_term);
    gtk_box_append(GTK_BOX(actions_vbox), self->insp_btn_rename);
    gtk_box_append(GTK_BOX(actions_vbox), self->insp_btn_trash);

    gtk_box_append(GTK_BOX(body), actions_vbox);
    gtk_box_append(GTK_BOX(card), body);

    gtk_box_append(GTK_BOX(self->inspector_box), card);
}

/* ─── Quick Look Modal Overlay Construction ─── */

static void
build_quicklook_overlay_ui(KajoQuickLook *self)
{
    if (!self->overlay) return;

    self->ql_backdrop = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(self->ql_backdrop, "files-quicklook-backdrop");
    gtk_widget_set_hexpand(self->ql_backdrop, TRUE);
    gtk_widget_set_vexpand(self->ql_backdrop, TRUE);
    gtk_widget_set_visible(self->ql_backdrop, FALSE);

    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(card, "shell-popover-card");
    gtk_widget_add_css_class(card, "files-quicklook-card");
    gtk_widget_set_halign(card, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(card, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(card, 800, 540);

    /* Zone 1: Header */
    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(hdr, "shell-popover-header");

    self->ql_title_lbl = gtk_label_new("QUICK LOOK");
    gtk_widget_add_css_class(self->ql_title_lbl, "heading");
    gtk_widget_set_halign(self->ql_title_lbl, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(self->ql_title_lbl), PANGO_ELLIPSIZE_MIDDLE);

    self->ql_badge_lbl = gtk_label_new("");
    gtk_widget_add_css_class(self->ql_badge_lbl, "shell-popover-badge");
    gtk_widget_set_hexpand(self->ql_badge_lbl, TRUE);
    gtk_widget_set_halign(self->ql_badge_lbl, GTK_ALIGN_END);

    GtkWidget *btn_close = gtk_button_new_with_label("✕");
    gtk_widget_add_css_class(btn_close, "settings-close-btn");
    g_signal_connect(btn_close, "clicked", G_CALLBACK(on_ql_close_clicked), self);

    gtk_box_append(GTK_BOX(hdr), self->ql_title_lbl);
    gtk_box_append(GTK_BOX(hdr), self->ql_badge_lbl);
    gtk_box_append(GTK_BOX(hdr), btn_close);
    gtk_box_append(GTK_BOX(card), hdr);

    /* Zone 2: Body Stack */
    self->ql_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(self->ql_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_widget_set_hexpand(self->ql_stack, TRUE);
    gtk_widget_set_vexpand(self->ql_stack, TRUE);

    /* Page A: Image Preview */
    GtkWidget *img_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(img_page, "shell-popover-body");
    self->ql_picture = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(self->ql_picture), GTK_CONTENT_FIT_CONTAIN);
    gtk_widget_set_hexpand(self->ql_picture, TRUE);
    gtk_widget_set_vexpand(self->ql_picture, TRUE);
    self->ql_img_info_lbl = gtk_label_new("");
    gtk_widget_add_css_class(self->ql_img_info_lbl, "shell-popover-subtitle");
    gtk_widget_set_halign(self->ql_img_info_lbl, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(img_page), self->ql_picture);
    gtk_box_append(GTK_BOX(img_page), self->ql_img_info_lbl);
    gtk_stack_add_named(GTK_STACK(self->ql_stack), img_page, "image");

    /* Page B: Audio / Video Preview (libmpv + GtkGLArea) */
    GtkWidget *media_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(media_page, "shell-popover-body");

    self->ql_gl_area = gtk_gl_area_new();
    gtk_widget_set_hexpand(self->ql_gl_area, TRUE);
    gtk_widget_set_vexpand(self->ql_gl_area, TRUE);
    gtk_box_append(GTK_BOX(media_page), self->ql_gl_area);

    GtkWidget *media_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(media_bar, "shell-popover-header");
    gtk_widget_set_valign(media_bar, GTK_ALIGN_CENTER);

    self->ql_media_play_btn = gtk_button_new_with_label("⏸");
    gtk_widget_add_css_class(self->ql_media_play_btn, "flat");
    gtk_widget_add_css_class(self->ql_media_play_btn, "files-nav-btn");
    gtk_widget_set_valign(self->ql_media_play_btn, GTK_ALIGN_CENTER);
    g_signal_connect(self->ql_media_play_btn, "clicked", G_CALLBACK(on_ql_media_play_clicked), self);

    self->ql_media_seek_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.005);
    gtk_scale_set_draw_value(GTK_SCALE(self->ql_media_seek_scale), FALSE);
    gtk_widget_set_hexpand(self->ql_media_seek_scale, TRUE);
    gtk_widget_set_valign(self->ql_media_seek_scale, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(self->ql_media_seek_scale, "player-seek-scale");
    g_signal_connect(self->ql_media_seek_scale, "value-changed", G_CALLBACK(on_ql_media_seek_value_changed), self);

    self->ql_media_time_lbl = gtk_label_new("00:00 / 00:00");
    gtk_widget_add_css_class(self->ql_media_time_lbl, "shell-popover-badge");
    gtk_widget_set_valign(self->ql_media_time_lbl, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(media_bar), self->ql_media_play_btn);
    gtk_box_append(GTK_BOX(media_bar), self->ql_media_seek_scale);
    gtk_box_append(GTK_BOX(media_bar), self->ql_media_time_lbl);
    gtk_box_append(GTK_BOX(media_page), media_bar);

    self->ql_mpv = kajo_player_mpv_new(GTK_GL_AREA(self->ql_gl_area), on_ql_mpv_state_changed, self);
    gtk_stack_add_named(GTK_STACK(self->ql_stack), media_page, "media");

    /* Page D: Text / Code Preview */
    GtkWidget *text_scroll = gtk_scrolled_window_new();
    gtk_widget_set_hexpand(text_scroll, TRUE);
    gtk_widget_set_vexpand(text_scroll, TRUE);
    self->ql_text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(self->ql_text_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(self->ql_text_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(self->ql_text_view), TRUE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(self->ql_text_view), 14);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(self->ql_text_view), 14);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(self->ql_text_view), 12);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(self->ql_text_view), 12);
    gtk_widget_add_css_class(self->ql_text_view, "files-quicklook-textview");
    self->ql_text_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->ql_text_view));
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(text_scroll), self->ql_text_view);
    gtk_stack_add_named(GTK_STACK(self->ql_stack), text_scroll, "text");

    /* Page E: Directory / Archive Contents Preview */
    GtkWidget *folder_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_add_css_class(folder_page, "shell-popover-body");
    self->ql_folder_summary_lbl = gtk_label_new("Loading contents...");
    gtk_widget_add_css_class(self->ql_folder_summary_lbl, "heading");
    gtk_widget_set_halign(self->ql_folder_summary_lbl, GTK_ALIGN_START);
    GtkWidget *folder_scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(folder_scroll, TRUE);
    self->ql_folder_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(folder_scroll), self->ql_folder_list);
    gtk_box_append(GTK_BOX(folder_page), self->ql_folder_summary_lbl);
    gtk_box_append(GTK_BOX(folder_page), folder_scroll);
    gtk_stack_add_named(GTK_STACK(self->ql_stack), folder_page, "folder");

    /* Page F: Binary Metadata Preview */
    GtkWidget *info_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_add_css_class(info_page, "shell-popover-body");
    gtk_widget_set_valign(info_page, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(info_page, GTK_ALIGN_CENTER);
    self->ql_info_icon = gtk_image_new_from_icon_name("text-x-generic");
    gtk_image_set_pixel_size(GTK_IMAGE(self->ql_info_icon), 80);
    self->ql_info_details_lbl = gtk_label_new("");
    gtk_widget_add_css_class(self->ql_info_details_lbl, "files-quicklook-meta");
    gtk_label_set_selectable(GTK_LABEL(self->ql_info_details_lbl), TRUE);
    gtk_box_append(GTK_BOX(info_page), self->ql_info_icon);
    gtk_box_append(GTK_BOX(info_page), self->ql_info_details_lbl);
    gtk_stack_add_named(GTK_STACK(self->ql_stack), info_page, "info");

    gtk_box_append(GTK_BOX(card), self->ql_stack);

    /* Zone 3: Footer */
    GtkWidget *ftr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(ftr, "shell-popover-footer");

    GtkWidget *btn_prev = gtk_button_new_with_label("← PREV ITEM");
    gtk_widget_add_css_class(btn_prev, "flat");
    g_signal_connect(btn_prev, "clicked", G_CALLBACK(on_ql_prev_clicked), self);

    GtkWidget *btn_next = gtk_button_new_with_label("NEXT ITEM →");
    gtk_widget_add_css_class(btn_next, "flat");
    g_signal_connect(btn_next, "clicked", G_CALLBACK(on_ql_next_clicked), self);

    self->ql_footer_mtime_lbl = gtk_label_new("");
    gtk_widget_add_css_class(self->ql_footer_mtime_lbl, "shell-popover-subtitle");
    gtk_widget_set_hexpand(self->ql_footer_mtime_lbl, TRUE);
    gtk_widget_set_halign(self->ql_footer_mtime_lbl, GTK_ALIGN_CENTER);

    GtkWidget *btn_open = gtk_button_new_with_label("↗ OPEN IN DEFAULT APP (⏎)");
    gtk_widget_add_css_class(btn_open, "files-accent-btn");
    g_signal_connect(btn_open, "clicked", G_CALLBACK(on_btn_open_clicked), self);

    gtk_box_append(GTK_BOX(ftr), btn_prev);
    gtk_box_append(GTK_BOX(ftr), btn_next);
    gtk_box_append(GTK_BOX(ftr), self->ql_footer_mtime_lbl);
    gtk_box_append(GTK_BOX(ftr), btn_open);

    gtk_box_append(GTK_BOX(card), ftr);
    gtk_box_append(GTK_BOX(self->ql_backdrop), card);

    gtk_overlay_add_overlay(self->overlay, self->ql_backdrop);
}

/* ─── Async Quick Look Content Loaders ─── */

static void
on_text_bytes_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
    KajoQuickLook *self = user_data;
    GError *error = NULL;
    g_autoptr(GBytes) bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), res, &error);
    g_input_stream_close(G_INPUT_STREAM(source), NULL, NULL);

    if (error) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            gtk_text_buffer_set_text(self->ql_text_buf, error->message, -1);
        }
        g_clear_error(&error);
        return;
    }

    gsize len = 0;
    const char *raw = bytes ? g_bytes_get_data(bytes, &len) : "";
    if (!raw || len == 0) {
        gtk_text_buffer_set_text(self->ql_text_buf, "/* Empty file (0 bytes) */", -1);
        return;
    }

    g_autofree char *valid_utf8 = g_utf8_make_valid(raw, (gssize)len);
    gtk_text_buffer_set_text(self->ql_text_buf, valid_utf8, -1);
}

static void
on_text_open_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
    KajoQuickLook *self = user_data;
    GError *error = NULL;
    GFileInputStream *stream = g_file_read_finish(G_FILE(source), res, &error);

    if (!stream) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            gtk_text_buffer_set_text(self->ql_text_buf, error ? error->message : "Unable to read file", -1);
        }
        g_clear_error(&error);
        return;
    }

    g_input_stream_read_bytes_async(
        G_INPUT_STREAM(stream),
        QUICKLOOK_MAX_TEXT_BYTES,
        G_PRIORITY_DEFAULT,
        self->preview_cancel,
        on_text_bytes_ready,
        self
    );
    g_object_unref(stream);
}

static void
on_folder_preview_enum_ready(GPtrArray *items, GError *error, gpointer user_data)
{
    KajoQuickLook *self = user_data;
    if (error) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            gtk_label_set_text(GTK_LABEL(self->ql_folder_summary_lbl), error->message);
        }
        return;
    }

    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(self->ql_folder_list)) != NULL) {
        gtk_box_remove(GTK_BOX(self->ql_folder_list), child);
    }

    guint total = items ? items->len : 0;
    guint dirs = 0, files = 0;
    guint64 total_bytes = 0;

    for (guint i = 0; i < total; i++) {
        KajoFileItem *it = g_ptr_array_index(items, i);
        if (kajo_file_item_is_dir(it))
            dirs++;
        else {
            files++;
            total_bytes += kajo_file_item_get_size(it);
        }
    }

    g_autofree char *size_str = kajo_files_format_size(total_bytes, FALSE);
    g_autofree char *sum = g_strdup_printf(
        "FOLDER CONTENTS: %u ITEMS (%u FOLDERS, %u FILES — %s)",
        total, dirs, files, size_str
    );
    gtk_label_set_text(GTK_LABEL(self->ql_folder_summary_lbl), sum);

    guint limit = MIN(total, 50U);
    for (guint i = 0; i < limit; i++) {
        KajoFileItem *it = g_ptr_array_index(items, i);
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_add_css_class(row, "widget-row-card");

        GtkWidget *ic = gtk_image_new_from_icon_name(kajo_file_item_get_icon_name(it));
        gtk_image_set_pixel_size(GTK_IMAGE(ic), 16);

        GtkWidget *nm = gtk_label_new(kajo_file_item_get_display_name(it));
        gtk_widget_set_halign(nm, GTK_ALIGN_START);
        gtk_widget_set_hexpand(nm, TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(nm), PANGO_ELLIPSIZE_END);

        g_autofree char *sz = kajo_files_format_size(kajo_file_item_get_size(it), kajo_file_item_is_dir(it));
        GtkWidget *sz_lbl = gtk_label_new(sz);
        gtk_widget_add_css_class(sz_lbl, "shell-popover-subtitle");

        gtk_box_append(GTK_BOX(row), ic);
        gtk_box_append(GTK_BOX(row), nm);
        gtk_box_append(GTK_BOX(row), sz_lbl);
        gtk_box_append(GTK_BOX(self->ql_folder_list), row);
    }

    if (items)
        g_ptr_array_unref(items);
}

static void
populate_quicklook_info_card(KajoQuickLook *self, KajoFileItem *item)
{
    const char *disp = kajo_file_item_get_display_name(item);
    const char *ctype = kajo_file_item_get_content_type(item);
    gboolean is_dir = kajo_file_item_is_dir(item);
    g_autofree char *sz_str = kajo_files_format_size(kajo_file_item_get_size(item), is_dir);
    g_autofree char *mtime_str = kajo_files_format_mtime(kajo_file_item_get_mtime(item));

    gtk_stack_set_visible_child_name(GTK_STACK(self->ql_stack), "info");
    gtk_image_set_from_icon_name(GTK_IMAGE(self->ql_info_icon), kajo_file_item_get_icon_name(item));

    g_autofree char *perms = kajo_files_format_permissions(
        kajo_file_item_get_unix_mode(item),
        is_dir,
        kajo_file_item_is_symlink(item)
    );
    const char *orig_path = kajo_file_item_get_trash_orig_path(item);
    const char *sym_target = kajo_file_item_get_symlink_target(item);

    g_autofree char *details = g_strdup_printf(
        "NAME:         %s\n"
        "TYPE:         %s (%s)\n"
        "EXACT SIZE:   %" G_GUINT64_FORMAT " bytes (%s)\n"
        "MODIFIED:     %s\n"
        "PERMISSIONS:  %s\n"
        "URI:          %s%s%s%s%s",
        disp,
        kajo_file_item_get_type_description(item),
        ctype,
        kajo_file_item_get_size(item),
        sz_str,
        mtime_str,
        perms,
        kajo_file_item_get_uri(item),
        sym_target ? "\nSYMLINK TO:   " : "",
        sym_target ? sym_target : "",
        orig_path ? "\nTRASH ORIGIN: " : "",
        orig_path ? orig_path : ""
    );
    gtk_label_set_text(GTK_LABEL(self->ql_info_details_lbl), details);
}

static void
populate_quicklook_modal_file(KajoQuickLook *self, KajoFileItem *item, GFile *file)
{
    const char *name = kajo_file_item_get_name(item);
    const char *ctype = kajo_file_item_get_content_type(item);
    gboolean is_dir = kajo_file_item_is_dir(item);
    gboolean is_media = (!is_dir && ctype &&
                         (g_str_has_prefix(ctype, "video/") || g_str_has_prefix(ctype, "audio/")));
    g_autofree char *sz_str = kajo_files_format_size(kajo_file_item_get_size(item), is_dir);

    /* 2. Image Preview */
    if (ctype && g_str_has_prefix(ctype, "image/")) {
        gtk_stack_set_visible_child_name(GTK_STACK(self->ql_stack), "image");
        gtk_picture_set_file(GTK_PICTURE(self->ql_picture), file);

        g_autoptr(GError) tex_err = NULL;
        g_autoptr(GdkTexture) tex = gdk_texture_new_from_file(file, &tex_err);
        if (tex) {
            g_autofree char *dim = g_strdup_printf(
                "%d × %d px  •  %s  •  %s",
                gdk_texture_get_width(tex),
                gdk_texture_get_height(tex),
                kajo_file_item_get_type_description(item),
                sz_str
            );
            gtk_label_set_text(GTK_LABEL(self->ql_img_info_lbl), dim);
        } else {
            gtk_label_set_text(GTK_LABEL(self->ql_img_info_lbl), kajo_file_item_get_type_description(item));
        }
        return;
    }

    /* 4. Audio & Video Preview (libmpv + GtkGLArea) */
    if (is_media) {
        gtk_stack_set_visible_child_name(GTK_STACK(self->ql_stack), "media");
        kajo_player_mpv_load_file(self->ql_mpv, file);
        return;
    }

    /* 5. Archive Preview (libarchive) */
    if (kajo_files_is_archive(ctype, name) && populate_archive_preview(self, file)) {
        return;
    }

    /* 6. Text / Code Preview */
    if (kajo_files_is_text_or_code(ctype, name)) {
        gtk_stack_set_visible_child_name(GTK_STACK(self->ql_stack), "text");
        gtk_text_buffer_set_text(self->ql_text_buf, "Loading preview...", -1);
        g_file_read_async(file, G_PRIORITY_DEFAULT, self->preview_cancel, on_text_open_ready, self);
        return;
    }

    /* 7. Fallback: Rich Metadata Card */
    populate_quicklook_info_card(self, item);
}

static void
on_ql_archive_temp_ready(GFile *temp_file, GError *error, gpointer user_data)
{
    if (error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

    KajoQuickLook *self = user_data;
    if (!self || !self->current_item)
        return;

    if (temp_file && !error) {
        populate_quicklook_modal_file(self, self->current_item, temp_file);
    } else {
        populate_quicklook_info_card(self, self->current_item);
    }
}

static void
populate_quicklook_modal(KajoQuickLook *self)
{
    if (!self || !self->current_item) return;

    KajoFileItem *item = self->current_item;
    GFile *file = kajo_file_item_get_file(item);
    const char *disp = kajo_file_item_get_display_name(item);
    const char *ctype = kajo_file_item_get_content_type(item);
    gboolean is_dir = kajo_file_item_is_dir(item);
    gboolean is_media = (!is_dir && ctype &&
                         (g_str_has_prefix(ctype, "video/") || g_str_has_prefix(ctype, "audio/")));

    if (!is_media)
        stop_media_playback(self);
    if (!ctype || !g_str_has_prefix(ctype, "image/"))
        gtk_picture_set_file(GTK_PICTURE(self->ql_picture), NULL);

    if (self->preview_cancel) {
        g_cancellable_cancel(self->preview_cancel);
        g_clear_object(&self->preview_cancel);
    }
    self->preview_cancel = g_cancellable_new();

    g_autofree char *hdr_title = g_strdup_printf("QUICK LOOK // %s", disp);
    gtk_label_set_text(GTK_LABEL(self->ql_title_lbl), hdr_title);

    g_autofree char *sz_str = kajo_files_format_size(kajo_file_item_get_size(item), is_dir);
    g_autofree char *badge = g_strdup_printf("[ %s | %s ]", ctype, sz_str);
    gtk_label_set_text(GTK_LABEL(self->ql_badge_lbl), badge);

    g_autofree char *mtime_str = kajo_files_format_mtime(kajo_file_item_get_mtime(item));
    g_autofree char *ftr_mtime = g_strdup_printf("MODIFIED: %s", mtime_str);
    gtk_label_set_text(GTK_LABEL(self->ql_footer_mtime_lbl), ftr_mtime);

    /* 1. Directory Preview */
    if (is_dir) {
        gtk_stack_set_visible_child_name(GTK_STACK(self->ql_stack), "folder");
        gtk_label_set_text(GTK_LABEL(self->ql_folder_summary_lbl), "Scanning folder...");
        kajo_files_enumerate_async(file, self->preview_cancel, on_folder_preview_enum_ready, self);
        return;
    }

    /* 1b. File inside archive:// -> extract to temp cache before previewing */
    const char *item_uri = kajo_file_item_get_uri(item);
    if (item_uri && kajo_files_parse_archive_uri(item_uri, NULL, NULL)) {
        gtk_stack_set_visible_child_name(GTK_STACK(self->ql_stack), "text");
        gtk_text_buffer_set_text(self->ql_text_buf, "Extracting entry for preview...", -1);
        kajo_files_op_extract_entry_to_temp_async(
            item_uri,
            self->preview_cancel,
            on_ql_archive_temp_ready,
            self
        );
        return;
    }

    populate_quicklook_modal_file(self, item, file);
}

/* ─── Public API ─── */

void
kajo_quicklook_set_item(KajoQuickLook *self, KajoFileItem *item, GFile *current_dir)
{
    g_return_if_fail(self != NULL);

    g_set_object(&self->current_item, item);
    g_set_object(&self->current_dir, current_dir);

    g_autofree char *cur_uri = current_dir ? g_file_get_uri(current_dir) : NULL;
    g_autofree char *arch_path = NULL;
    g_autofree char *inner_path = NULL;
    gboolean in_archive = cur_uri && kajo_files_parse_archive_uri(cur_uri, &arch_path, &inner_path);

    if (!item) {
        gtk_stack_set_visible_child_name(GTK_STACK(self->insp_preview_stack), "icon");
        gtk_image_set_from_icon_name(GTK_IMAGE(self->insp_icon), in_archive ? "package-x-generic" : "folder");
        gtk_label_set_text(GTK_LABEL(self->insp_type_badge), in_archive ? "ARCHIVE" : "DIRECTORY");

        if (current_dir) {
            g_autofree char *base = NULL;
            if (in_archive) {
                if (inner_path && inner_path[0] != '\0')
                    base = g_path_get_basename(inner_path);
                else if (arch_path)
                    base = g_path_get_basename(arch_path);
            } else {
                base = g_file_get_basename(current_dir);
            }
            gtk_label_set_text(GTK_LABEL(self->insp_title_lbl), base ? base : cur_uri);
            gtk_label_set_text(GTK_LABEL(self->insp_size_lbl), in_archive ? "[ ARCHIVE (READ-ONLY) ]" : "[ CURRENT VIEW ]");
            gtk_label_set_text(GTK_LABEL(self->insp_mtime_lbl), "--");
            gtk_label_set_text(GTK_LABEL(self->insp_perms_lbl), in_archive ? "dr-xr-xr-x" : "drwxr-xr-x");
            gtk_label_set_text(GTK_LABEL(self->insp_mime_lbl), "inode/directory");
            gtk_label_set_text(GTK_LABEL(self->insp_extra_lbl), cur_uri ? cur_uri : "--");
        } else {
            gtk_label_set_text(GTK_LABEL(self->insp_title_lbl), "No Item Selected");
            gtk_label_set_text(GTK_LABEL(self->insp_size_lbl), "--");
            gtk_label_set_text(GTK_LABEL(self->insp_mtime_lbl), "--");
            gtk_label_set_text(GTK_LABEL(self->insp_perms_lbl), "--");
            gtk_label_set_text(GTK_LABEL(self->insp_mime_lbl), "--");
            gtk_label_set_text(GTK_LABEL(self->insp_extra_lbl), "--");
        }

        gtk_widget_set_sensitive(self->insp_btn_quicklook, FALSE);
        gtk_widget_set_sensitive(self->insp_btn_open, FALSE);
        gtk_widget_set_visible(self->insp_btn_exec, FALSE);
        gtk_widget_set_sensitive(self->insp_btn_rename, FALSE);
        gtk_widget_set_sensitive(self->insp_btn_trash, FALSE);
        return;
    }

    gtk_widget_set_sensitive(self->insp_btn_quicklook, TRUE);
    gtk_widget_set_sensitive(self->insp_btn_open, TRUE);
    gtk_widget_set_sensitive(self->insp_btn_rename, !in_archive);
    gtk_widget_set_sensitive(self->insp_btn_trash, !in_archive);

    const char *ctype = kajo_file_item_get_content_type(item);
    GFile *file = kajo_file_item_get_file(item);
    gboolean is_dir = kajo_file_item_is_dir(item);
    g_autofree char *local_path = file ? g_file_get_path(file) : NULL;

    if (!is_dir && local_path != NULL && !in_archive) {
        gboolean is_exec = (kajo_file_item_get_unix_mode(item) & 0111U) != 0;
        gtk_button_set_label(
            GTK_BUTTON(self->insp_btn_exec),
            is_exec ? "☑ EXECUTABLE (+X)" : "☐ EXECUTABLE (+X)"
        );
        if (is_exec)
            gtk_widget_add_css_class(self->insp_btn_exec, "active");
        else
            gtk_widget_remove_css_class(self->insp_btn_exec, "active");
        gtk_widget_set_visible(self->insp_btn_exec, TRUE);
    } else {
        gtk_widget_set_visible(self->insp_btn_exec, FALSE);
    }

    if (ctype && g_str_has_prefix(ctype, "image/") && local_path != NULL) {
        gtk_picture_set_file(GTK_PICTURE(self->insp_picture), file);
        gtk_stack_set_visible_child_name(GTK_STACK(self->insp_preview_stack), "image");
    } else {
        gtk_image_set_from_icon_name(GTK_IMAGE(self->insp_icon), kajo_file_item_get_icon_name(item));
        gtk_stack_set_visible_child_name(GTK_STACK(self->insp_preview_stack), "icon");
    }

    gtk_label_set_text(GTK_LABEL(self->insp_title_lbl), kajo_file_item_get_display_name(item));
    gtk_label_set_text(GTK_LABEL(self->insp_type_badge), kajo_file_item_get_type_description(item));

    g_autofree char *sz = kajo_files_format_size(kajo_file_item_get_size(item), kajo_file_item_is_dir(item));
    g_autofree char *mt = kajo_files_format_mtime(kajo_file_item_get_mtime(item));
    g_autofree char *pm = kajo_files_format_permissions(
        kajo_file_item_get_unix_mode(item),
        kajo_file_item_is_dir(item),
        kajo_file_item_is_symlink(item)
    );

    gtk_label_set_text(GTK_LABEL(self->insp_size_lbl), sz);
    gtk_label_set_text(GTK_LABEL(self->insp_mtime_lbl), mt);
    gtk_label_set_text(GTK_LABEL(self->insp_perms_lbl), pm);
    gtk_label_set_text(GTK_LABEL(self->insp_mime_lbl), ctype);

    const char *orig = kajo_file_item_get_trash_orig_path(item);
    const char *sym = kajo_file_item_get_symlink_target(item);
    if (orig)
        gtk_label_set_text(GTK_LABEL(self->insp_extra_lbl), orig);
    else if (sym)
        gtk_label_set_text(GTK_LABEL(self->insp_extra_lbl), sym);
    else
        gtk_label_set_text(GTK_LABEL(self->insp_extra_lbl), kajo_file_item_get_uri(item));

    /* If Quick Look modal is currently open, live-update it in place! */
    if (kajo_quicklook_is_visible(self)) {
        populate_quicklook_modal(self);
    }
}

void
kajo_quicklook_show(KajoQuickLook *self)
{
    if (!self || !self->current_item || !self->ql_backdrop)
        return;

    gtk_widget_set_visible(self->ql_backdrop, TRUE);
    populate_quicklook_modal(self);
}

void
kajo_quicklook_hide(KajoQuickLook *self)
{
    if (!self || !self->ql_backdrop)
        return;

    if (self->preview_cancel) {
        g_cancellable_cancel(self->preview_cancel);
        g_clear_object(&self->preview_cancel);
    }
    stop_media_playback(self);
    gtk_widget_set_visible(self->ql_backdrop, FALSE);
}

void
kajo_quicklook_toggle(KajoQuickLook *self)
{
    if (!self) return;
    if (kajo_quicklook_is_visible(self))
        kajo_quicklook_hide(self);
    else
        kajo_quicklook_show(self);
}

gboolean
kajo_quicklook_is_visible(const KajoQuickLook *self)
{
    return (self && self->ql_backdrop && gtk_widget_get_visible(self->ql_backdrop));
}

KajoQuickLook *
kajo_quicklook_new(GtkOverlay *parent_overlay,
                   GtkWidget *inspector_container,
                   KajoQuickLookActionCallback action_cb,
                   gpointer user_data)
{
    KajoQuickLook *self = g_new0(KajoQuickLook, 1);
    self->overlay = parent_overlay;
    self->inspector_box = inspector_container;
    self->action_cb = action_cb;
    self->user_data = user_data;

    build_inspector_ui(self);
    build_quicklook_overlay_ui(self);

    return self;
}

void
kajo_quicklook_free(KajoQuickLook *self)
{
    if (!self) return;
    if (self->preview_cancel) {
        g_cancellable_cancel(self->preview_cancel);
        g_clear_object(&self->preview_cancel);
    }
    stop_media_playback(self);
    g_clear_pointer(&self->ql_mpv, kajo_player_mpv_free);
    g_clear_object(&self->current_item);
    g_clear_object(&self->current_dir);
    g_free(self);
}
